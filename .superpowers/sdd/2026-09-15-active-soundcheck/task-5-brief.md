### Task 5: `AudioEngine` — a capture hoist, three insertion points, seven atomics, one capture ring

**Mức level dự kiến (spec §3) — the biggest level change this project has made.** On the channel named by `scOutChannel_`, while a run is in flight: **every lane routed to that channel is muted** (it carries the sweep and nothing else), the sweep's peak is **−20 dBFS** with an RMS near −23 dBFS, and the channel is **completely silent** during the 0.5 s noise floor, the 0.7 s tail and the 0.3 s gap. That is **4.5 s of silence per output channel**, and up to **~72 s** for 8 stereo slots. Every other output channel: **0 dB, unchanged.** When idle: **0 dB and no lane muted.** There is no limiter in this app; from here outward the sweep meets exactly one hard clamp, `±1.0f` (`src/app/AudioEngine.cpp:631-647`), and the real loudness in the room is set by the operator's master fader, not by the app.

After this task the engine *can* emit a sweep but nothing drives it: `SoundcheckController` arrives in Task 6. **Do not release from this commit.**

**Files:**
- Modify: `src/app/AudioEngine.h` — the cross-thread block (anchor: `// Cross-thread state. std::atomic keeps the audio callback lock-free`, `:329-341`); new public accessors and the test seam near `getTapDropCount` (anchor: `std::uint64_t getTapDropCount (int slot, int lane) const;`, `:196`)
- Modify: `src/app/AudioEngine.cpp`:
  - the snapshot, **including the sample rate** (anchor: `const bool bypass = (currentMode_.load (std::memory_order_acquire) == Mode::Bypass);` `:514`) — I-9
  - the **capture hoist**, immediately after the snapshot's bounds check — B-6 moved this OUT of the lane loop
  - insertion 2 (the mute) inside the lane-table loop (anchor: `tapSource[slot][lane] = out;` `:561`)
  - insertion 3 between the end of the DSP block and the clamp (anchors: the closing `}` of the `else` branch at `:624`, and `// Final output guard: every sample actually handed to the driver` `:626`)
  - insertion 4 in the tap loop (anchor: `for (int slot = 0; slot < kMaxSlots; ++slot)` at `:657`)
  - `micCapture_.clear()` in the existing drain block (anchor: `for (auto& slotTaps : tapBuffers_)` … `tap.clear();`, `:729-731`)
- Test: `tests/test_audioengine.cpp` (append; the file's `CallbackDriver` helper at `:36-61` is the shape to reuse — extend it rather than writing a second driver)

**Interfaces:**
- Consumes: `SoundcheckSignal` (Task 1) — `AudioEngine.cpp` gains `#include "dsp/SoundcheckSignal.h"`.
- Produces, on `AudioEngine`:
  ```cpp
  // Message thread. All of these are plain atomic stores; none restarts the device.
  void setSoundcheckOutputChannel  (int channel);          // -1 = off
  void setSoundcheckCaptureChannel (int channel);          // -1 = off
  void setSoundcheckCaptureActive  (bool active);
  void setSoundcheckTapsSuspended  (bool suspended);
  void setSoundcheckSampleIndex    (std::int64_t n);       // negative = noise floor
  void setSoundcheckPeak           (float peak);           // CLAMPED to kSoundcheckMaxPeak here
  void requestSoundcheckRampOut();                         // anchors at the CURRENT sample index
  [[nodiscard]] int          getSoundcheckOutputChannel() const;
  [[nodiscard]] std::int64_t getSoundcheckSampleIndex()   const;
  [[nodiscard]] bool         soundcheckIsEmitting()       const;   // scOutChannel_ >= 0

  LockFreeRingBuffer<float>& getMicCaptureBuffer();        // read() only, one consumer
  [[nodiscard]] std::uint64_t getMicCaptureDropCount() const;

  // TEST SEAMS ONLY.
  //
  // setRunningForTest (B-4): isRunning_ is set ONLY by start()
  // (AudioEngine.cpp:86) and audioDeviceAboutToStart() does not touch it
  // (:686-739), so no headless test can reach a state Preflight accepts.
  void setRunningForTest (bool running);
  //
  // setSoundcheckGainUnclampedForTest (B-5): multiplied into the injected
  // sample at point 3, AFTER SoundcheckSignal has clamped. Plan rev 1 put this
  // seam at the PEAK instead, which achieved nothing: the SoundcheckSignal
  // constructor clamps the peak and sampleAt clamps the sample, so |v| <= 0.1
  // whatever the setter was handed, and the clamp test could never go red.
  // This is the only route by which the +-1.0f output clamp can be shown to
  // still cover the sweep path (F17, inv 4). Default 1.0f in every shipping
  // path.
  void setSoundcheckGainUnclampedForTest (float gain);
  ```

**The insertion points, in callback order.**

1. **Capture the raw mic — HOISTED OUT of the lane loop, beside the bounds check.** **B-6 is a production defect in plan rev 1, not a test defect.** Rev 1 set `capSource` *inside* the per-lane loop, so the mic was captured only when an **enabled, correctly-routed lane** happened to read from `scCaptureInChannel`. A measurement mic is normally **not in the routing table at all** — that is the whole point of Q13's "raw mic, before any DSP" — so the common case captured **nothing**, and the lane M thread would have seen an empty ring, declared every channel "không đo được", and nobody would have known why. It also made `NoiseFloorCapturesWithoutEmitting` and `DeviceRestartDrainsTheCaptureRing` impossible to pass.

   The capture channel is independent of the routing table, so the pointer is taken from `inputChannelData` directly, once, next to the bounds check. See Step 5.
2. **Mute every lane routed to the measured channel — inside the lane loop** (`:558-561`). The condition is **`outIdx == scOutChannel`**, not "(slot, lane) matches" (F2). A muted lane does **not** enter the `lanes[]` table and does **not** set `tapSource`. The channel is still cleared at `:570-577` like every other channel.
3. **Inject the sweep and generate the ramp-out — between `:624` (end of the DSP block) and `:631` (the clamp).** The position is **mandatory**: after the clamp, the sweep would reach the driver unclamped. The block writes `out[n] += ...` into **exactly one** channel. For each sample `n`: if `scRampOutAt >= 0`, multiply by `SoundcheckSignal::rampOut(idx, scRampOutAt, R)`; when the ramp has run out, **the callback itself** stores `scOutChannel_ = -1` and `scRampOutAtSample_ = -1` (F8). At the end of the block, `scSampleIndex_ += numSamples`.
4. **Suspend tap writes for the whole run — the tap loop** (`:657-683`). The key is **`scSuspendTaps`**, not `scOutChannel` (N3): `scOutChannel_` returns to −1 at **every** Gap, so keying on it would un-suspend the taps for 300 ms between channels, restarting the ~420 ms `tapAlive` window **per channel** — ≈ 0.72 s each, **≈ 11.5 s over 16 channels**, which exceeds `kReleaseStepMs = 10 s` and would silently walk every releasing notch down a rung while spec §3 declares 0 dB. With `scSuspendTaps_` held for the whole run the true statement is: **one run advances lane G's release clock by at most ~0.42 s, once** — far under the cheapest 10 s rung. When suspended, skip the entire tap-write loop (a **skip**, not a drop: `tapDropCounts_` must not move, or every reader of the log sees a false positive) and instead write `micCapture_.write (capSource, numSamples)` when `scCaptureActive`.

**Snapshot ONCE, beside `bypass`** (F4, inv 7). All seven atomics — **and the sample rate, and the test gain seam** (I-9, B-5) — are read exactly once, at `:509-514`, into stack locals, and only the locals are used afterwards. Plan rev 1 read `currentSampleRate_` down at insertion point 3, which contradicted the snapshot block's own "nothing below this point reads the atomics again": a rate change landing between the two reads would build the sweep with one `T` and index it with another. The comment already in place at that spot says why: *"The mode AND the mapping are snapshotted ONCE at the top of the block"*. Spec rev 1 read them at three different points; a flip between point 3 and point 4 gives a callback that **both injects the sweep and writes the tap** — precisely the detector-poisoning case this lane exists to avoid.

**Bounds-check every callback** (F3, inv 3). `scOutChannel` and `scCaptureInChannel` are re-checked against **this callback's** `numOutputChannels` / `numInputChannels`, the same way the lane loop already checks every channel index at `:546-548`. A device restart onto fewer channels without this step is an **out-of-bounds write on the realtime thread**. An invalid index means this callback behaves as if no soundcheck were running; the lane M thread notices the missing data / changed counts and aborts (Task 6).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_audioengine.cpp`. First extend the existing helpers in that file's anonymous namespace (it closes at `tests/test_audioengine.cpp:62` — put these **before** that line, beside `CallbackDriver`):

```cpp
// Like CallbackDriver but with a configurable channel count and per-channel
// input, so a test can prove that only ONE output channel changed.
struct MultiDriver
{
    MultiDriver (int channels, int numSamples, float inputLevel = 0.25f)
        : frames (numSamples)
    {
        in.assign  ((std::size_t) channels, std::vector<float> ((std::size_t) numSamples, inputLevel));
        out.assign ((std::size_t) channels, std::vector<float> ((std::size_t) numSamples, 0.0f));
        for (auto& v : in)  inPtr.push_back (v.data());
        for (auto& v : out) outPtr.push_back (v.data());
    }

    void operator() (AudioEngine& engine)
    {
        for (auto& v : out) std::fill (v.begin(), v.end(), 0.0f);
        const juce::AudioIODeviceCallbackContext context {};
        engine.audioDeviceIOCallbackWithContext (inPtr.data(), (int) inPtr.size(),
                                                 outPtr.data(), (int) outPtr.size(),
                                                 frames, context);
    }

    float peakOn (int channel) const
    {
        float m = 0.0f;
        for (float v : out[(std::size_t) channel]) m = std::max (m, std::abs (v));
        return m;
    }

    std::vector<std::vector<float>> in, out;
    std::vector<const float*> inPtr;
    std::vector<float*>       outPtr;
    int frames;
};

// Slot `s` enabled, mono, inCh -> outCh.
//
// SlotConfig's field names are taken from the two places the app already builds
// one: AudioEngine.cpp:530-544 (the callback reading them) and
// MainComponent.cpp:807-827 (changeSlotConfig). Open src/app/SlotConfig.h before
// writing this helper and use whatever is actually declared there.
void routeMono (AudioEngine& engine, int s, int inCh, int outCh)
{
    SlotConfig c;
    c.enabled = true;
    c.width   = 1;
    c.inputChannels[0]  = inCh;
    c.outputChannels[0] = outCh;
    engine.setSlotConfig (s, c);
}
```

Then the tests:

```cpp
// RED IF: the mute condition is written as "(slot, lane) matches" instead of
// "outIdx == scOutChannel_". Several slots SUM onto one output channel
// (AudioEngine.cpp:565-577 clears, :621 accumulates), so muting one pair leaves
// the feedback loop through that channel CLOSED -- spec rev 1's blocker F2.
// inv 8.
TEST (AudioEngineSoundcheck, SweptChannelCarriesOnlyTheSweep)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);              // B-4
    engine.setMode (AudioEngine::Mode::Bypass);   // Bypass copies in->out: loudest case

    routeMono (engine, 0, 0, 1);
    routeMono (engine, 1, 2, 1);   // a SECOND slot onto the same output channel

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 4, 256, 0.5f };
    d (engine);

    SoundcheckSignal::Params p;
    p.sampleRate = engine.getCurrentSampleRateHz();
    p.peak       = SoundcheckSignal::kSoundcheckMaxPeak;
    const SoundcheckSignal expected { p };

    for (int n = 0; n < 256; ++n)
        ASSERT_NEAR (d.out[1][(std::size_t) n], expected.sampleAt (n), 1.0e-6f)
            << "sample " << n << " carries something other than the sweep";
}

// RED IF: the injection point is moved BELOW the output clamp at
// AudioEngine.cpp:631-647.
//
// B-5: the seam must sit PAST SoundcheckSignal, not before it. Plan rev 1 used a
// seam on the PEAK that skipped only the setter's clamp -- but the
// SoundcheckSignal constructor clamps the peak and sampleAt clamps the sample,
// so |v| <= 0.1 whatever the setter was handed, sawSomething was never set, and
// the test asserted nothing at all. The gain seam multiplies the ALREADY-CLAMPED
// sample at point 3, which is the only way to put something over full scale in
// front of the output clamp. inv 4, F17.
TEST (AudioEngineSoundcheck, OutputClampStillCoversTheSweepPath)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckGainUnclampedForTest (50.0f);   // 0.1 * 50 = 5x full scale
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 2, 512 };
    d (engine);

    bool sawSomething = false;
    for (float v : d.out[1])
    {
        ASSERT_LE (std::abs (v), 1.0f) << "a sample escaped the +-1.0f clamp";
        ASSERT_TRUE (std::isfinite (v));
        if (std::abs (v) > 0.5f) sawSomething = true;
    }
    EXPECT_TRUE (sawSomething)
        << "the seam produced nothing over 0.5 -- the test proves nothing, and "
           "that is exactly what plan rev 1's peak seam did";
}

// RED IF: any other output channel receives a sample from the soundcheck path.
// Spec rev 2 dropped this test when it was rewritten; it is the ONLY test for
// invariant 5. N6.
TEST (AudioEngineSoundcheck, SweepTouchesOnlyTheMeasuredChannel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    engine.setMode (AudioEngine::Mode::Bypass);

    for (int ch = 0; ch < 4; ++ch)
        routeMono (engine, ch, ch, ch);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (2);

    MultiDriver d { 4, 256, 0.25f };
    d (engine);

    // Channels 0, 1, 3 still pass their own input through, untouched.
    for (int ch : { 0, 1, 3 })
        for (int n = 0; n < 256; ++n)
            ASSERT_FLOAT_EQ (d.out[(std::size_t) ch][(std::size_t) n], 0.25f)
                << "channel " << ch << " sample " << n;
}

// RED IF: the ramp-out is driven from the controller thread instead of being
// generated inside the callback. Nothing but the callback runs here -- no
// controller, no poll -- and the sweep must still reach exactly zero and release
// the channel on its own. inv 9, F8.
TEST (AudioEngineSoundcheck, AbortRampsDownInTheCallbackAlone)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (4800);         // mid-sweep
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 2, 64 };
    d (engine);
    ASSERT_GT (d.peakOn (1), 0.0f);

    engine.requestSoundcheckRampOut();

    // kRampOutMs = 30 ms; at 48 kHz that is 1440 samples = 23 callbacks of 64.
    for (int i = 0; i < 40; ++i)
        d (engine);

    EXPECT_FLOAT_EQ (d.peakOn (1), 0.0f);
    EXPECT_EQ (engine.getSoundcheckOutputChannel(), -1)
        << "the callback must release the channel itself";
    EXPECT_FALSE (engine.soundcheckIsEmitting());
}

// RED IF: tap suspension is keyed on scOutChannel_ rather than on
// scSuspendTaps_. scOutChannel_ goes to -1 at every Gap, so the taps would come
// back for 300 ms between channels, restarting lane G's ~420 ms tapAlive window
// per channel: ~0.72 s each, ~11.5 s over 16 channels, past kReleaseStepMs =
// 10 s. inv 10, N3.
TEST (AudioEngineSoundcheck, TapsStaySuspendedAcrossTheGap)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (0);

    MultiDriver d { 2, 256 };
    for (int i = 0; i < 8; ++i) d (engine);

    // The Gap: the channel is released, the suspend flag is NOT.
    engine.setSoundcheckOutputChannel (-1);
    const auto before = engine.getTapBuffer (0, 0).getAvailableRead();
    for (int i = 0; i < 64; ++i) d (engine);    // 64 x 256 = 16384 samples ~ 341 ms > kGapMs
    EXPECT_EQ (engine.getTapBuffer (0, 0).getAvailableRead(), before)
        << "a tap was written during the Gap";

    EXPECT_EQ (engine.getTapDropCount (0, 0), 0u)
        << "a SKIP is not a DROP -- tapDropCounts_ must not move";
}

// RED IF: capture is gated on scOutChannel_ instead of on its own flag, or the
// noise-floor phase emits. The NoiseFloor phase has a live channel, capture on,
// and a NEGATIVE sample index. inv 6, F5.
TEST (AudioEngineSoundcheck, NoiseFloorCapturesWithoutEmitting)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckCaptureChannel (1);
    engine.setSoundcheckCaptureActive (true);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (-24000);          // 0.5 s before the sweep

    MultiDriver d { 2, 256, 0.3f };
    d (engine);

    EXPECT_EQ (d.peakOn (0), 0.0f) << "the noise-floor phase emitted";
    EXPECT_GE (engine.getMicCaptureBuffer().getAvailableRead(), 256u);

    // And with capture off, nothing arrives even though the channel is live.
    engine.setSoundcheckCaptureActive (false);
    const auto have = engine.getMicCaptureBuffer().getAvailableRead();
    d (engine);
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), have);
}

// RED IF: the per-callback bounds check is dropped. A device restart onto fewer
// channels would then be an out-of-bounds write on the realtime thread. inv 3,
// F3.
TEST (AudioEngineSoundcheck, OutOfRangeChannelIsIgnored)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckCaptureActive (true);

    MultiDriver d { 2, 128 };

    engine.setSoundcheckOutputChannel (2);       // == numOutputChannels
    engine.setSoundcheckCaptureChannel (9);
    d (engine);                                  // must not crash, must not write
    EXPECT_EQ (d.peakOn (0), 0.25f);             // slot 0 still passes through
    EXPECT_EQ (d.peakOn (1), 0.0f);

    engine.setSoundcheckOutputChannel (-5);
    d (engine);
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), 0u);
}

// RED IF: the atomics are read at more than one point in the callback. A flip
// between the mute decision and the tap decision produces a callback that BOTH
// injects the sweep AND taps it into the detector -- the poisoning case this
// lane exists to prevent. inv 7, F4.
TEST (AudioEngineSoundcheck, AtomicsAreSnapshottedOnce)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);

    MultiDriver d { 2, 64 };

    // 200 callbacks while another thread flips the channel and the suspend flag
    // as fast as it can. Each callback must be internally consistent: if it
    // emitted, it must NOT have tapped, and vice versa.
    std::atomic<bool> stop { false };
    std::thread flipper ([&engine, &stop]
    {
        while (! stop.load())
        {
            engine.setSoundcheckOutputChannel (0);
            engine.setSoundcheckTapsSuspended (true);
            engine.setSoundcheckOutputChannel (-1);
            engine.setSoundcheckTapsSuspended (false);
        }
    });

    for (int i = 0; i < 200; ++i)
    {
        const auto tapBefore = engine.getTapBuffer (0, 0).getAvailableRead();
        d (engine);
        const auto tapAfter  = engine.getTapBuffer (0, 0).getAvailableRead();
        const bool tapped    = tapAfter != tapBefore;
        const bool emitted   = d.peakOn (0) > 0.0f && d.peakOn (0) != 0.25f;

        ASSERT_FALSE (tapped && emitted)
            << "callback " << i << " both injected and tapped -- the atomics were re-read";
        engine.getTapBuffer (0, 0).clear();
    }

    stop.store (true);
    flipper.join();
}

// RED IF: an idle engine emits anything, OR mutes a lane. The second half had no
// test at all until spec rev 3. inv 6.
TEST (AudioEngineSoundcheck, IdleEngineEmitsNoSweepAndMutesNoLane)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    engine.setMode (AudioEngine::Mode::Bypass);
    routeMono (engine, 0, 0, 0);
    routeMono (engine, 1, 1, 1);

    MultiDriver d { 2, 256, 0.4f };
    d (engine);

    // Both lanes contributed normally; nothing was muted and nothing injected.
    for (int ch : { 0, 1 })
        for (int n = 0; n < 256; ++n)
            ASSERT_FLOAT_EQ (d.out[(std::size_t) ch][(std::size_t) n], 0.4f) << "ch " << ch;

    EXPECT_EQ (engine.getSoundcheckOutputChannel(), -1);
    EXPECT_FALSE (engine.soundcheckIsEmitting());
}

// RED IF: micCapture_.clear() is left out of audioDeviceAboutToStart's drain
// block. Audio captured at the PREVIOUS device's sample rate would be spliced
// onto the front of the next run's first analysis windows -- the same defect the
// tap rings are already cleared to avoid (AudioEngine.cpp:713-731). F15.
TEST (AudioEngineSoundcheck, DeviceRestartDrainsTheCaptureRing)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckCaptureChannel (1);
    engine.setSoundcheckCaptureActive (true);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (-1000);

    MultiDriver d { 2, 256, 0.3f };
    d (engine);
    ASSERT_GT (engine.getMicCaptureBuffer().getAvailableRead(), 0u);

    engine.audioDeviceAboutToStart (nullptr);
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), 0u);
}
```

`tests/test_audioengine.cpp` will need `#include "dsp/SoundcheckSignal.h"`, `<thread>` and `<atomic>` added to its include block (`tests/test_audioengine.cpp:17-26`) — m-21. `<algorithm>`, `<cmath>` and `<vector>` are already there.

- [ ] **Step 2: Run and watch every new test fail**

```bash
cmake --build build --config Release
```
Expected: compile errors — `setSoundcheckOutputChannel` and friends are not members of `AudioEngine`.

- [ ] **Step 3: Declare the state in `src/app/AudioEngine.h`**

Add the seven atomics, the ring, the counter and the test gain seam exactly as the Global Constraints section spells them, immediately after the existing `numInputChannels_` / `numOutputChannels_` pair (`:340-341`). Add the accessors listed in Interfaces near `getTapDropCount` (`:194-196`), each with a one-line thread comment.

**`dsp/SoundcheckSignal.h` is included from `AudioEngine.cpp` only, never from `AudioEngine.h`** (m-20). The header needs nothing from it: the atomics are plain types and `kCaptureCapacity` is a plain constant. Only the `.cpp` calls `SoundcheckSignal::clampPeak` and builds the signal at point 3, and keeping the include out of the header stops a DSP header from riding into every TU that already pulls `juce_audio_devices`.

`setSoundcheckPeak` clamps at the setter:

```cpp
void AudioEngine::setSoundcheckPeak (float peak)
{
    // Clamped HERE and again inside SoundcheckSignal::sampleAt. Two clamps for
    // one value is deliberate: this number has two routes in (this setter and
    // the test seam below), and a value with two routes to becoming wrong needs
    // both clamped -- one clamp is half a clamp (lane G M-B).
    scPeak_.store (SoundcheckSignal::clampPeak (peak), std::memory_order_relaxed);
}

void AudioEngine::setSoundcheckGainUnclampedForTest (float gain)
{
    // TEST SEAM ONLY (B-5, F17). Applied at point 3 to the sample AFTER
    // SoundcheckSignal has clamped it, because a seam on the PEAK achieves
    // nothing -- the signal's constructor and sampleAt both clamp, so the
    // amplitude is already bounded twice before the +-1.0f output clamp and
    // invariant 4 could not be turned red by any test. Same shape as lane G's
    // setRingRiskOverrideForTest, which also lives in the shipping build.
    scGainUnclampedForTest_.store (gain, std::memory_order_relaxed);
}

void AudioEngine::setRunningForTest (bool running)
{
    // TEST SEAM ONLY (B-4). isRunning_ is otherwise written only by start()
    // (:86) and audioDeviceError() (:753); audioDeviceAboutToStart() does not
    // touch it, so a headless test can never reach a state SoundcheckController
    // ::preflight accepts. It stores the same atomic start() stores.
    isRunning_.store (running, std::memory_order_release);
}
```

`requestSoundcheckRampOut()` stores the CURRENT `scSampleIndex_` into `scRampOutAtSample_` — the anchor is a sample index, not a time, so the callback can evaluate the envelope with no clock.

- [ ] **Step 4: The snapshot, beside `bypass`**

At `src/app/AudioEngine.cpp:514`, directly under the existing `const bool bypass = ...` line and under its existing comment:

```cpp
    // Lane M (spec §4.1, invariant 7): NINE values are read HERE, once -- the
    // seven soundcheck atomics, the sample rate (I-9) and the test-only gain
    // seam (B-5) -- for the same reason the mode and the mapping are: a flip
    // mid-callback between the mute decision (point 2) and the tap decision
    // (point 4) would produce a block that BOTH injects the sweep and taps it
    // back into the detector.
    //
    // Nothing below this point reads ANY of the nine again. That includes
    // currentSampleRate_, which plan rev 1 re-read down at point 3 (I-9): a rate
    // change landing between the two reads would build the sweep with one T and
    // index it with another.
    const int          scOutChannel      = scOutChannel_.load       (std::memory_order_relaxed);
    const bool         scSuspendTaps     = scSuspendTaps_.load      (std::memory_order_relaxed);
    const int          scCaptureIn       = scCaptureInChannel_.load (std::memory_order_relaxed);
    const bool         scCaptureActive   = scCaptureActive_.load    (std::memory_order_relaxed);
    const std::int64_t scSampleIndex     = scSampleIndex_.load      (std::memory_order_relaxed);
    const float        scPeak            = scPeak_.load             (std::memory_order_relaxed);
    const std::int64_t scRampOutAt       = scRampOutAtSample_.load  (std::memory_order_relaxed);
    // I-9: the sample rate belongs in the SAME snapshot. Reading it down at
    // point 3 would mean a rate change landing between the two reads builds the
    // sweep with one T and indexes it with another.
    const double       scSampleRate      = currentSampleRate_.load   (std::memory_order_relaxed);
    // B-5, TEST SEAM. 1.0f in every shipping path.
    const float        scGainUnclamped   = scGainUnclampedForTest_.load (std::memory_order_relaxed);

    // Invariant 3: re-checked against THIS callback's counts, exactly as the
    // lane loop re-checks every channel index at :546-548. A device restart onto
    // fewer channels without this is an out-of-bounds write on the audio thread.
    const int  scOut   = (scOutChannel >= 0 && scOutChannel < numOutputChannels
                          && outputChannelData != nullptr) ? scOutChannel : -1;
    const int  scInCh  = (scCaptureIn  >= 0 && scCaptureIn  < numInputChannels
                          && inputChannelData  != nullptr) ? scCaptureIn  : -1;
```

- [ ] **Step 5: the capture hoist (point 1), and the mute (point 2)**

**Point 1 goes immediately after the snapshot's bounds check, OUTSIDE the lane loop** (B-6):

```cpp
    // Point 1 (Q13, B-6): the RAW mic, before any DSP, taken straight off the
    // callback's input pointers.
    //
    // This must NOT live inside the lane loop. A measurement mic is normally not
    // in the routing table at all -- that is what "raw mic" means -- so a capture
    // that only fired when an ENABLED, correctly-routed lane happened to read
    // from scCaptureInChannel_ would capture NOTHING in the common case, and the
    // lane M thread would report every channel as "could not measure" with no
    // clue why. scInCh is already bounds-checked against THIS callback's
    // numInputChannels (invariant 3).
    const float* capSource = (scInCh >= 0) ? inputChannelData[scInCh] : nullptr;
```

**Point 2 stays inside the lane loop**, after the existing bounds check at `:546-548` and before the `in`/`out` pointers are taken:

```cpp
            // Point 2 (Q15 relitigated, F2): mute EVERY lane routed to the
            // channel being measured -- not one (slot, lane) pair. Several slots
            // sum onto one output channel (:565-577 clears, :621 accumulates),
            // so muting a pair leaves that channel's feedback loop CLOSED. A
            // muted lane enters neither lanes[] nor tapSource.
            if (scOut >= 0 && outIdx == scOut)
                continue;
```

The `continue` must come **before** `lanes[numLanes++] = ...` at `:558` and before `tapSource[slot][lane] = out;` at `:561`.

- [ ] **Step 6: Insertion point 3 — the sweep, before the clamp**

Between the closing brace of the `else` DSP block (`:624`) and the `// Final output guard` comment (`:626`):

```cpp
    // Lane M point 3 (spec §4.1). MUST stay ABOVE the +-kMaxOutputLevel clamp
    // below: past it, the sweep would reach the driver unclamped (invariant 4).
    // Writes exactly ONE channel (invariant 5), accumulating like the DSP above.
    if (scOut >= 0)
    {
        SoundcheckSignal::Params params;
        params.sampleRate = scSampleRate;           // I-9: from the snapshot, not re-read
        params.peak       = scPeak;
        const SoundcheckSignal signal { params };   // stack, no allocation, one std::log

        const std::int64_t rampLen = SoundcheckSignal::rampOutSamples (scSampleRate);
        float* out = outputChannelData[scOut];

        if (out != nullptr)
        {
            for (int n = 0; n < numSamples; ++n)
            {
                const std::int64_t idx = scSampleIndex + n;
                float v = signal.sampleAt (idx);            // 0 while idx < 0 (NoiseFloor)
                if (scRampOutAt >= 0)
                    v *= SoundcheckSignal::rampOut (idx, scRampOutAt, rampLen);
                // B-5: 1.0f in every shipping path. The ONLY route past the
                // signal's own clamps, and it exists so the +-1.0f output clamp
                // below can be shown to still cover this path.
                out[n] += v * scGainUnclamped;
            }
        }

        scSampleIndex_.store (scSampleIndex + numSamples, std::memory_order_relaxed);

        // The callback ENDS the run itself once the ramp-out has reached zero
        // (F8, invariant 9): no other thread needs to still be alive for the
        // sound to stop.
        if (scRampOutAt >= 0 && scSampleIndex + numSamples >= scRampOutAt + rampLen)
        {
            scOutChannel_.store      (-1, std::memory_order_relaxed);
            scRampOutAtSample_.store (-1, std::memory_order_relaxed);
        }
    }
```

- [ ] **Step 7: Insertion point 4 — the tap loop**

Replace the head of the tap loop at `:657-659` with:

```cpp
    // Lane M point 4 (spec §4.1, invariant 10). Keyed on scSuspendTaps_ and NOT
    // on scOutChannel_: that index returns to -1 at every Gap, so keying on it
    // un-suspends the taps for 300 ms between channels and restarts lane G's
    // ~420 ms tapAlive window PER CHANNEL -- ~11.5 s over 16 channels, past
    // kReleaseStepMs = 10 s (N3). Held for the whole run, the true figure is
    // ~0.42 s once.
    //
    // This is a SKIP, not a drop: tapDropCounts_ must not move, or every reader
    // of the session log sees a burst of false positives that never happened.
    if (scSuspendTaps)
    {
        if (scCaptureActive && capSource != nullptr)
        {
            const std::size_t requested = (std::size_t) numSamples;
            const std::size_t written   = micCapture_.write (capSource, requested);
            if (written < requested)
                micCaptureDrops_.fetch_add (requested - written, std::memory_order_relaxed);
        }
    }
    else
    for (int slot = 0; slot < kMaxSlots; ++slot)
    for (int lane = 0; lane < kMaxSlotLanes; ++lane)
    {
        ... unchanged ...
    }
```

- [ ] **Step 8: `micCapture_.clear()` in the restart drain**

In `audioDeviceAboutToStart`, immediately after the tap-ring drain loop (anchor: `for (auto& slotTaps : tapBuffers_)` … `tap.clear();`, `:729-731`):

```cpp
    // Lane M: the same precondition and the same reason. Capture taken at the
    // PREVIOUS device's rate would be spliced onto the front of the next run's
    // first analysis windows, and every bin-to-Hz conversion would be wrong.
    // MainComponent aborts and joins the SoundcheckController before a restart
    // reaches here (spec §4.3), so neither producer nor consumer is running.
    micCapture_.clear();
```

- [ ] **Step 9: Build and run the suite**

`AudioEngine.h` changed ⇒ full reconfigure.

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R AudioEngine --output-on-failure
```
Expected: `100% tests passed` — the 37 pre-existing `AudioEngine` tests plus **10** new (m-13 recounted this; rev 1 said 11).

- [ ] **Step 10: The invariant-16 read, by a human**

No test can catch a violation here (spec §4.11, §5.2). Before committing, re-read the whole soundcheck path inside `audioDeviceIOCallbackWithContext` and confirm, line by line:

> no `lock`, no allocation (no `new`, no growing container, no `juce::String`), no logging, and nothing touched outside **the seven atomics + the sample rate + the test gain seam + `micCaptureDrops_` + `micCapture_`**.

The `SoundcheckSignal` constructed on the stack at point 3 is part of this read: it must have no member that allocates. Paste the conclusion into the commit message. **This line belongs in the reviewer brief of every task that touches `AudioEngine`.**

- [ ] **Step 11: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (587)` — 577 + 10. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/AudioEngine.h src/app/AudioEngine.cpp tests/test_audioengine.cpp
```
```bash
git commit -m "feat(lane-m): AudioEngine emits the soundcheck sweep -- mute by output channel, capture ring, callback-owned ramp-out"
```

---

