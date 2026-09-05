### Task 3: `NotchController` — `ClearReason`, `NotchEvent`, `EventSink`, every clear path, widen-reset, skipped count

**Files:**
- Modify: `src/app/NotchController.h` (public API after `clearAll`, private members), `src/app/NotchController.cpp:44-60` (setWidth), `:70-133` (setNotch/pushClearLocked/clearNotch/clearAll), `:135-161` (adoptPreset), `:279-296` (auto-release + flush), `:529-532` (unwind in placeConfirmed), `:688-710` (after flushOutbox)
- Modify: `src/app/MainComponent.cpp:105-110` only if it fails to compile (defaults keep the existing calls valid)
- Test: `tests/test_notchcontroller.cpp` (append)

**Interfaces:**
- Consumes: nothing new (Task 4 adds the scored `Set`).
- Produces:
  ```cpp
  enum class ClearReason : std::uint8_t { Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind };

  struct SpectralContext            // filled by Task 4 only
  {
      int    bins = Detector::kNumBins;
      double binHz = 0.0;
      double refAgeMs = 0.0;
      bool   hasRef = false, hasOther = false;
      std::array<float, Detector::kNumBins> now {}, ref {}, other {};
  };

  struct NotchEvent
  {
      enum class Kind : std::uint8_t { Set, Clear };
      Kind  kind = Kind::Set;
      int   slot = 0, lane = 0, index = 0;
      float hz = 0.0f, q = 0.0f, depthDb = 0.0f;
      Origin      origin = Origin::Detector;        // Set
      ClearReason reason = ClearReason::Manual;     // Clear
      double ageMs = 0.0;                           // Clear: liveMs_ - lockedAtMs
      // Detector placements only (Task 4):
      bool  hasScore = false;
      int   confirmedLane = 0;
      float score = 0.0f, peakiness = 0.0f, pNorm = 0.0f, rise = 0.0f, novelty = 0.0f,
            penalty = 1.0f, asymmetry = 1.0f, thr = 0.0f;
      int   persistNeeded = 0;
      std::shared_ptr<const SpectralContext> ctx;   // null unless hasScore
  };
  using EventSink = std::function<void (const NotchEvent&)>;

  static constexpr int kMaxPendingEvents = 64;
  void setEventSink (EventSink sink);              // detector thread STOPPED; nullptr = off
  void clearNotch (int channel, int index, ClearReason reason = ClearReason::Manual);
  void clearAll (ClearReason reason = ClearReason::ClearAll);
  int  adoptPreset (const std::vector<PresetNotch>& notches, int* skippedOut = nullptr);
  void stop (int timeoutMs);                       // now ALSO flushes the event outbox after the join
  std::uint64_t droppedEvents() const;
  // TEST ACCESSORS ONLY
  int  pendingEventsForTest() const;
  bool modelMutexIsFreeForTest();                  // try_lock + unlock
  void failNextSetNotchOnLaneForTest (int lane);   // -1 = off
  ```
  Private: `bool setNotchImpl (int channel, int index, double f, double Q, double d, Origin, const NotchEvent* scored)`; `void pushClearLocked (int channel, int index, ClearReason)`; `void pushEventLocked (NotchEvent&&)`; `void flushEventOutbox()`; members `EventSink eventSink_; std::vector<NotchEvent> eventOutbox_, eventScratch_; std::atomic<std::uint64_t> droppedEvents_; std::atomic<int> failSetNotchLaneForTest_ { -1 };`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_notchcontroller.cpp` (after the stereo section; the helpers `Harness`, `StereoHarness`, `pump`, `pumpStereo`, `SineSource`, `NoiseSource`, `drain`, `kWarmupBlocks` all exist above):

```cpp
// ===========================================================================
// Lane D (data loop): ClearReason on every clear path, NotchEvent sink.
// ===========================================================================
namespace {
using Ev = NotchController::NotchEvent;

struct Recorder
{
    std::vector<Ev> events;
    NotchController::EventSink sink()
    {
        return [this] (const Ev& e) { events.push_back (e); };
    }
    std::vector<Ev> clears() const
    {
        std::vector<Ev> out;
        for (const auto& e : events)
            if (e.kind == Ev::Kind::Clear)
                out.push_back (e);
        return out;
    }
};
} // namespace

// Spec test 8, six reasons. Red if any clear path stops carrying its reason.
TEST (NotchControllerEvents, EveryClearPathCarriesItsReason)
{
    // Manual (default), VerdictFalse, ClearAll -- one Harness.
    {
        Harness h; Recorder r; h.controller.setEventSink (r.sink());
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
        ASSERT_TRUE (h.controller.setNotch (0, 1, 2000.0, 30.0, -12.0, NotchController::Origin::Manual));
        ASSERT_TRUE (h.controller.setNotch (0, 2, 3000.0, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.clearNotch (0, 0);
        h.controller.clearNotch (0, 1, NotchController::ClearReason::VerdictFalse);
        h.controller.clearAll();
        h.controller.runOnce();
        const auto c = r.clears();
        ASSERT_EQ (c.size(), 3u);
        EXPECT_EQ (c[0].reason, NotchController::ClearReason::Manual);       EXPECT_EQ (c[0].index, 0);
        EXPECT_EQ (c[1].reason, NotchController::ClearReason::VerdictFalse); EXPECT_EQ (c[1].index, 1);
        EXPECT_EQ (c[2].reason, NotchController::ClearReason::ClearAll);     EXPECT_EQ (c[2].index, 2);
        EXPECT_FLOAT_EQ (c[2].hz, 3000.0f);
        // Set events preceded them, one per setNotch, no score (manual origin).
        int sets = 0;
        for (const auto& e : r.events) if (e.kind == Ev::Kind::Set) { ++sets; EXPECT_FALSE (e.hasScore); EXPECT_EQ (e.origin, NotchController::Origin::Manual); }
        EXPECT_EQ (sets, 3);
    }
    // AutoRelease: mirror LiveTapReleasesAfter30s (this file, ~line 151).
    {
        Harness h; Recorder r; h.controller.setEventSink (r.sink());
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
        NoiseSource quiet;
        const int blocks = (int) (NotchController::kAutoReleaseMs / kBlockMs) + 10;
        for (int i = 0; i < blocks; ++i)
            pump (h, quiet.hop());
        const auto c = r.clears();
        ASSERT_EQ (c.size(), 1u);
        EXPECT_EQ (c[0].reason, NotchController::ClearReason::AutoRelease);
        EXPECT_GT (c[0].ageMs, NotchController::kAutoReleaseMs);
    }
    // WidthChange: lane-1 notch, narrow to mono.
    {
        StereoHarness h; Recorder r; h.controller.setEventSink (r.sink());
        ASSERT_TRUE (h.controller.setNotch (1, 2, 800.0, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.setWidth (1);
        h.controller.runOnce();
        const auto c = r.clears();
        ASSERT_EQ (c.size(), 1u);
        EXPECT_EQ (c[0].reason, NotchController::ClearReason::WidthChange);
        EXPECT_EQ (c[0].lane, 1); EXPECT_EQ (c[0].index, 2);
    }
    // PartialApplyUnwind: adoptPreset with lane -1 on a stereo slot, lane 1 forced to fail.
    {
        StereoHarness h; Recorder r; h.controller.setEventSink (r.sink());
        h.controller.failNextSetNotchOnLaneForTest (1);
        EXPECT_EQ (h.controller.adoptPreset (onePresetNotch (4, 1000.0)), 0);
        h.controller.runOnce();
        const auto c = r.clears();
        ASSERT_EQ (c.size(), 1u);
        EXPECT_EQ (c[0].reason, NotchController::ClearReason::PartialApplyUnwind);
        EXPECT_EQ (c[0].lane, 0); EXPECT_EQ (c[0].index, 4);
        NotchController::SnapshotBuffer snap; h.controller.copySnapshot (snap);
        EXPECT_EQ (snap.notchCount, 0u);
    }
}

// Spec test 9. Red if the sink is ever invoked with modelMutex_ held: the
// re-entrant clearNotch below would deadlock, and the try_lock would fail.
TEST (NotchControllerEvents, SinkRunsOutsideTheModelMutexAndMayReenter)
{
    Harness h;
    bool mutexWasFree = false;
    int  calls = 0;
    h.controller.setEventSink ([&] (const Ev& e)
    {
        ++calls;
        mutexWasFree = h.controller.modelMutexIsFreeForTest();
        if (e.kind == Ev::Kind::Set && e.index == 0)
            h.controller.clearNotch (0, 1);   // re-entrant policy call from inside the sink
    });
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    ASSERT_TRUE (h.controller.setNotch (0, 1, 2000.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.runOnce();   // delivers the two Sets; the re-entrant clear lands in the outbox
    h.controller.runOnce();   // delivers that Clear
    EXPECT_TRUE (mutexWasFree);
    EXPECT_EQ (calls, 3);
}

// Spec test 10. Red if events accumulate with no sink attached.
TEST (NotchControllerEvents, NoSinkMeansNoAccumulation)
{
    Harness h;
    for (int i = 0; i < 200; ++i)
    {
        ASSERT_TRUE (h.controller.setNotch (0, i % 16, 1000.0 + i, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.clearNotch (0, i % 16);
    }
    h.controller.runOnce();
    EXPECT_EQ (h.controller.pendingEventsForTest(), 0);
    EXPECT_EQ (h.controller.droppedEvents(), 0u);
}

// Cap. Red if the outbox grows past kMaxPendingEvents instead of dropping
// and counting.
TEST (NotchControllerEvents, OutboxDropsAndCountsPastTheCap)
{
    Harness h; Recorder r; h.controller.setEventSink (r.sink());
    for (int i = 0; i < 100; ++i)
    {
        ASSERT_TRUE (h.controller.setNotch (0, i % 16, 1000.0 + i, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.clearNotch (0, i % 16);
    }
    EXPECT_EQ (h.controller.pendingEventsForTest(), NotchController::kMaxPendingEvents);
    EXPECT_EQ (h.controller.droppedEvents(), 200u - (std::uint64_t) NotchController::kMaxPendingEvents);
    h.controller.runOnce();
    EXPECT_EQ (r.events.size(), (std::size_t) NotchController::kMaxPendingEvents);
}

// Spec test 11. Red if stop() stops flushing what is still queued.
TEST (NotchControllerEvents, StopFlushesPendingEventsEvenWhenTheThreadNeverRan)
{
    Harness h; Recorder r; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.stop (1000);
    ASSERT_EQ (r.events.size(), 1u);
    EXPECT_EQ (r.events[0].kind, Ev::Kind::Set);
}

// Lane S loose end (A-9). Red if widening 1 -> 2 leaves lane 1's analysis
// window holding audio from before it went mono: one silent hop after the
// widen must publish a near-silent lane-1 spectrum, not 1536 stale samples
// of tone.
TEST (NotchControllerSlotAware, WideningResetsLaneOneDetectorState)
{
    StereoHarness h;
    SineSource tone; NoiseSource quiet;
    for (int i = 0; i < 8; ++i)
        pumpStereo (h, quiet.hop(), tone.hop());   // lane 1 window full of 1 kHz
    h.controller.setWidth (1);
    h.controller.setWidth (2);

    std::vector<float> silence ((std::size_t) Detector::kHopSize, 0.0f);
    pumpStereo (h, silence, silence);

    NotchController::SnapshotBuffer snap; h.controller.copySnapshot (snap);
    ASSERT_EQ (snap.laneCount, 2u);
    float peak = 0.0f;
    for (int b = 0; b < Detector::kNumBins; ++b)
        peak = std::max (peak, snap.magnitudes[1][(std::size_t) b]);
    // A window of pure tone gives bin 43 ~ 0.5 (amp 1, Hann); 512 zeros in a
    // 2048 window still leaves ~0.37. A reset window gives exactly 0.
    EXPECT_LT (peak, 1.0e-3f);
}

// Lane S loose end (A-9). Red if a lane-1 notch skipped on a mono slot is
// not counted.
TEST (NotchControllerPreset, AdoptPresetCountsLaneOneNotchesSkippedOnAMonoSlot)
{
    StereoHarness h;
    h.controller.setWidth (1);
    PresetNotch onLaneOne; onLaneOne.index = 0; onLaneOne.freq = 1000.0; onLaneOne.Q = 30.0; onLaneOne.depthDB = -12.0; onLaneOne.lane = 1;
    PresetNotch onLaneZero = onLaneOne; onLaneZero.index = 1; onLaneZero.lane = 0;
    int skipped = -1;
    EXPECT_EQ (h.controller.adoptPreset ({ onLaneOne, onLaneZero }, &skipped), 1);
    EXPECT_EQ (skipped, 1);
}
```

If `onePresetNotch` (line ~47) leaves `lane` at its default, confirm `PresetNotch::lane` defaults to `-1` in `PresetManager.h` before relying on it; the unwind test needs both lanes attempted.

- [ ] **Step 2: Run to verify failure** — `cmake --build build --config Release`. Expected: compile errors (`ClearReason`, `NotchEvent`, `setEventSink` undeclared).

- [ ] **Step 3: Header changes** — in `NotchController.h`, after `enum class Origin` add:

```cpp
    // Lane D (data loop). Why a notch left the model -- the session log's
    // training label depends on it, so the two internal unwind sites MUST say
    // PartialApplyUnwind rather than hide behind the Manual default.
    enum class ClearReason : std::uint8_t
    {
        Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind
    };
```

After `clearAll()` add the event API block (copy the structs and declarations from the Interfaces section above, with these comments):

```cpp
    // Lane D (data loop): one event per notch set / clear, delivered to the
    // sink from the DETECTOR thread by flushEventOutbox(), never under
    // modelMutex_ (D-6) and never from the audio thread. Events queue in
    // eventOutbox_ (cap kMaxPendingEvents, then drop + count) and go out at
    // the end of every runOnce() and once more from stop(). With no sink the
    // outbox is emptied, never grown. setEventSink() requires the thread to
    // be STOPPED, like setWidth().
```

Change the two signatures:

```cpp
    void clearNotch (int channel, int index, ClearReason reason = ClearReason::Manual);
    void clearAll (ClearReason reason = ClearReason::ClearAll);
    // `skippedOut` (optional) receives the number of notches this slot could
    // not take at all -- today only a lane-1 notch on a mono slot.
    int adoptPreset (const std::vector<PresetNotch>& notches, int* skippedOut = nullptr);
```

Private additions (next to `pushClearLocked`):

```cpp
    bool setNotchImpl (int channel, int index, double frequency, double Q, double depthDB,
                       Origin origin, const NotchEvent* scored);
    void pushClearLocked (int channel, int index, ClearReason reason);
    void pushEventLocked (NotchEvent&& event);   // modelMutex_ HELD
    void flushEventOutbox();                     // modelMutex_ NOT held when the sink runs
```

and members (after `outbox_`):

```cpp
    // Lane D. eventOutbox_ under modelMutex_; eventScratch_ is detector-thread
    // only (flushEventOutbox swaps them so the reserve() survives).
    EventSink eventSink_;
    std::vector<NotchEvent> eventOutbox_;
    std::vector<NotchEvent> eventScratch_;
    std::atomic<std::uint64_t> droppedEvents_ { 0 };
    std::atomic<int> failSetNotchLaneForTest_ { -1 };
```

Add `#include <functional>` and `#include <memory>`.

- [ ] **Step 4: Implementation** — in `NotchController.cpp`:

Constructor: `eventOutbox_.reserve ((std::size_t) kMaxPendingEvents); eventScratch_.reserve ((std::size_t) kMaxPendingEvents);`

```cpp
void NotchController::stop (int timeoutMs)
{
    if (isThreadRunning())
        stopThread (timeoutMs);
    // Whatever queued since the last poll -- or ever, if the thread never
    // ran -- reaches the sink before this returns (spec test 11).
    flushEventOutbox();
}

void NotchController::setEventSink (EventSink sink)
{
    // Precondition: thread STOPPED (see header). Same contract as setWidth().
    eventSink_ = std::move (sink);
}

std::uint64_t NotchController::droppedEvents() const { return droppedEvents_.load (std::memory_order_relaxed); }

int NotchController::pendingEventsForTest() const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return (int) eventOutbox_.size();
}

bool NotchController::modelMutexIsFreeForTest()
{
    if (! modelMutex_.try_lock())
        return false;
    modelMutex_.unlock();
    return true;
}

void NotchController::failNextSetNotchOnLaneForTest (int lane)
{
    failSetNotchLaneForTest_.store (lane, std::memory_order_relaxed);
}
```

`setWidth`: keep the narrowing block, pass `ClearReason::WidthChange` to `pushClearLocked`, and add the widening branch before `width_ = newWidth;`:

```cpp
    if (newWidth > width_)
    {
        // A lane coming back into scope must not analyse the tail of what it
        // heard before it left: the analysis window and the candidate streaks
        // describe audio the operator stopped routing. Same discontinuity
        // rule Detector::reset() documents for a device restart.
        for (int c = width_; c < newWidth; ++c)
        {
            auto& la = lanes_[(std::size_t) c];
            la.detector.reset();
            la.persistence.fill (0);
            la.previousBlockNowMs = 0.0;
        }
    }
```

`setNotch` becomes a thin wrapper; the body moves to `setNotchImpl`, which after the model write also queues an event **inside the same lock**:

```cpp
bool NotchController::setNotch (int channel, int index, double frequency, double Q, double depthDB, Origin origin)
{
    return setNotchImpl (channel, index, frequency, Q, depthDB, origin, nullptr);
}

bool NotchController::setNotchImpl (int channel, int index, double frequency, double Q, double depthDB,
                                    Origin origin, const NotchEvent* scored)
{
    // ... the existing validation lines, unchanged ...
    if (failSetNotchLaneForTest_.load (std::memory_order_relaxed) == channel)
    {
        failSetNotchLaneForTest_.store (-1, std::memory_order_relaxed);
        return false;   // TEST ONLY: forces the partial-apply unwind path
    }
    // ... existing NotchCommand cmd ...
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        // ... existing model writes and outbox_.push_back (cmd) ...

        NotchEvent ev = scored != nullptr ? *scored : NotchEvent {};
        ev.kind = NotchEvent::Kind::Set;
        ev.slot = slotId_; ev.lane = channel; ev.index = index;
        ev.hz = (float) frequency; ev.q = (float) Q; ev.depthDb = (float) depthDB;
        ev.origin = origin;
        pushEventLocked (std::move (ev));
    }
    return true;
}

void NotchController::pushEventLocked (NotchEvent&& event)
{
    if (eventSink_ == nullptr)
        return;   // no sink: nothing is ever queued (spec test 10)
    if ((int) eventOutbox_.size() >= kMaxPendingEvents)
    {
        droppedEvents_.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    eventOutbox_.push_back (std::move (event));
}

void NotchController::pushClearLocked (int channel, int index, ClearReason reason)
{
    auto& n = model_[slotOf (channel, index)];
    if (! n.active)
        return;
    n.active = false;
    outbox_.push_back ({ NotchCommandType::Clear, (std::uint8_t) channel, (std::uint8_t) index,
                         0.0f, 0.0f, 0.0f, slotId_ });

    NotchEvent ev;
    ev.kind = NotchEvent::Kind::Clear;
    ev.slot = slotId_; ev.lane = channel; ev.index = index;
    ev.hz = (float) n.frequency; ev.q = (float) n.Q; ev.depthDb = (float) n.depthDB;
    ev.origin = n.origin; ev.reason = reason;
    ev.ageMs = liveMs_ - n.lockedAtMs;
    pushEventLocked (std::move (ev));
}
```

Reading `eventSink_` under `modelMutex_` on the message thread while the detector thread reads it in `flushEventOutbox` is a benign same-value read: it is only written with the thread stopped. State that in the header comment; do not add a second mutex.

`clearNotch`/`clearAll` forward `reason`. Auto-release passes `ClearReason::AutoRelease`. The two unwind sites — `adoptPreset` and `placeConfirmed` — call `clearNotch (lane, index, ClearReason::PartialApplyUnwind)`.

`adoptPreset` counts:

```cpp
int NotchController::adoptPreset (const std::vector<PresetNotch>& notches, int* skippedOut)
{
    int adopted = 0, skipped = 0;
    for (const auto& p : notches)
    {
        // ... existing firstLane/lastLane ...
        if (firstLane >= width_)
        {
            ++skipped;   // lane 1 named on a mono slot: not adoptable here
            continue;
        }
        // ... unchanged, with PartialApplyUnwind on the unwind ...
    }
    if (skippedOut != nullptr)
        *skippedOut = skipped;
    return adopted;
}
```

`runOnce` step 4: `flushOutbox(); flushEventOutbox();` and the new function:

```cpp
void NotchController::flushEventOutbox()
{
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        if (eventOutbox_.empty())
            return;
        eventScratch_.clear();
        eventScratch_.swap (eventOutbox_);   // both keep their reserve()
    }
    // Lock released: the sink may take its own locks, log, or call straight
    // back into clearNotch() (spec test 9). eventSink_ is only written with
    // the thread stopped, so reading it here is race-free.
    if (eventSink_ != nullptr)
        for (const auto& e : eventScratch_)
            eventSink_ (e);
    eventScratch_.clear();
}
```

Also drop `pushClearLocked`'s old two-argument declaration; the compiler will point at every stale call (`setWidth`, `clearAll`, auto-release).

- [ ] **Step 5: Build and run** — `cmake -B build -G "Visual Studio 18 2026" -A x64 && cmake --build build --config Release && cd build && ctest -C Release --output-on-failure -R NotchController`. Expected: every existing NotchController test still passes plus the 8 new ones. If `WideningResetsLaneOneDetectorState` fails on the threshold, print `peak` and check the reset actually zeroes the window (`Detector::reset()` at `Detector.cpp` — read it); do not loosen the bound past `1e-2f`.

- [ ] **Step 6: Full suite** — `ctest -C Release`: `100% tests passed` (416).

- [ ] **Step 7: Commit**

```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp
git commit -m "feat(controller): ClearReason on every clear path, NotchEvent sink flushed outside the model mutex"
```

---

