// Detector: FFT analysis front-end for feedback detection (Task 10).
//
// The detector is the SOLE consumer of AudioEngine's post-notch tap. It is a
// plain synchronous object -- it owns no thread. The detector thread that will
// call it in a loop arrives with the notch controller (Tasks 13-14); keeping
// the FFT stage thread-free is what makes it unit-testable without an audio
// device.
//
// Analysis geometry
// =================
// 1024-point FFT with 50% overlap, i.e. a 512-sample hop (plan Task 10). Each
// call to processLatestBlock() pulls at most kHopSize NEW samples off the tap,
// slides them into a kFftSize-sample history window, and transforms the whole
// window. Consecutive spectra therefore share half their input, which is what
// gives a new spectrum every ~10.7 ms at 48 kHz instead of every ~21.3 ms.
//
// Why performFrequencyOnlyForwardTransform
// ========================================
// Tasks 11 (peakiness) and 12 (harmonic-aware scoring) only ever look at
// magnitudes; phase is never used. JUCE's magnitude-only entry point returns
// the kNumBins non-negative-frequency magnitudes directly, so there is no
// interleaved complex buffer to de-interleave and no sqrt(re^2+im^2) of our
// own to get wrong.
//
// JUCE 9 API contract (verified against external/JUCE/modules/juce_dsp/frequency/)
// ==============================================================================
// - juce::dsp::FFT (int order); order 10 => 1024 points.
// - performFrequencyOnlyForwardTransform (float* inOut, bool onlyNonNegative):
//   *** the array passed in must be 2 * getSize() floats ***, i.e. 2048 for a
//   1024-point FFT, even though only the first kNumBins entries are read back.
//   fftBuffer_ is sized accordingly; sizing it kFftSize would overrun by 4 KB.
// - juce::dsp::WindowingFunction<float> (size_t size, WindowingMethod,
//   bool normalise = true, FloatType beta = 0), applied via
//   multiplyWithWindowingTable (float* samples, size_t size).
//
// Timeline honesty (carry-over rule from Task 9)
// ==============================================
// AudioEngine only writes the tap when input channel 0 is non-null, so the hop
// is a BUFFER-POSITION hop, not a wall-clock hop. When fewer than kHopSize new
// samples are available the block is still analysed, but Spectrum::readCount
// reports the short hop rather than being rounded up to a full one. Task 14's
// auto-release cadence must read readCount instead of assuming kHopSize.
//
// Threading: processLatestBlock() calls tap.read() and nothing else; the
// detector never writes the tap, preserving the SPSC discipline. After
// construction the hot path performs no heap allocation, takes no lock and
// does no logging.

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "dsp/LockFreeRingBuffer.h"

#include <atomic>
#include <cstddef>
#include <vector>

class Detector
{
public:
    static constexpr int kFftSize = 1024;
    static constexpr int kHopSize = 512;
    static constexpr int kNumBins = kFftSize / 2 + 1;   // 513
    static constexpr int kFftOrder = 10;                // 2^10 == kFftSize

    struct Spectrum
    {
        // kNumBins magnitudes, owned by the Detector and valid until the next
        // call to processLatestBlock(). nullptr when no block was produced.
        const float* magnitudes = nullptr;

        // NEW samples consumed from the tap for this block: 0..kHopSize.
        // A value below kHopSize means the analysis timeline advanced by less
        // than one hop -- the tap under-ran.
        std::size_t readCount = 0;

        // The rate the magnitudes should be interpreted at, so consumers never
        // have to ask the Detector separately and risk a torn pair.
        double sampleRate = 0.0;
    };

    explicit Detector (double sampleRate);

    void   setSampleRate (double sampleRate);
    double getSampleRate() const;

    // Pulls up to kHopSize new samples off the tap, slides them into the
    // analysis window and returns its magnitude spectrum. Returns a Spectrum
    // whose magnitudes == nullptr when the tap had nothing new, since there is
    // then no fresh block to report.
    Spectrum processLatestBlock (LockFreeRingBuffer<float>& tap);

    // Zeroes the analysis window. Call whenever the audio timeline becomes
    // discontinuous -- a device restart or a sample-rate change -- so audio
    // captured at the old rate is never folded into a spectrum labelled with
    // the new one. Allocation-free.
    void reset();

    // TEST ACCESSOR ONLY -- do not build product behaviour on this.
    //
    // A read-only view of the kFftSize-sample analysis window as it stands
    // after the last processLatestBlock(). It exists because the alternative
    // was worse: the short-hop path can only be verified by where the samples
    // LANDED, and asserting that through the magnitude spectrum would need a
    // signal whose FFT magnitude is position-sensitive -- a shifted sinusoid
    // has an identical magnitude spectrum, so the obvious probe proves
    // nothing. One const pointer is a smaller surface than a contrived
    // spectral assertion, and it makes the mutation in
    // tests/test_detector.cpp fail on the first element.
    const float* getAnalysisWindowForTest() const;

private:
    // Written by setSampleRate() from the device thread and read by
    // processLatestBlock() on the detector thread, so it must be atomic --
    // a plain double here is a data race the moment Task 14 wires the two
    // together. Relaxed ordering is sufficient: it is a single scalar and no
    // other state depends on it. (Spectrum::sampleRate exists so consumers
    // never read a torn pair; the tear was one level further down.)
    std::atomic<double>                 sampleRate_;
    juce::dsp::FFT                      fft_;
    juce::dsp::WindowingFunction<float> window_;

    // The sliding kFftSize analysis window. Zero-initialised, so the first
    // blocks after construction carry a genuine silent prefix rather than
    // garbage; it is never used to disguise a short hop as a full one.
    std::vector<float> history_;

    // Landing area for one hop read off the tap, so the read happens before
    // the window is slid and a short read never corrupts the retained half.
    std::vector<float> hop_;

    // Scratch handed to JUCE. MUST be 2 * kFftSize floats -- see the API
    // contract above.
    std::vector<float> fftBuffer_;

    // The kNumBins magnitudes handed out by the last successful call.
    std::vector<float> magnitudes_;
};
