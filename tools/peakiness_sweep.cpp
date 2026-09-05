// PeakinessSweep -- offline distribution sweep for the peakiness threshold.
//
// Why this exists
// ================
// PeakinessAnalyzer::kDefaultThreshold (10.0) was MEASURED against the OLD
// 1024-point FFT geometry (see PeakinessAnalyzer.h, the "2026-08-24 tuning
// brief" notes). The FFT was then widened to 2048 points, which halves the
// bin width and reshapes both the tone peakiness and the noise-floor
// peakiness the annulus formula produces. The header says as much: "it has
// NOT yet been re-swept against real-room logs". This tool is that sweep,
// done offline against synthetic signals so it can run anywhere, on demand,
// with no ASIO device attached.
//
// It drives the REAL Detector (real 2048-point FFT, real Hann window) and the
// REAL PeakinessAnalyzer::peakinessAt through their public API only -- nothing
// here reimplements the metric. That is the entire point: if the metric
// itself ever changes, this tool's numbers change with it instead of silently
// going stale.
//
// The one thing worth getting right on a re-read
// ================================================
// PeakinessAnalyzer::analyse() only returns candidates whose peakiness is
// STRICTLY ABOVE the threshold. That is exactly the wrong lens for a
// noise-floor sweep: every noise-only block would report zero candidates and
// the very distribution we are trying to see -- how close the noise floor
// sits to 10.0 -- would be invisible. So this tool does NOT call analyse()
// for its per-block numbers. It computes the per-block MAXIMUM peakiness
// itself, over every bin with a full annulus, using the static
// PeakinessAnalyzer::peakinessAt -- the same "pessimistic bound" the
// maxPeakiness() helper in tests/test_peakiness.cpp uses. `score` in the CSV
// is a shadow of what analyse() WOULD have reported that block (0.5 if the
// max crossed the threshold, else 0.0); it is not analyse()'s own local-max
// selection, which can differ by discarding a second, weaker crossing in the
// same block. That is fine here: the sweep cares about the ceiling the noise
// floor reaches, not which single bin a real notch placement would pick.
//
// Scenarios
// =========
// Four synthetic signals, run back-to-back, all at 48 kHz:
//   noise-floor          broadband white noise only -- the number that
//                         decides whether 10.0 still has headroom at 2048.
//   tone-1khz-in-noise   a clean tone, to confirm real signal still clears
//                         the threshold by a wide margin.
//   howl-ramp            a 2 kHz tone ramping 0 -> full amplitude over a
//                         noise floor -- shows the block at which a growing
//                         howl actually crosses 10.0.
//   music-like           the same multi-partial programme material
//                         tools/snapshot.cpp uses for its live screenshot,
//                         plus a small noise floor -- false-positive headroom
//                         on real programme material, not a test tone.
//
// Output
// ======
// Per-block CSV (block,freq,peakiness,score,threshold) to stdout, one
// "# scenario: <name>" comment line and one column header per scenario, then
// a plain-text summary table after all four. Purely OFFLINE analysis: no
// audio device, no GUI, no window, and nothing here touches src/ beyond
// reading Detector and PeakinessAnalyzer through their existing public API.

#include "dsp/Detector.h"
#include "dsp/LockFreeRingBuffer.h"
#include "dsp/PeakinessAnalyzer.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace
{
constexpr double kPi         = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;

// Ring-buffer capacity the real AudioEngine tap uses (see test_peakiness.cpp,
// kTapCapacity). Sized well above one hop so a write-then-drain-immediately
// loop never truncates.
constexpr std::size_t kTapCapacity = 8192;

// Aim for ~200 SETTLED analysed blocks per scenario, on top of the priming
// hops the Detector needs to fill its kFftSize analysis window from a cold
// (zeroed) start. 200 * kHopSize + 4 * kFftSize = 102400 + 8192 = 110592
// samples, which divides evenly into 216 hops of kHopSize each.
constexpr std::size_t kSamplesPerScenario =
    200 * static_cast<std::size_t> (Detector::kHopSize)
    + 4 * static_cast<std::size_t> (Detector::kFftSize);

//----------------------------------------------------------------------------
// Signal generators. Copied in spirit from tests/test_peakiness.cpp
// (makeToneInNoise) and tools/snapshot.cpp (the music-like partial set) --
// this tool does not include either file, since it must build and link on
// its own without pulling in gtest or the GUI sources.
//----------------------------------------------------------------------------

// A sine tone of fixed amplitude plus uniform white noise. frequencyHz == 0.0
// and toneAmplitude == 0.0f together produce pure noise (the noise-floor
// scenario reuses this rather than a separate generator).
std::vector<float> makeToneInNoise (double frequencyHz,
                                    float  toneAmplitude,
                                    float  noiseAmplitude,
                                    unsigned seed,
                                    std::size_t numSamples)
{
    // Fixed seed: a sweep that gives a different answer on every run would be
    // useless for deciding whether the threshold has headroom.
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> noise (-noiseAmplitude, noiseAmplitude);

    std::vector<float> out (numSamples);
    for (std::size_t i = 0; i < numSamples; ++i)
    {
        const float tone = toneAmplitude
                         * static_cast<float> (std::sin (2.0 * kPi * frequencyHz
                                                         * static_cast<double> (i) / kSampleRate));
        out[i] = tone + noise (rng);
    }
    return out;
}

// A fake howl: a tone whose amplitude ramps LINEARLY from startAmplitude to
// endAmplitude across the whole signal, over a white-noise floor. Models a
// feedback path spinning up rather than an instantaneous full-level howl, so
// the sweep can report the block at which it first crosses the threshold.
std::vector<float> makeHowlRamp (double frequencyHz,
                                 float  startAmplitude,
                                 float  endAmplitude,
                                 float  noiseAmplitude,
                                 unsigned seed,
                                 std::size_t numSamples)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> noise (-noiseAmplitude, noiseAmplitude);

    std::vector<float> out (numSamples);
    for (std::size_t i = 0; i < numSamples; ++i)
    {
        const float frac = numSamples > 1
                              ? static_cast<float> (i) / static_cast<float> (numSamples - 1)
                              : 0.0f;
        const float amplitude = startAmplitude + (endAmplitude - startAmplitude) * frac;

        const float tone = amplitude
                         * static_cast<float> (std::sin (2.0 * kPi * frequencyHz
                                                         * static_cast<double> (i) / kSampleRate));
        out[i] = tone + noise (rng);
    }
    return out;
}

// A pink-ish multi-partial programme, reusing the exact partial frequencies
// and gains tools/snapshot.cpp's makeHop() uses for its live console shot,
// plus a small white-noise floor. Generated as one continuous signal rather
// than snapshot.cpp's per-hop juce::Random draws, since this tool has no
// juce::Random-dependent module linked -- the noise is still fixed-seed and
// the partial content is identical, which is what the brief asks to reuse.
std::vector<float> makeMusicLike (float noiseAmplitude, unsigned seed, std::size_t numSamples)
{
    static const double partials[] = { 82.0, 164.0, 247.0, 392.0, 660.0,
                                       1240.0, 1920.0, 2600.0, 5200.0 };
    static const double gains[]    = { 0.28, 0.22, 0.30, 0.18, 0.12,
                                       0.16, 0.20, 0.08, 0.04 };
    constexpr int kNumPartials = static_cast<int> (std::size (partials));

    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> noise (-noiseAmplitude, noiseAmplitude);

    std::vector<float> out (numSamples);
    for (std::size_t i = 0; i < numSamples; ++i)
    {
        const double t = static_cast<double> (i) / kSampleRate;

        double sample = 0.0;
        for (int p = 0; p < kNumPartials; ++p)
            sample += gains[p] * std::sin (2.0 * kPi * partials[p] * t);

        out[i] = static_cast<float> (sample) + noise (rng);
    }
    return out;
}

//----------------------------------------------------------------------------
// Driving one scenario through the real Detector + PeakinessAnalyzer.
//----------------------------------------------------------------------------

struct ScenarioSummary
{
    std::string name;
    int         blocks           = 0;
    float       maxPeakiness     = 0.0f;
    double      freqAtMax        = 0.0;
    bool        crossed          = false;
    int         firstCrossBlock  = -1;
};

// Feeds `signal` through a fresh Detector + PeakinessAnalyzer one hop at a
// time (write kHopSize samples, process once, repeat -- never overflowing the
// kTapCapacity ring since it is drained on every write), printing one CSV row
// per block that produced a real spectrum and returning the scenario summary.
ScenarioSummary runScenario (const std::string& name, const std::vector<float>& signal)
{
    LockFreeRingBuffer<float> tap (kTapCapacity);
    Detector                  detector (kSampleRate);
    PeakinessAnalyzer         analyzer;   // default threshold: kDefaultThreshold (10.0)

    std::printf ("# scenario: %s\n", name.c_str());
    std::printf ("block,freq,peakiness,score,threshold\n");

    ScenarioSummary summary;
    summary.name = name;

    const auto   hop         = static_cast<std::size_t> (Detector::kHopSize);
    const float  threshold   = analyzer.getThreshold();
    std::size_t  pos         = 0;
    int          block       = 0;

    while (pos + hop <= signal.size())
    {
        const std::size_t written = tap.write (signal.data() + pos, hop);
        pos += hop;

        // The tap is drained by processLatestBlock() immediately below on
        // every iteration, so a full hop always has room; this guard exists
        // only so a future capacity change fails loudly instead of silently
        // skewing the block count.
        if (written != hop)
        {
            std::fprintf (stderr, "warning: tap.write truncated (%zu of %zu) in scenario %s\n",
                          written, hop, name.c_str());
        }

        const Detector::Spectrum spectrum = detector.processLatestBlock (tap);
        if (spectrum.magnitudes == nullptr)
            continue;   // no fresh block this call -- do not count it

        // Largest peakiness over every bin that has a full annulus, exactly
        // the maxPeakiness() helper in tests/test_peakiness.cpp -- this is
        // what makes the noise floor (which never crosses the threshold, and
        // so never appears in analyse()'s own candidate list) visible at all.
        float bestPeak = 0.0f;
        int   bestBin  = -1;
        for (int bin = PeakinessAnalyzer::kNeighbourOuterRadius;
             bin <= Detector::kNumBins - 1 - PeakinessAnalyzer::kNeighbourOuterRadius;
             ++bin)
        {
            const float p = PeakinessAnalyzer::peakinessAt (spectrum.magnitudes,
                                                             Detector::kNumBins, bin);
            if (p > bestPeak)
            {
                bestPeak = p;
                bestBin  = bin;
            }
        }

        const double freq        = bestBin >= 0
                                       ? static_cast<double> (bestBin) * spectrum.sampleRate
                                             / static_cast<double> (Detector::kFftSize)
                                       : 0.0;
        const bool  blockCrossed = bestPeak > threshold;
        const float score        = blockCrossed ? PeakinessAnalyzer::kCandidateScore : 0.0f;

        std::printf ("%d,%.2f,%.4f,%.1f,%.2f\n", block, freq, bestPeak, score, threshold);

        if (bestPeak > summary.maxPeakiness)
        {
            summary.maxPeakiness = bestPeak;
            summary.freqAtMax    = freq;
        }
        if (blockCrossed && ! summary.crossed)
        {
            summary.crossed         = true;
            summary.firstCrossBlock = block;
        }

        ++block;
    }

    summary.blocks = block;
    return summary;
}

void printSummaryTable (const std::vector<ScenarioSummary>& summaries)
{
    std::printf ("\n");
    std::printf ("%-20s %8s %14s %14s %10s %16s\n",
                "scenario", "blocks", "maxPeakiness", "freq@max(Hz)", "crossed?", "firstCrossBlock");
    for (const auto& s : summaries)
    {
        char crossBlockBuf[16];
        if (s.firstCrossBlock >= 0)
            std::snprintf (crossBlockBuf, sizeof (crossBlockBuf), "%d", s.firstCrossBlock);
        else
            std::snprintf (crossBlockBuf, sizeof (crossBlockBuf), "-");

        std::printf ("%-20s %8d %14.4f %14.2f %10s %16s\n",
                    s.name.c_str(), s.blocks, s.maxPeakiness, s.freqAtMax,
                    s.crossed ? "yes" : "no", crossBlockBuf);
    }
}
} // namespace

int main (int argc, char** argv)
{
    // No arguments are required -- every scenario is synthetic and
    // self-contained. (argc/argv kept for a future optional --wav path, not
    // implemented here: the brief marks that a bonus, not the deliverable.)
    (void) argc;
    (void) argv;

    std::vector<ScenarioSummary> summaries;

    // 1. noise-floor -- broadband white noise only, amplitude 1.0, seed
    //    12345, no tone. This is the number that decides whether 10.0 still
    //    has headroom at the 2048-point FFT (old baseline worst ~7.35).
    summaries.push_back (runScenario (
        "noise-floor",
        makeToneInNoise (0.0, 0.0f, 1.0f, 12345u, kSamplesPerScenario)));

    // 2. tone-1khz-in-noise -- 1 kHz sine, tone amplitude 1.0, white-noise
    //    amplitude 0.05, seed 12345. Old rig measured ~131 at ~1007.8 Hz.
    summaries.push_back (runScenario (
        "tone-1khz-in-noise",
        makeToneInNoise (1000.0, 1.0f, 0.05f, 12345u, kSamplesPerScenario)));

    // 3. howl-ramp -- 2 kHz sine ramping amplitude 0.0 -> 1.0 across the
    //    scenario, over a white-noise floor of amplitude 0.1, seed 777.
    summaries.push_back (runScenario (
        "howl-ramp",
        makeHowlRamp (2000.0, 0.0f, 1.0f, 0.1f, 777u, kSamplesPerScenario)));

    // 4. music-like -- the snapshot.cpp partial set/gains plus a small noise
    //    floor (amplitude 0.05, seed 2024). Should NOT cross the threshold.
    summaries.push_back (runScenario (
        "music-like",
        makeMusicLike (0.05f, 2024u, kSamplesPerScenario)));

    printSummaryTable (summaries);
    return 0;
}
