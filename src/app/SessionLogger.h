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

    // Any thread except audio. `event` must be a DynamicObject var; a clone
    // is stamped with "t" -- the caller's own object is never mutated, so
    // the same var is safe to log again or read afterwards from any thread.
    // Inactive logger: no-op.
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
    // Authoritative "still open for business" flag, read/written ONLY under
    // queueMutex_. active_ (above) is a fast, unlocked pre-filter log() uses
    // to skip work when the logger has obviously never started or has long
    // since stopped; accepting_ is the actual gate a log() call re-checks
    // once it holds the same lock stop() uses to close the session, so a
    // call that raced stop() is always resolved -- pushed, or counted into
    // dropped_ -- never silently lost (review round 1, Important 1).
    bool accepting_ { false };
    std::atomic<std::uint64_t> dropped_ { 0 };

    JUCE_DECLARE_NON_COPYABLE (SessionLogger)
};
