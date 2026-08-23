// PresetFirstRun: seeds the shipped default presets into the user's preset
// directory (plan Task 26's "load on first run" half).
//
// Why this exists, verified 2026-08-23
// ===================================
// The presets lane report claimed "the installer copies them into %APPDATA%".
// It does not -- installer/handsfree.nsi mentions presets only in the
// uninstaller's delete prompt. This helper is therefore the ONLY path by which
// Speech.json and Music.json ever reach a user machine that did not run from
// the repo.
//
// The one rule that matters: NEVER overwrite. A preset file in %APPDATA% is
// user data; an upgrade or a second launch silently reverting it to the
// shipped text is data loss wearing a helpful expression.

#pragma once

#include <juce_core/juce_core.h>

namespace presetfirstrun
{

/** The shipped default preset file names, in ship order. */
juce::StringArray defaultPresetFileNames();

struct SeedResult
{
    int copied  = 0;  ///< Files written.
    int skipped = 0;  ///< Files left alone because they already existed.

    juce::StringArray errors;  ///< One per file that could not be seeded.
    bool ok() const { return errors.isEmpty(); }
};

/** Copies each default preset from `sourceDir` into `targetDir`, creating
    `targetDir` (including parents) when missing, and never overwriting a file
    that is already there.

    Both directories are parameters rather than assumed: tests use temp dirs,
    and the eventual caller decides where the shipped defaults live next to the
    installed exe.
*/
SeedResult seedDefaultPresets (const juce::File& sourceDir,
                               const juce::File& targetDir);

} // namespace presetfirstrun
