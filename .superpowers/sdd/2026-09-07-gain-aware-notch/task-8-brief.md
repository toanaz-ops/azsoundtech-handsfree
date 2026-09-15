### Task 8: room memory — a howl that comes back to the same bin starts where it left off

**Mức level dự kiến (spec §3):** a howl returning to the **same FFT bin** within 5 minutes of an auto-release is notched at the depth it needed last time (up to the ceiling's rung, at most −24 dB) **on the first block**, instead of crawling up from −6. At that bin, at that instant, the cut is **equal to or deeper than 1.1.3** — up to 18 dB deeper than the −6 dB a first-time placement would give. Outside the bin: **0 dB**. A neighbouring bin is a NEW howl and starts at −6 (Q10: ±0 bin tolerance, because ±1 bin is ±21.5 Hz at 44.1 kHz/2048 and an instrument partial next door must not inherit a deep cut). Each remembered entry is used once.

**m-8 — an entry may hold an off-rung depth, and that is fine.** A Manual notch placed at −9 and auto-released records `deepestDb = −9`. Under Q13 the read path needs no quantisation at all: the remembered depth is clamped to the CURRENT ceiling (`std::max(remembered, ceiling)` — max picks the shallower) by the same step-4 clamp every placement already runs, and the ceiling is itself the deepest legal rung. Nothing else is required. Do not add a "snap to a rung" step; it would only re-introduce the Q13 defect on the memory path.

**Files:**
- Modify: `src/app/NotchController.h` (the `MemoryEntry` struct, `roomMemory_`, `roomMemoryHead_`, three private helpers — declare them next to `pushRetuneLocked`)
- Modify: `src/app/NotchController.cpp:80-117` (`setWidth`), `:223-229` (`clearAll`), `placeConfirmed`'s index-lookup block and depth choice **as Task 5 left them** (Task 5 moved both — do not go looking for the v1 line numbers `:589-596`), the release path's Clear branch (Task 7), and `setSampleRate` (**m-B: pre-Task-5 that is `:864-877`, and Tasks 5–7 all insert above it — find it by the signature `void NotchController::setSampleRate (double sampleRate)`, not by the number**)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes (Tasks 4, 5, 7): `activeForTest`, `deepestDbForTest`, `ClearReason::AutoRelease`'s branch in step 3, and the hook Task 5 left inside the index-lookup lock.
- Produces:
  ```cpp
  // private
  struct MemoryEntry { double frequencyHz, deepestDb, clearedAtMs; bool used; };
  void   rememberReleaseLocked (int lane, double frequencyHz, double deepestDb, bool bothLanes);
  double takeRememberedDepthLocked (int lane, double frequencyHz, double binWidthHz, bool bothLanes);
  void   clearRoomMemoryLocked();
  std::array<std::array<MemoryEntry, kMemoryEntriesPerLane>, kChannels> roomMemory_ {};
  std::array<int, kChannels> roomMemoryHead_ {};
  ```

**Thread facts:** `roomMemory_` is written from the release path (detector thread, `modelMutex_` held) and read from `placeConfirmed` (detector thread) — and cleared from `setWidth`/`clearAll`/`setSampleRate` (message thread). It therefore lives under `modelMutex_`, the lock all four already take or can take without creating a new order. No allocation: fixed-size arrays, oldest entry overwritten.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`. `pumpQuietFor` comes from Task 7.

Two fixture rules apply to every test below and are not repeated in each comment:

- **B-3: liveness is `activeForTest`, never `depthDbForTest(...) < 0.0`.** `pushClearLocked` (`NotchController.cpp:195-212`) lowers only `active` — `depthDB` stays because the Clear event reads it at `:208`. A depth probe therefore matches every slot that has ever held a notch, and the v1 versions of these tests would have "found" a cleared slot and asserted the remembered depth against a corpse.
- **B-4: every quiet pump is derived from the rung the notch stands on**, using the table in Task 7's header. A notch that climbed to the −24 ceiling needs 30 + 10 + 10 + 10 = **60 s** to Clear, not the 55 s the v1 plan used, so the memory entry would never have been written at all.

**m-E — `firstActiveIndex` goes in the ANONYMOUS NAMESPACE, not at the end of the
file.** The anonymous namespace in `tests/test_notchcontroller.cpp` closes at
`} // namespace` on **line 405**; everything appended after that is at global scope,
where a non-`static` free function in a translation unit that is linked with the
others is an ODR hazard and a linker surprise waiting for the next test file to
declare the same name. Put it with the other helpers **at line 349, immediately after
`pump` closes** — the same insertion point Task 5 uses for `RampSineSource` /
`primeAndPlaceSlowly` and Task 7 uses for `pumpQuietFor`. (Numbers are pre-Task-5;
search for the closing `}` of `void pump (Harness& h, const std::vector<float>& hop)`.)

```cpp
// A findable helper for "which index is holding a notch on this lane" -- the
// tests below all need it and none of them may use the depth to answer it.
int firstActiveIndex (NotchController& c, int lane)
{
    for (int k = 0; k < NotchController::kSlots; ++k)
        if (c.activeForTest (lane, k))
            return k;
    return -1;
}
```

```cpp
// RED IF room memory stops working (spec 4.6, Q3/Q6). A howl that returns to
// the same bin within 5 minutes must not start the ladder over.
TEST (NotchControllerLadder, AHowlReturningToTheSameBinStartsAtTheRememberedDepth)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    SineSource tone;
    for (int i = 0; i < 200; ++i)     // climb to the ceiling
        pump (h, tone.hop());
    const double deepest = h.controller.deepestDbForTest (0, slot);
    ASSERT_DOUBLE_EQ (deepest, -24.0);

    NoiseSource quiet;
    // From -24: 30 + 10 + 10 + 10 = 60 s to Clear. 62 s leaves 2 s of margin.
    pumpQuietFor (h, quiet, 62000.0);
    ASSERT_FALSE (h.controller.activeForTest (0, slot))
        << "the notch had not finished the release ladder, so nothing was remembered";

    // The same howl comes back. With no memory this places at -12 (SineSource
    // is a hard start, so the steep-rise branch fires); the memory must beat
    // that and go straight to what the room needed.
    SineSource again;
    int placed = -1;
    for (int i = 0; i < 60 && placed < 0; ++i)
    {
        pump (h, again.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), deepest);
}

// RED IF the bin tolerance loosens (Q10). One bin away is a DIFFERENT howl --
// at 48 kHz / 2048 that is 23.4 Hz, and an instrument partial next door must
// not inherit a -24 dB cut on its first block.
//
// m-F: the name says "starts fresh", NOT "starts at -6", because -6 is not
// what this fixture produces. SineSource switches a full-scale tone on in one
// block, so riseRatio is enormous, the steep-rise branch fires and the fresh
// placement is -12 (spec 4.3 step 2). What is being pinned here is that the
// depth came from the PLACEMENT POLICY and not from room memory, so the
// assertion is exact: -12, never -24, and never the loose `>= -12` of rev 2,
// which -24 would also have to fail but which would silently accept -6 if the
// steep-rise branch broke.
TEST (NotchControllerLadder, OneBinAwayIsANewHowlAndStartsFreshNotFromMemory)
{
    Harness h;
    const double binHz = kTestSr / Detector::kFftSize;   // 23.4375
    h.controller.setNotchDefaults (30.0, -24.0);

    // Place, deepen by hand, then let the ladder clear it: the memory entry
    // is written by the AutoRelease at the bottom of the ladder.
    ASSERT_TRUE (h.controller.setNotch (0, 0, 43.0 * binHz, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    // A candidate exactly ONE bin up. Memory must not answer for it.
    h.controller.setDetectionActive (true);
    SineSource neighbour; neighbour.freq = 44.0 * binHz;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, neighbour.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -12.0)
        << "a neighbouring bin inherited the remembered depth (or the "
           "steep-rise placement branch stopped firing)";
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, placed), -12.0)
        << "the memory's -24 leaked into deepestDb even though the depth was "
           "placed fresh -- the next reclamp would then go to -24";
}

// RED IF the TTL stops expiring entries (spec 4.6). 5 minutes and one
// millisecond is a different show.
TEST (NotchControllerLadder, RoomMemoryExpiresAfterFiveMinutes)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);                       // -24: cleared, remembered
    ASSERT_FALSE (h.controller.activeForTest (0, 0));
    pumpQuietFor (h, quiet, NotchController::kMemoryTtlMs + 1000.0);   // expired

    h.controller.setDetectionActive (true);
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_GE (h.controller.depthDbForTest (0, placed), -12.0)
        << "an expired entry was still used";
}

// RED IF a remembered entry can be used twice (spec 4.6). It describes ONE
// release; a second howl at that bin has to earn its own depth.
TEST (NotchControllerLadder, ARememberedEntryIsUsedOnlyOnce)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setDetectionActive (true);
    SineSource tone;
    int first = -1;
    for (int i = 0; i < 80 && first < 0; ++i)
    {
        pump (h, tone.hop());
        first = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (first, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, first), -24.0);

    // Clear it by hand (no AutoRelease => no new memory entry) and let it
    // place again: the ladder must start from scratch.
    h.controller.clearNotch (0, first, NotchController::ClearReason::Manual);
    ASSERT_FALSE (h.controller.activeForTest (0, first));
    int second = -1;
    for (int i = 0; i < 80 && second < 0; ++i)
    {
        pump (h, tone.hop());
        second = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (second, 0);
    EXPECT_GE (h.controller.depthDbForTest (0, second), -12.0)
        << "the entry was consumed twice";
}

// RED IF the ceiling stops capping a remembered depth (spec 4.3 step 4, m-8).
// A -24 memory under a -12 ceiling must place at -12. Under Q13 that is the
// ONLY thing the read path does to a remembered depth: no rung quantisation,
// because the ceiling IS the deepest legal rung. A memory holding an odd depth
// (a Manual -9 that auto-released) is therefore placed at -9 verbatim under a
// -24 ceiling -- correct, and covered by the second half below.
TEST (NotchControllerLadder, ARememberedDepthIsStillCappedByTheCeiling)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setNotchDefaults (30.0, -12.0);   // ceiling drops
    h.controller.setDetectionActive (true);
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -12.0);
}

// RED IF the read path snaps a remembered depth to a fixed rung (m-8). A
// Manual notch is placed at whatever the operator typed, so an entry can hold
// -9; under a ceiling that permits it, -9 is what comes back. Quantising to
// -6 would throw away 3 dB the room demonstrably needed, and quantising to -12
// would place DEEPER than the notch ever ran, which invariant 2 forbids.
TEST (NotchControllerLadder, AnOffRungRememberedDepthComesBackVerbatim)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -9.0,
                                        NotchController::Origin::Manual));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 42000.0);   // from -9: -6 at 30 s, Clear at 40 s
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setDetectionActive (true);
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -9.0);
}

// RED IF a room change stops wiping the memory (spec 4.6). setWidth is the
// boundary that actually fires in the shipping app -- MainComponent's
// onAfterRestart hook calls it on every engine restart, i.e. every device,
// rate and buffer change (NotchController.cpp:106-114).
TEST (NotchControllerLadder, SetWidthClearAllAndSetSampleRateWipeRoomMemory)
{
    for (int which = 0; which < 3; ++which)
    {
        Harness h;
        h.controller.setNotchDefaults (30.0, -24.0);
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                            NotchController::Origin::Detector));
        NoiseSource quiet;
        pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
        ASSERT_FALSE (h.controller.activeForTest (0, 0)) << "which=" << which;

        if (which == 0) h.controller.setWidth (2);
        if (which == 1) h.controller.clearAll();
        if (which == 2) h.controller.setSampleRate (48000.0);

        h.controller.setDetectionActive (true);
        SineSource tone;
        int placed = -1;
        for (int i = 0; i < 80 && placed < 0; ++i)
        {
            pump (h, tone.hop());
            placed = firstActiveIndex (h.controller, 0);
        }
        ASSERT_GE (placed, 0) << "which=" << which;
        EXPECT_GE (h.controller.depthDbForTest (0, placed), -12.0)
            << "memory survived a room change, which=" << which;
    }
}

// RED IF a LINKED release leaves an orphan entry on one lane (M-11). One
// Clear writes both lanes; one placement consumes both.
TEST (NotchControllerLadder, LinkedReleaseWritesAndConsumesBothLanes)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    ASSERT_TRUE (h.controller.setNotch (1, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));

    NoiseSource quietL, quietR; quietR.rng.seed (999u);
    // From -24: 30 + 10 + 10 + 10 = 60 s to Clear, +2 s margin.
    const int blocks = (int) std::lround (62000.0 / kBlockMs);
    for (int i = 0; i < blocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());
    ASSERT_FALSE (h.controller.activeForTest (0, 0));
    ASSERT_FALSE (h.controller.activeForTest (1, 0));

    h.controller.setDetectionActive (true);
    SineSource toneL, toneR;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pumpStereo (h, toneL.hop(), toneR.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -24.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (1, placed), -24.0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchControllerLadder --output-on-failure`
Expected: FAIL — `AHowlReturningToTheSameBinStartsAtTheRememberedDepth` places at −12 (the steep-rise rung) instead of the remembered depth.

- [ ] **Step 3: Declare the memory in `src/app/NotchController.h`**

Next to the `pushRetuneLocked` declaration (Task 4's block):

```cpp
    // --- "Room memory" (spec 4.6, Q3/Q6/Q10) ------------------------------
    // What a bin needed LAST time, so a howl that comes back inside
    // kMemoryTtlMs is answered at the depth that killed it rather than
    // crawling up the ladder again while the room rings. Per lane, fixed size,
    // oldest entry overwritten -- no allocation, ever. Detector-thread reads
    // (placeConfirmed, the release path) and message-thread wipes (setWidth,
    // clearAll, setSampleRate) all go through modelMutex_, the lock every one
    // of those already takes: no new lock, no new order.
    //
    // NOT persisted. A preset describes a rig; this describes the last five
    // minutes of one room, and writing it to disk would let a soundman open a
    // file that silently deep-notches a frequency that is not ringing.
    struct MemoryEntry
    {
        double frequencyHz = 0.0;
        double deepestDb   = 0.0;
        double clearedAtMs = 0.0;
        bool   used        = false;   // false == empty slot
    };

    // modelMutex_ HELD. Records what `frequencyHz` needed. `bothLanes` writes
    // the pair (M-11: a LINKED clear must not leave an orphan on one lane).
    // An existing entry for the SAME frequency is merged rather than
    // duplicated -- both lanes of a linked pair clear in the same tick and
    // would otherwise each write twice.
    void   rememberReleaseLocked (int lane, double frequencyHz, double deepestDb, bool bothLanes);
    // modelMutex_ HELD. The remembered depth for (lane, bin), CONSUMING the
    // entry -- one use only. NaN when nothing matched. Bin equality is exact
    // (Q10): one bin away is a new howl.
    double takeRememberedDepthLocked (int lane, double frequencyHz, double binWidthHz, bool bothLanes);
    void   clearRoomMemoryLocked();
```

and, next to `model_` (line 454):

```cpp
    std::array<std::array<MemoryEntry, kMemoryEntriesPerLane>, kChannels> roomMemory_ {};
    std::array<int, kChannels> roomMemoryHead_ {};
```

- [ ] **Step 4: Implement in `src/app/NotchController.cpp`**

Add the three helpers after `pushRetuneLocked`:

```cpp
void NotchController::rememberReleaseLocked (int lane, double frequencyHz,
                                             double deepestDb, bool bothLanes)
{
    const int first = bothLanes ? 0 : lane;
    const int last  = bothLanes ? width_ - 1 : lane;
    for (int l = first; l <= last && l < kChannels; ++l)
    {
        auto& bank = roomMemory_[(std::size_t) l];

        // Merge onto an existing entry for the same frequency rather than
        // writing a second one. A LINKED pair clears on the same tick and both
        // lanes write both banks, so without this the ring fills with
        // duplicates and a lookup consumes one while leaving its twin behind.
        int target = -1;
        for (int k = 0; k < kMemoryEntriesPerLane; ++k)
            if (bank[(std::size_t) k].used
                && bank[(std::size_t) k].frequencyHz == frequencyHz)
            {
                target = k;
                break;
            }

        const bool merging = target >= 0;
        if (! merging)
        {
            target = roomMemoryHead_[(std::size_t) l];
            roomMemoryHead_[(std::size_t) l] = (target + 1) % kMemoryEntriesPerLane;
        }

        auto& e = bank[(std::size_t) target];
        // The DEEPEST of the two wins: whichever lane needed more is what the
        // room needed (M-11).
        e.deepestDb   = merging ? std::min (e.deepestDb, deepestDb) : deepestDb;
        e.frequencyHz = frequencyHz;
        e.clearedAtMs = liveMs_;
        e.used        = true;
    }
}

double NotchController::takeRememberedDepthLocked (int lane, double frequencyHz,
                                                   double binWidthHz, bool bothLanes)
{
    if (! (binWidthHz > 0.0))
        return std::numeric_limits<double>::quiet_NaN();

    const long targetBin = std::lround (frequencyHz / binWidthHz);
    const int  first = bothLanes ? 0 : lane;
    const int  last  = bothLanes ? width_ - 1 : lane;

    double best = std::numeric_limits<double>::quiet_NaN();
    for (int l = first; l <= last && l < kChannels; ++l)
        for (auto& e : roomMemory_[(std::size_t) l])
        {
            if (! e.used)
                continue;
            // Q10: SAME bin, +-0. One bin is 21.5 Hz at 44.1 kHz / 2048, and a
            // partial next door must not inherit a deep cut on its first block.
            if (std::lround (e.frequencyHz / binWidthHz) != targetBin)
                continue;
            if (liveMs_ - e.clearedAtMs > kMemoryTtlMs)
            {
                e.used = false;   // expired: drop it while we are here
                continue;
            }
            best   = std::isnan (best) ? e.deepestDb : std::min (best, e.deepestDb);
            e.used = false;       // one use only (spec 4.6)
        }
    return best;
}

void NotchController::clearRoomMemoryLocked()
{
    for (auto& bank : roomMemory_)
        for (auto& e : bank)
            e = MemoryEntry {};
    roomMemoryHead_.fill (0);
}
```

Wipe it at the three boundaries. In `setWidth`, after the `blocksSinceReset` loop (line 115-116):

```cpp
    for (auto& l : lanes_)
        l.blocksSinceReset = 0;

    // Same boundary, same reason (spec 4.6): a restart means a different
    // device, rate or room, and a remembered depth from the old one would be
    // applied to a bin that now means a different frequency.
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        clearRoomMemoryLocked();
    }
```

In `clearAll` (lines 223-229), inside the existing lock:

```cpp
void NotchController::clearAll (ClearReason reason)
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    for (int c = 0; c < kChannels; ++c)
        for (int i = 0; i < kSlots; ++i)
            pushClearLocked (c, i, reason);
    // CLEAR ALL is the operator saying "forget everything you think you know
    // about this room" -- leaving the memory would have the next howl come
    // back deep-notched immediately.
    clearRoomMemoryLocked();
}
```

In `setSampleRate`, after the `for (auto& l : lanes_)` loop closes (m-B: the
pre-Task-5 range is `864-877`, but Tasks 5, 6 and 7 all insert above it — anchor on
`void NotchController::setSampleRate (double sampleRate)` and on the
`l.blocksSinceReset = 0;` that ends the loop body):

```cpp
    // m-7: this has no production caller today (the device path reaches the
    // controller through setWidth), but the wipe belongs here for the day it
    // does -- and for tools/snapshot.cpp and the tests, which do call it.
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        clearRoomMemoryLocked();
    }
```

In `placeConfirmed`, fill in the hook Task 5 left inside the index-lookup lock (B-2 — Task 5 already moved this block above the depth choice, so `remembered` is in scope by the time the depth choice reads it; the v1 plan had them the other way round and would not have compiled):

```cpp
    int index = -1, firstLane = lane, lastLane = lane;
    double remembered = std::numeric_limits<double>::quiet_NaN();
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        if (linkedNow) { index = firstFreeIndexAllLanesLocked(); firstLane = 0; lastLane = width_ - 1; }
        else           { index = firstFreeIndexLocked (lane); }
        // Looked up only when a placement is actually going to happen: the
        // entry is CONSUMED by the lookup, and spending it on a placement that
        // then bails on a full chain would lose the room's history for nothing.
        if (index >= 0)
            remembered = takeRememberedDepthLocked (lane, cand.frequencyHz,
                                                    pc.sampleRate / (double) Detector::kFftSize,
                                                    linkedNow);
    }
    if (index < 0)
        return;   // chain full on the lanes concerned: same outcome as today
```

Then insert step 3 of §4.3 into Task 5's depth choice, at the marker it left. **The Soundcheck line stays LAST** — moving it above step 3, or dropping it as the v1 plan's replacement block did, reds `SoundcheckPlacesAtTheFullSliderDepth` and `SoundcheckNotchesNeverDeepen`. The final shape of the whole block:

```cpp
    double depthDb = kDepthLadderDb[0];                       // step 1: -6
    if (pc.breakdown.riseRatio >= kSteepRiseRatio)            // step 2: steep -> -12
        depthDb = kDepthLadderDb[1];
    // step 3 (spec 4.6): this bin howled before, recently, and we know what it
    // took. Skip the crawl.
    if (! std::isnan (remembered))
        depthDb = remembered;

    // step 4: never deeper than the ceiling, which under Q13 IS the deepest
    // rung. This is also the ONLY thing done to a remembered depth on read
    // (m-8): an entry may legitimately hold an off-rung value -- a Manual -9
    // that auto-released -- and max() clamps it to the current ceiling without
    // any rung quantisation. Snapping it to a rung would either throw away
    // depth the room needed or place DEEPER than the notch ever ran.
    depthDb = std::max (depthDb, ceiling);

    // KD-7, LAST: soundcheck has no ladder and no room memory.
    if (origin == Origin::Soundcheck)
        depthDb = ceiling;
```

Finally, in Task 7's release path, record before the Clear:

```cpp
                else
                {
                    // What this bin needed, before the record of it goes away
                    // with the notch (spec 4.6). ONLY on an AutoRelease: a
                    // manual clear, a CLEAR ALL, a width change, a FALSE
                    // verdict or an unwind are all statements that this notch
                    // should not have been there.
                    rememberReleaseLocked (c, n.frequency, n.deepestDb, effectiveLinked());
                    pushClearLocked (c, i, ClearReason::AutoRelease);
                }
```

- [ ] **Step 5: Run the controller tests**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchController --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (520).

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp
```
```bash
git commit -m "feat(controller): room memory -- a howl returning to the same bin skips the crawl"
```

---

