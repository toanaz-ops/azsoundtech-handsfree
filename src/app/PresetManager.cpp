#include "app/PresetManager.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

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

    // OPTIONAL v2 key: absent means slot 0, which is every v1 file ever
    // written. Present but malformed is still a shape error the user fixes.
    if (object->hasProperty ("slot"))
    {
        ok = readWholeNumber (*object, "slot", where, errors, out.slot) && ok;
    }

    // OPTIONAL lane-S key. Absent means every lane. Present, it must be 0 or
    // 1 -- anything else is a self-contradicting file (S-9), refused in this
    // SHAPE pass so both fromJSON overloads agree.
    if (object->hasProperty ("lane"))
    {
        int lane = -1;
        if (! readWholeNumber (*object, "lane", where, errors, lane))
            ok = false;
        else if (lane != 0 && lane != 1)
        {
            errors.add (where + "\"lane\" is " + juce::String (lane) + ", expected 0 or 1");
            ok = false;
        }
        else
            out.lane = lane;
    }

    return ok;
}

//==============================================================================
// SHAPE pass for one entry of the OPTIONAL "slots" section (preset format v2).
// Like readNotch, value rules are deliberately absent here -- validate()
// and the channel-aware routing pass own those.
bool readBool (const juce::DynamicObject& object,
               const juce::Identifier&    key,
               const juce::String&        where,
               juce::StringArray&         errors,
               bool&                      out)
{
    if (! object.hasProperty (key))
    {
        errors.add (where + "missing key \"" + key.toString() + "\"");
        return false;
    }

    const juce::var value = object.getProperty (key);

    // A JSON true must not slip through an integer check as 1 -- same rule
    // as isNumber() above, from the other side.
    if (! value.isBool())
    {
        errors.add (where + "\"" + key.toString() + "\" must be a boolean, but is "
                    + describeType (value));
        return false;
    }

    out = static_cast<bool> (value);
    return true;
}

// Both lanes are required because SlotConfig always stores kMaxSlotLanes
// entries: accepting "[2]" for a mono slot would mean inventing the second
// lane's value, and an invented channel number is a wire we cannot trace.
bool readChannelArray (const juce::DynamicObject& object,
                       const juce::Identifier&    key,
                       const juce::String&        where,
                       juce::StringArray&         errors,
                       int*                       out)
{
    if (! object.hasProperty (key))
    {
        errors.add (where + "missing key \"" + key.toString() + "\"");
        return false;
    }

    const juce::var value = object.getProperty (key);

    if (! value.isArray())
    {
        errors.add (where + "\"" + key.toString() + "\" must be an array of "
                    + juce::String (kMaxSlotLanes) + " channel numbers, but is "
                    + describeType (value));
        return false;
    }

    const juce::Array<juce::var>& entries = *value.getArray();

    if (entries.size() != kMaxSlotLanes)
    {
        errors.add (where + "\"" + key.toString() + "\" must hold exactly "
                    + juce::String (kMaxSlotLanes) + " channel numbers, but holds "
                    + juce::String (entries.size()));
        return false;
    }

    bool ok = true;

    for (int lane = 0; lane < kMaxSlotLanes; ++lane)
    {
        const juce::var entry = entries.getReference (lane);

        if (! isNumber (entry))
        {
            errors.add (where + "\"" + key.toString() + "[" + juce::String (lane)
                        + "]\" must be a number, but is " + describeType (entry));
            ok = false;
            continue;
        }

        const double asDouble = static_cast<double> (entry);

        if (asDouble != std::floor (asDouble))
        {
            errors.add (where + "\"" + key.toString() + "[" + juce::String (lane)
                        + "]\" must be a whole number, but is " + juce::String (asDouble));
            ok = false;
            continue;
        }

        out[lane] = static_cast<int> (asDouble);
    }

    return ok;
}

bool readSlotEntry (const juce::var&   entry,
                    int                position,
                    juce::StringArray& errors,
                    PresetSlot&        out)
{
    const juce::String where = "slots[" + juce::String (position) + "]: ";

    auto* object = entry.getDynamicObject();

    if (object == nullptr)
    {
        errors.add (where + "expected an object, but found " + describeType (entry));
        return false;
    }

    bool ok = readWholeNumber (*object, "index", where, errors, out.index);
    ok = readBool (*object, "enabled", where, errors, out.config.enabled)      && ok;
    ok = readWholeNumber (*object, "width", where, errors, out.config.width)   && ok;
    ok = readChannelArray (*object, "inputChannels",  where, errors,
                           out.config.inputChannels)                          && ok;
    ok = readChannelArray (*object, "outputChannels", where, errors,
                           out.config.outputChannels)                         && ok;

    // OPTIONAL lane-S key. Absent means false, same as every preset written
    // before lane S implied by never mentioning linking.
    if (object->hasProperty ("linked"))
        ok = readBool (*object, "linked", where, errors, out.linked) && ok;

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

        // v1 output stays v1-shaped: a preset with nothing on but slot 0
        // must not grow keys a v1 reader would trip over.
        if (notch.slot != 0)
        {
            entry->setProperty ("slot", notch.slot);
        }

        // Same v1-stays-v1 rule as "slot": only present when it says something
        // an absent key would not already mean (-1, every lane).
        if (notch.lane != -1)
        {
            entry->setProperty ("lane", notch.lane);
        }

        notches.add (juce::var (entry));
    }

    root->setProperty ("notches", notches);

    // The slots section is emitted only when some slot differs from the
    // configuration every v1 file implied -- disabled stereo {0,1} -> {0,1}
    // -- so an untouched preset keeps producing a pure v1 file.
    const SlotConfig defaultConfig;

    auto slotIsNonDefault = [&defaultConfig] (const PresetSlot& s)
    {
        if (s.config.enabled != defaultConfig.enabled) return true;
        if (s.config.width   != defaultConfig.width)   return true;
        if (s.linked)                                  return true;

        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            if (s.config.inputChannels[lane]  != defaultConfig.inputChannels[lane]
                || s.config.outputChannels[lane] != defaultConfig.outputChannels[lane])
            {
                return true;
            }
        }

        return false;
    };

    const bool anyNonDefault =
        std::any_of (preset.slots.begin(), preset.slots.end(), slotIsNonDefault);

    if (anyNonDefault)
    {
        juce::Array<juce::var> slots;

        for (const auto& slot : preset.slots)
        {
            auto* entry = new juce::DynamicObject();

            entry->setProperty ("index",   slot.index);
            entry->setProperty ("enabled", juce::var (slot.config.enabled));
            entry->setProperty ("width",   slot.config.width);

            juce::Array<juce::var> inputs;
            juce::Array<juce::var> outputs;

            for (int lane = 0; lane < kMaxSlotLanes; ++lane)
            {
                inputs.add (slot.config.inputChannels[lane]);
                outputs.add (slot.config.outputChannels[lane]);
            }

            entry->setProperty ("inputChannels",  inputs);
            entry->setProperty ("outputChannels", outputs);

            // Same v1-stays-v1 rule as "slot" on a notch: omitted when false,
            // which is what an absent key already means.
            if (slot.linked)
            {
                entry->setProperty ("linked", juce::var (true));
            }

            slots.add (juce::var (entry));
        }

        root->setProperty ("slots", slots);
    }

    return juce::JSON::toString (juce::var (root));
}

//==============================================================================
juce::StringArray PresetManager::validate (const Preset& preset)
{
    juce::StringArray errors;

    if (preset.version != PresetManager::CURRENT_VERSION)
    {
        errors.add ("unsupported preset version \"" + preset.version
                    + "\" -- this build understands \""
                    + juce::String (PresetManager::CURRENT_VERSION) + "\"");
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

    // Chain shape, PER (slot, lane) -- decision [S], widened for lane S.
    //
    // Before lane S both lanes of a routing slot always carried identical
    // notches, so "index" alone addressed a chain slot and counting the whole
    // notch list against MAX_NOTCHES was the same measurement. It is not any
    // more: with INDEP detection, lane 0 and lane 1 of one routing slot each
    // own a FULL 16-notch chain, and a notch at index 2 on lane 0 is a
    // different filter from a notch at index 2 on lane 1. Both rules therefore
    // key on (slot, lane, index):
    //
    //   * two entries sharing (slot, lane, index) -> refuse, for exactly the
    //     old reason: the chain is index-addressed, so "last one wins" means
    //     the preset the user hears is not the preset in the file;
    //   * lane -1 means EVERY lane of that slot, so it collides with any other
    //     entry at the same (slot, index) -- including another -1;
    //   * the count is per chain, i.e. per (slot, lane), and a lane -1 entry
    //     consumes its index on BOTH lanes so it counts against both.
    std::map<std::pair<int, int>, std::set<int>> lanesPerSlotIndex;   // (slot,index) -> lanes
    std::map<std::pair<int, int>, int>           notchesPerSlotLane;  // (slot,lane) -> count

    for (size_t i = 0; i < preset.notches.size(); ++i)
    {
        const int position = static_cast<int> (i);
        const auto& notch  = preset.notches[i];

        validateNotchValues (notch, position, nyquistHz, errors);

        auto& lanes = lanesPerSlotIndex[{ notch.slot, notch.index }];

        const bool clash = lanes.count (notch.lane) > 0        // same lane twice
                        || lanes.count (-1) > 0                // an every-lane entry is already here
                        || (notch.lane == -1 && ! lanes.empty());   // this one covers every lane

        if (clash)
        {
            errors.add ("notches[" + juce::String (position) + "]: duplicate \"index\" "
                        + juce::String (notch.index)
                        + " on slot " + juce::String (notch.slot)
                        + (notch.lane < 0 ? juce::String (" (every lane)")
                                          : " lane " + juce::String (notch.lane))
                        + " -- each chain slot may appear at most once per lane");
        }

        lanes.insert (notch.lane);

        if (notch.lane < 0)
        {
            for (int lane = 0; lane < kMaxSlotLanes; ++lane)
                ++notchesPerSlotLane[{ notch.slot, lane }];
        }
        else
        {
            ++notchesPerSlotLane[{ notch.slot, notch.lane }];
        }
    }

    for (const auto& entry : notchesPerSlotLane)
    {
        if (entry.second <= MAX_NOTCHES)
            continue;

        errors.add ("the preset holds " + juce::String (entry.second)
                    + " notches on slot " + juce::String (entry.first.first)
                    + " lane " + juce::String (entry.first.second)
                    + ", but a chain has only " + juce::String (MAX_NOTCHES) + " slots");
    }

    return errors;
}

//==============================================================================
namespace
{

// The PARSE half shared by both fromJSON() overloads. Reads shape only --
// version gate, key types, arrays -- and leaves every value rule to
// validate() exactly as before. Returns false on the early bails (not JSON,
// not an object, unknown version) where reading on would interpret fields
// whose meaning is exactly what is in doubt.
bool parsePresetText (const juce::String& text,
                      Preset&             preset,
                      juce::StringArray&  errors)
{
    juce::var          parsed;
    const juce::Result parseOutcome = juce::JSON::parse (text, parsed);

    if (! parseOutcome.wasOk())
    {
        // Covers the empty file and the truncated file, which is what most
        // real corruption looks like.
        errors.add ("not valid JSON: " + parseOutcome.getErrorMessage().trim());
        return false;
    }

    auto* root = parsed.getDynamicObject();

    if (root == nullptr)
    {
        errors.add ("not a preset: the top level is " + describeType (parsed)
                    + ", not a JSON object");
        return false;
    }

    // -- version, first and alone -------------------------------------------
    // Decision [V]: an unrecognised version returns immediately. Reading on
    // would mean interpreting fields whose meaning is exactly what is in
    // doubt.
    if (! root->hasProperty ("version"))
    {
        errors.add ("missing key \"version\"");
        return false;
    }

    const juce::var version = root->getProperty ("version");

    if (! version.isString())
    {
        errors.add ("\"version\" must be a string such as \""
                    + juce::String (PresetManager::CURRENT_VERSION) + "\", but is "
                    + describeType (version));
        return false;
    }

    if (version.toString() != PresetManager::CURRENT_VERSION)
    {
        errors.add ("unsupported preset version \"" + version.toString()
                    + "\" -- this build understands \""
                    + juce::String (PresetManager::CURRENT_VERSION) + "\"");
        return false;
    }

    preset.version = version.toString();

    // -- device: metadata, decision [D] -------------------------------------
    if (! root->hasProperty ("device"))
    {
        errors.add ("missing key \"device\"");
    }
    else if (! root->getProperty ("device").isString())
    {
        errors.add ("\"device\" must be a string, but is "
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
        errors.add ("missing key \"sampleRate\"");
    }
    else if (! isNumber (root->getProperty ("sampleRate")))
    {
        errors.add ("\"sampleRate\" must be a number in Hz, but is "
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
        errors.add ("missing key \"bufferSize\"");
    }
    else
    {
        readWholeNumber (*root, "bufferSize", "", errors, preset.bufferSize);
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
            readNumber (*defaultsObject, "Q",     "notchDefaults: ", errors,
                        preset.notchDefaults.Q);
            readNumber (*defaultsObject, "depth", "notchDefaults: ", errors,
                        preset.notchDefaults.depthDB);
        }
        else
        {
            errors.add ("\"notchDefaults\" must be an object, but is "
                        + describeType (defaults));
        }
    }

    // -- slots: OPTIONAL (preset format v2) ---------------------------------
    // Absent is every v1 file ever written. Present but malformed is a shape
    // error like any other; the routing pass only runs when the shape held.
    if (root->hasProperty ("slots"))
    {
        if (! root->getProperty ("slots").isArray())
        {
            errors.add ("\"slots\" must be an array, but is "
                        + describeType (root->getProperty ("slots")));
        }
        else
        {
            const juce::Array<juce::var>& entries =
                *root->getProperty ("slots").getArray();

            for (int i = 0; i < entries.size(); ++i)
            {
                PresetSlot slot;

                if (readSlotEntry (entries.getReference (i), i, errors, slot))
                {
                    preset.slots.push_back (slot);
                }
            }
        }
    }

    // -- notches ------------------------------------------------------------
    if (! root->hasProperty ("notches"))
    {
        errors.add ("missing key \"notches\"");
    }
    else if (! root->getProperty ("notches").isArray())
    {
        errors.add ("\"notches\" must be an array, but is "
                    + describeType (root->getProperty ("notches")));
    }
    else
    {
        const juce::Array<juce::var>& entries = *root->getProperty ("notches").getArray();

        for (int i = 0; i < entries.size(); ++i)
        {
            PresetNotch notch;

            if (readNotch (entries.getReference (i), i, errors, notch))
            {
                preset.notches.push_back (notch);
            }
        }
    }

    return errors.isEmpty();
}

//==============================================================================
// The ROUTING pass of the channel-aware overload. Three jobs, in order:
//
// 1. A notch whose "slot" is outside [0, kMaxSlots - 1] is DROPPED and
//    counted, never fatal: one hand-edited typo must not take down the other
//    fifteen measured notches in the file.
// 2. Every referenced slot is auto-activated. The config the file declared
//    wins; a slot no "slots" entry mentions gets the stereo default -- which
//    is precisely what a v1 file implied without saying.
// 3. Every slot's channels are clamped to what the device actually has.
//    Clamping ROUTING here is safe and wanted -- it maps a channel number to
//    the nearest existing one. Clamping FILTER COEFFICIENTS is what produced
//    the 4.11e18 peak in Biquad.h; that remains forbidden everywhere else.
void routeNotchesToSlots (Preset& preset,
                          int     numInputChannels,
                          int     numOutputChannels,
                          int&    skippedCount)
{
    std::vector<PresetNotch> kept;
    kept.reserve (preset.notches.size());

    for (const auto& notch : preset.notches)
    {
        if (notch.slot >= 0 && notch.slot < kMaxSlots)
        {
            kept.push_back (notch);
        }
        else
        {
            ++skippedCount;
        }
    }

    preset.notches = std::move (kept);

    // Duplicate declarations of one index would leave two configs for one
    // slot; the first wins, matching how a reader meets them in file order.
    std::set<int> seenSlotIndices;
    std::vector<PresetSlot> uniqueSlots;
    uniqueSlots.reserve (preset.slots.size());

    for (auto& slot : preset.slots)
    {
        if (seenSlotIndices.insert (slot.index).second)
        {
            uniqueSlots.push_back (slot);
        }
    }

    preset.slots = std::move (uniqueSlots);

    std::set<int> referenced;

    for (const auto& notch : preset.notches)
    {
        referenced.insert (notch.slot);
    }

    for (int index : referenced)
    {
        bool declared = false;

        for (const auto& slot : preset.slots)
        {
            if (slot.index == index)
            {
                declared = true;
                break;
            }
        }

        if (! declared)
        {
            preset.slots.push_back (PresetSlot { index, SlotConfig {} });
        }
    }

    for (auto& slot : preset.slots)
    {
        if (referenced.count (slot.index) != 0)
        {
            slot.config.enabled = true;
        }

        // Width outside {1,2} cannot survive the load: the engine treats an
        // invalid width as a DISABLED slot while NotchController clamps 0 up
        // to 1, so leaving the raw value would let the engine and the
        // detector DISAGREE about the very slot this entry describes. Clamp,
        // never reject, so a hand-edited "width": 0 stays a usable mono slot.
        slot.config.width = std::clamp (slot.config.width, 1, 2);

        slot.config = slotClampedTo (slot.config, numInputChannels, numOutputChannels);
    }
}

PresetLoadResult finishLoad (Preset& preset, const juce::StringArray& errors)
{
    PresetLoadResult result;

    // The VALUE pass runs only if the shape is sound, so that a notch whose
    // "freq" was a string does not also generate a bogus range complaint, and
    // so that the positions in the value messages still match the file.
    result.errors = errors.isEmpty() ? PresetManager::validate (preset) : errors;

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

} // namespace

//==============================================================================
PresetLoadResult PresetManager::fromJSON (const juce::String& text)
{
    Preset           preset;
    juce::StringArray errors;

    parsePresetText (text, preset, errors);

    return finishLoad (preset, errors);
}

//==============================================================================
PresetLoadResult PresetManager::fromJSON (const juce::String& text,
                                          int                 numInputChannels,
                                          int                 numOutputChannels)
{
    Preset           preset;
    juce::StringArray errors;
    int              skipped = 0;

    if (parsePresetText (text, preset, errors))
    {
        routeNotchesToSlots (preset, numInputChannels, numOutputChannels, skipped);
    }

    PresetLoadResult result = finishLoad (preset, errors);
    result.skippedNotchCount = skipped;

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
PresetLoadResult PresetManager::loadFromFile (const juce::File& file,
                                              int               numInputChannels,
                                              int               numOutputChannels)
{
    PresetLoadResult result;

    if (! file.existsAsFile())
    {
        result.errors.add ("no such preset file: " + file.getFullPathName());
        return result;
    }

    return fromJSON (file.loadFileAsString(),
                     numInputChannels,
                     numOutputChannels);
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
