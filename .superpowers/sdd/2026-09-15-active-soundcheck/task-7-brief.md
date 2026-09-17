### Task 7: `ÁP DỤNG` and `BỎ` — placing on the message thread, through the existing API

**Mức level dự kiến (spec §3):** after `ÁP DỤNG`, each candidate bin is cut by **−6 / −12 / −18 / −24 dB** (or by the preset ceiling, whichever is shallower), at most **6 bins per lane**. After `BỎ` or the 20 s timeout: **0 dB — nothing is placed.**

**Everything here runs on the message thread** (spec §4.6e, inv 17). `setNotch` / `clearNotch` are declared message-thread policy entry points (`src/app/NotchController.h:219-224`), and `setNotchImpl` reads `width_` (`:199`) and the detector's sample rate (`:209`) **outside** `modelMutex_`. So `SoundcheckController` publishes `OutputResult`s and nothing else; the free function `applySoundcheckResults` — called from the button lambda — does all the writing.

**a) Choosing an `index` — B-1, and this is a PRODUCTION defect plan rev 1 shipped.**

`firstFreeIndexLocked` is private (`src/app/NotchController.cpp:1064`), so lane M has to pick an index from `copySnapshot()`. Rev 1 said "re-read the snapshot before every `setNotch`" and stopped there. That does not work, and the cross-check is right about why:

> `latest_` is written **only** inside `runOnce()`'s drain loop (`src/app/NotchController.cpp:550-560` gathers, `:568-582` builds the list, `:617-650` publishes), and the detector thread gets there once per hop — about **every 10.7 ms**. `applySoundcheckResults` runs on the **message thread**, in a tight loop, placing up to six notches in microseconds. Every one of those re-reads therefore returns **the same frame**, `firstFreeIndexTopDown` returns **15** every time, and `setNotchImpl` overwrites without checking `n.active` (`:226-245`) — so **five of six proposals are silently lost.**

The fix is both halves, and the ruling is explicit that neither replaces the other:

- **A `takenThisCall` bitmap, seeded from ONE snapshot at entry**, and marked for every index this call hands out. It is what makes the six placements land on six different indices, and it does not depend on the snapshot refreshing at all.
- **Keep the per-`setNotch` re-read**, now doing the job it is actually good for: catching a **detector** placement that landed since entry. It is a guard, not the allocator.
- **Allocate TOP-DOWN**: 15, 14, 13 … while the detector allocates bottom-up (`firstFreeIndexLocked` scans `i = 0..kSlots`, `:1066-1068`). The two only meet when the chain is nearly full.
- **The remaining race is acknowledged, not hidden.** Between the last snapshot read and the `setNotch` there is still a microsecond-scale window in which the detector can take the slot and be silently overwritten. The detector's placement cadence is ≥ 300 ms (`kDeepenAfterMs`), so the probability is tiny but **not zero**. Closing it entirely would need a second write API, which Q7 forbids.

**b) Clearing the previous run — a re-run replaces the whole SLOT** (I-12, ruling). Read `copySnapshot()`; for **every** notch in that slot's snapshot with `origin == Origin::Soundcheck`, on **either lane**, call `clearNotch (n.channel, n.index, ClearReason::SoundcheckReplace)`.

Rev 1's comment said "this lane's" while the code walked the whole snapshot, which covers both lanes — and `ARerunReplacesItsOwnPreviousProposals` is mono, so nothing could tell the difference. **The whole-slot behaviour is the correct one and is now stated rather than implied**: a soundcheck measures a slot's outputs together, a LINKED placement writes both lanes at one index, and leaving one lane's stale proposal behind while replacing the other's would produce a pair the operator never asked for. A stereo test pins it. This is why Task 4 added both the field and the enumerator.

**c) LINKED slots** (F16, N4, N5). `setNotch` writes **one** lane (`src/app/NotchController.cpp:195-256`), while the detector's LINKED path fans out through `firstFreeIndexAllLanesLocked` (`:1072-1083`, used at `:1105`). If lane M placed off-lane on a linked slot, later LINKED placements would fail to find an index free on **both** lanes and silently slip.

- **Detecting "linked" from `SnapshotBuffer::linked` ALONE is wrong** (N5). That field is the **operator's switch**, not the behaviour: `latest_.linked = linked_.load(...)` with a comment saying exactly that (`src/app/NotchController.cpp:637-640`). The behaviour is `effectiveLinked() = isLinked() || width_ < 2 || taps_[1] == nullptr` (`src/app/NotchController.h:217`) — a mono slot, or a stereo slot missing its lane-1 tap, is **forced** LINKED even when the switch reads INDEP. It is recoverable from the snapshot with no new API:

  ```cpp
  const bool effectiveLinked = snapshot.linked || snapshot.laneCount < 2;
  ```

  because `laneCount = analysedLanes() = (width_ == 2 && taps_[1] != nullptr) ? 2 : 1` (`src/app/NotchController.h:655`, used at `src/app/NotchController.cpp:636`). **Use the right-hand side; never the bare `snapshot.linked`.**
- When effectively linked: **measure once on lane 0**, and `ÁP DỤNG` issues **two `setNotch` calls at the same `index`** for lanes 0 and 1 — the shape `placeConfirmed` produces, built from the message-thread side.
- The index chosen must be free on **both** lanes (only `active` notches enter the snapshot, `src/app/NotchController.cpp:576-580`).
- **A LINKED pair is ALL-OR-NOTHING** (N4). If the first call succeeds and the second returns `false`, unwind the first with `clearNotch (lane, index, ClearReason::PartialApplyUnwind)` — the same enumerator and the same behaviour `placeConfirmed` (`src/app/NotchController.cpp:1262-1264`) and `adoptPreset` (`:528-530`) already use, for the reason their comments give: one lane unprotected while the GUI claims both is worse than placing nothing.
- **"Stop, do not roll back" applies to INDEP only.** There each notch is independent, so a notch that did get placed is real protection; removing it because the next one failed is worse. `applySoundcheckResults` stops at the first `false` and reports `placed` / `refused` / `clearedPrevious`.

**d) The ceiling.** `setNotchImpl` sets `ceilingDb = depth` for every origin other than `Detector` (`src/app/NotchController.cpp:243-245`), so a preventive notch becomes its own ceiling. That is the intent, but it is the implicit behaviour of a ternary, so it gets a test.

**Files:**
- Modify: `src/app/SoundcheckController.h` / `.cpp` — the free function `applySoundcheckResults` and `SoundcheckApplyStats` (declared at the end of the header, defined at the end of the .cpp, both with a `// MESSAGE THREAD ONLY` banner). The `.cpp` needs `<array>`; `tests/test_soundcheckcontroller.cpp` needs `<thread>` (for `ApplyRunsOnTheMessageThread`) and `<memory>` (for `std::unique_ptr` in `NotchRig`) — m-21
- Test: `tests/test_soundcheckcontroller.cpp` (append)

**Interfaces:**
- Consumes: `NotchController::copySnapshot` (`src/app/NotchController.h:447`), `setNotch` (`:221`), `clearNotch` (`:224`), `Origin::Soundcheck` (`:62`), `ClearReason::SoundcheckReplace` / `::PartialApplyUnwind` (`:67-70`, after Task 4), `failNextSetNotchOnLaneForTest` (`:298`), `SnapshotBuffer::linked` / `::laneCount` (`:412-413`), `SoundcheckController::OutputResult` (Task 6).
- Produces:
  ```cpp
  struct SoundcheckApplyStats { int placed = 0, refused = 0, clearedPrevious = 0; };

  // File-local in SoundcheckController.cpp's anonymous namespace. The second
  // argument is B-1's bitmap: indices this CALL has already handed out, which
  // the snapshot cannot know about because it refreshes at hop cadence on
  // another thread.
  // int firstFreeIndexTopDown (const NotchController::SnapshotBuffer& snap,
  //                           const std::array<bool, NotchController::kSlots>& takenThisCall,
  //                           int laneOrMinusOneForAll, int laneCount);

  SoundcheckApplyStats applySoundcheckResults (
      NotchController& controller,
      const std::vector<SoundcheckController::OutputResult>& results);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_soundcheckcontroller.cpp`. Helper first, next to the other helpers in the anonymous namespace:

```cpp
// B-2: tests/test_notchcontroller.cpp's Recorder lives in THAT translation
// unit's anonymous namespace and is invisible here, it has no operator() (the
// sink comes from sink()), and it must outlive the controller it is wired to,
// because the controller's destructor flushes through the sink
// (tests/test_notchcontroller.cpp:1466-1486).
//
// So this TU defines its own, DECLARED BEFORE NotchRig for that lifetime reason.
//
// N-1: and that is not only about where the two TYPES are defined. In EVERY TEST
// BODY that wires a recorder to a rig, the `EventRecorder` OBJECT must be
// declared before the `NotchRig` OBJECT. Locals are destroyed in reverse
// declaration order, so `NotchRig rig; EventRecorder rec;` puts the flush from
// ~NotchController into a vector that has already gone. The bug is silent in a
// passing run and shows up as a heap corruption somewhere else.
//
// It is not promoted into tests/test_gui_helpers.h: that header is GUI-only
// (namespace gui_test, and its only include is juce_gui_basics), so putting a
// NotchController::NotchEvent recorder in it would drag app/NotchController.h
// into every GUI test TU and force an edit to test_notchcontroller.cpp to
// consume the promoted copy.
struct EventRecorder
{
    std::vector<NotchController::NotchEvent> events;

    NotchController::EventSink sink()
    {
        return [this] (const NotchController::NotchEvent& e) { events.push_back (e); };
    }

    bool sawClearWithReason (NotchController::ClearReason r) const
    {
        for (const auto& e : events)
            if (e.kind == NotchController::NotchEvent::Kind::Clear && e.reason == r)
                return true;
        return false;
    }
};

// A NotchController wired the way MainComponent wires one, with the width and
// the lane-1 tap set EXPLICITLY. Lane G lesson 18: a mono harness left at
// width_ == 2 turns effectiveLinked() on and writes a dead lane-1 entry that
// poisons the NEXT test in the same file. Never rely on the default.
//
// B-1(c): pump() exists because latest_ is published only inside runOnce()'s
// drain loop, and that loop body runs only when a block was actually drained off
// a tap. Without it copySnapshot() returns an empty buffer forever, every
// snapshot-reading assertion passes vacuously, and effectiveLinked() cannot be
// derived because laneCount keeps its default of 1
// (NotchController.h:412). Precedent: tests/test_gui_wiring.cpp:1033-1035.
struct NotchRig
{
    LockFreeRingBuffer<float> tapL { 8192 }, tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> cmds { 128 };
    FakeClock clock;
    int lanes = 1;
    std::unique_ptr<NotchController> controller;

    explicit NotchRig (int laneCount) : lanes (laneCount)
    {
        if (lanes == 2) controller = std::make_unique<NotchController> (tapL, &tapR, cmds, clock);
        else            controller = std::make_unique<NotchController> (tapL, cmds, clock);
        controller->setWidth (lanes);
        pump();                       // so the FIRST snapshot is a real one
    }

    void pump()
    {
        const std::vector<float> block (512, 0.0f);
        tapL.write (block.data(), block.size());
        if (lanes == 2)
            tapR.write (block.data(), block.size());   // B-1(b): laneCount needs BOTH
        controller->runOnce();
    }

    NotchController::SnapshotBuffer snapshot()
    {
        pump();
        NotchController::SnapshotBuffer s {};
        controller->copySnapshot (s);
        return s;
    }
};

SoundcheckController::OutputResult oneCandidate (int slot, int lane, double hz, double depthDb)
{
    SoundcheckController::OutputResult r;
    r.slot = slot; r.lane = lane; r.measured = true;
    r.candidateCount = 1;
    r.candidates[0].hz      = (float) hz;
    r.candidates[0].q       = 30.0f;
    r.candidates[0].depthDb = (float) depthDb;
    r.candidates[0].marginDb = -9.0f;
    return r;
}
```

```cpp
// RED IF: applySoundcheckResults is moved onto the lane M thread, or is given a
// NotchController pointer to keep. setNotchImpl reads width_ (:199) and the
// detector's sample rate (:209) OUTSIDE modelMutex_, so it is message-thread
// only by contract (NotchController.h:219-224). inv 17, F27.
TEST (SoundcheckApply, ApplyRunsOnTheMessageThread)
{
    NotchRig rig { 1 };
    const auto id = std::this_thread::get_id();

    std::vector<SoundcheckController::OutputResult> results { oneCandidate (0, 0, 1000.0, -12.0) };
    const auto stats = applySoundcheckResults (*rig.controller, results);

    EXPECT_EQ (std::this_thread::get_id(), id);
    EXPECT_EQ (stats.placed, 1);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15)) << "top-down allocation starts at 15";
}

// RED IF: either half of B-1 is dropped -- the takenThisCall bitmap, or the
// per-setNotch re-read. The bitmap is what makes six placements land on six
// indices (the snapshot refreshes at hop cadence, ~10.7 ms, on the DETECTOR
// thread, while this loop runs in microseconds on the message thread, so every
// re-read returns the same frame). The re-read is what catches a detector
// placement that landed since entry. F10, B-1.
TEST (SoundcheckApply, ApplyReReadsTheSnapshotBeforeEachSetNotch)
{
    NotchRig rig { 1 };

    std::vector<SoundcheckController::OutputResult> results;
    auto r = oneCandidate (0, 0, 1000.0, -12.0);
    r.candidateCount = 2;
    r.candidates[1].hz = 2000.0f; r.candidates[1].q = 30.0f; r.candidates[1].depthDb = -12.0f;
    results.push_back (r);

    // Occupy index 14 with a DETECTOR notch before the call, so the re-read has
    // something real to catch. The takenThisCall bitmap handles 15; the re-read
    // handles 14. Both halves are exercised by this one test.
    ASSERT_TRUE (rig.controller->setNotch (0, 14, 5000.0, 30.0, -12.0,
                                           NotchController::Origin::Detector));
    rig.pump();

    const auto stats = applySoundcheckResults (*rig.controller, results);

    EXPECT_EQ (stats.placed, 2);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15));
    EXPECT_TRUE (rig.controller->activeForTest (0, 14)) << "the detector's notch is still here";
    EXPECT_TRUE (rig.controller->activeForTest (0, 13))
        << "14 was taken by the detector, and 15 by THIS call -- 13 is next. "
           "Without takenThisCall (B-1) the second placement lands on 15 again "
           "and overwrites the first, because the snapshot cannot have refreshed "
           "in the microseconds between the two setNotch calls.";
    // And the detector's notch at 14 is still there, not overwritten.
    EXPECT_NEAR (rig.controller->depthDbForTest (0, 14), -12.0, 1.0e-9);
}

// RED IF: an INDEP failure rolls back. Each INDEP notch stands alone, so a notch
// that WAS placed is real protection and pulling it because the next one failed
// is strictly worse. N4.
TEST (SoundcheckApply, IndepApplyDoesNotUnwind)
{
    NotchRig rig { 2 };
    rig.controller->setLinked (false);

    std::vector<SoundcheckController::OutputResult> results {
        oneCandidate (0, 0, 1000.0, -12.0),
        oneCandidate (0, 1, 1500.0, -12.0)
    };

    rig.controller->failNextSetNotchOnLaneForTest (1);
    const auto stats = applySoundcheckResults (*rig.controller, results);

    EXPECT_EQ (stats.placed, 1);
    EXPECT_EQ (stats.refused, 1);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15)) << "the lane-0 notch STAYS";
}

// RED IF: an INDEP failure keeps going and silently loses count. Stop at the
// first false, and report it. F10.
TEST (SoundcheckApply, PartialApplyStopsAndReportsRefused)
{
    NotchRig rig { 1 };

    auto r = oneCandidate (0, 0, 1000.0, -12.0);
    r.candidateCount = 3;
    r.candidates[1].hz = 2000.0f; r.candidates[1].q = 30.0f; r.candidates[1].depthDb = -12.0f;
    r.candidates[2].hz = 3000.0f; r.candidates[2].q = 30.0f; r.candidates[2].depthDb = -12.0f;

    // An invalid Q makes setNotchImpl refuse at NotchController.cpp:211.
    r.candidates[1].q = 0.0f;

    const auto stats = applySoundcheckResults (*rig.controller, { r });

    EXPECT_EQ (stats.placed, 1);
    EXPECT_GE (stats.refused, 1);
    EXPECT_FALSE (rig.controller->activeForTest (0, 13)) << "it must STOP, not skip and carry on";
}

// RED IF: a linked slot gets one lane only, or two different indices. The
// detector's LINKED path would then never find an index free on both lanes and
// would silently slip. F16.
TEST (SoundcheckApply, LinkedSlotPlacesBothLanesAtOneIndex)
{
    NotchRig rig { 2 };                   // width 2 AND a real lane-1 tap
    rig.controller->setLinked (true);

    // B-1(b): laneCount is published from analysedLanes()
    // (NotchController.h:655, used at .cpp:636) and defaults to 1
    // (NotchController.h:412). If the snapshot has never refreshed -- or
    // refreshed without a lane-1 block -- lane M reads laneCount == 1 and places
    // ONE lane on a LINKED slot. NotchRig::pump() writes BOTH taps; assert the
    // precondition rather than assuming it.
    const auto pre = rig.snapshot();
    ASSERT_EQ (pre.laneCount, 2u) << "pump both taps, or this test cannot fail correctly";

    const auto stats = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });

    EXPECT_EQ (stats.placed, 2);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15));
    EXPECT_TRUE (rig.controller->activeForTest (1, 15));
    EXPECT_NEAR (rig.controller->depthDbForTest (0, 15),
                 rig.controller->depthDbForTest (1, 15), 1.0e-9);
}

// RED IF: linkedness is read from SnapshotBuffer::linked alone. That field is the
// operator's SWITCH, not the behaviour (NotchController.cpp:637-640): a mono slot
// is FORCED linked while the switch still reads INDEP. N5.
TEST (SoundcheckApply, LinkedIsDerivedFromLaneCountNotJustTheSwitch)
{
    NotchRig rig { 1 };                   // mono: laneCount == 1, linked switch OFF
    rig.controller->setLinked (false);

    const auto snap = rig.snapshot();
    ASSERT_FALSE (snap.linked);
    ASSERT_LT (snap.laneCount, 2u);

    const auto stats = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });

    // One lane exists, so "both lanes" is one placement -- and crucially NOTHING
    // is written to lane 1, which has no tap.
    EXPECT_EQ (stats.placed, 1);
    EXPECT_TRUE  (rig.controller->activeForTest (0, 15));
    EXPECT_FALSE (rig.controller->activeForTest (1, 15));
}

// RED IF: a linked pair is left half-placed. One lane protected while the GUI
// claims both is worse than placing nothing -- the same reasoning
// placeConfirmed (:1262-1264) and adoptPreset (:528-530) already follow. N4.
TEST (SoundcheckApply, LinkedPairUnwindsWhenTheSecondLaneFails)
{
    // N-1: the RECORDER FIRST. Destruction runs in reverse declaration order, so
    // a recorder declared after the rig is already gone when ~NotchController
    // runs stop() and flushes its remaining events through the sink -- a write
    // into a destroyed vector. Same rule, same reason, as
    // tests/test_notchcontroller.cpp:1466-1470.
    EventRecorder rec;
    NotchRig      rig { 2 };
    rig.controller->setLinked (true);
    rig.controller->setEventSink (rec.sink());

    rig.controller->failNextSetNotchOnLaneForTest (1);
    const auto stats = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });

    EXPECT_EQ (stats.placed, 0);
    EXPECT_GE (stats.refused, 1);
    EXPECT_FALSE (rig.controller->activeForTest (0, 15)) << "lane 0 must be unwound";

    rig.pump();
    EXPECT_TRUE (rec.sawClearWithReason (NotchController::ClearReason::PartialApplyUnwind))
        << "and with THAT reason, not Manual";
}

// RED IF: the previous run's preventive notches are left in place. They never
// auto-release (NotchController.cpp:729), so the 16-slot chain drains after a
// few soundchecks. §4.6b.
TEST (SoundcheckApply, ARerunReplacesItsOwnPreviousProposals)
{
    // N-1: the RECORDER FIRST -- see LinkedPairUnwindsWhenTheSecondLaneFails.
    EventRecorder rec;
    NotchRig      rig { 1 };
    rig.controller->setEventSink (rec.sink());

    auto first = applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1000.0, -12.0) });
    ASSERT_EQ (first.placed, 1);
    rig.pump();                                   // B-1(c)

    const auto second = applySoundcheckResults (*rig.controller,
                                                { oneCandidate (0, 0, 1200.0, -12.0) });
    rig.pump();

    EXPECT_EQ (second.clearedPrevious, 1);
    EXPECT_EQ (second.placed, 1);

    EXPECT_TRUE (rec.sawClearWithReason (NotchController::ClearReason::SoundcheckReplace));

    // Exactly ONE preventive notch survives.
    const auto snap = rig.snapshot();
    int soundcheckNotches = 0;
    for (std::uint32_t i = 0; i < snap.notchCount; ++i)
        if (snap.notches[i].origin == NotchController::Origin::Soundcheck)
            ++soundcheckNotches;
    EXPECT_EQ (soundcheckNotches, 1);
}

// RED IF: a re-run leaves the OTHER lane's stale proposal behind. I-12's ruling:
// a re-run replaces the whole SLOT, not one lane. A soundcheck measures a slot's
// outputs together and a LINKED placement writes both lanes at one index, so
// half-replacing produces a pair the operator never asked for. The mono test
// above cannot tell the difference; this one can.
TEST (SoundcheckApply, ARerunReplacesTheWholeSlotNotJustOneLane)
{
    NotchRig rig { 2 };
    rig.controller->setLinked (false);          // INDEP: two independent lanes

    ASSERT_EQ (applySoundcheckResults (*rig.controller,
                                       { oneCandidate (0, 0, 1000.0, -12.0),
                                         oneCandidate (0, 1, 1500.0, -12.0) }).placed, 2);
    rig.pump();

    // A re-run that names only lane 0 must still clear BOTH previous proposals.
    const auto again = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1100.0, -12.0) });
    rig.pump();

    EXPECT_EQ (again.clearedPrevious, 2) << "the whole slot, not just this lane";

    const auto snap = rig.snapshot();
    int soundcheckNotches = 0;
    for (std::uint32_t i = 0; i < snap.notchCount; ++i)
        if (snap.notches[i].origin == NotchController::Origin::Soundcheck)
            ++soundcheckNotches;
    EXPECT_EQ (soundcheckNotches, 1);
}

// RED IF: a Detector or Preset notch is cleared by the replace pass. Only lane
// M's OWN previous proposals go.
TEST (SoundcheckApply, ReplacingLeavesEveryOtherOriginAlone)
{
    NotchRig rig { 1 };
    ASSERT_TRUE (rig.controller->setNotch (0, 0, 500.0, 30.0, -12.0,
                                           NotchController::Origin::Detector));
    ASSERT_TRUE (rig.controller->setNotch (0, 1, 700.0, 30.0, -12.0,
                                           NotchController::Origin::Preset));
    ASSERT_TRUE (rig.controller->setNotch (0, 2, 900.0, 30.0, -12.0,
                                           NotchController::Origin::Manual));
    rig.pump();

    applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1000.0, -12.0) });
    rig.pump();
    const auto again = applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1100.0, -12.0) });

    EXPECT_EQ (again.clearedPrevious, 1);
    EXPECT_TRUE (rig.controller->activeForTest (0, 0));
    EXPECT_TRUE (rig.controller->activeForTest (0, 1));
    EXPECT_TRUE (rig.controller->activeForTest (0, 2));
}

// RED IF: the ternary at NotchController.cpp:243-245 changes and a preventive
// notch stops being its own ceiling. It is implicit behaviour, so it gets an
// explicit test. §4.6d.
TEST (SoundcheckApply, APreventiveNotchIsItsOwnCeiling)
{
    NotchRig rig { 1 };
    applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1000.0, -12.0) });

    EXPECT_DOUBLE_EQ (rig.controller->rawCeilingDbForTest (0, 15), -12.0);
    EXPECT_DOUBLE_EQ (rig.controller->ceilingDbForTest (0, 15), -12.0);
}

// RED IF: a result that could not be measured still places something. inv 13.
TEST (SoundcheckApply, AnUnmeasuredResultPlacesNothing)
{
    NotchRig rig { 1 };
    auto r = oneCandidate (0, 0, 1000.0, -12.0);
    r.measured = false;

    const auto stats = applySoundcheckResults (*rig.controller, { r });
    EXPECT_EQ (stats.placed, 0);
    EXPECT_FALSE (rig.controller->activeForTest (0, 15));
}
```

- [ ] **Step 2: Run and watch them fail**

```bash
cmake --build build --config Release
```
Expected: `'applySoundcheckResults': identifier not found`.

- [ ] **Step 3: Implement `applySoundcheckResults`**

At the end of `src/app/SoundcheckController.cpp`, under a banner comment repeating the thread rule:

```cpp
//==============================================================================
// MESSAGE THREAD ONLY (spec §4.6e, invariant 17).
//
// setNotch/clearNotch are policy entry points declared message-thread
// (NotchController.h:219-224), and setNotchImpl additionally reads width_
// (:199) and the detector's sample rate (:209) OUTSIDE modelMutex_. This is a
// FREE FUNCTION rather than a SoundcheckController method so that no route
// exists by which the lane M thread could reach it: that class holds no
// NotchController pointer at all.
//==============================================================================
SoundcheckApplyStats applySoundcheckResults (
    NotchController& controller,
    const std::vector<SoundcheckController::OutputResult>& results)
{
    SoundcheckApplyStats stats;

    // B-1: indices THIS CALL has handed out. The snapshot cannot know about them
    // -- latest_ is republished only inside runOnce()'s drain loop on the
    // DETECTOR thread, about once per hop (~10.7 ms), while this loop runs in
    // microseconds on the message thread. Without this array every re-read
    // returns the same frame, every lookup answers 15, and five of six proposals
    // are overwritten in silence (setNotchImpl does not check n.active,
    // NotchController.cpp:226-245).
    std::array<bool, NotchController::kSlots> takenThisCall {};

    // --- (b) clear THIS SLOT's previous preventive notches --------------------
    // Whole slot, BOTH lanes (I-12): a soundcheck measures a slot's outputs
    // together, and a LINKED placement writes both lanes at one index, so
    // replacing one lane's proposal while leaving the other's behind produces a
    // pair the operator never asked for.
    {
        NotchController::SnapshotBuffer snap {};
        controller.copySnapshot (snap);
        for (std::uint32_t i = 0; i < snap.notchCount; ++i)
        {
            const auto& n = snap.notches[i];
            if (n.origin != NotchController::Origin::Soundcheck)
                continue;
            controller.clearNotch ((int) n.channel, (int) n.index,
                                   NotchController::ClearReason::SoundcheckReplace);
            ++stats.clearedPrevious;
            // N-5: nothing is marked free here, and that is deliberate.
            //
            // takenThisCall starts all-false, so clearing an index to false
            // would be a no-op anyway. More importantly, an index freed HERE is
            // NOT reusable by THIS call: the snapshot each placement re-reads
            // still lists the cleared notch for up to ~10.7 ms (latest_ is
            // republished only inside runOnce()'s drain loop on the detector
            // thread), so firstFreeIndexTopDown skips it regardless. The result
            // is conservative -- a re-run may place lower down the chain than it
            // strictly had to -- and conservative is the right side to be on
            // when the alternative is two writers on one index.
        }
    }

    for (const auto& result : results)
    {
        if (! result.measured || result.routingInvalid)
            continue;                                        // a valid result that places nothing

        for (int c = 0; c < result.candidateCount; ++c)
        {
            const auto& cand = result.candidates[(std::size_t) c];

            // (a) RE-READ before EVERY setNotch. This is a GUARD against a
            // DETECTOR placement that landed since entry -- it is NOT the
            // allocator, because it cannot see what this call has already
            // handed out (B-1).
            NotchController::SnapshotBuffer snap {};
            controller.copySnapshot (snap);

            // (c/N5) The BEHAVIOUR, not the switch: a mono slot or a stereo slot
            // missing its lane-1 tap is forced LINKED even with the switch off
            // (NotchController.h:217, :655; NotchController.cpp:636-640).
            const bool linked    = snap.linked || snap.laneCount < 2;
            const int  laneCount = (int) snap.laneCount;

            // TOP-DOWN, while the detector goes bottom-up
            // (NotchController.cpp:1066-1068): the two only meet when the chain
            // is nearly full.
            const int index = firstFreeIndexTopDown (snap, takenThisCall,
                                                     linked ? -1 : result.lane, laneCount);
            if (index < 0) { ++stats.refused; break; }
            takenThisCall[(std::size_t) index] = true;   // B-1: before any write

            if (linked)
            {
                // ALL-OR-NOTHING (N4): one lane protected while the GUI claims
                // both is worse than placing nothing -- the same reasoning
                // placeConfirmed (:1262-1264) and adoptPreset (:528-530) use.
                int placedLanes = 0;
                bool ok = true;
                for (int lane = 0; lane < laneCount && ok; ++lane)
                {
                    if (controller.setNotch (lane, index, cand.hz, cand.q, cand.depthDb,
                                             NotchController::Origin::Soundcheck))
                        ++placedLanes;
                    else
                        ok = false;
                }

                if (! ok)
                {
                    for (int lane = 0; lane < placedLanes; ++lane)
                        controller.clearNotch (lane, index,
                                               NotchController::ClearReason::PartialApplyUnwind);
                    ++stats.refused;
                    break;                                   // stop this result
                }
                stats.placed += placedLanes;
            }
            else
            {
                if (! controller.setNotch (result.lane, index, cand.hz, cand.q, cand.depthDb,
                                           NotchController::Origin::Soundcheck))
                {
                    // INDEP: STOP, do NOT roll back. A notch already placed is
                    // real protection (F10).
                    ++stats.refused;
                    break;
                }
                ++stats.placed;
            }
        }
    }

    return stats;
}
```

`firstFreeIndexTopDown` is a file-local static in `SoundcheckController.cpp`'s anonymous namespace:

```cpp
int firstFreeIndexTopDown (const NotchController::SnapshotBuffer& snap,
                           const std::array<bool, NotchController::kSlots>& takenThisCall,
                           int lane, int laneCount)
{
    for (int index = NotchController::kSlots - 1; index >= 0; --index)
    {
        // B-1: what THIS call has already handed out. The snapshot cannot know.
        if (takenThisCall[(std::size_t) index])
            continue;

        bool free = true;
        for (std::uint32_t i = 0; i < snap.notchCount && free; ++i)
        {
            const auto& n = snap.notches[i];
            if ((int) n.index != index)
                continue;
            // lane < 0 means "must be free on every lane" (a LINKED pair).
            if (lane < 0 || (int) n.channel == lane)
                free = false;
        }
        if (free)
            return index;
    }
    return -1;
}
```

Only `active` notches enter the snapshot (`src/app/NotchController.cpp:576-580`), so "not present" is exactly "free". `laneCount` is unused in the scan itself and is kept in the signature so a future per-lane rule has somewhere to go; a reviewer may reasonably ask for it to be dropped.

- [ ] **Step 4: Wire `Results` → `Idle`**

`SoundcheckController::applyRequested()` and `dismissRequested()` both take `state_` from `Results` to `Idle` and clear the results. `applyRequested()` is called by the GUI **after** `applySoundcheckResults` has returned, so the controller never sees a `NotchController` even indirectly.

- [ ] **Step 5: Build, run, gate, commit**

```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R "SoundcheckApply|SoundcheckController" --output-on-failure
```
Expected: `100% tests passed` (the 20 `SoundcheckController` tests plus **12** `SoundcheckApply` tests).

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (619)` — 608 + 11. ESTIMATE. (Twelve `SoundcheckApply` tests are written, but `ARerunReplacesTheWholeSlotNotJustOneLane` replaces nothing — it is a net +11 because `LinkedIsDerivedFromLaneCountNotJustTheSwitch` and `ARerunReplacesItsOwnPreviousProposals` were already counted. Use the number `ctest` prints.)

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/SoundcheckController.h src/app/SoundcheckController.cpp tests/test_soundcheckcontroller.cpp
```
```bash
git commit -m "feat(lane-m): APPLY places on the message thread -- top-down index, linked all-or-nothing, SoundcheckReplace"
```

---

