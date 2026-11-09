// Lane C1a: the CandidateClassifier runtime (spec 2026-11-09-candidate-classifier-design.md
// section 4). The allocation gate lives in its own executable
// (candidate_classifier_alloc_test.cpp): replacing operator new here would
// override the sanitizer allocators of the nightly jobs.

#include <gtest/gtest.h>

#include "app/CandidateClassifier.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
using RawInput = CandidateClassifier::RawInput;

juce::File fixtureFile (const char* name)
{
    return juce::File (juce::String (HANDSFREE_SOURCE_DIR)).getChildFile ("tests/fixtures").getChildFile (name);
}

juce::String weightsJson() { return fixtureFile ("classifier-fixture-v1.json").loadFileAsString(); }

RawInput inputFromVar (const juce::var& v)
{
    RawInput in;
    in.hz = (float) (double) v["hz"];
    in.peakiness = (float) (double) v["peakiness"];
    in.pNorm = (float) (double) v["pNorm"];
    in.rNorm = (float) (double) v["rNorm"];
    in.riseRatio = (float) (double) v["riseRatio"];
    in.mNorm = (float) (double) v["mNorm"];
    in.penalty = (float) (double) v["penalty"];
    in.hasOtherLane = (bool) v["hasOtherLane"];
    in.lrRatio = (float) (double) v["lrRatio"];
    const auto* near = v["nearMags"].getArray();
    for (int i = 0; i < CandidateClassifier::kNearBins; ++i)
        in.nearMags[(std::size_t) i] = (float) (double) (*near)[i];
    return in;
}

RawInput typicalInput()
{
    RawInput in;
    in.hz = 1250.0f; in.peakiness = 14.0f; in.pNorm = 0.8f; in.rNorm = 0.7f; in.riseRatio = 3.0f;
    in.mNorm = 0.6f; in.penalty = 1.0f; in.hasOtherLane = true; in.lrRatio = 2.0f;
    const float prof[11] = { 0.002f, 0.003f, 0.01f, 0.05f, 0.3f, 1.0f, 0.3f, 0.05f, 0.01f, 0.003f, 0.002f };
    std::copy (prof, prof + 11, in.nearMags.begin());
    return in;
}

class CandidateClassifierTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        juce::String error;
        ASSERT_TRUE (cls.loadFromJson (weightsJson(), error)) << error;
    }
    CandidateClassifier cls;
};
} // namespace

TEST_F (CandidateClassifierTest, MatchesPythonReferenceWithinTolerance)
{
    juce::var parity;
    ASSERT_TRUE (juce::JSON::parse (fixtureFile ("classifier-parity-v1.json").loadFileAsString(), parity).wasOk());
    const double tol = (double) parity["tolerance"];
    const auto* cases = parity["cases"].getArray();
    ASSERT_NE (cases, nullptr);
    ASSERT_GE (cases->size(), 64);

    double worst = 0.0;
    int mono = 0, stereo = 0;
    for (const auto& c : *cases)
    {
        const RawInput in = inputFromVar (c["input"]);
        (in.hasOtherLane ? stereo : mono)++;
        const auto got = cls.score (in);
        ASSERT_TRUE (got.has_value());
        worst = std::max (worst, std::fabs ((double) *got - (double) c["expected"]));
    }
    std::printf ("[ C1a ] parity cases=%d max_abs_error=%.3e tolerance=%.1e\n", cases->size(), worst, tol);
    EXPECT_GT (mono, 5);
    EXPECT_GT (stereo, 5);
    EXPECT_LE (worst, tol);
}

TEST_F (CandidateClassifierTest, NonFiniteInputGivesNoOpinion)
{
    ASSERT_TRUE (cls.score (typicalInput()).has_value());
    const float bad[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity() };
    for (const float b : bad)
    {
        const auto check = [&] (const char* what, auto mutate)
        {
            RawInput in = typicalInput();
            mutate (in, b);
            EXPECT_FALSE (cls.score (in).has_value()) << what << " = " << b;
        };
        check ("hz", [] (RawInput& i, float v) { i.hz = v; });
        check ("peakiness", [] (RawInput& i, float v) { i.peakiness = v; });
        check ("pNorm", [] (RawInput& i, float v) { i.pNorm = v; });
        check ("rNorm", [] (RawInput& i, float v) { i.rNorm = v; });
        check ("riseRatio", [] (RawInput& i, float v) { i.riseRatio = v; });
        check ("mNorm", [] (RawInput& i, float v) { i.mNorm = v; });
        check ("penalty", [] (RawInput& i, float v) { i.penalty = v; });
        check ("lrRatio", [] (RawInput& i, float v) { i.lrRatio = v; });
        check ("lrRatio (mono)", [] (RawInput& i, float v) { i.hasOtherLane = false; i.lrRatio = v; });
        for (int k = 0; k < CandidateClassifier::kNearBins; ++k)
            check ("nearMags", [k] (RawInput& i, float v) { i.nearMags[(std::size_t) k] = v; });
    }
}

TEST_F (CandidateClassifierTest, DenormalInputsStayFiniteAndFast)
{
    RawInput in = typicalInput();
    in.nearMags.fill (1.0e-40f);   // denormal float
    in.riseRatio = 1.0e-40f;
    in.lrRatio = 1.0e-40f;
    in.peakiness = 1.0e-40f;
    in.pNorm = 1.0e-40f;
    const auto t0 = std::chrono::steady_clock::now();
    std::optional<float> s;
    for (int i = 0; i < 1000; ++i)
        s = cls.score (in);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE (s.has_value());
    EXPECT_TRUE (std::isfinite (*s));
    EXPECT_GE (*s, 0.0f);
    EXPECT_LE (*s, 1.0f);
    EXPECT_LT (us, 200000) << "1000 denormal scores took " << us << " us";
}

TEST_F (CandidateClassifierTest, ExtremeFiniteInputsAreClamped)
{
    RawInput in = typicalInput();
    in.riseRatio = 1.0e12f;
    in.lrRatio = 1.0e-12f;
    in.peakiness = 1.0e30f;
    in.hz = 1.0e9f;
    in.pNorm = 5.0f;
    in.nearMags[5] = 3.0e38f;
    auto s = cls.score (in);
    ASSERT_TRUE (s.has_value());
    EXPECT_TRUE (std::isfinite (*s));
    EXPECT_GE (*s, 0.0f);
    EXPECT_LE (*s, 1.0f);

    in.riseRatio = 0.0f; in.lrRatio = 0.0f; in.peakiness = -5.0f; in.hz = -3.0f; in.pNorm = -2.0f;
    in.nearMags.fill (0.0f);
    s = cls.score (in);
    ASSERT_TRUE (s.has_value());
    EXPECT_GE (*s, 0.0f);
    EXPECT_LE (*s, 1.0f);

    // The clamp, not luck: a ratio beyond the clamp scores exactly like the clamp edge.
    RawInput a = typicalInput(), b = typicalInput();
    a.riseRatio = 256.0f; b.riseRatio = 1.0e12f;   // log2 clamp is 8 == log2 (256)
    EXPECT_EQ (*cls.score (a), *cls.score (b));
}

TEST_F (CandidateClassifierTest, RawFeaturesFollowTheSpecTable)
{
    std::array<float, CandidateClassifier::kNumFeatures> f {};
    ASSERT_TRUE (CandidateClassifier::rawFeatures (typicalInput(), f));
    EXPECT_FLOAT_EQ (f[0], (float) std::log2 (1.25));
    EXPECT_NEAR (f[1], std::log10 (14.0), 1e-6);
    EXPECT_FLOAT_EQ (f[7], 1.0f);
    EXPECT_NEAR (f[8], std::log10 (2.0), 1e-6);
    EXPECT_NEAR (f[9], 20.0 * std::log10 (0.05), 1e-4);
    EXPECT_NEAR (f[10], 20.0 * std::log10 (0.3), 1e-4);
    EXPECT_NEAR (f[15], 0.0, 1e-6);

    RawInput mono = typicalInput();
    mono.hasOtherLane = false;
    ASSERT_TRUE (CandidateClassifier::rawFeatures (mono, f));
    EXPECT_FLOAT_EQ (f[7], 0.0f);
    EXPECT_FLOAT_EQ (f[8], 0.0f);

    RawInput louder = typicalInput();
    louder.nearMags[4] = 10.0f;   // neighbour louder than the centre clamps to 0 dB
    ASSERT_TRUE (CandidateClassifier::rawFeatures (louder, f));
    EXPECT_FLOAT_EQ (f[10], 0.0f);
}

TEST (CandidateClassifierLoad, MissingOrCorruptWeightsLeaveItInactive)
{
    CandidateClassifier c;
    juce::String error;
    const juce::String good = weightsJson();
    ASSERT_TRUE (c.loadFromJson (good, error)) << error;
    ASSERT_TRUE (c.isLoaded());

    const auto expectRejected = [&] (const juce::String& json, const char* what)
    {
        // Start from a LOADED classifier: a failed load must also take it down,
        // not keep scoring with the previous model.
        ASSERT_TRUE (c.loadFromJson (good, error)) << error;
        juce::String err;
        EXPECT_FALSE (c.loadFromJson (json, err)) << what;
        EXPECT_FALSE (err.isEmpty()) << what;
        EXPECT_FALSE (c.isLoaded()) << what;
        EXPECT_FALSE (c.score (typicalInput()).has_value()) << what;
    };

    expectRejected ({}, "empty");
    expectRejected ("{}", "empty object");
    expectRejected ("not json at all", "garbage");
    expectRejected (good.substring (0, good.length() / 2), "truncated");

    const auto mutated = [&] (auto edit)
    {
        juce::var v;
        EXPECT_TRUE (juce::JSON::parse (good, v).wasOk());
        edit (v);
        return juce::JSON::toString (v, true);
    };
    expectRejected (mutated ([] (juce::var& v) { v.getDynamicObject()->setProperty ("feature_set", 2); }),
                    "wrong feature_set");
    expectRejected (mutated ([] (juce::var& v) { v.getDynamicObject()->removeProperty ("feature_set"); }),
                    "missing feature_set");
    expectRejected (mutated ([] (juce::var& v) { v.getDynamicObject()->removeProperty ("dense3"); }),
                    "missing layer");
    expectRejected (mutated ([] (juce::var& v) { v["dense1"]["weights"].getArray()->removeLast(); }),
                    "wrong row count");
    expectRejected (mutated ([] (juce::var& v) { (*v["dense2"]["weights"].getArray())[0].getArray()->removeLast(); }),
                    "wrong row length");
    expectRejected (mutated ([] (juce::var& v) { v["input_scale"].getArray()->removeLast(); }),
                    "wrong input_scale length");
    expectRejected (mutated ([] (juce::var& v) { (*v["dense1"]["weights"].getArray())[3].getArray()->set (2, "x"); }),
                    "non-numeric weight");
    expectRejected (mutated ([] (juce::var& v) { (*v["dense3"]["weights"].getArray())[0].getArray()->set (0, 1.0e9); }),
                    "absurd weight");
    expectRejected (mutated ([] (juce::var& v) { v["input_offset"].getArray()->removeLast(); }),
                    "wrong input_offset length");
    expectRejected (mutated ([] (juce::var& v) { v["input_offset"].getArray()->set (3, 1.0e9); }),
                    "absurd input_offset");
    expectRejected (mutated ([] (juce::var& v) { v["dense1"]["bias"].getArray()->removeLast(); }),
                    "wrong dense1 bias length");
    expectRejected (mutated ([] (juce::var& v) { v["dense3"]["bias"].getArray()->add (0.0); }),
                    "wrong dense3 bias length");
    expectRejected (mutated ([] (juce::var& v) { v["input_scale"].getArray()->set (2, 0.0); }),
                    "zero input_scale");
    expectRejected (mutated ([] (juce::var& v) { v["input_scale"].getArray()->set (2, 1.0e-9); }),
                    "near-zero input_scale");

    // NaN / inf cannot be written as JSON, so go through the Weights overload.
    CandidateClassifier::Weights w;
    ASSERT_TRUE (CandidateClassifier::parseWeights (good, w, error)) << error;
    const float bad[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() };
    for (const float b : bad)
    {
        for (int which = 0; which < 5; ++which)
        {
            CandidateClassifier::Weights x = w;
            if (which == 4) x.inputOffset[6] = b;
            if (which == 0) x.dense1W[7] = b;
            if (which == 1) x.dense2B[1] = b;
            if (which == 2) x.inputScale[4] = b;
            if (which == 3) x.dense3W[0] = b;
            ASSERT_TRUE (c.loadFromJson (good, error)) << error;
            juce::String err;
            EXPECT_FALSE (c.load (x, err)) << "non-finite weight variant " << which;
            EXPECT_FALSE (c.isLoaded());
            EXPECT_FALSE (c.score (typicalInput()).has_value());
        }
    }
    {
        CandidateClassifier::Weights z = w;
        z.inputScale[0] = 0.0f;
        juce::String err;
        EXPECT_FALSE (c.load (z, err));
        EXPECT_FALSE (c.isLoaded());
    }
    EXPECT_TRUE (c.loadFromJson (good, error)) << error;   // and it recovers
    EXPECT_EQ (c.modelName(), juce::String ("fixture-v1-seeded-random"));
}

TEST (CandidateClassifierLoad, UnloadedClassifierHasNoOpinion)
{
    CandidateClassifier c;
    EXPECT_FALSE (c.isLoaded());
    EXPECT_FALSE (c.score (typicalInput()).has_value());
    juce::String error;
    ASSERT_TRUE (c.loadFromJson (weightsJson(), error)) << error;
    EXPECT_TRUE (c.score (typicalInput()).has_value());
    c.unload();
    EXPECT_FALSE (c.score (typicalInput()).has_value());
}

TEST (CandidateClassifierBench, ScoreCostIsReported)
{
    CandidateClassifier c;
    juce::String error;
    ASSERT_TRUE (c.loadFromJson (weightsJson(), error)) << error;
    const RawInput in = typicalInput();

    constexpr int kBatches = 15, kCalls = 20000;
    std::vector<double> nsPerCall;
    float sink = 0.0f;
    for (int b = 0; b < kBatches; ++b)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kCalls; ++i)
            sink += c.score (in).value_or (0.0f);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now() - t0).count();
        nsPerCall.push_back ((double) ns / kCalls);
    }
    std::sort (nsPerCall.begin(), nsPerCall.end());
    const double median = nsPerCall[nsPerCall.size() / 2];
    std::printf ("[ C1a ] score() median %.0f ns/call (min %.0f, max %.0f), sink %.3f\n",
                 median, nsPerCall.front(), nsPerCall.back(), (double) sink);
    EXPECT_LT (median, 50000.0);   // loose ceiling: 50 us
}

// Lane C1c: a file written by tools/nn/train_mlp.py (the synthetic bootstrap
// model, never embedded in the app) must go through the app's own parser and
// RTNeural path and score like the Python reference. Proves the exporter writes
// what parseWeights reads, and that the larger weights a trained model carries
// stay inside the tolerance C1a measured on the random fixture.
TEST (CandidateClassifierLoad, ExportedTrainedModelLoadsAndMatchesPythonReference)
{
    const auto dir = juce::File (juce::String (HANDSFREE_SOURCE_DIR)).getChildFile ("tools/nn/models");
    const auto weightsFile = dir.getChildFile ("synthetic-bootstrap-v0.json");
    const auto parityFile = dir.getChildFile ("synthetic-bootstrap-v0.parity.json");
    ASSERT_TRUE (weightsFile.existsAsFile());
    ASSERT_TRUE (parityFile.existsAsFile());

    CandidateClassifier cls;
    juce::String error;
    ASSERT_TRUE (cls.loadFromJson (weightsFile.loadFileAsString(), error)) << error;
    EXPECT_EQ (cls.modelName(), juce::String ("synthetic-bootstrap-v0"));

    juce::var parity;
    ASSERT_TRUE (juce::JSON::parse (parityFile.loadFileAsString(), parity).wasOk());
    const double tol = (double) parity["tolerance"];
    const auto* cases = parity["cases"].getArray();
    ASSERT_NE (cases, nullptr);
    ASSERT_GE (cases->size(), 8);

    double worst = 0.0;
    int stereo = 0;
    for (const auto& c : *cases)
    {
        const RawInput in = inputFromVar (c["input"]);
        stereo += in.hasOtherLane ? 1 : 0;
        const auto got = cls.score (in);
        ASSERT_TRUE (got.has_value());
        worst = std::max (worst, std::fabs ((double) *got - (double) c["expected"]));
    }
    std::printf ("[ C1c ] trained-model parity cases=%d (stereo %d) max_abs_error=%.3e tolerance=%.1e\n",
                 cases->size(), stereo, worst, tol);
    EXPECT_LE (worst, tol);
}
