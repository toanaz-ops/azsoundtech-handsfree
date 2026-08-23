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

//==============================================================================
// The public surface the GUI lane needs (Lane C audit section 3).
//
// The parallel execution plan section 2 argues this should land in ONE commit
// before any GUI task starts, so GUI / licensing / installer lanes do not each
// widen the same header and collide. These tests pin the contract.
//
// Every test here runs with NO DEVICE OPEN, and that is the point rather than a
// limitation. The GUI calls these while populating combo boxes at startup --
// before start() has ever been called. "Returns something safe and does not
// crash with no device" IS the contract for most of this surface, and it is the
// half a device-based test would never cover.

TEST (AudioEngine, DeviceTypeEnumerationWorksBeforeAnyDeviceIsOpened)
{
    // Populating the device-type combo happens at construction time, long
    // before start(). juce::AudioDeviceManager registers its types in its own
    // constructor, so this must already work.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    const juce::StringArray types = engine.getAvailableDeviceTypeNames();

    // Windows always has at least WASAPI and DirectSound registered. ASIO
    // appears only when the SDK was present at build time, so it is NOT
    // asserted -- .gitignore excludes external/asiosdk for licensing reasons
    // and CI builds without it.
    EXPECT_FALSE (types.isEmpty());
}

TEST (AudioEngine, DeviceQueriesAreSafeWithNoDeviceOpen)
{
    // Not a crash test for its own sake. Every one of these is on the path the
    // GUI walks before a device exists, and juce::AudioDeviceManager returns
    // nullptr from getCurrentAudioDevice() in that state -- which is exactly
    // the dereference that would take the app down on launch.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    ASSERT_FALSE (engine.isRunning());

    EXPECT_TRUE (engine.getAvailableSampleRates().isEmpty());
    EXPECT_TRUE (engine.getAvailableBufferSizes().isEmpty());
    EXPECT_TRUE (engine.getAvailableDeviceNames().isEmpty());
    EXPECT_TRUE (engine.getCurrentDeviceName().isEmpty());

    // Setters must REFUSE rather than pretend. A GUI that cannot tell a
    // successful rate change from a silent no-op will show the user a value
    // the hardware is not running -- the specific failure the audit called out.
    EXPECT_FALSE (engine.setSampleRate (44100.0));
    EXPECT_FALSE (engine.setBufferSize (128));
}

TEST (AudioEngine, NumericSampleRateAgreesWithTheDisplayString)
{
    // getCurrentSampleRate() returns "48000 Hz" -- a display string. Task 17's
    // ComboBox needs the number and Task 25's preset JSON stores it
    // numerically, so both forms must exist AND must never disagree.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;

    const double  hz   = engine.getCurrentSampleRateHz();
    const juce::String text = engine.getCurrentSampleRate();

    EXPECT_DOUBLE_EQ (hz, 48000.0);
    EXPECT_TRUE (text.startsWith (juce::String (hz)))
        << "display string '" << text << "' does not begin with " << hz;
}

TEST (AudioEngine, ChannelCountsReportWhatTheCallbackActuallyDelivered)
{
    // Spec 6.1's status line reads "2 in / 2 out". Sourcing that from the
    // callback rather than from a device handle is what makes it true: a device
    // can advertise channels the callback is not actually handed.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;

    // Nothing has flowed yet -- claiming 2 in / 2 out here would be a lie.
    EXPECT_EQ (engine.getNumInputChannels(), 0);
    EXPECT_EQ (engine.getNumOutputChannels(), 0);

    SineDriver drive (128);
    drive (engine);

    EXPECT_EQ (engine.getNumInputChannels(), 2);
    EXPECT_EQ (engine.getNumOutputChannels(), 2);
}

TEST (AudioEngine, TheLastDeviceErrorIsRetrievableInsteadOfOnlyLogged)
{
    // audioDeviceError() used to log the message and drop it. The status
    // indicator went dark with the reason available nowhere in the UI -- a
    // soundman mid-show got a dead app and no cause.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    EXPECT_TRUE (engine.getLastDeviceError().isEmpty());

    engine.audioDeviceError ("ASIO driver stopped responding");

    EXPECT_EQ (engine.getLastDeviceError(), "ASIO driver stopped responding");
    EXPECT_FALSE (engine.isRunning());
}

//==============================================================================
// Detector -> audio command queue (bridge design §2).
//
// Each test constructs the AudioEngine directly; the ScopedJuceInitialiser_GUI
// is required for the same reason as every other test in this file (the
// AudioDeviceManager constructor and JUCE's leak-detector machinery expect the
// MessageManager to be up).

TEST (AudioEngineCommands, CommandCrossesBoundaryInOneCallback)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    auto& q = engine.getCommandQueue();
    const NotchCommand set { NotchCommandType::Set, 0, 2, 1000.0f, 30.0f, -12.0f };
    ASSERT_EQ (q.write (&set, 1), 1u);

    float* out[2] = { nullptr, nullptr };
    const float* in[2] = { nullptr, nullptr };
    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});

    EXPECT_EQ (engine.getNotchChainForTest (0).getNotchInfo (2).state, NotchChain::NotchState::Active);
    EXPECT_DOUBLE_EQ (engine.getNotchChainForTest (0).getNotchInfo (2).frequency, 1000.0);
}

TEST (AudioEngineCommands, ClearCommandDeactivates)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    auto& q = engine.getCommandQueue();
    const NotchCommand set   { NotchCommandType::Set,   1, 0, 800.0f, 30.0f, -12.0f };
    const NotchCommand clear { NotchCommandType::Clear, 1, 0, 0.0f, 0.0f, 0.0f };
    ASSERT_EQ (q.write (&set, 1), 1u);
    ASSERT_EQ (q.write (&clear, 1), 1u);

    float* out[2] = { nullptr, nullptr };
    const float* in[2] = { nullptr, nullptr };
    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});

    EXPECT_NE (engine.getNotchChainForTest (1).getNotchInfo (0).state, NotchChain::NotchState::Active);
}

TEST (AudioEngineCommands, OutOfRangeChannelIsSkippedNotApplied)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    auto& q = engine.getCommandQueue();
    const NotchCommand evil { NotchCommandType::Set, 9, 2, 1000.0f, 30.0f, -12.0f };
    ASSERT_EQ (q.write (&evil, 1), 1u);

    float* out[2] = { nullptr, nullptr };
    const float* in[2] = { nullptr, nullptr };
    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});
    SUCCEED();   // surviving hostile ring content without OOB indexing IS the assertion
}

TEST (AudioEngineCommands, DrainCappedAt64PerCallback)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    auto& q = engine.getCommandQueue();
    for (int i = 0; i < 100; ++i) {
        const NotchCommand c { NotchCommandType::Set, 0, (std::uint8_t)(i % 16), 500.0f + i, 30.0f, -12.0f };
        ASSERT_EQ (q.write (&c, 1), 1u);
    }

    float* out[2] = { nullptr, nullptr };
    const float* in[2] = { nullptr, nullptr };
    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});

    EXPECT_EQ (q.getAvailableRead(), 36u);   // exactly 64 consumed this callback
}
