// CopilotCredentialStore: the app-side WRITER of the sidecar's API-key store.
//
// Spec: docs/superpowers/specs/2026-11-09-copilot-panel-design.md section 7 and
// "L2b sidecar contract" (source of truth: tools/copilot/copilot.py). The key is
// encrypted with Windows DPAPI (per Windows user) and never goes through argv,
// the environment, a log or settings.json; the sidecar decrypts it itself.
// This stays in the app, not a shared library: Hands-free is the only product
// that uses it today (CLAUDE.md rule on shared code).

#pragma once

#include <juce_core/juce_core.h>

/** `HANDSFREE_COPILOT_CREDENTIALS_DIR` when set and non-empty, else the directory
    of defaultAppSettingsFile() (%APPDATA%/AZSoundtech/HandsFree). */
juce::File defaultCopilotCredentialsDirectory();

class CopilotCredentialStore
{
public:
    explicit CopilotCredentialStore (juce::File directory = defaultCopilotCredentialsDirectory());

    /** Providers: exactly "anthropic" and "openai_compatible". The key is trimmed
        and must be a non-empty single line of at most 4096 UTF-8 bytes. False
        (nothing written) otherwise, or when encryption/writing fails. */
    bool storeKey (const juce::String& provider, const juce::String& key);

    /** True only when the profile exists AND its blob decrypts. */
    bool hasKey (const juce::String& provider) const;

    /** Removes that profile (the other is kept). True if a profile was removed. */
    bool deleteKey (const juce::String& provider);

    juce::File file() const;

    /** False off Windows: store/has/delete then always return false. */
    static bool isSupported();

private:
    juce::File directory_;
};
