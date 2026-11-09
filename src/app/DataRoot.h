#pragma once

#include <juce_core/juce_core.h>

// Where per-user data lives. Real mode: %APPDATA%/AZSoundtech/HandsFree and
// the user's Documents folder. Test mode: when the environment variable
// DEVCAVE_TEST_PROFILE names an absolute directory, everything moves under it
// (<profile>/AppData/AZSoundtech/HandsFree, <profile>/Documents) so a UI test
// can never touch the owner's real settings, presets, logs or license.
//
// JUCE's getSpecialLocation uses the Windows shell API, so overriding the
// APPDATA environment variable does not redirect it -- the app has to honour
// the override itself. A relative or blank value is an ERROR, never a silent
// fallback to real data.
namespace DataRoot
{
struct Resolved
{
    bool ok = true;
    bool testProfile = false;
    juce::File profileDir;     // empty unless testProfile
    juce::File roamingRoot;    // .../AZSoundtech/HandsFree (empty when !ok)
    juce::File documentsDir;   // user Documents, or <profile>/Documents (empty when !ok)
    juce::String error;        // set when !ok
};

constexpr const char* kProfileEnvVar = "DEVCAVE_TEST_PROFILE";

// Pure, testable: `profileValue` is the raw env value (empty string = unset).
Resolved resolveFor (const juce::String& profileValue,
                     const juce::File& realAppData,
                     const juce::File& realDocuments);

// Reads the environment and the real JUCE locations. Not cached.
Resolved current();

// Convenience accessors over current(). Empty File when the profile is invalid
// (the app refuses to start in that case, see main.cpp).
juce::File roamingRoot();
juce::File documentsDir();
bool isTestProfile();
}
