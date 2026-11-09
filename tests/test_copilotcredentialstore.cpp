// Lane L2b: CopilotCredentialStore (DPAPI writer of copilot-credentials.json),
// including a cross-check against the REAL Python reader in tools/copilot.
//
// Only obviously fake key values are used here.

#include <gtest/gtest.h>

#include "app/CopilotCredentialStore.h"

#include <cstdlib>

namespace
{
const juce::String kFakeKey = "dummy-key-for-tests-0000";
const juce::String kLocalUrl = "http://localhost:11434/v1";

struct TempDir
{
    TempDir()
    {
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getNonexistentChildFile ("hf-copilot-cred", {}, false);
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }

    juce::File dir;
};

// Sets an environment variable for this process and restores it on scope exit.
struct ScopedEnv
{
    ScopedEnv (const char* n, const juce::String& value) : name (n)
    {
        hadOld = juce::SystemStats::getEnvironmentVariable (name, {}).isNotEmpty();
        old = juce::SystemStats::getEnvironmentVariable (name, {});
        set (value);
    }
    ~ScopedEnv() { set (hadOld ? old : juce::String()); }

    void set (const juce::String& v) const
    {
       #if JUCE_WINDOWS
        _putenv_s (name, v.toRawUTF8()); // empty value removes the variable
       #else
        if (v.isEmpty()) unsetenv (name); else setenv (name, v.toRawUTF8(), 1);
       #endif
    }

    const char* name;
    bool hadOld = false;
    juce::String old;
};

constexpr const char* kEnvName = "HANDSFREE_COPILOT_CREDENTIALS_DIR";

juce::String readAll (const juce::File& f) { return f.loadFileAsString(); }

#define REQUIRE_DPAPI() \
    do { if (! CopilotCredentialStore::isSupported()) GTEST_SKIP() << "DPAPI needs Windows"; } while (false)
}

TEST (CopilotCredentialStore, DefaultDirectoryHonoursTheEnvVar)
{
    TempDir t;
    {
        ScopedEnv env (kEnvName, t.dir.getFullPathName());
        EXPECT_EQ (defaultCopilotCredentialsDirectory(), t.dir);
        EXPECT_EQ (CopilotCredentialStore().file(), t.dir.getChildFile ("copilot-credentials.json"));
    }
    {
        ScopedEnv env (kEnvName, "");
        const auto dir = defaultCopilotCredentialsDirectory();
        EXPECT_EQ (dir.getFileName(), "HandsFree");
        EXPECT_EQ (dir.getParentDirectory().getFileName(), "AZSoundtech");
    }
}

TEST (CopilotCredentialStore, RoundTripStoreHasDelete)
{
    REQUIRE_DPAPI();
    TempDir t;
    CopilotCredentialStore store (t.dir);

    EXPECT_FALSE (store.hasKey ("anthropic"));
    ASSERT_TRUE (store.storeKey ("anthropic", "  " + kFakeKey + " \t"));
    EXPECT_TRUE (store.hasKey ("anthropic"));
    EXPECT_FALSE (store.hasKey ("openai_compatible"));

    const auto text = readAll (store.file());
    EXPECT_FALSE (text.contains (kFakeKey));
    EXPECT_FALSE (text.contains ("dummy"));

    juce::var parsed;
    ASSERT_TRUE (juce::JSON::parse (text, parsed).wasOk());
    EXPECT_EQ ((int) parsed["version"], 1);
    EXPECT_TRUE (parsed["profiles"]["anthropic"]["blob"].toString().isNotEmpty());

    EXPECT_TRUE (store.deleteKey ("anthropic"));
    EXPECT_FALSE (store.hasKey ("anthropic"));
    EXPECT_FALSE (store.deleteKey ("anthropic"));
}

TEST (CopilotCredentialStore, SecondProviderIsPreserved)
{
    REQUIRE_DPAPI();
    TempDir t;
    CopilotCredentialStore store (t.dir);

    ASSERT_TRUE (store.storeKey ("anthropic", kFakeKey));
    ASSERT_TRUE (store.storeKey ("openai_compatible", kFakeKey + "-b", kLocalUrl));
    EXPECT_TRUE (store.hasKey ("anthropic"));
    EXPECT_TRUE (store.hasKey ("openai_compatible", kLocalUrl));

    // Re-storing one keeps the other.
    ASSERT_TRUE (store.storeKey ("anthropic", kFakeKey + "-c"));
    EXPECT_TRUE (store.hasKey ("openai_compatible", kLocalUrl));

    EXPECT_TRUE (store.deleteKey ("anthropic"));
    EXPECT_TRUE (store.hasKey ("openai_compatible", kLocalUrl));
    EXPECT_FALSE (store.hasKey ("anthropic"));
}

TEST (CopilotCredentialStore, CorruptFileIsMovedAsideNotDeleted)
{
    REQUIRE_DPAPI();
    TempDir t;
    CopilotCredentialStore store (t.dir);
    const auto corrupt = t.dir.getChildFile ("copilot-credentials.json.corrupt");

    ASSERT_TRUE (store.file().replaceWithText ("this is not json"));
    ASSERT_TRUE (store.storeKey ("anthropic", kFakeKey));
    EXPECT_TRUE (store.hasKey ("anthropic"));
    ASSERT_TRUE (corrupt.existsAsFile());
    EXPECT_EQ (readAll (corrupt), "this is not json");

    // A second unreadable file replaces the older .corrupt.
    ASSERT_TRUE (store.file().replaceWithText ("[1,2,3]"));
    ASSERT_TRUE (store.storeKey ("openai_compatible", kFakeKey, kLocalUrl));
    EXPECT_EQ (readAll (corrupt), "[1,2,3]");
    EXPECT_TRUE (store.hasKey ("openai_compatible", kLocalUrl));
    EXPECT_FALSE (store.hasKey ("anthropic"));
}

TEST (CopilotCredentialStore, InvalidInputWritesNothing)
{
    TempDir t;
    CopilotCredentialStore store (t.dir);

    EXPECT_FALSE (store.storeKey ("gemini", kFakeKey));
    EXPECT_FALSE (store.storeKey ("", kFakeKey));
    EXPECT_FALSE (store.storeKey ("anthropic", ""));
    EXPECT_FALSE (store.storeKey ("anthropic", "   \t "));
    EXPECT_FALSE (store.storeKey ("anthropic", "line1\nline2"));
    EXPECT_FALSE (store.storeKey ("anthropic", "line1\r\nline2"));
    EXPECT_FALSE (store.storeKey ("anthropic", juce::String::repeatedString ("k", 4097)));
    EXPECT_FALSE (store.file().existsAsFile());
    EXPECT_FALSE (store.hasKey ("gemini"));
    EXPECT_FALSE (store.deleteKey ("gemini"));
}

TEST (CopilotCredentialStore, MaximumKeyLengthIsAccepted)
{
    REQUIRE_DPAPI();
    TempDir t;
    CopilotCredentialStore store (t.dir);
    ASSERT_TRUE (store.storeKey ("anthropic", juce::String::repeatedString ("k", 4096)));
    EXPECT_TRUE (store.hasKey ("anthropic"));
}

TEST (CopilotCredentialStore, GarbageBlobIsNotAKey)
{
    REQUIRE_DPAPI();
    TempDir t;
    CopilotCredentialStore store (t.dir);
    ASSERT_TRUE (store.file().replaceWithText (
        R"({"version": 1, "profiles": {"anthropic": {"blob": "AAAA"}, "openai_compatible": {"blob": "not base64!"}}})"));
    EXPECT_FALSE (store.hasKey ("anthropic"));
    EXPECT_FALSE (store.hasKey ("openai_compatible", kLocalUrl));
}

//==============================================================================
// Origin binding of an OpenAI-compatible key.
TEST (CopilotBaseUrlOrigin, Vectors)
{
    EXPECT_EQ (copilotBaseUrlOrigin ("https://API.Example.com/v1"), "https://api.example.com");
    EXPECT_EQ (copilotBaseUrlOrigin ("https://api.example.com:443/v1/"), "https://api.example.com");
    EXPECT_EQ (copilotBaseUrlOrigin ("http://localhost:11434/v1"), "http://localhost:11434");
    EXPECT_EQ (copilotBaseUrlOrigin ("http://127.0.0.1:80"), "http://127.0.0.1");
    EXPECT_EQ (copilotBaseUrlOrigin ("http://[::1]:1234/v1"), "http://[::1]:1234");

    // Extra shapes: query/fragment dropped, non-default ports kept.
    EXPECT_EQ (copilotBaseUrlOrigin ("https://Host.Example:8443?x=1#f"), "https://host.example:8443");
    EXPECT_EQ (copilotBaseUrlOrigin ("https://host:80/v1"), "https://host:80");   // 80 is not https' default
    EXPECT_EQ (copilotBaseUrlOrigin ("http://[::1]/v1"), "http://[::1]");

    // Edge vectors, identical to OriginVectors in tests/test_copilot.py.
    EXPECT_EQ (copilotBaseUrlOrigin ("https://x:80"), "https://x:80");
    EXPECT_EQ (copilotBaseUrlOrigin ("http://x:443"), "http://x:443");
    EXPECT_EQ (copilotBaseUrlOrigin ("HTTP://X/"), "http://x");

    // Not a valid base URL -> no origin.
    for (const char* bad : { "", "ftp://a/b", "localhost:11434", "http://", "http://a:99999/v1",
                             "http://user@x/v1", "http://user:pw-not-real@x/v1", "http:///v1",
                             "http://x:abc/v1", "http://x:65536/v1", "http://x:80:90/v1" })
        EXPECT_TRUE (copilotBaseUrlOrigin (bad).isEmpty()) << bad;
}

TEST (CopilotCredentialStore, OpenAiKeyIsBoundToTheOriginItWasSavedFor)
{
    REQUIRE_DPAPI();
    TempDir t;
    CopilotCredentialStore store (t.dir);

    // A base URL is required and must be valid (no userinfo either).
    EXPECT_FALSE (store.storeKey ("openai_compatible", kFakeKey));
    EXPECT_FALSE (store.storeKey ("openai_compatible", kFakeKey, "https://user:pw@host/v1"));
    EXPECT_FALSE (store.storeKey ("openai_compatible", kFakeKey, "nonsense"));
    EXPECT_FALSE (store.file().existsAsFile());

    ASSERT_TRUE (store.storeKey ("openai_compatible", kFakeKey, "https://API.Example.com/v1"));
    juce::var parsed;
    ASSERT_TRUE (juce::JSON::parse (readAll (store.file()), parsed).wasOk());
    EXPECT_EQ ((int) parsed["version"], 1);
    EXPECT_EQ (parsed["profiles"]["openai_compatible"]["origin"].toString(), "https://api.example.com");

    using K = CopilotCredentialStore::KeyState;
    EXPECT_EQ (store.keyState ("openai_compatible", "https://api.example.com:443/other"), K::ready);
    EXPECT_TRUE (store.hasKey ("openai_compatible", "https://api.example.com/v2"));
    EXPECT_EQ (store.keyState ("openai_compatible", "https://evil.example.com/v1"), K::otherOrigin);
    EXPECT_EQ (store.keyState ("openai_compatible", "http://api.example.com/v1"), K::otherOrigin);
    EXPECT_EQ (store.keyState ("openai_compatible", ""), K::otherOrigin);
    EXPECT_FALSE (store.hasKey ("openai_compatible", "https://evil.example.com/v1"));

    // Claude's entry has no origin and ignores the address.
    ASSERT_TRUE (store.storeKey ("anthropic", kFakeKey));
    EXPECT_TRUE (store.hasKey ("anthropic", "https://whatever/"));
    EXPECT_TRUE (store.hasKey ("anthropic"));
}

TEST (CopilotCredentialStore, EntryWithoutOriginFromRelease150IsNeverUsed)
{
    REQUIRE_DPAPI();
    TempDir t;
    CopilotCredentialStore store (t.dir);
    ASSERT_TRUE (store.storeKey ("openai_compatible", kFakeKey, kLocalUrl));

    // Rewrite the file in the 1.5.0 shape: same DPAPI blob, no "origin".
    juce::var parsed;
    ASSERT_TRUE (juce::JSON::parse (readAll (store.file()), parsed).wasOk());
    auto* entry = parsed["profiles"]["openai_compatible"].getDynamicObject();
    ASSERT_NE (entry, nullptr);
    entry->removeProperty ("origin");
    ASSERT_TRUE (store.file().replaceWithText (juce::JSON::toString (parsed, false), false, false, "\n"));

    using K = CopilotCredentialStore::KeyState;
    EXPECT_EQ (store.keyState ("openai_compatible", kLocalUrl), K::otherOrigin);
    EXPECT_FALSE (store.hasKey ("openai_compatible", kLocalUrl));

    // The delete button still removes it, after which the status is plain "no key".
    EXPECT_TRUE (store.deleteKey ("openai_compatible"));
    EXPECT_EQ (store.keyState ("openai_compatible", kLocalUrl), K::none);
}

//==============================================================================
// Cross-check against the real Python reader/writer (tools/copilot/copilot.py).
namespace
{
juce::File copilotScript()
{
    return juce::File (juce::String (HANDSFREE_SOURCE_DIR)).getChildFile ("tools/copilot/copilot.py");
}

// Runs `python copilot.py <args...>`; returns the exit code, or -1 when python
// cannot be started. The caller sets HANDSFREE_COPILOT_CREDENTIALS_DIR first.
int runPython (const juce::StringArray& args)
{
    juce::StringArray argv { "python", copilotScript().getFullPathName() };
    argv.addArray (args);

    juce::ChildProcess child;
    if (! child.start (argv, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return -1;
    child.readAllProcessOutput();
    if (! child.waitForProcessToFinish (30000))
    {
        child.kill();
        return -1;
    }
    return (int) child.getExitCode();
}

bool pythonIsUsable()
{
    juce::ChildProcess child;
    if (! child.start (juce::StringArray { "python", "-c", "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 7)" }))
        return false;
    child.readAllProcessOutput();
    return child.waitForProcessToFinish (30000) && child.getExitCode() == 0;
}
}

TEST (CopilotCredentialStoreCrossCheck, PythonReaderSeesWhatCppWrote)
{
    REQUIRE_DPAPI();
    if (! copilotScript().existsAsFile())
        GTEST_SKIP() << "copilot.py not found";
    if (! pythonIsUsable())
        GTEST_SKIP() << "python (>= 3.10) cannot be started";

    TempDir t;
    ScopedEnv env (kEnvName, t.dir.getFullPathName());
    CopilotCredentialStore store (t.dir);

    EXPECT_EQ (runPython ({ "--has-key", "--provider", "anthropic" }), 1);

    ASSERT_TRUE (store.storeKey ("anthropic", kFakeKey));
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "anthropic" }), 0);
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "openai_compatible", "--base-url", "http://localhost:11434/v1" }), 1);

    // An OpenAI-compatible key stored by C++ for one origin: Python sees it for
    // that origin (path and host case do not matter) and for no other.
    ASSERT_TRUE (store.storeKey ("openai_compatible", kFakeKey, "http://localhost:11434/v1"));
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "openai_compatible",
                            "--base-url", "http://localhost:11434/v1" }), 0);
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "openai_compatible",
                            "--base-url", "http://LOCALHOST:11434/other" }), 0);
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "openai_compatible",
                            "--base-url", "http://localhost:11435/v1" }), 1);
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "openai_compatible",
                            "--base-url", "https://localhost:11434/v1" }), 1);
    // No --base-url is a usage error (exit 5), not "no key".
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "openai_compatible" }), 5);

    ASSERT_TRUE (store.deleteKey ("anthropic"));
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "anthropic" }), 1);
}

TEST (CopilotCredentialStoreCrossCheck, CppReaderSeesWhatPythonWrote)
{
    REQUIRE_DPAPI();
    if (! copilotScript().existsAsFile())
        GTEST_SKIP() << "copilot.py not found";
    if (! pythonIsUsable())
        GTEST_SKIP() << "python (>= 3.10) cannot be started";

   #if JUCE_WINDOWS
    TempDir t;
    ScopedEnv env (kEnvName, t.dir.getFullPathName());

    // --store-key reads the key from stdin; ChildProcess has no stdin, so a
    // throw-away .cmd redirects a file holding the fake key into it.
    const auto keyFile = t.dir.getChildFile ("fake-key.txt");
    ASSERT_TRUE (keyFile.replaceWithText (kFakeKey + "\r\n"));
    const auto cmd = t.dir.getChildFile ("store.cmd");
    ASSERT_TRUE (cmd.replaceWithText (
        "@echo off\r\npython \"" + copilotScript().getFullPathName()
        + "\" --store-key --provider openai_compatible --base-url http://localhost:11434/v1 < \"" + keyFile.getFullPathName() + "\"\r\n"));

    juce::ChildProcess child;
    ASSERT_TRUE (child.start (juce::StringArray { cmd.getFullPathName() },
                              juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr));
    child.readAllProcessOutput();
    ASSERT_TRUE (child.waitForProcessToFinish (30000));
    ASSERT_EQ ((int) child.getExitCode(), 0);

    CopilotCredentialStore store (t.dir);
    using K = CopilotCredentialStore::KeyState;
    EXPECT_EQ (store.keyState ("openai_compatible", "http://localhost:11434/v1"), K::ready);
    EXPECT_EQ (store.keyState ("openai_compatible", "http://LOCALHOST:11434/other"), K::ready);
    EXPECT_EQ (store.keyState ("openai_compatible", "http://localhost:11435/v1"), K::otherOrigin);
    EXPECT_FALSE (store.hasKey ("anthropic"));

    // And the C++ writer can add the other profile without losing Python's.
    ASSERT_TRUE (store.storeKey ("anthropic", kFakeKey));
    EXPECT_EQ (store.keyState ("openai_compatible", "http://localhost:11434/v1"), K::ready);
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "anthropic" }), 0);
   #endif
}
