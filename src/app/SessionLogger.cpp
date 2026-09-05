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

    // Minor 3: a failed start() below must not leave currentFile()/
    // directory_ reporting the PREVIOUS session -- clear both up front so
    // every early-return branch already reflects "no active file".
    file_      = {};
    directory_ = {};

    if (directory.existsAsFile())
        return false;
    if (! directory.createDirectory().wasOk())
        return false;

    const auto now = juce::Time::getCurrentTime();
    const auto stamp = "session-" + now.formatted ("%Y%m%d-%H%M%S")
                       + "-" + juce::String (now.getMilliseconds()).paddedLeft ('0', 3);

    // Minor 7: the ms suffix alone can collide (two sessions started inside
    // the same millisecond). Append _2, _3, ... before .jsonl on collision.
    // "_" (0x5F) sorts after "." (0x2E), so a suffixed name still sorts
    // lexically after its un-suffixed base -- pruneOldFiles()'s "lexical
    // order == chronological order" assumption keeps holding.
    juce::File candidate = directory.getChildFile (stamp + ".jsonl");
    for (int suffix = 2; candidate.existsAsFile(); ++suffix)
        candidate = directory.getChildFile (stamp + "_" + juce::String (suffix) + ".jsonl");

    directory_ = directory;
    file_      = candidate;

    stream_ = std::make_unique<juce::FileOutputStream> (file_);
    if (! stream_->openedOk())
    {
        stream_.reset();
        file_      = {};
        directory_ = {};
        return false;
    }

    t0_ = std::chrono::steady_clock::now();
    dropped_.store (0, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock (queueMutex_);
        pending_.clear();
        accepting_ = true;
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

    // Important 1 fix: close is ONE state transition under queueMutex_ --
    // clear accepting_ and take the final batch together, atomically. A
    // log() call still in flight either wins the lock first (pushes
    // successfully, picked up in finalBatch below) or loses it and finds
    // accepting_ already false (counts itself into dropped_ instead of
    // pushing into a queue nobody will ever drain again).
    std::deque<juce::String> finalBatch;
    {
        const std::lock_guard<std::mutex> lock (queueMutex_);
        accepting_ = false;
        finalBatch.swap (pending_);
    }
    for (const auto& line : finalBatch)
        writeLineNow (line);

    // Read dropped_ only now, after the final drain above: any log() call
    // that lost the race for queueMutex_ has, by this point, already
    // incremented it, so session_end reports the true final count.
    const auto droppedAtClose = dropped_.load (std::memory_order_relaxed);

    auto end = makeEvent ("session_end");
    end.getDynamicObject()->setProperty ("t", elapsedMs());
    end.getDynamicObject()->setProperty ("dropped_events", (juce::int64) droppedAtClose);
    writeLineNow (juce::JSON::toString (end, true));

    if (stream_ != nullptr)
        stream_->flush();
    stream_.reset();
}

void SessionLogger::log (const juce::var& event)
{
    // Fast, unlocked pre-filter: skip all work (clone, stamp, serialise) for
    // the common inactive cases -- never started, or long since stopped.
    // This is NOT the authoritative check; see the accepting_ recheck below.
    if (! isActive())
        return;

    auto* obj = event.getDynamicObject();
    if (obj == nullptr)
        return;

    // Important 2 fix: never mutate the caller's object. getDynamicObject()
    // is non-const even from a const juce::var, so setProperty("t") would
    // otherwise write into whatever DynamicObject the caller passed in --
    // visible to them afterwards, and a data race if they log the same var
    // from two threads. Clone first, stamp the clone, serialise the clone.
    juce::var stamped (new juce::DynamicObject (*obj));
    stamped.getDynamicObject()->setProperty ("t", elapsedMs());
    juce::String line = juce::JSON::toString (stamped, true);

    const std::lock_guard<std::mutex> lock (queueMutex_);

    // Important 1 fix: the authoritative check, under the same lock stop()
    // uses to close the session. A call that read isActive()==true above
    // but loses the race for this lock to a concurrent stop() lands here
    // instead of pushing into an already-abandoned queue.
    if (! accepting_)
    {
        dropped_.fetch_add (1, std::memory_order_relaxed);
        return;
    }
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
