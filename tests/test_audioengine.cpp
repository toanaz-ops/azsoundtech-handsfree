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
#include "dsp/SoundcheckSignal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
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

// Like CallbackDriver but with a configurable channel count and per-channel
// input, so a test can prove that only ONE output channel changed.
struct MultiDriver
{
    MultiDriver (int channels, int numSamples, float inputLevel = 0.25f)
        : frames (numSamples)
    {
        in.assign  ((std::size_t) channels, std::vector<float> ((std::size_t) numSamples, inputLevel));
        out.assign ((std::size_t) channels, std::vector<float> ((std::size_t) numSamples, 0.0f));
        for (auto& v : in)  inPtr.push_back (v.data());
        for (auto& v : out) outPtr.push_back (v.data());
    }

    void operator() (AudioEngine& engine)
    {
        for (auto& v : out) std::fill (v.begin(), v.end(), 0.0f);
        const juce::AudioIODeviceCallbackContext context {};
        // declaredChannels defaults to every allocated channel. Setting it
        // LOWER allocates real, readable guard buffers the callback is told
        // nothing about (M-2): an index the engine failed to bounds-check
        // lands in one of them and the test can assert on it, instead of the
        // test depending on undefined behaviour to crash at the right moment.
        const int declared = (declaredChannels > 0) ? declaredChannels : (int) outPtr.size();
        engine.audioDeviceIOCallbackWithContext (inPtr.data(), declared,
                                                 outPtr.data(), declared,
                                                 frames, context);
    }

    float peakOn (int channel) const
    {
        float m = 0.0f;
        for (float v : out[(std::size_t) channel]) m = std::max (m, std::abs (v));
        return m;
    }

    std::vector<std::vector<float>> in, out;
    std::vector<const float*> inPtr;
    std::vector<float*>       outPtr;
    int frames;
    int declaredChannels = 0;   // 0 = tell the engine about every channel
};

// Slot `s` enabled, mono, inCh -> outCh. SlotConfig's field names come from
// src/app/SlotConfig.h.
void routeMono (AudioEngine& engine, int s, int inCh, int outCh)
{
    SlotConfig c;
    c.enabled = true;
    c.width   = 1;
    c.inputChannels[0]  = inCh;
    c.outputChannels[0] = outCh;
    engine.setSlotConfig (s, c);
}
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

TEST (AudioEngineCommands, DrainBudgetIsSharedAcrossAllSlots)
{
    // The cap is ONE budget of 256 per callback across ALL eight rings, not
    // per ring: eight independently-capped rings would multiply the worst-
    // case callback time by 8.
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;

    auto& q0 = engine.getCommandQueue (0);
    auto& q1 = engine.getCommandQueue (1);
    for (int i = 0; i < 200; ++i)
    {
        const NotchCommand c0 { NotchCommandType::Set, 0, (std::uint8_t) (i % 16),
                                500.0f + i, 30.0f, -12.0f, 0 };
        ASSERT_EQ (q0.write (&c0, 1), 1u);

        const NotchCommand c1 { NotchCommandType::Set, 0, (std::uint8_t) (i % 16),
                                900.0f + i, 30.0f, -12.0f, 1 };
        ASSERT_EQ (q1.write (&c1, 1), 1u);
    }

    float* out[2] = { nullptr, nullptr };
    const float* in[2] = { nullptr, nullptr };
    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});

    // Slot 0's queue drains first and eats 200 of the budget; slot 1 gets
    // only the remaining 56 -- NOT a fresh 256 of its own. The rest waits for
    // the next callback rather than being dropped.
    EXPECT_EQ (q0.getAvailableRead(), 0u);
    EXPECT_EQ (q1.getAvailableRead(), 144u);

    engine.audioDeviceIOCallbackWithContext (in, 2, out, 2, 64, {});
    EXPECT_EQ (q1.getAvailableRead(), 0u);
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

// Lane S: the detector needs to hear BOTH lanes. Turns red if the callback
// stops writing lane 1's post-DSP output into tapBuffers_[slot][1].
TEST (AudioEngineRouting, StereoSlotTapsBothLanesWithLaneOneCarryingLaneOneOutput)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;   // Bypass by default; slot 0 default stereo {0,1}->{0,1}

    RoutingDriver d (512, 2, 2);
    std::fill (d.in[0].begin(), d.in[0].end(), 0.25f);
    std::fill (d.in[1].begin(), d.in[1].end(), -0.5f);
    d (engine);

    std::vector<float> tapped0 ((std::size_t) d.frames), tapped1 ((std::size_t) d.frames);
    ASSERT_EQ (engine.getTapBuffer (0, 0).read (tapped0.data(), tapped0.size()), tapped0.size());
    ASSERT_EQ (engine.getTapBuffer (0, 1).read (tapped1.data(), tapped1.size()), tapped1.size());
    EXPECT_FLOAT_EQ (tapped0[100],  0.25f);
    EXPECT_FLOAT_EQ (tapped1[100], -0.5f);   // NOT a copy of lane 0
    EXPECT_EQ (engine.getTapDropCount (0, 1), 0u);
}

// Turns red if a mono slot starts writing lane 1's ring (there is no lane 1).
TEST (AudioEngineRouting, MonoSlotLeavesLaneOneTapUntouched)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;   // Bypass by default

    SlotConfig mono;
    mono.enabled = true; mono.width = 1;
    mono.inputChannels[0] = 0; mono.outputChannels[0] = 0;
    engine.setSlotConfig (0, mono);

    RoutingDriver d (512, 2, 2);
    std::fill (d.in[0].begin(), d.in[0].end(), 0.25f);
    std::fill (d.in[1].begin(), d.in[1].end(), -0.5f);
    d (engine);

    EXPECT_EQ (engine.getTapBuffer (0, 0).getAvailableRead(), 512u);
    EXPECT_EQ (engine.getTapBuffer (0, 1).getAvailableRead(), 0u);
    EXPECT_EQ (engine.getTapDropCount (0, 1), 0u);
}

// Turns red if the legacy accessors stop aliasing lane 0.
TEST (AudioEngineRouting, LegacyTapAccessorsAliasLaneZero)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    EXPECT_EQ (&engine.getTapBuffer(),  &engine.getTapBuffer (0, 0));
    EXPECT_EQ (&engine.getTapBuffer (3), &engine.getTapBuffer (3, 0));
    // Out-of-range lane clamps to 0, matching the slot-clamp convention.
    EXPECT_EQ (&engine.getTapBuffer (3, 7), &engine.getTapBuffer (3, 0));
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

//==============================================================================
// Output safety guard: nothing non-finite and nothing beyond full scale may
// ever reach the driver.
//
// This callback feeds a PA system. A NaN or Inf handed to the DAC conversion
// is undefined, and the Direct Form I biquads keep persistent z1_/z2_ state --
// ONE non-finite input poisons a chain until reset(), which otherwise happens
// only on device restart. These tests pin three promises: poisoned input
// produces finite output, the chain SELF-HEALS instead of staying poisoned,
// and every sample handed to the driver is inside [-1, +1].

namespace
{
// Shared body for the NaN / +Inf poison tests: identical stimulus and
// recovery oracle, only the poison value differs.
void expectPoisonedInputIsAbsorbedAndTheChainRecovers (const float poison)
{
    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Auto);

    // An ACTIVE notch, so the input actually flows through a biquad whose
    // persistent state a single bad sample would poison. Idle slots are
    // straight passthrough and would hide the state-poisoning defect.
    auto& q = engine.getCommandQueue();
    const NotchCommand set { NotchCommandType::Set, 0, 0, 1000.0f, 30.0f, -18.0f };
    ASSERT_EQ (q.write (&set, 1), 1u);

    constexpr int kBlock = 512;
    long long n = 0;

    // Settle the Q=30 notch on the clean phase-continuous stimulus first
    // (same settling budget as NotchAttenuatesSignalThroughTheCallback).
    for (int block = 0; block < 16; ++block, n += kBlock)
    {
        SineDriver drive (kBlock, 48000.0, n);
        drive (engine);
    }

    // The poisoned block: one bad sample mid-block on L. EVERY output sample
    // on BOTH channels must still be finite -- this buffer goes to the DAC.
    {
        SineDriver drive (kBlock, 48000.0, n);
        drive.inL[100] = poison;
        drive (engine);
        n += kBlock;

        for (int i = 0; i < kBlock; ++i)
        {
            ASSERT_TRUE (std::isfinite (drive.outL[(std::size_t) i])) << "L sample " << i;
            ASSERT_TRUE (std::isfinite (drive.outR[(std::size_t) i])) << "R sample " << i;
        }
    }

    // Recovery: after clean input the notch must be back at its settled
    // ~-18 dB depth on L (window borrowed from the attenuation test). A
    // biquad left poisoned emits NaN forever, and NaN fails EXPECT_NEAR by
    // definition -- so this asserts recovery, not merely survival.
    double dbL = 0.0;
    for (int block = 0; block < 16; ++block, n += kBlock)
    {
        SineDriver drive (kBlock, 48000.0, n);
        drive (engine);

        dbL = 20.0 * std::log10 (rmsOf (drive.outL) / rmsOf (drive.inL));
    }

    EXPECT_NEAR (dbL, -18.0, 6.0) << "measured " << dbL << " dB";
}
} // namespace

TEST (AudioEngine, NaNInputProducesFiniteOutputAndTheChainRecovers)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    expectPoisonedInputIsAbsorbedAndTheChainRecovers (
        std::numeric_limits<float>::quiet_NaN());
}

TEST (AudioEngine, InfInputProducesFiniteOutputAndTheChainRecovers)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    expectPoisonedInputIsAbsorbedAndTheChainRecovers (
        std::numeric_limits<float>::infinity());
}

TEST (AudioEngine, GuardLeavesCleanAudioBelowFullScaleUntouched)
{
    // Deliberately duplicates the transparency oracle under the guard
    // section's name: if the output guard ever alters in-range audio (an
    // off-by-one in the clamp bound, a wrong branch on the finite check),
    // THIS is the failure that names the guard as the culprit rather than
    // pointing at the passthrough contract.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Auto);

    SineDriver drive (512);   // 0.5 amplitude -- comfortably below full scale
    drive (engine);

    EXPECT_LT (worstRelativeError (drive.inL, drive.outL), kToleranceRatio);
    EXPECT_LT (worstRelativeError (drive.inR, drive.outR), kToleranceRatio);
}

TEST (AudioEngine, OutputIsClampedToFullScaleAndNonFiniteBecomesSilence)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    // Phase 1 -- Bypass with a hot input: the raw copy would hand +-1.5f
    // straight to the driver. The guard must clamp to exactly full scale,
    // and a non-finite sample must become 0, not "clamped NaN" (any
    // comparison with NaN clamps to an arbitrary bound).
    {
        AudioEngine engine;   // Bypass by default

        constexpr int frames = 256;
        std::vector<float> inL (frames, 1.5f);
        std::vector<float> inR (frames, -1.5f);
        std::vector<float> outL (frames, 0.0f), outR (frames, 0.0f);
        inL[10] = std::numeric_limits<float>::quiet_NaN();

        const float* ins[2] { inL.data(), inR.data() };
        float* outs[2] { outL.data(), outR.data() };

        const juce::AudioIODeviceCallbackContext context {};
        engine.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, frames, context);

        for (int i = 0; i < frames; ++i)
        {
            const float expectedL = (i == 10) ? 0.0f : 1.0f;
            ASSERT_FLOAT_EQ (outL[(std::size_t) i], expectedL) << "L sample " << i;
            ASSERT_FLOAT_EQ (outR[(std::size_t) i], -1.0f)    << "R sample " << i;
        }
    }

    // Phase 2 -- multi-slot summing: two mono slots aimed at one output, each
    // carrying 0.8. The raw sum is 1.6; the guard bounds what summing can
    // stack up, so the driver sees exactly 1.0.
    {
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

        RoutingDriver d (128, 2, 2);
        for (auto& channel : d.in)
            std::fill (channel.begin(), channel.end(), 0.8f);
        d (engine);

        for (int i = 0; i < d.frames; ++i)
            ASSERT_FLOAT_EQ (d.out[0][(std::size_t) i], 1.0f) << "sample " << i;
    }
}

// ===================================================================
// Lane M -- active soundcheck (Task 5). The engine's side of the sweep:
// injection before the output clamp, mute by OUTPUT CHANNEL, a raw-mic capture
// ring, a callback-generated ramp-out, tap suspension, and a single snapshot of
// every soundcheck atomic per callback.
// ===================================================================

// RED IF: the mute condition is written as "(slot, lane) matches" instead of
// "outIdx == scOutChannel_". Several slots SUM onto one output channel
// (AudioEngine.cpp clears every channel, then the DSP accumulates), so muting
// one pair leaves the feedback loop through that channel CLOSED -- spec rev 1's
// blocker F2. inv 8.
TEST (AudioEngineSoundcheck, SweptChannelCarriesOnlyTheSweep)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);              // B-4
    engine.setMode (AudioEngine::Mode::Bypass);   // Bypass copies in->out: loudest case

    routeMono (engine, 0, 0, 1);
    routeMono (engine, 1, 2, 1);   // a SECOND slot onto the same output channel

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 4, 256, 0.5f };
    d (engine);

    SoundcheckSignal::Params p;
    p.sampleRate = engine.getCurrentSampleRateHz();
    p.peak       = SoundcheckSignal::kSoundcheckMaxPeak;
    const SoundcheckSignal expected { p };

    for (int n = 0; n < 256; ++n)
        ASSERT_NEAR (d.out[1][(std::size_t) n], expected.sampleAt (n), 1.0e-6f)
            << "sample " << n << " carries something other than the sweep";
}

// RED IF: the injection point is moved BELOW the output clamp.
//
// B-5: the seam must sit PAST SoundcheckSignal, not before it. A seam on the
// PEAK would skip only the setter's clamp -- but the SoundcheckSignal
// constructor clamps the peak and sampleAt clamps the sample, so |v| <= 0.1
// whatever the setter was handed, sawSomething would never be set, and the test
// would assert nothing at all. The gain seam multiplies the ALREADY-CLAMPED
// sample at point 3, which is the only way to put something over full scale in
// front of the output clamp. inv 4, F17.
TEST (AudioEngineSoundcheck, OutputClampStillCoversTheSweepPath)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckGainUnclampedForTest (50.0f);   // 0.1 * 50 = 5x full scale
    // PAST the 30 ms ramp-in, deliberately. Measured 2026-09-16: starting at
    // sample 0 the raised-cosine ramp-in holds the sweep to 0.01557 over the
    // first 512 samples, so 50x is only 0.778 -- over this test's 0.5 bar but
    // NEVER over full scale, and the test then passed with the +-1.0f clamp
    // DELETED. That is plan rev 1's "the test asserts nothing" defect one level
    // up. rampSamples = 30 ms * 48 kHz / 1000 = 1440, and from there w == 1, so
    // the sweep reaches 0.0999999 and 50x is 5.0 -- five times full scale, which
    // the clamp cannot silently survive.
    engine.setSoundcheckSampleIndex (1440);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 2, 512 };
    d (engine);

    bool sawSomething = false;
    bool sawTheClampEngage = false;
    for (float v : d.out[1])
    {
        ASSERT_LE (std::abs (v), 1.0f) << "a sample escaped the +-1.0f clamp";
        ASSERT_TRUE (std::isfinite (v));
        if (std::abs (v) > 0.5f) sawSomething = true;
        // The clamp did not merely permit this block -- it ACTED on it. Without
        // this the test cannot tell "the signal stayed under full scale" from
        // "the clamp caught it", and only the second proves invariant 4.
        if (std::abs (v) >= 1.0f - 1.0e-6f) sawTheClampEngage = true;
    }
    EXPECT_TRUE (sawSomething)
        << "the seam produced nothing over 0.5 -- the test proves nothing, and "
           "that is exactly what plan rev 1's peak seam did";
    EXPECT_TRUE (sawTheClampEngage)
        << "nothing reached the +-1.0f bound, so this block never exercised the "
           "clamp at all";
}

// RED IF: any other output channel receives a sample from the soundcheck path.
// It is the ONLY test for invariant 5. N6.
TEST (AudioEngineSoundcheck, SweepTouchesOnlyTheMeasuredChannel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    engine.setMode (AudioEngine::Mode::Bypass);

    for (int ch = 0; ch < 4; ++ch)
        routeMono (engine, ch, ch, ch);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (2);

    MultiDriver d { 4, 256, 0.25f };
    d (engine);

    // Channels 0, 1, 3 still pass their own input through, untouched.
    for (int ch : { 0, 1, 3 })
        for (int n = 0; n < 256; ++n)
            ASSERT_FLOAT_EQ (d.out[(std::size_t) ch][(std::size_t) n], 0.25f)
                << "channel " << ch << " sample " << n;
}

// RED IF: the ramp-out is driven from the controller thread instead of being
// generated inside the callback. Nothing but the callback runs here -- no
// controller, no poll -- and the sweep must still reach exactly zero and release
// the channel on its own. inv 9, F8.
TEST (AudioEngineSoundcheck, AbortRampsDownInTheCallbackAlone)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (4800);         // mid-sweep
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 2, 64 };
    d (engine);
    ASSERT_GT (d.peakOn (1), 0.0f);

    engine.requestSoundcheckRampOut();

    // kRampOutMs = 30 ms; at 48 kHz that is 1440 samples = 23 callbacks of 64.
    for (int i = 0; i < 40; ++i)
        d (engine);

    EXPECT_FLOAT_EQ (d.peakOn (1), 0.0f);
    EXPECT_EQ (engine.getSoundcheckOutputChannel(), -1)
        << "the callback must release the channel itself";
    EXPECT_FALSE (engine.soundcheckIsEmitting());
}

// RED IF: tap writes continue while a run is in flight.
//
// I-1: the MEASURED channel's own lane proves nothing here. It is muted, so it
// never sets tapSource and never writes a tap whether the suspension works or
// not -- the original version of this test asserted on that lane alone and was
// therefore green against a broken suspension. The lane that matters is one
// routed to an UNMEASURED output: it is processing normally, it HAS a
// tapSource, and the only thing keeping its ring empty is scSuspendTaps_.
// inv 10.
TEST (AudioEngineSoundcheck, TapsAreSuspendedDuringARun)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);   // the MEASURED channel -- muted, no tapSource
    routeMono (engine, 1, 1, 1);   // UNMEASURED -- normal programme, normal tap

    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (0);

    MultiDriver d { 2, 256 };
    for (int i = 0; i < 8; ++i) d (engine);

    EXPECT_EQ (engine.getTapBuffer (1, 0).getAvailableRead(), 0u)
        << "the UNMEASURED lane was tapped during the run -- the detector is "
           "being fed while the sweep is in the room";
    EXPECT_EQ (engine.getTapBuffer (0, 0).getAvailableRead(), 0u);
    EXPECT_EQ (engine.getTapDropCount (1, 0), 0u)
        << "a SKIP is not a DROP -- tapDropCounts_ must not move";

    // Control: the same lane DOES tap once the suspension is lifted, so the
    // assertion above is not passing merely because the route is broken.
    engine.setSoundcheckTapsSuspended (false);
    engine.setSoundcheckOutputChannel (-1);
    d (engine);
    EXPECT_GT (engine.getTapBuffer (1, 0).getAvailableRead(), 0u)
        << "the unmeasured lane never taps at all, so this test proves nothing";
}

// RED IF: tap suspension is keyed on scOutChannel_ rather than on
// scSuspendTaps_. scOutChannel_ goes to -1 at every Gap, so the taps would come
// back for 300 ms between channels, restarting lane G's ~420 ms tapAlive window
// per channel: ~0.72 s each, ~11.5 s over 16 channels, past kReleaseStepMs =
// 10 s. inv 10, N3.
TEST (AudioEngineSoundcheck, TapsStaySuspendedAcrossTheGap)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);   // measured
    routeMono (engine, 1, 1, 1);   // unmeasured -- the lane that can actually tap

    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (0);

    MultiDriver d { 2, 256 };
    for (int i = 0; i < 8; ++i) d (engine);
    ASSERT_EQ (engine.getTapBuffer (1, 0).getAvailableRead(), 0u);

    // The Gap: the channel is released, the suspend flag is NOT.
    engine.setSoundcheckOutputChannel (-1);
    const auto before0 = engine.getTapBuffer (0, 0).getAvailableRead();
    const auto before1 = engine.getTapBuffer (1, 0).getAvailableRead();
    for (int i = 0; i < 64; ++i) d (engine);    // 64 x 256 = 16384 samples ~ 341 ms > kGapMs
    EXPECT_EQ (engine.getTapBuffer (0, 0).getAvailableRead(), before0)
        << "a tap was written during the Gap";
    EXPECT_EQ (engine.getTapBuffer (1, 0).getAvailableRead(), before1)
        << "the unmeasured lane was tapped during the Gap";

    EXPECT_EQ (engine.getTapDropCount (0, 0), 0u)
        << "a SKIP is not a DROP -- tapDropCounts_ must not move";
    EXPECT_EQ (engine.getTapDropCount (1, 0), 0u);
}

// RED IF: capture is gated on scOutChannel_ instead of on its own flag, or the
// noise-floor phase emits. The NoiseFloor phase has a live channel, capture on,
// and a NEGATIVE sample index. inv 6, F5.
TEST (AudioEngineSoundcheck, NoiseFloorCapturesWithoutEmitting)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckCaptureChannel (1);
    engine.setSoundcheckCaptureActive (true);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (-24000);          // 0.5 s before the sweep

    MultiDriver d { 2, 256, 0.3f };
    d (engine);

    EXPECT_EQ (d.peakOn (0), 0.0f) << "the noise-floor phase emitted";
    EXPECT_GE (engine.getMicCaptureBuffer().getAvailableRead(), 256u);

    // And with capture off, nothing arrives even though the channel is live.
    engine.setSoundcheckCaptureActive (false);
    const auto have = engine.getMicCaptureBuffer().getAvailableRead();
    d (engine);
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), have);
}

// RED IF: the per-callback bounds check is dropped. A device restart onto fewer
// channels would then be an out-of-bounds write on the realtime thread. inv 3,
// F3.
TEST (AudioEngineSoundcheck, OutOfRangeChannelIsIgnored)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckCaptureActive (true);

    // THREE allocated channels, but the engine is told about TWO (M-2). Channel
    // 2 is a guard buffer: it exists and is readable, so an unchecked write to
    // the out-of-range index lands somewhere this test can assert on, rather
    // than in undefined behaviour that may or may not crash on the day.
    MultiDriver d { 3, 128 };
    d.declaredChannels = 2;

    engine.setSoundcheckOutputChannel (2);       // == numOutputChannels
    engine.setSoundcheckCaptureChannel (9);
    d (engine);
    EXPECT_EQ (d.peakOn (0), 0.25f);             // slot 0 still passes through
    EXPECT_EQ (d.peakOn (1), 0.0f);
    EXPECT_EQ (d.peakOn (2), 0.0f)
        << "the sweep was written to a channel this callback was never given";
    EXPECT_EQ (engine.getSoundcheckSampleIndex(), 0)
        << "an out-of-range channel must behave as if no soundcheck were "
           "running -- the sweep clock must not advance either";

    engine.setSoundcheckOutputChannel (-5);
    d (engine);
    EXPECT_EQ (d.peakOn (2), 0.0f);
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), 0u);
}

// RED IF: the atomics are read at more than one point in the callback. A flip
// between the mute decision and the tap decision produces a callback that BOTH
// injects the sweep AND taps it into the detector -- the poisoning case this
// lane exists to prevent. inv 7, F4.
TEST (AudioEngineSoundcheck, AtomicsAreSnapshottedOnce)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckSampleIndex (0);

    MultiDriver d { 2, 64 };

    // Callbacks driven while another thread flips the channel and the suspend
    // flag as fast as it can. Each callback must be internally consistent: if
    // it emitted, it must NOT have tapped, and vice versa.
    //
    // 20000, not the 200 this test was first written with. Measured 2026-09-16
    // against two deliberate mutants -- a re-read of scOutChannel_ at the mute
    // decision, and one at the injection point -- 200 callbacks caught them in
    // roughly 1 run out of 60: a guard that fires 2% of the time is not a
    // guard. The first disagreement typically lands near callback 200, i.e.
    // once the flipper thread has actually been scheduled, so the old bound sat
    // exactly on the edge. At 20000 the two mutants are caught 10/10 and 9/10
    // over separate processes, the real code passed 15/15, and the whole test
    // still runs in 0.10 s.
    std::atomic<bool> stop { false };
    std::thread flipper ([&engine, &stop]
    {
        while (! stop.load())
        {
            engine.setSoundcheckOutputChannel (0);
            engine.setSoundcheckTapsSuspended (true);
            engine.setSoundcheckOutputChannel (-1);
            engine.setSoundcheckTapsSuspended (false);
        }
    });

    for (int i = 0; i < 20000; ++i)
    {
        // M-1: the callback advances scSampleIndex_ on every emitting block, so
        // after ~2250 of them the index runs past totalSamples_ (3.0 s x 48 kHz
        // = 144000), sampleAt() returns 0 for the rest of the run, `emitted` is
        // never true again and the assertion below becomes vacuous -- it would
        // pass against anything. Rewinding every 1000 iterations keeps the
        // index under 64000 and the test honest.
        if (i % 1000 == 0)
            engine.setSoundcheckSampleIndex (0);

        const auto tapBefore = engine.getTapBuffer (0, 0).getAvailableRead();
        d (engine);
        const auto tapAfter  = engine.getTapBuffer (0, 0).getAvailableRead();
        const bool tapped    = tapAfter != tapBefore;
        const bool emitted   = d.peakOn (0) > 0.0f && d.peakOn (0) != 0.25f;

        ASSERT_FALSE (tapped && emitted)
            << "callback " << i << " both injected and tapped -- the atomics were re-read";
        engine.getTapBuffer (0, 0).clear();
    }

    stop.store (true);
    flipper.join();
}

// RED IF: an idle engine emits anything, OR mutes a lane. inv 6.
TEST (AudioEngineSoundcheck, IdleEngineEmitsNoSweepAndMutesNoLane)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    engine.setMode (AudioEngine::Mode::Bypass);
    routeMono (engine, 0, 0, 0);
    routeMono (engine, 1, 1, 1);

    MultiDriver d { 2, 256, 0.4f };
    d (engine);

    // Both lanes contributed normally; nothing was muted and nothing injected.
    for (int ch : { 0, 1 })
        for (int n = 0; n < 256; ++n)
            ASSERT_FLOAT_EQ (d.out[(std::size_t) ch][(std::size_t) n], 0.4f) << "ch " << ch;

    EXPECT_EQ (engine.getSoundcheckOutputChannel(), -1);
    EXPECT_FALSE (engine.soundcheckIsEmitting());
}

// RED IF: micCapture_.clear() is left out of audioDeviceAboutToStart's drain
// block. Audio captured at the PREVIOUS device's sample rate would be spliced
// onto the front of the next run's first analysis windows -- the same defect the
// tap rings are already cleared to avoid. F15.
TEST (AudioEngineSoundcheck, DeviceRestartDrainsTheCaptureRing)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckCaptureChannel (1);
    engine.setSoundcheckCaptureActive (true);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckOutputChannel (0);
    engine.setSoundcheckSampleIndex (-1000);

    MultiDriver d { 2, 256, 0.3f };
    d (engine);
    ASSERT_GT (engine.getMicCaptureBuffer().getAvailableRead(), 0u);

    engine.audioDeviceAboutToStart (nullptr);
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), 0u);
}

// ===================================================================
// Review round 1 additions: what happens AROUND the four insertion points.
// ===================================================================

// RED IF: the soundcheck atomics survive a device stop (C-1). None of them
// belongs to a device, and MainComponent aborts and JOINS the controller before
// a restart reaches audioDeviceAboutToStart -- so if that function does not
// stand them down, nothing does, and the next device to open emits sweep on
// channel N and mutes every lane routed there with nobody left to stop it.
TEST (AudioEngineSoundcheck, DeviceRestartClearsTheSoundcheckAtomics)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    engine.setMode (AudioEngine::Mode::Bypass);
    routeMono (engine, 0, 0, 0);

    // A run in full flight: emitting, capturing, taps suspended.
    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckCaptureChannel (1);
    engine.setSoundcheckCaptureActive (true);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckSampleIndex (1522);
    engine.setSoundcheckOutputChannel (0);
    engine.requestSoundcheckRampOut();

    MultiDriver d { 2, 256, 0.3f };
    d (engine);
    ASSERT_GT (engine.getMicCaptureBuffer().getAvailableRead(), 0u);

    // The device goes away and comes back.
    engine.audioDeviceAboutToStart (nullptr);

    EXPECT_EQ (engine.getSoundcheckOutputChannel(), -1);
    EXPECT_FALSE (engine.soundcheckIsEmitting());

    d (engine);

    // Lane 0 is un-muted and passes its input through; nothing was emitted,
    // nothing captured, and the taps are running again.
    for (int n = 0; n < 256; ++n)
        ASSERT_FLOAT_EQ (d.out[0][(std::size_t) n], 0.3f)
            << "sample " << n << " -- the channel is still muted or still swept";
    EXPECT_EQ (engine.getMicCaptureBuffer().getAvailableRead(), 0u)
        << "capture is still armed after the device restarted";
    EXPECT_GT (engine.getTapBuffer (0, 0).getAvailableRead(), 0u)
        << "the taps are still suspended after the device restarted";
}

// RED IF: the ramp-out anchor is latched by the message thread instead of by
// the callback (I-2). rampOut(anchor, anchor, R) is exactly 1.0, so latching in
// the callback opens the envelope at full scale at EVERY buffer size. An anchor
// taken from the message thread's view of the index can be a full buffer behind
// the block that first applies it, opening the envelope at
// 0.5*(1 + cos(pi*N/R)) instead: 0.999 at 64 samples, 0.720 at 512, and EXACTLY
// 0 at 2048 -- a hard cut, which is the click the ramp exists to prevent.
TEST (AudioEngineSoundcheck, RampOutStartsAtFullScaleAtAnyBufferSize)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    // 1522: past the sweep's own 1440-sample ramp-in, and chosen because
    // |sampleAt(1522)| = 0.0999989 -- within 1e-6 of the full -20 dBFS peak. A
    // start index where the sweep happens to cross zero would make the
    // comparison below true for any envelope at all.
    const std::int64_t start = 1522;

    for (int frames : { 64, 512, 2048 })
    {
        AudioEngine plain;
        plain.setRunningForTest (true);
        routeMono (plain, 0, 0, 0);
        plain.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
        plain.setSoundcheckTapsSuspended (true);
        plain.setSoundcheckSampleIndex (start);
        plain.setSoundcheckOutputChannel (1);

        MultiDriver ref { 2, frames };
        ref (plain);

        AudioEngine ramped;
        ramped.setRunningForTest (true);
        routeMono (ramped, 0, 0, 0);
        ramped.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
        ramped.setSoundcheckTapsSuspended (true);
        ramped.setSoundcheckSampleIndex (start);
        ramped.setSoundcheckOutputChannel (1);
        ramped.requestSoundcheckRampOut();      // BETWEEN callbacks

        MultiDriver d { 2, frames };
        d (ramped);

        ASSERT_GT (std::abs (ref.out[1][0]), 1.0e-3f)
            << "frames " << frames << ": the un-ramped reference sample is ~0, "
               "so the comparison below would prove nothing";
        EXPECT_GE (std::abs (d.out[1][0]), 0.99f * std::abs (ref.out[1][0]))
            << "frames " << frames << ": the ramp-out envelope did not open at "
               "full scale -- the first sample is a step, not a fade";
    }
}

// RED IF: a ramp-out anchor left behind by an aborted run survives into the
// next one (C-2). The abort path that produces it is real: the controller
// requests the ramp-out, then the device changes and the channel index stops
// being valid, so NO emitting callback ever runs to clear the anchor itself.
// setSoundcheckOutputChannel() is what must clear it, on any value.
TEST (AudioEngineSoundcheck, StaleRampAnchorDoesNotTruncateTheNextRun)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckSampleIndex (1440);
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 2, 256 };
    d (engine);                                 // emitting; index -> 1696

    // The abort the callback can never finish.
    engine.requestSoundcheckRampOut();
    engine.setSoundcheckOutputChannel (7);      // device_changed: out of range
    d (engine);                                 // scOut == -1: nothing happens

    // Re-arm a fresh run from the top of the flat part of the sweep.
    engine.setSoundcheckSampleIndex (1440);
    engine.setSoundcheckOutputChannel (1);

    SoundcheckSignal::Params p;
    p.sampleRate = engine.getCurrentSampleRateHz();
    p.peak       = SoundcheckSignal::kSoundcheckMaxPeak;
    const SoundcheckSignal expected { p };

    // Two blocks: the first covers 1440-1695, which is still BEFORE the stale
    // anchor at 1696 and would pass even with the bug. The second crosses it.
    for (int block = 0; block < 2; ++block)
    {
        d (engine);
        for (int n = 0; n < 256; ++n)
            ASSERT_NEAR (d.out[1][(std::size_t) n],
                         expected.sampleAt (1440 + block * 256 + n), 1.0e-6f)
                << "block " << block << " sample " << n
                << " -- the new run is being faded by the previous run's anchor";
    }
    EXPECT_TRUE (engine.soundcheckIsEmitting());
}

// RED IF: a muted lane's notch chain keeps its filter state (C-3). Muting by
// skipping the lane freezes the biquads' persistent Direct Form I state for as
// long as the mute lasts -- up to 4.5 s per channel -- and on un-mute that
// stored energy discharges as a free response on top of live programme: a click
// on every channel the soundcheck touched. Resetting the chain on every muted
// block makes the un-mute resume from zero state, so silence in is silence out.
TEST (AudioEngineSoundcheck, UnmuteResumesFromZeroFilterState)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    engine.setMode (AudioEngine::Mode::Auto);   // Bypass would skip the chain
    routeMono (engine, 0, 0, 0);

    // A high-Q notch: the narrower it is, the longer its stored energy rings.
    auto& q = engine.getCommandQueue();
    const NotchCommand set { NotchCommandType::Set, 0, 0, 1000.0f, 30.0f, -18.0f, 0 };
    ASSERT_EQ (q.write (&set, 1), 1u);

    MultiDriver d { 2, 256 };

    // Charge the filter state with a tone AT the notch frequency.
    for (int n = 0; n < 256; ++n)
        d.in[0][(std::size_t) n] = 0.5f * std::sin (2.0f * 3.14159265f * 1000.0f
                                                    * (float) n / 48000.0f);
    for (int i = 0; i < 4; ++i) d (engine);
    ASSERT_GT (d.peakOn (0), 0.0f) << "the chain never saw any signal";

    // Mute: the soundcheck takes this output channel. A NEGATIVE sample index
    // keeps it in the noise-floor phase, so the channel is silent rather than
    // swept and the assertion below is about the mute, not about the sweep.
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckSampleIndex (-48000);
    engine.setSoundcheckOutputChannel (0);
    for (int i = 0; i < 4; ++i) d (engine);
    ASSERT_EQ (d.peakOn (0), 0.0f) << "the muted channel was not silent";

    // Un-mute into SILENCE. With the chain reset on every muted block there is
    // no stored state left, so a zero input can only produce a zero output.
    for (auto& v : d.in) std::fill (v.begin(), v.end(), 0.0f);
    engine.setSoundcheckOutputChannel (-1);
    engine.setSoundcheckTapsSuspended (false);
    d (engine);

    for (int n = 0; n < 256; ++n)
        ASSERT_FLOAT_EQ (d.out[0][(std::size_t) n], 0.0f)
            << "sample " << n << " is the free response of stale filter state -- "
               "on a PA that is a click on every channel the soundcheck touched";
}

// ===================================================================
// Review round 2 additions.
// ===================================================================

// RED IF: a muted lane's chain is reset() instead of clearState() (N-1).
// reset() also zeroes rampRemaining_, so on a mute that can last 4.5 s it
// cancels an in-flight lane-G depth ramp on EVERY block -- and command draining
// is NOT suspended during a run, so a retune landing mid-mute is entirely
// normal. The filter would strand at the intermediate depth while
// NotchInfo.depthDB already reads the target: the rare NaN-heal GAP turned into
// routine behaviour.
TEST (AudioEngineSoundcheck, MuteDoesNotCancelAnInFlightDepthRamp)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    engine.setMode (AudioEngine::Mode::Auto);   // Bypass would skip the chain
    routeMono (engine, 0, 0, 0);

    auto& q = engine.getCommandQueue();
    const NotchCommand place { NotchCommandType::Set, 0, 0, 1000.0f, 8.0f, -6.0f, 0 };
    ASSERT_EQ (q.write (&place, 1), 1u);

    MultiDriver d { 2, 64 };
    d (engine);                     // the notch is placed

    // A depth-only retune of the SAME notch (same freq, same Q): NotchChain
    // ::setNotch takes the rampNotchDepth path, kRampMs = 10 ms = 480 samples
    // at 48 kHz.
    const NotchCommand deepen { NotchCommandType::Set, 0, 0, 1000.0f, 8.0f, -18.0f, 0 };
    ASSERT_EQ (q.write (&deepen, 1), 1u);
    d (engine);                     // 64 of the 480 ramp samples are spent

    const NotchChain& chain = engine.getNotchChainForTest (0, 0);
    const int remainingBeforeMute = chain.getFilterForTest (0).rampRemainingForTest();
    ASSERT_EQ (remainingBeforeMute, 480 - 64)
        << "the retune did not start a depth ramp, so this test proves nothing";

    // Mute the lane for a while. A negative sweep index keeps the soundcheck in
    // its noise-floor phase, so the channel is silent rather than swept.
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckSampleIndex (-48000);
    engine.setSoundcheckOutputChannel (0);
    for (int i = 0; i < 20; ++i) d (engine);

    EXPECT_EQ (chain.getFilterForTest (0).rampRemainingForTest(), remainingBeforeMute)
        << "the mute cancelled the in-flight depth ramp -- reset() was called "
           "where clearState() belongs";

    // Un-mute and let the ramp finish: 416 samples left, 8 blocks of 64.
    engine.setSoundcheckOutputChannel (-1);
    engine.setSoundcheckTapsSuspended (false);
    for (int i = 0; i < 8; ++i) d (engine);

    EXPECT_EQ (chain.getFilterForTest (0).rampRemainingForTest(), 0)
        << "the ramp did not finish after the un-mute";

    // ...and it landed on the TARGET design, not on an intermediate one.
    Biquad target;
    ASSERT_TRUE (target.setNotchFilter (1000.0, 8.0, 48000.0, -18.0));
    const Biquad::Coeffs want = target.coeffsForTest();
    const Biquad::Coeffs got  = chain.getFilterForTest (0).coeffsForTest();
    EXPECT_DOUBLE_EQ (got.b0, want.b0);
    EXPECT_DOUBLE_EQ (got.b1, want.b1);
    EXPECT_DOUBLE_EQ (got.b2, want.b2);
    EXPECT_DOUBLE_EQ (got.a1, want.a1);
    EXPECT_DOUBLE_EQ (got.a2, want.a2);
}

// Pins what an abort requested during the NOISE-FLOOR phase actually does,
// because it is not what anyone reading the callback assumes at first glance.
//
// The sweep index is NEGATIVE during the noise floor, so the anchor the
// callback latches is negative too -- and -1 is also the "no anchor" sentinel,
// so `rampAt >= 0` is false: the envelope is neither applied nor completed, and
// the request is simply re-latched on each block until the index reaches 0.
// Nothing is EMITTED in the meantime (sampleAt returns 0 for a negative index),
// but the channel stays muted and the taps stay suspended for the rest of the
// noise floor -- up to ~0.5 s.
//
// That is why the controller's abort backstop is setSoundcheckOutputChannel(-1)
// and not requestSoundcheckRampOut() alone. This test exists so the comment on
// the atomics cannot drift away from the code.
TEST (AudioEngineSoundcheck, AbortDuringTheNoiseFloorIsDeferredUntilTheSweepStarts)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckSampleIndex (-24000);   // 0.5 s of noise floor still to run
    engine.setSoundcheckOutputChannel (1);

    MultiDriver d { 2, 256 };
    d (engine);                                 // index -> -23744
    ASSERT_TRUE (engine.soundcheckIsEmitting());

    engine.requestSoundcheckRampOut();

    // 50 blocks = 12800 samples: still 10944 samples of noise floor to go.
    for (int i = 0; i < 50; ++i) d (engine);
    EXPECT_TRUE (engine.soundcheckIsEmitting())
        << "the deferral this test documents is gone -- if the negative-anchor "
           "case was fixed, replace this test with the immediate-abort one";
    EXPECT_EQ (d.peakOn (1), 0.0f) << "the noise-floor phase emitted";

    int blocks = 0;
    while (engine.soundcheckIsEmitting() && blocks < 400)
    {
        d (engine);
        ++blocks;
    }

    EXPECT_FALSE (engine.soundcheckIsEmitting()) << "the abort was never honoured";
    // 10944 samples of noise floor = 43 blocks of 256, then the anchor latches
    // at the first non-negative index and one ramp length (kRampOutMs = 30 ms =
    // 1440 samples = 6 blocks) finishes it. A couple of blocks of slack.
    EXPECT_LE (blocks, 43 + 8)
        << "the abort took longer than the remaining noise floor plus one ramp";
}

// The backstop that makes the deferral above harmless: one store puts the
// soundcheck side of the engine back to idle, with no callback needed.
TEST (AudioEngineSoundcheck, SettingTheChannelToMinusOneIsTheAbortBackstop)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    engine.setRunningForTest (true);
    engine.setMode (AudioEngine::Mode::Bypass);
    routeMono (engine, 0, 0, 0);

    engine.setSoundcheckPeak (SoundcheckSignal::kSoundcheckMaxPeak);
    engine.setSoundcheckTapsSuspended (true);
    engine.setSoundcheckSampleIndex (-24000);
    engine.setSoundcheckOutputChannel (0);

    MultiDriver d { 2, 256, 0.3f };
    d (engine);
    ASSERT_TRUE (engine.soundcheckIsEmitting());
    ASSERT_EQ (d.peakOn (0), 0.0f) << "the lane should be muted here";

    engine.requestSoundcheckRampOut();
    engine.setSoundcheckOutputChannel (-1);     // the backstop

    EXPECT_FALSE (engine.soundcheckIsEmitting());

    d (engine);
    for (int n = 0; n < 256; ++n)
        ASSERT_FLOAT_EQ (d.out[0][(std::size_t) n], 0.3f)
            << "sample " << n << " -- the lane is still muted after the abort";
}
