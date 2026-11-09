#include "app/CopilotCredentialStore.h"

#include "app/AppSettings.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <wincrypt.h>
#endif

namespace
{
constexpr const char* kEnvDir = "HANDSFREE_COPILOT_CREDENTIALS_DIR";
constexpr const char* kFileName = "copilot-credentials.json";

#if JUCE_WINDOWS
constexpr int kMaxKeyBytes = 4096;

// Exactly the bytes of DPAPI_ENTROPY in tools/copilot/copilot.py (no terminator).
constexpr char kEntropy[] = "AZSoundtech.HandsFree.Copilot.v1";
constexpr std::size_t kEntropyLength = sizeof (kEntropy) - 1;

using Bytes = std::vector<std::uint8_t>;

bool isKnownProvider (const juce::String& p)
{
    return p == "anthropic" || p == "openai_compatible";
}

void wipe (Bytes& b)
{
    if (b.empty())
        return;
    SecureZeroMemory (b.data(), b.size());
    b.clear();
}

// Standard RFC 4648 base64 with padding (what Python's base64.b64decode reads).
// juce::Base64 / MemoryBlock::toBase64Encoding are deliberately not used.
juce::String base64Encode (const Bytes& in)
{
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    juce::String out;
    for (std::size_t i = 0; i < in.size(); i += 3)
    {
        const std::size_t n = std::min<std::size_t> (3, in.size() - i);
        const std::uint32_t v = ((std::uint32_t) in[i] << 16)
                              | (n > 1 ? (std::uint32_t) in[i + 1] << 8 : 0u)
                              | (n > 2 ? (std::uint32_t) in[i + 2] : 0u);
        const char quad[5] = { alphabet[(v >> 18) & 63], alphabet[(v >> 12) & 63],
                               n > 1 ? alphabet[(v >> 6) & 63] : '=',
                               n > 2 ? alphabet[v & 63] : '=', 0 };
        out += juce::String (quad);
    }
    return out;
}

// Strict: length multiple of 4, alphabet only, padding only at the end.
bool base64Decode (const juce::String& text, Bytes& out)
{
    out.clear();
    const auto s = text.toStdString();
    if (s.empty() || s.size() % 4 != 0)
        return false;

    auto value = [] (char c) -> int
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    for (std::size_t i = 0; i < s.size(); i += 4)
    {
        const bool last = i + 4 == s.size();
        int pad = 0;
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char c = s[i + (std::size_t) k];
            if (c == '=')
            {
                if (! last || k < 2)
                    return false;
                ++pad;
                v <<= 6;
            }
            else
            {
                if (pad > 0)
                    return false;
                const int d = value (c);
                if (d < 0)
                    return false;
                v = (v << 6) | (std::uint32_t) d;
            }
        }
        out.push_back ((std::uint8_t) (v >> 16));
        if (pad < 2) out.push_back ((std::uint8_t) (v >> 8));
        if (pad < 1) out.push_back ((std::uint8_t) v);
    }
    return true;
}

bool dpapiProtect (const Bytes& plain, Bytes& out)
{
    DATA_BLOB in { (DWORD) plain.size(), const_cast<BYTE*> (plain.data()) };
    DATA_BLOB ent { (DWORD) kEntropyLength, reinterpret_cast<BYTE*> (const_cast<char*> (kEntropy)) };
    DATA_BLOB res {};
    if (! CryptProtectData (&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &res))
        return false;
    out.assign (res.pbData, res.pbData + res.cbData);
    LocalFree (res.pbData);
    return true;
}

// On success `out` holds the plaintext; the caller wipes it.
bool dpapiUnprotect (const Bytes& blob, Bytes& out)
{
    DATA_BLOB in { (DWORD) blob.size(), const_cast<BYTE*> (blob.data()) };
    DATA_BLOB ent { (DWORD) kEntropyLength, reinterpret_cast<BYTE*> (const_cast<char*> (kEntropy)) };
    DATA_BLOB res {};
    if (! CryptUnprotectData (&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &res))
        return false;
    out.assign (res.pbData, res.pbData + res.cbData);
    SecureZeroMemory (res.pbData, res.cbData);
    LocalFree (res.pbData);
    return true;
}

enum class LoadState { missing, ok, corrupt };

// `profiles` is a fresh, mutable copy of the stored profiles object.
LoadState loadProfiles (const juce::File& file, juce::DynamicObject::Ptr& profiles)
{
    profiles = new juce::DynamicObject();
    if (! file.existsAsFile())
        return LoadState::missing;

    juce::var parsed;
    if (! juce::JSON::parse (file.loadFileAsString(), parsed).wasOk())
        return LoadState::corrupt;

    auto* root = parsed.getDynamicObject();
    if (root == nullptr)
        return LoadState::corrupt;
    auto* stored = root->getProperty ("profiles").getDynamicObject();
    if (stored == nullptr)
        return LoadState::corrupt;

    for (const auto& nv : stored->getProperties())
        profiles->setProperty (nv.name, nv.value);
    return LoadState::ok;
}

bool writeProfiles (const juce::File& file, const juce::DynamicObject::Ptr& profiles)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("profiles", juce::var (profiles.get()));
    const auto text = juce::JSON::toString (juce::var (root.get()), false);

    if (! file.getParentDirectory().createDirectory().wasOk())
        return false;

    const auto tmp = file.getSiblingFile (file.getFileName() + ".tmp");
    if (! tmp.replaceWithText (text, false, false, "\n"))
    {
        tmp.deleteFile();
        return false;
    }
    if (! tmp.replaceFileIn (file))
    {
        tmp.deleteFile();
        return false;
    }
    return true;
}
#endif // JUCE_WINDOWS
}

//==============================================================================
juce::File defaultCopilotCredentialsDirectory()
{
    const auto fromEnv = juce::SystemStats::getEnvironmentVariable (kEnvDir, {});
    if (fromEnv.isNotEmpty())
        return juce::File (fromEnv);
    return defaultAppSettingsFile().getParentDirectory();
}

CopilotCredentialStore::CopilotCredentialStore (juce::File directory)
    : directory_ (std::move (directory)) {}

juce::File CopilotCredentialStore::file() const
{
    return directory_.getChildFile (kFileName);
}

bool CopilotCredentialStore::isSupported()
{
#if JUCE_WINDOWS
    return true;
#else
    return false;
#endif
}

#if JUCE_WINDOWS

bool CopilotCredentialStore::storeKey (const juce::String& provider, const juce::String& rawKey)
{
    if (! isKnownProvider (provider))
        return false;

    const auto key = rawKey.trim();
    if (key.isEmpty() || key.containsAnyOf ("\r\n")
        || (int) key.getNumBytesAsUTF8() > kMaxKeyBytes)
        return false;

    const auto* utf8 = (const std::uint8_t*) key.toRawUTF8();
    Bytes plain (utf8, utf8 + key.getNumBytesAsUTF8());
    Bytes encrypted;
    const bool encryptedOk = dpapiProtect (plain, encrypted);
    wipe (plain);
    if (! encryptedOk)
        return false;

    const auto target = file();
    juce::DynamicObject::Ptr profiles;
    if (loadProfiles (target, profiles) == LoadState::corrupt)
    {
        // Never delete an unreadable file: it may hold the other provider's blob.
        const auto aside = target.getSiblingFile (target.getFileName() + ".corrupt");
        const bool moved = aside.existsAsFile() ? target.replaceFileIn (aside)
                                                : target.moveFileTo (aside);
        if (! moved)
            return false;
        profiles = new juce::DynamicObject();
    }

    juce::DynamicObject::Ptr entry = new juce::DynamicObject();
    entry->setProperty ("blob", base64Encode (encrypted));
    profiles->setProperty (juce::Identifier (provider), juce::var (entry.get()));
    return writeProfiles (target, profiles);
}

bool CopilotCredentialStore::hasKey (const juce::String& provider) const
{
    if (! isKnownProvider (provider))
        return false;

    juce::DynamicObject::Ptr profiles;
    if (loadProfiles (file(), profiles) != LoadState::ok)
        return false;

    auto* entry = profiles->getProperty (juce::Identifier (provider)).getDynamicObject();
    if (entry == nullptr)
        return false;
    const auto blobText = entry->getProperty ("blob");
    if (! blobText.isString())
        return false;

    Bytes blob, plain;
    if (! base64Decode (blobText.toString(), blob))
        return false;
    const bool ok = dpapiUnprotect (blob, plain);
    wipe (plain);
    return ok;
}

bool CopilotCredentialStore::deleteKey (const juce::String& provider)
{
    if (! isKnownProvider (provider))
        return false;

    const auto target = file();
    juce::DynamicObject::Ptr profiles;
    if (loadProfiles (target, profiles) != LoadState::ok)
        return false;

    const juce::Identifier id (provider);
    if (! profiles->hasProperty (id))
        return false;
    profiles->removeProperty (id);
    return writeProfiles (target, profiles);
}

#else // ! JUCE_WINDOWS

bool CopilotCredentialStore::storeKey (const juce::String&, const juce::String&) { return false; }
bool CopilotCredentialStore::hasKey (const juce::String&) const { return false; }
bool CopilotCredentialStore::deleteKey (const juce::String&) { return false; }

#endif
