// PresetManager tests -- the FORMAT half of plan Task 25, plus Task 26.
//
// What is deliberately absent
// ===========================
// Nothing here mentions AudioEngine, NotchChain or the detector. Owner
// decision D-05 routes every notch command through the detector, which does
// not exist yet, so installing a loaded preset into a running engine cannot be
// built. What CAN be built -- and is what these tests pin down -- is the file
// format itself: a pure translation between a plain struct and JSON on disk,
// testable with no audio device present.
//
// Why the validation tests outnumber the round-trip tests
// =======================================================
// A preset file lives in %APPDATA% and is plain text. It is the easiest way
// for a user, a text editor or a botched sync to get a bad value into a DSP
// chain that feeds a PA system. Biquad.h documents what those bad values do: a
// notch above Nyquist puts the poles outside the unit circle and the measured
// output reaches 4.1e18 within 1000 samples. The loader is the last place that
// can refuse such a file while the consequence is still a message on screen
// rather than full-scale garbage into the drivers.
//
// So the loader is tested mostly on files it did not write itself.

#include <gtest/gtest.h>

#include "app/PresetManager.h"

// Included ONLY for the static_assert below. PresetManager itself must not
// depend on the dsp layer -- see the comment on PresetManager::MAX_NOTCHES.
#include "dsp/NotchChain.h"

#include <juce_core/juce_core.h>

// The preset format bounds "index" by its own constant so that the file
// format does not include an audio header. The two must still agree: a preset
// that addresses slot 16 of a 16-slot chain is unloadable. Making this a
// compile error means the disagreement can never reach a test run, let alone
// a PA system.
static_assert (PresetManager::MAX_NOTCHES == NotchChain::MAX_NOTCHES,
               "PresetManager::MAX_NOTCHES has drifted from NotchChain::MAX_NOTCHES");

namespace
{

// The plan Task 25 sample, verbatim, as the baseline every malformed case
// mutates away from.
const char* kValidPresetJson = R"({
  "version": "1.0",
  "device": "Audient iD14 MK2",
  "sampleRate": 48000,
  "bufferSize": 64,
  "notches": [
    {"index": 0, "freq": 482.0, "Q": 30.0, "depth": -12.0}
  ]
})";

Preset makeValidPreset()
{
    Preset p;
    p.version    = "1.0";
    p.device     = "Audient iD14 MK2";
    p.sampleRate = 48000.0;
    p.bufferSize = 64;
    p.notches    = { PresetNotch { 0, 482.0,  30.0, -12.0 },
                     PresetNotch { 3, 1240.5, 25.0, -18.0 } };
    return p;
}

// Renders a one-notch preset with a single field replaced, so each malformed
// test differs from a known-good file in exactly one place.
juce::String presetWithNotchField (const juce::String& field, const juce::String& value)
{
    juce::String index = "0", freq = "482.0", q = "30.0", depth = "-12.0";

    if (field == "index") index = value;
    if (field == "freq")  freq  = value;
    if (field == "Q")     q     = value;
    if (field == "depth") depth = value;

    return juce::String (R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,)")
         + R"("notches":[{"index":)" + index
         + R"(,"freq":)"  + freq
         + R"(,"Q":)"     + q
         + R"(,"depth":)" + depth + "}]}";
}

// A directory under %TEMP% that is deleted when the test ends. Deliberately
// NOT created by the constructor, so a test can assert that saveToFile makes
// its own parent directory.
class ScopedTempDir
{
public:
    ScopedTempDir()
        : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("HandsFreePresetTests")
                   .getChildFile (juce::Uuid().toString()))
    {
    }

    ~ScopedTempDir() { dir.deleteRecursively(); }

    juce::File dir;
};

} // namespace

//==============================================================================
// Round trip
//==============================================================================

// Fails if any field is dropped, renamed or retyped on the way out or back in.
TEST (PresetManager, RoundTripPreservesEveryField)
{
    const Preset original = makeValidPreset();

    const auto result = PresetManager::fromJSON (PresetManager::toJSON (original));

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_EQ (result.preset.version,    original.version);
    EXPECT_EQ (result.preset.device,     original.device);
    EXPECT_DOUBLE_EQ (result.preset.sampleRate, original.sampleRate);
    EXPECT_EQ (result.preset.bufferSize, original.bufferSize);

    ASSERT_EQ (result.preset.notches.size(), original.notches.size());

    for (size_t i = 0; i < original.notches.size(); ++i)
    {
        EXPECT_EQ (result.preset.notches[i].index, original.notches[i].index) << "notch " << i;
        EXPECT_DOUBLE_EQ (result.preset.notches[i].freq,    original.notches[i].freq)    << "notch " << i;
        EXPECT_DOUBLE_EQ (result.preset.notches[i].Q,       original.notches[i].Q)       << "notch " << i;
        EXPECT_DOUBLE_EQ (result.preset.notches[i].depthDB, original.notches[i].depthDB) << "notch " << i;
    }
}

// Fails the instant anyone tidies the depth with std::abs or a sign flip.
// Separate from the round-trip test because this is the field most likely to
// be quietly corrected by a well-meaning reader, and a bulk assertion makes it
// easy to miss which field broke.
TEST (PresetManager, DepthKeepsItsNegativeSignThroughTheFile)
{
    Preset p = makeValidPreset();
    p.notches = { PresetNotch { 0, 482.0, 30.0, -18.0 } };

    const juce::String text = PresetManager::toJSON (p);
    EXPECT_TRUE (text.contains ("-18")) << text;

    const auto result = PresetManager::fromJSON (text);
    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_DOUBLE_EQ (result.preset.notches[0].depthDB, -18.0);
    EXPECT_LT (result.preset.notches[0].depthDB, 0.0);
}

// AudioEngine has TWO sample-rate getters: getCurrentSampleRate() returns the
// display string "48000 Hz" and getCurrentSampleRateHz() returns the number.
// Reaching for the wrong one puts a formatted string into the preset file.
// Fails if the writer ever emits sampleRate as anything but a JSON number.
TEST (PresetManager, SampleRateIsWrittenAsANumberNotADisplayString)
{
    const juce::String text = PresetManager::toJSON (makeValidPreset());

    juce::var parsed;
    ASSERT_TRUE (juce::JSON::parse (text, parsed).wasOk()) << text;

    const juce::var rate = parsed["sampleRate"];
    EXPECT_FALSE (rate.isString()) << "sampleRate was written as text: " << text;
    EXPECT_TRUE  (rate.isInt() || rate.isInt64() || rate.isDouble());
    EXPECT_DOUBLE_EQ (static_cast<double> (rate), 48000.0);
    EXPECT_FALSE (text.contains ("Hz")) << text;
}

// A "clear all" preset is a legitimate thing to save.
TEST (PresetManager, APresetWithNoNotchesIsValid)
{
    Preset p = makeValidPreset();
    p.notches.clear();

    const auto result = PresetManager::fromJSON (PresetManager::toJSON (p));
    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_TRUE (result.preset.notches.empty());
}

// The plan sample must parse. Fails if the reader drifts away from the
// documented format.
TEST (PresetManager, ThePlanSampleFileParses)
{
    const auto result = PresetManager::fromJSON (kValidPresetJson);

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_EQ (result.preset.device, "Audient iD14 MK2");
    EXPECT_DOUBLE_EQ (result.preset.sampleRate, 48000.0);
    EXPECT_EQ (result.preset.bufferSize, 64);
    ASSERT_EQ (result.preset.notches.size(), 1u);
    EXPECT_DOUBLE_EQ (result.preset.notches[0].freq, 482.0);
}

//==============================================================================
// Version -- DECISION: a strict allow-list, currently {"1.0"}
//==============================================================================

// Fails if the loader ever treats an unknown version as close enough.
TEST (PresetManager, AFutureVersionIsRefusedRatherThanReadAsCurrent)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"2.0","device":"d","sampleRate":48000,"bufferSize":64,"notches":[]})");

    EXPECT_FALSE (result.ok);
    EXPECT_TRUE (result.errors.joinIntoString ("; ").contains ("2.0"))
        << "the refusal should quote the version it did not understand: "
        << result.errors.joinIntoString ("; ");
}

TEST (PresetManager, AMissingVersionIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"device":"d","sampleRate":48000,"bufferSize":64,"notches":[]})");

    EXPECT_FALSE (result.ok);
}

TEST (PresetManager, ANumericVersionIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":1.0,"device":"d","sampleRate":48000,"bufferSize":64,"notches":[]})");

    EXPECT_FALSE (result.ok);
}

//==============================================================================
// Malformed input -- files the loader did not write
//==============================================================================

TEST (PresetManager, AnEmptyFileIsRefused)
{
    const auto result = PresetManager::fromJSON ("");

    EXPECT_FALSE (result.ok);
    EXPECT_FALSE (result.errors.isEmpty()) << "a refusal must say why";
}

TEST (PresetManager, ATruncatedFileIsRefused)
{
    const juce::String truncated = juce::String (kValidPresetJson).substring (0, 60);

    const auto result = PresetManager::fromJSON (truncated);
    EXPECT_FALSE (result.ok);
}

TEST (PresetManager, ValidJsonThatIsNotAPresetIsRefused)
{
    EXPECT_FALSE (PresetManager::fromJSON (R"([1, 2, 3])").ok);
    EXPECT_FALSE (PresetManager::fromJSON (R"({"hello":"world"})").ok);
    EXPECT_FALSE (PresetManager::fromJSON (R"("just a string")").ok);
}

TEST (PresetManager, ANotchesValueThatIsNotAnArrayIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,"notches":"none"})");

    EXPECT_FALSE (result.ok);
}

TEST (PresetManager, ANotchesArrayContainingAStringIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,"notches":["nope"]})");

    EXPECT_FALSE (result.ok);
}

TEST (PresetManager, ANotchMissingAKeyIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,)"
        R"("notches":[{"index":0,"freq":482.0,"Q":30.0}]})");

    EXPECT_FALSE (result.ok);
    EXPECT_TRUE (result.errors.joinIntoString ("; ").containsIgnoreCase ("depth"))
        << result.errors.joinIntoString ("; ");
}

// A quoted number is the single most likely hand-edit mistake.
TEST (PresetManager, AStringWhereANumberBelongsIsRefused)
{
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("freq", R"("482.0")")).ok);
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("Q",    R"("30")")).ok);
}

// juce::var has a distinct bool type; true must not slip through an
// isInt()-style check as 1.
TEST (PresetManager, ABooleanWhereANumberBelongsIsRefused)
{
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("freq", "true")).ok);
}

TEST (PresetManager, ANonNumericSampleRateIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":"48000 Hz","bufferSize":64,"notches":[]})");

    EXPECT_FALSE (result.ok);
}

//==============================================================================
// Value validation -- exactly the four inputs Biquad refuses, plus the chain
// index range
//==============================================================================

// Biquad.cpp refuses depthDB > 0 because the peaking form is symmetric: a
// positive gain BOOSTS the frequency that is already ringing. Catching it at
// load time turns a silently-dropped notch into a message.
TEST (PresetManager, APositiveDepthIsRefused)
{
    const auto result = PresetManager::fromJSON (presetWithNotchField ("depth", "12.0"));

    EXPECT_FALSE (result.ok);
    EXPECT_TRUE (result.errors.joinIntoString ("; ").containsIgnoreCase ("depth"))
        << result.errors.joinIntoString ("; ");
}

// Biquad accepts depthDB == 0 (A == 1, H == 1, a mathematical no-op). The
// loader must not be stricter than the filter it feeds.
TEST (PresetManager, AZeroDepthIsAcceptedBecauseTheFilterAcceptsIt)
{
    const auto result = PresetManager::fromJSON (presetWithNotchField ("depth", "0.0"));

    EXPECT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
}

// freq >= sampleRate/2 is the 4.1e18 defect in Biquad.h. Checked against the
// rate the FILE claims -- a file describing a notch that could not have
// existed at its own sample rate is corrupt, not merely inapplicable.
TEST (PresetManager, AFrequencyAtOrAboveTheStoredNyquistIsRefused)
{
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("freq", "24000.0")).ok);
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("freq", "30000.0")).ok);
    EXPECT_TRUE  (PresetManager::fromJSON (presetWithNotchField ("freq", "23999.0")).ok);
}

TEST (PresetManager, ANonPositiveFrequencyIsRefused)
{
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("freq", "0.0")).ok);
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("freq", "-482.0")).ok);
}

TEST (PresetManager, ANonPositiveQIsRefused)
{
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("Q", "0.0")).ok);
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("Q", "-30.0")).ok);
}

TEST (PresetManager, ANonPositiveSampleRateIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":0,"bufferSize":64,"notches":[]})");

    EXPECT_FALSE (result.ok);
}

//==============================================================================
// Chain shape -- NotchChain::MAX_NOTCHES is 16 and slots are index-addressed
//==============================================================================

TEST (PresetManager, AnIndexOutsideTheChainIsRefused)
{
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("index", "16")).ok);
    EXPECT_FALSE (PresetManager::fromJSON (presetWithNotchField ("index", "-1")).ok);
    EXPECT_TRUE  (PresetManager::fromJSON (presetWithNotchField ("index", "15")).ok);
}

// DECISION: refuse rather than let the last one win. Two notches claiming one
// slot means the preset the user hears is not the preset in the file.
TEST (PresetManager, ADuplicateIndexIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,"notches":[)"
        R"({"index":2,"freq":482.0,"Q":30.0,"depth":-12.0},)"
        R"({"index":2,"freq":900.0,"Q":30.0,"depth":-12.0}]})");

    EXPECT_FALSE (result.ok);
    EXPECT_TRUE (result.errors.joinIntoString ("; ").containsIgnoreCase ("duplicate"))
        << result.errors.joinIntoString ("; ");
}

TEST (PresetManager, MoreNotchesThanTheChainHasSlotsIsRefused)
{
    juce::String text (R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,"notches":[)");

    for (int i = 0; i < 17; ++i)
        text += juce::String (i == 0 ? "" : ",")
              + R"({"index":)" + juce::String (i)
              + R"(,"freq":482.0,"Q":30.0,"depth":-12.0})";

    text += "]}";

    const auto result = PresetManager::fromJSON (text);
    EXPECT_FALSE (result.ok);
}

//==============================================================================
// Reporting
//==============================================================================

// Reject and report, one message per problem. Fails if the loader
// short-circuits on the first fault and makes the user fix a broken file one
// round trip at a time.
TEST (PresetManager, EveryProblemIsReportedNotJustTheFirst)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,"notches":[)"
        R"({"index":0,"freq":-1.0,"Q":30.0,"depth":-12.0},)"
        R"({"index":1,"freq":482.0,"Q":-2.0,"depth":-12.0},)"
        R"({"index":2,"freq":482.0,"Q":30.0,"depth":6.0}]})");

    EXPECT_FALSE (result.ok);
    EXPECT_GE (result.errors.size(), 3) << result.errors.joinIntoString ("; ");
}

//==============================================================================
// Device -- DECISION: metadata, not a gate
//==============================================================================

// A soundman opening last week preset on a different interface is a normal
// Tuesday. Fails if anyone makes the device name a precondition for loading.
TEST (PresetManager, APresetFromAnInterfaceThatIsNotPresentStillLoads)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"Some Console Nobody Here Owns","sampleRate":48000,)"
        R"("bufferSize":64,"notches":[{"index":0,"freq":482.0,"Q":30.0,"depth":-12.0}]})");

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_EQ (result.preset.device, "Some Console Nobody Here Owns")
        << "the stored name must survive so the caller can warn about it";
    EXPECT_EQ (result.preset.notches.size(), 1u);
}

TEST (PresetManager, AMissingDeviceKeyIsRefusedBecauseTheKeyIsPartOfTheFormat)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","sampleRate":48000,"bufferSize":64,"notches":[]})");

    EXPECT_FALSE (result.ok);
}

//==============================================================================
// notchDefaults -- the character of a preset that has locked nothing yet
//==============================================================================
//
// Plan Task 26 asks for Speech (Q=40, depth -18) and Music (Q=25, depth -10).
// Spec 7 defines a preset as the list of notches ALREADY LOCKED, and a freshly
// shipped default has locked nothing, so those two numbers have nowhere to
// live in the format as the plan writes it. The block is OPTIONAL: the plan
// Task 25 sample has no notchDefaults and must keep parsing.

TEST (PresetManager, NotchDefaultsAreOptionalAndFallBackToTheDocumentedValues)
{
    const auto result = PresetManager::fromJSON (kValidPresetJson);

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.Q,       30.0);
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.depthDB, -12.0);
}

TEST (PresetManager, NotchDefaultsSurviveTheRoundTrip)
{
    Preset p = makeValidPreset();
    p.notchDefaults.Q       = 40.0;
    p.notchDefaults.depthDB = -18.0;

    const auto result = PresetManager::fromJSON (PresetManager::toJSON (p));

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.Q,       40.0);
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.depthDB, -18.0);
    EXPECT_LT (result.preset.notchDefaults.depthDB, 0.0);
}

// The same rules as a real notch. A bad default is worse than a bad notch,
// because it seeds every notch the detector goes on to create.
TEST (PresetManager, APositiveDefaultDepthIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,)"
        R"("notchDefaults":{"Q":40.0,"depth":18.0},"notches":[]})");

    EXPECT_FALSE (result.ok);
}

TEST (PresetManager, ANonPositiveDefaultQIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,)"
        R"("notchDefaults":{"Q":0.0,"depth":-18.0},"notches":[]})");

    EXPECT_FALSE (result.ok);
}

//==============================================================================
// bufferSize
//==============================================================================

// Fails if the reader ever static_casts a fractional value into an int and
// calls the truncation a load.
TEST (PresetManager, ANonIntegralBufferSizeIsRefused)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64.5,"notches":[]})");

    EXPECT_FALSE (result.ok);
}

//==============================================================================
// Where presets live
//==============================================================================

// Spec 7 and plan Task 25 both name %APPDATA%/AZSoundtech/HandsFree/presets.
// Fails if anyone hard-codes a path instead of asking JUCE for the platform
// location.
TEST (PresetManager, ThePresetDirectoryIsUnderTheUserApplicationDataDirectory)
{
    const juce::File appData =
        juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

    const juce::File presets = PresetManager::getPresetDirectory();

    EXPECT_TRUE (presets.isAChildOf (appData))
        << presets.getFullPathName() << " is not under " << appData.getFullPathName();
    EXPECT_EQ (presets.getFileName(), "presets");
    EXPECT_EQ (presets.getParentDirectory().getFileName(), "HandsFree");
    EXPECT_EQ (presets.getParentDirectory().getParentDirectory().getFileName(), "AZSoundtech");
}

//==============================================================================
// Disk round trip
//==============================================================================

TEST (PresetManager, SaveThenLoadFromDiskRoundTrips)
{
    ScopedTempDir temp;
    const juce::File file = temp.dir.getChildFile ("RoundTrip.json");

    const Preset original = makeValidPreset();

    juce::StringArray saveErrors;
    ASSERT_TRUE (PresetManager::saveToFile (original, file, saveErrors))
        << saveErrors.joinIntoString ("; ");
    ASSERT_TRUE (file.existsAsFile());

    const auto result = PresetManager::loadFromFile (file);

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_EQ (result.preset.device, original.device);
    ASSERT_EQ (result.preset.notches.size(), original.notches.size());
    EXPECT_DOUBLE_EQ (result.preset.notches[1].depthDB, original.notches[1].depthDB);
}

TEST (PresetManager, SavingCreatesThePresetDirectoryIfItIsMissing)
{
    ScopedTempDir temp;
    const juce::File file = temp.dir.getChildFile ("nested").getChildFile ("New.json");

    ASSERT_FALSE (file.getParentDirectory().exists());

    juce::StringArray saveErrors;
    EXPECT_TRUE (PresetManager::saveToFile (makeValidPreset(), file, saveErrors))
        << saveErrors.joinIntoString ("; ");
    EXPECT_TRUE (file.existsAsFile());
}

// The writer must never be able to produce a file the reader refuses, or the
// app can save a preset it can never open again. Fails if saveToFile skips
// validation.
TEST (PresetManager, SavingRefusesAPresetItsOwnLoaderWouldRejectAndWritesNothing)
{
    ScopedTempDir temp;
    const juce::File file = temp.dir.getChildFile ("Bad.json");

    Preset bad = makeValidPreset();
    bad.notches[0].depthDB = 6.0;   // a boost, which Biquad refuses

    juce::StringArray saveErrors;
    EXPECT_FALSE (PresetManager::saveToFile (bad, file, saveErrors));
    EXPECT_FALSE (saveErrors.isEmpty());
    EXPECT_FALSE (file.existsAsFile()) << "a refused save must not leave a file behind";
}

TEST (PresetManager, LoadingAFileThatDoesNotExistIsRefused)
{
    ScopedTempDir temp;

    const auto result = PresetManager::loadFromFile (temp.dir.getChildFile ("Nope.json"));

    EXPECT_FALSE (result.ok);
    EXPECT_FALSE (result.errors.isEmpty());
}

//==============================================================================
// Preset v2 -- optional "slot" on each notch and an optional "slots" section.
// A v1 file has neither key and must keep loading exactly as before.
//==============================================================================

TEST (PresetManager, AV1PresetWithoutSlotKeysLeavesEveryNotchOnSlotZeroWithNoSlots)
{
    const auto result = PresetManager::fromJSON (kValidPresetJson);

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    ASSERT_EQ (result.preset.notches.size(), 1u);
    EXPECT_EQ (result.preset.notches[0].slot, 0);
    EXPECT_TRUE (result.preset.slots.empty());
}

TEST (PresetManager, ASlotIdAndASlotsSectionParseFromAVersionTwoFile)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,)"
        R"("slots":[{"index":3,"enabled":true,"width":1,"inputChannels":[2,0],"outputChannels":[3,1]}],)"
        R"("notches":[{"index":0,"freq":482.0,"Q":30.0,"depth":-12.0,"slot":3}]})");

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    ASSERT_EQ (result.preset.notches.size(), 1u);
    EXPECT_EQ (result.preset.notches[0].slot, 3);

    ASSERT_EQ (result.preset.slots.size(), 1u);
    EXPECT_EQ (result.preset.slots[0].index, 3);
    EXPECT_TRUE (result.preset.slots[0].config.enabled);
    EXPECT_EQ (result.preset.slots[0].config.width, 1);
    EXPECT_EQ (result.preset.slots[0].config.inputChannels[0], 2);
    EXPECT_EQ (result.preset.slots[0].config.outputChannels[0], 3);
}

TEST (PresetManager, LoadingWithDeviceChannelsAutoActivatesTheReferencedSlot)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,)"
        R"("slots":[{"index":3,"enabled":false,"width":1,"inputChannels":[2,0],"outputChannels":[3,1]}],)"
        R"("notches":[{"index":0,"freq":482.0,"Q":30.0,"depth":-12.0,"slot":3}]})",
        4, 4);

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");

    const PresetSlot* slot3 = nullptr;
    for (const auto& s : result.preset.slots)
        if (s.index == 3) slot3 = &s;

    ASSERT_NE (slot3, nullptr) << "a referenced slot must exist after load";
    EXPECT_TRUE (slot3->config.enabled) << "a referenced slot auto-activates";
    EXPECT_EQ (slot3->config.width, 1);
    EXPECT_EQ (slot3->config.inputChannels[0], 2);
    EXPECT_EQ (slot3->config.outputChannels[0], 3);
}

TEST (PresetManager, ANotchWhoseSlotIsOutsideTheSlotRangeIsSkippedAndCounted)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,"notches":[)"
        R"({"index":0,"freq":482.0,"Q":30.0,"depth":-12.0,"slot":0},)"
        R"({"index":1,"freq":900.0,"Q":30.0,"depth":-12.0,"slot":9}]})",
        4, 4);

    EXPECT_TRUE (result.ok) << "an out-of-range slot skips one notch, not the file";
    EXPECT_EQ (result.skippedNotchCount, 1);
    ASSERT_EQ (result.preset.notches.size(), 1u);
    EXPECT_EQ (result.preset.notches[0].slot, 0);
}

TEST (PresetManager, ANotchWithANegativeSlotIsSkippedAndCounted)
{
    const auto result = PresetManager::fromJSON (
        R"({"version":"1.0","device":"d","sampleRate":48000,"bufferSize":64,"notches":[)"
        R"({"index":0,"freq":482.0,"Q":30.0,"depth":-12.0,"slot":-1}]})",
        4, 4);

    EXPECT_TRUE (result.ok);
    EXPECT_EQ (result.skippedNotchCount, 1);
    EXPECT_TRUE (result.preset.notches.empty());
}

TEST (PresetManager, TheChannelAwareOverloadKeepsALegacyPresetStereo)
{
    const auto result = PresetManager::fromJSON (kValidPresetJson, 4, 4);

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_EQ (result.skippedNotchCount, 0);

    const PresetSlot* slot0 = nullptr;
    for (const auto& s : result.preset.slots)
        if (s.index == 0) slot0 = &s;

    ASSERT_NE (slot0, nullptr);
    EXPECT_TRUE (slot0->config.enabled);
    EXPECT_EQ (slot0->config.width, 2);
    EXPECT_EQ (slot0->config.inputChannels[0], 0);
    EXPECT_EQ (slot0->config.inputChannels[1], 1);
    EXPECT_EQ (slot0->config.outputChannels[0], 0);
    EXPECT_EQ (slot0->config.outputChannels[1], 1);
}

TEST (PresetManager, SlotIdsAndSlotConfigsSurviveARoundTrip)
{
    Preset p = makeValidPreset();
    p.notches[1].slot = 3;
    p.slots.push_back (PresetSlot { 3, [] {
        SlotConfig c;
        c.enabled = true;
        c.width   = 1;
        c.inputChannels[0]  = 2;
        c.outputChannels[0] = 3;
        return c;
    }() });

    const auto result = PresetManager::fromJSON (PresetManager::toJSON (p));

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    ASSERT_EQ (result.preset.notches.size(), 2u);
    EXPECT_EQ (result.preset.notches[1].slot, 3);

    ASSERT_EQ (result.preset.slots.size(), 1u);
    EXPECT_EQ (result.preset.slots[0].index, 3);
    EXPECT_TRUE (result.preset.slots[0].config.enabled);
    EXPECT_EQ (result.preset.slots[0].config.width, 1);
    EXPECT_EQ (result.preset.slots[0].config.inputChannels[0], 2);
    EXPECT_EQ (result.preset.slots[0].config.outputChannels[0], 3);
}

// The v1 writer emitted neither key; a preset carrying nothing new must
// produce byte-recognisable v1 output, not a file full of "slot": 0 noise.
TEST (PresetManager, TheWriterOmitsSlotKeysWhenTheyCarryNothing)
{
    const juce::String text = PresetManager::toJSON (makeValidPreset());

    EXPECT_FALSE (text.contains ("\"slot\"")) << text;
    EXPECT_FALSE (text.contains ("\"slots\"")) << text;
}

TEST (PresetManager, TheWriterEmitsSlotAndSlotsWhenTheyCarrySomething)
{
    Preset p = makeValidPreset();
    p.notches[1].slot = 3;
    SlotConfig c;
    c.enabled = true;
    p.slots.push_back (PresetSlot { 3, c });

    const juce::String text = PresetManager::toJSON (p);

    EXPECT_TRUE (text.contains ("\"slot\": 3") || text.contains ("\"slot\":3")) << text;
    EXPECT_TRUE (text.contains ("\"slots\"")) << text;
}

//==============================================================================
// planFor -- owner decision D-00, and the hand-off to the wiring half
//==============================================================================

// D-00: a notch above the CURRENT Nyquist is deactivated, never clamped.
// Clamping is what produced the measured 4.11e18 peak recorded in Biquad.h.
// Fails if planFor ever rewrites a frequency to make it fit.
TEST (PresetManager, ANotchAboveTheTargetNyquistIsPlannedIdleWithItsParametersIntact)
{
    Preset p     = makeValidPreset();
    p.sampleRate = 96000.0;
    p.notches    = { PresetNotch { 0, 30000.0, 25.0, -10.0 } };

    const auto plan = PresetManager::planFor (p, 44100.0);

    ASSERT_EQ (plan.size(), 1u);
    EXPECT_EQ (plan[0].applicability, NotchApplicability::AboveNyquist);
    EXPECT_DOUBLE_EQ (plan[0].notch.freq, 30000.0) << "D-00 forbids clamping";
    EXPECT_DOUBLE_EQ (plan[0].notch.Q, 25.0);
    EXPECT_DOUBLE_EQ (plan[0].notch.depthDB, -10.0);
    EXPECT_EQ (plan[0].notch.index, 0);
}

TEST (PresetManager, ANotchBelowTheTargetNyquistIsPlannedApplicable)
{
    Preset p  = makeValidPreset();
    p.notches = { PresetNotch { 0, 482.0, 30.0, -12.0 } };

    const auto plan = PresetManager::planFor (p, 44100.0);

    ASSERT_EQ (plan.size(), 1u);
    EXPECT_EQ (plan[0].applicability, NotchApplicability::Applicable);
    EXPECT_DOUBLE_EQ (plan[0].notch.freq, 482.0);
}

// No open device means nothing is installable. Fails if a zero rate divides
// its way into a comparison that happens to come out true.
TEST (PresetManager, PlanningAgainstNoOpenDeviceMakesEveryNotchUnusable)
{
    const Preset p = makeValidPreset();

    const auto plan = PresetManager::planFor (p, 0.0);

    ASSERT_EQ (plan.size(), p.notches.size());

    for (const auto& planned : plan)
    {
        EXPECT_EQ (planned.applicability, NotchApplicability::AboveNyquist);
    }
}

// The plan is per-notch and ordered so a caller can walk it beside the stored
// list. Fails if planFor ever filters the unusable ones out, which would
// silently discard exactly the notches D-00 says to keep.
TEST (PresetManager, PlanningKeepsEveryStoredNotchInOrder)
{
    Preset p     = makeValidPreset();
    p.sampleRate = 96000.0;
    p.notches    = { PresetNotch { 0, 482.0,   30.0, -12.0 },
                     PresetNotch { 1, 30000.0, 25.0, -10.0 },
                     PresetNotch { 2, 1200.0,  20.0,  -8.0 } };

    const auto plan = PresetManager::planFor (p, 44100.0);

    ASSERT_EQ (plan.size(), 3u);
    EXPECT_EQ (plan[0].applicability, NotchApplicability::Applicable);
    EXPECT_EQ (plan[1].applicability, NotchApplicability::AboveNyquist);
    EXPECT_EQ (plan[2].applicability, NotchApplicability::Applicable);
    EXPECT_EQ (plan[0].notch.index, 0);
    EXPECT_EQ (plan[1].notch.index, 1);
    EXPECT_EQ (plan[2].notch.index, 2);
}

//==============================================================================
// Task 26 -- the two shipped defaults, read through this loader
//==============================================================================
//
// "Written by hand and eyeballed" is not tested. These read the real shipped
// files off disk through the real loader, so a typo in either one is a red
// test here rather than a dialog on a soundman's laptop.

TEST (PresetManager, TheSpeechDefaultPresetLoadsAndCarriesItsCharacter)
{
    const juce::File file =
        juce::File (HANDSFREE_PRESET_SOURCE_DIR).getChildFile ("Speech.json");
    ASSERT_TRUE (file.existsAsFile()) << file.getFullPathName();

    const auto result = PresetManager::loadFromFile (file);

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.Q, 40.0);
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.depthDB, -18.0);
}

TEST (PresetManager, TheMusicDefaultPresetLoadsAndCarriesItsCharacter)
{
    const juce::File file =
        juce::File (HANDSFREE_PRESET_SOURCE_DIR).getChildFile ("Music.json");
    ASSERT_TRUE (file.existsAsFile()) << file.getFullPathName();

    const auto result = PresetManager::loadFromFile (file);

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.Q, 25.0);
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.depthDB, -10.0);
}

// A default that shipped with notches nobody measured would cut real dB at a
// guessed frequency on every PA it is loaded onto.
TEST (PresetManager, TheShippedDefaultsLockNoNotches)
{
    for (const char* name : { "Speech.json", "Music.json" })
    {
        const auto result = PresetManager::loadFromFile (
            juce::File (HANDSFREE_PRESET_SOURCE_DIR).getChildFile (name));

        ASSERT_TRUE (result.ok) << name << ": " << result.errors.joinIntoString ("; ");
        EXPECT_TRUE (result.preset.notches.empty())
            << name << " ships with notches at frequencies nobody measured";
    }
}
