// SessionLogger tests -- lane D (data loop), spec §4 tests 1-4.
//
// A logger is a file plus a thread; every assertion here reads the FILE back
// through juce::JSON::parse, so what is checked is what a Python reader will
// see, not a member the logger could keep consistent with itself.

#include <gtest/gtest.h>
#include <juce_events/juce_events.h>

#include "app/SessionLogger.h"

#include <chrono>
#include <cstdint>

namespace
{
juce::File freshTempDir (const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("az-handsfree-sessionlogger")
                   .getChildFile (name);
    dir.deleteRecursively();
    return dir;
}

juce::StringArray linesOf (const juce::File& file)
{
    juce::StringArray lines;
    lines.addLines (file.loadFileAsString());
    lines.removeEmptyStrings();
    return lines;
}

juce::var parsedLine (const juce::String& line)
{
    juce::var parsed;
    const auto outcome = juce::JSON::parse (line, parsed);
    EXPECT_TRUE (outcome.wasOk()) << line;
    return parsed;
}

juce::var header()
{
    auto h = SessionLogger::makeEvent ("session_start");
    h.getDynamicObject()->setProperty ("app_version", "test");
    return h;
}
} // namespace

// Spec test 1. Red if start() stops writing session_start first, stop() stops
// writing session_end last, or any line stops being one JSON object.
TEST (SessionLogger, StartWritesHeaderFirstStopWritesEndLastEveryLineParses)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshTempDir ("basic");

    SessionLogger logger;
    ASSERT_TRUE (logger.start (dir, header()));
    ASSERT_TRUE (logger.isActive());
    const auto file = logger.currentFile();
    ASSERT_TRUE (file.existsAsFile());

    auto ev = SessionLogger::makeEvent ("mode");
    ev.getDynamicObject()->setProperty ("mode", "auto");
    logger.log (ev);
    logger.stop();
    EXPECT_FALSE (logger.isActive());

    const auto lines = linesOf (file);
    ASSERT_EQ (lines.size(), 3);
    const auto first = parsedLine (lines[0]);
    const auto mid   = parsedLine (lines[1]);
    const auto last  = parsedLine (lines[2]);
    EXPECT_EQ (first["ev"].toString(), "session_start");
    EXPECT_EQ (first["app_version"].toString(), "test");
    EXPECT_EQ ((int) first["t"], 0);
    EXPECT_EQ (mid["ev"].toString(), "mode");
    EXPECT_TRUE (mid.hasProperty ("t"));
    EXPECT_EQ (last["ev"].toString(), "session_end");
    EXPECT_EQ ((int) last["dropped_events"], 0);
}

// Spec test 2. Red if log() ever blocks on the file, or if the deque stops
// being capped (dropped stays 0) or the accounting invariant breaks:
// lines in file + dropped == log() calls + 2.
TEST (SessionLogger, FiveThousandLogsNeverBlockAndTheAccountingBalances)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshTempDir ("burst");

    SessionLogger logger;
    ASSERT_TRUE (logger.start (dir, header()));

    constexpr int kCalls = 5000;
    double worstMs = 0.0, totalMs = 0.0;
    for (int i = 0; i < kCalls; ++i)
    {
        auto ev = SessionLogger::makeEvent ("tick");
        ev.getDynamicObject()->setProperty ("i", i);
        const auto before = std::chrono::steady_clock::now();
        logger.log (ev);
        const double ms = std::chrono::duration<double, std::milli> (
            std::chrono::steady_clock::now() - before).count();
        worstMs = std::max (worstMs, ms);
        totalMs += ms;
    }
    // Rough measure (spec): mean well under 1 ms, no single call anywhere
    // near a file write. 10 ms is 10x the spec's per-call figure, chosen so a
    // loaded CI box does not flake while a real block (fsync ~ tens of ms)
    // still fails.
    EXPECT_LT (totalMs / kCalls, 1.0);
    EXPECT_LT (worstMs, 10.0);

    const auto file = logger.currentFile();
    logger.stop();

    const auto dropped = logger.droppedEvents();
    EXPECT_GT (dropped, 0u) << "5000 synchronous log() calls must overflow a 4096 cap";
    const auto lines = linesOf (file);
    EXPECT_EQ ((std::uint64_t) lines.size() + dropped, (std::uint64_t) kCalls + 2);
    EXPECT_EQ (parsedLine (lines[lines.size() - 1])["ev"].toString(), "session_end");
    EXPECT_EQ ((std::uint64_t) (juce::int64) parsedLine (lines[lines.size() - 1])["dropped_events"], dropped);
}

// Spec test 3. Red if pruning stops running, prunes the file just opened, or
// keeps more than keepFiles.
TEST (SessionLogger, KeepFilesThreePrunesToTheNewestThree)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshTempDir ("prune");

    juce::StringArray names;
    for (int i = 0; i < 5; ++i)
    {
        SessionLogger logger (3);
        ASSERT_TRUE (logger.start (dir, header()));
        names.add (logger.currentFile().getFileName());
        logger.stop();
        juce::Thread::sleep (2);   // distinct millisecond suffix
    }

    juce::Array<juce::File> left;
    dir.findChildFiles (left, juce::File::findFiles, false, "session-*.jsonl");
    ASSERT_EQ (left.size(), 3);
    juce::StringArray leftNames;
    for (const auto& f : left)
        leftNames.add (f.getFileName());
    leftNames.sort (false);
    EXPECT_TRUE (leftNames.contains (names[2]));
    EXPECT_TRUE (leftNames.contains (names[3]));
    EXPECT_TRUE (leftNames.contains (names[4]));
    EXPECT_FALSE (leftNames.contains (names[0]));
    EXPECT_FALSE (leftNames.contains (names[1]));
}

// Spec test 4. Red if an un-creatable directory throws, crashes, or leaves
// the logger claiming to be active.
TEST (SessionLogger, UncreatableDirectoryMakesStartFalseAndLogANoOp)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    // A directory path UNDER a regular file cannot be created.
    const auto blocker = freshTempDir ("blocker-parent").getChildFile ("blocker.txt");
    blocker.getParentDirectory().createDirectory();
    ASSERT_TRUE (blocker.replaceWithText ("not a directory"));

    SessionLogger logger;
    EXPECT_FALSE (logger.start (blocker.getChildFile ("logs"), header()));
    EXPECT_FALSE (logger.isActive());
    EXPECT_FALSE (logger.currentFile().existsAsFile());
    logger.log (SessionLogger::makeEvent ("mode"));   // must not throw
    logger.stop();                                   // must not throw
    EXPECT_EQ (logger.droppedEvents(), 0u);
}
