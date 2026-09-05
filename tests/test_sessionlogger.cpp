// SessionLogger tests -- lane D (data loop), spec §4 tests 1-4.
//
// A logger is a file plus a thread; every assertion here reads the FILE back
// through juce::JSON::parse, so what is checked is what a Python reader will
// see, not a member the logger could keep consistent with itself.

#include <gtest/gtest.h>
#include <juce_events/juce_events.h>

#include "app/SessionLogger.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

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

// ---------------------------------------------------------------------
// Review round 1 fixes -- see task-1-review.md.
// ---------------------------------------------------------------------

// Regression for review Important 1. Red if stop() reads/clears state
// without doing so as ONE transition under queueMutex_ together with the
// final drain -- i.e. if a log() call that takes the lock right after
// drainToFile() swapped the deque out is allowed to push into the abandoned
// pending_ instead of being counted into dropped_. A deterministic repro is
// hard (the review's own words): several producer threads hammer log() --
// each gated on isActive() so "calls" only counts attempts log() itself
// must resolve (write or drop), never silently lose -- while stop() runs
// concurrently; more threads means more of them are genuinely queued on
// queueMutex_ at the instant stop() takes it, which is what actually
// exercises the race (a single producer thread almost never catches it).
TEST (SessionLogger, LogRacingStopNeverLosesALine)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    constexpr int kProducers = 8;

    for (int attempt = 0; attempt < 20; ++attempt)
    {
        const auto dir = freshTempDir ("race-" + juce::String (attempt));
        SessionLogger logger;
        ASSERT_TRUE (logger.start (dir, header()));

        std::atomic<std::uint64_t> calls { 0 };
        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p)
        {
            producers.emplace_back ([&]
            {
                while (logger.isActive())
                {
                    logger.log (SessionLogger::makeEvent ("tick"));
                    calls.fetch_add (1, std::memory_order_relaxed);
                }
            });
        }

        logger.stop();
        for (auto& t : producers)
            t.join();

        const auto lines = linesOf (logger.currentFile());
        const auto dropped = logger.droppedEvents();
        EXPECT_EQ ((std::uint64_t) lines.size() + dropped, calls.load() + 2)
            << "attempt " << attempt;
    }
}

// Regression for review Important 2. Red if log() stamps "t" onto the
// caller's own DynamicObject instead of a clone -- the caller's var would
// then carry a "t" it never set, and logging the same var twice would
// silently reuse (or race on) the first call's timestamp storage.
TEST (SessionLogger, LogDoesNotMutateTheCallersVar)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshTempDir ("no-mutate");

    SessionLogger logger;
    ASSERT_TRUE (logger.start (dir, header()));

    auto ev = SessionLogger::makeEvent ("tick");
    ASSERT_FALSE (ev.hasProperty ("t"));
    logger.log (ev);
    EXPECT_FALSE (ev.hasProperty ("t")) << "log() must stamp a clone, not the caller's var";

    logger.log (ev);   // same var again -- must not throw or collide
    logger.stop();

    const auto lines = linesOf (logger.currentFile());
    ASSERT_EQ (lines.size(), 4);   // session_start, tick, tick, session_end
    EXPECT_TRUE (parsedLine (lines[1]).hasProperty ("t"));
    EXPECT_TRUE (parsedLine (lines[2]).hasProperty ("t"));
}

// Minor 8. Red if a second stop() call re-drains, writes a second
// session_end, or crashes joining an already-joined thread.
TEST (SessionLogger, StopCalledTwiceIsIdempotent)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshTempDir ("stop-twice");

    SessionLogger logger;
    ASSERT_TRUE (logger.start (dir, header()));
    logger.log (SessionLogger::makeEvent ("tick"));
    logger.stop();
    EXPECT_FALSE (logger.isActive());

    const auto linesAfterFirstStop = linesOf (logger.currentFile());
    logger.stop();   // must not throw, must not change the file
    EXPECT_FALSE (logger.isActive());

    const auto linesAfterSecondStop = linesOf (logger.currentFile());
    EXPECT_EQ (linesAfterFirstStop.size(), linesAfterSecondStop.size());
    ASSERT_EQ (linesAfterSecondStop.size(), 3);
    EXPECT_EQ (parsedLine (linesAfterSecondStop[2])["ev"].toString(), "session_end");
}

// Minor 8. The brief's header says start() calls stop() first -- a second
// start() closes the first session (writing ITS session_end) before opening
// a new file. Red if start() does not close the previous session first, or
// if Minor 7's collision guard is removed and the second file collides with
// (overwrites) the first.
TEST (SessionLogger, StartCalledTwiceClosesFirstSessionAndOpensASecondFile)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshTempDir ("start-twice");

    SessionLogger logger;
    ASSERT_TRUE (logger.start (dir, header()));
    const auto firstFile = logger.currentFile();
    logger.log (SessionLogger::makeEvent ("tick"));

    ASSERT_TRUE (logger.start (dir, header()));
    const auto secondFile = logger.currentFile();
    EXPECT_NE (firstFile.getFullPathName(), secondFile.getFullPathName());
    EXPECT_TRUE (logger.isActive());

    logger.stop();

    ASSERT_TRUE (firstFile.existsAsFile());
    const auto firstLines = linesOf (firstFile);
    ASSERT_EQ (firstLines.size(), 3);   // start, tick, end -- closed by the second start()
    EXPECT_EQ (parsedLine (firstLines[2])["ev"].toString(), "session_end");

    ASSERT_TRUE (secondFile.existsAsFile());
    const auto secondLines = linesOf (secondFile);
    ASSERT_EQ (secondLines.size(), 2);   // start, end -- no events logged in this session
}

// Minor 8. Red if log() after a clean, already-completed stop() counts
// itself into droppedEvents() -- the logger is inactive at that point, so
// nothing was "dropped"; it is simply a no-op.
TEST (SessionLogger, LogAfterStopIsANoOpAndNotCounted)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshTempDir ("log-after-stop");

    SessionLogger logger;
    ASSERT_TRUE (logger.start (dir, header()));
    logger.stop();

    const auto droppedBefore = logger.droppedEvents();
    const auto linesBefore = linesOf (logger.currentFile());

    logger.log (SessionLogger::makeEvent ("tick"));   // must be a true no-op

    EXPECT_EQ (logger.droppedEvents(), droppedBefore);
    EXPECT_EQ (linesOf (logger.currentFile()).size(), linesBefore.size());
}

// Minor 8. Red if the destructor does not call stop() -- session_end would
// never be written when a logger simply goes out of scope.
TEST (SessionLogger, DestructionWithoutStopWritesSessionEnd)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshTempDir ("dtor");

    juce::File file;
    {
        SessionLogger logger;
        ASSERT_TRUE (logger.start (dir, header()));
        file = logger.currentFile();
        logger.log (SessionLogger::makeEvent ("tick"));
        // no explicit stop() -- destructor must close the session.
    }

    const auto lines = linesOf (file);
    ASSERT_EQ (lines.size(), 3);
    EXPECT_EQ (parsedLine (lines[2])["ev"].toString(), "session_end");
}
