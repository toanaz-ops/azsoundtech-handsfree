// Lane L3a (docs/superpowers/specs/2026-11-09-ai-drive-design.md 5.1, 5.2):
// the typed command set the supervising LLM may PROPOSE, the parser that turns
// the sidecar's stdout into those commands, and the validator that decides
// whether a proposal may be SHOWN.
//
// Pure: no GUI, no threads, no I/O, no controller state. In phase L3a nothing
// is applied -- a proposal that validates is displayed and the operator can
// only dismiss it. The parser is a TRUST BOUNDARY: the model's text is never
// kept (every field is a number or a value from a closed enum), and anything
// malformed is dropped rather than repaired.

#pragma once

#include "app/NotchController.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <optional>
#include <vector>

// A token count the provider did not report is nullopt, never 0.
struct AiUsage
{
    std::optional<std::int64_t> inputTokens, outputTokens;
};

struct AiProposal
{
    enum class Tool : std::uint8_t { Deepen, Hold, Release, NoAction };
    // The closed set of reasons. Each tool accepts only its own subset.
    enum class Reason : std::uint8_t
    {
        RecurringAtFrequency, StillRingingAtDepth,   // deepen
        ShowInProgress,                              // hold (and RecurringAtFrequency)
        JudgedFalse, StaleNoRecurrence,              // release
        NothingToDo, InsufficientData, OutsideScope  // no_action
    };

    Tool          tool = Tool::NoAction;
    std::uint32_t nid = 0;          // 0 for NoAction
    float         expectHz = 0.0f;  // the frequency the model believes the notch is at
    float         targetDb = 0.0f;  // Deepen only
    int           holdSeconds = 0;  // Hold only
    Reason        reason = Reason::NothingToDo;
};

// Name tables. English identifiers; these are exactly the strings in the
// sidecar JSON and in the session log.
[[nodiscard]] const char* aiToolName (AiProposal::Tool tool) noexcept;      // propose_deepen|propose_hold|propose_release|no_action
[[nodiscard]] const char* aiReasonName (AiProposal::Reason reason) noexcept;

enum class AiParseError : std::uint8_t
{
    None,
    NotJson,          // does not parse, or is not one JSON object
    NotObject,
    NotSupervise,     // `supervise` missing or not exactly true
    BadGeneration,    // `generation` missing or not an integer
    Stale,            // `generation` is an integer but not the one expected
    BadActions,       // `actions` missing or not an array
    TooManyActions    // more than kMaxAiActions
};

constexpr int kMaxAiActions = 8;

struct AiParseResult
{
    AiParseError error = AiParseError::None;
    std::vector<AiProposal> proposals;   // only well-formed actions
    int droppedActions = 0;              // individually rejected actions
    AiUsage usage;                       // only meaningful when error == None

    [[nodiscard]] bool ok() const noexcept { return error == AiParseError::None; }
};

// `stdoutText` is the sidecar's whole stdout: ONE JSON object
// {"supervise":true,"generation":N,"actions":[...],"provider":..,"model":..,"usage":{..}}.
// provider and model are free text and are ignored.
[[nodiscard]] AiParseResult parseSupervisorOutput (const juce::String& stdoutText,
                                                   std::uint32_t expectedGeneration);

// The ring-risk level of the slot a notch lives in. Declared here (not in
// AiSupervisor.h) because the validator needs it; AiSupervisor.h includes this.
// Na = the slot has no score yet (unknown), which is NOT "low".
enum class AiRingRisk : std::uint8_t { Na, Low, Rising, Critical };

// One live notch as the validator sees it: a plain copy, made by the caller
// from a SnapshotBuffer, so this file never touches controller state.
struct AiLiveNotch
{
    std::uint32_t nid = 0;
    int           lane = 0, index = 0;
    float         hz = 0.0f;
    float         depthDb = 0.0f;     // the depth RUNNING now
    float         ceilingDb = 0.0f;   // the RESOLVED ceiling (deepest this notch may go)
    NotchController::Origin origin = NotchController::Origin::Detector;
    float         binHz = 0.0f;       // analysis bin width at this slot's sample rate
    // The slot's ring risk. Default Na on purpose: a caller that forgets to fill
    // it gets a release REFUSED, never a release shown beside a ringing slot.
    AiRingRisk    ringRisk = AiRingRisk::Na;
};

// The deepen target the validator accepts for a notch, and the one the state
// file advertises as `next_rung_db`: ONE function, two callers. It is
// NotchController::nextDeeperRungDb (the controller's own ladder), accepted only
// when it is strictly deeper than the running depth (by more than 0.001 dB) and
// not past the resolved ceiling (0.001 dB slack). nullopt = there is no legal
// deeper step (at or past the ceiling, or a non-finite input).
[[nodiscard]] std::optional<double> aiNextRungDb (double depthDb, double ceilingDb);

enum class AiRefusal : std::uint8_t
{
    UnknownNid, FrequencyMismatch, NotDetectorOrigin,
    NotDeeper, BeyondCeiling, MoreThanOneRung,
    RingRiskNotLow   // release only: the slot's ring risk is not at its lowest level
};
[[nodiscard]] const char* aiRefusalName (AiRefusal refusal) noexcept;

// What became of a proposal that was shown (ai_decision log line).
enum class AiDecision : std::uint8_t { Dismissed, Expired, Refused, Stale };
[[nodiscard]] const char* aiDecisionName (AiDecision decision) noexcept;

// nullopt = may be shown. In L3a "accepted" only means "worth displaying".
[[nodiscard]] std::optional<AiRefusal> validateProposal (const AiProposal& proposal,
                                                         const std::vector<AiLiveNotch>& liveNotches);
