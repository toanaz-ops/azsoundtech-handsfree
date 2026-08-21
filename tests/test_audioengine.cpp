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
