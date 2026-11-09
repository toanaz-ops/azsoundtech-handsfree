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

// --- lane L2b: copilot ask settings -----------------------------------------

TEST (AppSettings, CopilotDefaultsWhenKeysAreAbsent)
{
    // A file written by an older build has none of the copilot_* keys.
    const auto s = appSettingsFromHost (hostFromJson (R"({"uiMode":"geek"})"));
    EXPECT_EQ (s.uiMode, UiMode::Geek);
    EXPECT_EQ (s.copilotProvider, "anthropic");
    EXPECT_EQ (s.copilotModel, "claude-opus-5-5");
    EXPECT_TRUE (s.copilotBaseUrl.isEmpty());
    EXPECT_FALSE (s.copilotConsent);
    EXPECT_FALSE (s.hasRouting);
}

TEST (AppSettings, CopilotRoundTrip)
{
    AppSettings in;
    in.copilotProvider = "openai_compatible";
    in.copilotModel = "llama3:8b";
    in.copilotBaseUrl = "http://localhost:11434/v1";
    in.copilotConsent = true;

    const auto out = appSettingsFromHost (appSettingsToHost (in));
    EXPECT_EQ (out.copilotProvider, "openai_compatible");
    EXPECT_EQ (out.copilotModel, "llama3:8b");
    EXPECT_EQ (out.copilotBaseUrl, "http://localhost:11434/v1");
    EXPECT_TRUE (out.copilotConsent);
}

TEST (AppSettings, CopilotInvalidValuesFallBackToDefaults)
{
    const auto s = appSettingsFromHost (hostFromJson (
        R"({"copilot_provider":"mystery","copilot_model":"bad model!","copilot_base_url":"ftp://x","copilot_consent":"yes"})"));
    EXPECT_EQ (s.copilotProvider, "anthropic");
    EXPECT_EQ (s.copilotModel, "claude-opus-5-5");
    EXPECT_TRUE (s.copilotBaseUrl.isEmpty());
    EXPECT_FALSE (s.copilotConsent);
}

TEST (AppSettings, CopilotDashLeadingModelFallsBackToDefault)
{
    const auto s = appSettingsFromHost (hostFromJson (R"({"copilot_model":"-abc"})"));
    EXPECT_EQ (s.copilotModel, "claude-opus-5-5");
}

TEST (AppSettings, CopilotSettingsNeverCarryAKeyField)
{
    const auto host = appSettingsToHost ({});
    EXPECT_FALSE (host.product.hasProperty ("copilot_key"));
    EXPECT_FALSE (host.product.hasProperty ("copilot_api_key"));
}

// Lane L3a: the requested AI mode.
TEST (AppSettings, AiModeDefaultsToShadowAndOldFilesLoadAsShadow)
{
    EXPECT_EQ (AppSettings {}.aiMode, AiMode::Shadow);
    EXPECT_EQ (appSettingsFromHost ({}).aiMode, AiMode::Shadow);
    // A settings file written before the key existed.
    EXPECT_EQ (appSettingsFromHost (hostFromJson (R"({"uiMode":"geek","copilot_consent":true})")).aiMode,
               AiMode::Shadow);
}

TEST (AppSettings, AiModeRoundTripsOffShadowSuggest)
{
    for (const auto m : { AiMode::Off, AiMode::Shadow, AiMode::Suggest })
    {
        AppSettings in;
        in.aiMode = m;
        EXPECT_EQ (appSettingsFromHost (appSettingsToHost (in)).aiMode, m) << aiModeToString (m);
    }
}

TEST (AppSettings, StoredActIsDemotedToSuggestOnLoad)
{
    AppSettings in;
    in.aiMode = AiMode::Act;
    const auto host = appSettingsToHost (in);
    EXPECT_EQ (host.product["ai_mode"].toString(), "act");   // saved as asked...
    EXPECT_EQ (appSettingsFromHost (host).aiMode, AiMode::Suggest);   // ...never restored as Act (D2)
}

TEST (AppSettings, UnknownOrMistypedAiModeLoadsAsShadow)
{
    for (const char* json : { R"({"ai_mode":"turbo"})", R"({"ai_mode":""})", R"({"ai_mode":3})",
                              R"({"ai_mode":true})", R"({"ai_mode":null})", R"({"ai_mode":"ACT"})" })
        EXPECT_EQ (appSettingsFromHost (hostFromJson (json)).aiMode, AiMode::Shadow) << json;
}

TEST (AiMode, NamesRoundTripAndEffectiveModeIsTheLowerOfTwo)
{
    for (const auto m : { AiMode::Off, AiMode::Shadow, AiMode::Suggest, AiMode::Act })
        EXPECT_EQ (aiModeFromString (aiModeToString (m)), m);
    EXPECT_STREQ (aiModeToString (AiMode::Off), "off");
    EXPECT_STREQ (aiModeToString (AiMode::Shadow), "shadow");
    EXPECT_STREQ (aiModeToString (AiMode::Suggest), "suggest");
    EXPECT_STREQ (aiModeToString (AiMode::Act), "act");
    EXPECT_EQ (aiModeFromString ("nonsense"), AiMode::Shadow);

    for (const auto a : { AiMode::Off, AiMode::Shadow, AiMode::Suggest, AiMode::Act })
        for (const auto b : { AiMode::Off, AiMode::Shadow, AiMode::Suggest, AiMode::Act })
        {
            const auto e = effectiveAiMode (a, b);
            EXPECT_EQ (e, effectiveAiMode (b, a));
            EXPECT_LE ((int) e, (int) a);
            EXPECT_LE ((int) e, (int) b);
            EXPECT_TRUE (e == a || e == b);
        }
    EXPECT_EQ (effectiveAiMode (AiMode::Act, AiMode::Shadow), AiMode::Shadow);   // network ceiling today
}
