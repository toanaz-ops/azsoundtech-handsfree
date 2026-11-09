// DataRoot: DEVCAVE_TEST_PROFILE redirects every per-user path, and a bad
// value is refused instead of falling back to real data.

#include <gtest/gtest.h>

#include "app/DataRoot.h"
#include "app/LicenseManager.h"
#include "app/AppSettings.h"
#include "app/PresetManager.h"
#include "app/SessionLogger.h"

namespace
{
const juce::File kRealAppData ("C:/RealUser/AppData/Roaming");
const juce::File kRealDocs ("C:/RealUser/Documents");
const juce::String kProfile ("C:/Tmp/profile-x");
}

TEST (DataRoot, UnsetUsesRealPaths)
{
    const auto r = DataRoot::resolveFor ({}, kRealAppData, kRealDocs);

    ASSERT_TRUE (r.ok);
    EXPECT_FALSE (r.testProfile);
    EXPECT_EQ (r.roamingRoot, kRealAppData.getChildFile ("AZSoundtech").getChildFile ("HandsFree"));
    EXPECT_EQ (r.documentsDir, kRealDocs);
}

TEST (DataRoot, SetPlacesEverythingUnderTheProfile)
{
    const auto r = DataRoot::resolveFor (kProfile, kRealAppData, kRealDocs);

    ASSERT_TRUE (r.ok);
    EXPECT_TRUE (r.testProfile);
    const juce::File profile (kProfile);
    EXPECT_TRUE (r.roamingRoot.isAChildOf (profile));
    EXPECT_TRUE (r.documentsDir.isAChildOf (profile));
    EXPECT_EQ (r.roamingRoot, profile.getChildFile ("AppData/AZSoundtech/HandsFree"));
    EXPECT_EQ (r.documentsDir, profile.getChildFile ("Documents"));
    EXPECT_FALSE (r.roamingRoot.isAChildOf (kRealAppData));
}

TEST (DataRoot, RelativeOrBlankValueIsRejectedWithoutFallback)
{
    for (const auto* bad : { "relative/dir", "profile", "   " })
    {
        const auto r = DataRoot::resolveFor (bad, kRealAppData, kRealDocs);
        EXPECT_FALSE (r.ok) << bad;
        EXPECT_TRUE (r.error.isNotEmpty());
        EXPECT_EQ (r.roamingRoot, juce::File());
        EXPECT_EQ (r.documentsDir, juce::File());
    }
}

// The four routed helpers all follow DataRoot::current(); with the variable
// unset (the normal CI case) they sit under the real roaming root, and with it
// set under the profile. Either way they are children of roamingRoot().
TEST (DataRoot, RoutedPathsAreChildrenOfTheRoamingRoot)
{
    const auto root = DataRoot::roamingRoot();
    ASSERT_NE (root, juce::File());

    EXPECT_TRUE (defaultAppSettingsFile().isAChildOf (root));
    EXPECT_TRUE (LicenseManager::defaultStorageFile().isAChildOf (root));
    EXPECT_TRUE (PresetManager::getPresetDirectory().isAChildOf (root));
    EXPECT_TRUE (SessionLogger::defaultDirectory().isAChildOf (root));
}
