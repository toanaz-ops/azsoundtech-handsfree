// src/dsp/LoopGainEstimator.cpp
//
// The formulas and the delay / truncation argument live in LoopGainEstimator.h.
// This file is only their arithmetic.
#include "dsp/LoopGainEstimator.h"

// Only finish() needs it, for kSweepLowHz -- the header does not, so it does not
// pay for it.
#include "dsp/SoundcheckSignal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// Floor for every logarithm and every denominator. A silent bin gives EY = EX =
// 0, and 0/0 is not a number a notch frequency may be derived from.
constexpr double kEps = 1.0e-20;

double toDb (double numerator, double denominator)
{
    return 10.0 * std::log10 (std::max (numerator, kEps) / std::max (denominator, kEps));
}
} // namespace

LoopGainEstimator::LoopGainEstimator (double sampleRate)
    : sampleRate_ (sampleRate > 0.0 ? sampleRate : 48000.0),
      scratch_ ((std::size_t) (2 * Detector::kFftSize), 0.0f)
{
}

void LoopGainEstimator::reset (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    noiseStream_     = Stream {};
    referenceStream_ = Stream {};
    captureStream_   = Stream {};

    noiseAccum_.fill (0.0);
    referenceAccum_.fill (0.0);
    captureAccum_.fill (0.0);

    // scratch_ keeps its allocation: its size is fixed by kFftSize, not by the
    // sample rate.
    std::fill (scratch_.begin(), scratch_.end(), 0.0f);
}

void LoopGainEstimator::pushNoiseFloor (const float* samples, int numSamples)
{
    pushInto (noiseStream_, noiseAccum_, samples, numSamples);
}

void LoopGainEstimator::pushReference (const float* samples, int numSamples)
{
    pushInto (referenceStream_, referenceAccum_, samples, numSamples);
}

void LoopGainEstimator::pushCapture (const float* samples, int numSamples)
{
    pushInto (captureStream_, captureAccum_, samples, numSamples);
}

void LoopGainEstimator::pushInto (Stream& s, std::array<double, kNumBins>& accum,
                                  const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        s.window[(std::size_t) s.fill++] = samples[i];
        ++s.sinceLastFrame;

        // A frame is emitted only from a FULL window, so no frame is ever half
        // signal and half the zero prefix the window starts life with. A stream
        // shorter than kFftSize therefore produces no frames at all, which
        // finish() reports as "not measured" rather than as a quiet room.
        if (s.fill == Detector::kFftSize && s.sinceLastFrame >= Detector::kHopSize)
        {
            analyseFrame (s, accum);
            ++s.frames;

            constexpr int kRetained = Detector::kFftSize - Detector::kHopSize;
            std::memmove (s.window.data(), s.window.data() + Detector::kHopSize,
                          (std::size_t) kRetained * sizeof (float));
            s.fill           = kRetained;
            s.sinceLastFrame = 0;
        }
    }
}

void LoopGainEstimator::analyseFrame (const Stream& s, std::array<double, kNumBins>& accum)
{
    std::memcpy (scratch_.data(), s.window.data(),
                 (std::size_t) Detector::kFftSize * sizeof (float));
    std::fill (scratch_.begin() + Detector::kFftSize, scratch_.end(), 0.0f);

    hann_.multiplyWithWindowingTable (scratch_.data(), (std::size_t) Detector::kFftSize);
    fft_.performFrequencyOnlyForwardTransform (scratch_.data(), true);

    for (int k = 0; k < kNumBins; ++k)
    {
        const double mag = (double) scratch_[(std::size_t) k];
        accum[(std::size_t) k] += mag * mag;
    }
}

LoopGainEstimator::Result LoopGainEstimator::finish() const
{
    Result r;
    r.noiseFrames     = noiseStream_.frames;
    r.referenceFrames = referenceStream_.frames;
    r.captureFrames   = captureStream_.frames;

    const double framesN = (double) r.noiseFrames;
    const double framesY = (double) r.captureFrames;

    const int loBin = hzToBin (SoundcheckSignal::kSweepLowHz, sampleRate_);
    const int hiBin = hzToBin (kTrustedHighHz,                sampleRate_);

    // Both accumulators are SUMMED SQUARED MAGNITUDES over their own frames, so
    // the noise mean must be scaled back up by the CAPTURE frame count before it
    // can be subtracted from the capture sum -- the two streams are different
    // lengths and a per-frame mean is the only comparable form.
    double bandCapture = 0.0;   // summed squared magnitudes
    double bandNoise   = 0.0;   // summed squared magnitudes, same scale

    for (int k = 0; k < kNumBins; ++k)
    {
        const auto i = (std::size_t) k;

        const double nBar     = framesN > 0.0 ? noiseAccum_[i] / framesN : 0.0;
        const double noiseInY = nBar * framesY;              // summed squared magnitudes
        const double eY       = captureAccum_[i];            // summed squared magnitudes
        const double eX       = referenceAccum_[i];          // summed squared magnitudes

        r.hDb[i] = (float) toDb (eY - noiseInY, eX);

        // Both sides of this comparison are dB.
        const double binSnrDb = toDb (eY, noiseInY);
        r.trusted[i] = binToHz (k, sampleRate_) <= kTrustedHighHz
                    && binSnrDb >= kMinBinSnrDb;

        if (k >= loBin && k <= hiBin)
        {
            bandCapture += eY;
            bandNoise   += noiseInY;
        }
    }

    r.bandSnrDb = (float) toDb (bandCapture, bandNoise);

    // Both sides of this comparison are dB.
    r.measured = r.referenceFrames > 0 && r.captureFrames > 0 && r.noiseFrames > 0
              && (double) r.bandSnrDb >= kMinBandSnrDb;

    // `measured` OVERRIDES `trusted` -- see the precedence note on Result. The
    // per-bin loop above cannot apply this itself because bandSnrDb is not known
    // until the loop that computes it has finished, so the gate lands here, on
    // the whole array, and no bin can outlive the measurement it came from.
    if (! r.measured)
        r.trusted.fill (false);

    return r;
}

double LoopGainEstimator::binToHz (int bin, double sampleRate)
{
    return (double) bin * sampleRate / (double) Detector::kFftSize;
}

int LoopGainEstimator::hzToBin (double hz, double sampleRate)
{
    if (! (sampleRate > 0.0) || ! std::isfinite (hz))
        return 0;

    const double raw = std::round (hz * (double) Detector::kFftSize / sampleRate);
    if (raw <= 0.0)
        return 0;
    if (raw >= (double) (kNumBins - 1))
        return kNumBins - 1;

    return (int) raw;
}
