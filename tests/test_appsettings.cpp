// Lane U (spec 2026-10-18 section 3.2): Hands-free's section of settings.json.

#include <gtest/gtest.h>

#include "app/AppSettings.h"

namespace
{
az::host::HostSettings hostFromJson (const char* productJson)
{
    az::host::HostSettings h;
    h.product = juce::JSON::parse (juce::String (productJson));
    return h;
}
}

TEST (AppSettings, FreshInstallIsDumbWithNoRouting)
{
    const auto s = appSettingsFromHost ({});
    EXPECT_EQ (s.uiMode, UiMode::Dumb);
    EXPECT_FALSE (s.hasRouting);
}

TEST (AppSettings, UnknownUiModeFallsBackToDumb)
{
    EXPECT_EQ (appSettingsFromHost (hostFromJson (R"({"uiMode":"expert"})")).uiMode, UiMode::Dumb);
    EXPECT_EQ (appSettingsFromHost (hostFromJson (R"({"uiMode":"geek"})")).uiMode,   UiMode::Geek);
}

TEST (AppSettings, RoundTripKeepsModeDeviceAndEverySlot)
{
    AppSettings in;
    in.uiMode     = UiMode::Geek;
    in.device     = { "ASIO", "RME", 96000.0, 64 };
    in.hasRouting = true;
    in.slots[0]   = { true, 1, { 3, 0 }, { 5, 0 } };
    in.slots[7]   = { true, 2, { 6, 7 }, { 0, 1 } };

    const auto out = appSettingsFromHost (appSettingsToHost (in));
    EXPECT_EQ (out.uiMode, UiMode::Geek);
    EXPECT_EQ (out.device, in.device);
    ASSERT_TRUE (out.hasRouting);
    for (int i = 0; i < kMaxSlots; ++i)
    {
        const auto& a = in.slots[(std::size_t) i];
        const auto& b = out.slots[(std::size_t) i];
        EXPECT_EQ (a.enabled, b.enabled) << i;
        EXPECT_EQ (a.width, b.width) << i;
        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            EXPECT_EQ (a.inputChannels[lane],  b.inputChannels[lane])  << i;
            EXPECT_EQ (a.outputChannels[lane], b.outputChannels[lane]) << i;
        }
    }
}

TEST (AppSettings, NoRoutingIsNotWrittenAsEightDisabledSlots)
{
    const auto host = appSettingsToHost ({});
    EXPECT_FALSE (host.product.hasProperty ("slots"));
}

TEST (AppSettings, BadSlotEntryComesBackDisabledAndTheRestStillLoad)
{
    const auto s = appSettingsFromHost (hostFromJson (R"({"slots":[
        {"enabled":true,"width":1,"inputChannels":[-1,0],"outputChannels":[0,1]},
        {"enabled":true,"width":3,"inputChannels":[0,1],"outputChannels":[0,1]},
        {"enabled":true,"width":2,"inputChannels":[0,1.5],"outputChannels":[0,1]},
        {"enabled":true,"width":2,"inputChannels":[2,3],"outputChannels":[4,5]}
    ]})"));

    ASSERT_TRUE (s.hasRouting);
    EXPECT_FALSE (s.slots[0].enabled);
    EXPECT_FALSE (s.slots[1].enabled);
    EXPECT_FALSE (s.slots[2].enabled);
    EXPECT_TRUE  (s.slots[3].enabled);
    EXPECT_EQ    (s.slots[3].outputChannels[1], 5);
    EXPECT_FALSE (s.slots[4].enabled);   // absent entries: disabled default
}
