// Lane U (spec 2026-10-18 section 3.2): Hands-free's own section of the shared
// host settings file -- which UI mode was last used, and the routing table.
//
// The library (az_host_settings) stores the device and an opaque `product`
// var; this file is the only place that knows what Hands-free puts in it. It
// is pure conversion: no file I/O beyond naming the default path, no engine.

#pragma once

#include <juce_core/juce_core.h>

#include "app/AiMode.h"
#include "app/SlotConfig.h"
#include "az/host/HostSettings.h"

#include <array>

enum class UiMode { Dumb, Geek };

struct AppSettings
{
    // A fresh install opens in Dumb (decision record Q2). An unreadable value
    // falls back to the same default.
    UiMode uiMode = UiMode::Dumb;

    az::host::DeviceSetup device;

    // False when the file carried no routing -- first run, or a file from a
    // build that did not write it. The engine's routing is then left exactly
    // as it is: "absent" must never read as "eight disabled slots".
    bool hasRouting = false;
    std::array<SlotConfig, kMaxSlots> slots {};

    // Lane L2b "ask the AI": which provider/model to use and whether the owner
    // agreed to send the question + session log to it. NEVER an API key (that
    // lives only in the DPAPI store, CopilotCredentialStore).
    juce::String copilotProvider = "anthropic"; // "anthropic" | "openai_compatible"
    juce::String copilotModel = "claude-opus-5-5";
    juce::String copilotBaseUrl;                // only meaningful for openai_compatible
    bool copilotConsent = false;

    // Lane L3a: the REQUESTED AI mode (each actor is clamped by its own
    // ceiling, see effectiveAiMode). Shadow = today's behaviour. Act is never
    // restored from disk: appSettingsFromHost demotes a stored Act to Suggest,
    // so acting is opted into per session (decision D2).
    AiMode aiMode = AiMode::Shadow;
};

// Lenient by design, unlike PresetManager: settings are the app's own scratch
// memory, not a file a person hands to someone else. A slot entry that does
// not parse comes back DISABLED (never "clamped into range": a clamped channel
// is a mic routed somewhere nobody chose), and the rest still load.
AppSettings appSettingsFromHost (const az::host::HostSettings& host);
az::host::HostSettings appSettingsToHost (const AppSettings& settings);

// %APPDATA%/AZSoundtech/HandsFree/settings.json -- sibling of presets/ and
// logs/ (PresetManager::getPresetDirectory, SessionLogger).
juce::File defaultAppSettingsFile();

juce::String uiModeName (UiMode mode);
