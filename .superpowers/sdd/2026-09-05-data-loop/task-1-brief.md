### Task 1: `SessionLogger`

**Files:**
- Create: `src/app/SessionLogger.h`, `src/app/SessionLogger.cpp`
- Create: `tests/test_sessionlogger.cpp`
- Modify: `CMakeLists.txt:84-136` (`HANDSFREE_CORE_SOURCES`), `tests/CMakeLists.txt:19-43` (test source list)

**Interfaces:**
- Produces:
  ```cpp
  class SessionLogger
  {
  public:
      static constexpr int kMaxPendingLines  = 4096;   // D-4
      static constexpr int kFlushIntervalMs  = 1000;
      static constexpr int kDefaultKeepFiles = 30;

      explicit SessionLogger (int keepFiles = kDefaultKeepFiles);
      ~SessionLogger();                                   // stop()

      static juce::File defaultDirectory();               // %APPDATA%/AZSoundtech/HandsFree/logs
      static juce::var  makeEvent (const juce::String& name);   // {"ev": name}

      bool start (const juce::File& directory, const juce::var& sessionHeader);
      void stop();
      bool isActive() const;

      void log (const juce::var& event);                  // thread-safe, never blocks on I/O
      juce::File    currentFile() const;
      std::uint64_t droppedEvents() const;
  };
  ```
  Every line is one JSON object with `"t"` (ms since `start()`, steady clock) and `"ev"`. `start()` writes `session_start` (the header with `ev` and `t: 0` added) directly; `stop()` writes `session_end` with `dropped_events` directly. Neither goes through the deque.

- [ ] **Step 1: Write the failing tests** — create `tests/test_sessionlogger.cpp`:

```cpp
// SessionLogger tests -- lane D (data loop), spec §4 tests 1-4.
//
// A logger is a file plus a thread; every assertion here reads the FILE back
// through juce::JSON::parse, so what is checked is what a Python reader will
// see, not a member the logger could keep consistent with itself.

#include <gtest/gtest.h>

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
```

- [ ] **Step 2: Register the sources** — in `CMakeLists.txt` add to `HANDSFREE_CORE_SOURCES` right after the `MainComponent.h` line:

```cmake
    ${CMAKE_SOURCE_DIR}/src/app/SessionLogger.cpp
    ${CMAKE_SOURCE_DIR}/src/app/SessionLogger.h
```

and in `tests/CMakeLists.txt` add `test_sessionlogger.cpp` after `test_presetsfirstrun.cpp`.

- [ ] **Step 3: Run the test to verify it fails** — `cmake -B build -G "Visual Studio 18 2026" -A x64` then `cmake --build build --config Release`. Expected: compile error, `app/SessionLogger.h` not found.

- [ ] **Step 4: Write the header** — `src/app/SessionLogger.h`:

```cpp
// SessionLogger -- one JSONL file per app run (lane D, data-loop design §3.1).
//
// Every line is one JSON object: {"t": <ms since start>, "ev": "<name>", ...}.
// Producers (detector threads, message thread) call log(); a writer thread
// drains a bounded deque to the file once a second. log() NEVER touches the
// file and NEVER blocks: over kMaxPendingLines the event is dropped and
// counted (D-4). session_start and session_end are written directly by
// start()/stop(), outside the deque, so the invariant
//     lines in file + droppedEvents() == log() calls + 2
// holds for every session.
//
// NOT for the audio thread: log() takes a mutex and allocates a String.
//
// Encoding: UTF-8, no BOM (repo rule 6). The file is opened for APPEND; a
// crash mid-session leaves every completed line readable.

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

class SessionLogger : private juce::Thread
{
public:
    static constexpr int kMaxPendingLines  = 4096;
    static constexpr int kFlushIntervalMs  = 1000;
    static constexpr int kDefaultKeepFiles = 30;

    explicit SessionLogger (int keepFiles = kDefaultKeepFiles);
    ~SessionLogger() override;

    // %APPDATA%/AZSoundtech/HandsFree/logs -- sibling of PresetManager's
    // presets/ directory, asked of JUCE the same way.
    static juce::File defaultDirectory();

    // {"ev": name} as a DynamicObject var, ready for setProperty().
    static juce::var makeEvent (const juce::String& name);

    // Creates `directory` if needed, opens session-YYYYMMDD-HHMMSS-mmm.jsonl,
    // writes session_start (sessionHeader with "ev" and "t" set), prunes
    // older session files down to keepFiles, starts the writer thread.
    // false: the directory could not be created or the file not opened; the
    // logger stays inactive and log() is a no-op. Message thread.
    bool start (const juce::File& directory, const juce::var& sessionHeader);

    // Drains what is pending, writes session_end, closes the file, joins the
    // thread. Idempotent. Message thread.
    void stop();

    bool isActive() const { return active_.load (std::memory_order_acquire); }

    // Any thread except audio. `event` must be a DynamicObject var; "t" is
    // stamped here. Inactive logger: no-op.
    void log (const juce::var& event);

    juce::File    currentFile() const;
    std::uint64_t droppedEvents() const { return dropped_.load (std::memory_order_relaxed); }

private:
    void run() override;
    void drainToFile();
    void writeLineNow (const juce::String& line);   // writer thread, or start()/stop()
    double elapsedMs() const;
    void pruneOldFiles();

    const int keepFiles_;
    std::atomic<bool> active_ { false };
    std::chrono::steady_clock::time_point t0_ {};

    // File state: touched by start()/stop() and the writer thread only, and
    // the two never overlap (start() runs before startThread(), stop() after
    // stopThread()).
    juce::File directory_;
    juce::File file_;
    std::unique_ptr<juce::FileOutputStream> stream_;

    mutable std::mutex queueMutex_;
    std::deque<juce::String> pending_;
    std::atomic<std::uint64_t> dropped_ { 0 };

    JUCE_DECLARE_NON_COPYABLE (SessionLogger)
};
```

- [ ] **Step 5: Write the implementation** — `src/app/SessionLogger.cpp`:

```cpp
#include "app/SessionLogger.h"

#include <algorithm>
#include <utility>

SessionLogger::SessionLogger (int keepFiles)
    : juce::Thread ("AZSessionLogger"),
      keepFiles_ (std::max (0, keepFiles))
{
}

SessionLogger::~SessionLogger()
{
    stop();
}

juce::File SessionLogger::defaultDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("AZSoundtech")
               .getChildFile ("HandsFree")
               .getChildFile ("logs");
}

juce::var SessionLogger::makeEvent (const juce::String& name)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("ev", name);
    return juce::var (obj);
}

double SessionLogger::elapsedMs() const
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0_).count();
}

bool SessionLogger::start (const juce::File& directory, const juce::var& sessionHeader)
{
    stop();

    if (directory.existsAsFile())
        return false;
    if (! directory.createDirectory().wasOk())
        return false;

    const auto now = juce::Time::getCurrentTime();
    const auto name = "session-" + now.formatted ("%Y%m%d-%H%M%S")
                      + "-" + juce::String (now.getMilliseconds()).paddedLeft ('0', 3)
                      + ".jsonl";
    directory_ = directory;
    file_      = directory.getChildFile (name);

    stream_ = std::make_unique<juce::FileOutputStream> (file_);
    if (! stream_->openedOk())
    {
        stream_.reset();
        file_ = {};
        return false;
    }

    t0_ = std::chrono::steady_clock::now();
    dropped_.store (0, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock (queueMutex_);
        pending_.clear();
    }

    juce::var header = sessionHeader;
    if (header.getDynamicObject() == nullptr)
        header = makeEvent ("session_start");
    header.getDynamicObject()->setProperty ("ev", "session_start");
    header.getDynamicObject()->setProperty ("t", 0);
    writeLineNow (juce::JSON::toString (header, true));
    stream_->flush();

    // Prune AFTER the new file is safely written (same principle as the
    // release script's drop-folder prune).
    pruneOldFiles();

    active_.store (true, std::memory_order_release);
    startThread();
    return true;
}

void SessionLogger::stop()
{
    if (! active_.exchange (false, std::memory_order_acq_rel))
    {
        // Never started, or already stopped -- but a thread may still be
        // joining from a previous stop(); be safe.
        if (isThreadRunning())
            stopThread (5000);
        return;
    }

    notify();
    stopThread (5000);
    drainToFile();

    auto end = makeEvent ("session_end");
    end.getDynamicObject()->setProperty ("t", elapsedMs());
    end.getDynamicObject()->setProperty ("dropped_events", (juce::int64) dropped_.load (std::memory_order_relaxed));
    writeLineNow (juce::JSON::toString (end, true));

    if (stream_ != nullptr)
        stream_->flush();
    stream_.reset();
}

void SessionLogger::log (const juce::var& event)
{
    if (! isActive())
        return;
    auto* obj = event.getDynamicObject();
    if (obj == nullptr)
        return;

    obj->setProperty ("t", elapsedMs());
    juce::String line = juce::JSON::toString (event, true);

    const std::lock_guard<std::mutex> lock (queueMutex_);
    if ((int) pending_.size() >= kMaxPendingLines)
    {
        dropped_.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    pending_.push_back (std::move (line));
}

juce::File SessionLogger::currentFile() const
{
    return file_;
}

void SessionLogger::run()
{
    while (! threadShouldExit())
    {
        wait (kFlushIntervalMs);
        drainToFile();
    }
}

void SessionLogger::drainToFile()
{
    std::deque<juce::String> batch;
    {
        const std::lock_guard<std::mutex> lock (queueMutex_);
        batch.swap (pending_);
    }
    if (batch.empty() || stream_ == nullptr)
        return;
    for (const auto& line : batch)
        writeLineNow (line);
    stream_->flush();
}

void SessionLogger::writeLineNow (const juce::String& line)
{
    if (stream_ == nullptr)
        return;
    stream_->write (line.toRawUTF8(), line.getNumBytesAsUTF8());
    stream_->write ("\n", 1);
}

void SessionLogger::pruneOldFiles()
{
    juce::Array<juce::File> files;
    directory_.findChildFiles (files, juce::File::findFiles, false, "session-*.jsonl");
    // Names embed the timestamp, so lexical order IS chronological order.
    std::sort (files.begin(), files.end(),
               [] (const juce::File& a, const juce::File& b) { return a.getFileName() > b.getFileName(); });

    int kept = 0;
    for (const auto& f : files)
    {
        if (f == file_)
            continue;   // never the file just opened
        if (kept + 1 < keepFiles_)   // the current file counts as one of keepFiles
        {
            ++kept;
            continue;
        }
        f.deleteFile();
    }
}
```

Note on `pruneOldFiles`: with `keepFiles_ == 3`, the current file plus the two newest others survive — total three, as the test asserts. `keepFiles_ == 0` deletes every older file and keeps only the current one (`-Keep 0` in the release script disables pruning; here 0 means "only the current session", which is the more useful reading for a logger — document this in the header comment if the reviewer asks).

- [ ] **Step 6: Build and run** — `cmake --build build --config Release` then `cd build && ctest -C Release --output-on-failure -R SessionLogger`. Expected: 4 tests pass.

- [ ] **Step 7: Full suite** — `ctest -C Release` from `build/`. Expected `100% tests passed` (406).

- [ ] **Step 8: Commit**

```bash
git add src/app/SessionLogger.h src/app/SessionLogger.cpp tests/test_sessionlogger.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(log): SessionLogger writes one JSONL file per run from its own thread"
```

---

