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
    ASSERT_TRUE (store.storeKey ("openai_compatible", kFakeKey + "-b"));
    EXPECT_TRUE (store.hasKey ("anthropic"));
    EXPECT_TRUE (store.hasKey ("openai_compatible"));

    // Re-storing one keeps the other.
    ASSERT_TRUE (store.storeKey ("anthropic", kFakeKey + "-c"));
    EXPECT_TRUE (store.hasKey ("openai_compatible"));

    EXPECT_TRUE (store.deleteKey ("anthropic"));
    EXPECT_TRUE (store.hasKey ("openai_compatible"));
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
    ASSERT_TRUE (store.storeKey ("openai_compatible", kFakeKey));
    EXPECT_EQ (readAll (corrupt), "[1,2,3]");
    EXPECT_TRUE (store.hasKey ("openai_compatible"));
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
    EXPECT_FALSE (store.hasKey ("openai_compatible"));
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
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "openai_compatible" }), 1);

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
        + "\" --store-key --provider openai_compatible < \"" + keyFile.getFullPathName() + "\"\r\n"));

    juce::ChildProcess child;
    ASSERT_TRUE (child.start (juce::StringArray { cmd.getFullPathName() },
                              juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr));
    child.readAllProcessOutput();
    ASSERT_TRUE (child.waitForProcessToFinish (30000));
    ASSERT_EQ ((int) child.getExitCode(), 0);

    CopilotCredentialStore store (t.dir);
    EXPECT_TRUE (store.hasKey ("openai_compatible"));
    EXPECT_FALSE (store.hasKey ("anthropic"));

    // And the C++ writer can add the other profile without losing Python's.
    ASSERT_TRUE (store.storeKey ("anthropic", kFakeKey));
    EXPECT_TRUE (store.hasKey ("openai_compatible"));
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "openai_compatible" }), 0);
    EXPECT_EQ (runPython ({ "--has-key", "--provider", "anthropic" }), 0);
   #endif
}
