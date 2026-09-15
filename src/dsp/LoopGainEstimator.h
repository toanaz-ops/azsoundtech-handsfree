// src/dsp/LoopGainEstimator.h
//
// Lane M, spec §4.4. Turns three captured sample streams -- the noise floor,
// the reference sweep regenerated from SoundcheckSignal, and what the mic
// actually heard -- into one per-bin loop gain H_dB[k].
//
//   Nbar[k]  = (sum over noise-floor frames of |mic_f[k]|^2) / frames_N
//   EX[k]    =  sum over reference frames of |X_f[k]|^2
//   EY[k]    =  sum over capture   frames of |Y_f[k]|^2
//   H_dB[k]  = 10*log10( max(EY[k] - Nbar[k]*frames_Y, eps) / max(EX[k], eps) )
//
// WHAT H_dB MEANS. X is dBFS at the app's OUTPUT and Y is dBFS at the app's
// INPUT, so H is the transfer function of the entire PHYSICAL part of the loop.
// The loop closes through the app, and the app is unity at every frequency with
// no notch (AudioEngine.cpp:592-624). Therefore howling happens at any bin with
// H_dB[k] >= 0, and that bin's margin is -H_dB[k] dB.
//
// WHY NO DELAY COMPENSATION, AND WHERE THAT ARGUMENT STOPS BEING TRUE. This is
// a per-bin ENERGY RATIO, not a time correlation: the energy the sweep put into
// bin k does not depend on when it arrived, and neither does the energy the mic
// received in bin k -- PROVIDED the capture window contains both the swept part
// and the ringing tail. That proviso is the whole risk. A resonance whose decay
// outlives the capture window is UNDER-READ by the fraction of its energy that
// fell outside, and nothing here corrects for it: the error is measured by
// tests/test_loopgainestimator.cpp (DecayLongerThanTheTailIsUnderRead) rather
// than hidden. Under-reading is the safe direction -- it proposes less gain
// reduction than the room needs, never more.
//
// TRUSTED BAND NARROWER THAN SWEPT BAND (F19). A constant-amplitude log sweep
// spends equal time per octave, so energy PER HZ falls as 1/f: with a fixed
// 23.4 Hz bin, a bin at 10 kHz receives exactly 20 dB less than one at 100 Hz.
// v1 therefore sweeps 100 Hz - 10 kHz but only marks bins inside
// [kSweepLowHz, kTrustedHighHz] as trusted. Bins above kTrustedHighHz still
// carry an hDb value -- they are DRAWN -- and are never proposed. Pre-emphasis
// is deferred (spec §8).
//
// THREADING. Pure arithmetic, no device and no thread of its own; lane M's
// controller thread owns an instance and is the only caller. Allocation happens
// in the constructor only -- push*() and finish() never allocate.
//
// Deliberately in src/dsp/ with no src/app/ include and no juce_audio_devices;
// <juce_dsp/juce_dsp.h> only, exactly as Detector.h:55 does.
#pragma once

#include <juce_dsp/juce_dsp.h>

#include "dsp/Detector.h"
#include "dsp/SoundcheckSignal.h"

#include <array>
#include <cstddef>
#include <vector>

class LoopGainEstimator
{
public:
    // The estimator analyses over the DETECTOR's transform -- same 2048-point
    // FFT, same 512 hop, same Hann window -- so a bin index means the same
    // frequency in both, and a soundcheck proposal lands on the bin the live
    // detector will later watch.
    static constexpr int    kNumBins        = Detector::kNumBins;   // 1025
    static constexpr double kTrustedHighHz  = 6000.0;
    static constexpr double kMinBandSnrDb   = 12.0;
    static constexpr double kMinBinSnrDb    = 6.0;

    explicit LoopGainEstimator (double sampleRate);

    // Clears every stream and accumulator and adopts the new rate. Does not
    // reallocate: the scratch and the windows are sized by kFftSize, not by the
    // sample rate.
    void reset (double sampleRate);

    void pushNoiseFloor (const float* samples, int numSamples);
    void pushReference  (const float* samples, int numSamples);
    void pushCapture    (const float* samples, int numSamples);

    struct Result
    {
        std::array<float, kNumBins> hDb {};
        std::array<bool,  kNumBins> trusted {};
        float bandSnrDb    = 0.0f;
        int   noiseFrames  = 0, referenceFrames = 0, captureFrames = 0;
        bool  measured     = false;   // bandSnrDb >= kMinBandSnrDb and all three streams present
    };

    [[nodiscard]] Result finish() const;

    [[nodiscard]] static double binToHz  (int bin, double sampleRate);
    [[nodiscard]] static int    hzToBin  (double hz, double sampleRate);

private:
    // Three INDEPENDENT sliding windows, one per stream, each emitting a frame
    // every Detector::kHopSize new samples. Same shape as Detector's window
    // (Detector.h:128) and deliberately duplicated rather than reused: Detector
    // reads from a LockFreeRingBuffer and owns detection state, and lane M needs
    // three streams with no ring and no detection.
    struct Stream
    {
        std::array<float, Detector::kFftSize> window {};
        int  fill = 0;               // samples in `window`
        int  sinceLastFrame = 0;     // new samples since the last emitted frame
        int  frames = 0;
    };

    void pushInto (Stream& s, std::array<double, kNumBins>& accum,
                   const float* samples, int numSamples);
    void analyseFrame (const Stream& s, std::array<double, kNumBins>& accum);

    double sampleRate_ = 48000.0;
    juce::dsp::FFT fft_ { Detector::kFftOrder };
    juce::dsp::WindowingFunction<float> hann_
        { (std::size_t) Detector::kFftSize, juce::dsp::WindowingFunction<float>::hann, false };

    // MUST be 2 * kFftSize floats: performFrequencyOnlyForwardTransform reads
    // and writes the whole array even though only kNumBins entries come back.
    // Sizing it kFftSize overruns by 8 KB (Detector.h:34-35).
    std::vector<float> scratch_;

    Stream noiseStream_, referenceStream_, captureStream_;

    // double, not float: ~280 frames x 1025 bins of squared magnitudes at very
    // different scales loses bits in float.
    std::array<double, kNumBins> noiseAccum_ {}, referenceAccum_ {}, captureAccum_ {};
};
