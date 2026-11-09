#include "app/AiSupervisor.h"

#include "app/SessionLogger.h"

#include <algorithm>
#include <cmath>

namespace
{
// Same rounding the other session-log shapers use: JSON::toString would print a
// double to full precision.
double roundSig3 (double v)
{
    if (v == 0.0 || ! std::isfinite (v)) return 0.0;
    const double e = std::floor (std::log10 (std::abs (v)));
    const double scale = std::pow (10.0, 2.0 - e);
    return std::round (v * scale) / scale;
}

// State-file numbers: 2 decimals, always finite.
double round2 (double v)
{
    if (! std::isfinite (v)) return 0.0;
    return std::round (v * 100.0) / 100.0;
}

// next_rung_db is a TARGET the model echoes back and the validator compares at
// 0.001 dB: 2 decimals could move an off-ladder ceiling by up to 0.005 dB.
double round3 (double v)
{
    if (! std::isfinite (v)) return 0.0;
    return std::round (v * 1000.0) / 1000.0;
}

const char* originText (NotchController::Origin o)
{
    switch (o)
    {
        case NotchController::Origin::Detector:   return "detector";
        case NotchController::Origin::Preset:     return "preset";
        case NotchController::Origin::Manual:     return "manual";
        case NotchController::Origin::Soundcheck: return "soundcheck";
    }
    return "detector";
}

const char* engineModeText (AiEngineMode m)
{
    switch (m)
    {
        case AiEngineMode::Auto:       return "auto";
        case AiEngineMode::Soundcheck: return "soundcheck";
        case AiEngineMode::Bypass:     return "bypass";
    }
    return "auto";
}

const char* ringRiskText (AiRingRisk r)
{
    switch (r)
    {
        case AiRingRisk::Na:       return "na";
        case AiRingRisk::Low:      return "low";
        case AiRingRisk::Rising:   return "rising";
        case AiRingRisk::Critical: return "critical";
    }
    return "na";
}

// Fixed Vietnamese status lines, UTF-8 escaped (MSVC is not run with /utf-8).
juce::String viText (const char* utf8) { return juce::String::fromUTF8 (utf8); }
} // namespace

//==============================================================================
juce::var aiModeEventToVar (AiMode requested, AiMode effectiveLlm, AiMode effectiveNetwork)
{
    auto v = SessionLogger::makeEvent ("ai_mode");
    auto* o = v.getDynamicObject();
    o->setProperty ("mode", juce::String (aiModeToString (requested)));
    o->setProperty ("effective_llm", juce::String (aiModeToString (effectiveLlm)));
    o->setProperty ("effective_network", juce::String (aiModeToString (effectiveNetwork)));
    return v;
}

juce::var aiCallEventToVar (const juce::String& trigger, std::uint32_t generation,
                            double ms, const juce::String& status, const AiUsage* usage)
{
    auto v = SessionLogger::makeEvent ("ai_call");
    auto* o = v.getDynamicObject();
    o->setProperty ("trigger", trigger);
    o->setProperty ("gen", (juce::int64) generation);
    o->setProperty ("ms", roundSig3 (ms));
    o->setProperty ("status", status);
    if (usage != nullptr)
    {
        // null, not 0, for a count the provider did not report.
        auto* u = new juce::DynamicObject();
        u->setProperty ("input_tokens",  usage->inputTokens  ? juce::var ((juce::int64) *usage->inputTokens)  : juce::var());
        u->setProperty ("output_tokens", usage->outputTokens ? juce::var ((juce::int64) *usage->outputTokens) : juce::var());
        o->setProperty ("usage", juce::var (u));
    }
    return v;
}

juce::var aiProposalEventToVar (std::uint32_t generation, const AiProposal& p)
{
    auto v = SessionLogger::makeEvent ("ai_proposal");
    auto* o = v.getDynamicObject();
    o->setProperty ("gen", (juce::int64) generation);
    o->setProperty ("tool", juce::String (aiToolName (p.tool)));
    if (p.tool != AiProposal::Tool::NoAction)   // no_action addresses no notch
    {
        // `p.nid` is the GLOBAL id; a log reader joins on notch_set lines, which
        // carry the controller-local nid, so the line says slot + local nid.
        o->setProperty ("slot", AiSupervisor::slotOfGlobalNid (p.nid));
        o->setProperty ("nid", (juce::int64) AiSupervisor::localNidOf (p.nid));
        o->setProperty ("hz", roundSig3 ((double) p.expectHz));
    }
    if (p.tool == AiProposal::Tool::Deepen)
        o->setProperty ("target_db", roundSig3 ((double) p.targetDb));
    if (p.tool == AiProposal::Tool::Hold)
        o->setProperty ("hold_s", p.holdSeconds);
    o->setProperty ("reason", juce::String (aiReasonName (p.reason)));
    return v;
}

juce::var aiDecisionEventToVar (std::uint32_t generation, std::uint32_t nid,
                                AiProposal::Tool tool, AiDecision decision,
                                std::optional<AiRefusal> refusal, const char* reason)
{
    auto v = SessionLogger::makeEvent ("ai_decision");
    auto* o = v.getDynamicObject();
    o->setProperty ("gen", (juce::int64) generation);
    o->setProperty ("slot", AiSupervisor::slotOfGlobalNid (nid));   // `nid` is GLOBAL: log slot + local nid
    o->setProperty ("nid", (juce::int64) AiSupervisor::localNidOf (nid));
    o->setProperty ("tool", juce::String (aiToolName (tool)));
    o->setProperty ("decision", juce::String (aiDecisionName (decision)));
    if (refusal)
        o->setProperty ("refusal", juce::String (aiRefusalName (*refusal)));
    // `reason` explains an `expired` decision (superseded, or the validator's
    // refusal name when the proposal stopped validating against the live notch).
    if (reason != nullptr)
        o->setProperty ("reason", juce::String (reason));
    return v;
}

const char* aiTriggerName (AiTrigger t) noexcept
{
    switch (t)
    {
        case AiTrigger::RecurringBin: return "recurring_bin";
        case AiTrigger::AtCeiling:    return "at_ceiling";
        case AiTrigger::Heartbeat:    return "heartbeat";
        case AiTrigger::Manual:       return "manual";
    }
    return "manual";
}

//==============================================================================
std::uint32_t AiSupervisor::globalNid (int slot, std::uint32_t localNid) noexcept
{
    if (slot < 0 || slot >= kMaxSlots || localNid == 0 || localNid > 0xFFFFFFu)
        return 0;
    return ((std::uint32_t) slot << 24) | localNid;
}

juce::String AiSupervisor::buildStateJson (const AiStateSnapshot& state, std::uint32_t generation,
                                           AiMode aiMode, AiTrigger trigger)
{
    const double sr = std::isfinite (state.sampleRate) && state.sampleRate >= 1.0 ? state.sampleRate : 48000.0;
    const double binHz = std::isfinite (state.binHz) && state.binHz >= 0.001 ? state.binHz : sr / 2048.0;

    auto* root = new juce::DynamicObject();
    root->setProperty ("schema", 2);
    root->setProperty ("generation", (juce::int64) generation);
    // The file only ever says what the supervisor is allowed to be: suggest or act.
    root->setProperty ("ai_mode", aiMode == AiMode::Act ? "act" : "suggest");
    root->setProperty ("engine_mode", engineModeText (state.engineMode));
    root->setProperty ("sample_rate", round2 (sr));
    root->setProperty ("bin_hz", round2 (binHz) >= 0.01 ? round2 (binHz) : binHz);
    root->setProperty ("trigger", aiTriggerName (trigger));

    juce::Array<juce::var> slots;
    std::set<std::uint32_t> seenNids;
    std::set<int> seenSlots;
    for (const auto& s : state.slots)
    {
        if ((int) slots.size() >= kMaxSlots)
            break;
        if (s.slot < 0 || s.slot >= kMaxSlots || ! seenSlots.insert (s.slot).second)
            continue;

        auto* so = new juce::DynamicObject();
        so->setProperty ("slot", s.slot);
        so->setProperty ("ring_risk", ringRiskText (s.ringRisk));
        const bool scoreOk = s.hasRingRiskScore && std::isfinite (s.ringRiskScore)
                             && s.ringRiskScore >= 0.0f && s.ringRiskScore <= 1.0e6f;
        so->setProperty ("ring_risk_score", scoreOk ? juce::var (round2 ((double) s.ringRiskScore)) : juce::var());

        juce::Array<juce::var> notches;
        for (const auto& n : s.notches)
        {
            if ((int) notches.size() >= kMaxNotchesPerSlot)
                break;
            const auto gnid = globalNid (s.slot, n.nid);
            if (gnid == 0 || seenNids.count (gnid) != 0)
                continue;
            const double hz = round2 ((double) n.hz), q = round2 ((double) n.q);
            if (! std::isfinite ((double) n.hz) || ! std::isfinite ((double) n.q) || ! std::isfinite ((double) n.depthDb)
                || ! std::isfinite ((double) n.ceilingDb) || ! std::isfinite (n.seenS)
                || hz < 0.01 || hz > sr / 2.0 || q < 0.01
                || n.lane < 0 || n.lane > 1 || n.index < 0 || n.index > 63)
                continue;
            seenNids.insert (gnid);

            auto* no = new juce::DynamicObject();
            no->setProperty ("nid", (juce::int64) gnid);
            no->setProperty ("lane", n.lane);
            no->setProperty ("index", n.index);
            no->setProperty ("hz", hz);
            no->setProperty ("q", q);
            no->setProperty ("depth_db", round2 (juce::jlimit (-120.0, 0.0, (double) n.depthDb)));
            no->setProperty ("ceiling_db", round2 (juce::jlimit (-120.0, 0.0, (double) n.ceilingDb)));
            // The ONE legal deepen target (the validator's own function), or null.
            const auto nextRung = aiNextRungDb ((double) n.depthDb, (double) n.ceilingDb);
            no->setProperty ("next_rung_db", nextRung ? juce::var (round3 (*nextRung)) : juce::var());
            no->setProperty ("origin", originText (n.origin));
            no->setProperty ("placed_by", authorName (n.placedBy));
            no->setProperty ("last_changed_by", authorName (n.lastChangedBy));
            no->setProperty ("seen_s", round2 (juce::jlimit (0.0, 1.0e8, n.seenS)));
            no->setProperty ("hold_s", 0);   // L3a: nothing is held
            no->setProperty ("verdict", "none");
            notches.add (juce::var (no));
        }
        so->setProperty ("notches", notches);
        slots.add (juce::var (so));
    }
    root->setProperty ("slots", slots);
    return juce::JSON::toString (juce::var (root), false);
}

//==============================================================================
namespace
{
struct NotchWhere { int slot, lane, index; float hz; };

// The notches as the validator sees them (shared by the book's re-check and the
// result path, so the two cannot disagree about ring risk or bin width).
std::vector<AiLiveNotch> buildLiveNotches (const AiStateSnapshot& state, double binHzHint,
                                           std::map<std::uint32_t, NotchWhere>* where)
{
    std::vector<AiLiveNotch> live;
    for (const auto& s : state.slots)
        for (const auto& n : s.notches)
        {
            const auto g = AiSupervisor::globalNid (s.slot, n.nid);
            if (g == 0)
                continue;
            AiLiveNotch l;
            l.nid = g; l.lane = n.lane; l.index = n.index; l.hz = n.hz; l.depthDb = n.depthDb;
            l.ceilingDb = n.ceilingDb; l.origin = n.origin;
            l.binHz = (float) (state.binHz > 0.0 ? state.binHz : binHzHint);
            l.ringRisk = s.ringRisk;
            live.push_back (l);
            if (where != nullptr)
                (*where)[g] = { s.slot, n.lane, n.index, n.hz };
        }
    return live;
}
} // namespace

AiSupervisor::AiSupervisor (Environment env)
    : env_ (std::move (env))
{
    jassert (env_.nowMs != nullptr && env_.launcher != nullptr && env_.readState != nullptr);
    appStartMs_ = env_.nowMs();
    if (env_.loadCallStarts)
    {
        // Malformed entries (non-positive) are dropped; order is not trusted.
        for (const auto v : env_.loadCallStarts())
            if (v > 0)
                callStartsWall_.push_back (v);
        std::sort (callStartsWall_.begin(), callStartsWall_.end());
        if ((int) callStartsWall_.size() > kMaxPersistedCallStarts)
            callStartsWall_.erase (callStartsWall_.begin(), callStartsWall_.end() - kMaxPersistedCallStarts);
    }
}

std::int64_t AiSupervisor::wallNow() const
{
    return env_.wallMs ? env_.wallMs() : env_.nowMs();
}

AiSupervisor::~AiSupervisor()
{
    shutdown();
}

void AiSupervisor::setRequestedMode (AiMode mode)
{
    if (mode == requested_)
        return;
    requested_ = mode;
    consecutiveFailures_ = 0;   // any AI-mode change lifts the failure pause
    log (aiModeEventToVar (requested_, effectiveLlmMode(), effectiveNetworkMode()));
    invalidate();

    if (effectiveLlmMode() < AiMode::Suggest)
    {
        pendingRecurring_ = pendingManual_ = false;
        while (! proposals_.empty())
            removeProposal (proposals_.size() - 1, AiDecision::Expired);
    }
    changed();
}

void AiSupervisor::bumpGeneration()
{
    ++generation_;
    if (generation_ == 0)
        generation_ = 1;
}

void AiSupervisor::invalidate()
{
    bumpGeneration();
    if (inFlight_)
    {
        const auto call = *inFlight_;
        inFlight_.reset();
        lastCancelled_ = call;
        env_.launcher->cancel();
        log (aiCallEventToVar (aiTriggerName (call.trigger), call.generation,
                               (double) (now() - call.startMs), "cancelled", nullptr));
        changed();
    }
}

void AiSupervisor::shutdown()
{
    if (shutDown_)
        return;
    invalidate();
    env_.launcher->cancel();
    shutDown_ = true;
}

void AiSupervisor::notePlacement (int slot, int lane, double hz, std::int64_t nowMs)
{
    if (shutDown_ || ! std::isfinite (hz) || hz <= 0.0)
        return;
    const int bin = (int) std::lround (hz / binHzHint_);
    auto& q = placements_[{ slot, lane, bin }];
    q.push_back (nowMs);
    while (! q.empty() && nowMs - q.front() > kRecurringWindowMs)
        q.pop_front();
    if ((int) q.size() >= kRecurringCount && effectiveLlmMode() >= AiMode::Suggest)
        pendingRecurring_ = true;
}

void AiSupervisor::abortAndDropPending()
{
    pendingRecurring_ = pendingManual_ = false;
    invalidate();   // cancels the child, logs `cancelled`, makes a late result stale
    changed();
}

void AiSupervisor::requestManualCall()
{
    if (effectiveLlmMode() >= AiMode::Suggest)
        pendingManual_ = true;
}

std::uint64_t AiSupervisor::signatureOf (const AiStateSnapshot& state) const
{
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h] (std::uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (const auto& s : state.slots)
        for (const auto& n : s.notches)
        {
            mix ((std::uint64_t) globalNid (s.slot, n.nid));
            mix ((std::uint64_t) std::llround ((double) n.hz * 10.0));
            mix ((std::uint64_t) (std::int64_t) std::llround ((double) n.depthDb * 10.0));
        }
    return h;
}

void AiSupervisor::removeProposal (std::size_t index, AiDecision decision, const char* reason)
{
    const auto p = proposals_[index];
    proposals_.erase (proposals_.begin() + (std::ptrdiff_t) index);
    log (aiDecisionEventToVar (p.generation, p.proposal.nid, p.proposal.tool, decision, std::nullopt, reason));
}

void AiSupervisor::dismiss (std::uint32_t id)
{
    for (std::size_t i = 0; i < proposals_.size(); ++i)
        if (proposals_[i].id == id)
        {
            removeProposal (i, AiDecision::Dismissed);
            changed();
            return;
        }
}

void AiSupervisor::dismissAll()
{
    if (proposals_.empty())
        return;
    while (! proposals_.empty())
        removeProposal (proposals_.size() - 1, AiDecision::Dismissed);
    changed();
}

void AiSupervisor::setAiProposalsForTest (std::vector<ShownProposal> proposals)
{
    proposals_ = std::move (proposals);
    changed();
}

void AiSupervisor::expireProposals (const AiStateSnapshot& state, std::int64_t nowMs)
{
    if (proposals_.empty())
        return;
    const double binHz = state.binHz > 0.0 ? state.binHz : binHzHint_;
    const auto liveNotches = buildLiveNotches (state, binHzHint_, nullptr);
    bool any = false;
    for (std::size_t i = proposals_.size(); i-- > 0;)
    {
        const auto& p = proposals_[i];
        const AiStateNotch* live = nullptr;
        for (const auto& s : state.slots)
            if (s.slot == p.slot)
                for (const auto& n : s.notches)
                    if (globalNid (s.slot, n.nid) == p.proposal.nid) { live = &n; break; }

        const bool gone  = live == nullptr;
        const bool moved = live != nullptr && std::abs ((double) live->hz - (double) p.hz) > binHz;
        const bool old   = nowMs - p.receivedMs > kProposalLifetimeMs;
        if (gone || moved || old)
        {
            removeProposal (i, AiDecision::Expired);
            any = true;
            continue;
        }
        // Re-check against the notch and slot as they are NOW: a deepen whose
        // rung is no longer the next one, a release beside a slot that began to
        // ring, a notch that is no longer detector-owned.
        if (const auto refusal = validateProposal (p.proposal, liveNotches))
        {
            removeProposal (i, AiDecision::Expired, aiRefusalName (*refusal));
            any = true;
        }
    }
    if (any)
        changed();
}

void AiSupervisor::trackCeiling (const AiStateSnapshot& state, std::int64_t nowMs)
{
    std::set<std::uint32_t> atCeiling;
    for (const auto& s : state.slots)
        for (const auto& n : s.notches)
            if (n.origin == NotchController::Origin::Detector && n.depthDb <= n.ceilingDb + 0.05f)
            {
                const auto g = globalNid (s.slot, n.nid);
                if (g != 0)
                    atCeiling.insert (g);
            }

    for (auto it = atCeilingSince_.begin(); it != atCeilingSince_.end();)
        it = atCeiling.count (it->first) != 0 ? std::next (it) : atCeilingSince_.erase (it);
    for (const auto g : atCeiling)
        atCeilingSince_.emplace (g, nowMs);

    // A notch that is gone can never trigger again.
    for (auto it = ceilingTriggered_.begin(); it != ceilingTriggered_.end();)
        it = atCeilingSince_.count (*it) != 0 ? std::next (it) : ceilingTriggered_.erase (it);
}

std::optional<AiTrigger> AiSupervisor::pickTrigger (const AiStateSnapshot& state, std::int64_t nowMs)
{
    // Paused after repeated failures: only an operator's manual call may go.
    if (consecutiveFailures_ >= kPauseAfterFailures)
    {
        pendingRecurring_ = false;
        return pendingManual_ ? std::optional<AiTrigger> (AiTrigger::Manual) : std::nullopt;
    }
    if (pendingRecurring_)
        return AiTrigger::RecurringBin;
    for (const auto& [g, since] : atCeilingSince_)
        if (nowMs - since >= kAtCeilingMs && ceilingTriggered_.count (g) == 0)
            return AiTrigger::AtCeiling;
    if (nowMs - lastCallMs_ >= kHeartbeatMs && signatureOf (state) != lastSignature_)
        return AiTrigger::Heartbeat;
    if (pendingManual_)
        return AiTrigger::Manual;
    return std::nullopt;
}

void AiSupervisor::tick()
{
    if (shutDown_)
        return;
    const auto nowMs = now();
    if (lastTickMs_ >= 0 && nowMs - lastTickMs_ < kTickIntervalMs)
        return;
    lastTickMs_ = nowMs;

    // Below Suggest there is nothing to decide and (once the book is empty)
    // nothing to expire: do not pay for a snapshot of 16 slots.
    if (effectiveLlmMode() < AiMode::Suggest && proposals_.empty())
    {
        pendingRecurring_ = pendingManual_ = false;
        return;
    }

    const auto state = env_.readState();
    configured_ = state.configured;
    if (state.binHz > 0.0)
        binHzHint_ = state.binHz;

    if (! baselineSet_)
    {
        baselineSet_ = true;
        lastCallMs_ = nowMs;
        lastSignature_ = signatureOf (state);
    }

    expireProposals (state, nowMs);

    if (effectiveLlmMode() < AiMode::Suggest)
    {
        pendingRecurring_ = pendingManual_ = false;
        return;
    }

    trackCeiling (state, nowMs);

    // The blocking conditions. None of them drops a pending trigger: it fires
    // once the way is clear (an ask in progress only defers it).
    if (state.engineMode != AiEngineMode::Auto || state.measuring || ! state.configured)
        return;
    if (inFlight_ || env_.launcher->isBusy())
        return;
    if (env_.isAskRunning && env_.isAskRunning())
        return;
    if (callsThisSession_ >= sessionBudget_)
        return;
    if (! spendLimitsAllow (nowMs))
        return;

    if (const auto trigger = pickTrigger (state, nowMs))
        startCall (state, *trigger, nowMs);
}

// The hourly cap and the minimum gap, both measured on the WALL clock against
// the persisted call starts so a restart does not reset them. An entry dated in
// the future counts as "now": setting the clock back must not free budget.
bool AiSupervisor::spendLimitsAllow (std::int64_t nowMs) const
{
    // The first call waits kMinGapMs from app start; later ones from the latest start.
    const auto anchorMs = std::max (appStartMs_, lastStartMs_);
    if (nowMs - anchorMs < kMinGapMs)
        return false;

    const auto wall = wallNow();
    int inLastHour = 0;
    std::int64_t latest = 0;
    for (const auto raw : callStartsWall_)
    {
        const auto at = std::min (raw, wall);   // future-dated = now
        latest = std::max (latest, at);
        if (wall - at < 3600 * 1000)
            ++inLastHour;
    }
    if (inLastHour >= kMaxCallsPerHour)
        return false;
    return latest == 0 || wall - latest >= kMinGapMs;
}

void AiSupervisor::startCall (const AiStateSnapshot& state, AiTrigger trigger, std::int64_t nowMs)
{
    bumpGeneration();
    const auto gen = generation_;

    CopilotRequest request = state.request;
    const auto logFile = request.sessionLog;
    if (logFile == juce::File())
    {
        lastFailed_ = true;
        changed();
        return;
    }
    const auto statePath = logFile.getParentDirectory().getChildFile (kStateFileName);

    // Temp file, then replace: the sidecar never reads a half-written file.
    {
        juce::TemporaryFile tmp (statePath);
        const auto text = buildStateJson (state, gen, effectiveLlmMode(), trigger);
        if (! tmp.getFile().replaceWithText (text, false, false, "\n")
            || ! tmp.overwriteTargetFileWithTemporary())
        {
            failBeforeStart (trigger, gen, nowMs);
            return;
        }
    }

    request.supervise = true;
    request.statePath = statePath;
    request.timeoutMs = kWatchdogMs;

    if (! env_.launcher->start (std::move (request), [this, gen] (CopilotRunResult r) { onResult (gen, r); }))
        return;   // a cancelled run is still unwinding: not a call, costs no budget; try again next tick

    inFlight_ = InFlight { gen, trigger, nowMs };
    lastCallMs_ = nowMs;
    lastStartMs_ = nowMs;
    lastSignature_ = signatureOf (state);
    callStartsWall_.push_back (wallNow());
    if ((int) callStartsWall_.size() > kMaxPersistedCallStarts)
        callStartsWall_.erase (callStartsWall_.begin(), callStartsWall_.end() - kMaxPersistedCallStarts);
    if (env_.saveCallStarts)
        env_.saveCallStarts (callStartsWall_);
    ++callsThisSession_;
    pendingRecurring_ = false;
    pendingManual_ = false;
    if (trigger == AiTrigger::AtCeiling)
        for (const auto& [g, since] : atCeilingSince_)
            if (nowMs - since >= kAtCeilingMs)
                ceilingTriggered_.insert (g);
    changed();
}

void AiSupervisor::failCall (const InFlight& call, const char* status, const AiUsage* usage)
{
    lastFailed_ = true;
    // Only AUTOMATIC calls count toward the pause: an operator's manual retry is
    // the way out of it, so it must not be able to deepen it. (cancelled never
    // reaches here.)
    if (call.trigger != AiTrigger::Manual)
        ++consecutiveFailures_;
    log (aiCallEventToVar (aiTriggerName (call.trigger), call.generation,
                           (double) (now() - call.startMs), status, usage));
    changed();
}

// The state file could not be written: ONE failed call. The trigger that led to
// it is dropped (a retry every tick would hammer a broken disk 2x a second) and
// it counts toward the failure pause; nothing was started, so no budget is spent.
void AiSupervisor::failBeforeStart (AiTrigger trigger, std::uint32_t gen, std::int64_t nowMs)
{
    pendingRecurring_ = false;
    pendingManual_ = false;
    if (trigger == AiTrigger::AtCeiling)
        for (const auto& [g, since] : atCeilingSince_)
            if (nowMs - since >= kAtCeilingMs)
                ceilingTriggered_.insert (g);   // same as a started call: do not re-pick it every tick
    lastCallMs_ = nowMs;                        // and a heartbeat must not re-fire at once either
    failCall (InFlight { gen, trigger, nowMs }, "failed", nullptr);
}

void AiSupervisor::onResult (std::uint32_t generation, const CopilotRunResult& result)
{
    if (shutDown_ || ! inFlight_ || inFlight_->generation != generation || generation != generation_)
    {
        // Late delivery for a call that was invalidated: drop it, say so.
        const auto trig = (lastCancelled_ && lastCancelled_->generation == generation) ? lastCancelled_->trigger
                                                                                      : AiTrigger::Manual;
        const auto start = (lastCancelled_ && lastCancelled_->generation == generation) ? lastCancelled_->startMs : now();
        log (aiCallEventToVar (aiTriggerName (trig), generation, (double) (now() - start), "stale", nullptr));
        return;
    }

    const auto call = *inFlight_;
    inFlight_.reset();

    if (result.status == CopilotRunResult::Status::timedOut)
        return failCall (call, "timeout", nullptr);
    if (result.status != CopilotRunResult::Status::ok || result.exitCode != 0)
        return failCall (call, "failed", nullptr);

    const auto parsed = parseSupervisorOutput (result.stdoutText, call.generation);
    if (! parsed.ok())
        return failCall (call, "failed", nullptr);

    lastFailed_ = false;
    consecutiveFailures_ = 0;   // a success lifts the failure pause
    log (aiCallEventToVar (aiTriggerName (call.trigger), call.generation,
                           (double) (now() - call.startMs), "ok", &parsed.usage));

    // Validate against the notches as they are NOW, not as the state file had them.
    const auto state = env_.readState();
    std::map<std::uint32_t, NotchWhere> where;
    const auto live = buildLiveNotches (state, binHzHint_, &where);

    const auto nowMs = now();
    for (const auto& p : parsed.proposals)
    {
        if (p.tool == AiProposal::Tool::NoAction)
        {
            log (aiProposalEventToVar (call.generation, p));   // logged, never shown
            continue;
        }
        if (const auto refusal = validateProposal (p, live))
        {
            log (aiDecisionEventToVar (call.generation, p.nid, p.tool, AiDecision::Refused, refusal));
            continue;
        }

        const auto w = where.find (p.nid);
        if (w == where.end())
            continue;   // validateProposal found it live; unreachable in practice

        // A newer proposal for the same (tool, notch) replaces the old one; the
        // old one is logged as expired (superseded) so the log never loses it.
        for (std::size_t i = proposals_.size(); i-- > 0;)
            if (proposals_[i].proposal.tool == p.tool && proposals_[i].proposal.nid == p.nid)
                removeProposal (i, AiDecision::Expired, "superseded");

        ShownProposal sp;
        sp.id = nextProposalId_++;
        sp.generation = call.generation;
        sp.proposal = p;
        sp.slot = w->second.slot; sp.lane = w->second.lane; sp.index = w->second.index;
        sp.hz = w->second.hz;
        sp.receivedMs = nowMs;
        proposals_.push_back (sp);
        log (aiProposalEventToVar (call.generation, p));
    }
    changed();
}

//==============================================================================
AiSupervisor::Status AiSupervisor::status() const
{
    if (effectiveLlmMode() < AiMode::Suggest)
        return { StatusKind::Disabled, 0 };
    if (inFlight_)
        return { StatusKind::Pending, (int) std::max<std::int64_t> (0, (env_.nowMs() - inFlight_->startMs) / 1000) };
    if (! configured_)
        return { StatusKind::NotConfigured, 0 };
    if (callsThisSession_ >= sessionBudget_)
        return { StatusKind::BudgetExhausted, 0 };
    if (consecutiveFailures_ >= kPauseAfterFailures)
        return { StatusKind::Paused, 0 };
    if (lastFailed_)
        return { StatusKind::LastFailed, 0 };
    return { StatusKind::Idle, 0 };
}

juce::String AiSupervisor::statusText() const
{
    const auto s = status();
    switch (s.kind)
    {
        case StatusKind::Idle:
            return viText ("AI \xc2\xb7 s\xe1\xba\xb5n s\xc3\xa0ng");
        case StatusKind::Disabled:
            return viText ("AI \xc2\xb7 t\xe1\xba\xaft");
        case StatusKind::NotConfigured:
            return viText ("AI \xc2\xb7 ch\xc6\xb0" "a c\xe1\xba\xa5u h\xc3\xacnh");
        case StatusKind::Pending:
            return viText ("AI \xc2\xb7 \xc4\x91" "ang x\xc3\xa9t ") + juce::String (s.seconds) + " s";
        case StatusKind::LastFailed:
            return viText ("AI \xc2\xb7 l\xe1\xba\xa7n x\xc3\xa9t g\xe1\xba\xa7n nh\xe1\xba\xa5t l\xe1\xbb\x97i");
        case StatusKind::Paused:
            return viText ("AI \xc2\xb7 t\xe1\xba\xa1m d\xe1\xbb\xabng sau 3 l\xe1\xbb\x97i li\xc3\xaan ti\xe1\xba\xbfp");
        case StatusKind::BudgetExhausted:
            return viText ("AI \xc2\xb7 h\xe1\xba\xbft l\xc6\xb0\xe1\xbb\xa3t phi\xc3\xaan n\xc3\xa0y");
    }
    return {};
}
