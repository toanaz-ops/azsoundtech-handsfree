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
