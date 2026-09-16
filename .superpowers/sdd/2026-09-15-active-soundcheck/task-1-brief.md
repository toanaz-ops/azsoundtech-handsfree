### Task 1: `SoundcheckSignal` — the sweep, the window, and the ramp-out, as pure functions of a sample index

**Mức level dự kiến (spec §3):** **0 dB.** This task adds an unreachable code path: nothing calls `SoundcheckSignal` until Task 5. The properties it must carry forward: `|x| <= kSoundcheckMaxPeak = 0.1f` (−20 dBFS) for **any** requested peak including NaN, and `x(0) == 0`, last-sample `== 0`, `x(n<0) == 0` so the noise-floor phase needs no separate "emitting" flag.

**Files:**
- Create: `src/dsp/SoundcheckSignal.h`, `src/dsp/SoundcheckSignal.cpp`
- Create: `tests/test_soundchecksignal.cpp`
- Modify: `CMakeLists.txt` — add both to `HANDSFREE_CORE_SOURCES` (anchor text: `set(HANDSFREE_CORE_SOURCES`, `CMakeLists.txt:84`; the paths there are ABSOLUTE, `${CMAKE_SOURCE_DIR}/src/...`, because `tools/` and `tests/` resolve relative paths against their own directory)
- Modify: `tests/CMakeLists.txt` — add `test_soundchecksignal.cpp` to the `add_executable(HandsFreeTests ...)` list (anchor text: `test_slotconfig.cpp`, `tests/CMakeLists.txt:20`)

**Interfaces:**
- Consumes: nothing. `<cmath>`, `<cstdint>`, `<algorithm>` only — no JUCE, no `src/app/`.
- Produces:
  ```cpp
  class SoundcheckSignal
  {
  public:
      static constexpr double kSweepLowHz       = 100.0;
      static constexpr double kSweepHighHz      = 10000.0;
      static constexpr double kSweepSeconds     = 3.0;
      static constexpr double kRampMs           = 30.0;
      static constexpr double kRampOutMs        = 30.0;
      static constexpr float  kSoundcheckMaxPeak = 0.1f;    // -20 dBFS
      static constexpr float  kSoundcheckMinPeak = 0.01f;   // -40 dBFS

      static float clampPeak (float requested) noexcept;

      struct Params
      {
          double sampleRate    = 48000.0;
          double lowHz         = kSweepLowHz;
          double highHz        = kSweepHighHz;
          double sweepSeconds  = kSweepSeconds;
          double rampMs        = kRampMs;
          float  peak          = kSoundcheckMaxPeak;
      };

      explicit SoundcheckSignal (const Params& p) noexcept;

      [[nodiscard]] float        sampleAt       (std::int64_t n) const noexcept;
      [[nodiscard]] double       instantaneousHz (std::int64_t n) const noexcept;
      [[nodiscard]] std::int64_t totalSamples()   const noexcept;
      [[nodiscard]] std::int64_t rampSamples()    const noexcept;
      [[nodiscard]] float        clampedPeak()    const noexcept;

      static float rampOut (std::int64_t n, std::int64_t anchor,
                            std::int64_t rampLengthSamples) noexcept;
      static std::int64_t rampOutSamples (double sampleRate) noexcept;
  };
  ```
  `sampleAt` returns `0.0f` for `n < 0` and for `n >= totalSamples()`. `rampOut` returns `1.0f` for `n < anchor`, the raised-cosine half-window for `anchor <= n < anchor + rampLengthSamples`, and **exactly** `0.0f` for `n >= anchor + rampLengthSamples`.

**Why the callback may evaluate this per sample.** `sampleAt` costs one `std::exp` and one `std::sin`. At the worst rate/buffer combination the app supports (192 kHz, 64-sample buffer) the callback budget is 333 µs and this adds 64 × (exp + sin) ≈ 64 × 40 ns ≈ **2.6 µs**, under 1 %. Constructing the object is one `std::log` plus arithmetic, once per callback on the stack — no allocation, no lock (inv 16).

- [ ] **Step 1: Write the failing tests**

Create `tests/test_soundchecksignal.cpp`:

```cpp
// tests/test_soundchecksignal.cpp
//
// SoundcheckSignal is the ONLY thing between "the app decided to measure" and a
// loudspeaker. Every test here is a safety test; none of them needs a device,
// a thread or a clock.
#include <gtest/gtest.h>

#include "dsp/SoundcheckSignal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

SoundcheckSignal::Params defaultParams (float peak = SoundcheckSignal::kSoundcheckMaxPeak)
{
    SoundcheckSignal::Params p;
    p.sampleRate = kSr;
    p.peak       = peak;
    return p;
}

// Zero crossings of the sweep over a short span, converted back to a frequency.
// Counting crossings is deliberately independent of instantaneousHz(): a test
// that asked the class for the answer it is checking proves nothing.
double measuredHzAround (const SoundcheckSignal& s, std::int64_t centre, std::int64_t span)
{
    int crossings = 0;
    float prev = s.sampleAt (centre - span / 2);
    for (std::int64_t n = centre - span / 2 + 1; n < centre + span / 2; ++n)
    {
        const float cur = s.sampleAt (n);
        if ((prev < 0.0f && cur >= 0.0f) || (prev >= 0.0f && cur < 0.0f))
            ++crossings;
        prev = cur;
    }
    return (crossings / 2.0) * kSr / (double) span;
}
} // namespace

// RED IF: clampPeak() stops clamping, or the clamp is applied only at the
// setter and not at the point of use (spec §3 safety table: "a value with two
// routes to becoming wrong needs BOTH clamped -- one clamp is half a clamp",
// lane G lesson M-B). inv 1.
TEST (SoundcheckSignal, PeakIsClampedWhateverIsAsked)
{
    for (float asked : { 1.0f, 10.0f, -5.0f, std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity() })
    {
        const SoundcheckSignal s { defaultParams (asked) };

        EXPECT_LE (s.clampedPeak(), SoundcheckSignal::kSoundcheckMaxPeak) << "asked " << asked;
        EXPECT_GE (s.clampedPeak(), 0.0f) << "asked " << asked;

        for (std::int64_t n = 0; n < s.totalSamples(); n += 97)
            ASSERT_LE (std::abs (s.sampleAt (n)), SoundcheckSignal::kSoundcheckMaxPeak)
                << "asked " << asked << " at n=" << n;
    }
}

// RED IF: the raised-cosine window is dropped, or it is applied as w(n)+something
// that does not vanish at the ends. A non-zero first sample IS a click into a PA.
// inv 2.
TEST (SoundcheckSignal, FirstAndLastSampleAreExactlyZero)
{
    const SoundcheckSignal s { defaultParams() };

    EXPECT_FLOAT_EQ (s.sampleAt (0), 0.0f);
    EXPECT_FLOAT_EQ (s.sampleAt (s.totalSamples() - 1), 0.0f);
}

// RED IF: the noise-floor phase is reintroduced as a separate "emitting" flag
// instead of a negative sample index. inv 2 / inv 6.
TEST (SoundcheckSignal, NegativeIndexReturnsZero)
{
    const SoundcheckSignal s { defaultParams() };

    for (std::int64_t n : { -1LL, -100LL, -24000LL, -(std::int64_t) (0.5 * kSr) })
        EXPECT_FLOAT_EQ (s.sampleAt (n), 0.0f) << "n=" << n;

    EXPECT_FLOAT_EQ (s.sampleAt (s.totalSamples()), 0.0f);
    EXPECT_FLOAT_EQ (s.sampleAt (s.totalSamples() + 4096), 0.0f);
}

// RED IF: the exponential phase term is written with the wrong sign or the wrong
// K, which produces a sweep that runs backwards or stops short of 10 kHz.
//
// B-8: an earlier draft asserted 100 Hz +- 5 and 10000 Hz +- 500 at the probe
// CENTRES, and the sweep cannot meet that. f(n) = 100 * 100^(n/T) with
// T = 3.0 * 48000 = 144000, so a probe centred at 2400 samples is already at
// 100 * 100^(2400/144000) = 107.98 Hz, and one centred near the end reads about
// 9263 Hz. Both are CORRECT for a log sweep; the tolerances were wrong.
//
// So the test compares each measurement against the closed form evaluated at
// THAT SAME centre, which pins the shape without pretending the endpoints are
// reachable by a finite probe. The endpoints themselves are pinned by
// instantaneousHz(0) and instantaneousHz(total), where no window is needed.
TEST (SoundcheckSignal, InstantaneousFrequencyIsMonotoneAndHitsBothEnds)
{
    const SoundcheckSignal s { defaultParams() };
    const std::int64_t total = s.totalSamples();

    // The closed form, written out here rather than asked of the class, so the
    // two are independent.
    auto closedForm = [total] (std::int64_t n)
    {
        return 100.0 * std::pow (100.0, (double) n / (double) total);
    };

    // 20 ms holds ~2 cycles at 100 Hz; 4 ms holds ~37 at 9 kHz.
    const std::int64_t spanA = (std::int64_t) (0.020 * kSr);
    const std::int64_t spanB = (std::int64_t) (0.004 * kSr);
    const std::int64_t cA    = spanA / 2 + 1;
    const std::int64_t cB    = total - spanB / 2 - 1;

    const double atStart = measuredHzAround (s, cA, spanA);
    const double atEnd   = measuredHzAround (s, cB, spanB);

    EXPECT_NEAR (atStart, closedForm (cA), closedForm (cA) * 0.05);
    EXPECT_NEAR (atEnd,   closedForm (cB), closedForm (cB) * 0.05);

    // The ends themselves, where no probe window is involved.
    EXPECT_NEAR (s.instantaneousHz (0),     100.0,   0.01);
    EXPECT_NEAR (s.instantaneousHz (total), 10000.0, 1.0);

    double previous = 0.0;
    for (std::int64_t n = 0; n < total; n += total / 64)
    {
        const double hz = s.instantaneousHz (n);
        ASSERT_GT (hz, previous) << "not monotone at n=" << n;
        previous = hz;
    }
}

// RED IF: the fade-in is made linear or shortened. The ramp must rise from 0 to
// the full peak envelope over exactly kRampMs and never overshoot.
TEST (SoundcheckSignal, RampIsMonotoneOverThirtyMilliseconds)
{
    const SoundcheckSignal s { defaultParams() };
    const std::int64_t ramp = s.rampSamples();

    EXPECT_EQ (ramp, (std::int64_t) (SoundcheckSignal::kRampMs * kSr / 1000.0));

    // The envelope is not directly observable, so probe it by the running peak
    // over one period of the (100 Hz, 480-sample) tone at each point.
    auto envelopeNear = [&s] (std::int64_t centre)
    {
        float m = 0.0f;
        for (std::int64_t n = centre; n < centre + 480; ++n)
            m = std::max (m, std::abs (s.sampleAt (n)));
        return m;
    };

    float previous = -1.0f;
    for (std::int64_t n = 0; n + 480 < ramp; n += 480)
    {
        const float e = envelopeNear (n);
        ASSERT_GE (e, previous) << "envelope fell during the fade-in at n=" << n;
        previous = e;
    }
}

// RED IF: the ramp-out reuses the fade-in window instead of being its own
// function (F8), or its last sample is "very small" rather than EXACTLY zero.
// The callback relies on reaching exactly 0 to know the ramp is over and to
// clear scOutChannel_ itself. inv 9.
TEST (SoundcheckSignal, RampOutIsMonotoneAndReachesExactlyZero)
{
    const std::int64_t r = SoundcheckSignal::rampOutSamples (kSr);
    EXPECT_EQ (r, (std::int64_t) (SoundcheckSignal::kRampOutMs * kSr / 1000.0));

    const std::int64_t anchor = 12345;

    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor - 1, anchor, r), 1.0f);
    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor,     anchor, r), 1.0f);

    float previous = 1.0f;
    for (std::int64_t n = anchor; n < anchor + r; ++n)
    {
        const float v = SoundcheckSignal::rampOut (n, anchor, r);
        ASSERT_LE (v, previous + 1.0e-6f) << "ramp-out rose at n=" << n;
        ASSERT_GE (v, 0.0f);
        previous = v;
    }

    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor + r,       anchor, r), 0.0f);
    EXPECT_FLOAT_EQ (SoundcheckSignal::rampOut (anchor + r + 999, anchor, r), 0.0f);
}

// RED IF: the two envelopes are multiplied in the wrong order or the ramp-out is
// applied to the raw sine instead of the already-windowed sample. This is the
// composition the callback performs, asserted here once so Task 5 only has to
// wire it.
TEST (SoundcheckSignal, RampOutMultipliesTheWindowedSampleAndNeverExceedsThePeak)
{
    const SoundcheckSignal s { defaultParams() };
    const std::int64_t r      = SoundcheckSignal::rampOutSamples (kSr);
    const std::int64_t anchor = s.totalSamples() / 2;

    for (std::int64_t n = anchor; n < anchor + r + 64; ++n)
    {
        const float v = s.sampleAt (n) * SoundcheckSignal::rampOut (n, anchor, r);
        ASSERT_LE (std::abs (v), SoundcheckSignal::kSoundcheckMaxPeak);
    }

    EXPECT_FLOAT_EQ (s.sampleAt (anchor + r + 10)
                         * SoundcheckSignal::rampOut (anchor + r + 10, anchor, r), 0.0f);
}
```

- [ ] **Step 2: Run the tests and watch them fail to COMPILE**

```bash
cmake --build build --config Release
```
Expected: `Cannot open include file: 'dsp/SoundcheckSignal.h'`.

- [ ] **Step 3: Write `src/dsp/SoundcheckSignal.h`**

```cpp
// src/dsp/SoundcheckSignal.h
//
// Lane M. The log sine sweep the active soundcheck plays, as a PURE function of
// a sample index -- no state advanced by anyone, no allocation, no JUCE. That
// shape is load-bearing three ways:
//
//   1. The audio callback can evaluate it directly, so the emergency ramp-out
//      needs no other thread to still be alive (spec §4.2, invariant 9).
//   2. The lane M thread can regenerate the EXACT sequence that was emitted, to
//      use as the reference spectrum X of the loop-gain estimate (spec §4.4).
//   3. A headless test can assert any individual sample.
//
// The noise-floor phase is expressed as a NEGATIVE sample index rather than as
// a separate "emitting" flag: sampleAt(n) returns 0 for n < 0, so one gate
// cannot disagree with the other (spec §4.2).
//
// SAFETY. This class is the last thing before a loudspeaker. The peak is
// clamped in clampPeak() and the clamped value is the ONLY one stored, so there
// is no route by which an unclamped number reaches sampleAt() -- a value with
// two routes to becoming wrong needs both clamped, and one clamp is half a
// clamp (lane G M-B). The +-1.0f output clamp in AudioEngine still sits AFTER
// the injection point and must never be removed (invariant 4).
//
// Deliberately in src/dsp/ with no juce_audio_devices and no src/app/ include:
// AudioEngine.cpp includes this header from inside the realtime callback, and
// app/SoundcheckController.h aliases its constants (see that header).
#pragma once

#include <cstdint>

class SoundcheckSignal
{
public:
    // Spec §4.10. These are the DEFINITIONS; SoundcheckController.h aliases
    // them so there is one place to look up a lane M constant and still only
    // one literal per value (lane G m-D).
    static constexpr double kSweepLowHz        = 100.0;
    static constexpr double kSweepHighHz       = 10000.0;
    static constexpr double kSweepSeconds      = 3.0;
    static constexpr double kRampMs            = 30.0;
    // A SEPARATE constant from kRampMs even though the value matches: the
    // ramp-out is a different function, anchored at an arbitrary sample the
    // callback is handed, and the two must be able to move apart (F8).
    static constexpr double kRampOutMs         = 30.0;
    static constexpr float  kSoundcheckMaxPeak = 0.1f;    // -20 dBFS (Q2)
    static constexpr float  kSoundcheckMinPeak = 0.01f;   // -40 dBFS (Q2)

    // [0, kSoundcheckMaxPeak]. NaN and infinity map to 0 (silence), not to the
    // maximum: any comparison with NaN picks an arbitrary branch, so the
    // non-finite case is decided explicitly, exactly as AudioEngine.cpp:642
    // decides it for the output clamp.
    static float clampPeak (float requested) noexcept;

    struct Params
    {
        double sampleRate   = 48000.0;
        double lowHz        = kSweepLowHz;
        double highHz       = kSweepHighHz;
        double sweepSeconds = kSweepSeconds;
        double rampMs       = kRampMs;
        float  peak         = kSoundcheckMaxPeak;
    };

    explicit SoundcheckSignal (const Params& p) noexcept;

    // 0 for n < 0 and for n >= totalSamples().
    [[nodiscard]] float sampleAt (std::int64_t n) const noexcept;

    // f0 * exp(K * n / T). Reported for the marker maths and the tests; the
    // sample generator does not call it.
    [[nodiscard]] double instantaneousHz (std::int64_t n) const noexcept;

    [[nodiscard]] std::int64_t totalSamples() const noexcept { return totalSamples_; }
    [[nodiscard]] std::int64_t rampSamples()  const noexcept { return rampSamples_; }
    [[nodiscard]] float        clampedPeak()  const noexcept { return peak_; }

    // Half a raised cosine, falling from 1 at `anchor` to EXACTLY 0 at
    // `anchor + rampLengthSamples` and staying there. 1 before the anchor.
    // Static because the callback applies it to a signal it may have built from
    // a different Params instance, and because reaching exactly 0 is what tells
    // the callback the ramp is finished (invariant 9).
    static float rampOut (std::int64_t n, std::int64_t anchor,
                          std::int64_t rampLengthSamples) noexcept;

    static std::int64_t rampOutSamples (double sampleRate) noexcept;

private:
    double       sampleRate_   = 48000.0;
    double       lowHz_        = kSweepLowHz;
    double       k_            = 0.0;   // ln(f1/f0)
    double       phaseScale_   = 0.0;   // 2*pi*f0*T_s/K
    std::int64_t totalSamples_ = 0;
    std::int64_t rampSamples_  = 0;
    float        peak_         = 0.0f;
};
```

- [ ] **Step 4: Write `src/dsp/SoundcheckSignal.cpp`**

```cpp
// src/dsp/SoundcheckSignal.cpp
#include "dsp/SoundcheckSignal.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kPi = 3.14159265358979323846;
}

float SoundcheckSignal::clampPeak (float requested) noexcept
{
    // Decided explicitly rather than by jlimit/std::clamp: both are undefined
    // for NaN inputs, and this value drives a loudspeaker.
    if (! std::isfinite (requested))
        return 0.0f;
    return std::min (std::max (requested, 0.0f), kSoundcheckMaxPeak);
}

SoundcheckSignal::SoundcheckSignal (const Params& p) noexcept
{
    sampleRate_ = (p.sampleRate > 0.0) ? p.sampleRate : 48000.0;
    lowHz_      = (p.lowHz > 0.0)      ? p.lowHz      : kSweepLowHz;

    const double highHz = (p.highHz > lowHz_) ? p.highHz : (lowHz_ * 2.0);
    const double tS     = (p.sweepSeconds > 0.0) ? p.sweepSeconds : kSweepSeconds;

    k_            = std::log (highHz / lowHz_);
    phaseScale_   = 2.0 * kPi * lowHz_ * tS / k_;
    totalSamples_ = (std::int64_t) (tS * sampleRate_);
    rampSamples_  = (std::int64_t) (std::max (0.0, p.rampMs) * sampleRate_ / 1000.0);

    // Never more than half the sweep at each end, or the two windows overlap
    // and the "last sample is exactly zero" property stops holding.
    rampSamples_ = std::min (rampSamples_, totalSamples_ / 2);

    peak_ = clampPeak (p.peak);
}

float SoundcheckSignal::sampleAt (std::int64_t n) const noexcept
{
    if (n < 0 || n >= totalSamples_)
        return 0.0f;

    const double t   = (double) n / (double) totalSamples_;
    const double phi = phaseScale_ * (std::exp (k_ * t) - 1.0);

    // Raised cosine at BOTH ends. w(0) == 0 and w(totalSamples_-1) == 0 by
    // construction: the denominator is rampSamples_, and the index that reaches
    // it is exactly the first sample of the flat middle.
    double w = 1.0;
    if (rampSamples_ > 0)
    {
        const std::int64_t fromStart = n;
        const std::int64_t fromEnd   = totalSamples_ - 1 - n;
        const std::int64_t edge      = std::min (fromStart, fromEnd);

        if (edge < rampSamples_)
            w = 0.5 * (1.0 - std::cos (kPi * (double) edge / (double) rampSamples_));
    }

    const double v = (double) peak_ * w * std::sin (phi);

    // The peak is already clamped, w is in [0,1] and |sin| <= 1, so this cannot
    // exceed kSoundcheckMaxPeak. The clamp is here anyway because it costs one
    // instruction and this is the sample that leaves for a loudspeaker.
    return (float) std::min (std::max (v, -(double) kSoundcheckMaxPeak),
                             (double) kSoundcheckMaxPeak);
}

double SoundcheckSignal::instantaneousHz (std::int64_t n) const noexcept
{
    if (totalSamples_ <= 0)
        return lowHz_;
    const double t = (double) n / (double) totalSamples_;
    return lowHz_ * std::exp (k_ * t);
}

std::int64_t SoundcheckSignal::rampOutSamples (double sampleRate) noexcept
{
    if (! (sampleRate > 0.0))
        return 0;
    return (std::int64_t) (kRampOutMs * sampleRate / 1000.0);
}

float SoundcheckSignal::rampOut (std::int64_t n, std::int64_t anchor,
                                 std::int64_t rampLengthSamples) noexcept
{
    if (n < anchor)
        return 1.0f;
    if (rampLengthSamples <= 0 || n >= anchor + rampLengthSamples)
        return 0.0f;   // EXACTLY zero -- the callback uses this to end the run

    const double x = (double) (n - anchor) / (double) rampLengthSamples;
    return (float) (0.5 * (1.0 + std::cos (kPi * x)));
}
```

- [ ] **Step 5: Add the files to both CMake lists**

In `CMakeLists.txt`, inside `set(HANDSFREE_CORE_SOURCES` (`CMakeLists.txt:84`), append beside the other `src/dsp` entries:

```cmake
    ${CMAKE_SOURCE_DIR}/src/dsp/SoundcheckSignal.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/SoundcheckSignal.h
```

In `tests/CMakeLists.txt`, inside `add_executable(HandsFreeTests` (`tests/CMakeLists.txt:19-44`), append:

```cmake
    test_soundchecksignal.cpp
```

- [ ] **Step 6: Reconfigure, build, run**

A new header and two `CMakeLists.txt` edits mean a **full reconfigure** (CLAUDE.md build table):

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R SoundcheckSignal --output-on-failure
```
Expected: `100% tests passed` (7 tests).

- [ ] **Step 7: Full gate**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (554)` — 547 + 7. ESTIMATE; use what ctest prints.

- [ ] **Step 8: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/dsp/SoundcheckSignal.h src/dsp/SoundcheckSignal.cpp tests/test_soundchecksignal.cpp CMakeLists.txt tests/CMakeLists.txt
```
```bash
git commit -m "feat(lane-m): SoundcheckSignal -- clamped log sweep and ramp-out as pure functions"
```

---

