#include "app/PresetManager.h"

#include <cmath>
#include <set>

// Reading is two passes on purpose
// ================================
// fromJSON() first checks SHAPE (is this JSON, is it an object, are the keys
// present, is each one the right juce::var type) and only then checks VALUES
// (is the frequency below Nyquist, is the depth negative). The value pass is
// PresetManager::validate(), which operates on a fully-typed Preset and knows
// nothing about JSON.
//
// That split is what lets saveToFile() run exactly the same value rules the
// loader runs. A writer with its own private idea of validity is how an app
// ends up saving a preset it can never open again -- and here the reader is
// the last guard before a bad coefficient reaches a PA.
//
// The shape pass is also why a file with a type error does not go on to
// report value errors: an entry whose "freq" is the string "482.0" has no
// frequency to range-check, and inventing one produces a second, misleading
// message about a problem the user does not have.

namespace
{

//==============================================================================
// juce::var distinguishes bool from int, and a JSON `true` must NOT slip
// through as 1 -- a preset that says "freq": true is a corrupt file, not a
// 1 Hz notch. isBool() is therefore excluded explicitly rather than assumed.
bool isNumber (const juce::var& v)
{
    if (v.isBool() || v.isString() || v.isArray() || v.isObject() || v.isVoid())
    {
        return false;
    }

    return v.isInt() || v.isInt64() || v.isDouble();
}

juce::String describeType (const juce::var& v)
{
    if (v.isVoid())   return "nothing";
    if (v.isBool())   return "a boolean";
    if (v.isString()) return "a string";
    if (v.isArray())  return "an array";
    if (v.isObject()) return "an object";

    return "a number";
}

// Whole numbers are written as JSON integers (48000, 64) rather than as
// 48000.0. The plan's format block shows integer literals, and a preset file
// is meant to be readable and hand-editable -- a "48000.0" invites someone to
// tidy it back. Non-integral values still round-trip as doubles.
juce::var numberVar (double value)
{
    if (std::isfinite (value)
        && value == std::floor (value)
        && std::abs (value) < 2147483647.0)
    {
        return juce::var (static_cast<int> (value));
    }

    return juce::var (value);
}

//==============================================================================
// SHAPE pass helper: reads one numeric field, appending a located message on
// any failure. Leaves `out` untouched and returns false if the field is
// missing, the wrong type, or not finite.
bool readNumber (const juce::DynamicObject& object,
                 const juce::Identifier&    key,
                 const juce::String&        where,
                 juce::StringArray&         errors,
                 double&                    out)
{
    if (! object.hasProperty (key))
    {
        errors.add (where + "missing key \"" + key.toString() + "\"");
        return false;
    }

    const juce::var value = object.getProperty (key);

    if (! isNumber (value))
    {
        errors.add (where + "\"" + key.toString() + "\" must be a number, but is "
                    + describeType (value));
        return false;
    }

    const double asDouble = static_cast<double> (value);

    if (! std::isfinite (asDouble))
    {
        errors.add (where + "\"" + key.toString() + "\" is not a finite number");
        return false;
    }

    out = asDouble;
    return true;
}

bool readWholeNumber (const juce::DynamicObject& object,
                      const juce::Identifier&    key,
                      const juce::String&        where,
                      juce::StringArray&         errors,
                      int&                       out)
{
    double asDouble = 0.0;

    if (! readNumber (object, key, where, errors, asDouble))
    {
        return false;
    }

    // Truncating silently is how a bufferSize of 64.5 becomes a 64 that
    // nobody asked for and nobody can trace back to the file.
    if (asDouble != std::floor (asDouble))
    {
        errors.add (where + "\"" + key.toString() + "\" must be a whole number, but is "
                    + juce::String (asDouble));
        return false;
    }

    out = static_cast<int> (asDouble);
    return true;
}

//==============================================================================
// SHAPE pass: one entry of the "notches" array. Value rules are deliberately
// absent here -- validate() owns those.
bool readNotch (const juce::var&   entry,
                int                position,
                juce::StringArray& errors,
                PresetNotch&       out)
{
    const juce::String where = "notches[" + juce::String (position) + "]: ";

    auto* object = entry.getDynamicObject();

    if (object == nullptr)
    {
        errors.add (where + "expected an object, but found " + describeType (entry));
        return false;
    }

    // Every field is read even after one fails, so a notch with three typos
    // produces three messages rather than one per save-and-retry cycle.
    bool ok = readWholeNumber (*object, "index", where, errors, out.index);

    ok = readNumber (*object, "freq",  where, errors, out.freq)    && ok;
    ok = readNumber (*object, "Q",     where, errors, out.Q)       && ok;
    ok = readNumber (*object, "depth", where, errors, out.depthDB) && ok;

    return ok;
}

//==============================================================================
// VALUE pass for a single notch. `nyquistHz` is 0 when the preset's own
// sample rate was unusable: that has already been reported once, and
// repeating it as "above Nyquist" on every notch would bury the real cause.
void validateNotchValues (const PresetNotch& notch,
                          int                position,
                          double             nyquistHz,
                          juce::StringArray& errors)
{
    const juce::String where = "notches[" + juce::String (position) + "]: ";

    if (notch.index < 0 || notch.index >= PresetManager::MAX_NOTCHES)
    {
        errors.add (where + "\"index\" is " + juce::String (notch.index)
                    + ", outside the chain range [0, "
                    + juce::String (PresetManager::MAX_NOTCHES - 1) + "]");
    }

    // Both of the next two put the biquad's poles on or outside the unit
    // circle. Biquad.h carries the derivation and the measured divergence.
    if (notch.freq <= 0.0)
    {
        errors.add (where + "\"freq\" must be greater than 0 Hz, but is "
                    + juce::String (notch.freq));
    }
    else if (nyquistHz > 0.0 && notch.freq >= nyquistHz)
    {
        errors.add (where + "\"freq\" is " + juce::String (notch.freq)
                    + " Hz, at or above the Nyquist frequency of "
                    + juce::String (nyquistHz)
                    + " Hz implied by this preset's own sample rate");
    }

    // Q == 0 divides by zero in the alpha term and every coefficient becomes
    // NaN -- the nastiest of the four failures, because NaN compares false
    // against every downstream sanity check.
    if (notch.Q <= 0.0)
    {
        errors.add (where + "\"Q\" must be greater than 0, but is "
                    + juce::String (notch.Q));
    }

    // A positive depth BOOSTS the ringing frequency. Biquad refuses it;
    // refusing here too means the user is told, instead of the notch silently
    // doing nothing. Zero is allowed because the filter allows it (A == 1,
    // H == 1, a mathematical no-op).
    if (notch.depthDB > 0.0)
    {
        errors.add (where + "\"depth\" is " + juce::String (notch.depthDB)
                    + " dB. Depth is an attenuation and must be negative or zero"
                      " -- a positive value would boost the frequency being notched");
    }
}

} // namespace

//==============================================================================
juce::File PresetManager::getPresetDirectory()
{
    // userApplicationDataDirectory is %APPDATA% on Windows. Asked of JUCE so
    // the path stays right if the platform's convention does.
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("AZSoundtech")
               .getChildFile ("HandsFree")
               .getChildFile ("presets");
}

//==============================================================================
juce::String PresetManager::toJSON (const Preset& preset)
{
    auto* root = new juce::DynamicObject();

    root->setProperty ("version", preset.version);
    root->setProperty ("device",  preset.device);

    // NUMERIC, not the "48000 Hz" display string that
    // AudioEngine::getCurrentSampleRate() returns. Its numeric companion is
    // AudioEngine::getCurrentSampleRateHz().
    root->setProperty ("sampleRate", numberVar (preset.sampleRate));
    root->setProperty ("bufferSize", preset.bufferSize);

    auto* defaults = new juce::DynamicObject();
    defaults->setProperty ("Q",     preset.notchDefaults.Q);
    defaults->setProperty ("depth", preset.notchDefaults.depthDB);
    root->setProperty ("notchDefaults", juce::var (defaults));

    juce::Array<juce::var> notches;

    for (const auto& notch : preset.notches)
    {
        auto* entry = new juce::DynamicObject();

        entry->setProperty ("index", notch.index);
        entry->setProperty ("freq",  notch.freq);
        entry->setProperty ("Q",     notch.Q);
        entry->setProperty ("depth", notch.depthDB);

        notches.add (juce::var (entry));
    }

    root->setProperty ("notches", notches);

    return juce::JSON::toString (juce::var (root));
}

//==============================================================================
juce::StringArray PresetManager::validate (const Preset& preset)
{
    juce::StringArray errors;

    if (preset.version != CURRENT_VERSION)
    {
        errors.add ("unsupported preset version \"" + preset.version
                    + "\" -- this build understands \""
                    + juce::String (CURRENT_VERSION) + "\"");
    }

    double nyquistHz = 0.0;

    if (! std::isfinite (preset.sampleRate) || preset.sampleRate <= 0.0)
    {
        errors.add ("\"sampleRate\" must be greater than 0 Hz, but is "
                    + juce::String (preset.sampleRate));
    }
    else
    {
        nyquistHz = 0.5 * preset.sampleRate;
    }

    if (preset.bufferSize <= 0)
    {
        errors.add ("\"bufferSize\" must be greater than 0, but is "
                    + juce::String (preset.bufferSize));
    }

    // The defaults seed every notch the detector goes on to create, so a bad
    // default is worse than a bad notch, not better.
    if (! std::isfinite (preset.notchDefaults.Q) || preset.notchDefaults.Q <= 0.0)
    {
        errors.add ("\"notchDefaults\".\"Q\" must be greater than 0, but is "
                    + juce::String (preset.notchDefaults.Q));
    }

    if (! std::isfinite (preset.notchDefaults.depthDB) || preset.notchDefaults.depthDB > 0.0)
    {
        errors.add ("\"notchDefaults\".\"depth\" is " + juce::String (preset.notchDefaults.depthDB)
                    + " dB and must be negative or zero");
    }

    if (preset.notches.size() > static_cast<size_t> (MAX_NOTCHES))
    {
        errors.add ("the preset holds " + juce::String (static_cast<int> (preset.notches.size()))
                    + " notches, but a chain has only " + juce::String (MAX_NOTCHES) + " slots");
    }

    std::set<int> seenIndices;

    for (size_t i = 0; i < preset.notches.size(); ++i)
    {
        const int position = static_cast<int> (i);

        validateNotchValues (preset.notches[i], position, nyquistHz, errors);

        // Slots are index-addressed, so letting the later of two entries win
        // would mean the preset the user hears is not the preset in the file.
        if (! seenIndices.insert (preset.notches[i].index).second)
        {
            errors.add ("notches[" + juce::String (position) + "]: duplicate \"index\" "
                        + juce::String (preset.notches[i].index)
                        + " -- each chain slot may appear at most once");
        }
    }

    return errors;
}

//==============================================================================
PresetLoadResult PresetManager::fromJSON (const juce::String& text)
{
    PresetLoadResult result;

    juce::var          parsed;
    const juce::Result parseOutcome = juce::JSON::parse (text, parsed);

    if (! parseOutcome.wasOk())
    {
        // Covers the empty file and the truncated file, which is what most
        // real corruption looks like.
        result.errors.add ("not valid JSON: " + parseOutcome.getErrorMessage().trim());
        return result;
    }

    auto* root = parsed.getDynamicObject();

    if (root == nullptr)
    {
        result.errors.add ("not a preset: the top level is " + describeType (parsed)
                           + ", not a JSON object");
        return result;
    }

    // -- version, first and alone -------------------------------------------
    // Decision [V]: an unrecognised version returns immediately. Reading on
    // would mean interpreting fields whose meaning is exactly what is in
    // doubt.
    if (! root->hasProperty ("version"))
    {
        result.errors.add ("missing key \"version\"");
        return result;
    }

    const juce::var version = root->getProperty ("version");

    if (! version.isString())
    {
        result.errors.add ("\"version\" must be a string such as \""
                           + juce::String (CURRENT_VERSION) + "\", but is "
                           + describeType (version));
        return result;
    }

    if (version.toString() != CURRENT_VERSION)
    {
        result.errors.add ("unsupported preset version \"" + version.toString()
                           + "\" -- this build understands \""
                           + juce::String (CURRENT_VERSION) + "\"");
        return result;
    }

    Preset preset;
    preset.version = version.toString();

    // -- device: metadata, decision [D] -------------------------------------
    if (! root->hasProperty ("device"))
    {
        result.errors.add ("missing key \"device\"");
    }
    else if (! root->getProperty ("device").isString())
    {
        result.errors.add ("\"device\" must be a string, but is "
                           + describeType (root->getProperty ("device")));
    }
    else
    {
        // Kept verbatim even when no such interface is present, so the caller
        // can say WHICH one the preset came from.
        preset.device = root->getProperty ("device").toString();
    }

    // -- sampleRate ---------------------------------------------------------
    if (! root->hasProperty ("sampleRate"))
    {
        result.errors.add ("missing key \"sampleRate\"");
    }
    else if (! isNumber (root->getProperty ("sampleRate")))
    {
        result.errors.add ("\"sampleRate\" must be a number in Hz, but is "
                           + describeType (root->getProperty ("sampleRate"))
                           + " -- a formatted string such as \"48000 Hz\" is not a sample rate");
    }
    else
    {
        preset.sampleRate = static_cast<double> (root->getProperty ("sampleRate"));
    }

    // -- bufferSize ---------------------------------------------------------
    if (! root->hasProperty ("bufferSize"))
    {
        result.errors.add ("missing key \"bufferSize\"");
    }
    else
    {
        readWholeNumber (*root, "bufferSize", "", result.errors, preset.bufferSize);
    }

    // -- notchDefaults: OPTIONAL --------------------------------------------
    // Absent is normal: the plan Task 25 sample has no such key, and presets
    // written before this block existed must keep loading. The struct's own
    // documented defaults stand in.
    if (root->hasProperty ("notchDefaults"))
    {
        const juce::var defaults = root->getProperty ("notchDefaults");

        if (auto* defaultsObject = defaults.getDynamicObject())
        {
            readNumber (*defaultsObject, "Q",     "notchDefaults: ", result.errors,
                        preset.notchDefaults.Q);
            readNumber (*defaultsObject, "depth", "notchDefaults: ", result.errors,
                        preset.notchDefaults.depthDB);
        }
        else
        {
            result.errors.add ("\"notchDefaults\" must be an object, but is "
                               + describeType (defaults));
        }
    }

    // -- notches ------------------------------------------------------------
    if (! root->hasProperty ("notches"))
    {
        result.errors.add ("missing key \"notches\"");
    }
    else if (! root->getProperty ("notches").isArray())
    {
        result.errors.add ("\"notches\" must be an array, but is "
                           + describeType (root->getProperty ("notches")));
    }
    else
    {
        const juce::Array<juce::var>& entries = *root->getProperty ("notches").getArray();

        for (int i = 0; i < entries.size(); ++i)
        {
            PresetNotch notch;

            if (readNotch (entries.getReference (i), i, result.errors, notch))
            {
                preset.notches.push_back (notch);
            }
        }
    }

    // The VALUE pass runs only if the shape is sound, so that a notch whose
    // "freq" was a string does not also generate a bogus range complaint, and
    // so that the positions in the value messages still match the file.
    if (result.errors.isEmpty())
    {
        result.errors = validate (preset);
    }

    result.ok = result.errors.isEmpty();

    if (result.ok)
    {
        result.preset = preset;
    }
    else
    {
        // A partially populated Preset is the shape of bug where a caller
        // reads the message, shrugs, and uses the struct anyway.
        result.preset = Preset {};
    }

    return result;
}

//==============================================================================
bool PresetManager::saveToFile (const Preset&      preset,
                                const juce::File&  file,
                                juce::StringArray& errors)
{
    errors = validate (preset);

    if (! errors.isEmpty())
    {
        // Nothing is written. A half-saved or unreadable preset is worse than
        // a refused save, because it is discovered at the next soundcheck
        // rather than now.
        return false;
    }

    const juce::Result directory = file.getParentDirectory().createDirectory();

    if (directory.failed())
    {
        errors.add ("could not create " + file.getParentDirectory().getFullPathName()
                    + ": " + directory.getErrorMessage().trim());
        return false;
    }

    // UTF-8 without a BOM, explicitly: device names are whatever the driver
    // reports and are not guaranteed to be ASCII.
    if (! file.replaceWithText (toJSON (preset), false, false, "\n"))
    {
        errors.add ("could not write " + file.getFullPathName());
        return false;
    }

    return true;
}

//==============================================================================
PresetLoadResult PresetManager::loadFromFile (const juce::File& file)
{
    PresetLoadResult result;

    if (! file.existsAsFile())
    {
        result.errors.add ("no such preset file: " + file.getFullPathName());
        return result;
    }

    // loadFileAsString honours a BOM if one is present and assumes UTF-8
    // otherwise, which is what saveToFile writes.
    return fromJSON (file.loadFileAsString());
}

//==============================================================================
std::vector<PlannedNotch> PresetManager::planFor (const Preset& preset,
                                                  double        targetSampleRateHz)
{
    std::vector<PlannedNotch> plan;
    plan.reserve (preset.notches.size());

    const bool   rateIsUsable = std::isfinite (targetSampleRateHz) && targetSampleRateHz > 0.0;
    const double nyquistHz    = rateIsUsable ? 0.5 * targetSampleRateHz : 0.0;

    for (const auto& notch : preset.notches)
    {
        PlannedNotch planned;

        // Stored parameters are copied through untouched. D-00 is explicit
        // that an inapplicable notch keeps them so it can be reinstated
        // verbatim if the rate goes back up.
        planned.notch = notch;

        planned.applicability = (rateIsUsable && notch.freq < nyquistHz)
                                    ? NotchApplicability::Applicable
                                    : NotchApplicability::AboveNyquist;

        plan.push_back (planned);
    }

    return plan;
}
