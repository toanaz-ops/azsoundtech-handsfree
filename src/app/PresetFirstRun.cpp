#include "app/PresetFirstRun.h"

namespace presetfirstrun
{

juce::StringArray defaultPresetFileNames()
{
    return { "Speech.json", "Music.json" };
}

SeedResult seedDefaultPresets (const juce::File& sourceDir,
                               const juce::File& targetDir)
{
    SeedResult result;

    // Create the target hierarchy up front; a failure here is one error for
    // the whole run, not one per file.
    if (! targetDir.isDirectory() && ! targetDir.createDirectory().wasOk())
    {
        result.errors.add ("Could not create preset directory: "
                           + targetDir.getFullPathName());
        return result;
    }

    for (const auto& name : defaultPresetFileNames())
    {
        const auto destination = targetDir.getChildFile (name);

        // The load-bearing guard: existing file wins, always. See the header.
        if (destination.existsAsFile())
        {
            ++result.skipped;
            continue;
        }

        const auto source = sourceDir.getChildFile (name);
        if (! source.existsAsFile())
        {
            result.errors.add ("Shipped preset missing from the install: " + name);
            continue;
        }

        if (! source.copyFileTo (destination))
        {
            result.errors.add ("Could not copy preset into place: " + name);
            continue;
        }

        ++result.copied;
    }

    return result;
}

} // namespace presetfirstrun
