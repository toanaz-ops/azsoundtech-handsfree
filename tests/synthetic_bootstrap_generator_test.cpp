// Lane C1c, spec 2026-11-09-candidate-classifier-design.md section 8.4:
// the synthetic bootstrap generator. A DISABLED opt-in test, never part of the
// normal suite:
//
//   HF_SYNTH_OUT_DIR=<dir> HandsFreeTests.exe
//       --gtest_also_run_disabled_tests --gtest_filter=SyntheticBootstrap.*
//
// It feeds synthetic audio scenes through a REAL NotchController (real
// detector, scorer, spectral context, shadow classifier) and serialises the
// events with the app's own serialisers, so a notch_set line is the very line
// the app would write. The label is the SCENE, by construction: a howl scene
// (a tone that grows, or steps in) -> verdict "good"; a music scene (an
// enveloped harmonic note, with or without vibrato) -> verdict "false". Only
// scenes in which the real detector actually placed a notch produce a labelled
// row. What this does and does not prove: tools/nn/README.md.
//
// The generator changes nothing in the app. Expected level change: 0 dB.

#include <gtest/gtest.h>

#include "app/EmbeddedClassifier.h"
#include "app/MainComponent.h"
#include "app/NotchController.h"
#include "app/SessionLogger.h"
#include "dsp/PeakinessAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace
{
constexpr double kPi      = 3.14159265358979323846;
constexpr double kSr      = 48000.0;
constexpr double kBlockMs = Detector::kHopSize / kSr * 1000.0;
constexpr int    kWarmupBlocks = 64;     // same warm-up as the detector tests
constexpr int    kMaxSceneBlocks = 260;  // ~2.8 s of scene after the warm-up
constexpr double kClockStartMs = 1000.0; // FakeClock origin

class FakeClock : public ClockSource
{
public:
    double nowMs() const override { return ms_; }
    void advance (double m) { ms_ += m; }
private:
    double ms_ = kClockStartMs;
};

// Portable uniform draws: mt19937 itself is specified by the standard, the
// std::*_distribution classes are not, so we do not use them.
struct Rng
{
    std::mt19937 g;
    explicit Rng (std::uint32_t seed) : g (seed) {}
    double u() { return (double) (g() >> 8) / 16777216.0; }                 // [0,1)
    double u (double a, double b) { return a + (b - a) * u(); }
    int    i (int a, int b) { return a + (int) (u() * (double) (b - a + 1)); }  // [a,b]
    bool   coin (double p) { return u() < p; }
};

struct Voice
{
    virtual ~Voice() = default;
    virtual void hop (float* out) = 0;     // fills Detector::kHopSize samples
};

// A feedback howl: one tone that grows slowly (ramp) or steps in (hard onset),
// starting BELOW the noise floor like the detector tests' RampSineSource.
struct HowlVoice : Voice
{
    double freq = 1000.0, drift = 0.0;
    double amp = 1.0e-4, gainDbPerMs = 0.04;
    bool   step = false;
    int    stepAtBlock = 0;
    float  stepAmp = 0.3f;
    double harm2Db = -200.0;               // second harmonic level, -200 = none
    double phase = 0.0, phase2 = 0.0;
    int    block = 0;

    void hop (float* out) override
    {
        const double a = (step && block >= stepAtBlock) ? (double) stepAmp : amp;
        const double h2 = std::pow (10.0, harm2Db / 20.0);
        const double f = freq + drift * std::sin (2.0 * kPi * 0.3 * block * kBlockMs / 1000.0);
        for (int i = 0; i < Detector::kHopSize; ++i)
        {
            out[i] = (float) (a * (std::sin (phase) + h2 * std::sin (phase2)));
            phase += 2.0 * kPi * f / kSr;
            phase2 += 2.0 * kPi * 2.0 * f / kSr;
        }
        if (! step)
            amp = std::min (0.9, amp * std::pow (10.0, gainDbPerMs * kBlockMs / 20.0));
        ++block;
    }
};

// Not a howl: an enveloped harmonic note, optionally with vibrato and a long
// sustain (the organ / pad case that looks most like a howl).
struct MusicVoice : Voice
{
    double f0 = 220.0, alpha = 1.0;
    int    nHarm = 6;
    double vibCents = 0.0, vibHz = 5.5;
    double attackMs = 20.0, tauS = 1.5, peak = 0.2;
    int    onsetBlock = 0;
    double phase[12] = {};
    int    block = 0;
    double norm = 1.0;

    void prepare()
    {
        double s = 0.0;
        for (int k = 1; k <= nHarm; ++k)
            s += std::pow ((double) k, -alpha);
        norm = 1.0 / s;
    }

    void hop (float* out) override
    {
        for (int i = 0; i < Detector::kHopSize; ++i)
        {
            const double tS = ((double) (block - onsetBlock) * Detector::kHopSize + i) / kSr;
            double env = 0.0;
            if (block >= onsetBlock)
                env = peak * (1.0 - std::exp (-3.0 * tS * 1000.0 / attackMs)) * std::exp (-tS / tauS);
            const double f = f0 * std::pow (2.0, vibCents / 1200.0 * std::sin (2.0 * kPi * vibHz * tS));
            double s = 0.0;
            for (int k = 1; k <= nHarm; ++k)
            {
                s += std::pow ((double) k, -alpha) * std::sin (phase[k - 1]);
                phase[k - 1] += 2.0 * kPi * (double) k * f / kSr;
            }
            out[i] = (float) (env * norm * s);
        }
        ++block;
    }
};

struct TimedEvent  { double tMs; NotchController::NotchEvent e; };
struct TimedCand   { double tMs; NotchController::CandidateRecord r; };

struct SceneResult
{
    bool placed = false;
    NotchController::NotchEvent set;       // first detector Set
    double setT = 0.0;                     // scene-local ms
    double endT = 0.0;                     // scene-local ms of the last block
    std::vector<TimedCand> cands;
    std::string modelName;
};

SceneResult runScene (Voice& voice, bool stereo, double noiseAmp, double gainL, double gainR, Rng& rng)
{
    SceneResult out;
    LockFreeRingBuffer<float> tapL { 8192 }, tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    FakeClock clock;
    std::unique_ptr<NotchController> ctl = stereo
        ? std::make_unique<NotchController> (tapL, &tapR, commands, clock)
        : std::make_unique<NotchController> (tapL, commands, clock);

    std::vector<TimedEvent> events;
    ctl->setEventSink ([&] (const NotchController::NotchEvent& e) { events.push_back ({ clock.nowMs() - kClockStartMs, e }); });
    ctl->setCandidateSink ([&] (const NotchController::CandidateRecord& r)
                           { out.cands.push_back ({ clock.nowMs() - kClockStartMs, r }); });
    juce::String error;
    auto classifier = makeEmbeddedCandidateClassifier (error);
    if (classifier != nullptr)
        out.modelName = classifier->modelName().toStdString();
    ctl->setCandidateClassifier (std::move (classifier));
    ctl->setDetectionActive (true);

    std::vector<float> sig ((std::size_t) Detector::kHopSize), l (sig.size()), r (sig.size());
    auto noise = [&] (std::vector<float>& v)
    {
        for (auto& x : v)
            x = (float) (noiseAmp * (rng.u() * 2.0 - 1.0));
    };
    auto pump = [&]
    {
        tapL.write (l.data(), l.size());
        if (stereo)
            tapR.write (r.data(), r.size());
        clock.advance (kBlockMs);
        ctl->runOnce();
        NotchCommand cmd {};
        while (commands.read (&cmd, 1) == 1) {}      // the app side would apply these; we only log
    };

    for (int b = 0; b < kWarmupBlocks; ++b)
    {
        noise (l); noise (r);
        pump();
    }
    for (int b = 0; b < kMaxSceneBlocks; ++b)
    {
        voice.hop (sig.data());
        noise (l); noise (r);
        for (std::size_t i = 0; i < sig.size(); ++i)
        {
            l[i] += (float) (gainL * sig[i]);
            r[i] += (float) (gainR * sig[i]);
        }
        pump();
        out.endT = clock.nowMs() - kClockStartMs;
        for (const auto& te : events)
            if (te.e.kind == NotchController::NotchEvent::Kind::Set && te.e.hasScore
                && te.e.origin == NotchController::Origin::Detector)
            {
                out.placed = true;
                out.set = te.e;
                out.setT = te.tMs;
                break;
            }
        if (out.placed)
            break;
    }
    return out;
}

struct SessionPlan
{
    const char* name;
    std::uint32_t seed;
    double noiseAmp;      // background noise (uniform, peak amplitude)
    double stereoShare;   // fraction of scenes through a stereo controller
    double howlShare;     // fraction of attempted scenes that are howls
};

// Fixed plan: eight sessions, different noise floors and mono / stereo mixes.
const SessionPlan kPlans[] = {
    { "s0", 1001u, 0.005, 0.5, 0.50 },
    { "s1", 1002u, 0.010, 0.5, 0.50 },
    { "s2", 1003u, 0.020, 0.5, 0.50 },
    { "s3", 1004u, 0.010, 0.0, 0.50 },
    { "s4", 1005u, 0.010, 1.0, 0.50 },
    { "s5", 1006u, 0.030, 0.5, 0.50 },
    { "s6", 1007u, 0.005, 0.5, 0.50 },
    { "s7", 1008u, 0.015, 0.5, 0.50 },
};

void writeLine (std::ofstream& f, const juce::var& v, double tMs)
{
    juce::var stamped (new juce::DynamicObject (*v.getDynamicObject()));
    stamped.getDynamicObject()->setProperty ("t", tMs);
    f << juce::JSON::toString (stamped, true).toStdString() << "\n";
}
} // namespace

TEST (SyntheticBootstrap, DISABLED_GenerateSessionLogs)
{
    const char* dirEnv = std::getenv ("HF_SYNTH_OUT_DIR");
    if (dirEnv == nullptr || *dirEnv == '\0')
        GTEST_SKIP() << "set HF_SYNTH_OUT_DIR to the output directory";
    const char* scenesEnv = std::getenv ("HF_SYNTH_SCENES");
    const int scenesPerSession = scenesEnv != nullptr ? std::max (1, std::atoi (scenesEnv)) : 90;

    const juce::File outDir (juce::String::fromUTF8 (dirEnv));
    ASSERT_TRUE (outDir.createDirectory().wasOk());

    int totalHowl = 0, totalMusic = 0, attemptedHowl = 0, attemptedMusic = 0;
    for (const auto& plan : kPlans)
    {
        Rng rng (plan.seed);
        const auto path = outDir.getChildFile (juce::String ("session-synthetic-v0-") + plan.name + ".jsonl");
        std::ofstream f (path.getFullPathName().toStdString(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE (f.good());

        std::string modelName;
        std::vector<std::pair<double, juce::var>> lines;    // (t, event), written after the header
        double sessionT = 1000.0;
        int sceneNo = 0, howl = 0, music = 0;

        for (int n = 0; n < scenesPerSession; ++n)
        {
            const bool isHowl = rng.coin (plan.howlShare);
            const bool stereo = rng.coin (plan.stereoShare);
            const double noiseAmp = plan.noiseAmp * rng.u (0.7, 1.4);
            // the same source heard at different levels by the two mics
            const double gainL = rng.u (0.3, 1.0);
            const double gainR = stereo ? rng.u (0.05, 1.0) : 1.0;

            std::unique_ptr<Voice> voice;
            if (isHowl)
            {
                auto v = std::make_unique<HowlVoice>();
                v->freq = std::exp (rng.u (std::log (150.0), std::log (9000.0)));
                v->drift = rng.coin (0.5) ? rng.u (0.0, 1.5) : 0.0;
                v->amp = noiseAmp * rng.u (0.01, 0.03);
                v->gainDbPerMs = rng.u (0.02, 0.09);
                v->step = rng.coin (0.25);
                v->stepAtBlock = rng.i (4, 30);
                v->stepAmp = (float) rng.u (0.05, 0.5);
                v->harm2Db = rng.coin (0.3) ? rng.u (-40.0, -22.0) : -200.0;
                voice = std::move (v);
                ++attemptedHowl;
            }
            else
            {
                auto v = std::make_unique<MusicVoice>();
                v->f0 = std::exp (rng.u (std::log (80.0), std::log (1200.0)));
                v->nHarm = rng.i (4, 10);
                v->alpha = rng.u (0.5, 1.5);
                const bool sustained = rng.coin (0.2);       // organ / pad: long sustain, no vibrato
                v->vibCents = sustained ? 0.0 : (rng.coin (0.7) ? rng.u (5.0, 45.0) : 0.0);
                v->vibHz = rng.u (4.5, 6.5);
                v->attackMs = rng.u (3.0, 150.0);
                v->tauS = sustained ? rng.u (6.0, 30.0) : rng.u (0.4, 3.0);
                v->peak = rng.u (0.05, 0.5);
                v->onsetBlock = rng.i (0, 10);
                v->prepare();
                voice = std::move (v);
                ++attemptedMusic;
            }

            const auto res = runScene (*voice, stereo, noiseAmp, gainL, gainR, rng);
            if (modelName.empty())
                modelName = res.modelName;
            const double base = sessionT;
            for (const auto& c : res.cands)
                lines.push_back ({ base + c.tMs, MainComponent::candidateToVarForTest (c.r) });
            if (res.placed)
            {
                auto e = res.set;
                e.slot = sceneNo % 8;               // unique (slot, lane, index) per scene so the
                e.index = (sceneNo / 8) % 16;       // verdict join never meets a reused key
                auto setVar = MainComponent::notchEventToVarForTest (e);
                lines.push_back ({ base + res.setT, setVar });

                auto v = SessionLogger::makeEvent ("verdict");
                auto* o = v.getDynamicObject();
                o->setProperty ("slot", e.slot);
                o->setProperty ("lane", e.lane);
                o->setProperty ("index", e.index);
                o->setProperty ("hz", (double) e.hz);
                o->setProperty ("verdict", isHowl ? "good" : "false");
                o->setProperty ("age_ms", 1000.0);
                lines.push_back ({ base + res.setT + 1000.0, v });
                (isHowl ? howl : music)++;
            }
            ++sceneNo;
            sessionT = base + res.endT + 4000.0;   // quiet gap between scenes
        }

        auto header = SessionLogger::makeEvent ("session_start");
        auto* h = header.getDynamicObject();
        h->setProperty ("log_schema", 3);
        h->setProperty ("source", "synthetic");                       // read by tools/nn/dataset.py
        h->setProperty ("generator", "hf-synthetic-bootstrap-v0");
        h->setProperty ("nn_model", juce::String (modelName));
        h->setProperty ("nn_feature_set", CandidateClassifier::kFeatureSet);
        h->setProperty ("app_version", "synthetic");
        h->setProperty ("os", "synthetic");
        h->setProperty ("device", "synthetic");
        h->setProperty ("sample_rate", kSr);
        h->setProperty ("buffer_size", Detector::kHopSize);
        writeLine (f, header, 0.0);
        for (const auto& [t, v] : lines)
            writeLine (f, v, t);
        auto end = SessionLogger::makeEvent ("session_end");
        end.getDynamicObject()->setProperty ("dropped_events", 0);
        end.getDynamicObject()->setProperty ("dropped_candidates", 0);
        end.getDynamicObject()->setProperty ("write_failed", false);
        writeLine (f, end, sessionT);

        std::printf ("%s: attempted %d, placed howl %d, placed music %d\n", plan.name, scenesPerSession, howl, music);
        totalHowl += howl;
        totalMusic += music;
    }
    std::printf ("TOTAL: howl scenes attempted %d placed %d; music scenes attempted %d placed %d\n",
                 attemptedHowl, totalHowl, attemptedMusic, totalMusic);
    EXPECT_GT (totalHowl, 0);
    EXPECT_GT (totalMusic, 0) << "the real detector placed no notch on any music scene: "
                                 "fall back to the feature-space generator (see tools/nn/README.md)";
}
