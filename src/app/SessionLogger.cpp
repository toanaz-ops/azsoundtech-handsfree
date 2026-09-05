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
