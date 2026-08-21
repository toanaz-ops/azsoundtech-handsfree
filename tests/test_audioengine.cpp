// AudioEngine wiring tests (fix pass 1, Part B).
//
// Why these are possible without an audio device
// ==============================================
// Constructing an AudioEngine constructs a juce::AudioDeviceManager, but the
// AudioDeviceManager constructor opens NO device -- AudioEngine::start() does,
// and nothing here calls start(). juce::ScopedJuceInitialiser_GUI brings up the
// MessageManager that JUCE's ChangeBroadcaster and leak-detector machinery
// expect, and tears it down at the end of each test.
//
// audioDeviceIOCallbackWithContext() and audioDeviceAboutToStart() are public
// (they are overrides of juce::AudioIODeviceCallback), so the tests can drive
// the callback directly with fabricated buffers. That is what lets the tap
// drop counter and the restart drain be tested for real rather than by
// inspection.

#include <gtest/gtest.h>

#include "app/AudioEngine.h"
#include "dsp/LockFreeRingBuffer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
// Matches AudioEngine::kTapCapacity, which is private.
constexpr std::size_t kTapCapacity = 8192;

// Drives one callback of `numSamples` frames of stereo input at a constant
// level. Bypass (the default mode) copies input to output, and the tap is
// written from the output, so every callback pushes `numSamples` into the ring.
struct CallbackDriver
{
    explicit CallbackDriver (int numSamples)
        : inL (static_cast<std::size_t> (numSamples), 0.25f)
        , inR (static_cast<std::size_t> (numSamples), 0.25f)
        , outL (static_cast<std::size_t> (numSamples), 0.0f)
        , outR (static_cast<std::size_t> (numSamples), 0.0f)
        , frames (numSamples)
    {
        ins[0]  = inL.data();
        ins[1]  = inR.data();
        outs[0] = outL.data();
        outs[1] = outR.data();
    }

    void operator() (AudioEngine& engine)
    {
        const juce::AudioIODeviceCallbackContext context {};
        engine.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, frames, context);
    }

    std::vector<float> inL, inR, outL, outR;
    const float* ins[2] {};
    float*       outs[2] {};
    int          frames;
};
} // namespace

// B1. getTapBuffer() was DECLARED in AudioEngine.h and never DEFINED in
// AudioEngine.cpp. It linked only because nothing called it; the first line of
// Task 13 to call it would have failed with LNK2019. This test is that first
// caller, so the defect can never come back silently.
TEST (AudioEngine, TapBufferAccessorIsDefinedAndUsable)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;

    LockFreeRingBuffer<float>& tap = engine.getTapBuffer();

    EXPECT_EQ (tap.getCapacity(), kTapCapacity);
    EXPECT_EQ (tap.getAvailableRead(), 0u);

    // Same object on every call -- the accessor must hand out the engine's own
    // ring, not a copy (LockFreeRingBuffer is non-copyable, but a value return
    // of a different member would still compile as a reference to something).
    EXPECT_EQ (&tap, &engine.getTapBuffer());
}

// B3. A truncated tap write is correct BEHAVIOUR on the audio thread, but
// discarding the fact is not: a short write splices sample N onto sample N+k,
// and through a 1024-point Hann window that step is broadband energy in every
// bin -- exactly what the peakiness scorer reacts to. The count is the only
// way anything downstream can tell a real howl from a drop.
//
// Arithmetic: the ring holds 8192 floats. Eight callbacks of 1024 fill it
// exactly, so nothing is dropped. The ninth finds zero free slots and drops
// all 1024.
TEST (AudioEngine, TapDropCountRecordsTruncatedWrites)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;

    constexpr int kBlock = 1024;
    CallbackDriver drive { kBlock };

    EXPECT_EQ (engine.getTapDropCount(), 0u);

    for (int i = 0; i < 8; ++i)
        drive (engine);

    EXPECT_EQ (engine.getTapBuffer().getAvailableRead(), kTapCapacity);
    EXPECT_EQ (engine.getTapDropCount(), 0u) << "a write that fits must not count as a drop";

    drive (engine);
    EXPECT_EQ (engine.getTapDropCount(), 1024u);

    drive (engine);
    EXPECT_EQ (engine.getTapDropCount(), 2048u);
}

// The partial case, which is the one that actually splices: 100 samples are
// drained, so the next 1024-sample block writes 100 and drops 924.
TEST (AudioEngine, TapDropCountRecordsPartialWrites)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;

    constexpr int kBlock = 1024;
    CallbackDriver drive { kBlock };

    for (int i = 0; i < 8; ++i)
        drive (engine);
    ASSERT_EQ (engine.getTapDropCount(), 0u);

    std::vector<float> scratch (100);
    ASSERT_EQ (engine.getTapBuffer().read (scratch.data(), scratch.size()), 100u);

    drive (engine);
    EXPECT_EQ (engine.getTapDropCount(), 1024u - 100u);   // 924
}

// B2. Stale audio must not be spliced across a device restart. Stopping at
// 48 kHz can leave thousands of samples in the ring; restarting at 96 kHz would
// then produce a Spectrum labelled 96000 Hz over a window whose first half is
// 48 kHz audio, and every bin-to-Hz conversion in Tasks 11-14 would be wrong by
// up to an octave. audioDeviceAboutToStart() drains the ring.
//
// A null device is passed deliberately: the drain must not be conditional on
// the device pointer, and a unit test has no juce::AudioIODevice to hand.
TEST (AudioEngine, DeviceRestartDrainsTheTap)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;

    CallbackDriver drive { 512 };
    for (int i = 0; i < 4; ++i)
        drive (engine);
    ASSERT_EQ (engine.getTapBuffer().getAvailableRead(), 2048u);

    engine.audioDeviceAboutToStart (nullptr);

    EXPECT_EQ (engine.getTapBuffer().getAvailableRead(), 0u);
    EXPECT_EQ (engine.getTapBuffer().getAvailableWrite(), kTapCapacity);
}

//==============================================================================
// The passthrough contract -- plan Task 9's specified test, never written.
//
// Task 9 called for "verify output matches input within 0.01 dB" and closed
// without it. The engine's headline promise is that when it is NOT filtering,
// what comes out is what went in; nothing in the repo checked that until now.
//
// These are characterisation tests, not red-green TDD: they pin behaviour that
// already works. To confirm they are capable of failing rather than merely
// green, a mutation was injected during development -- scaling the Bypass copy
// by 0.999 (a 0.0087 dB change, deliberately just inside the tolerance) and
// then by 0.99 (0.087 dB). The first passed and the second failed, which is
// what a 0.01 dB bound should do.

namespace
{
constexpr double kPi = 3.14159265358979323846;

// 0.01 dB expressed as an amplitude ratio: 10^(0.01/20) - 1 == 0.00115...
// Anything the engine does to the signal beyond this is audible drift, not
// arithmetic noise.
constexpr double kToleranceRatio = 0.00116;

// Drives one callback with a sine on L and a different sine on R, so a
// channel swap or a mono fold cannot pass. Retains the input for comparison,
// which the constant-level CallbackDriver above cannot do.
struct SineDriver
{
    explicit SineDriver (int numSamples, double sampleRate = 48000.0)
        : inL (static_cast<std::size_t> (numSamples))
        , inR (static_cast<std::size_t> (numSamples))
        , outL (static_cast<std::size_t> (numSamples), 0.0f)
        , outR (static_cast<std::size_t> (numSamples), 0.0f)
        , frames (numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            inL[static_cast<std::size_t> (i)] =
                0.5f * static_cast<float> (std::sin (2.0 * kPi * 1000.0 * i / sampleRate));
            inR[static_cast<std::size_t> (i)] =
                0.5f * static_cast<float> (std::sin (2.0 * kPi * 3000.0 * i / sampleRate));
        }

        ins[0]  = inL.data();
        ins[1]  = inR.data();
        outs[0] = outL.data();
        outs[1] = outR.data();
    }

    void operator() (AudioEngine& engine)
    {
        const juce::AudioIODeviceCallbackContext context {};
        engine.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, frames, context);
    }

    std::vector<float> inL, inR, outL, outR;
    const float* ins[2] {};
    float*       outs[2] {};
    int          frames;
};

// Largest |out - in| across a channel, as a fraction of the input's peak.
double worstRelativeError (const std::vector<float>& in, const std::vector<float>& out)
{
    double peak  = 0.0;
    double worst = 0.0;

    for (std::size_t i = 0; i < in.size(); ++i)
    {
        peak  = std::max (peak,  std::abs (static_cast<double> (in[i])));
        worst = std::max (worst, std::abs (static_cast<double> (out[i]) -
                                           static_cast<double> (in[i])));
    }

    return peak > 0.0 ? worst / peak : worst;
}
} // namespace

TEST (AudioEngine, BypassPassesInputThroughWithin001dB)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    ASSERT_EQ (engine.getMode(), AudioEngine::Mode::Bypass);

    SineDriver drive (512);
    drive (engine);

    EXPECT_LT (worstRelativeError (drive.inL, drive.outL), kToleranceRatio);
    EXPECT_LT (worstRelativeError (drive.inR, drive.outR), kToleranceRatio);
}

TEST (AudioEngine, AutoModeWithNoNotchesIsAlsoTransparent)
{
    // The more valuable half. Bypass short-circuits the DSP entirely, so it
    // proves only that the copy works. Auto mode runs every sample through
    // NotchChain::processSample -- float to double, sixteen Idle slots, back to
    // float. If an Idle slot were ever anything other than a straight
    // passthrough, THIS is the test that catches it, and Bypass would not.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Auto);

    SineDriver drive (512);
    drive (engine);

    EXPECT_LT (worstRelativeError (drive.inL, drive.outL), kToleranceRatio);
    EXPECT_LT (worstRelativeError (drive.inR, drive.outR), kToleranceRatio);
}

TEST (AudioEngine, PassthroughHoldsAcrossManyConsecutiveCallbacks)
{
    // One callback cannot show state leaking between callbacks. A biquad that
    // retained state across an Idle slot, or an off-by-one in the block
    // handling, would show up as drift only after several blocks.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Auto);

    for (int block = 0; block < 32; ++block)
    {
        SineDriver drive (256);
        drive (engine);

        ASSERT_LT (worstRelativeError (drive.inL, drive.outL), kToleranceRatio)
            << "block " << block;
        ASSERT_LT (worstRelativeError (drive.inR, drive.outR), kToleranceRatio)
            << "block " << block;
    }
}

TEST (AudioEngine, ANullInputChannelProducesSilenceNotGarbage)
{
    // JUCE does not pre-clear the output buffers, so a missing input must be
    // met with an explicit clear. Without it the callback hands the driver
    // whatever was in that memory -- at full scale, into a PA.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;

    constexpr int frames = 128;
    std::vector<float> outL (frames, 0.7f);   // pre-poisoned, must be cleared
    std::vector<float> outR (frames, 0.7f);

    const float* ins[2] { nullptr, nullptr };
    float* outs[2] { outL.data(), outR.data() };

    const juce::AudioIODeviceCallbackContext context {};
    engine.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, frames, context);

    for (int i = 0; i < frames; ++i)
    {
        ASSERT_FLOAT_EQ (outL[static_cast<std::size_t> (i)], 0.0f) << "sample " << i;
        ASSERT_FLOAT_EQ (outR[static_cast<std::size_t> (i)], 0.0f) << "sample " << i;
    }
}
