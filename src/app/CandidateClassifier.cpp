#include "app/CandidateClassifier.h"

#include <juce_audio_basics/juce_audio_basics.h>

// The ONLY translation unit that includes RTNeural (spec section 2). Backend
// must be the STL one: the Eigen and xsimd paths want aligned input buffers and
// were not what the real-time audit read.
#if defined (RTNEURAL_USE_EIGEN) || defined (RTNEURAL_USE_XSIMD)
 #error "CandidateClassifier requires the RTNeural STL backend: do not define RTNEURAL_USE_EIGEN or RTNEURAL_USE_XSIMD"
#endif
#include <RTNeural/RTNeural.h>

#include <algorithm>
#include <cmath>

namespace
{
using Net = RTNeural::ModelT<float, CandidateClassifier::kNumFeatures, 1,
                             RTNeural::DenseT<float, 16, 16>,
                             RTNeural::TanhActivationT<float, 16>,
                             RTNeural::DenseT<float, 16, 8>,
                             RTNeural::TanhActivationT<float, 8>,
                             RTNeural::DenseT<float, 8, 1>,
                             RTNeural::SigmoidActivationT<float, 1>>;

// Layer sizes in the Net type above must match the public constants.
static_assert (CandidateClassifier::kNumFeatures == 16 && CandidateClassifier::kHidden1 == 16
               && CandidateClassifier::kHidden2 == 8, "Net layer sizes out of sync with the header");

constexpr float kMaxAbsWeight = 1000.0f;    // anything bigger is a corrupt file, not a model
constexpr float kMaxAbsInputXform = 1.0e6f;
constexpr float kMinAbsInputScale = 1.0e-6f;   // a scale this small would erase the feature (0 = dead input)

// Valid detector band for feature 0 (the log2 would otherwise blow up on 0).
constexpr double kMinHz = 20.0;
constexpr double kMaxHz = 20000.0;

constexpr double kEps = 1.0e-12;   // magnitude floor: keeps log10 finite on silence and denormals

double clampd (double x, double lo, double hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }

double dbRelative (double mag, double centre) noexcept
{
    return clampd (20.0 * std::log10 (std::max (mag, kEps) / std::max (centre, kEps)), -60.0, 0.0);
}

bool allFinite (const CandidateClassifier::RawInput& in) noexcept
{
    if (! (std::isfinite (in.hz) && std::isfinite (in.peakiness) && std::isfinite (in.pNorm)
           && std::isfinite (in.rNorm) && std::isfinite (in.riseRatio) && std::isfinite (in.mNorm)
           && std::isfinite (in.penalty) && std::isfinite (in.lrRatio)))
        return false;
    for (const float m : in.nearMags)
        if (! std::isfinite (m))
            return false;
    return true;
}

bool numberOf (const juce::var& v, float& out) noexcept
{
    if (! (v.isInt() || v.isInt64() || v.isDouble()))
        return false;
    out = (float) (double) v;
    return true;
}

bool readVector (const juce::var& parent, const char* key, std::size_t expected,
                 std::vector<float>& out, juce::String& error)
{
    const auto* arr = parent.getProperty (key, juce::var()).getArray();
    if (arr == nullptr || (std::size_t) arr->size() != expected)
    {
        error = juce::String ("field '") + key + "' missing or wrong length";
        return false;
    }
    out.clear();
    out.reserve (expected);
    for (const auto& e : *arr)
    {
        float f = 0.0f;
        if (! numberOf (e, f))
        {
            error = juce::String ("field '") + key + "' holds a non-number";
            return false;
        }
        out.push_back (f);
    }
    return true;
}

// weights: array of `outs` arrays of `ins` numbers, flattened row-major [out][in].
bool readMatrix (const juce::var& parent, const char* key, std::size_t outs, std::size_t ins,
                 std::vector<float>& out, juce::String& error)
{
    const auto* rows = parent.getProperty (key, juce::var()).getArray();
    if (rows == nullptr || (std::size_t) rows->size() != outs)
    {
        error = juce::String ("matrix '") + key + "' missing or wrong row count";
        return false;
    }
    out.clear();
    out.reserve (outs * ins);
    for (const auto& r : *rows)
    {
        const auto* cols = r.getArray();
        if (cols == nullptr || (std::size_t) cols->size() != ins)
        {
            error = juce::String ("matrix '") + key + "' has a row of the wrong length";
            return false;
        }
        for (const auto& e : *cols)
        {
            float f = 0.0f;
            if (! numberOf (e, f))
            {
                error = juce::String ("matrix '") + key + "' holds a non-number";
                return false;
            }
            out.push_back (f);
        }
    }
    return true;
}

bool layerFromJson (const juce::var& root, const char* name, std::size_t outs, std::size_t ins,
                    std::vector<float>& w, std::vector<float>& b, juce::String& error)
{
    const auto layer = root.getProperty (name, juce::var());
    if (! layer.isObject())
    {
        error = juce::String ("layer '") + name + "' missing";
        return false;
    }
    return readMatrix (layer, "weights", outs, ins, w, error) && readVector (layer, "bias", outs, b, error);
}

bool checkFiniteBounded (const std::vector<float>& v, float bound) noexcept
{
    for (const float f : v)
        if (! std::isfinite (f) || std::fabs (f) > bound)
            return false;
    return true;
}
} // namespace

struct CandidateClassifier::Impl
{
    std::unique_ptr<Net> net;          // heap: the model holds its weights inline
    std::array<float, kNumFeatures> offset {}, scale {};
    juce::String name;
};

CandidateClassifier::CandidateClassifier() : impl (std::make_unique<Impl>()) {}
CandidateClassifier::~CandidateClassifier() = default;

bool CandidateClassifier::validate (const Weights& w, juce::String& error)
{
    if (w.featureSet != kFeatureSet)
    {
        error = "feature_set " + juce::String (w.featureSet) + " not supported (expected " + juce::String (kFeatureSet) + ")";
        return false;
    }
    if (w.dense1W.size() != (std::size_t) (kHidden1 * kNumFeatures) || w.dense1B.size() != (std::size_t) kHidden1
        || w.dense2W.size() != (std::size_t) (kHidden2 * kHidden1) || w.dense2B.size() != (std::size_t) kHidden2
        || w.dense3W.size() != (std::size_t) kHidden2 || w.dense3B.size() != 1u)
    {
        error = "weights have the wrong shape";
        return false;
    }
    for (int i = 0; i < kNumFeatures; ++i)
    {
        const float o = w.inputOffset[(std::size_t) i], s = w.inputScale[(std::size_t) i];
        if (! std::isfinite (o) || ! std::isfinite (s) || std::fabs (o) > kMaxAbsInputXform
            || std::fabs (s) > kMaxAbsInputXform
            || std::fabs (s) < kMinAbsInputScale)
        {
            error = "input_offset / input_scale not finite, absurd, or a zero / near-zero scale";
            return false;
        }
    }
    if (! (checkFiniteBounded (w.dense1W, kMaxAbsWeight) && checkFiniteBounded (w.dense1B, kMaxAbsWeight)
           && checkFiniteBounded (w.dense2W, kMaxAbsWeight) && checkFiniteBounded (w.dense2B, kMaxAbsWeight)
           && checkFiniteBounded (w.dense3W, kMaxAbsWeight) && checkFiniteBounded (w.dense3B, kMaxAbsWeight)))
    {
        error = "a weight is not finite or exceeds the sanity bound";
        return false;
    }
    return true;
}

bool CandidateClassifier::parseWeights (const juce::String& json, Weights& out, juce::String& error)
{
    juce::var root;
    if (json.isEmpty() || ! juce::JSON::parse (json, root).wasOk() || ! root.isObject())
    {
        error = "weights file is empty or not a JSON object";
        return false;
    }

    Weights w;
    const auto fs = root.getProperty ("feature_set", juce::var());
    if (! fs.isInt())
    {
        error = "feature_set missing";
        return false;
    }
    w.featureSet = (int) fs;
    if (w.featureSet != kFeatureSet)   // reject before looking at anything else
    {
        error = "feature_set " + juce::String (w.featureSet) + " not supported (expected " + juce::String (kFeatureSet) + ")";
        return false;
    }
    w.modelName = root.getProperty ("model", juce::var()).toString();

    std::vector<float> off, sc;
    if (! readVector (root, "input_offset", (std::size_t) kNumFeatures, off, error)
        || ! readVector (root, "input_scale", (std::size_t) kNumFeatures, sc, error))
        return false;
    std::copy (off.begin(), off.end(), w.inputOffset.begin());
    std::copy (sc.begin(), sc.end(), w.inputScale.begin());

    if (! layerFromJson (root, "dense1", (std::size_t) kHidden1, (std::size_t) kNumFeatures, w.dense1W, w.dense1B, error)
        || ! layerFromJson (root, "dense2", (std::size_t) kHidden2, (std::size_t) kHidden1, w.dense2W, w.dense2B, error)
        || ! layerFromJson (root, "dense3", 1u, (std::size_t) kHidden2, w.dense3W, w.dense3B, error))
        return false;

    if (! validate (w, error))
        return false;
    out = std::move (w);
    return true;
}

bool CandidateClassifier::load (const Weights& w, juce::String& error)
{
    unload();
    if (! validate (w, error))
        return false;

    auto net = std::make_unique<Net>();
    const auto rows = [] (const std::vector<float>& flat, std::size_t outs, std::size_t ins)
    {
        std::vector<std::vector<float>> m (outs, std::vector<float> (ins));
        for (std::size_t o = 0; o < outs; ++o)
            for (std::size_t i = 0; i < ins; ++i)
                m[o][i] = flat[o * ins + i];
        return m;
    };
    net->template get<0>().setWeights (rows (w.dense1W, (std::size_t) kHidden1, (std::size_t) kNumFeatures));
    net->template get<0>().setBias (w.dense1B.data());
    net->template get<2>().setWeights (rows (w.dense2W, (std::size_t) kHidden2, (std::size_t) kHidden1));
    net->template get<2>().setBias (w.dense2B.data());
    net->template get<4>().setWeights (rows (w.dense3W, 1u, (std::size_t) kHidden2));
    net->template get<4>().setBias (w.dense3B.data());

    impl->offset = w.inputOffset;
    impl->scale = w.inputScale;
    impl->name = w.modelName;
    impl->net = std::move (net);
    return true;
}

bool CandidateClassifier::loadFromJson (const juce::String& json, juce::String& error)
{
    unload();
    Weights w;
    return parseWeights (json, w, error) && load (w, error);
}

void CandidateClassifier::unload()
{
    impl->net.reset();
    impl->name = juce::String();
}

bool CandidateClassifier::isLoaded() const noexcept { return impl->net != nullptr; }
const juce::String& CandidateClassifier::modelName() const noexcept { return impl->name; }

bool CandidateClassifier::rawFeatures (const RawInput& in, std::array<float, kNumFeatures>& out) noexcept
{
    // Finite check on EVERY raw field BEFORE any clamp or log: std::max (x, NaN)
    // returns x, so a clamp-first order would launder a NaN into a number.
    if (! allFinite (in))
        return false;

    const double hz = clampd ((double) in.hz, kMinHz, kMaxHz);
    const double peakiness = std::max ((double) in.peakiness, 1.0);
    const double riseRatio = std::max ((double) in.riseRatio, kEps);
    const double centre = std::max ((double) in.nearMags[5], kEps);

    out[0] = (float) std::log2 (hz / 1000.0);
    out[1] = (float) clampd (std::log10 (peakiness), 0.0, 4.0);
    out[2] = (float) clampd ((double) in.pNorm, 0.0, 1.0);
    out[3] = (float) clampd ((double) in.rNorm, 0.0, 1.0);
    out[4] = (float) clampd (std::log2 (riseRatio), -4.0, 8.0);
    out[5] = (float) clampd ((double) in.mNorm, 0.0, 1.0);
    out[6] = (float) clampd ((double) in.penalty, 0.0, 1.0);
    out[7] = in.hasOtherLane ? 1.0f : 0.0f;
    out[8] = in.hasOtherLane
                 ? (float) clampd (std::log10 (std::max ((double) in.lrRatio, kEps)), -3.0, 3.0)
                 : 0.0f;
    out[9]  = (float) dbRelative ((double) in.nearMags[3], centre);
    out[10] = (float) dbRelative ((double) in.nearMags[4], centre);
    out[11] = (float) dbRelative ((double) in.nearMags[6], centre);
    out[12] = (float) dbRelative ((double) in.nearMags[7], centre);
    // Skirts: mean LINEAR magnitude of bins -5..-3 and +3..+5, then dB vs centre.
    out[13] = (float) dbRelative (((double) in.nearMags[0] + (double) in.nearMags[1] + (double) in.nearMags[2]) / 3.0, centre);
    out[14] = (float) dbRelative (((double) in.nearMags[8] + (double) in.nearMags[9] + (double) in.nearMags[10]) / 3.0, centre);
    out[15] = (float) clampd (std::log10 (centre), -12.0, 6.0);
    return true;
}

std::optional<float> CandidateClassifier::score (const RawInput& in) noexcept
{
    if (impl->net == nullptr)
        return std::nullopt;

    std::array<float, kNumFeatures> x {};
    if (! rawFeatures (in, x))
        return std::nullopt;

    // Denormals make tanh/exp slow on x86; flush them for the forward pass only.
    const juce::ScopedNoDenormals noDenormals;

    for (std::size_t i = 0; i < (std::size_t) kNumFeatures; ++i)
        x[i] = (x[i] - impl->offset[i]) * impl->scale[i];

    const float y = impl->net->forward (x.data());
    if (! std::isfinite (y))
        return std::nullopt;
    return std::min (1.0f, std::max (0.0f, y));
}
