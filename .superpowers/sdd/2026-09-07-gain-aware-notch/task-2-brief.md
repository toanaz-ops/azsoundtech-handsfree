### Task 2: `NotchChain::setNotch` routes a depth-only change to the ramp

**Mức level dự kiến (spec §3):** still **0 dB** in the shipped app — `NotchController` does not send a second `Set` onto a live index until Task 6. What this task fixes is the mechanism: from here on, a `Set` with the same freq and Q on an Active slot changes the cut over 10 ms instead of resetting the filter. The measured attenuation at f0 after the ramp completes is the requested rung within **±0.5 dB** at −6 / −12 / −18 / −24 (asserted in the test below).

**Files:**
- Modify: `src/dsp/NotchChain.h:24-56` (include, `kRampMs`, `setNotch` doc)
- Modify: `src/dsp/NotchChain.cpp:1-53` (include, `setNotch`)
- Test: `tests/test_notchchain.cpp` (append after `TEST(NotchChain, SetNotchRejectsAPositiveDepthAndLeavesTheSlotIdle)` at line 440)

**Interfaces:**
- Consumes (Task 1): `bool Biquad::rampNotchDepth(double freq, double Q, double sampleRate, double depthDB, int rampSamples)`.
- Produces:
  ```cpp
  static constexpr double NotchChain::kRampMs = 10.0;
  // void NotchChain::setNotch(int index, double freq, double Q, double depthDB)
  //   -- unchanged signature; NEW behaviour when the slot is Active and both
  //      freq and Q compare equal to the stored NotchInfo.
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchchain.cpp`. `sineWave` (line 20) and `rms` (line 31) already exist there.

```cpp
// RED IF a depth-only setNotch on a running slot goes back through
// setNotchFilter (which resets the state). After a reset, feeding 0.0 into a
// charged chain returns EXACTLY 0.0; after a ramp it does not.
TEST(NotchChain, DepthOnlyRetuneOfARunningNotchKeepsTheFilterState)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);

    const auto tone = sineWave(1000.0, 48000.0, 4800);
    for (int i = 0; i < 3600; ++i)
        chain.processSample(tone[static_cast<std::size_t>(i)]);

    chain.setNotch(0, 1000.0, 30.0, -12.0);   // same freq, same Q: ramp
    EXPECT_NE(chain.processSample(0.0), 0.0) << "state was cleared: this was a reset, not a ramp";

    // NotchInfo reports the TARGET immediately, not the ramp's position.
    EXPECT_DOUBLE_EQ(chain.getNotchInfo(0).depthDB, -12.0);
    EXPECT_EQ(chain.getNotchInfo(0).state, NotchChain::NotchState::Active);
}

// RED IF a frequency change stops taking the reset path.
TEST(NotchChain, ChangingFrequencyStillResetsTheFilterState)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);

    const auto tone = sineWave(1000.0, 48000.0, 4800);
    for (int i = 0; i < 3600; ++i)
        chain.processSample(tone[static_cast<std::size_t>(i)]);

    chain.setNotch(0, 1500.0, 30.0, -6.0);            // different freq
    EXPECT_DOUBLE_EQ(chain.processSample(0.0), 0.0);  // state cleared
    EXPECT_DOUBLE_EQ(chain.getNotchInfo(0).frequency, 1500.0);
}

// RED IF a Q change stops taking the reset path. m-2: the controller must
// resend the STORED Q for exactly this reason -- one bit of difference and the
// retune becomes a reset, i.e. a click.
TEST(NotchChain, ChangingQStillResetsTheFilterState)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);

    const auto tone = sineWave(1000.0, 48000.0, 4800);
    for (int i = 0; i < 3600; ++i)
        chain.processSample(tone[static_cast<std::size_t>(i)]);

    chain.setNotch(0, 1000.0, 30.000000001, -6.0);
    EXPECT_DOUBLE_EQ(chain.processSample(0.0), 0.0);
}

// RED IF an Idle slot stops taking the reset path (it has no state worth
// keeping, and its stored NotchInfo may name a different design entirely).
TEST(NotchChain, SetNotchOnAnIdleSlotStillTakesTheResetPath)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);
    const auto tone = sineWave(1000.0, 48000.0, 4800);
    for (int i = 0; i < 3600; ++i)
        chain.processSample(tone[static_cast<std::size_t>(i)]);

    chain.clearNotch(0);
    chain.setNotch(0, 1000.0, 30.0, -12.0);   // same params, but the slot was Idle
    EXPECT_DOUBLE_EQ(chain.processSample(0.0), 0.0);
}

// The measured level, rung by rung (CLAUDE.md: state the level change, and
// prove it in a test). RED IF a ramped retune lands anywhere but the rung it
// was asked for. 0.5 dB is the tolerance the spec 5.2 names.
TEST(NotchChain, MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel)
{
    const double ladder[] = { -6.0, -12.0, -18.0, -24.0 };
    const int    rampSamples = static_cast<int>(0.010 * 48000.0);   // kRampMs

    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -6.0);

    const auto tone = sineWave(1000.0, 48000.0, 96000);
    for (double rung : ladder)
    {
        chain.setNotch(0, 1000.0, 30.0, rung);

        // Let the ramp finish and the filter settle at the new design before
        // measuring: a Q of 30 at 1 kHz rings for ~10 ms on its own.
        std::vector<double> out(48000);
        for (int i = 0; i < 48000; ++i)
            out[static_cast<std::size_t>(i)] = chain.processSample(tone[static_cast<std::size_t>(i)]);
        ASSERT_GT(48000, rampSamples);

        const double measuredDb = 20.0 * std::log10(rms(out, 24000)
                                                    / rms(std::vector<double>(tone.begin(),
                                                                              tone.begin() + 48000), 24000));
        EXPECT_NEAR(measuredDb, rung, 0.5) << "rung " << rung << " dB";
    }
}

// RED IF a ramped retune can be half-applied. A rejected design must leave the
// slot -- coefficients, state and reported depth -- exactly as it was.
TEST(NotchChain, RejectedDepthOnARunningNotchLeavesTheSlotUnchanged)
{
    NotchChain chain(48000.0);
    chain.setNotch(0, 1000.0, 30.0, -12.0);
    chain.setNotch(0, 1000.0, 30.0, +3.0);   // boost: refused by the biquad
    EXPECT_DOUBLE_EQ(chain.getNotchInfo(0).depthDB, -12.0);
    EXPECT_EQ(chain.getNotchInfo(0).state, NotchChain::NotchState::Active);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchChain --output-on-failure`
Expected: FAIL — `DepthOnlyRetuneOfARunningNotchKeepsTheFilterState` reports `chain.processSample(0.0)` equal to 0 (today's `setNotch` always resets).

- [ ] **Step 3: Extend `src/dsp/NotchChain.h`**

Add after `static constexpr int MAX_NOTCHES = 16;` (line 33):

```cpp
    // Depth-only retune ramp (spec 4.7, decision Q5). 10 ms is one block at a
    // 2048 buffer and seven at 64, so it is always at least one callback and
    // never long enough to be heard as a slew. 6 dB over 10 ms is 0.6 dB/ms --
    // the invariant the controller's ladder is written against.
    static constexpr double kRampMs = 10.0;
```

Replace the `setNotch` doc comment (lines 53-56) with:

```cpp
    // Installs a notch in `index`. If the biquad rejects the parameters
    // (sampleRate <= 0, Q <= 0, freq <= 0, or freq >= sampleRate/2 -- see
    // Biquad.h) the slot is left untouched and stays whatever it was.
    //
    // DEPTH-ONLY RETUNE (spec 4.7): when the slot is already Active and both
    // `freq` and `Q` compare EQUAL to the stored NotchInfo, only the depth is
    // moving, so the filter state is still meaningful and clearing it would be
    // a step discontinuity into the PA. That case routes to
    // Biquad::rampNotchDepth and interpolates over kRampMs instead. Everything
    // else -- an Idle slot, a new frequency, a new Q -- keeps taking the
    // setNotchFilter + reset path exactly as before.
    //
    // The comparison is a plain `==` on doubles ON PURPOSE: the caller
    // (NotchController::pushRetuneLocked) resends the freq and Q it stored
    // when the notch was placed, so the values are bit-identical by
    // construction. A near-miss falls through to the reset path, which is the
    // SAFE direction to fail in -- an audible click, never a filter running
    // one design while claiming another.
    void   setNotch(int index, double freq, double Q, double depthDB);
```

- [ ] **Step 4: Implement in `src/dsp/NotchChain.cpp`**

Add the include at the top (after line 1):

```cpp
#include "dsp/NotchChain.h"

#include <cmath>
```

Replace `setNotch` (lines 25-53) — keep the whole existing comment block, and insert the ramp branch before the reset path:

```cpp
void NotchChain::setNotch(int index, double freq, double Q, double depthDB)
{
    if (index < 0 || index >= MAX_NOTCHES)
    {
        return;
    }

    // Depth-only retune of a RUNNING notch: keep the state, ramp the
    // coefficients (see the header). rampSamples is rounded from kRampMs
    // against the chain's CURRENT rate, so the ramp is 10 ms of real time at
    // any device rate. A rejected design leaves the slot completely alone,
    // exactly like the path below.
    if (notchInfo_[index].state == NotchState::Active
        && freq == notchInfo_[index].frequency
        && Q    == notchInfo_[index].Q)
    {
        const int rampSamples = (int) std::lround(kRampMs * sampleRate_ / 1000.0);
        if (! filters_[index].rampNotchDepth(freq, Q, sampleRate_, depthDB, rampSamples))
        {
            return;
        }
        // The reported depth is the TARGET, from the instant the command is
        // accepted -- the model, the GUI and the preset all describe intent,
        // not the ramp's momentary position (spec 4.7).
        notchInfo_[index].depthDB = depthDB;
        return;
    }

    // Configure the biquad for this notch, DEPTH INCLUDED. `depthDB` used to
    // be recorded here and dropped on the floor -- the three-argument
    // setNotchFilter is an infinite-depth null and has no depth parameter --
    // so every notch this app placed was a full null no matter what was asked
    // for, and spec 5.1's 6-24 dB range was unreachable.
    //
    // If the biquad rejects the parameters (see Biquad.h) the slot is left
    // COMPLETELY untouched. Activating it anyway would mark the slot Active
    // while filters_[index] still held whatever design was there before, so
    // the chain would report a notch at one frequency and filter another.
    // A positive depthDB is among the rejected cases: it would boost the
    // ringing frequency instead of cutting it.
    if (! filters_[index].setNotchFilter(freq, Q, sampleRate_, depthDB))
    {
        return;
    }

    notchInfo_[index].frequency = freq;
    notchInfo_[index].Q         = Q;
    notchInfo_[index].depthDB   = depthDB;
    notchInfo_[index].state     = NotchState::Active;
}
```

- [ ] **Step 5: Run the NotchChain tests**

Run:
```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release -R NotchChain --output-on-failure
```
Expected: PASS. Record the four measured attenuations printed by a failure-free run of `MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel` — they are the numbers the release note quotes.

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (469).

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/dsp/NotchChain.h src/dsp/NotchChain.cpp tests/test_notchchain.cpp
```
```bash
git commit -m "feat(dsp): NotchChain ramps a depth-only retune over kRampMs instead of resetting"
```

---

