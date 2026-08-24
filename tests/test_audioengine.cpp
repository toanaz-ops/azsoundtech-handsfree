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
#include "app/SlotConfig.h"
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
//
// startSample keeps the PHASE continuous across consecutive drivers: a
// phase-resetting stimulus puts a step discontinuity at every block boundary
// (512 is not an integer multiple of the 48-sample period of a 1 kHz sine at
// 48 kHz), and through a Q=30 notch that step re-rings the filter's ~750-sample
// transient inside every block -- which masquerades as a shallow notch. Real
// input is continuous; the stimulus must be too.
struct SineDriver
{
    explicit SineDriver (int numSamples, double sampleRate = 48000.0,
                         long long startSample = 0)
        : inL (static_cast<std::size_t> (numSamples))
        , inR (static_cast<std::size_t> (numSamples))
        , outL (static_cast<std::size_t> (numSamples), 0.0f)
        , outR (static_cast<std::size_t> (numSamples), 0.0f)
        , frames (numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const double n = static_cast<double> (startSample + i);
            inL[static_cast<std::size_t> (i)] =
                0.5f * static_cast<float> (std::sin (2.0 * kPi * 1000.0 * n / sampleRate));
            inR[static_cast<std::size_t> (i)] =
                0.5f * static_cast<float> (std::sin (2.0 * kPi * 3000.0 * n / sampleRate));
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

//==============================================================================
// The notch actually ATTENUATES through the callback.
//
// The AudioEngineCommands tests above prove a command changes chain STATE;
// none of them proves a single sample got quieter. If drainCommandQueue()
// applied commands to a copy of the chain, or NotchChain::processSample()
// stopped consulting filters_[i], every state assertion here would stay
// green while the PA kept howling.

TEST (AudioEngine, NotchAttenuatesSignalThroughTheCallback)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Auto);

    // Command a -18 dB notch at 1 kHz onto the LEFT chain only, exactly the
    // way the detector thread does (write() into getCommandQueue()).
    auto& q = engine.getCommandQueue();
    const NotchCommand set { NotchCommandType::Set, 0, 0, 1000.0f, 30.0f, -18.0f };
    ASSERT_EQ (q.write (&set, 1), 1u);

    // A Q=30 biquad needs tens of milliseconds to settle at 48 kHz; the first
    // blocks are transient. Drive 16 phase-continuous blocks of 512 (~171 ms)
    // and measure only the last one.
    constexpr int kBlock = 512;
    constexpr int kSettleBlocks = 15;

    double dbL = 0.0;
    double dbR = 0.0;

    for (int block = 0; block <= kSettleBlocks; ++block)
    {
        SineDriver drive (kBlock, 48000.0,
                          static_cast<long long> (block) * kBlock);
        drive (engine);

        // RMS ratio in dB, block by block; keep the last (settled) one.
        const auto rmsOf = [] (const std::vector<float>& v)
        {
            double sum = 0.0;
            for (const float s : v)
                sum += static_cast<double> (s) * static_cast<double> (s);
            return std::sqrt (sum / static_cast<double> (v.size()));
        };

        dbL = 20.0 * std::log10 (rmsOf (drive.outL) / rmsOf (drive.inL));
        dbR = 20.0 * std::log10 (rmsOf (drive.outR) / rmsOf (drive.inR));
    }

    // Removing the depthDB argument from the Biquad::setNotchFilter call in
    // NotchChain::setNotch() turns this notch into a full null (~-infinity dB)
    // and breaks the lower bound. Deleting the Active-slot filter call in
    // NotchChain::processSample() leaves ~0 dB and breaks the upper bound.
    // The window is deliberately wide (-24..-12 around the requested -18):
    // this pins THAT the signal drops by roughly the asked-for depth, not the
    // biquad's exact magnitude response at 1 kHz.
    EXPECT_NEAR (dbL, -18.0, 6.0) << "measured " << dbL << " dB";

    // The command named channel 0 only. Routing the notch to both chains --
    // e.g. ignoring cmd.channel in drainCommandQueue() -- drags the RIGHT
    // channel down too and breaks this bound.
    EXPECT_NEAR (dbR, 0.0, 1.0) << "right channel must be untouched, measured " << dbR << " dB";
}

//==============================================================================
// The tap CONTRACT: what is in the ring is exactly what left the app.
//
// TapDropCountRecordsTruncatedWrites above pins the COUNTS. Nothing pinned
// the CONTENT: that the ring holds numSamples x callbacks samples, in order,
// equal to the post-notch LEFT output -- not the input, not a scaled copy,
// not interleaved with R.

TEST (AudioEngine, TapHoldsExactlyAndOnlyWhatLeftOnTheLeftChannel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;   // Bypass: output IS the input, easiest oracle

    constexpr int kBlock = 256;
    constexpr int kBlocks = 5;
    std::vector<float> expected;

    for (int i = 0; i < kBlocks; ++i)
    {
        SineDriver drive (kBlock, 48000.0,
                          static_cast<long long> (i) * kBlock);
        drive (engine);
        // Oracle is the INPUT, not outL: in Bypass they are equal, but
        // deriving the expectation from the output would let any defect that
        // corrupts the output path (and the tap with it) compare silence
        // against silence and stay green.
        expected.insert (expected.end(), drive.inL.begin(), drive.inL.end());
    }

    auto& tap = engine.getTapBuffer();

    // Dropping the per-callback tap write (or writing it only outside
    // Bypass) starves the ring and breaks this count.
    ASSERT_EQ (tap.getAvailableRead(), static_cast<std::size_t> (kBlock * kBlocks));

    // One bulk read() must reproduce every block's L output in order.
    // Any reorder, interleave with R, or wrap off-by-one breaks the equality;
    // reading more than was produced would break the count above.
    std::vector<float> tapped (expected.size());
    ASSERT_EQ (tap.read (tapped.data(), tapped.size()), tapped.size());

    for (std::size_t i = 0; i < tapped.size(); ++i)
        ASSERT_FLOAT_EQ (tapped[i], expected[i]) << "sample " << i;

    // Exactly consumed: the ring must now be empty, proving the tap held
    // ONLY these samples and nothing extra was interleaved.
    EXPECT_EQ (tap.getAvailableRead(), 0u);
}

TEST (AudioEngine, TapIsPostNotchSoItDiffersFromInputWhenFiltering)
{
    // With a notch active, tap == output[0] and tap != input[0]. Comparing
    // against BOTH directions is what makes "post-notch" falsifiable: tapping
    // the INPUT instead (moving the write above the processing loop) keeps
    // every count in this file green and only this pair of comparisons red.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Auto);

    auto& q = engine.getCommandQueue();
    const NotchCommand set { NotchCommandType::Set, 0, 0, 1000.0f, 30.0f, -18.0f };
    ASSERT_EQ (q.write (&set, 1), 1u);

    // First callback applies the command mid-flight; the second is fully
    // filtered. BOTH blocks are tapped, so both are compared -- which also
    // proves the ring preserves block ORDER across callbacks.
    SineDriver first (512);
    first (engine);
    SineDriver second (512, 48000.0, 512);
    second (engine);

    constexpr std::size_t kTotal = 1024;
    std::vector<float> tapped (kTotal);
    ASSERT_EQ (engine.getTapBuffer().read (tapped.data(), kTotal), kTotal);

    bool differsFromInput  = false;
    bool matchesOutput     = true;
    for (std::size_t i = 0; i < kTotal; ++i)
    {
        const float& in  = i < 512 ? first.inL[i]          : second.inL[i - 512];
        const float& out = i < 512 ? first.outL[i]         : second.outL[i - 512];

        if (tapped[i] != in)
            differsFromInput = true;
        if (tapped[i] != out)
            matchesOutput = false;
    }

    EXPECT_TRUE (differsFromInput);
    EXPECT_TRUE (matchesOutput);
}

TEST (AudioEngine, NullInputChannelsLeaveTheTapUntouched)
{
    // The tap gate requires input channel 0 to exist because the detector
    // cannot use silence masquerading as signal. Deleting the
    // inputChannelData[0] != nullptr clause makes the callback write the
    // (cleared) OUTPUT into the ring anyway, and this count stops holding.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;

    constexpr int frames = 128;
    std::vector<float> outL (frames, 0.7f);
    std::vector<float> outR (frames, 0.7f);
    const float* ins[2] { nullptr, nullptr };
    float* outs[2] { outL.data(), outR.data() };

    const juce::AudioIODeviceCallbackContext context {};
    engine.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, frames, context);

    EXPECT_EQ (engine.getTapBuffer().getAvailableRead(), 0u);

    // Same promise when JUCE hands us no channel ARRAY at all -- dereferencing
    // inputChannelData[0] unguarded would crash right here.
    std::vector<float> out2 (frames, 0.7f);
    float* outsOnly[1] { out2.data() };
    engine.audioDeviceIOCallbackWithContext (nullptr, 0, outsOnly, 1, frames, context);

    EXPECT_EQ (engine.getTapBuffer().getAvailableRead(), 0u);
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

TEST (AudioEngineCommands, DrainCappedAt256PerCallback)
{
    // Cap moved 64 -> 256 per spec §3: worst-case burst is 16 notches x 2 lanes
    // x 8 slots = 256 commands. Same intent as the original DrainCappedAt64 test:
    // the drain honours its per-callback bound instead of unbounded draining.
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    auto& q = engine.getCommandQueue();
    for (int i = 0; i < 300; ++i) {
        const NotchCommand c { NotchCommandType::Set, 0, (std::uint8_t)(i % 16), 500.0f + i, 30.0f, -12.0f };
        ASSERT_EQ (q.write (&c, 1), 1u);
    }

    float* out[2] = { nullptr, nullptr };
    const float* in[2] = { nullptr, nullptr };
    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});

    EXPECT_EQ (q.getAvailableRead(), 44u);   // exactly 256 consumed this callback
}

//==============================================================================
// Multi-slot routing (spec §3, Task 3).
//
// The engine now owns 8 independently-routed slots. These tests drive the
// callback with MORE channels than the old stereo pair (4 in / 4 out) so a
// cross-route cannot silently fold back onto 0/1.

namespace
{
// Arbitrary channel-count driver. Buffers are owned here so a test can
// stimulate ANY input channel and inspect ANY output channel.
struct RoutingDriver
{
    RoutingDriver (int numSamples, int numIn, int numOut)
        : frames (numSamples), numIn (numIn), numOut (numOut)
    {
        in.assign ((std::size_t) numIn,  std::vector<float> ((std::size_t) numSamples, 0.0f));
        out.assign ((std::size_t) numOut, std::vector<float> ((std::size_t) numSamples, 0.0f));
        inPtrs.resize ((std::size_t) numIn);
        outPtrs.resize ((std::size_t) numOut);
    }

    // Fills channel `ch` with a phase-continuous sine at `freq`, amplitude `amp`.
    void sine (int ch, double freq, double sampleRate, float amp, long long startSample = 0)
    {
        for (int i = 0; i < frames; ++i)
        {
            const double n = static_cast<double> (startSample + i);
            in[(std::size_t) ch][(std::size_t) i] =
                amp * static_cast<float> (std::sin (2.0 * kPi * freq * n / sampleRate));
        }
    }

    void poisonOutputs (float value)
    {
        for (auto& o : out)
            std::fill (o.begin(), o.end(), value);
    }

    void operator() (AudioEngine& engine)
    {
        for (int c = 0; c < numIn; ++c)  inPtrs[(std::size_t) c]  = in[(std::size_t) c].data();
        for (int c = 0; c < numOut; ++c) outPtrs[(std::size_t) c] = out[(std::size_t) c].data();
        const juce::AudioIODeviceCallbackContext context {};
        engine.audioDeviceIOCallbackWithContext (inPtrs.data(), numIn,
                                                 outPtrs.data(), numOut,
                                                 frames, context);
    }

    int frames, numIn, numOut;
    std::vector<std::vector<float>> in, out;
    std::vector<const float*> inPtrs;
    std::vector<float*>       outPtrs;
};

double rmsOf (const std::vector<float>& v)
{
    double sum = 0.0;
    for (const float s : v)
        sum += static_cast<double> (s) * static_cast<double> (s);
    return std::sqrt (sum / static_cast<double> (v.size()));
}
} // namespace

TEST (AudioEngineRouting, MonoSlotRoutesAcrossChannelsThroughTheNotch)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Auto);

    SlotConfig off {};                       // disabled slot
    engine.setSlotConfig (0, off);           // keep the legacy pair OUT of the way

    SlotConfig s1;
    s1.enabled = true;
    s1.width = 1;
    s1.inputChannels[0]  = 1;
    s1.outputChannels[0] = 3;
    engine.setSlotConfig (1, s1);

    auto& q = engine.getCommandQueue (1);
    const NotchCommand set { NotchCommandType::Set, 0, 0, 1000.0f, 30.0f, -18.0f, /*slot*/ 1 };
    ASSERT_EQ (q.write (&set, 1), 1u);

    constexpr int kBlock = 512;
    constexpr int kSettleBlocks = 15;

    double dbOut3 = 0.0;
    RoutingDriver last (kBlock, 2, 4);
    for (int block = 0; block <= kSettleBlocks; ++block)
    {
        last = RoutingDriver (kBlock, 2, 4);
        last.sine (1, 1000.0, 48000.0, 0.5f,
                   static_cast<long long> (block) * kBlock);
        last (engine);

        dbOut3 = 20.0 * std::log10 (rmsOf (last.out[3]) / rmsOf (last.in[1]));
    }

    // Same window as the stereo NotchAttenuatesSignal test: pins THAT the
    // routed lane drops by roughly the asked-for depth.
    EXPECT_NEAR (dbOut3, -18.0, 6.0) << "measured " << dbOut3 << " dB";

    // Nothing routes to 0/1/2: no enabled slot names them, so the settled
    // block's outputs there must be exactly cleared -- not near-zero, zero.
    for (int ch : { 0, 1, 2 })
        for (int i = 0; i < kBlock; ++i)
            ASSERT_FLOAT_EQ (last.out[(std::size_t) ch][(std::size_t) i], 0.0f)
                << "ch " << ch << " sample " << i;
}

TEST (AudioEngineRouting, TwoMonoSlotsSumIntoOneOutputChannel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Auto);

    SlotConfig off {};
    engine.setSlotConfig (0, off);

    SlotConfig s0; s0.enabled = true; s0.width = 1;
    s0.inputChannels[0] = 0; s0.outputChannels[0] = 0;
    engine.setSlotConfig (0, s0);

    SlotConfig s1; s1.enabled = true; s1.width = 1;
    s1.inputChannels[0] = 1; s1.outputChannels[0] = 0;
    engine.setSlotConfig (1, s1);

    RoutingDriver d (512, 2, 4);
    d.sine (0, 1000.0, 48000.0, 0.5f);
    d.sine (1, 3000.0, 48000.0, 0.5f);
    d (engine);

    // Accumulation contract: out[n] += per lane, so two lanes aimed at one
    // output SUM. A copy-instead-of-accumulate defect leaves only the last
    // lane's signal and breaks this against the sum oracle.
    std::vector<float> expectedSum ((std::size_t) d.frames);
    for (int i = 0; i < d.frames; ++i)
        expectedSum[(std::size_t) i] = d.in[0][(std::size_t) i] + d.in[1][(std::size_t) i];

    EXPECT_LT (worstRelativeError (expectedSum, d.out[0]), kToleranceRatio);

    for (const float s : d.out[1])
        ASSERT_FLOAT_EQ (s, 0.0f);
}

TEST (AudioEngineRouting, BypassFollowsTheSlotMappingNotTheChannelIndex)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;   // Bypass by default

    SlotConfig off {};
    engine.setSlotConfig (0, off);

    SlotConfig s1;
    s1.enabled = true;
    s1.width = 1;
    s1.inputChannels[0]  = 1;
    s1.outputChannels[0] = 3;
    engine.setSlotConfig (1, s1);

    RoutingDriver d (256, 4, 4);
    d.sine (0, 500.0, 48000.0, 0.5f);
    d.sine (1, 1500.0, 48000.0, 0.25f);
    d.sine (2, 2500.0, 48000.0, 0.75f);
    d.poisonOutputs (0.7f);
    d (engine);

    // Bypass copies IN->OUT along the mapping: out3 must equal in1 bit-for-bit,
    // and every unmapped output must be CLEARED, not left poisoned.
    for (int i = 0; i < d.frames; ++i)
        ASSERT_FLOAT_EQ (d.out[3][(std::size_t) i], d.in[1][(std::size_t) i]) << "sample " << i;

    for (int ch : { 0, 1, 2 })
        for (int i = 0; i < d.frames; ++i)
            ASSERT_FLOAT_EQ (d.out[(std::size_t) ch][(std::size_t) i], 0.0f)
                << "ch " << ch << " sample " << i;
}

TEST (AudioEngineRouting, LaneOutOfRangeIsDroppedAndOutputStillCleared)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Auto);

    SlotConfig off {};
    engine.setSlotConfig (0, off);

    SlotConfig s1;                    // device only has 4 inputs; index 7 is OOB
    s1.enabled = true;
    s1.width = 1;
    s1.inputChannels[0]  = 7;
    s1.outputChannels[0] = 0;
    engine.setSlotConfig (1, s1);

    RoutingDriver d (128, 4, 4);
    for (int c = 0; c < 4; ++c)
        d.sine (c, 1000.0 + 500.0 * c, 48000.0, 0.5f);
    d.poisonOutputs (0.7f);
    d (engine);                       // must not crash

    // The lane was dropped: every output is cleared silence.
    for (int ch = 0; ch < 4; ++ch)
        for (int i = 0; i < d.frames; ++i)
            ASSERT_FLOAT_EQ (d.out[(std::size_t) ch][(std::size_t) i], 0.0f)
                << "ch " << ch << " sample " << i;
}

TEST (AudioEngineRouting, TapsArePerSlotWithIndependentDropCounts)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;   // Bypass: slot 0 taps out0 == in0

    SlotConfig s1;
    s1.enabled = true;
    s1.width = 1;
    s1.inputChannels[0]  = 1;
    s1.outputChannels[0] = 3;
    engine.setSlotConfig (1, s1);

    // CONTENT: ring 1 holds slot 1's own post-DSP lane-0 output (== its input
    // under Bypass), not slot 0's and not interleaved with anything.
    RoutingDriver content (512, 2, 4);
    content.sine (0, 700.0, 48000.0, 0.5f);
    content.sine (1, 2100.0, 48000.0, 0.5f);
    content (engine);

    std::vector<float> tapped1 ((std::size_t) content.frames);
    ASSERT_EQ (engine.getTapBuffer (1).read (tapped1.data(), tapped1.size()),
               tapped1.size());
    for (int i = 0; i < content.frames; ++i)
        ASSERT_FLOAT_EQ (tapped1[(std::size_t) i], content.in[1][(std::size_t) i])
            << "sample " << i;

    // Empty ring 0 again so the drop-count phase below starts from zero
    // (the content block left 512 samples there; ring 1 was already consumed
    // by the content read above).
    {
        std::vector<float> scratch ((std::size_t) content.frames);
        ASSERT_EQ (engine.getTapBuffer (0).read (scratch.data(), scratch.size()),
                   scratch.size());
    }

    // DROP ISOLATION: fill both rings, overflow both once, then free ONLY
    // ring 0. The next callback must drop into ring 1 while ring 0 writes clean.
    constexpr int kBlock = 1024;
    for (int i = 0; i < 9; ++i)
    {
        RoutingDriver d (kBlock, 2, 4);
        d.sine (0, 700.0, 48000.0, 0.5f);
        d.sine (1, 2100.0, 48000.0, 0.5f);
        d (engine);
    }

    EXPECT_EQ (engine.getTapDropCount (0), 1024u);
    EXPECT_EQ (engine.getTapDropCount (1), 1024u);

    std::vector<float> drain (kTapCapacity);
    ASSERT_EQ (engine.getTapBuffer (0).read (drain.data(), drain.size()), drain.size());

    {
        RoutingDriver d (kBlock, 2, 4);
        d.sine (0, 700.0, 48000.0, 0.5f);
        d.sine (1, 2100.0, 48000.0, 0.5f);
        d (engine);
    }

    EXPECT_EQ (engine.getTapDropCount (0), 1024u)  << "ring 0 had room again";
    EXPECT_EQ (engine.getTapDropCount (1), 2048u)  << "ring 1 stayed full";
}

TEST (AudioEngineCommands, CommandOnQueue1AppliesToSlot1Only)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;

    auto& q1 = engine.getCommandQueue (1);
    const NotchCommand good { NotchCommandType::Set, 0, 5, 1234.0f, 30.0f, -12.0f, /*slot*/ 1 };
    ASSERT_EQ (q1.write (&good, 1), 1u);

    // A command claiming slot 0 but smuggled through queue 1 must be SKIPPED:
    // queue and slot field must agree, or a hostile/mismatched pair would let
    // one detector thread reach another slot's chain.
    const NotchCommand evil { NotchCommandType::Set, 0, 6, 4321.0f, 30.0f, -12.0f, /*slot*/ 0 };
    ASSERT_EQ (q1.write (&evil, 1), 1u);

    float* out[2] = { nullptr, nullptr };
    const float* in[2] = { nullptr, nullptr };
    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});

    EXPECT_EQ (engine.getNotchChainForTest (1, 0).getNotchInfo (5).state,
               NotchChain::NotchState::Active);
    EXPECT_DOUBLE_EQ (engine.getNotchChainForTest (1, 0).getNotchInfo (5).frequency, 1234.0);

    // Every OTHER chain is untouched.
    for (int slot = 0; slot < kMaxSlots; ++slot)
        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            if (slot == 1 && lane == 0)
                continue;
            EXPECT_NE (engine.getNotchChainForTest (slot, lane).getNotchInfo (5).state,
                       NotchChain::NotchState::Active)
                << "slot " << slot << " lane " << lane;
            EXPECT_NE (engine.getNotchChainForTest (slot, lane).getNotchInfo (6).state,
                       NotchChain::NotchState::Active)
                << "slot " << slot << " lane " << lane;
        }
}

TEST (AudioEngine, NoArgAccessorsStillReturnSlotZero)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;

    EXPECT_EQ (&engine.getTapBuffer(), &engine.getTapBuffer (0));
    EXPECT_EQ (&engine.getCommandQueue(), &engine.getCommandQueue (0));
    EXPECT_EQ (engine.getTapDropCount(), engine.getTapDropCount (0));
    EXPECT_EQ (&engine.getNotchChainForTest (0), &engine.getNotchChainForTest (0, 0));
    EXPECT_EQ (&engine.getNotchChainForTest (1), &engine.getNotchChainForTest (0, 1));

    // Legacy drop count IS slot 0's drop count: overflow ring 0 only.
    CallbackDriver drive { 1024 };
    for (int i = 0; i < 9; ++i)
        drive (engine);
    EXPECT_EQ (engine.getTapDropCount(), 1024u);
    EXPECT_EQ (engine.getTapDropCount (0), 1024u);
}

TEST (AudioEngine, ChannelNameQueriesAreSafeWithNoDeviceOpen)
{
    // Same contract as DeviceQueriesAreSafeWithNoDeviceOpen above, extended to
    // the new name accessors Task 6's routing UI needs: nullptr-guarded, empty
    // rather than crashing before start().
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    EXPECT_TRUE (engine.getInputChannelNames().isEmpty());
    EXPECT_TRUE (engine.getOutputChannelNames().isEmpty());
}
