#include "DataRoot.h"

namespace DataRoot
{
Resolved resolveFor (const juce::String& profileValue,
                     const juce::File& realAppData,
                     const juce::File& realDocuments)
{
    Resolved r;

    if (profileValue.isEmpty())
    {
        r.roamingRoot = realAppData.getChildFile ("AZSoundtech").getChildFile ("HandsFree");
        r.documentsDir = realDocuments;
        return r;
    }

    // Set but unusable: refuse, do not fall back to real data.
    if (profileValue.trim().isEmpty() || ! juce::File::isAbsolutePath (profileValue))
    {
        r.ok = false;
        r.error = juce::String (kProfileEnvVar)
                  + " must be an absolute directory path; refusing to start (value: '"
                  + profileValue + "').";
        return r;
    }

    r.testProfile = true;
    r.profileDir = juce::File (profileValue);
    r.roamingRoot = r.profileDir.getChildFile ("AppData")
                        .getChildFile ("AZSoundtech")
                        .getChildFile ("HandsFree");
    r.documentsDir = r.profileDir.getChildFile ("Documents");
    return r;
}

Resolved current()
{
    return resolveFor (juce::SystemStats::getEnvironmentVariable (kProfileEnvVar, {}),
                       juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory),
                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
}

juce::File roamingRoot()   { return current().roamingRoot; }
juce::File documentsDir()  { return current().documentsDir; }
bool isTestProfile()       { return current().testProfile; }
}
