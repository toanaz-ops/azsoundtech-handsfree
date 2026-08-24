// RtaProcessing -- display-side maths for the SpectrumView tuning toolbar.
//
// Header-only and GUI-free on purpose: everything here is pure arithmetic on
// plain floats so it can be unit-tested without a MessageManager, while
// SpectrumView calls it from its 30 Hz timer (message thread only -- nothing
// here ever touches the audio thread, the detector or the notch chain).
//
// Units contract: spectrum magnitudes arriving from NotchController's
// snapshot are LINEAR; every level this module returns is dB.

#pragma once

#include <cmath>
#include <vector>

namespace rta
{

enum class BandMode    { Line, Octave1, Octave3 };
enum class AverageMode { Off, S1, S3, S10 };

// Level reported for a band that received no bins. Not the plot floor --
// just an unambiguous "empty" value callers can clamp or skip.
inline constexpr float kSilenceDb = -120.0f;

// Per-frame EMA weight for a new sample at the given frame rate.
//
// Chosen as alpha = 1 - exp(-1 / (tau * fps)), i.e. the exact discrete EMA
// whose step response reaches 63% after tau seconds of WALL TIME regardless
// of frame rate. (The brief floated alpha = 1 - exp(-fps/(0.4*tau)); at RTA
// frame rates that expression evaluates to ~1.0 for every mode, i.e. no
// smoothing at all, so the standard form is used instead.)
//
// Off => 1.0 (each frame replaces, no smoothing). A degenerate fps also
// degrades to 1.0 rather than dividing by zero.
inline float averageAlpha (AverageMode mode, float framesPerSecond)
{
    float tauSeconds = 0.0f;
    switch (mode)
    {
        case AverageMode::S1:  tauSeconds = 1.0f;  break;
        case AverageMode::S3:  tauSeconds = 3.0f;  break;
        case AverageMode::S10: tauSeconds = 10.0f; break;
        case AverageMode::Off:
        default:               return 1.0f;
    }

    if (! (framesPerSecond > 0.0f))
        return 1.0f;

    return 1.0f - std::exp (-1.0f / (tauSeconds * framesPerSecond));
}

// Nominal center frequencies of the standard band series, in Hz, ascending,
// restricted to [minHz, maxHz]. Tabulated rather than generated: the ISO
// nominal centers are what every RTA labels (and what the tests pin), and
// regenerating them from a geometric formula lands between nominals.
//
//   1/1 Oct: 31.5 .. 16000  (10 bands)
//   1/3 Oct: 20 .. 20000    (31 bands)
inline std::vector<float> bandCenters (BandMode mode, float minHz, float maxHz)
{
    static constexpr float kOctave1[] = {
        31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
        1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f,
    };
    static constexpr float kOctave3[] = {
        20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f,
        125.0f, 160.0f, 200.0f, 250.0f, 315.0f, 400.0f, 500.0f, 630.0f,
        800.0f, 1000.0f, 1250.0f, 1600.0f, 2000.0f, 2500.0f, 3150.0f,
        4000.0f, 5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f,
        16000.0f, 20000.0f,
    };

    std::vector<float> centers;
    switch (mode)
    {
        case BandMode::Octave1:
            centers.assign (std::begin (kOctave1), std::end (kOctave1));
            break;
        case BandMode::Octave3:
            centers.assign (std::begin (kOctave3), std::end (kOctave3));
            break;
        case BandMode::Line:
        default:
            return centers;   // deliberately empty
    }

    std::vector<float> kept;
    kept.reserve (centers.size());
    for (const float c : centers)
        if (c >= minHz && c <= maxHz)
            kept.push_back (c);
    return kept;
}

// Number of bands a mode produces over a range -- lets callers pre-size
// their output buffer outside the per-frame path.
inline int bandCount (BandMode mode, float minHz, float maxHz)
{
    return (int) bandCenters (mode, minHz, maxHz).size();
}

// Aggregate FFT bins into bands by POWER: p = 10^(db/10) with
// db = 20*log10(mag); each bin joins the band whose edges
// center * 2^(+/-0.5/N) (N = 1 or 3) contain its frequency. Bins with a
// center frequency outside [minHz, maxHz] are ignored.
//
// outLevelsDb must have room for one entry per band returned by
// bandCenters(mode, minHz, maxHz). Empty bands report kSilenceDb.
//
// Bins arrive in ascending frequency order, so a single forward walk over
// the band list keeps the whole pass O(bins + bands).
inline void bandLevelsDb (BandMode mode, const float* magnitudesLinear,
                          int numBins, float hzPerBin,
                          float minHz, float maxHz,
                          float* outLevelsDb)
{
    const auto centers = bandCenters (mode, minHz, maxHz);
    const int numBands = (int) centers.size();
    if (numBands == 0 || magnitudesLinear == nullptr || outLevelsDb == nullptr)
        return;

    const float n = (mode == BandMode::Octave1) ? 1.0f : 3.0f;
    const float halfWidthFactor = std::exp2 (0.5f / n);

    for (int b = 0; b < numBands; ++b)
        outLevelsDb[b] = kSilenceDb;

    std::vector<float> power ((std::size_t) numBands, 0.0f);
    int band = 0;

    for (int bin = 0; bin < numBins; ++bin)
    {
        const float hz = (float) bin * hzPerBin;
        if (hz < minHz)
            continue;
        if (hz > maxHz)
            break;

        // Advance to the band whose upper edge passes this bin. Bins before
        // the first lower edge fall through with band == 0 and get rejected
        // below instead of wrapping around.
        while (band < numBands - 1
               && hz > centers[(std::size_t) band] * halfWidthFactor)
            ++band;

        const float center = centers[(std::size_t) band];
        if (hz < center / halfWidthFactor)
            continue;

        // Exact-zero bins contribute nothing and must not drag an "empty"
        // band off kSilenceDb by adding a denormal-scale power.
        const float mag = magnitudesLinear[bin];
        if (! (mag > 0.0f))
            continue;

        const float db  = 20.0f * std::log10 (std::max (mag, 1.0e-12f));
        power[(std::size_t) band] += std::pow (10.0f, db / 10.0f);
    }

    for (int b = 0; b < numBands; ++b)
        if (power[(std::size_t) b] > 0.0f)
            outLevelsDb[b] = 10.0f * std::log10 (power[(std::size_t) b]);
}

} // namespace rta
