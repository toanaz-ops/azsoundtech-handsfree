// Lane L3a: the headless AI supervisor engine. Fake clock, fake launcher: no
// process, no wall clock, no network, no key. Phase L3a changes the audio by
// exactly 0 dB -- nothing the supervisor produces is applied to a notch, and
// the AiMode.* tests below prove it against a real NotchController.

#include <gtest/gtest.h>

#include "app/AiSupervisor.h"
#include "app/MainComponent.h"
#include "app/NotchController.h"
#include "dsp/ClockSource.h"
#include "dsp/LockFreeRingBuffer.h"
#include "dsp/NotchCommand.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace
{
class FakeLauncher : public AiLauncher
{
public:
    std::vector<CopilotRequest> requests;
    std::function<void (CopilotRunResult)> pending;
    bool busy = false, refuse = false;
    int cancels = 0;

    bool start (CopilotRequest r, std::function<void (CopilotRunResult)> onDone) override
    {
        if (refuse)
            return false;
        requests.push_back (std::move (r));
        pending = std::move (onDone);
        busy = true;
        return true;
    }
    void cancel() override { ++cancels; busy = false; }   // pending is kept: a test may deliver LATE
    [[nodiscard]] bool isBusy() const override { return busy; }

    void deliver (CopilotRunResult r)
    {
        busy = false;
        auto cb = pending;
        ASSERT_TRUE (cb != nullptr);
        cb (std::move (r));
    }
};

CopilotRunResult okResult (const juce::String& stdoutText)
{
    CopilotRunResult r;
    r.status = CopilotRunResult::Status::ok;
    r.exitCode = 0;
    r.stdoutText = stdoutText;
    return r;
}

juce::String output (std::uint32_t generation, const juce::String& actions)
{
    return "{\"supervise\": true, \"generation\": " + juce::String ((int) generation)
           + ", \"actions\": [" + actions + "], \"provider\": \"anthropic\", \"model\": \"m\","
             " \"usage\": {\"input_tokens\": 100, \"output_tokens\": 20}}";
}

juce::String deepen (int nid, double hz, double targetDb)
{
    return "{\"tool\": \"propose_deepen\", \"nid\": " + juce::String (nid) + ", \"expect_hz\": " + juce::String (hz)
           + ", \"target_db\": " + juce::String (targetDb) + ", \"reason\": \"recurring_at_frequency\"}";
}
juce::String hold (int nid, double hz)
{
    return "{\"tool\": \"propose_hold\", \"nid\": " + juce::String (nid) + ", \"expect_hz\": " + juce::String (hz)
           + ", \"hold_s\": 120, \"reason\": \"show_in_progress\"}";
}
juce::String release (int nid, double hz)
{
    return "{\"tool\": \"propose_release\", \"nid\": " + juce::String (nid) + ", \"expect_hz\": " + juce::String (hz)
           + ", \"reason\": \"judged_false\"}";
}

AiStateNotch notchAt (std::uint32_t nid, double hz, double depth = -6.0, double ceiling = -18.0, int index = 0)
{
    AiStateNotch n;
    n.nid = nid; n.index = index; n.hz = (float) hz; n.q = 30.0f;
    n.depthDb = (float) depth; n.ceilingDb = (float) ceiling;
    n.seenS = 12.0;
    return n;
}

constexpr std::int64_t kSec = 1000;

struct Rig
{
    std::int64_t ms = 5'000'000;
    FakeLauncher* launcher = nullptr;
    std::unique_ptr<AiSupervisor> sup;
    AiStateSnapshot state;
    std::vector<juce::var> logs;
    bool asking = false;
    int changedCount = 0;
    juce::File dir;
    std::vector<std::int64_t> persisted;   // what the supervisor asked to be saved (wall-clock epoch ms)
    int saves = 0;
    bool failWrites = false;               // make the state-file write fail (a directory sits on its path)

    // `skipStartupGap`: the first call normally waits 120 s from app start; most
    // tests are not about that and skip it. `startMs` doubles as the wall clock.
    explicit Rig (AiMode mode = AiMode::Suggest, bool skipStartupGap = true,
                  std::vector<std::int64_t> initialStarts = {}, std::int64_t startMs = 5'000'000)
    {
        ms = startMs;
        persisted = initialStarts;
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("aisup-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        const auto log = dir.getChildFile ("session.jsonl");
        log.replaceWithText ("{}\n");

        state.configured = true;
        state.sampleRate = 48000.0;
        state.binHz = 48000.0 / 2048.0;
        state.request.executable = juce::File ("C:/x/HandsFreeCopilot.exe");
        state.request.sessionLog = log;
        state.request.provider = "anthropic";
        state.request.model = "m";
        AiStateSlot s;
        s.slot = 0;
        s.notches.push_back (notchAt (5, 1000.0));
        state.slots.push_back (s);

        AiSupervisor::Environment env;
        env.nowMs = [this] { return ms; };
        auto l = std::make_unique<FakeLauncher>();
        launcher = l.get();
        env.launcher = std::move (l);
        env.readState = [this] { return state; };
        env.logSink = [this] (const juce::var& v) { logs.push_back (v); };
        env.isAskRunning = [this] { return asking; };
        env.wallMs = [this] { return ms; };
        env.loadCallStarts = [initialStarts] { return initialStarts; };
        env.saveCallStarts = [this] (const std::vector<std::int64_t>& v) { persisted = v; ++saves; };
        sup = std::make_unique<AiSupervisor> (std::move (env));
        if (skipStartupGap)
            sup->skipStartupGapForTest();
        sup->onChanged = [this] { ++changedCount; };
        sup->setRequestedMode (mode);
        logs.clear();
        sup->tick();   // sets the heartbeat baseline
    }
    ~Rig() { sup.reset(); dir.deleteRecursively(); }

    void step (std::int64_t addMs)
    {
        ms += addMs;
        sup->tick();
    }
    int calls() const { return (int) launcher->requests.size(); }

    // Start a call now (manual) and return its generation.
    std::uint32_t startManual (std::int64_t waitMs = 2 * kSec)
    {
        sup->requestManualCall();
        step (waitMs);
        return sup->generation();
    }
    void finishOk (const juce::String& actions = {})
    {
        launcher->deliver (okResult (output (sup->generation(), actions)));
    }

    std::vector<juce::var> events (const juce::String& ev) const
    {
        std::vector<juce::var> out;
        for (const auto& v : logs)
            if (v["ev"].toString() == ev)
                out.push_back (v);
        return out;
    }
    [[nodiscard]] bool hasCallStatus (const juce::String& status) const
    {
        for (const auto& v : events ("ai_call"))
            if (v["status"].toString() == status)
                return true;
        return false;
    }
    [[nodiscard]] juce::var stateFile() const
    {
        return juce::JSON::parse (launcher->requests.back().statePath);
    }
    [[nodiscard]] juce::var readStateFile (std::size_t request = 0) const
    {
        return juce::JSON::parse (launcher->requests[request].statePath);
    }
};

std::set<juce::String> keysOf (const juce::var& v)
{
    std::set<juce::String> k;
    if (auto* o = v.getDynamicObject())
        for (const auto& p : o->getProperties())
            k.insert (p.name.toString());
    return k;
}
} // namespace

//==============================================================================
// Triggers.

TEST (AiSupervisorTrigger, RecurringBinFiresOnTheThirdPlacementInTenMinutes)
{
    Rig r;
    r.sup->notePlacement (0, 0, 1000.0, r.ms);
    r.sup->notePlacement (0, 0, 1004.0, r.ms + 1000);          // same bin
    r.step (2 * kSec);
    EXPECT_EQ (r.calls(), 0) << "two placements are not recurrence";
    r.sup->notePlacement (0, 0, 3000.0, r.ms);                  // a different bin does not count
    r.step (2 * kSec);
    EXPECT_EQ (r.calls(), 0);
    r.sup->notePlacement (0, 0, 1002.0, r.ms);
    r.step (kSec);
    ASSERT_EQ (r.calls(), 1);
    EXPECT_EQ (r.stateFile()["trigger"].toString(), "recurring_bin");
}

TEST (AiSupervisorTrigger, RecurringBinIgnoresPlacementsOlderThanTenMinutes)
{
    Rig r;
    r.sup->notePlacement (0, 0, 1000.0, r.ms);
    r.sup->notePlacement (0, 0, 1000.0, r.ms + 1000);
    r.ms += 11 * 60 * kSec;
    r.sup->notePlacement (0, 0, 1000.0, r.ms);
    r.step (kSec);
    EXPECT_EQ (r.calls(), 0) << "only one placement is inside the window";
}

TEST (AiSupervisorTrigger, AtCeilingFiresAfterThirtySecondsAndOnlyOncePerNotch)
{
    Rig r;
    r.state.slots[0].notches[0] = notchAt (5, 1000.0, -18.0, -18.0);
    r.step (kSec);                       // first seen at the ceiling
    r.step (29 * kSec);
    EXPECT_EQ (r.calls(), 0) << "29 s is not 30 s";
    r.step (kSec);                       // 30 s since first seen
    ASSERT_EQ (r.calls(), 1);
    EXPECT_EQ (r.stateFile()["trigger"].toString(), "at_ceiling");
    r.finishOk();

    // The same notch never triggers again, however long it sits there.
    r.step (130 * kSec);
    r.step (130 * kSec);
    EXPECT_EQ (r.calls(), 1);
}

TEST (AiSupervisorTrigger, AtCeilingNeedsADetectorNotchActuallyAtItsCeiling)
{
    Rig r;
    r.state.slots[0].notches[0] = notchAt (5, 1000.0, -6.0, -18.0);   // shallower than its ceiling
    auto manual = notchAt (6, 2000.0, -18.0, -18.0, 1);
    manual.origin = NotchController::Origin::Manual;
    r.state.slots[0].notches.push_back (manual);
    r.step (kSec);
    r.step (60 * kSec);
    EXPECT_EQ (r.calls(), 0);
}

TEST (AiSupervisorTrigger, HeartbeatNeedsTenMinutesAndAChangedNotchSet)
{
    Rig r;
    r.step (9 * 60 * kSec + 59 * kSec);
    EXPECT_EQ (r.calls(), 0) << "under ten minutes";
    r.step (kSec);
    EXPECT_EQ (r.calls(), 0) << "ten minutes but nothing changed";
    r.state.slots[0].notches.push_back (notchAt (6, 2500.0, -6.0, -18.0, 1));
    r.step (kSec);
    ASSERT_EQ (r.calls(), 1);
    EXPECT_EQ (r.stateFile()["trigger"].toString(), "heartbeat");
    r.finishOk();

    // And after a call, another ten minutes AND a change are needed again.
    r.step (11 * 60 * kSec);
    EXPECT_EQ (r.calls(), 1);
    r.state.slots[0].notches[0].depthDb = -12.0f;
    r.step (kSec);
    EXPECT_EQ (r.calls(), 2);
}

//==============================================================================
// Limits.

TEST (AiSupervisorLimits, AtLeastOneHundredTwentySecondsBetweenCalls)
{
    Rig r;
    r.startManual();
    ASSERT_EQ (r.calls(), 1);
    r.finishOk();
    r.sup->requestManualCall();
    r.step (60 * kSec);
    EXPECT_EQ (r.calls(), 1) << "60 s after the start";
    r.step (59 * kSec);
    EXPECT_EQ (r.calls(), 1) << "119 s after the start";
    r.step (2 * kSec);
    EXPECT_EQ (r.calls(), 2);
}

TEST (AiSupervisorLimits, TwelveCallsPerRollingHour)
{
    Rig r;
    std::int64_t firstStart = 0;
    for (int i = 0; i < 12; ++i)
    {
        r.sup->requestManualCall();
        r.step (121 * kSec);
        ASSERT_EQ (r.calls(), i + 1) << "call " << i;
        if (i == 0)
            firstStart = r.ms;
        r.finishOk();
    }
    r.sup->requestManualCall();
    r.step (121 * kSec);
    EXPECT_EQ (r.calls(), 12) << "the 13th inside the hour must wait";

    int guard = 0;
    while (r.calls() == 12 && guard++ < 100)
        r.step (60 * kSec);
    ASSERT_EQ (r.calls(), 13);
    EXPECT_GE (r.ms - firstStart, 3600 * kSec) << "it fires only once the first call is an hour old";
}

TEST (AiSupervisorLimits, SessionBudgetIsSettableAndReportedAsExhausted)
{
    Rig r;
    EXPECT_EQ (r.sup->sessionBudget(), 40);
    r.sup->setSessionBudget (2);
    for (int i = 0; i < 2; ++i)
    {
        r.startManual (121 * kSec);
        ASSERT_EQ (r.calls(), i + 1);
        r.finishOk();
    }
    r.sup->requestManualCall();
    r.step (121 * kSec);
    EXPECT_EQ (r.calls(), 2);
    EXPECT_EQ (r.sup->callsThisSession(), 2);
    EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::BudgetExhausted);
}

TEST (AiSupervisorLimits, NoSecondCallWhileOneIsInFlight)
{
    Rig r;
    r.startManual();
    ASSERT_EQ (r.calls(), 1);
    r.sup->requestManualCall();
    r.step (300 * kSec);
    EXPECT_EQ (r.calls(), 1);
}

TEST (AiSupervisorLimits, ALauncherThatRefusesCostsNoBudget)
{
    Rig r;
    r.launcher->refuse = true;
    r.sup->requestManualCall();
    r.step (2 * kSec);
    EXPECT_EQ (r.sup->callsThisSession(), 0);
    r.launcher->refuse = false;
    r.step (kSec);
    EXPECT_EQ (r.calls(), 1) << "the pending trigger survived the refusal";
}

TEST (AiSupervisorLimits, TheFirstCallWaitsOneHundredTwentySecondsFromAppStart)
{
    Rig r (AiMode::Suggest, false);
    r.sup->requestManualCall();
    r.step (60 * kSec);
    EXPECT_EQ (r.calls(), 0) << "60 s after start";
    r.step (59 * kSec);
    EXPECT_EQ (r.calls(), 0) << "119 s after start";
    r.step (2 * kSec);
    EXPECT_EQ (r.calls(), 1) << "past 120 s the pending trigger fires";
}

TEST (AiSupervisorLimits, TheHourlyBudgetSurvivesARestart)
{
    Rig first;
    for (int i = 0; i < 12; ++i)
    {
        first.sup->requestManualCall();
        first.step (121 * kSec);
        ASSERT_EQ (first.calls(), i + 1) << "call " << i;
        first.finishOk();
    }
    ASSERT_EQ (first.persisted.size(), 12u);
    EXPECT_EQ (first.saves, 12) << "written when each call starts";

    // A second supervisor, built from the persisted values only, a minute later.
    Rig second (AiMode::Suggest, true, first.persisted, first.ms + 60 * kSec);
    second.sup->requestManualCall();
    second.step (5 * kSec);
    second.step (200 * kSec);
    EXPECT_EQ (second.calls(), 0) << "the 13th call inside the hour is refused after a restart";
    EXPECT_EQ (second.saves, 0);

    // The oldest entry leaves the window: one call is allowed again.
    second.ms = first.persisted.front() + 3600 * kSec + kSec;
    second.step (kSec);
    EXPECT_EQ (second.calls(), 1);
}

TEST (AiSupervisorLimits, TheMinimumGapAlsoCountsFromThePersistedLastStart)
{
    Rig a (AiMode::Suggest, true, { 5'000'000 - 30 * kSec });   // the previous run's call, 30 s ago
    a.sup->requestManualCall();
    a.step (60 * kSec);
    EXPECT_EQ (a.calls(), 0) << "only 90 s since the persisted start";
    a.step (31 * kSec);
    EXPECT_EQ (a.calls(), 1);
}

TEST (AiSupervisorLimits, FutureDatedAndMalformedPersistedEntries)
{
    // 12 entries dated a few seconds ahead (within the 60 s tolerance): they count as "now".
    std::vector<std::int64_t> near;
    for (int i = 0; i < 12; ++i)
        near.push_back (5'000'000 + 30 * kSec + i);
    Rig n (AiMode::Suggest, true, near);
    n.sup->requestManualCall();
    n.step (50 * kSec);   // wall now 5'050'000: the entries are 20 s old, still inside the hour
    EXPECT_EQ (n.calls(), 0) << "near-future entries count as now and fill the hour";

    // 12 entries days ahead cannot be real: dropped, so they neither fill the
    // hour nor block the minimum gap forever (the old behaviour bricked the
    // supervisor until the wall clock passed them, with status Idle).
    std::vector<std::int64_t> future;
    for (int i = 0; i < 12; ++i)
        future.push_back (5'000'000 + 7 * 24 * 3600 * kSec + i);
    Rig f (AiMode::Suggest, false, future);   // keep the real 120 s startup gap
    f.sup->requestManualCall();
    f.step (60 * kSec);
    EXPECT_EQ (f.calls(), 0) << "the first call still waits the gap from app start";
    f.step (61 * kSec);
    EXPECT_EQ (f.calls(), 1) << "a far-future entry must not brick the supervisor";
    ASSERT_EQ (f.persisted.size(), 1u) << "the unreal entries are not written back";

    // Malformed entries (zero, negative) are dropped, not counted.
    Rig m (AiMode::Suggest, true, { 0, -5, -1000000 });
    m.startManual();
    EXPECT_EQ (m.calls(), 1);
    ASSERT_EQ (m.persisted.size(), 1u) << "only the real start is kept";
}

TEST (AiSupervisorLimits, OnlyTheNewestTwelveStartsArePersisted)
{
    std::vector<std::int64_t> old;
    for (int i = 0; i < 20; ++i)
        old.push_back (1000 + i);   // ancient: outside the hour
    Rig r (AiMode::Suggest, true, old);
    r.startManual();
    ASSERT_EQ (r.calls(), 1);
    ASSERT_EQ (r.persisted.size(), 12u);
    EXPECT_EQ (r.persisted.back(), r.ms);
}

TEST (AiSupervisorLimits, ThreeFailuresInARowPauseAutomaticCallsAndASuccessResumes)
{
    Rig r;
    CopilotRunResult bad; bad.status = CopilotRunResult::Status::sidecarMissing;
    CopilotRunResult slow; slow.status = CopilotRunResult::Status::timedOut;

    // Three AUTOMATIC calls (recurring_bin) that fail / time out.
    const CopilotRunResult results[3] = { bad, slow, bad };
    for (int i = 0; i < 3; ++i)
    {
        for (int k = 0; k < 3; ++k)
            r.sup->notePlacement (0, 0, 1000.0, r.ms);
        r.step (121 * kSec);
        ASSERT_EQ (r.calls(), i + 1) << "automatic call " << i;
        r.launcher->deliver (results[i]);
    }
    EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::Paused);
    EXPECT_EQ (r.sup->statusText(), juce::String::fromUTF8 ("AI \xc2\xb7 t\xe1\xba\xa1m d\xe1\xbb\xabng sau 3 l\xe1\xbb\x97i li\xc3\xaan ti\xe1\xba\xbfp"));

    // Paused: automatic triggers do not start a call...
    for (int k = 0; k < 3; ++k)
        r.sup->notePlacement (0, 0, 1000.0, r.ms);
    r.step (300 * kSec);
    EXPECT_EQ (r.calls(), 3);
    // ...an operator's manual call still does, and its success resumes.
    r.sup->requestManualCall();
    r.step (kSec);
    ASSERT_EQ (r.calls(), 4);
    r.finishOk();
    EXPECT_NE (r.sup->status().kind, AiSupervisor::StatusKind::Paused);
    for (int k = 0; k < 3; ++k)
        r.sup->notePlacement (0, 0, 1000.0, r.ms);
    r.step (121 * kSec);
    EXPECT_EQ (r.calls(), 5) << "automatic calls are back";
}

TEST (AiSupervisorLimits, ACancelledCallDoesNotCountAndAModeChangeLiftsThePause)
{
    Rig r;
    CopilotRunResult bad; bad.status = CopilotRunResult::Status::sidecarMissing;
    for (int i = 0; i < 3; ++i)
    {
        for (int k = 0; k < 3; ++k)
            r.sup->notePlacement (0, 0, 1000.0, r.ms);
        r.step (121 * kSec);
        ASSERT_EQ (r.calls(), i + 1);
        r.launcher->deliver (bad);
    }
    ASSERT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::Paused);
    r.sup->setRequestedMode (AiMode::Shadow);
    r.sup->setRequestedMode (AiMode::Suggest);
    EXPECT_NE (r.sup->status().kind, AiSupervisor::StatusKind::Paused);

    // cancelled is not a failure: three invalidated calls leave no pause.
    Rig c;
    for (int i = 0; i < 3; ++i)
    {
        for (int k = 0; k < 3; ++k)
            c.sup->notePlacement (0, 0, 1000.0, c.ms);
        c.step (121 * kSec);
        ASSERT_EQ (c.calls(), i + 1);
        c.sup->invalidate();
    }
    EXPECT_NE (c.sup->status().kind, AiSupervisor::StatusKind::Paused);
}

TEST (AiSupervisorLimits, AFailedStateFileWriteIsOneFailedCallNotARetryLoop)
{
    Rig r;
    // The state file's path is occupied by a non-empty directory: the replace fails.
    const auto target = r.state.request.sessionLog.getParentDirectory().getChildFile (AiSupervisor::kStateFileName);
    ASSERT_TRUE (target.createDirectory());
    ASSERT_TRUE (target.getChildFile ("keep").replaceWithText ("x"));

    r.sup->requestManualCall();
    for (int i = 0; i < 20; ++i)
        r.step (kSec);
    EXPECT_EQ (r.calls(), 0);
    EXPECT_EQ (r.events ("ai_call").size(), 1u) << "one ai_call failed line, not one per tick";
    EXPECT_EQ (r.events ("ai_call")[0]["status"].toString(), "failed");
    EXPECT_EQ (r.sup->callsThisSession(), 0) << "nothing started: no budget spent";
    EXPECT_EQ (r.saves, 0);
    EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::LastFailed);

    // Recover: the pending trigger was dropped, so nothing fires until asked again.
    target.deleteRecursively();
    r.step (10 * kSec);
    EXPECT_EQ (r.calls(), 0);
    r.sup->requestManualCall();
    r.step (kSec);
    EXPECT_EQ (r.calls(), 1);
}

TEST (AiSupervisorLimits, ThreeAutomaticWriteFailuresPause)
{
    Rig r;
    const auto target = r.state.request.sessionLog.getParentDirectory().getChildFile (AiSupervisor::kStateFileName);
    ASSERT_TRUE (target.createDirectory());
    ASSERT_TRUE (target.getChildFile ("keep").replaceWithText ("x"));
    for (int i = 0; i < 3; ++i)
    {
        for (int k = 0; k < 3; ++k)
            r.sup->notePlacement (0, 0, 1000.0, r.ms);
        r.step (kSec);
    }
    EXPECT_EQ (r.events ("ai_call").size(), 3u);
    EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::Paused);
}

//==============================================================================
// Consent withdrawn / key deleted.

TEST (AiSupervisorConsent, WithdrawingStopsTheCallInFlightAndDropsPendingTriggers)
{
    Rig r;
    r.startManual();
    ASSERT_EQ (r.calls(), 1);
    EXPECT_TRUE (r.launcher->busy);
    const int cancelsBefore = r.launcher->cancels;

    r.sup->requestManualCall();                       // a trigger waiting behind the call in flight
    for (int k = 0; k < 3; ++k)
        r.sup->notePlacement (0, 0, 1000.0, r.ms);    // and a recurring one
    r.logs.clear();

    // What MainComponent does on withdraw: abort + the next readState says "not configured".
    r.sup->abortAndDropPending();
    r.state.configured = false;

    EXPECT_GT (r.launcher->cancels, cancelsBefore) << "the child is cancelled";
    ASSERT_TRUE (r.hasCallStatus ("cancelled"));

    // The late result of the cancelled call is stale: nothing is applied or shown.
    r.launcher->deliver (okResult (output (r.sup->generation() - 1, deepen (5, 1000.0, -12.0))));
    EXPECT_TRUE (r.sup->proposals().empty());

    // Even if the key / consent came back, the dropped triggers must not fire.
    r.state.configured = true;
    for (int i = 0; i < 10; ++i)
        r.step (130 * kSec);
    EXPECT_EQ (r.calls(), 1) << "no new call started from a pending trigger";
}

TEST (AiSupervisorConsent, WithdrawingClearsShownProposalsAndLogsEach)
{
    Rig r;
    r.state.slots[0].notches.push_back (notchAt (6, 2000.0, -6.0, -18.0, 1));
    r.startManual();
    r.finishOk (deepen (5, 1000.0, -12.0) + "," + hold (6, 2000.0));
    ASSERT_EQ (r.sup->proposals().size(), 2u);
    r.logs.clear();

    r.sup->abortAndDropPending();
    r.state.configured = false;

    EXPECT_TRUE (r.sup->proposals().empty());
    const auto d = r.events ("ai_decision");
    ASSERT_EQ (d.size(), 2u);
    for (const auto& v : d)
    {
        EXPECT_EQ (v["decision"].toString(), "expired");
        EXPECT_EQ (v["reason"].toString(), "consent_withdrawn");
    }

    r.step (kSec);
    EXPECT_EQ (r.events ("ai_decision").size(), 2u) << "no further decision line on the next tick";
}

//==============================================================================
// Blockers. None drops the pending trigger: it fires once the way is clear.

TEST (AiSupervisorBlockers, NoCallInBypassSoundcheckMeasuringUnconfiguredOrAsking)
{
    struct Case { const char* name; std::function<void (Rig&, bool)> set; };
    const std::vector<Case> cases {
        { "bypass",       [] (Rig& r, bool on) { r.state.engineMode = on ? AiEngineMode::Bypass : AiEngineMode::Auto; } },
        { "soundcheck",   [] (Rig& r, bool on) { r.state.engineMode = on ? AiEngineMode::Soundcheck : AiEngineMode::Auto; } },
        { "measuring",    [] (Rig& r, bool on) { r.state.measuring = on; } },
        { "no consent/key", [] (Rig& r, bool on) { r.state.configured = ! on; } },
        { "ask running",  [] (Rig& r, bool on) { r.asking = on; } },
    };
    for (const auto& c : cases)
    {
        Rig r;
        c.set (r, true);
        r.sup->requestManualCall();
        r.step (5 * kSec);
        EXPECT_EQ (r.calls(), 0) << c.name;
        c.set (r, false);
        r.step (kSec);
        EXPECT_EQ (r.calls(), 1) << c.name << ": deferred, not dropped";
    }
}

TEST (AiSupervisorBlockers, ATriggerDuringAnAskIsDeferredThenFired)
{
    Rig r;
    r.asking = true;
    r.sup->notePlacement (0, 0, 1000.0, r.ms);
    r.sup->notePlacement (0, 0, 1000.0, r.ms);
    r.sup->notePlacement (0, 0, 1000.0, r.ms);
    r.step (10 * kSec);
    EXPECT_EQ (r.calls(), 0);
    r.asking = false;
    r.step (kSec);
    ASSERT_EQ (r.calls(), 1);
    EXPECT_EQ (r.stateFile()["trigger"].toString(), "recurring_bin");
}

TEST (AiSupervisorBlockers, NothingRunsBelowSuggestAndActIsClampedToSuggest)
{
    for (const auto mode : { AiMode::Off, AiMode::Shadow })
    {
        Rig r (mode);
        r.sup->requestManualCall();
        r.sup->notePlacement (0, 0, 1000.0, r.ms);
        r.sup->notePlacement (0, 0, 1000.0, r.ms);
        r.sup->notePlacement (0, 0, 1000.0, r.ms);
        r.step (5 * kSec);
        r.sup->setRequestedMode (AiMode::Suggest);
        r.step (5 * kSec);
        EXPECT_EQ (r.calls(), 0) << "triggers noted while below Suggest are not kept";
    }

    Rig r (AiMode::Act);
    EXPECT_EQ (r.sup->requestedMode(), AiMode::Act);
    EXPECT_EQ (r.sup->effectiveLlmMode(), AiMode::Suggest);
    EXPECT_EQ (r.sup->effectiveNetworkMode(), AiMode::Shadow);
    r.startManual();
    ASSERT_EQ (r.calls(), 1);
    EXPECT_EQ (r.stateFile()["ai_mode"].toString(), "suggest") << "L3a never tells the sidecar it may act";
}

//==============================================================================
// State file.

TEST (AiSupervisorState, FileHasExactlyTheDocumentedShape)
{
    Rig r;
    auto manual = notchAt (7, 2500.0, -12.0, -12.0, 1);
    manual.origin = NotchController::Origin::Preset;
    manual.placedBy = Author::Human;
    manual.lastChangedBy = Author::Llm;
    manual.lane = 1;
    r.state.slots[0].notches.push_back (manual);
    r.state.slots[0].ringRisk = AiRingRisk::Rising;
    r.state.slots[0].hasRingRiskScore = true;
    r.state.slots[0].ringRiskScore = 0.4321f;
    AiStateSlot s1;
    s1.slot = 1;
    s1.notches.push_back (notchAt (5, 4000.0));   // the SAME local nid as slot 0's
    r.state.slots.push_back (s1);
    r.state.engineMode = AiEngineMode::Auto;

    r.startManual();
    ASSERT_EQ (r.calls(), 1);
    const auto doc = r.stateFile();
    ASSERT_TRUE (doc.isObject());

    EXPECT_TRUE (keysOf (doc) == (std::set<juce::String> { "schema", "generation", "ai_mode", "engine_mode", "sample_rate",
                                                           "bin_hz", "trigger", "slots" }));
    EXPECT_TRUE (doc["schema"].isInt());
    EXPECT_EQ ((int) doc["schema"], 2);
    EXPECT_EQ ((juce::int64) doc["generation"], (juce::int64) r.sup->generation());
    EXPECT_EQ (doc["ai_mode"].toString(), "suggest");
    EXPECT_EQ (doc["engine_mode"].toString(), "auto");
    EXPECT_EQ (doc["trigger"].toString(), "manual");
    EXPECT_DOUBLE_EQ ((double) doc["sample_rate"], 48000.0);
    EXPECT_NEAR ((double) doc["bin_hz"], 23.44, 0.01);

    const auto* slots = doc["slots"].getArray();
    ASSERT_NE (slots, nullptr);
    ASSERT_EQ (slots->size(), 2);
    const auto& s0 = (*slots)[0];
    EXPECT_TRUE (keysOf (s0) == (std::set<juce::String> { "slot", "ring_risk", "ring_risk_score", "notches" }));
    EXPECT_EQ ((int) s0["slot"], 0);
    EXPECT_EQ (s0["ring_risk"].toString(), "rising");
    EXPECT_NEAR ((double) s0["ring_risk_score"], 0.43, 0.005);
    EXPECT_TRUE ((*slots)[1]["ring_risk_score"].isVoid()) << "null when there is no valid score";

    const auto* notches = s0["notches"].getArray();
    ASSERT_NE (notches, nullptr);
    ASSERT_EQ (notches->size(), 2);
    const auto& n = (*notches)[0];
    EXPECT_TRUE (keysOf (n) == (std::set<juce::String> { "nid", "lane", "index", "hz", "q", "depth_db", "ceiling_db", "next_rung_db", "origin",
                                                         "placed_by", "last_changed_by", "seen_s", "hold_s", "verdict" }));
    EXPECT_TRUE (n["nid"].isInt() || n["nid"].isInt64());
    EXPECT_EQ ((int) n["nid"], 5);
    EXPECT_DOUBLE_EQ ((double) n["hz"], 1000.0);
    EXPECT_DOUBLE_EQ ((double) n["depth_db"], -6.0);
    EXPECT_DOUBLE_EQ ((double) n["ceiling_db"], -18.0);
    EXPECT_DOUBLE_EQ ((double) n["next_rung_db"], -12.0) << "the one legal deepen target";
    EXPECT_EQ (n["origin"].toString(), "detector");
    EXPECT_EQ (n["placed_by"].toString(), "detector");
    EXPECT_EQ ((int) n["hold_s"], 0);
    EXPECT_EQ (n["verdict"].toString(), "none");
    const auto& m = (*notches)[1];
    EXPECT_TRUE (m["next_rung_db"].isVoid()) << "at its ceiling: null";
    EXPECT_EQ (m["origin"].toString(), "preset");
    EXPECT_EQ (m["placed_by"].toString(), "human");
    EXPECT_EQ (m["last_changed_by"].toString(), "llm");
    EXPECT_EQ ((int) m["lane"], 1);

    // Ids are unique ACROSS slots (the sidecar rejects a repeat).
    const auto& other = (*(*slots)[1]["notches"].getArray())[0];
    EXPECT_EQ ((juce::int64) other["nid"], (juce::int64) ((1 << 24) | 5));
    EXPECT_NE ((juce::int64) other["nid"], (juce::int64) n["nid"]);
}

namespace
{
// Structural JSON equality with numbers compared as numbers (1000 == 1000.0):
// the fixture is hand-written, the writer emits what juce prints.
bool sameJson (const juce::var& a, const juce::var& b, juce::String& why, const juce::String& path = "$")
{
    const auto isNum = [] (const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); };
    if (isNum (a) && isNum (b))
    {
        if (std::abs ((double) a - (double) b) <= 1.0e-9) return true;
        why = path + ": " + a.toString() + " != " + b.toString();
        return false;
    }
    if (a.isVoid() && b.isVoid()) return true;
    if (a.isString() && b.isString()) { if (a.toString() == b.toString()) return true; why = path + ": " + a.toString() + " != " + b.toString(); return false; }
    if (a.isBool() && b.isBool()) { if ((bool) a == (bool) b) return true; why = path + ": bool"; return false; }
    if (const auto* x = a.getArray())
    {
        const auto* y = b.getArray();
        if (y == nullptr || x->size() != y->size()) { why = path + ": array size"; return false; }
        for (int i = 0; i < x->size(); ++i)
            if (! sameJson ((*x)[i], (*y)[i], why, path + "[" + juce::String (i) + "]"))
                return false;
        return true;
    }
    if (const auto* x = a.getDynamicObject())
    {
        const auto* y = b.getDynamicObject();
        if (y == nullptr) { why = path + ": not an object"; return false; }
        if (x->getProperties().size() != y->getProperties().size()) { why = path + ": key set differs"; return false; }
        for (const auto& p : x->getProperties())
        {
            if (! y->hasProperty (p.name)) { why = path + "." + p.name.toString() + " missing"; return false; }
            if (! sameJson (p.value, y->getProperty (p.name), why, path + "." + p.name.toString()))
                return false;
        }
        return true;
    }
    why = path + ": type differs";
    return false;
}
} // namespace

// The state file as the Python agent's fixture documents it (schema 2:
// seen_s, next_rung_db). Equality is on PARSED trees, not text.
TEST (AiSupervisorState, ProductionWriterReproducesTheSchemaTwoFixture)
{
    const auto fixtureFile = juce::File (__FILE__).getParentDirectory().getChildFile ("fixtures/ai-state-schema2.json");
    ASSERT_TRUE (fixtureFile.existsAsFile());
    const auto fixture = juce::JSON::parse (fixtureFile.loadFileAsString());
    ASSERT_TRUE (fixture.isObject());

    const auto at = [] (std::uint32_t nid, int lane, int index, double hz, double depth, double ceiling)
    {
        AiStateNotch n;
        n.nid = nid; n.lane = lane; n.index = index; n.hz = (float) hz; n.q = 30.0f;
        n.depthDb = (float) depth; n.ceilingDb = (float) ceiling; n.seenS = 42.0;
        return n;
    };
    AiStateSnapshot st;
    st.sampleRate = 48000.0;
    st.binHz = 48000.0 / 2048.0;
    AiStateSlot s0;
    s0.slot = 0; s0.ringRisk = AiRingRisk::Low; s0.hasRingRiskScore = true; s0.ringRiskScore = 0.2f;
    s0.notches = { at (1, 0, 0, 1000.0, -6.0, -18.0), at (2, 0, 1, 2000.0, -18.0, -18.0), at (3, 1, 0, 500.0, -9.5, -24.0) };
    AiStateSlot s1;
    s1.slot = 1; s1.ringRisk = AiRingRisk::Rising; s1.hasRingRiskScore = true; s1.ringRiskScore = 0.65f;
    s1.notches = { at (1, 0, 0, 3000.0, -12.0, -24.0) };
    st.slots = { s0, s1 };

    const auto text = AiSupervisor::buildStateJson (st, 7, AiMode::Suggest, AiTrigger::RecurringBin);
    const auto produced = juce::JSON::parse (text);
    juce::String why;
    EXPECT_TRUE (sameJson (produced, fixture, why)) << why;
    EXPECT_TRUE (sameJson (fixture, produced, why)) << why;
}

TEST (AiSupervisorState, AnOffGridCeilingAdvertisesATargetBothChecksAccept)
{
    // depth -12, ceiling -13.3333: the only legal target is the ceiling itself.
    // depth, ceiling and next_rung are all rounded to 3 decimals, and the rung
    // is computed from the rounded numbers, so next_rung_db >= ceiling_db holds
    // exactly in the file (the sidecar compares against the file's ceiling).
    Rig r;
    r.state.slots[0].notches = { notchAt (5, 1000.0, -12.0, -13.3333) };
    const auto doc = juce::JSON::parse (AiSupervisor::buildStateJson (r.state, 1, AiMode::Suggest, AiTrigger::Manual));
    const auto n = (*doc["slots"].getArray())[0]["notches"][0];
    EXPECT_DOUBLE_EQ ((double) n["ceiling_db"], -13.333);
    ASSERT_FALSE (n["next_rung_db"].isVoid());
    EXPECT_GE ((double) n["next_rung_db"], (double) n["ceiling_db"]);
    EXPECT_LT ((double) n["next_rung_db"], (double) n["depth_db"]);
    EXPECT_DOUBLE_EQ ((double) n["next_rung_db"], -13.333);
}

TEST (AiSupervisorState, NonFiniteOrOutOfRangeValuesNeverReachTheFile)
{
    Rig r;
    auto& ns = r.state.slots[0].notches;
    auto bad = [] (std::uint32_t nid) { return notchAt (nid, 1500.0, -6.0, -18.0, (int) nid); };
    auto a = bad (10); a.hz = std::numeric_limits<float>::quiet_NaN();                ns.push_back (a);
    auto b = bad (11); b.q = std::numeric_limits<float>::infinity();                   ns.push_back (b);
    auto c = bad (12); c.hz = 30000.0f;                                                ns.push_back (c);   // above Nyquist
    auto d = bad (13); d.depthDb = -500.0f;                                            ns.push_back (d);   // clamped, kept
    auto e = bad (14); e.seenS = std::numeric_limits<double>::quiet_NaN();             ns.push_back (e);
    auto f = bad (0);                                                                  ns.push_back (f);   // nid 0 = none
    r.state.slots[0].hasRingRiskScore = true;
    r.state.slots[0].ringRiskScore = std::numeric_limits<float>::infinity();

    r.startManual();
    ASSERT_EQ (r.calls(), 1);
    const auto path = r.launcher->requests[0].statePath;
    const auto text = path.loadFileAsString();
    const auto lower = text.toLowerCase();
    EXPECT_FALSE (lower.contains ("nan"));
    EXPECT_FALSE (lower.contains ("inf"));

    const auto doc = juce::JSON::parse (text);
    const auto* notches = (*doc["slots"].getArray())[0]["notches"].getArray();
    ASSERT_NE (notches, nullptr);
    std::set<int> nids;
    for (const auto& n : *notches)
    {
        nids.insert ((int) n["nid"]);
        EXPECT_GE ((double) n["depth_db"], -120.0);
        EXPECT_LE ((double) n["depth_db"], 0.0);
        EXPECT_LE ((double) n["hz"], 24000.0);
    }
    EXPECT_EQ (nids, (std::set<int> { 5, 13 }));
    EXPECT_TRUE ((*doc["slots"].getArray())[0]["ring_risk_score"].isVoid());
}

TEST (AiSupervisorState, CapsAtSixteenSlotsAndThirtyTwoNotchesAndRepeatsAreDropped)
{
    Rig r;
    r.state.slots.clear();
    for (int sl = 0; sl < 20; ++sl)
    {
        AiStateSlot s;
        s.slot = sl % 18;   // 16 and 17 are out of range; 0 and 1 repeat at 18, 19
        for (std::uint32_t i = 1; i <= 40; ++i)
            s.notches.push_back (notchAt (i, 200.0 + 10.0 * i, -6.0, -18.0, (int) (i % 60)));
        r.state.slots.push_back (s);
    }
    r.startManual();
    ASSERT_EQ (r.calls(), 1);
    const auto doc = r.stateFile();
    const auto* slots = doc["slots"].getArray();
    ASSERT_NE (slots, nullptr);
    EXPECT_LE (slots->size(), 16);
    std::set<juce::int64> nids;
    std::set<int> slotIds;
    for (const auto& s : *slots)
    {
        EXPECT_TRUE (slotIds.insert ((int) s["slot"]).second) << "a slot index repeats";
        EXPECT_LE (s["notches"].getArray()->size(), 32);
        for (const auto& n : *s["notches"].getArray())
            EXPECT_TRUE (nids.insert ((juce::int64) n["nid"]).second) << "a nid repeats";
    }
}

TEST (AiSupervisorState, TheFileIsReplacedAtomicallyAndLeavesNoTemporary)
{
    Rig r;
    r.startManual();
    ASSERT_EQ (r.calls(), 1);
    const auto path = r.launcher->requests[0].statePath;
    EXPECT_EQ (path.getFileName(), "supervisor-state.json");
    EXPECT_EQ (path.getParentDirectory(), r.state.request.sessionLog.getParentDirectory());
    r.finishOk();
    r.sup->requestManualCall();
    r.step (121 * kSec);
    ASSERT_EQ (r.calls(), 2);
    EXPECT_TRUE (juce::JSON::parse (path).isObject());
    int files = 0;
    for (const auto& f : juce::RangedDirectoryIterator (r.dir, false))
        { (void) f; ++files; }
    EXPECT_EQ (files, 2) << "session.jsonl and supervisor-state.json only";

    const auto& req = r.launcher->requests[0];
    EXPECT_TRUE (req.supervise);
    EXPECT_EQ (req.timeoutMs, AiSupervisor::kWatchdogMs);
    EXPECT_EQ (req.timeoutMs, 105000);
}

//==============================================================================
// Results.

TEST (AiSupervisorResult, ProposalsAreCollectedLoggedAndNeverApplied)
{
    Rig r;
    r.state.slots[0].notches.push_back (notchAt (6, 2000.0, -6.0, -18.0, 1));
    r.state.slots[0].notches.push_back (notchAt (7, 3000.0, -6.0, -18.0, 2));
    r.state.slots[0].ringRisk = AiRingRisk::Low;   // a release is only shown beside a quiet slot
    r.startManual();
    r.finishOk (deepen (5, 1000.0, -12.0) + "," + hold (6, 2000.0) + "," + release (7, 3000.0) + ","
                + "{\"tool\": \"no_action\", \"reason\": \"nothing_to_do\"}");
    const auto& p = r.sup->proposals();
    ASSERT_EQ (p.size(), 3u) << "no_action produces no proposal";
    EXPECT_EQ (p[0].proposal.tool, AiProposal::Tool::Deepen);
    EXPECT_EQ (p[0].slot, 0);
    EXPECT_EQ (p[0].index, 0);
    EXPECT_NE (p[0].id, p[1].id);
    EXPECT_EQ (p[0].generation, r.sup->generation());
    EXPECT_FLOAT_EQ (p[0].hz, 1000.0f);

    EXPECT_EQ (r.events ("ai_proposal").size(), 4u) << "three proposals and the no_action line";
    ASSERT_EQ (r.events ("ai_call").size(), 1u);
    EXPECT_EQ (r.events ("ai_call")[0]["status"].toString(), "ok");
    EXPECT_EQ (r.events ("ai_call")[0]["trigger"].toString(), "manual");
    EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::Idle);
}

TEST (AiSupervisorResult, RefusedProposalsAreLoggedAndNotShown)
{
    Rig r;
    r.startManual();
    // The notch moves while the call is out: validation reads the CURRENT notches.
    r.state.slots[0].notches[0].hz = 3000.0f;
    r.finishOk (deepen (5, 1000.0, -12.0) + "," + deepen (99, 1000.0, -12.0));
    EXPECT_TRUE (r.sup->proposals().empty());
    const auto d = r.events ("ai_decision");
    ASSERT_EQ (d.size(), 2u);
    EXPECT_EQ (d[0]["decision"].toString(), "refused");
    EXPECT_EQ ((int) d[0]["slot"], 0) << "log lines carry slot + LOCAL nid";
    EXPECT_EQ ((int) d[0]["nid"], 5);
    EXPECT_EQ (d[0]["refusal"].toString(), "frequency_mismatch");
    EXPECT_EQ (d[1]["refusal"].toString(), "unknown_nid");

    Rig r2;
    r2.startManual();
    r2.finishOk (deepen (5, 1000.0, -18.0));   // two rungs, not one
    EXPECT_TRUE (r2.sup->proposals().empty());
    EXPECT_EQ (r2.events ("ai_decision")[0]["refusal"].toString(), "more_than_one_rung");
}

TEST (AiSupervisorResult, ANewResultForTheSameToolAndNotchReplacesTheOldProposal)
{
    Rig r;
    r.startManual();
    r.finishOk (deepen (5, 1000.0, -12.0));
    ASSERT_EQ (r.sup->proposals().size(), 1u);
    const auto firstId = r.sup->proposals()[0].id;
    r.sup->requestManualCall();
    r.step (121 * kSec);
    r.finishOk (deepen (5, 1000.0, -12.0) + "," + hold (5, 1000.0));
    ASSERT_EQ (r.sup->proposals().size(), 2u) << "deepen replaced, hold added";
    for (const auto& p : r.sup->proposals())
        EXPECT_NE (p.id, firstId);

    // The replaced proposal is not lost from the log: expired, reason superseded.
    bool sawSuperseded = false;
    for (const auto& d : r.events ("ai_decision"))
        sawSuperseded = sawSuperseded || (d["decision"].toString() == "expired" && d["reason"].toString() == "superseded"
                                          && d["tool"].toString() == "propose_deepen");
    EXPECT_TRUE (sawSuperseded);
}

TEST (AiSupervisorResult, FailuresSetLastFailedAndChangeNothingElse)
{
    struct Case { const char* name; CopilotRunResult result; const char* status; };
    CopilotRunResult missing; missing.status = CopilotRunResult::Status::sidecarMissing;
    CopilotRunResult timedOut; timedOut.status = CopilotRunResult::Status::timedOut;
    CopilotRunResult killed; killed.status = CopilotRunResult::Status::failed; killed.exitCode = -1;
    CopilotRunResult exit1; exit1.status = CopilotRunResult::Status::ok; exit1.exitCode = 1; exit1.stdoutText = "{}";
    const auto badJson = okResult ("{not json");
    const auto wrongKind = okResult ("{\"supervise\": false, \"generation\": 1, \"actions\": []}");
    for (const auto& c : { Case { "missing", missing, "failed" }, Case { "timeout", timedOut, "timeout" },
                           Case { "killed", killed, "failed" }, Case { "exit 1", exit1, "failed" },
                           Case { "bad json", badJson, "failed" }, Case { "not a supervise reply", wrongKind, "failed" } })
    {
        Rig r;
        r.startManual();
        r.launcher->deliver (c.result);
        EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::LastFailed) << c.name;
        EXPECT_TRUE (r.sup->proposals().empty()) << c.name;
        EXPECT_TRUE (r.hasCallStatus (c.status)) << c.name;
        EXPECT_TRUE (r.events ("ai_proposal").empty()) << c.name;
        EXPECT_TRUE (r.events ("ai_decision").empty()) << c.name;

        // A later good call clears the failure.
        r.sup->requestManualCall();
        r.step (121 * kSec);
        r.finishOk();
        EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::Idle) << c.name;
    }
}

//==============================================================================
// Generations.

TEST (AiSupervisorGeneration, EveryInvalidatingEventDropsAnInFlightResultAsStale)
{
    const std::vector<std::pair<const char*, std::function<void (Rig&)>>> events {
        { "invalidate (device restart / preset load / CLEAR ALL / engine mode)", [] (Rig& r) { r.sup->invalidate(); } },
        { "AI mode change", [] (Rig& r) { r.sup->setRequestedMode (AiMode::Off); } },
        { "AI mode change to Act", [] (Rig& r) { r.sup->setRequestedMode (AiMode::Act); } },
        { "shutdown", [] (Rig& r) { r.sup->shutdown(); } },
    };
    for (const auto& [name, fire] : events)
    {
        Rig r;
        const auto gen = r.startManual();
        ASSERT_EQ (r.calls(), 1) << name;
        const auto cancelsBefore = r.launcher->cancels;
        fire (r);
        EXPECT_GT (r.launcher->cancels, cancelsBefore) << name << ": the launcher was told to cancel";
        EXPECT_NE (r.sup->generation(), gen) << name;
        EXPECT_TRUE (r.hasCallStatus ("cancelled")) << name;

        r.launcher->deliver (okResult (output (gen, deepen (5, 1000.0, -12.0))));   // arrives late anyway
        EXPECT_TRUE (r.sup->proposals().empty()) << name;
        EXPECT_TRUE (r.hasCallStatus ("stale")) << name;
        EXPECT_TRUE (r.events ("ai_proposal").empty()) << name;
    }
}

TEST (AiSupervisorGeneration, AModeChangeIsLoggedWithTheEffectiveModes)
{
    Rig r;
    r.sup->setRequestedMode (AiMode::Act);
    const auto e = r.events ("ai_mode");
    ASSERT_EQ (e.size(), 1u);
    EXPECT_EQ (e[0]["mode"].toString(), "act");
    EXPECT_EQ (e[0]["effective_llm"].toString(), "suggest");
    EXPECT_EQ (e[0]["effective_network"].toString(), "shadow");
    r.sup->setRequestedMode (AiMode::Act);
    EXPECT_EQ (r.events ("ai_mode").size(), 1u) << "no change, no event";
}

TEST (AiSupervisorGeneration, AfterInvalidationANewCallCanStart)
{
    Rig r;
    r.startManual();
    r.sup->invalidate();
    r.sup->requestManualCall();
    r.step (121 * kSec);
    EXPECT_EQ (r.calls(), 2);
}

//==============================================================================
// The proposal book.

TEST (AiSupervisorBook, ProposalsExpireWhenTheirNotchGoesMovesOrAges)
{
    {
        Rig r;
        r.startManual();
        r.finishOk (deepen (5, 1000.0, -12.0));
        ASSERT_EQ (r.sup->proposals().size(), 1u);
        r.state.slots[0].notches.clear();
        r.step (kSec);
        EXPECT_TRUE (r.sup->proposals().empty());
        EXPECT_EQ (r.events ("ai_decision").back()["decision"].toString(), "expired");
    }
    {
        Rig r;
        r.startManual();
        r.finishOk (deepen (5, 1000.0, -12.0));
        r.state.slots[0].notches[0].hz = 1010.0f;   // inside one bin: stays
        r.step (kSec);
        EXPECT_EQ (r.sup->proposals().size(), 1u);
        r.state.slots[0].notches[0].hz = 1100.0f;   // more than a bin: gone
        r.step (kSec);
        EXPECT_TRUE (r.sup->proposals().empty());
    }
    {
        Rig r;
        r.startManual();
        r.finishOk (deepen (5, 1000.0, -12.0));
        r.step (9 * 60 * kSec);
        EXPECT_EQ (r.sup->proposals().size(), 1u);
        r.step (61 * kSec);
        EXPECT_TRUE (r.sup->proposals().empty());
        EXPECT_EQ (r.events ("ai_decision").back()["decision"].toString(), "expired");
    }
}

TEST (AiSupervisorBook, ReleaseIsRefusedUnlessTheSlotRingRiskIsLow)
{
    for (const auto level : { AiRingRisk::Na, AiRingRisk::Rising, AiRingRisk::Critical })
    {
        Rig r;
        r.state.slots[0].ringRisk = level;
        r.startManual();
        r.finishOk (release (5, 1000.0));
        EXPECT_TRUE (r.sup->proposals().empty()) << (int) level;
        const auto d = r.events ("ai_decision");
        ASSERT_EQ (d.size(), 1u) << (int) level;
        EXPECT_EQ (d[0]["decision"].toString(), "refused");
        EXPECT_EQ (d[0]["refusal"].toString(), "ring_risk_not_low");
    }
    Rig ok;
    ok.state.slots[0].ringRisk = AiRingRisk::Low;
    ok.startManual();
    ok.finishOk (release (5, 1000.0));
    EXPECT_EQ (ok.sup->proposals().size(), 1u);
}

TEST (AiSupervisorBook, AShownReleaseGoesWhenTheSlotStartsToRingAndIsLogged)
{
    Rig r;
    r.state.slots[0].ringRisk = AiRingRisk::Low;
    r.startManual();
    r.finishOk (release (5, 1000.0) + "," + hold (5, 1000.0));
    ASSERT_EQ (r.sup->proposals().size(), 2u);

    r.state.slots[0].ringRisk = AiRingRisk::Rising;
    r.step (kSec);
    ASSERT_EQ (r.sup->proposals().size(), 1u) << "only the hold stays";
    EXPECT_EQ (r.sup->proposals()[0].proposal.tool, AiProposal::Tool::Hold);
    const auto d = r.events ("ai_decision");
    ASSERT_EQ (d.size(), 1u);
    EXPECT_EQ (d[0]["decision"].toString(), "expired");
    EXPECT_EQ (d[0]["tool"].toString(), "propose_release");
    EXPECT_EQ (d[0]["reason"].toString(), "ring_risk_not_low");
}

TEST (AiSupervisorBook, AShownReleaseSurvivesABypassTapAndReturnToAuto)
{
    Rig r;
    r.state.slots[0].ringRisk = AiRingRisk::Low;
    r.startManual();
    r.finishOk (release (5, 1000.0));
    ASSERT_EQ (r.sup->proposals().size(), 1u);

    // BYPASS: detection stops, ring risk reads Na. Past the proposal lifetime
    // too: nothing supervises, so nothing is re-validated and nothing expires.
    r.state.engineMode = AiEngineMode::Bypass;
    r.state.slots[0].ringRisk = AiRingRisk::Na;
    r.step (kSec);
    r.step (AiSupervisor::kProposalLifetimeMs + 60 * kSec);
    EXPECT_EQ (r.sup->proposals().size(), 1u) << "bypass must not wipe a shown release";
    EXPECT_TRUE (r.events ("ai_decision").empty());

    // Back to AUTO with a quiet slot: still there, and the frozen time is given back.
    r.state.engineMode = AiEngineMode::Auto;
    r.state.slots[0].ringRisk = AiRingRisk::Low;
    r.step (kSec);
    EXPECT_EQ (r.sup->proposals().size(), 1u);
    EXPECT_TRUE (r.events ("ai_decision").empty());

    // A sustained Critical still expires it, with the reason, exactly once.
    r.state.slots[0].ringRisk = AiRingRisk::Critical;
    r.step (kSec);
    r.step (kSec);
    r.step (kSec);
    EXPECT_TRUE (r.sup->proposals().empty());
    const auto d = r.events ("ai_decision");
    ASSERT_EQ (d.size(), 1u);
    EXPECT_EQ (d[0]["reason"].toString(), "ring_risk_not_low");
}

TEST (AiSupervisorBook, AResultDeliveredMidFreezeIsNotDatedInTheFutureAfterUnfreeze)
{
    Rig r;
    r.startManual();                                   // call in flight
    r.state.engineMode = AiEngineMode::Bypass;         // freeze starts
    r.step (kSec);
    r.step (60 * kSec);
    r.finishOk (deepen (5, 1000.0, -12.0));            // arrives DURING the freeze
    ASSERT_EQ (r.sup->proposals().size(), 1u);
    r.step (60 * kSec);

    r.state.engineMode = AiEngineMode::Auto;           // unfreeze
    r.step (kSec);
    ASSERT_EQ (r.sup->proposals().size(), 1u);
    EXPECT_LE (r.sup->proposals()[0].receivedMs, r.ms) << "never dated in the future";

    // Normal lifetime counted from the unfreeze, not later.
    r.step (AiSupervisor::kProposalLifetimeMs - 2 * kSec);
    EXPECT_EQ (r.sup->proposals().size(), 1u);
    r.step (3 * kSec);
    EXPECT_TRUE (r.sup->proposals().empty());
}

TEST (AiSupervisorBook, AShownDeepenGoesWhenTheDetectorDeepensTheNotch)
{
    Rig r;
    r.startManual();
    r.finishOk (deepen (5, 1000.0, -12.0));
    ASSERT_EQ (r.sup->proposals().size(), 1u);

    r.state.slots[0].notches[0].depthDb = -12.0f;   // the detector got there first
    r.step (kSec);
    EXPECT_TRUE (r.sup->proposals().empty());
    const auto d = r.events ("ai_decision");
    ASSERT_EQ (d.size(), 1u);
    EXPECT_EQ (d[0]["decision"].toString(), "expired");
    EXPECT_EQ (d[0]["reason"].toString(), "not_deeper");
}

TEST (AiSupervisorBook, DismissAndDismissAllRemoveAndLog)
{
    Rig r;
    r.state.slots[0].notches.push_back (notchAt (6, 2000.0, -6.0, -18.0, 1));
    r.startManual();
    r.finishOk (deepen (5, 1000.0, -12.0) + "," + hold (6, 2000.0));
    ASSERT_EQ (r.sup->proposals().size(), 2u);
    const auto id0 = r.sup->proposals()[0].id;
    const auto before = r.changedCount;
    r.sup->dismiss (id0);
    ASSERT_EQ (r.sup->proposals().size(), 1u);
    EXPECT_NE (r.sup->proposals()[0].id, id0);
    EXPECT_GT (r.changedCount, before);
    EXPECT_EQ (r.events ("ai_decision").back()["decision"].toString(), "dismissed");
    r.sup->dismiss (id0);   // unknown now: a no-op
    EXPECT_EQ (r.sup->proposals().size(), 1u);
    r.sup->dismissAll();
    EXPECT_TRUE (r.sup->proposals().empty());
    EXPECT_EQ (r.events ("ai_decision").size(), 2u);
}

TEST (AiSupervisorBook, DroppingBelowSuggestClearsTheBook)
{
    Rig r;
    r.startManual();
    r.finishOk (deepen (5, 1000.0, -12.0));
    ASSERT_EQ (r.sup->proposals().size(), 1u);
    r.sup->setRequestedMode (AiMode::Shadow);
    EXPECT_TRUE (r.sup->proposals().empty());
}

TEST (AiSupervisorBook, TestSeamStagesProposalsAndNotifies)
{
    Rig r;
    ShownProposal p;
    p.id = 77; p.slot = 2; p.proposal.tool = AiProposal::Tool::Hold; p.proposal.nid = 3;
    const auto before = r.changedCount;
    r.sup->setAiProposalsForTest ({ p });
    ASSERT_EQ (r.sup->proposals().size(), 1u);
    EXPECT_EQ (r.sup->proposals()[0].id, 77u);
    EXPECT_GT (r.changedCount, before);
}

//==============================================================================
// Status.

TEST (AiSupervisorStatus, OneFixedLinePerStatus)
{
    Rig r;
    EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::Idle);
    const auto idle = r.sup->statusText();

    r.startManual();
    r.step (23 * kSec);
    auto s = r.sup->status();
    EXPECT_EQ (s.kind, AiSupervisor::StatusKind::Pending);
    EXPECT_EQ (s.seconds, 23);
    EXPECT_TRUE (r.sup->statusText().contains ("23 s"));
    EXPECT_TRUE (r.sup->statusText().startsWith ("AI"));

    CopilotRunResult bad; bad.status = CopilotRunResult::Status::sidecarMissing;
    r.launcher->deliver (bad);
    EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::LastFailed);
    const auto failed = r.sup->statusText();

    r.sup->setRequestedMode (AiMode::Shadow);
    EXPECT_EQ (r.sup->status().kind, AiSupervisor::StatusKind::Disabled);
    const auto disabled = r.sup->statusText();

    Rig u;
    u.state.configured = false;
    u.step (kSec);
    EXPECT_EQ (u.sup->status().kind, AiSupervisor::StatusKind::NotConfigured);
    const auto notConfigured = u.sup->statusText();

    std::set<juce::String> distinct { idle, failed, disabled, notConfigured };
    EXPECT_EQ (distinct.size(), 4u);
    for (const auto& t : distinct)
        EXPECT_TRUE (t.isNotEmpty());
}

//==============================================================================
// Phase L3a changes the audio by exactly 0 dB -- against a REAL controller.

namespace
{
class TickClock : public ClockSource
{
public:
    double nowMs() const override { return ms_; }
    void advance (double m) { ms_ += m; }
private:
    double ms_ = 1000.0;
};

struct CtrlRig
{
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 512 };
    TickClock clock;
    NotchController controller { tap, commands, clock, 0 };
};

constexpr double kSr = 48000.0;
constexpr double kBlockMs = Detector::kHopSize / kSr * 1000.0;

std::vector<NotchCommand> drain (CtrlRig& c)
{
    std::vector<NotchCommand> out;
    NotchCommand cmd {};
    while (c.commands.read (&cmd, 1) == 1)
        out.push_back (cmd);
    return out;
}

bool sameCommandList (const std::vector<NotchCommand>& a, const std::vector<NotchCommand>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].type != b[i].type || a[i].channel != b[i].channel || a[i].index != b[i].index
            || a[i].frequency != b[i].frequency || a[i].Q != b[i].Q || a[i].depthDB != b[i].depthDB
            || a[i].slot != b[i].slot)
            return false;
    return true;
}

// The state a supervisor would read, built from the controller's published snapshot.
AiStateSnapshot stateOf (const CtrlRig& c)
{
    AiStateSnapshot st;
    NotchController::SnapshotBuffer snap {};
    c.controller.copySnapshot (snap);
    st.configured = true;
    st.sampleRate = kSr;
    st.binHz = kSr / 2048.0;
    AiStateSlot s;
    s.slot = 0;
    s.ringRisk = AiRingRisk::Low;   // a quiet slot: a release is allowed to be shown
    for (std::uint32_t i = 0; i < snap.notchCount; ++i)
    {
        const auto& n = snap.notches[i];
        if (n.nid == 0)
            continue;
        AiStateNotch a;
        a.nid = n.nid; a.lane = n.channel; a.index = n.index; a.hz = n.frequency; a.q = n.Q;
        a.depthDb = n.depthDB; a.origin = n.origin;
        a.ceilingDb = n.origin == NotchController::Origin::Detector ? (float) c.controller.getNotchDepthDb() : n.deepestDb;
        a.placedBy = n.placedBy; a.lastChangedBy = n.lastChangedBy;
        s.notches.push_back (a);
    }
    st.slots.push_back (s);
    return st;
}

std::vector<float> noiseHop (std::mt19937& rng)
{
    std::uniform_real_distribution<float> dist { -1.0f, 1.0f };
    std::vector<float> out ((std::size_t) Detector::kHopSize);
    for (auto& v : out) v = 0.01f * dist (rng);
    return out;
}

std::vector<float> toneHop (double freq, double& n)
{
    std::vector<float> out ((std::size_t) Detector::kHopSize);
    for (auto& v : out)
    {
        v = (float) std::sin (2.0 * 3.14159265358979323846 * freq * n / kSr);
        n += 1.0;
    }
    return out;
}

enum class Failure { None, Missing, Timeout, BadJson, Killed };

// One fixed block sequence through a real controller. With a supervisor, a call
// is requested at the start and finishes with `failure` once the tone is on.
std::vector<NotchCommand> runScenario (bool withSupervisor, Failure failure, bool* calledOut = nullptr)
{
    CtrlRig c;
    c.controller.setDetectionActive (true);

    std::unique_ptr<Rig> rig;
    CopilotRequest requestTemplate;
    if (withSupervisor)
    {
        rig = std::make_unique<Rig>();
        requestTemplate = rig->state.request;
        rig->sup->requestManualCall();
    }

    std::mt19937 rng { 777u };
    double phase = 0.0;
    auto pump = [&] (const std::vector<float>& hop)
    {
        c.tap.write (hop.data(), hop.size());
        c.clock.advance (kBlockMs);
        c.controller.runOnce();
        if (rig != nullptr)
        {
            rig->ms += (std::int64_t) std::llround (kBlockMs);
            rig->state = stateOf (c);
            rig->state.request = requestTemplate;
            rig->sup->tick();
        }
    };

    for (int i = 0; i < 64; ++i) pump (noiseHop (rng));
    for (int i = 0; i < 80; ++i) pump (toneHop (3100.0, phase));
    for (int i = 0; i < 60; ++i) pump (noiseHop (rng));

    if (rig != nullptr)
    {
        if (calledOut) *calledOut = rig->calls() == 1;
        CopilotRunResult r;
        switch (failure)
        {
            case Failure::Missing: r.status = CopilotRunResult::Status::sidecarMissing; break;
            case Failure::Timeout: r.status = CopilotRunResult::Status::timedOut; break;
            case Failure::BadJson: r = okResult ("{not json"); break;
            case Failure::Killed:  r.status = CopilotRunResult::Status::failed; r.exitCode = -1; break;
            case Failure::None:    r = okResult (output (rig->sup->generation(), {})); break;
        }
        if (rig->launcher->busy)
            rig->launcher->deliver (r);
        for (int i = 0; i < 20; ++i) pump (noiseHop (rng));
    }
    return drain (c);
}
} // namespace

TEST (AiMode, SuggestEmitsNoCommand)
{
    CtrlRig c;
    // Three detector notches, as the detector would have left them.
    ASSERT_TRUE (c.controller.setNotch (0, 0, 1000.0, 30.0, -6.0, NotchController::Origin::Detector));
    ASSERT_TRUE (c.controller.setNotch (0, 1, 2000.0, 30.0, -6.0, NotchController::Origin::Detector));
    ASSERT_TRUE (c.controller.setNotch (0, 2, 3000.0, 30.0, -6.0, NotchController::Origin::Detector));
    std::mt19937 rng { 5u };
    {
        const auto hop = noiseHop (rng);
        c.tap.write (hop.data(), hop.size());
        c.clock.advance (kBlockMs);
        c.controller.runOnce();   // publishes the snapshot
    }
    (void) drain (c);             // the setup commands

    const auto st = stateOf (c);
    ASSERT_EQ (st.slots[0].notches.size(), 3u);
    const auto nid = [&] (std::size_t i) { return (int) st.slots[0].notches[i].nid; };
    const double ceiling = st.slots[0].notches[0].ceilingDb;
    ASSERT_LE (ceiling, -12.0) << "teeth: a -12 rung must exist below -6";

    Rig r;
    const auto requestTemplate = r.state.request;
    r.state = st;
    r.state.request = requestTemplate;
    r.startManual();
    ASSERT_EQ (r.calls(), 1);

    const auto modelBefore = stateOf (c);
    const auto depth0 = c.controller.depthDbForTest (0, 0);
    r.finishOk (deepen (nid (0), st.slots[0].notches[0].hz, -12.0) + "," + hold (nid (1), st.slots[0].notches[1].hz)
                + "," + release (nid (2), st.slots[0].notches[2].hz));

    ASSERT_EQ (r.sup->proposals().size(), 3u) << "teeth: three valid proposals were accepted";
    // 0 dB: no command was queued, and the model is byte-for-byte what it was.
    EXPECT_TRUE (drain (c).empty());
    const auto modelAfter = stateOf (c);
    ASSERT_EQ (modelAfter.slots[0].notches.size(), modelBefore.slots[0].notches.size());
    for (std::size_t i = 0; i < modelBefore.slots[0].notches.size(); ++i)
    {
        const auto& a = modelBefore.slots[0].notches[i];
        const auto& b = modelAfter.slots[0].notches[i];
        EXPECT_EQ (a.nid, b.nid);
        EXPECT_EQ (a.hz, b.hz);
        EXPECT_EQ (a.q, b.q);
        EXPECT_EQ (a.depthDb, b.depthDb);
        EXPECT_EQ (a.lastChangedBy, b.lastChangedBy);
    }
    EXPECT_EQ (c.controller.depthDbForTest (0, 0), depth0);
    EXPECT_TRUE (c.controller.activeForTest (0, 0));
    EXPECT_TRUE (c.controller.activeForTest (0, 1));
    EXPECT_TRUE (c.controller.activeForTest (0, 2));

    // Dismissing changes nothing either.
    r.sup->dismissAll();
    EXPECT_TRUE (drain (c).empty());
    EXPECT_EQ (c.controller.depthDbForTest (0, 0), depth0);
}

TEST (AiMode, FailureLeavesDetectorUntouched)
{
    const auto baseline = runScenario (false, Failure::None);
    ASSERT_GE (baseline.size(), 1u) << "teeth: the scenario must place a notch";

    for (const auto f : { Failure::Missing, Failure::Timeout, Failure::BadJson, Failure::Killed })
    {
        bool called = false;
        const auto withSup = runScenario (true, f, &called);
        EXPECT_TRUE (called) << "teeth: the supervisor really made its call, failure " << (int) f;
        EXPECT_TRUE (sameCommandList (baseline, withSup)) << "failure " << (int) f;
    }
    // And a SUCCESSFUL call with no actions is just as silent.
    bool called = false;
    EXPECT_TRUE (sameCommandList (baseline, runScenario (true, Failure::None, &called)));
    EXPECT_TRUE (called);
}

TEST (AiMode, OffDisablesShadowScoring)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;

    CandidateClassifier::RawInput in;
    in.hz = 1000.0f; in.peakiness = 12.0f; in.pNorm = 0.5f; in.rNorm = 0.5f; in.riseRatio = 2.0f;
    in.mNorm = 0.5f; in.penalty = 1.0f;
    in.nearMags.fill (0.1f);

    auto* c0 = mc.getNotchControllerForTest (0);
    ASSERT_NE (c0, nullptr);
    EXPECT_TRUE (c0->shadowScoringEnabled()) << "today's behaviour is Shadow: score and log";
    EXPECT_EQ (mc.aiMode(), AiMode::Shadow);

    mc.setAiMode (AiMode::Off);
    EXPECT_EQ (mc.appSettingsForTest().aiMode, AiMode::Off);
    EXPECT_EQ (mc.aiSupervisor().requestedMode(), AiMode::Off);
    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_FALSE (mc.getNotchControllerForTest (s)->shadowScoringEnabled()) << "slot " << s;
    EXPECT_FALSE (c0->shadowScoreForTest (in).has_value()) << "no classifier call at all while Off";

    mc.setAiMode (AiMode::Suggest);
    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_TRUE (mc.getNotchControllerForTest (s)->shadowScoringEnabled()) << "slot " << s;
    EXPECT_TRUE (c0->shadowScoreForTest (in).has_value());
    EXPECT_EQ (mc.aiSupervisor().effectiveLlmMode(), AiMode::Suggest);
}

TEST (AiMode, TheSupervisorNeverTouchesNotchControllerApi)
{
    // Structural guard for the 0 dB claim: the supervisor translation units
    // name no mutating NotchController entry point.
    const auto srcRoot = juce::File (__FILE__).getParentDirectory().getParentDirectory().getChildFile ("src");
    const auto root = srcRoot.getChildFile ("app");
    for (const auto* name : { "AiSupervisor.cpp", "AiSupervisor.h" })
    {
        const auto text = root.getChildFile (name).loadFileAsString();
        ASSERT_TRUE (text.isNotEmpty()) << name;
        for (const auto* banned : { ".setNotch", "->setNotch", "clearNotch", "clearAll", "retuneNotch", "holdNotch",
                                    "requestRelease", "adoptPreset", "NotchController&", "NotchController*" })
            EXPECT_FALSE (text.contains (banned)) << name << " mentions " << banned;
    }

    // Second layer: the panel that SHOWS proposals (and the proposal / validator
    // files) must not mention the controller's mutating API either. CopilotPanel
    // may not even name the controller.
    for (const auto* rel : { "gui/CopilotPanel.cpp", "gui/CopilotPanel.h", "app/AiAction.cpp", "app/AiAction.h" })
    {
        const auto text = srcRoot.getChildFile (rel).loadFileAsString();
        ASSERT_TRUE (text.isNotEmpty()) << rel;
        for (const auto* banned : { ".setNotch", "->setNotch", "clearNotch", "clearAll", "retuneNotch", "holdNotch",
                                    "requestRelease", "adoptPreset", "setNotchAuthored", "pushRetune", "pushClear",
                                    "NotchController&", "NotchController*" })
            EXPECT_FALSE (text.contains (banned)) << rel << " mentions " << banned;
    }
}
