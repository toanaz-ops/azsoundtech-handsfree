// Lane L3a (docs/superpowers/specs/2026-11-09-ai-drive-design.md 5.1, 5.2, 7):
// the AI command parser (a trust boundary), the proposal validator, the
// supervise argv, the Author tag, and the schema-4 log shapes. Headless, no
// controller thread: phase L3a changes the audio by exactly 0 dB.

#include <gtest/gtest.h>

#include "app/AiAction.h"
#include "app/AiMode.h"
#include "app/MainComponent.h"
#include "app/NotchController.h"
#include "app/SidecarRunner.h"

#include <cmath>
#include <set>
#include <string>

namespace
{
using Tool = AiProposal::Tool;
using Reason = AiProposal::Reason;

constexpr std::uint32_t kGen = 7;

juce::String output (const juce::String& actionsJson, const juce::String& generation = "7",
                     const juce::String& supervise = "true")
{
    return "{\"supervise\": " + supervise + ", \"generation\": " + generation
           + ", \"actions\": [" + actionsJson + "], \"provider\": \"anthropic\", \"model\": \"m\","
             " \"usage\": {\"input_tokens\": 1200, \"output_tokens\": null}}";
}

const juce::String kDeepen  = "{\"tool\": \"propose_deepen\", \"nid\": 5, \"expect_hz\": 1000.5, "
                              "\"target_db\": -12, \"reason\": \"recurring_at_frequency\"}";
const juce::String kHold    = "{\"tool\": \"propose_hold\", \"nid\": 6, \"expect_hz\": 2000, "
                              "\"hold_s\": 120, \"reason\": \"show_in_progress\"}";
const juce::String kRelease = "{\"tool\": \"propose_release\", \"nid\": 7, \"expect_hz\": 300, "
                              "\"reason\": \"judged_false\"}";
const juce::String kNoop    = "{\"tool\": \"no_action\", \"reason\": \"insufficient_data\"}";

// One deepen action with a single field replaced / added; everything else valid.
juce::String deepenWith (const juce::String& field, const juce::String& rawValue)
{
    juce::String s = "{\"tool\": \"propose_deepen\", \"nid\": 5, \"expect_hz\": 1000, "
                     "\"target_db\": -12, \"reason\": \"recurring_at_frequency\"";
    const auto key = "\"" + field + "\":";
    if (s.contains (key))
    {
        const auto start = s.indexOf (key);
        auto end = s.indexOf (start, ",");
        if (end < 0) end = s.length();
        s = s.substring (0, start) + key + " " + rawValue + s.substring (end);
    }
    else
        s += ", \"" + field + "\": " + rawValue;
    return s + "}";
}

// The action must be DROPPED (output itself still accepted) -- or, for the
// non-JSON tokens, the whole output refused. Either way: no proposal.
void expectNoProposal (const juce::String& actionJson, const char* what)
{
    const auto r = parseSupervisorOutput (output (actionJson), kGen);
    EXPECT_TRUE (r.proposals.empty()) << what;
    if (r.ok())
        EXPECT_EQ (r.droppedActions, 1) << what;
}

AiLiveNotch live (std::uint32_t nid, float hz, float depth, float ceiling)
{
    AiLiveNotch n;
    n.nid = nid; n.hz = hz; n.depthDb = depth; n.ceilingDb = ceiling; n.binHz = 23.4375f;
    return n;
}

AiProposal deepenTo (std::uint32_t nid, float hz, float target)
{
    AiProposal p;
    p.tool = Tool::Deepen; p.nid = nid; p.expectHz = hz; p.targetDb = target;
    p.reason = Reason::RecurringAtFrequency;
    return p;
}
} // namespace

// ---------------------------------------------------------------------------
// Parser: the whole output

TEST (AiAction, ParseAcceptsEveryToolAndReadsUsage)
{
    const auto r = parseSupervisorOutput (output (kDeepen + "," + kHold + "," + kRelease + "," + kNoop), kGen);
    ASSERT_TRUE (r.ok());
    EXPECT_EQ (r.droppedActions, 0);
    ASSERT_EQ (r.proposals.size(), 4u);
    EXPECT_EQ (r.proposals[0].tool, Tool::Deepen);
    EXPECT_EQ (r.proposals[0].nid, 5u);
    EXPECT_FLOAT_EQ (r.proposals[0].expectHz, 1000.5f);
    EXPECT_FLOAT_EQ (r.proposals[0].targetDb, -12.0f);
    EXPECT_EQ (r.proposals[0].reason, Reason::RecurringAtFrequency);
    EXPECT_EQ (r.proposals[1].tool, Tool::Hold);
    EXPECT_EQ (r.proposals[1].holdSeconds, 120);
    EXPECT_EQ (r.proposals[2].tool, Tool::Release);
    EXPECT_EQ (r.proposals[2].reason, Reason::JudgedFalse);
    EXPECT_EQ (r.proposals[3].tool, Tool::NoAction);
    EXPECT_EQ (r.proposals[3].reason, Reason::InsufficientData);
    ASSERT_TRUE (r.usage.inputTokens.has_value());
    EXPECT_EQ (*r.usage.inputTokens, 1200);
    EXPECT_FALSE (r.usage.outputTokens.has_value());   // null stays "unknown", never 0
}

TEST (AiAction, ParseAcceptsEmptyActionsAndExactlyEight)
{
    EXPECT_TRUE (parseSupervisorOutput (output (""), kGen).ok());
    juce::String eight;
    for (int i = 0; i < 8; ++i)
        eight += (i ? "," : "") + kNoop;
    const auto r = parseSupervisorOutput (output (eight), kGen);
    ASSERT_TRUE (r.ok());
    EXPECT_EQ (r.proposals.size(), 8u);
}

TEST (AiAction, ParseRejectsNonObjectOutput)
{
    for (const char* text : { "", "   ", "garbage", "[]", "[{\"supervise\": true}]", "\"x\"", "42", "null",
                              "{\"supervise\": true, \"generation\": 7, \"actions\": [" })
        EXPECT_EQ (parseSupervisorOutput (text, kGen).error, AiParseError::NotJson) << text;
}

TEST (AiAction, ParseRejectsWhenSuperviseIsNotExactlyTrue)
{
    for (const char* v : { "false", "\"true\"", "1", "null", "[]" })
        EXPECT_EQ (parseSupervisorOutput (output ("", "7", v), kGen).error, AiParseError::NotSupervise) << v;
    EXPECT_EQ (parseSupervisorOutput ("{\"generation\": 7, \"actions\": []}", kGen).error,
               AiParseError::NotSupervise);
}

TEST (AiAction, ParseRejectsWrongGenerationAsStale)
{
    const auto r = parseSupervisorOutput (output (kNoop, "8"), kGen);
    EXPECT_EQ (r.error, AiParseError::Stale);
    EXPECT_TRUE (r.proposals.empty());
    EXPECT_EQ (parseSupervisorOutput (output (kNoop, "0"), kGen).error, AiParseError::Stale);
    // Not stale, just malformed: a different error, so the caller can tell them apart.
    for (const char* g : { "\"7\"", "7.0", "null", "true", "[7]" })
        EXPECT_EQ (parseSupervisorOutput (output (kNoop, g), kGen).error, AiParseError::BadGeneration) << g;
    EXPECT_NE (AiParseError::Stale, AiParseError::BadGeneration);
}

TEST (AiAction, ParseRejectsMissingOrNonArrayActions)
{
    EXPECT_EQ (parseSupervisorOutput ("{\"supervise\": true, \"generation\": 7}", kGen).error,
               AiParseError::BadActions);
    for (const char* v : { "{}", "\"x\"", "null", "5", "true" })
        EXPECT_EQ (parseSupervisorOutput (juce::String ("{\"supervise\": true, \"generation\": 7, \"actions\": ")
                                              + v + "}", kGen).error,
                   AiParseError::BadActions) << v;
}

TEST (AiAction, ParseRejectsNineActions)
{
    juce::String nine;
    for (int i = 0; i < 9; ++i)
        nine += (i ? "," : "") + kNoop;
    const auto r = parseSupervisorOutput (output (nine), kGen);
    EXPECT_EQ (r.error, AiParseError::TooManyActions);
    EXPECT_TRUE (r.proposals.empty());   // nothing from an over-long list, not even the first eight
}

// ---------------------------------------------------------------------------
// Parser: one action at a time (dropped and counted, the rest survive)

TEST (AiAction, ParseRejectsUnknownToolAndUnknownReason)
{
    expectNoProposal ("{\"tool\": \"propose_place\", \"nid\": 5, \"expect_hz\": 1000, \"reason\": \"judged_false\"}", "unknown tool");
    expectNoProposal ("{\"tool\": \"clear_all\", \"reason\": \"nothing_to_do\"}", "clear_all");
    expectNoProposal ("{\"tool\": 3, \"reason\": \"nothing_to_do\"}", "numeric tool");
    expectNoProposal (deepenWith ("reason", "\"because I said so\""), "free-text reason");
    expectNoProposal (deepenWith ("reason", "\"judged_false\""), "a reason that belongs to another tool");
    expectNoProposal (deepenWith ("reason", "5"), "numeric reason");
    expectNoProposal ("{\"tool\": \"no_action\", \"reason\": \"recurring_at_frequency\"}", "no_action with a deepen reason");
}

TEST (AiAction, ParseRejectsMissingFieldsAndExtraKeys)
{
    expectNoProposal ("{\"tool\": \"propose_deepen\", \"nid\": 5, \"expect_hz\": 1000, \"reason\": \"recurring_at_frequency\"}", "no target_db");
    expectNoProposal ("{\"tool\": \"propose_hold\", \"nid\": 5, \"expect_hz\": 1000, \"reason\": \"show_in_progress\"}", "no hold_s");
    expectNoProposal ("{\"tool\": \"propose_release\", \"expect_hz\": 1000, \"reason\": \"judged_false\"}", "no nid");
    expectNoProposal ("{\"tool\": \"propose_release\", \"nid\": 5, \"reason\": \"judged_false\"}", "no expect_hz");
    expectNoProposal ("{\"tool\": \"no_action\"}", "no reason");
    expectNoProposal ("{\"nid\": 5}", "no tool");
    // Extra keys, including the free-text kind the model might try to smuggle.
    expectNoProposal (deepenWith ("note", "\"please\""), "extra string key");
    expectNoProposal (deepenWith ("q", "30"), "extra numeric key");
    expectNoProposal ("{\"tool\": \"no_action\", \"reason\": \"nothing_to_do\", \"detail\": \"x\"}", "extra key on no_action");
    expectNoProposal ("{\"tool\": \"propose_release\", \"nid\": 5, \"expect_hz\": 1000, \"reason\": \"judged_false\", \"target_db\": -6}", "deepen field on release");
    expectNoProposal ("42", "action is a number");
    expectNoProposal ("\"propose_deepen\"", "action is a string");
    expectNoProposal ("[1]", "action is an array");
}

TEST (AiAction, ParseRejectsNonFiniteNumbers)
{
    // Python's json.dumps writes these tokens by default.
    for (const char* v : { "Infinity", "-Infinity", "NaN", "1e999", "-1e999" })
    {
        expectNoProposal (deepenWith ("expect_hz", v), v);
        expectNoProposal (deepenWith ("target_db", v), v);
        expectNoProposal (deepenWith ("nid", v), v);
    }
}

TEST (AiAction, ParseRejectsStringsBoolsAndObjectsForNumbers)
{
    for (const char* v : { "\"1000\"", "true", "null", "[1000]", "{\"hz\": 1000}", "{}" })
    {
        expectNoProposal (deepenWith ("expect_hz", v), v);
        expectNoProposal (deepenWith ("target_db", v), v);
        expectNoProposal (deepenWith ("nid", v), v);
    }
    expectNoProposal ("{\"tool\": \"propose_hold\", \"nid\": 5, \"expect_hz\": 1000, \"hold_s\": \"60\", \"reason\": \"show_in_progress\"}", "string hold_s");
    expectNoProposal ("{\"tool\": \"propose_hold\", \"nid\": 5, \"expect_hz\": 1000, \"hold_s\": true, \"reason\": \"show_in_progress\"}", "bool hold_s");
    expectNoProposal ("{\"tool\": \"propose_hold\", \"nid\": 5, \"expect_hz\": 1000, \"hold_s\": {\"s\": 60}, \"reason\": \"show_in_progress\"}", "object hold_s");
}

TEST (AiAction, ParseRejectsOutOfRangeAndNonIntegerNumbers)
{
    // nid 1..2^32-1, integer.
    for (const char* v : { "0", "-1", "4294967296", "1.5", "5.0" })
        expectNoProposal (deepenWith ("nid", v), v);
    // expect_hz 20..24000.
    for (const char* v : { "19.9", "0", "-100", "24000.1", "1e6" })
        expectNoProposal (deepenWith ("expect_hz", v), v);
    // target_db -60..0.
    for (const char* v : { "0.5", "1", "-60.5", "-1000" })
        expectNoProposal (deepenWith ("target_db", v), v);
    // hold_s 30..600, integer.
    for (const char* v : { "29", "601", "0", "-30", "60.5", "60.0" })
        expectNoProposal ("{\"tool\": \"propose_hold\", \"nid\": 5, \"expect_hz\": 1000, \"hold_s\": "
                              + juce::String (v) + ", \"reason\": \"show_in_progress\"}", v);

    // The bounds themselves are legal.
    for (const char* v : { "20", "24000" })
        EXPECT_EQ (parseSupervisorOutput (output (deepenWith ("expect_hz", v)), kGen).proposals.size(), 1u) << v;
    for (const char* v : { "-60", "0" })
        EXPECT_EQ (parseSupervisorOutput (output (deepenWith ("target_db", v)), kGen).proposals.size(), 1u) << v;
    EXPECT_EQ (parseSupervisorOutput (output (deepenWith ("nid", "4294967295")), kGen).proposals.size(), 1u);
}

TEST (AiAction, ParseKeepsGoodActionsAroundBadOnes)
{
    const auto r = parseSupervisorOutput (output (kDeepen + ", {\"tool\": \"nope\"}, " + kNoop + ", 7"), kGen);
    ASSERT_TRUE (r.ok());
    EXPECT_EQ (r.proposals.size(), 2u);
    EXPECT_EQ (r.droppedActions, 2);
}

TEST (AiAction, ParseUsageIsBestEffortAndNeverRejectsTheOutput)
{
    const auto bad = parseSupervisorOutput (
        "{\"supervise\": true, \"generation\": 7, \"actions\": [], \"usage\": {\"input_tokens\": \"x\", \"output_tokens\": -4}}", kGen);
    ASSERT_TRUE (bad.ok());
    EXPECT_FALSE (bad.usage.inputTokens.has_value());
    EXPECT_FALSE (bad.usage.outputTokens.has_value());
    EXPECT_TRUE (parseSupervisorOutput ("{\"supervise\": true, \"generation\": 7, \"actions\": []}", kGen).ok());
}

// ---------------------------------------------------------------------------
// Validator

TEST (AiAction, AcceptsADeepenOfExactlyOneRung)
{
    EXPECT_FALSE (validateProposal (deepenTo (5, 1000.0f, -12.0f), { live (5, 1000.0f, -6.0f, -18.0f) }).has_value());
    // Within one bin of expect_hz, either side.
    EXPECT_FALSE (validateProposal (deepenTo (5, 1000.0f + 23.0f, -12.0f), { live (5, 1000.0f, -6.0f, -18.0f) }).has_value());
    EXPECT_FALSE (validateProposal (deepenTo (5, 1000.0f - 23.0f, -12.0f), { live (5, 1000.0f, -6.0f, -18.0f) }).has_value());
    // The ceiling counts as the last rung (Q13): -6 -> -10 when the ceiling is -10.
    EXPECT_FALSE (validateProposal (deepenTo (5, 1000.0f, -10.0f), { live (5, 1000.0f, -6.0f, -10.0f) }).has_value());
    // no_action addresses nothing.
    AiProposal none;
    EXPECT_FALSE (validateProposal (none, {}).has_value());
}

TEST (AiAction, RefusesUnknownNid)
{
    const auto notches = std::vector<AiLiveNotch> { live (5, 1000.0f, -6.0f, -18.0f) };
    EXPECT_EQ (validateProposal (deepenTo (6, 1000.0f, -12.0f), notches), AiRefusal::UnknownNid);
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -12.0f), {}), AiRefusal::UnknownNid);
    AiProposal rel; rel.tool = Tool::Release; rel.nid = 9; rel.expectHz = 1000.0f;
    EXPECT_EQ (validateProposal (rel, notches), AiRefusal::UnknownNid);
}

TEST (AiAction, RefusesFrequencyMismatch)
{
    const auto notches = std::vector<AiLiveNotch> { live (5, 1000.0f, -6.0f, -18.0f) };
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f + 24.0f, -12.0f), notches), AiRefusal::FrequencyMismatch);
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f - 24.0f, -12.0f), notches), AiRefusal::FrequencyMismatch);
    EXPECT_EQ (validateProposal (deepenTo (5, 3000.0f, -12.0f), notches), AiRefusal::FrequencyMismatch);
    // A notch whose bin width is unknown cannot vouch for a frequency.
    auto blind = live (5, 1000.0f, -6.0f, -18.0f);
    blind.binHz = 0.0f;
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -12.0f), { blind }), AiRefusal::FrequencyMismatch);
    // Release and hold are checked the same way.
    AiProposal rel; rel.tool = Tool::Release; rel.nid = 5; rel.expectHz = 2000.0f;
    EXPECT_EQ (validateProposal (rel, notches), AiRefusal::FrequencyMismatch);
}

TEST (AiAction, RefusesNotDetectorOrigin)
{
    for (const auto o : { NotchController::Origin::Preset, NotchController::Origin::Manual,
                          NotchController::Origin::Soundcheck })
    {
        auto n = live (5, 1000.0f, -6.0f, -18.0f);
        n.origin = o;
        EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -12.0f), { n }), AiRefusal::NotDetectorOrigin) << (int) o;
        AiProposal hold; hold.tool = Tool::Hold; hold.nid = 5; hold.expectHz = 1000.0f; hold.holdSeconds = 60;
        EXPECT_EQ (validateProposal (hold, { n }), AiRefusal::NotDetectorOrigin) << (int) o;
    }
}

TEST (AiAction, RefusesNotDeeper)
{
    const auto notches = std::vector<AiLiveNotch> { live (5, 1000.0f, -12.0f, -24.0f) };
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -12.0f), notches), AiRefusal::NotDeeper);   // equal
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -6.0f), notches), AiRefusal::NotDeeper);    // shallower
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, 0.0f), notches), AiRefusal::NotDeeper);
}

TEST (AiAction, RefusesBeyondCeiling)
{
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -24.0f), { live (5, 1000.0f, -12.0f, -18.0f) }),
               AiRefusal::BeyondCeiling);
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -12.0f), { live (5, 1000.0f, -6.0f, -10.0f) }),
               AiRefusal::BeyondCeiling);   // -12 is a rung but the ceiling is -10
    // A notch already AT its ceiling has nowhere deeper to go.
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -24.0f), { live (5, 1000.0f, -18.0f, -18.0f) }),
               AiRefusal::BeyondCeiling);
    // A non-finite ceiling never validates a deepen.
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -12.0f),
                                 { live (5, 1000.0f, -6.0f, std::nanf ("")) }),
               AiRefusal::BeyondCeiling);
}

TEST (AiAction, RefusesMoreThanOneRung)
{
    const auto notches = std::vector<AiLiveNotch> { live (5, 1000.0f, -6.0f, -24.0f) };
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -18.0f), notches), AiRefusal::MoreThanOneRung);   // two rungs
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -24.0f), notches), AiRefusal::MoreThanOneRung);   // three
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -9.0f), notches), AiRefusal::MoreThanOneRung);    // off the ladder
    EXPECT_EQ (validateProposal (deepenTo (5, 1000.0f, -12.5f), notches), AiRefusal::MoreThanOneRung);
    // The rung is read from the controller, so the test derives it the same way.
    const double next = NotchController::nextDeeperRungDb (-6.0, -24.0);
    EXPECT_FALSE (validateProposal (deepenTo (5, 1000.0f, (float) next), notches).has_value());
}

// ---------------------------------------------------------------------------
// Names (every enumerator, not a hand-picked list)

TEST (Author, NameLoopsOverEveryEnumerator)
{
    // Llm is the LAST enumerator. Adding one after it fails this line, which is
    // the cue to extend the table below -- a loop that stops at a hardcoded
    // "last" would silently skip the new one.
    ASSERT_EQ ((int) Author::Llm, 3);
    const char* expected[] = { "detector", "human", "network", "llm" };
    std::set<std::string> seen;
    for (int i = 0; i <= (int) Author::Llm; ++i)
    {
        const std::string name = authorName ((Author) i);
        EXPECT_EQ (name, expected[i]) << i;
        EXPECT_TRUE (seen.insert (name).second) << "duplicate name " << name;
    }
    EXPECT_EQ (seen.size(), 4u);
}

TEST (AiAction, NameTablesCoverEveryEnumeratorWithDistinctNames)
{
    ASSERT_EQ ((int) AiProposal::Tool::NoAction, 3);
    ASSERT_EQ ((int) AiProposal::Reason::OutsideScope, 7);
    ASSERT_EQ ((int) AiRefusal::MoreThanOneRung, 5);
    ASSERT_EQ ((int) AiDecision::Stale, 3);
    std::set<std::string> tools, reasons, refusals, decisions;
    for (int i = 0; i <= 3; ++i)
    {
        EXPECT_TRUE (tools.insert (aiToolName ((Tool) i)).second);
        EXPECT_TRUE (decisions.insert (aiDecisionName ((AiDecision) i)).second);
    }
    for (int i = 0; i <= 7; ++i)
        EXPECT_TRUE (reasons.insert (aiReasonName ((Reason) i)).second);
    for (int i = 0; i <= 5; ++i)
        EXPECT_TRUE (refusals.insert (aiRefusalName ((AiRefusal) i)).second);
    EXPECT_TRUE (decisions.count ("dismissed") && decisions.count ("expired")
                 && decisions.count ("refused") && decisions.count ("stale"));
    EXPECT_TRUE (refusals.count ("unknown_nid") && refusals.count ("more_than_one_rung"));
}

// ---------------------------------------------------------------------------
// Supervise argv

namespace
{
CopilotRequest superviseRequest()
{
    CopilotRequest r;
    r.executable = juce::File ("C:/x/HandsFreeCopilot.exe");
    r.sessionLog = juce::File ("C:/x/session.jsonl");
    r.statePath  = juce::File ("C:/x y/supervisor-state.json");
    r.supervise  = true;
    r.provider   = "anthropic";
    return r;
}
}

TEST (SuperviseArguments, AnthropicExactArgv)
{
    auto r = superviseRequest();
    const juce::StringArray expected { r.executable.getFullPathName(), r.sessionLog.getFullPathName(),
                                       "--supervise", "--state=" + r.statePath.getFullPathName(),
                                       "--json", "--low-priority", "--credentials-only",
                                       "--provider=anthropic", "--timeout=50" };
    EXPECT_EQ (buildCopilotSuperviseArguments (r), expected);
    EXPECT_EQ (buildCopilotArguments (r), expected);   // a supervise request routes here

    r.model = "claude-opus-5-5";
    const juce::StringArray withModel { r.executable.getFullPathName(), r.sessionLog.getFullPathName(),
                                        "--supervise", "--state=" + r.statePath.getFullPathName(),
                                        "--json", "--low-priority", "--credentials-only",
                                        "--provider=anthropic", "--model=claude-opus-5-5", "--timeout=50" };
    EXPECT_EQ (buildCopilotSuperviseArguments (r), withModel);
}

TEST (SuperviseArguments, OpenAiCompatibleExactArgv)
{
    auto r = superviseRequest();
    r.provider = "openai_compatible";
    r.model    = "llama3";
    r.baseUrl  = "http://localhost:11434/v1";
    const juce::StringArray expected { r.executable.getFullPathName(), r.sessionLog.getFullPathName(),
                                       "--supervise", "--state=" + r.statePath.getFullPathName(),
                                       "--json", "--low-priority", "--credentials-only",
                                       "--provider=openai_compatible", "--model=llama3",
                                       "--base-url=http://localhost:11434/v1", "--timeout=50" };
    EXPECT_EQ (buildCopilotSuperviseArguments (r), expected);
}

TEST (SuperviseArguments, EveryInvalidRequestIsEmpty)
{
    auto bad = [] (auto mutate)
    {
        auto r = superviseRequest();
        mutate (r);
        return buildCopilotSuperviseArguments (r).isEmpty();
    };
    EXPECT_TRUE (bad ([] (CopilotRequest& r) { r.provider = "gemini"; }));
    EXPECT_TRUE (bad ([] (CopilotRequest& r) { r.provider = ""; }));
    EXPECT_TRUE (bad ([] (CopilotRequest& r) { r.model = "bad model"; }));
    EXPECT_TRUE (bad ([] (CopilotRequest& r) { r.baseUrl = "https://x"; }));   // anthropic takes no base URL
    EXPECT_TRUE (bad ([] (CopilotRequest& r) { r.provider = "openai_compatible"; r.baseUrl = "http://h"; }));   // no model
    EXPECT_TRUE (bad ([] (CopilotRequest& r) { r.provider = "openai_compatible"; r.model = "m"; r.baseUrl = "ftp://h"; }));
    EXPECT_TRUE (bad ([] (CopilotRequest& r) { r.statePath = juce::File(); }));
    EXPECT_TRUE (bad ([] (CopilotRequest& r) { r.statePath = juce::File ("C:/x/a\"b.json"); }));
}

TEST (SuperviseArguments, AskAndFindingsArgvAreUnchanged)
{
    CopilotRequest r;
    r.executable = juce::File ("C:/x/HandsFreeCopilot.exe");
    r.sessionLog = juce::File ("C:/x/session.jsonl");
    r.statePath  = juce::File ("C:/x/state.json");   // ignored unless `supervise`
    const auto findings = buildCopilotArguments (r);
    const juce::StringArray expectedFindings { r.executable.getFullPathName(), r.sessionLog.getFullPathName(),
                                               "--json", "--low-priority" };
    EXPECT_EQ (findings, expectedFindings);
    r.question = "why?";
    r.provider = "anthropic";
    EXPECT_EQ (buildCopilotArguments (r).joinIntoString ("|").fromFirstOccurrenceOf ("--ask", true, false),
               "--ask=why?|--json|--low-priority|--credentials-only|--provider=anthropic|--timeout=100");
}

// ---------------------------------------------------------------------------
// Log schema 4

TEST (NotchEventToVar, AuthorAndNidOnEveryNotchLine)
{
    using Ev = NotchController::NotchEvent;
    ASSERT_EQ ((int) Ev::Kind::Retune, 2);   // last kind: a new kind must be added below
    const std::uint32_t nids[] = { 1u, 77u, 3000000000u };   // the last is above 2^31

    for (int k = 0; k <= (int) Ev::Kind::Retune; ++k)
        for (int a = 0; a <= (int) Author::Llm; ++a)
            for (const auto nid : nids)
            {
                Ev e;
                e.kind = (Ev::Kind) k;
                e.author = (Author) a;
                e.nid = nid;
                const auto v = MainComponent::notchEventToVarForTest (e);
                const std::string where = "kind " + std::to_string (k) + " author " + std::to_string (a);
                EXPECT_EQ (v["author"].toString(), juce::String (authorName ((Author) a))) << where;
                EXPECT_EQ ((juce::int64) v["nid"], (juce::int64) nid) << where;
                EXPECT_GT ((juce::int64) v["nid"], 0) << where;
                // The existing fields are still all there.
                EXPECT_TRUE (v.hasProperty ("slot") && v.hasProperty ("origin") && v.hasProperty ("depth_db")) << where;
                // And it survives the text round trip the log file does.
                const auto parsed = juce::JSON::parse (juce::JSON::toString (v, true));
                EXPECT_EQ ((juce::int64) parsed["nid"], (juce::int64) nid) << where;
            }

    // A scored placement (the long form) carries them too.
    Ev scored;
    scored.kind = Ev::Kind::Set;
    scored.hasScore = true;
    scored.author = Author::Detector;
    scored.nid = 9;
    const auto v = MainComponent::notchEventToVarForTest (scored);
    EXPECT_EQ (v["author"].toString(), "detector");
    EXPECT_EQ ((int) v["nid"], 9);
    EXPECT_TRUE (v.hasProperty ("score"));
}

TEST (AiLog, ModeEventShape)
{
    const auto v = MainComponent::aiModeEventToVar (AiMode::Act, AiMode::Suggest, AiMode::Shadow);
    EXPECT_EQ (v["ev"].toString(), "ai_mode");
    EXPECT_EQ (v["mode"].toString(), "act");
    EXPECT_EQ (v["effective_llm"].toString(), "suggest");
    EXPECT_EQ (v["effective_network"].toString(), "shadow");
}

TEST (AiLog, CallEventShapeAndRounding)
{
    AiUsage u;
    u.inputTokens = 1234;   // outputTokens left unknown
    const auto v = MainComponent::aiCallEventToVar ("recurrence", 7, 1234.5678, "ok", &u);
    EXPECT_EQ (v["ev"].toString(), "ai_call");
    EXPECT_EQ (v["trigger"].toString(), "recurrence");
    EXPECT_EQ ((int) v["gen"], 7);
    EXPECT_DOUBLE_EQ ((double) v["ms"], 1230.0);   // 3 significant digits
    EXPECT_EQ (v["status"].toString(), "ok");
    ASSERT_TRUE (v["usage"].getDynamicObject() != nullptr);
    EXPECT_EQ ((int) v["usage"]["input_tokens"], 1234);
    EXPECT_TRUE (v["usage"]["output_tokens"].isVoid());   // null, not 0

    const auto none = MainComponent::aiCallEventToVar ("timer", 8, 0.0, "timed_out", nullptr);
    EXPECT_FALSE (none.hasProperty ("usage"));
}

TEST (AiLog, ProposalEventShapePerTool)
{
    AiProposal d = deepenTo (5, 1000.123f, -12.0f);
    const auto vd = MainComponent::aiProposalEventToVar (7, d);
    EXPECT_EQ (vd["ev"].toString(), "ai_proposal");
    EXPECT_EQ ((int) vd["gen"], 7);
    EXPECT_EQ (vd["tool"].toString(), "propose_deepen");
    EXPECT_EQ ((int) vd["nid"], 5);
    EXPECT_DOUBLE_EQ ((double) vd["hz"], 1000.0);   // 3 significant digits
    EXPECT_DOUBLE_EQ ((double) vd["target_db"], -12.0);
    EXPECT_FALSE (vd.hasProperty ("hold_s"));
    EXPECT_EQ (vd["reason"].toString(), "recurring_at_frequency");

    AiProposal h; h.tool = Tool::Hold; h.nid = 6; h.expectHz = 2000.0f; h.holdSeconds = 120;
    h.reason = Reason::ShowInProgress;
    const auto vh = MainComponent::aiProposalEventToVar (7, h);
    EXPECT_EQ ((int) vh["hold_s"], 120);
    EXPECT_FALSE (vh.hasProperty ("target_db"));

    AiProposal r; r.tool = Tool::Release; r.nid = 7; r.expectHz = 300.0f; r.reason = Reason::JudgedFalse;
    const auto vr = MainComponent::aiProposalEventToVar (7, r);
    EXPECT_EQ (vr["tool"].toString(), "propose_release");
    EXPECT_FALSE (vr.hasProperty ("target_db") || vr.hasProperty ("hold_s"));

    AiProposal n;   // defaults: no_action / nothing_to_do
    const auto vn = MainComponent::aiProposalEventToVar (7, n);
    EXPECT_EQ (vn["tool"].toString(), "no_action");
    EXPECT_FALSE (vn.hasProperty ("nid") || vn.hasProperty ("hz"));
    EXPECT_EQ (vn["reason"].toString(), "nothing_to_do");
}

TEST (AiLog, DecisionEventShape)
{
    const auto refused = MainComponent::aiDecisionEventToVar (7, 5, Tool::Deepen, AiDecision::Refused,
                                                              AiRefusal::BeyondCeiling);
    EXPECT_EQ (refused["ev"].toString(), "ai_decision");
    EXPECT_EQ ((int) refused["gen"], 7);
    EXPECT_EQ ((int) refused["nid"], 5);
    EXPECT_EQ (refused["tool"].toString(), "propose_deepen");
    EXPECT_EQ (refused["decision"].toString(), "refused");
    EXPECT_EQ (refused["refusal"].toString(), "beyond_ceiling");

    for (const auto d : { AiDecision::Dismissed, AiDecision::Expired, AiDecision::Stale })
    {
        const auto v = MainComponent::aiDecisionEventToVar (7, 5, Tool::Hold, d, std::nullopt);
        EXPECT_EQ (v["decision"].toString(), juce::String (aiDecisionName (d)));
        EXPECT_FALSE (v.hasProperty ("refusal"));
    }
}
