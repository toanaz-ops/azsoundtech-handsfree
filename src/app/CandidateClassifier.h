#pragma once

// Lane C1 (candidate classifier), step C1a: the runtime only. Nothing in the
// app constructs this yet -- C1b wires it into the detector thread in shadow
// mode. Spec: docs/superpowers/specs/2026-11-09-candidate-classifier-design.md.
//
// A tiny MLP (16 -> 16 tanh -> 8 tanh -> 1 sigmoid, 417 parameters) that turns
// the raw scalar features of a feedback candidate into one number in 0..1.
// "No opinion" (std::nullopt) is a first-class answer and is NEVER turned into
// a decision by this class.
//
// Threading: one instance per NotchController. score() mutates the model's
// scratch state (RTNeural has no synchronisation), so it must be called from a
// single thread, and load() / unload() only while no thread is inside score().

#include <juce_core/juce_core.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

class CandidateClassifier
{
public:
    static constexpr int kFeatureSet   = 1;
    static constexpr int kNumFeatures  = 16;
    static constexpr int kHidden1      = 16;
    static constexpr int kHidden2      = 8;
    static constexpr int kNearBins     = 11;   // bins -5..+5 around the peak, centre at index 5

    // Raw, unclamped detector-side values. A struct of its own so this class
    // does not depend on NotchController (and tests need no detector).
    struct RawInput
    {
        float hz = 0.0f;
        float peakiness = 0.0f;       // unbounded ratio, not the 0..1 score
        float pNorm = 0.0f;           // 0..1
        float rNorm = 0.0f;           // 0..1 (the saturating one)
        float riseRatio = 1.0f;       // unbounded ratio, neutral 1.0
        float mNorm = 0.0f;           // 0..1
        float penalty = 1.0f;         // 0.5 or 1
        bool  hasOtherLane = false;   // lrRatio is meaningful only when true
        float lrRatio = 0.0f;
        std::array<float, kNearBins> nearMags {};   // linear magnitudes, bins -5..+5
    };

    // Everything a weights file carries. Dense layers are [out][in] row-major.
    struct Weights
    {
        int featureSet = 0;
        juce::String modelName;
        std::array<float, kNumFeatures> inputOffset {};
        std::array<float, kNumFeatures> inputScale {};
        std::vector<float> dense1W, dense1B;   // 16x16, 16
        std::vector<float> dense2W, dense2B;   // 8x16, 8
        std::vector<float> dense3W, dense3B;   // 1x8, 1
    };

    CandidateClassifier();
    ~CandidateClassifier();
    CandidateClassifier (const CandidateClassifier&) = delete;
    CandidateClassifier& operator= (const CandidateClassifier&) = delete;

    // Reads the app's own weights JSON (juce::JSON, off the detector thread --
    // deliberately NOT RTNeural's nlohmann loader, which allocates). Rejects a
    // wrong feature_set, wrong shapes, missing fields, non-finite or absurd
    // numbers. `error` is set on failure.
    static bool parseWeights (const juce::String& json, Weights& out, juce::String& error);

    // Same checks on an already-built Weights (what load() runs first).
    static bool validate (const Weights& w, juce::String& error);

    // Builds the model on the heap. On failure the classifier is left (or put)
    // in the inactive state and `error` says why. Call only while no thread is
    // inside score().
    bool load (const Weights& w, juce::String& error);
    bool loadFromJson (const juce::String& json, juce::String& error);
    void unload();

    bool isLoaded() const noexcept;
    const juce::String& modelName() const noexcept;

    // The only call that may run on the detector thread: no allocation, no
    // lock, no logging, no exception. nullopt when unloaded, when ANY raw field
    // is not finite (checked before any clamp: std::max (x, NaN) returns x), or
    // when the network output is not finite.
    std::optional<float> score (const RawInput& in) noexcept;

    // The 16 UNNORMALISED, clamped features (spec section 3 table) for `in`, or
    // false when any raw field is not finite. Exposed so tests can pin the
    // feature maths directly; score() normalises these with the weights file's
    // input_offset / input_scale.
    static bool rawFeatures (const RawInput& in, std::array<float, kNumFeatures>& out) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
