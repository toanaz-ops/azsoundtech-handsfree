// PeakinessAnalyzer: spectral peakiness scoring for feedback detection (Task 11).
//
// Consumes a Detector::Spectrum and reports the bins that look like a howl:
// narrow peaks standing well above their immediate spectral surroundings.
//
// The metric (spec 5.2 step 4, as amended)
// ========================================
//   peakiness = bin_mag / mean(bins at offsets -5,-4,-3, +3,+4,+5)
//
// That is an ANNULUS, not a solid neighbourhood: offsets 0, +-1 and +-2 are
// skipped because they are the tone's own main lobe, and the centre bin is
// excluded for the additional reason that including it would drag every value
// toward 1 and flatten the dynamic range the threshold relies on.
//
// Why the annulus, and why the spec's +-2 was wrong
// =================================================
// Detector applies a Hann window before its 2048-point FFT, and a Hann main
// lobe is FOUR bins wide regardless of N. Measured shape of an on-bin tone,
// normalised to its peak (JUCE symmetric Hann, N = 1024 at the time -- see
// the 2026-08-24 NOTE below):
//
//   offset:   -2       -1        0       +1       +2
//   value:  0.0003   0.5007   1.0000   0.5007   0.0003
//
// Bins i+-1 carry HALF the peak -- they ARE the tone. Averaging over a solid
// +-2 neighbourhood therefore measures the tone against itself:
//
//   mean(+-1, +-2) = (0.5007 + 0.5007 + 0.0003 + 0.0003)/4 = 0.2505
//   peakiness      = 1.0 / 0.2505 = 3.99      <-- hard ceiling, ANY tone
//
// Measured at the old radius: on-bin tone 3.99, 1 kHz off-bin tone 3.29,
// broadband noise up to 3.46 -- i.e. noise scored HIGHER than the tone, and
// kDefaultThreshold (10.0) could never fire. The spec's radius was a
// rectangular-window figure (2-bin main lobe) applied to a Hann-windowed FFT.
// The window is correct; the radius was the defect. kDefaultThreshold is
// unchanged.
//
// Why +-3..+-5 specifically
// =========================
// Independent double-precision reference model, 48 kHz, N = 1024 (the FFT size
// when this was measured), 1 kHz tone at 26 dB SNR, worst noise-only local
// maximum over 50 seeds:
//
//   annulus    1 kHz tone   worst noise local max   ratio   low-freq floor
//   +-3..+-4      100.3          10.37 (FALSE POSITIVE)  9.7x    187.5 Hz
//   +-3..+-5      124.4           7.20                  17.3x    234.4 Hz
//   +-3..+-6      152.9           6.23                  24.6x    281.3 Hz
//
// +-3..+-4 is rejected because a noise-only frame actually crossed 10.0.
// +-3..+-6 separates slightly better but pushes the low-frequency blind spot
// up another 47 Hz for no detection benefit that matters. +-3..+-5 is the
// chosen trade-off.
//
// NOTE (2026-08-24 tuning brief): the detector FFT is now 2048 points, so the
// bin width HALVED and every low-frequency floor quoted here moved down by
// 2x (~117 Hz at 48 kHz). The annulus shape itself is unchanged; the numeric
// sweep above predates the wider FFT and should be re-run before anyone
// retunes kDefaultThreshold from it.
//
// Confirmed in the real rig (Detector + JUCE FFT, tests/test_peakiness.cpp):
// the 1 kHz tone scores 131.70 and the worst noise-only bin over seeds 1..60
// is 7.35, with zero false candidates at the 10.0 threshold. The usable
// headroom under the threshold is therefore ~1.4x, NOT the ~2x that a short
// seed list suggests -- do not raise the annulus's inner radius or lower
// kDefaultThreshold without re-running that sweep.
//
// *** FORMER v1 LIMITATION, FIXED 2026-08-24: the blind spot moved down ***
// ========================================================================
// A bin needs a full annulus on BOTH sides, so the lowest scoreable bin is
// kNeighbourOuterRadius = 5. The v1 1024-point FFT made that ~234 Hz at
// 48 kHz -- above the 100 Hz product floor and a genuine coverage gap for
// low-mid feedback. The tuning brief widened the FFT to 2048 points, so the
// same radius now binds at ~117 Hz (48 kHz) / ~107 Hz (44.1 kHz) / ~234 Hz
// (96 kHz): the 100 Hz floor is nearly reached at the common rates. The gap
// still scales with the sample rate; anyone retuning should recompute it as
// 5 * sampleRate / Detector::kFftSize before blaming the threshold for a
// missed howl.
//
// Rejected alternative: a one-sided (upper-only) annulus near the low edge
// would extend coverage down to bin 0. It is NOT implemented, because the
// low-frequency noise floor of live sound rises toward DC: averaging only the
// bins ABOVE a low bin systematically underestimates its local background and
// so inflates its peakiness -- more false positives exactly where the
// product's headline goal is fewer. Fixing the gap properly means a longer
// FFT (finer bins) or a per-bin noise-floor tracker, both out of scope here.
//
// Threading / allocation
// ======================
// The detector thread is not the audio thread, so this is not hard real-time.
// It is allocation-free anyway: candidates_ is a fixed std::array sized in the
// header, analyse() writes into it and hands back a pointer plus a count. The
// returned pointer is valid until the next analyse() on the same object.

#pragma once

#include "dsp/Detector.h"

#include <array>
#include <atomic>
#include <cstddef>

class PeakinessAnalyzer
{
public:
    // Half-width of the Hann main lobe, in bins. Offsets 0..+-kMainLobeRadius
    // are EXCLUDED from the neighbourhood: they belong to the tone, not to its
    // background. Not directly used by the loop -- kNeighbourInnerRadius is
    // what the loop reads -- but it is the reason that constant is 3.
    static constexpr int    kMainLobeRadius         = 2;

    // The neighbourhood is the annulus [inner, outer] on both sides, i.e. the
    // six bins at offsets -5,-4,-3,+3,+4,+5.
    static constexpr int    kNeighbourInnerRadius   = kMainLobeRadius + 1;  // 3
    static constexpr int    kNeighbourOuterRadius   = 5;

    static constexpr int    kNeighbourCount         = 2 * (kNeighbourOuterRadius
                                                           - kNeighbourInnerRadius + 1);  // 6

    static constexpr double kDefaultMinFrequencyHz  = 100.0;  // spec 5.2 step 4
    // Runtime-tunable (brief 2026-08-24). 10.0 was MEASURED for the old
    // 1024-point geometry (see above); with the 2048-point FFT it has NOT yet
    // been re-swept against real-room logs -- expect a field calibration pass
    // before this default is trusted. Clamped on set to [5, 20].
    static constexpr float  kDefaultThreshold       = 10.0f;
    static constexpr float  kMinThreshold           = 5.0f;
    static constexpr float  kMaxThreshold           = 20.0f;
    static constexpr float  kCandidateScore         = 0.5f;   // plan Task 11
    static constexpr int    kMaxCandidates          = 32;

    static_assert (kNeighbourInnerRadius > kMainLobeRadius,
                   "the neighbourhood must start OUTSIDE the Hann main lobe");
    static_assert (kNeighbourOuterRadius >= kNeighbourInnerRadius,
                   "the annulus must contain at least one bin per side");

    struct Candidate
    {
        int    bin         = 0;
        double frequencyHz = 0.0;   // bin * sampleRate / Detector::kFftSize
        float  magnitude   = 0.0f;
        float  peakiness   = 0.0f;
        float  score       = 0.0f;  // kCandidateScore; Tasks 12+ adjust it
    };

    struct Result
    {
        const Candidate* candidates = nullptr;  // valid until the next analyse()
        std::size_t      count      = 0;
    };

    PeakinessAnalyzer();

    // Message thread writes, detector thread loads relaxed (brief 2026-08-24).
    void   setThreshold (float peakinessThreshold);
    float  getThreshold() const;
    void   setMinFrequencyHz (double hz);
    double getMinFrequencyHz() const;

    Result analyse (const Detector::Spectrum& spectrum);

    // Peakiness of one bin against the six bins at offsets +-3..+-5.
    // Returns 0.0f when `bin` is too close to either end for a full annulus
    // (valid range [kNeighbourOuterRadius, numBins-1-kNeighbourOuterRadius]),
    // or when the neighbourhood mean is zero.
    // Exposed because Task 14's auto-release must re-test a locked notch's bin
    // without re-running candidate selection.
    static float peakinessAt (const float* magnitudes, int numBins, int bin);

private:
    std::atomic<float> threshold_;
    double minFrequencyHz_;
    std::array<Candidate, kMaxCandidates> candidates_;  // pre-allocated
};
