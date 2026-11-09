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
void CopilotCancelToken::cancel()
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        cancelled_ = true;
    }
    cv_.notify_all();
}

bool CopilotCancelToken::isCancelled() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return cancelled_;
}

bool CopilotCancelToken::waitFor (int timeoutMs, const std::function<bool()>& stopEarly)
{
    std::unique_lock<std::mutex> lock (mutex_);
    cv_.wait_for (lock, std::chrono::milliseconds (juce::jmax (0, timeoutMs)),
                  [&] { return cancelled_ || (stopEarly && stopEarly()); });
    return cancelled_;
}

void CopilotCancelToken::notify()
{
    { std::lock_guard<std::mutex> lock (mutex_); } // orders against a waiter's predicate check
    cv_.notify_all();
}

//==============================================================================
CopilotRunResult ChildProcessBackend::run (const CopilotRequest& request, CopilotCancelToken& cancel)
{
    CopilotRunResult result;

    if (! request.executable.existsAsFile())
    {
        result.status = CopilotRunResult::Status::sidecarMissing;
        return result;
    }

    // Cancelled before the child even exists: do not spawn it.
    if (cancel.isCancelled())
    {
        result.status = CopilotRunResult::Status::failed;
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
    std::atomic<bool> finished { false };

    std::thread watchdog ([&]
    {
        // A token cancelled before this point is seen at once: it is sticky.
        const bool wasCancelled = cancel.waitFor (juce::jmax (1, request.timeoutMs),
                                                  [&] { return finished.load(); });
        if (! finished)
        {
            cancelled = wasCancelled;
            timedOut = ! wasCancelled;
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

    finished = true;
    cancel.notify();
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
                 juce::uint64 generation, std::shared_ptr<CopilotCancelToken> token)
    {
        {
            const juce::ScopedLock lock (lock_);
            request_ = std::move (request);
            onDone_ = std::move (onDone);
            generation_ = generation;
            token_ = std::move (token);
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
            std::shared_ptr<CopilotCancelToken> token;
            {
                const juce::ScopedLock lock (lock_);
                if (! pending_)
                    continue;
                pending_ = false;
                request = std::move (request_);
                onDone = std::move (onDone_);
                generation = generation_;
                token = std::move (token_);
            }

            if (threadShouldExit() || shared_->generation.load() != generation)
            {
                shared_->busy = false;
                continue;
            }

            auto result = backend_.run (request, *token);
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
    std::shared_ptr<CopilotCancelToken> token_;
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
    // Order matters: mark dead first so any queued delivery is dropped, cancel
    // the run in flight (sticky, so even a run() not yet entered sees it), then
    // stop the thread with a bounded wait.
    shared_->dead = true;
    cancelCurrent();
    worker_->signalThreadShouldExit();
    worker_->notify();
    worker_->stopThread (kStopThreadTimeoutMs);
}

bool SidecarRunner::request (CopilotRequest req, std::function<void (CopilotRunResult)> onDone)
{
    bool expected = false;
    if (! shared_->busy.compare_exchange_strong (expected, true))
        return false;

    auto token = std::make_shared<CopilotCancelToken>();
    juce::uint64 generation = 0;
    {
        std::lock_guard<std::mutex> lock (shared_->tokenMutex);
        generation = ++shared_->generation;
        shared_->token = token;
    }
    worker_->submit (std::move (req), std::move (onDone), generation, std::move (token));
    return true;
}

void SidecarRunner::cancel()
{
    cancelCurrent();
}

void SidecarRunner::cancelCurrent()
{
    std::shared_ptr<CopilotCancelToken> token;
    {
        std::lock_guard<std::mutex> lock (shared_->tokenMutex);
        ++shared_->generation;
        token = shared_->token;
    }
    if (token != nullptr)
        token->cancel();
}

bool SidecarRunner::isBusy() const noexcept
{
    return shared_->busy.load();
}
