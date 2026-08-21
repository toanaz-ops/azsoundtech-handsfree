#include "dsp/Detector.h"

#include <algorithm>
#include <cstring>

Detector::Detector (double sampleRate)
    : sampleRate_ (sampleRate)
    , fft_ (kFftOrder)
    , window_ (static_cast<std::size_t> (kFftSize),
               juce::dsp::WindowingFunction<float>::hann)
    , history_ (static_cast<std::size_t> (kFftSize), 0.0f)
    , hop_ (static_cast<std::size_t> (kHopSize), 0.0f)
    , fftBuffer_ (static_cast<std::size_t> (2 * kFftSize), 0.0f)
    , magnitudes_ (static_cast<std::size_t> (kNumBins), 0.0f)
{
}

void Detector::setSampleRate (double sampleRate)
{
    // Guard: a non-positive rate is invalid -- leave the detector untouched.
    // Matches NotchChain::setSampleRate, so a bad rate from
    // audioDeviceAboutToStart is rejected identically on both sides.
    if (sampleRate <= 0.0)
    {
        return;
    }

    sampleRate_ = sampleRate;
}

double Detector::getSampleRate() const
{
    return sampleRate_;
}

Detector::Spectrum Detector::processLatestBlock (LockFreeRingBuffer<float>& tap)
{
    // One hop of NEW audio at most. A short read is a short hop, not an error:
    // it is carried out in Spectrum::readCount rather than padded up.
    const std::size_t readCount = tap.read (hop_.data(), static_cast<std::size_t> (kHopSize));

    if (readCount == 0)
    {
        // Nothing new arrived, so there is no fresh block to report. Handing
        // back the previous magnitudes would let a caller mistake a stale
        // spectrum for a new one.
        return {};
    }

    // Slide the analysis window left by exactly the hop we got, then append
    // the new samples at the end. Overlapping this way is what makes
    // consecutive spectra share half their input.
    const std::size_t retained = static_cast<std::size_t> (kFftSize) - readCount;
    std::memmove (history_.data(), history_.data() + readCount, retained * sizeof (float));
    std::memcpy (history_.data() + retained, hop_.data(), readCount * sizeof (float));

    // JUCE requires 2 * getSize() floats; the upper half is engine workspace.
    // Zeroing it keeps the transform deterministic run to run.
    std::memcpy (fftBuffer_.data(), history_.data(),
                 static_cast<std::size_t> (kFftSize) * sizeof (float));
    std::fill (fftBuffer_.begin() + kFftSize, fftBuffer_.end(), 0.0f);

    window_.multiplyWithWindowingTable (fftBuffer_.data(), static_cast<std::size_t> (kFftSize));
    fft_.performFrequencyOnlyForwardTransform (fftBuffer_.data(), true);

    std::memcpy (magnitudes_.data(), fftBuffer_.data(),
                 static_cast<std::size_t> (kNumBins) * sizeof (float));

    return { magnitudes_.data(), readCount, sampleRate_ };
}
