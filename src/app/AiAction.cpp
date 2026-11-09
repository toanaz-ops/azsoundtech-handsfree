#include "app/AiAction.h"

#include <cmath>
#include <initializer_list>
#include <limits>

namespace
{
using Tool   = AiProposal::Tool;
using Reason = AiProposal::Reason;

constexpr double kMinHz = 20.0, kMaxHz = 24000.0;
constexpr double kMinTargetDb = -60.0, kMaxTargetDb = 0.0;
constexpr int    kMinHoldSeconds = 30, kMaxHoldSeconds = 600;
constexpr double kDepthEpsilonDb = 1.0e-3;   // targets arrive as JSON doubles, depths as floats

bool isInteger (const juce::var& v)
{
    return v.isInt() || v.isInt64();   // a double (even 5.0) and a bool are NOT integers here
}

bool readNumber (const juce::var& v, double& out)
{
    if (! (v.isInt() || v.isInt64() || v.isDouble()))
        return false;   // strings, bools, arrays and objects all fail here
    const double d = (double) v;
    if (! std::isfinite (d))
        return false;
    out = d;
    return true;
}

bool readNid (const juce::var& v, std::uint32_t& out)
{
    if (! isInteger (v))
        return false;
    const auto n = (juce::int64) v;
    if (n < 1 || n > (juce::int64) std::numeric_limits<std::uint32_t>::max())
        return false;
    out = (std::uint32_t) n;
    return true;
}

bool readHz (const juce::var& v, float& out)
{
    double d = 0.0;
    if (! readNumber (v, d) || d < kMinHz || d > kMaxHz)
        return false;
    out = (float) d;
    return true;
}

struct ReasonEntry { const char* name; Reason reason; };

bool readReason (const juce::var& v, Tool tool, Reason& out)
{
    if (! v.isString())
        return false;
    const auto text = v.toString();
    static const ReasonEntry deepen[]  = { { "recurring_at_frequency", Reason::RecurringAtFrequency },
                                           { "still_ringing_at_depth", Reason::StillRingingAtDepth } };
    static const ReasonEntry hold[]    = { { "recurring_at_frequency", Reason::RecurringAtFrequency },
                                           { "show_in_progress", Reason::ShowInProgress } };
    static const ReasonEntry release[] = { { "judged_false", Reason::JudgedFalse },
                                           { "stale_no_recurrence", Reason::StaleNoRecurrence } };
    static const ReasonEntry none[]    = { { "nothing_to_do", Reason::NothingToDo },
                                           { "insufficient_data", Reason::InsufficientData },
                                           { "outside_scope", Reason::OutsideScope } };
    const ReasonEntry* table = none;
    int n = 3;
    switch (tool)
    {
        case Tool::Deepen:   table = deepen;  n = 2; break;
        case Tool::Hold:     table = hold;    n = 2; break;
        case Tool::Release:  table = release; n = 2; break;
        case Tool::NoAction: break;
    }
    for (int i = 0; i < n; ++i)
        if (text == table[i].name)
        {
            out = table[i].reason;
            return true;
        }
    return false;
}

// The exact key set an action of this tool may carry: nothing missing, nothing
// extra. (No free-text key exists for any tool.)
bool hasExactlyKeys (const juce::DynamicObject& obj, std::initializer_list<const char*> keys)
{
    const auto& props = obj.getProperties();
    if (props.size() != (int) keys.size())
        return false;
    for (const auto* k : keys)
        if (! props.contains (juce::Identifier (k)))
            return false;
    return true;
}

// Whole action or nothing.
bool parseAction (const juce::var& v, AiProposal& out)
{
    const auto* obj = v.getDynamicObject();
    if (obj == nullptr)
        return false;
    const auto toolVar = obj->getProperty ("tool");
    if (! toolVar.isString())
        return false;
    const auto toolName = toolVar.toString();

    AiProposal p;
    if (toolName == "propose_deepen")
    {
        p.tool = Tool::Deepen;
        if (! hasExactlyKeys (*obj, { "tool", "nid", "expect_hz", "target_db", "reason" }))
            return false;
        double target = 0.0;
        if (! readNid (obj->getProperty ("nid"), p.nid)
            || ! readHz (obj->getProperty ("expect_hz"), p.expectHz)
            || ! readNumber (obj->getProperty ("target_db"), target)
            || target < kMinTargetDb || target > kMaxTargetDb)
            return false;
        p.targetDb = (float) target;
    }
    else if (toolName == "propose_hold")
    {
        p.tool = Tool::Hold;
        if (! hasExactlyKeys (*obj, { "tool", "nid", "expect_hz", "hold_s", "reason" }))
            return false;
        const auto hold = obj->getProperty ("hold_s");
        if (! readNid (obj->getProperty ("nid"), p.nid)
            || ! readHz (obj->getProperty ("expect_hz"), p.expectHz)
            || ! isInteger (hold)
            || (juce::int64) hold < kMinHoldSeconds || (juce::int64) hold > kMaxHoldSeconds)
            return false;
        p.holdSeconds = (int) (juce::int64) hold;
    }
    else if (toolName == "propose_release")
    {
        p.tool = Tool::Release;
        if (! hasExactlyKeys (*obj, { "tool", "nid", "expect_hz", "reason" }))
            return false;
        if (! readNid (obj->getProperty ("nid"), p.nid)
            || ! readHz (obj->getProperty ("expect_hz"), p.expectHz))
            return false;
    }
    else if (toolName == "no_action")
    {
        p.tool = Tool::NoAction;
        if (! hasExactlyKeys (*obj, { "tool", "reason" }))
            return false;
    }
    else
        return false;   // unknown tool

    if (! readReason (obj->getProperty ("reason"), p.tool, p.reason))
        return false;

    out = p;
    return true;
}

std::optional<std::int64_t> readCount (const juce::var& v)
{
    if (! isInteger (v))
        return std::nullopt;
    const auto n = (juce::int64) v;
    if (n < 0)
        return std::nullopt;
    return (std::int64_t) n;
}
} // namespace

const char* aiToolName (AiProposal::Tool tool) noexcept
{
    switch (tool)
    {
        case Tool::Deepen:   return "propose_deepen";
        case Tool::Hold:     return "propose_hold";
        case Tool::Release:  return "propose_release";
        case Tool::NoAction: return "no_action";
    }
    return "no_action";
}

const char* aiReasonName (AiProposal::Reason reason) noexcept
{
    switch (reason)
    {
        case Reason::RecurringAtFrequency: return "recurring_at_frequency";
        case Reason::StillRingingAtDepth:  return "still_ringing_at_depth";
        case Reason::ShowInProgress:       return "show_in_progress";
        case Reason::JudgedFalse:          return "judged_false";
        case Reason::StaleNoRecurrence:    return "stale_no_recurrence";
        case Reason::NothingToDo:          return "nothing_to_do";
        case Reason::InsufficientData:     return "insufficient_data";
        case Reason::OutsideScope:         return "outside_scope";
    }
    return "nothing_to_do";
}

const char* aiRefusalName (AiRefusal refusal) noexcept
{
    switch (refusal)
    {
        case AiRefusal::UnknownNid:        return "unknown_nid";
        case AiRefusal::FrequencyMismatch: return "frequency_mismatch";
        case AiRefusal::NotDetectorOrigin: return "not_detector_origin";
        case AiRefusal::NotDeeper:         return "not_deeper";
        case AiRefusal::BeyondCeiling:     return "beyond_ceiling";
        case AiRefusal::MoreThanOneRung:   return "more_than_one_rung";
    }
    return "unknown_nid";
}

const char* aiDecisionName (AiDecision decision) noexcept
{
    switch (decision)
    {
        case AiDecision::Dismissed: return "dismissed";
        case AiDecision::Expired:   return "expired";
        case AiDecision::Refused:   return "refused";
        case AiDecision::Stale:     return "stale";
    }
    return "dismissed";
}

AiParseResult parseSupervisorOutput (const juce::String& stdoutText, std::uint32_t expectedGeneration)
{
    AiParseResult r;
    const auto text = stdoutText.trim();
    juce::var root;
    if (! text.startsWithChar ('{') || ! text.endsWithChar ('}')
        || ! juce::JSON::parse (text, root).wasOk())
    {
        r.error = AiParseError::NotJson;
        return r;
    }
    const auto* obj = root.getDynamicObject();
    if (obj == nullptr)
    {
        r.error = AiParseError::NotObject;
        return r;
    }

    const auto supervise = obj->getProperty ("supervise");
    if (! (supervise.isBool() && (bool) supervise))
    {
        r.error = AiParseError::NotSupervise;
        return r;
    }

    const auto generation = obj->getProperty ("generation");
    if (! isInteger (generation))
    {
        r.error = AiParseError::BadGeneration;
        return r;
    }
    if ((juce::int64) generation != (juce::int64) expectedGeneration)
    {
        r.error = AiParseError::Stale;
        return r;
    }

    const auto* actions = obj->getProperty ("actions").getArray();
    if (actions == nullptr)
    {
        r.error = AiParseError::BadActions;
        return r;
    }
    if (actions->size() > kMaxAiActions)
    {
        r.error = AiParseError::TooManyActions;
        return r;
    }

    for (const auto& a : *actions)
    {
        AiProposal p;
        if (parseAction (a, p))
            r.proposals.push_back (p);
        else
            ++r.droppedActions;
    }

    if (const auto* u = obj->getProperty ("usage").getDynamicObject())
    {
        r.usage.inputTokens  = readCount (u->getProperty ("input_tokens"));
        r.usage.outputTokens = readCount (u->getProperty ("output_tokens"));
    }
    return r;
}

std::optional<AiRefusal> validateProposal (const AiProposal& proposal,
                                           const std::vector<AiLiveNotch>& liveNotches)
{
    if (proposal.tool == Tool::NoAction)
        return std::nullopt;   // addresses nothing

    const AiLiveNotch* notch = nullptr;
    for (const auto& n : liveNotches)
        if (n.nid == proposal.nid)
        {
            notch = &n;
            break;
        }
    if (notch == nullptr || proposal.nid == 0)
        return AiRefusal::UnknownNid;

    // Written so that a NaN on either side refuses.
    if (! (notch->binHz > 0.0f && std::fabs (notch->hz - proposal.expectHz) <= notch->binHz))
        return AiRefusal::FrequencyMismatch;

    if (notch->origin != NotchController::Origin::Detector)
        return AiRefusal::NotDetectorOrigin;

    if (proposal.tool == Tool::Deepen)
    {
        if (! (proposal.targetDb < notch->depthDb - kDepthEpsilonDb))
            return AiRefusal::NotDeeper;
        if (! std::isfinite (notch->ceilingDb) || proposal.targetDb < notch->ceilingDb - kDepthEpsilonDb)
            return AiRefusal::BeyondCeiling;
        // The only legal target is the controller's own next rung (which is the
        // ceiling when the ceiling comes first). Read from the controller, not
        // copied: a change to the ladder must change this too.
        const double next = NotchController::nextDeeperRungDb (notch->depthDb, notch->ceilingDb);
        if (std::fabs (proposal.targetDb - next) > kDepthEpsilonDb)
            return AiRefusal::MoreThanOneRung;
    }

    // L3b: ring-risk freeze, release rate limits and "one AI release in flight
    // per slot" are refused here, once something can actually be applied.
    return std::nullopt;
}
