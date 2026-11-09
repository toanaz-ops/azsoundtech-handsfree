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

/** The ORIGIN of an http(s) base URL, the unit a stored OpenAI-compatible key is
    bound to: lowercase scheme + "://" + lowercase host (IPv6 keeps its brackets)
    + ":port" only when a port is given and is not the scheme default (80 / 443);
    path, query and fragment are dropped. Empty when the URL is not a valid
    base URL (isValidCopilotBaseUrl). The Python sidecar implements the same
    function; both must satisfy the vectors in tests/test_copilotcredentialstore.cpp. */
juce::String copilotBaseUrlOrigin (const juce::String& baseUrl);

class CopilotCredentialStore
{
public:
    explicit CopilotCredentialStore (juce::File directory = defaultCopilotCredentialsDirectory());

    /** Providers: exactly "anthropic" and "openai_compatible". The key is trimmed
        and must be a non-empty single line of at most 4096 UTF-8 bytes. False
        (nothing written) otherwise, or when encryption/writing fails.
        For "openai_compatible" `baseUrl` is REQUIRED and valid: the entry records
        copilotBaseUrlOrigin (baseUrl) next to the blob, so the key is only ever
        sent to the origin it was saved for. `baseUrl` is ignored for "anthropic". */
    bool storeKey (const juce::String& provider, const juce::String& key,
                   const juce::String& baseUrl = {});

    enum class KeyState
    {
        none,          // no entry, unreadable, or its blob does not decrypt
        ready,         // usable (for "openai_compatible": origin matches `baseUrl`)
        otherOrigin    // "openai_compatible" entry without an origin (release 1.5.0)
                       // or bound to another origin: never used
    };

    [[nodiscard]] KeyState keyState (const juce::String& provider, const juce::String& baseUrl = {}) const;

    /** keyState (...) == KeyState::ready. */
    bool hasKey (const juce::String& provider, const juce::String& baseUrl = {}) const;

    /** Removes that profile (the other is kept). True if a profile was removed. */
    bool deleteKey (const juce::String& provider);

    juce::File file() const;

    /** False off Windows: store/has/delete then always return false. */
    static bool isSupported();

private:
    juce::File directory_;
};
