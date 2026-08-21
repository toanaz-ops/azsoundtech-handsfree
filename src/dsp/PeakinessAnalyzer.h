// PeakinessAnalyzer: spectral peakiness scoring for feedback detection (Task 11).
//
// Consumes a Detector::Spectrum and reports the bins that look like a howl:
// narrow peaks standing well above their immediate spectral surroundings.
//
// The metric (spec 5.2 step 4)
// ============================
//   peakiness = bin_mag / mean(neighbours at -2, -1, +1, +2)
//
// The centre bin is deliberately NOT part of its own neighbourhood. Including
// it would drag every value toward 1 and flatten the dynamic range the
// threshold relies on.
//
// *** KNOWN DEFECT -- READ BEFORE TRUSTING kDefaultThreshold ***
// =============================================================
// With Detector's Hann window and 1024-point FFT, a single sinusoid occupies a
// FOUR-bin main lobe. The +-1 and +-2 neighbours therefore sit INSIDE the
// tone's own main lobe and scale with the peak, so peakiness is bounded above
// by 4.0 for any single tone, no matter how loud:
//
//   Periodic-Hann kernel magnitudes are (0.5, 0.25, 0.25, 0, 0) at offsets
//   (0, -1, +1, -2, +2). For a tone centred on bin k:
//     mean(neighbours) = (0.25 + 0.25 + 0 + 0)/4 * mag = 0.125/0.5 * mag[k]
//                      = 0.25 * mag[k]
//     peakiness        = mag[k] / (0.25 * mag[k]) = 4.0     <-- hard ceiling
//
// Measured: an on-bin tone scores 3.99; a 1 kHz tone at 48 kHz (bin 21.33,
// the worst half-bin case is 2.80) scores 3.29. Pure broadband noise scores up
// to 3.21. The tone and the noise floor are NOT separable by this metric at
// this radius, and kDefaultThreshold (10.0, from plan Task 11) can never fire.
//
// The constants below are kept exactly as spec 5.2 and plan Task 11 specify.
// Changing kNeighbourRadius is a spec decision, not an implementation one --
// see tests/test_peakiness.cpp and the Task 11 report for the evidence and the
// proposed fix (neighbours at +-3..+-6, which excludes the main lobe and
// restores a ~30x tone/noise separation at the plan's 10.0 threshold).
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
#include <cstddef>

class PeakinessAnalyzer
{
public:
    static constexpr int    kNeighbourRadius        = 2;      // spec: neighbours +-2
    static constexpr double kDefaultMinFrequencyHz  = 100.0;  // spec 5.2 step 4
    static constexpr float  kDefaultThreshold       = 10.0f;  // plan Task 11
    static constexpr float  kCandidateScore         = 0.5f;   // plan Task 11
    static constexpr int    kMaxCandidates          = 32;

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

    void   setThreshold (float peakinessThreshold);
    float  getThreshold() const;
    void   setMinFrequencyHz (double hz);
    double getMinFrequencyHz() const;

    Result analyse (const Detector::Spectrum& spectrum);

    // Peakiness of one bin against its +-kNeighbourRadius neighbours.
    // Returns 0.0f when `bin` is too close to either end for a full
    // neighbourhood, or when the neighbourhood mean is zero.
    // Exposed because Task 14's auto-release must re-test a locked notch's bin
    // without re-running candidate selection.
    static float peakinessAt (const float* magnitudes, int numBins, int bin);

private:
    float  threshold_;
    double minFrequencyHz_;
    std::array<Candidate, kMaxCandidates> candidates_;  // pre-allocated
};
