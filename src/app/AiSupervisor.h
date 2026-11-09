// Lane L3a (docs/superpowers/specs/2026-11-09-ai-drive-design.md 5.3, 7, 8):
// the headless supervisor engine.
//
// It decides WHEN to ask the LLM sidecar to look at the notch set, writes the
// state file the sidecar reads, runs the sidecar through a SECOND
// SidecarRunner, validates what comes back and keeps the accepted proposals in
// a book the GUI can show and the operator can dismiss.
//
// PHASE L3a CHANGES THE AUDIO BY EXACTLY 0 dB. Nothing here calls into
// NotchController to set, retune, hold or clear a notch -- this file does not
// even hold a controller. The only inputs are plain-struct snapshots the
// owner hands in through AiSupervisor::Environment. A proposal is a record to
// be displayed; it is never applied.
//
// THREAD: message thread only (tick(), notePlacement(), every setter and the
// result callback, which SidecarRunner posts to the message thread).
//
// Nothing in this file touches a key: credentials are read by the sidecar from
// the DPAPI store (the argv carries --credentials-only).

#pragma once

#include "app/AiAction.h"
#include "app/AiMode.h"
#include "app/NotchController.h"
#include "app/SidecarRunner.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

//==============================================================================
// Ceilings per actor (spec section 2). ONE place; raise them here and nowhere
// else. L3a: the LLM may only SUGGEST (L3c raises it to Act), the network may
// only run in the shadow (phase C2 raises it, behind the assist gate).
constexpr AiMode kLlmAiCeiling     = AiMode::Suggest;
constexpr AiMode kNetworkAiCeiling = AiMode::Shadow;

//==============================================================================
// Log event shapers (schema 4). Pure. MainComponent's static wrappers of the
// same names delegate here, so the supervisor does not depend on MainComponent.
[[nodiscard]] juce::var aiModeEventToVar (AiMode requested, AiMode effectiveLlm, AiMode effectiveNetwork);
[[nodiscard]] juce::var aiCallEventToVar (const juce::String& trigger, std::uint32_t generation,
                                          double ms, const juce::String& status, const AiUsage* usage);
[[nodiscard]] juce::var aiProposalEventToVar (std::uint32_t generation, const AiProposal& proposal);
[[nodiscard]] juce::var aiDecisionEventToVar (std::uint32_t generation, std::uint32_t nid,
                                              AiProposal::Tool tool, AiDecision decision,
                                              std::optional<AiRefusal> refusal,
                                              const char* reason = nullptr);

//==============================================================================
// What the supervisor reads of the world, as plain data.
enum class AiEngineMode : std::uint8_t { Auto, Soundcheck, Bypass };
// AiRingRisk lives in AiAction.h (the validator reads it).

struct AiStateNotch
{
    std::uint32_t nid = 0;             // the controller-LOCAL id (ModelNotch::nid)
    int           lane = 0, index = 0;
    float         hz = 0.0f, q = 0.0f, depthDb = 0.0f, ceilingDb = 0.0f;
    NotchController::Origin origin = NotchController::Origin::Detector;
    Author        placedBy = Author::Detector, lastChangedBy = Author::Detector;
    double        seenS = 0.0;   // seconds since the supervisor FIRST OBSERVED this notch (not the notch's true age)
};

struct AiStateSlot
{
    int        slot = 0;
    AiRingRisk ringRisk = AiRingRisk::Na;
    bool       hasRingRiskScore = false;
    float      ringRiskScore = 0.0f;
    std::vector<AiStateNotch> notches;   // ACTIVE notches only
};

struct AiStateSnapshot
{
    AiEngineMode engineMode = AiEngineMode::Auto;
    bool   measuring = false;       // an active-soundcheck measurement is running
    bool   configured = false;      // consent on + usable key state + a session log -- the ask flow's predicate
    double sampleRate = 48000.0;
    double binHz = 0.0;
    std::vector<AiStateSlot> slots;
    // exe, session log, provider, model, base URL as the ask flow fills them.
    // The supervisor sets `supervise`, `statePath` and `timeoutMs` itself.
    CopilotRequest request;
};

//==============================================================================
// Launcher seam: tests use a fake; production adapts a SidecarRunner.
class AiLauncher
{
public:
    virtual ~AiLauncher() = default;
    /** False = refused (a cancelled run is still unwinding). onDone runs on the message thread. */
    virtual bool start (CopilotRequest request, std::function<void (CopilotRunResult)> onDone) = 0;
    virtual void cancel() = 0;
    [[nodiscard]] virtual bool isBusy() const = 0;
};

/** A SidecarRunner of its own -- never the one the ask box uses. The runner
    (and its worker thread) is created on the first start(), so a component
    that never supervises owns no thread. */
class SidecarAiLauncher : public AiLauncher
{
public:
    explicit SidecarAiLauncher (std::unique_ptr<CopilotBackend> backend = std::make_unique<ChildProcessBackend>(),
                                SidecarRunner::Poster poster = {})
        : backend_ (std::move (backend)), poster_ (std::move (poster)) {}

    bool start (CopilotRequest request, std::function<void (CopilotRunResult)> onDone) override
    {
        if (runner_ == nullptr)
            runner_ = std::make_unique<SidecarRunner> (std::move (backend_), std::move (poster_));
        return runner_->request (std::move (request), std::move (onDone));
    }
    void cancel() override { if (runner_ != nullptr) runner_->cancel(); }
    [[nodiscard]] bool isBusy() const override { return runner_ != nullptr && runner_->isBusy(); }

private:
    std::unique_ptr<CopilotBackend> backend_;
    SidecarRunner::Poster poster_;
    std::unique_ptr<SidecarRunner> runner_;
};

//==============================================================================
enum class AiTrigger : std::uint8_t { RecurringBin, AtCeiling, Heartbeat, Manual };
[[nodiscard]] const char* aiTriggerName (AiTrigger trigger) noexcept;

// A proposal that passed validateProposal and is on display. `proposal.nid` is
// the GLOBAL nid (see globalNid()).
struct ShownProposal
{
    std::uint32_t id = 0;            // unique per supervisor, never reused
    std::uint32_t generation = 0;    // the call that produced it
    AiProposal    proposal;
    int           slot = 0, lane = 0, index = 0;
    float         hz = 0.0f;         // the live notch's frequency when accepted
    std::int64_t  receivedMs = 0;
};

class AiSupervisor
{
public:
    struct Environment
    {
        std::function<std::int64_t()> nowMs;                         // monotonic ms
        std::unique_ptr<AiLauncher> launcher;
        std::function<AiStateSnapshot()> readState;
        std::function<void (const juce::var&)> logSink;              // may be empty
        std::function<bool()> isAskRunning;                          // may be empty = false
        // Spend limits that survive a restart (decision D6). Wall-clock epoch
        // milliseconds; all three may be empty (= nothing persisted, and then
        // nowMs is used as the wall clock). The supervisor never sees the settings object.
        std::function<std::int64_t()> wallMs;
        std::function<std::vector<std::int64_t>()> loadCallStarts;   // read once, at construction
        std::function<void (const std::vector<std::int64_t>&)> saveCallStarts;   // newest kMaxPersistedCallStarts
    };

    // Limits (decision D6). Public so tests and the GUI read the same numbers.
    static constexpr std::int64_t kMinGapMs            = 120 * 1000;
    static constexpr int          kMaxCallsPerHour     = 12;
    static constexpr int          kDefaultSessionBudget = 40;
    static constexpr std::int64_t kHeartbeatMs         = 10 * 60 * 1000;
    static constexpr std::int64_t kAtCeilingMs         = 30 * 1000;
    static constexpr std::int64_t kRecurringWindowMs   = 10 * 60 * 1000;
    static constexpr int          kRecurringCount      = 3;
    static constexpr std::int64_t kProposalLifetimeMs  = 10 * 60 * 1000;
    static constexpr std::int64_t kTickIntervalMs      = 500;   // tick() does real work at most this often
    static constexpr int          kWatchdogMs          = 105000;
    // The sidecar's own deadline (SidecarRunner.h) must fire first, with room for
    // process start-up and teardown: the watchdog is only the last resort.
    static_assert (kWatchdogMs >= kSuperviseDeadlineS * 1000 + 10000, "watchdog must outlast the sidecar deadline");
    static constexpr int          kMaxPersistedCallStarts = 12;
    static constexpr int          kPauseAfterFailures  = 3;
    static constexpr int          kMaxSlots            = 16;
    static constexpr int          kMaxNotchesPerSlot   = 32;
    static constexpr const char*  kStateFileName       = "supervisor-state.json";

    explicit AiSupervisor (Environment env);
    ~AiSupervisor();

    AiSupervisor (const AiSupervisor&) = delete;
    AiSupervisor& operator= (const AiSupervisor&) = delete;

    // --- mode ---------------------------------------------------------------
    // Logs `ai_mode` and bumps the generation on every CHANGE.
    void   setRequestedMode (AiMode mode);
    [[nodiscard]] AiMode requestedMode() const noexcept { return requested_; }
    [[nodiscard]] AiMode effectiveLlmMode() const noexcept { return effectiveAiMode (requested_, kLlmAiCeiling); }
    [[nodiscard]] AiMode effectiveNetworkMode() const noexcept { return effectiveAiMode (requested_, kNetworkAiCeiling); }

    void setSessionBudget (int calls) noexcept { sessionBudget_ = calls; }
    [[nodiscard]] int sessionBudget() const noexcept { return sessionBudget_; }

    // --- feeding ------------------------------------------------------------
    /** A DETECTOR placement just happened. Counted per (slot, lane, bin). */
    void notePlacement (int slot, int lane, double hz, std::int64_t nowMs);

    /** Drive from the GUI timer. Cheap: real work at most every kTickIntervalMs. */
    void tick();

    /** Ask for a call as soon as the blocking conditions allow (trigger "manual"). */
    void requestManualCall();

    /** Consent withdrawn / key deleted: cancel the call in flight (logged as
        `cancelled`) and drop every pending trigger, so nothing starts later from one. */
    void abortAndDropPending();

    /** Device restart, preset load, CLEAR ALL, engine-mode change: cancel the
        call in flight and make its result stale. */
    void invalidate();

    /** invalidate() for good; later tick() calls do nothing. Idempotent. */
    void shutdown();

    // --- observability ------------------------------------------------------
    enum class StatusKind : std::uint8_t { Idle, Disabled, NotConfigured, Pending, LastFailed, BudgetExhausted, Paused };
    struct Status { StatusKind kind = StatusKind::Idle; int seconds = 0; };

    [[nodiscard]] Status status() const;
    [[nodiscard]] juce::String statusText() const;   // ONE fixed Vietnamese line per status
    [[nodiscard]] const std::vector<ShownProposal>& proposals() const noexcept { return proposals_; }
    [[nodiscard]] int callsThisSession() const noexcept { return callsThisSession_; }
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_; }

    void dismiss (std::uint32_t id);
    void dismissAll();

    /** Fired when status() or proposals() change. */
    std::function<void()> onChanged;

    // --- helpers ------------------------------------------------------------
    // NotchController::nid is per controller, but the state file (and so the
    // model) needs ids unique across slots: global = slot << 24 | local.
    // 0 = not representable (local id above 2^24 - 1): that notch is left out.
    [[nodiscard]] static std::uint32_t globalNid (int slot, std::uint32_t localNid) noexcept;
    [[nodiscard]] static int slotOfGlobalNid (std::uint32_t nid) noexcept { return (int) (nid >> 24); }
    [[nodiscard]] static std::uint32_t localNidOf (std::uint32_t nid) noexcept { return nid & 0xFFFFFFu; }

    /** The exact state-file text for a snapshot (see the brief for the shape).
        Only finite, rounded values; at most 16 slots / 32 notches; no repeated nid. */
    [[nodiscard]] static juce::String buildStateJson (const AiStateSnapshot& state, std::uint32_t generation,
                                                      AiMode aiMode, AiTrigger trigger);

    // --- test seams -----------------------------------------------------------
    void setAiProposalsForTest (std::vector<ShownProposal> proposals);
    /** The first call waits kMinGapMs from app start (a restart must not reset the gap); a test that is not about that skips it. */
    void skipStartupGapForTest() { appStartMs_ = env_.nowMs() - kMinGapMs; }

private:
    struct InFlight { std::uint32_t generation = 0; AiTrigger trigger = AiTrigger::Manual; std::int64_t startMs = 0; };
    struct PlacementKey
    {
        int slot, lane, bin;
        bool operator< (const PlacementKey& o) const
            { return slot != o.slot ? slot < o.slot : lane != o.lane ? lane < o.lane : bin < o.bin; }
    };

    [[nodiscard]] std::int64_t now() const { return env_.nowMs(); }
    void log (const juce::var& v) const { if (env_.logSink) env_.logSink (v); }
    void changed() { if (onChanged) onChanged(); }

    void bumpGeneration();
    void expireProposals (const AiStateSnapshot& state, std::int64_t nowMs);
    void trackCeiling (const AiStateSnapshot& state, std::int64_t nowMs);
    [[nodiscard]] std::optional<AiTrigger> pickTrigger (const AiStateSnapshot& state, std::int64_t nowMs);
    void startCall (const AiStateSnapshot& state, AiTrigger trigger, std::int64_t nowMs);
    void onResult (std::uint32_t generation, const CopilotRunResult& result);
    void failCall (const InFlight& call, const char* status, const AiUsage* usage);
    [[nodiscard]] std::uint64_t signatureOf (const AiStateSnapshot& state) const;
    void removeProposal (std::size_t index, AiDecision decision, const char* reason = nullptr);
    [[nodiscard]] std::int64_t wallNow() const;
    [[nodiscard]] bool spendLimitsAllow (std::int64_t nowMs) const;
    void failBeforeStart (AiTrigger trigger, std::uint32_t gen, std::int64_t nowMs);

    Environment env_;
    AiMode requested_ = AiMode::Shadow;
    int sessionBudget_ = kDefaultSessionBudget;
    int callsThisSession_ = 0;
    std::uint32_t generation_ = 0;
    std::uint32_t nextProposalId_ = 1;
    bool shutDown_ = false;
    bool configured_ = false;   // cached from the last tick, for status()
    bool lastFailed_ = false;
    double binHzHint_ = 48000.0 / 2048.0;

    std::optional<InFlight> inFlight_;
    std::optional<InFlight> lastCancelled_;
    std::int64_t lastTickMs_ = -1;
    bool baselineSet_ = false;
    std::int64_t lastCallMs_ = 0;
    std::uint64_t lastSignature_ = 0;
    std::int64_t appStartMs_ = 0;                // monotonic, at construction: the first call waits kMinGapMs from here
    std::int64_t lastStartMs_ = -1;              // monotonic start of this process's latest call
    std::vector<std::int64_t> callStartsWall_;   // wall-clock epoch ms, ascending, newest kMaxPersistedCallStarts
    int consecutiveFailures_ = 0;                // automatic calls ending failed/timeout in a row

    std::map<PlacementKey, std::deque<std::int64_t>> placements_;
    bool pendingRecurring_ = false, pendingManual_ = false;
    std::map<std::uint32_t, std::int64_t> atCeilingSince_;   // global nid -> first seen at ceiling
    std::set<std::uint32_t> ceilingTriggered_;               // global nids that already triggered a call

    std::vector<ShownProposal> proposals_;
};
