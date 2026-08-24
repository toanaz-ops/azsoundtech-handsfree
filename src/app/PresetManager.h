// PresetManager: the preset FILE FORMAT -- plan Task 25, format half.
//
// What this class deliberately does not know
// ==========================================
// Nothing here references AudioEngine, NotchChain, Biquad or the detector.
// A caller hands it a Preset and asks for text; a caller hands it text and
// asks for a Preset. That is the whole job.
//
// The restraint is not stylistic. Owner decision D-05 rules that the detector
// owns the notch model, so a loaded preset cannot be pushed into the audio
// chain directly -- it has to enter through the detector, which does not exist
// yet. A PresetManager that reached for AudioEngine would therefore be
// unbuildable today AND wrong tomorrow. Keeping it a pure translation is what
// lets the format ship now, and lets it be tested with no audio device
// present.
//
// The hand-off to the wiring half is planFor(): DATA describing what should
// happen to each notch at a given sample rate, never an action. See D-00
// below.
//
// Sign convention
// ===============
// `depthDB` is a NEGATIVE number of decibels: -12.0 means 12 dB down. This
// matches NotchChain::setNotch, Biquad::setNotchFilter's four-argument form,
// the JSON sample in plan Task 25, and the GUI in spec 6.1. A POSITIVE depth
// is refused at load time rather than passed down, because the peaking form
// Biquad uses is symmetric: a positive gain would BOOST the frequency that is
// already ringing. Biquad refuses it too; refusing here as well turns a
// silently-dropped notch into a message the user can act on.
//
// Recorded decisions
// ==================
// The plan specifies the format but not its edge cases, and a preset file
// outlives the version that wrote it. These were decided in this lane:
//
// [V] Version mismatch -> REFUSE, with the unrecognised version quoted.
//     A strict allow-list (currently just "1.0"), not a >= comparison. When a
//     1.1 exists, it is added to the allow-list together with whatever
//     migration it needs. Refusing costs a soundman one dialog; guessing at a
//     format we do not know costs a filter we cannot predict.
//     A version failure returns IMMEDIATELY without validating the remaining
//     fields, because their meaning is exactly what is in doubt.
//
// [N] Nyquist. Validation compares freq against the sample rate THE FILE
//     STORES, not the rate of the device currently open. A file whose notch
//     sits above its own Nyquist could never have been created by this
//     program and is corrupt -> refuse. A file that is internally consistent
//     but whose notch is above the CURRENT device's Nyquist is a normal
//     situation (96 kHz session reopened at 44.1 kHz) and is not a file
//     error: planFor() reports that notch as AboveNyquist so the caller
//     leaves it Idle with its stored parameters intact. That is owner
//     decision D-00 -- deactivate, never clamp. Clamping is what produced a
//     measured output peak of 4.11e18; see the derivation in Biquad.h.
//
// [S] Chain shape. More than MAX_NOTCHES entries, or two entries claiming one
//     index, are REFUSED. Slots are index-addressed, so letting the last
//     duplicate win would mean the preset the user hears is not the preset in
//     the file.
//
// [D] Device name. METADATA, never a gate. A preset saved on another
//     interface loads normally and keeps the stored name so the caller can
//     warn about it. Opening last week's preset on a different box is a
//     normal Tuesday, not an error.

#pragma once

#include "app/SlotConfig.h"

#include <juce_core/juce_core.h>

#include <vector>

//==============================================================================
/** One notch as it appears in a preset file.

    A plain aggregate on purpose: this is the type the future NotchController
    receives, and it must not drag a JUCE class or an engine reference along
    with it.
*/
struct PresetNotch
{
    int    index   = 0;    ///< Chain slot, [0, PresetManager::MAX_NOTCHES - 1].
    double freq    = 0.0;  ///< Hz. Strictly between 0 and the file's Nyquist.
    double Q       = 0.0;  ///< Quality factor. Strictly positive.
    double depthDB = 0.0;  ///< Attenuation in dB, NEGATIVE or zero.

    /** Routing slot, [0, kMaxSlots - 1]. OPTIONAL in the file: a preset
        written before multi-slot routing has no "slot" key and loads onto
        slot 0, which is the whole of backward compatibility. A value outside
        that range is not an error -- the channel-aware load skips that notch
        and counts it in PresetLoadResult::skippedNotchCount so the caller can
        warn, instead of refusing a file whose other notches are fine.
    */
    int slot = 0;
};

//==============================================================================
/** One entry of the OPTIONAL "slots" section: the routing configuration of a
    single processing slot.

    Absent slots are stereo {0,1} -> {0,1} and disabled -- SlotConfig's own
    defaults -- which is exactly what every v1 preset implied without saying.
*/
struct PresetSlot
{
    int        index = 0;   ///< [0, kMaxSlots - 1].
    SlotConfig config;
};

//==============================================================================
/** The Q and depth a preset wants NEW notches to be given.

    Why this exists, since the plan does not mention it
    ==================================================
    Plan Task 26 asks for two shipped presets described purely by character:
    Speech is Q=40 / -18 dB (narrow, aggressive) and Music is Q=25 / -10 dB
    (wider, gentler). Spec 7 defines a preset as "danh sach notch da lock" --
    the notches ALREADY LOCKED -- plus the device and buffer size. A preset
    that ships in the installer has locked nothing, so under the spec's
    definition alone those four numbers have nowhere to live, and Speech.json
    and Music.json would be byte-identical apart from their filenames.

    So the format carries an OPTIONAL notchDefaults block. Optional matters:
    the plan Task 25 sample has no such key and still parses, and a preset
    written before this block existed still loads. When it is absent these
    documented values apply -- they are the ones in the plan's own sample
    notch and in the spec 6.1 notch table (482 Hz, -12 dB, Q 30).

    This is the one place this lane extended the format rather than
    implementing it. It is additive and reversible, and the owner can overturn
    it without touching any other decision here.
*/
struct PresetNotchDefaults
{
    double Q       = 30.0;
    double depthDB = -12.0;
};

//==============================================================================
/** A whole preset: spec 7 defines it as the locked notches plus the audio
    device and buffer size.
*/
struct Preset
{
    juce::String version = "1.0";
    juce::String device;
    double       sampleRate = 0.0;  ///< Hz as a NUMBER. See PresetManager.cpp.
    int          bufferSize = 0;    ///< Samples.

    PresetNotchDefaults      notchDefaults;
    std::vector<PresetNotch> notches;

    /** OPTIONAL routing section. Empty for every v1 file. */
    std::vector<PresetSlot> slots;
};

//==============================================================================
/** What should happen to one stored notch at a given sample rate. */
enum class NotchApplicability
{
    Applicable,    ///< Below the target Nyquist: install it.
    AboveNyquist   ///< D-00: keep the parameters, leave the slot Idle.
};

/** A stored notch paired with the verdict for one target sample rate. */
struct PlannedNotch
{
    PresetNotch        notch;
    NotchApplicability applicability = NotchApplicability::Applicable;
};

//==============================================================================
/** The outcome of a load.

    `errors` carries ONE MESSAGE PER PROBLEM rather than stopping at the first,
    so a user who hand-edited a preset fixes it in one pass instead of one
    round trip per typo. When `ok` is false, `preset` is unspecified and must
    not be used.
*/
struct PresetLoadResult
{
    bool              ok = false;
    Preset            preset;
    juce::StringArray errors;

    /** How many notches were dropped because their "slot" was outside
        [0, kMaxSlots - 1]. Set by the channel-aware load only; a skip is a
        warning the caller surfaces, never a refused file.
    */
    int skippedNotchCount = 0;
};

//==============================================================================
class PresetManager
{
public:
    /** Chain slots addressable by a preset.

        Deliberately NOT taken from NotchChain::MAX_NOTCHES: including a dsp
        header here would couple the file format to the audio chain, which is
        the one thing this class is built to avoid. The two constants must
        still agree, so tests/test_presetmanager.cpp static_asserts them
        against each other -- a disagreement is a compile error, not a runtime
        surprise.
    */
    static constexpr int MAX_NOTCHES = 16;

    /** The only preset version this build understands. See decision [V]. */
    static constexpr const char* CURRENT_VERSION = "1.0";

    /** %APPDATA%/AZSoundtech/HandsFree/presets, per spec 7 and plan Task 25.
        Asked of JUCE rather than hard-coded, and NOT created as a side effect
        of being named -- saveToFile() creates it.
    */
    static juce::File getPresetDirectory();

    /** Serialises to JSON text. Does not touch the filesystem. */
    static juce::String toJSON (const Preset& preset);

    /** Parses and validates JSON text. Never throws; every problem, including
        "this is not JSON at all", arrives as a message in the result.
    */
    static PresetLoadResult fromJSON (const juce::String& text);

    /** Same parse, then a routing pass against the OPEN DEVICE's channel
        counts: notches whose "slot" is outside [0, kMaxSlots - 1] are skipped
        and counted (never fatal), every referenced slot is auto-activated
        with the config the file declared (or stereo when none did), and every
        slot's channels are clamped to what the device actually has.
    */
    static PresetLoadResult fromJSON (const juce::String& text,
                                      int numInputChannels,
                                      int numOutputChannels);

    /** Value rules only -- the caller has already produced a well-typed
        Preset. Shared by fromJSON() and saveToFile() so that the writer can
        never emit a file the reader would refuse. Empty means valid.
    */
    static juce::StringArray validate (const Preset& preset);

    /** Writes UTF-8 JSON, creating parent directories as needed.

        Validates FIRST and writes nothing if the preset is invalid: an app
        that can save a preset it cannot reopen is worse than one that refuses
        to save.
    */
    static bool saveToFile (const Preset&      preset,
                            const juce::File&  file,
                            juce::StringArray& errors);

    /** Reads and parses a preset file. A missing file is a reported failure,
        not an empty preset.
    */
    static PresetLoadResult loadFromFile (const juce::File& file);

    /** Channel-aware companion of loadFromFile(); see fromJSON(). */
    static PresetLoadResult loadFromFile (const juce::File& file,
                                          int numInputChannels,
                                          int numOutputChannels);

    //==========================================================================
    /** THE HAND-OFF TO THE WIRING HALF OF TASK 25.

        Returns one PlannedNotch per stored notch, in stored order, parameters
        untouched -- DATA describing what should happen, never an action. The
        wiring half cannot be an action from here anyway: owner decision D-05
        gives the detector sole ownership of the notch model, so a loaded
        preset enters through the detector and never through AudioEngine or
        NotchChain directly.

        The verdict implements owner decision D-00. A stored notch at or above
        the target Nyquist is reported AboveNyquist, with its frequency, Q and
        depth exactly as stored, so the caller registers it Idle and can
        reinstate it verbatim if the device rate goes back up. It is never
        clamped to fit and never dropped from the list -- clamping is what put
        the poles outside the unit circle and produced the measured 4.11e18
        output peak recorded in Biquad.h, and dropping would discard precisely
        the notches D-00 says to keep.

        A target rate of zero or less (no device open) makes every notch
        AboveNyquist, since nothing can be installed into a chain that is not
        running.
    */
    static std::vector<PlannedNotch> planFor (const Preset& preset,
                                              double        targetSampleRateHz);
};
