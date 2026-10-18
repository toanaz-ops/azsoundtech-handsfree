// az_host_settings (lane U, spec 2026-10-18 section 3.1): the shared library
// that remembers the audio device between launches.

#include <gtest/gtest.h>

#include "az/host/HostSettings.h"

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("hf-hostsettings-" + juce::Uuid().toString());
    TempDir()  { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
    juce::File file() const { return dir.getChildFile ("settings.json"); }
};
}

TEST (HostSettings, MissingFileIsFirstRunNotAnError)
{
    TempDir t;
    const auto r = az::host::loadHostSettings (t.file());
    EXPECT_TRUE (r.error.isEmpty());
    EXPECT_TRUE (r.mayOverwrite);
    EXPECT_TRUE (r.settings.device.type.isEmpty());
    EXPECT_TRUE (r.settings.product.isVoid());
}

TEST (HostSettings, RoundTripKeepsDeviceAndProductSection)
{
    TempDir t;
    az::host::HostSettings in;
    in.device = { "ASIO", "Focusrite USB ASIO", 48000.0, 128 };
    auto product = std::make_unique<juce::DynamicObject>();
    product->setProperty ("uiMode", "geek");
    in.product = juce::var (product.release());

    ASSERT_TRUE (az::host::saveHostSettings (t.file(), in));

    const auto r = az::host::loadHostSettings (t.file());
    EXPECT_TRUE (r.error.isEmpty());
    EXPECT_EQ (r.settings.device, in.device);
    EXPECT_EQ (r.settings.product["uiMode"].toString(), "geek");
}

TEST (HostSettings, SaveCreatesParentAndLeavesNoTemporaryBehind)
{
    TempDir t;
    const auto nested = t.dir.getChildFile ("a").getChildFile ("settings.json");
    ASSERT_TRUE (az::host::saveHostSettings (nested, {}));
    EXPECT_TRUE (nested.existsAsFile());
    EXPECT_EQ (nested.getParentDirectory().getNumberOfChildFiles (juce::File::findFiles), 1);
}

TEST (HostSettings, MalformedFileReturnsDefaultsAndIsQuarantined)
{
    TempDir t;
    ASSERT_TRUE (t.file().replaceWithText ("{ not json"));

    const auto r = az::host::loadHostSettings (t.file());
    EXPECT_FALSE (r.error.isEmpty());
    EXPECT_TRUE (r.mayOverwrite);
    EXPECT_TRUE (r.settings.device.type.isEmpty());
    EXPECT_FALSE (t.file().existsAsFile());
    EXPECT_TRUE (t.dir.getChildFile ("settings.json.bad").existsAsFile());
}

TEST (HostSettings, NewerSchemaIsLeftUntouchedAndMustNotBeOverwritten)
{
    TempDir t;
    const juce::String newer = R"({"schema":2,"device":{"type":"ASIO"}})";
    ASSERT_TRUE (t.file().replaceWithText (newer));

    const auto r = az::host::loadHostSettings (t.file());
    EXPECT_FALSE (r.error.isEmpty());
    EXPECT_FALSE (r.mayOverwrite);
    EXPECT_TRUE (r.settings.device.type.isEmpty());
    EXPECT_EQ (t.file().loadFileAsString(), newer);
    EXPECT_FALSE (t.dir.getChildFile ("settings.json.bad").existsAsFile());
}

TEST (HostSettings, MissingSchemaIsMalformed)
{
    TempDir t;
    ASSERT_TRUE (t.file().replaceWithText (R"({"device":{"type":"ASIO"}})"));

    const auto r = az::host::loadHostSettings (t.file());
    EXPECT_FALSE (r.error.isEmpty());
    EXPECT_TRUE (r.mayOverwrite);
    EXPECT_TRUE (t.dir.getChildFile ("settings.json.bad").existsAsFile());
}

TEST (HostSettings, OutOfRangeRateAndBufferReadAsNotSaved)
{
    TempDir t;
    ASSERT_TRUE (t.file().replaceWithText (
        R"({"schema":1,"device":{"type":"ASIO","name":"X","sampleRate":-48000,"bufferSize":999999}})"));

    const auto r = az::host::loadHostSettings (t.file());
    EXPECT_TRUE (r.error.isEmpty());
    EXPECT_EQ (r.settings.device.type, "ASIO");
    EXPECT_EQ (r.settings.device.sampleRate, 0.0);
    EXPECT_EQ (r.settings.device.bufferSize, 0);
}

TEST (HostSettings, EmptyFileIsMalformedNotUnreadable)
{
    TempDir t;
    ASSERT_TRUE (t.file().replaceWithText (""));
    const auto r = az::host::loadHostSettings (t.file());
    EXPECT_FALSE (r.error.isEmpty());
    EXPECT_TRUE (r.mayOverwrite);   // 0 bytes: nothing to protect
}
