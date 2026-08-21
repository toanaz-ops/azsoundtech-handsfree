#include "dsp/PeakinessAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <utility>

PeakinessAnalyzer::PeakinessAnalyzer()
    : threshold_ (kDefaultThreshold)
    , minFrequencyHz_ (kDefaultMinFrequencyHz)
{
}

void PeakinessAnalyzer::setThreshold (float peakinessThreshold)
{
    threshold_ = peakinessThreshold;
}

float PeakinessAnalyzer::getThreshold() const
{
    return threshold_;
}

void PeakinessAnalyzer::setMinFrequencyHz (double hz)
{
    minFrequencyHz_ = hz;
}

double PeakinessAnalyzer::getMinFrequencyHz() const
{
    return minFrequencyHz_;
}

float PeakinessAnalyzer::peakinessAt (const float* magnitudes, int numBins, int bin)
{
    if (magnitudes == nullptr)
    {
        return 0.0f;
    }

    // A bin without a full annulus on BOTH sides has no defined peakiness.
    // Reporting a partial-neighbourhood ratio would make the edges of the
    // spectrum systematically peakier than the middle. The OUTER radius is
    // what binds here -- an annulus is only complete once its farthest bin is
    // in range.
    if (bin < kNeighbourOuterRadius || bin > numBins - 1 - kNeighbourOuterRadius)
    {
        return 0.0f;
    }

    // Mean of the SIX bins at offsets -5, -4, -3, +3, +4, +5. Offsets 0, +-1
    // and +-2 are skipped: a Hann main lobe is four bins wide, so those bins
    // are the tone itself (bin+-1 carries ~0.50 of the peak) and averaging
    // them in would measure the tone against itself -- the defect that capped
    // the original spec formula at 4.0. See PeakinessAnalyzer.h.
    float sum = 0.0f;
    for (int offset = kNeighbourInnerRadius; offset <= kNeighbourOuterRadius; ++offset)
    {
        sum += magnitudes[bin - offset];
        sum += magnitudes[bin + offset];
    }

    const float mean = sum / static_cast<float> (kNeighbourCount);

    // Silence gives an all-zero spectrum, so this is 0/0 = NaN, and an isolated
    // spike on a silent floor is x/0 = +infinity. NaN compares false against
    // every threshold, so neither would LOOK broken -- they would just poison
    // whatever arithmetic Tasks 12-14 do with the value. Return a real number.
    // The negated form also catches a NaN mean.
    if (! (mean > 0.0f))
    {
        return 0.0f;
    }

    const float peakiness = magnitudes[bin] / mean;
    return std::isfinite (peakiness) ? peakiness : 0.0f;
}

PeakinessAnalyzer::Result PeakinessAnalyzer::analyse (const Detector::Spectrum& spectrum)
{
    // Detector hands back magnitudes == nullptr when the tap had no new audio,
    // and a non-positive rate would make every bin-to-Hz conversion nonsense.
    // Neither is dereferenced.
    if (spectrum.magnitudes == nullptr || spectrum.sampleRate <= 0.0)
    {
        return {};
    }

    const float* const mags     = spectrum.magnitudes;
    const double       binWidth = spectrum.sampleRate / static_cast<double> (Detector::kFftSize);

    // The last bin that still has a full annulus.
    const int lastBin = Detector::kNumBins - 1 - kNeighbourOuterRadius;

    // The minimum bin is DERIVED from the spectrum's own sample rate -- the app
    // supports 44.1/48/88.2/96 kHz, so a hardcoded bin index would silently
    // mean a different frequency at every rate.
    const double firstBinExact = std::ceil (minFrequencyHz_ / binWidth);

    // At every supported rate kNeighbourOuterRadius, not minFrequencyHz_, is
    // what binds: 5 * 48000/1024 = 234.375 Hz, well above the 100 Hz default.
    // That blind spot is documented in PeakinessAnalyzer.h.
    int firstBin = kNeighbourOuterRadius;
    if (firstBinExact > static_cast<double> (lastBin))
    {
        // Nothing in range. Also the guard that keeps the cast below in range;
        // a NaN or negative ratio falls through to the outer radius instead.
        return { candidates_.data(), 0 };
    }
    if (firstBinExact > static_cast<double> (kNeighbourOuterRadius))
    {
        firstBin = static_cast<int> (firstBinExact);
    }

    std::size_t count = 0;

    for (int bin = firstBin; bin <= lastBin; ++bin)
    {
        // Local-maximum rule. A tone that falls between two bins lights up
        // both of them; without this, one howl would be reported twice and
        // Task 13 would burn two of the sixteen notches on it. The asymmetry
        // (`>` left, `>=` right) collapses an exact two-bin plateau to its
        // lower bin instead of emitting both or neither.
        if (! (mags[bin] > mags[bin - 1] && mags[bin] >= mags[bin + 1]))
        {
            continue;
        }

        const float peakiness = peakinessAt (mags, Detector::kNumBins, bin);

        // Strictly greater: spec 5.2 step 4 and plan Task 11 both say
        // "> threshold". A bin sitting exactly on the threshold is not a
        // candidate.
        if (! (peakiness > threshold_))
        {
            continue;
        }

        Candidate candidate;
        candidate.bin         = bin;
        candidate.frequencyHz = static_cast<double> (bin) * spectrum.sampleRate
                              / static_cast<double> (Detector::kFftSize);
        candidate.magnitude   = mags[bin];
        candidate.peakiness   = peakiness;
        candidate.score       = kCandidateScore;

        // Insertion into the pre-allocated array, kept in descending peakiness.
        // When full we evict the WEAKEST survivor, never "stop scanning at 32":
        // the strongest feedback is often at a high bin index, and dropping it
        // because 32 weaker bins were seen first is exactly the failure that
        // would leave a real howl un-notched.
        if (count < static_cast<std::size_t> (kMaxCandidates))
        {
            candidates_[count] = candidate;
            ++count;
        }
        else if (peakiness > candidates_[count - 1].peakiness)
        {
            candidates_[count - 1] = candidate;
        }
        else
        {
            continue;
        }

        for (std::size_t i = count - 1;
             i > 0 && candidates_[i].peakiness > candidates_[i - 1].peakiness;
             --i)
        {
            std::swap (candidates_[i], candidates_[i - 1]);
        }
    }

    return { candidates_.data(), count };
}
