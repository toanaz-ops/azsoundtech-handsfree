#include "app/SidecarRunner.h"

#include <juce_events/juce_events.h>

#include <chrono>
#include <thread>

namespace
{
constexpr size_t kMaxOutputBytes = 8u * 1024u * 1024u;
constexpr int kStopThreadTimeoutMs = 5000;
}

juce::File defaultCopilotExecutable()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
        .getParentDirectory()
        .getChildFile ("copilot")
        .getChildFile ("HandsFreeCopilot")
        .getChildFile ("HandsFreeCopilot.exe");
}

//==============================================================================
CopilotRunResult ChildProcessBackend::run (const CopilotRequest& request)
{
    CopilotRunResult result;

    if (! request.executable.existsAsFile())
    {
        result.status = CopilotRunResult::Status::sidecarMissing;
        return result;
    }

    // One StringArray element per argument: the paths contain spaces, and
    // ChildProcess quotes each element itself.
    juce::StringArray args;
    args.add (request.executable.getFullPathName());
    args.add (request.sessionLog.getFullPathName());
    args.add ("--json");
    args.add ("--low-priority");

    juce::ChildProcess child;
    if (! child.start (args, juce::ChildProcess::wantStdOut))
    {
        result.status = CopilotRunResult::Status::failed;
        return result;
    }

    // Reading blocks until the child exits, so the timeout needs a watchdog
    // that kills the child from outside; the read then sees it gone and returns.
    std::atomic<bool> timedOut { false };
    std::atomic<bool> cancelled { false };
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        // A cancel() issued before this point is intentionally not honoured:
        // the runner checks its own generation before calling run().
        cancelRequested_ = false;
    }

    std::thread watchdog ([&]
    {
        std::unique_lock<std::mutex> lock (mutex_);
        cv_.wait_for (lock, std::chrono::milliseconds (juce::jmax (1, request.timeoutMs)),
                      [&] { return finished || cancelRequested_; });

        if (! finished)
        {
            cancelled = cancelRequested_;
            timedOut = ! cancelRequested_;
            child.kill();
        }
    });

    juce::MemoryOutputStream collected;
    char buffer[4096];
    bool tooLarge = false;
    for (;;)
    {
        const int n = child.readProcessOutput (buffer, (int) sizeof (buffer));
        if (n <= 0)
            break;
        collected.write (buffer, (size_t) n);
        if (collected.getDataSize() > kMaxOutputBytes)
        {
            tooLarge = true;
            child.kill();
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock (mutex_);
        finished = true;
    }
    cv_.notify_all();
    watchdog.join();

    child.waitForProcessToFinish (2000);

    result.exitCode = (int) child.getExitCode();
    result.stdoutText = juce::String::fromUTF8 ((const char*) collected.getData(),
                                                (int) collected.getDataSize());

    if (timedOut)
        result.status = CopilotRunResult::Status::timedOut;
    else if (cancelled || tooLarge || result.exitCode != 0)
        result.status = CopilotRunResult::Status::failed;
    else
        result.status = CopilotRunResult::Status::ok;

    return result;
}

void ChildProcessBackend::cancel()
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        cancelRequested_ = true;
    }
    cv_.notify_all();
}

//==============================================================================
class SidecarRunner::Worker : public juce::Thread
{
public:
    Worker (CopilotBackend& backend, Poster& poster, std::shared_ptr<Shared> shared)
        : juce::Thread ("CopilotSidecarRunner"), backend_ (backend), poster_ (poster),
          shared_ (std::move (shared))
    {
        startThread();
    }

    void submit (CopilotRequest request, std::function<void (CopilotRunResult)> onDone,
                 juce::uint64 generation)
    {
        {
            const juce::ScopedLock lock (lock_);
            request_ = std::move (request);
            onDone_ = std::move (onDone);
            generation_ = generation;
            pending_ = true;
        }
        notify();
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            wait (-1); // woken by notify(), including the one that follows signalThreadShouldExit()

            CopilotRequest request;
            std::function<void (CopilotRunResult)> onDone;
            juce::uint64 generation = 0;
            {
                const juce::ScopedLock lock (lock_);
                if (! pending_)
                    continue;
                pending_ = false;
                request = std::move (request_);
                onDone = std::move (onDone_);
                generation = generation_;
            }

            if (threadShouldExit() || shared_->generation.load() != generation)
            {
                shared_->busy = false;
                continue;
            }

            auto result = backend_.run (request);
            shared_->busy = false;

            if (threadShouldExit())
                return;

            poster_ ([shared = shared_, generation, onDone = std::move (onDone),
                      result = std::move (result)]() mutable
            {
                if (! shared->dead.load() && shared->generation.load() == generation && onDone)
                    onDone (std::move (result));
            });
        }
    }

    CopilotBackend& backend_;
    Poster& poster_;
    std::shared_ptr<Shared> shared_;

    juce::CriticalSection lock_;
    CopilotRequest request_;
    std::function<void (CopilotRunResult)> onDone_;
    juce::uint64 generation_ = 0;
    bool pending_ = false;
};

//==============================================================================
SidecarRunner::SidecarRunner (std::unique_ptr<CopilotBackend> backend, Poster poster)
    : backend_ (std::move (backend)), poster_ (std::move (poster))
{
    jassert (backend_ != nullptr);

    if (! poster_)
        poster_ = [] (std::function<void()> fn) { juce::MessageManager::callAsync (std::move (fn)); };

    worker_ = std::make_unique<Worker> (*backend_, poster_, shared_);
}

SidecarRunner::~SidecarRunner()
{
    // Order matters: mark dead first so any queued delivery is dropped, ask the
    // backend to stop the child, then stop the thread with a bounded wait.
    shared_->dead = true;
    ++shared_->generation;
    backend_->cancel();
    worker_->signalThreadShouldExit();
    worker_->notify();
    worker_->stopThread (kStopThreadTimeoutMs);
}

bool SidecarRunner::request (CopilotRequest req, std::function<void (CopilotRunResult)> onDone)
{
    bool expected = false;
    if (! shared_->busy.compare_exchange_strong (expected, true))
        return false;

    const auto generation = ++shared_->generation;
    worker_->submit (std::move (req), std::move (onDone), generation);
    return true;
}

void SidecarRunner::cancel()
{
    ++shared_->generation;
    backend_->cancel();
}

bool SidecarRunner::isBusy() const noexcept
{
    return shared_->busy.load();
}
