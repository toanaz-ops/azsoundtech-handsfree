// Tests for PresetFirstRun: seeding the shipped default presets into the
// user's preset directory on first run (plan Task 26, second half).
//
// The installer does NOT copy them (verified against installer/handsfree.nsi
// 2026-08-23 -- its only presets mention is the uninstaller's delete prompt),
// so this in-app path is the only one that exists.

#include <gtest/gtest.h>

#include <app/PresetFirstRun.h>

namespace
{

struct TempDir
{
    TempDir()
    {
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("HFSeedTest_" + juce::String (juce::Random::getSystemRandom().nextInt (0x7fffffff)));
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }
    juce::File dir;
};

juce::File sourceWithDefaults()
{
    // The REAL shipped files off disk, via the same CMake definition
    // test_presetmanager.cpp uses -- a test that only exercises a copy of a
    // copy proves nothing about what ships.
    return juce::File (HANDSFREE_PRESET_SOURCE_DIR);
}

TEST (PresetFirstRun, SeedsBothShippedDefaultsIntoAnEmptyDirectory)
{
    TempDir target;
    const auto result = presetfirstrun::seedDefaultPresets (sourceWithDefaults(), target.dir);

    EXPECT_TRUE (result.ok()) << result.errors.joinIntoString ("; ");
    EXPECT_EQ (result.copied, 2);
    for (const auto& name : presetfirstrun::defaultPresetFileNames())
        EXPECT_TRUE (target.dir.getChildFile (name).existsAsFile())
            << name << " was not seeded";
}

TEST (PresetFirstRun, NeverOverwritesAFileTheUserAlreadyHas)
{
    // Breaks if seeding uses File::copyFileTo unconditionally: an upgrade or a
    // second launch would silently revert every user edit to the shipped text.
    TempDir target;
    auto userCopy = target.dir.getChildFile ("Speech.json");
    userCopy.replaceWithText ("{ \"kept\": true }");

    const auto result = presetfirstrun::seedDefaultPresets (sourceWithDefaults(), target.dir);

    EXPECT_TRUE (result.ok());
    EXPECT_EQ (result.copied, 1);   // Music.json seeded...
    EXPECT_EQ (result.skipped, 1);  // ...Speech.json left alone
    EXPECT_TRUE (userCopy.loadFileAsString().contains ("\"kept\""))
        << "the user's Speech.json was overwritten";
}

TEST (PresetFirstRun, CreatesTheTargetDirectoryWhenMissing)
{
    // Breaks if seeding requires the caller to pre-create %APPDATA%/.../presets.
    TempDir base;
    const auto nested = base.dir.getChildFile ("AZSoundtech").getChildFile ("presets");
    const auto result = presetfirstrun::seedDefaultPresets (sourceWithDefaults(), nested);

    EXPECT_TRUE (result.ok()) << result.errors.joinIntoString ("; ");
    EXPECT_TRUE (nested.getChildFile ("Music.json").existsAsFile());
}

TEST (PresetFirstRun, ReportsAMissingSourceInsteadOfFailingSilently)
{
    // Breaks if a missing source file is swallowed: a renamed default would
    // ship an empty preset folder and nobody would know why.
    TempDir source, target;
    const auto result = presetfirstrun::seedDefaultPresets (source.dir, target.dir);

    EXPECT_FALSE (result.ok());
    EXPECT_EQ (result.copied, 0);
    EXPECT_EQ (result.errors.size(), presetfirstrun::defaultPresetFileNames().size());
}

} // namespace
