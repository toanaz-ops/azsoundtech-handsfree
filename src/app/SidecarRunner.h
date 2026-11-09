// SidecarRunner: runs the frozen copilot sidecar off the message thread.
//
// Spec: docs/superpowers/specs/2026-11-09-copilot-panel-design.md section 4.2.
//
// Seam: CopilotBackend is the injected, synchronous "run a process" step (same
// idea as LicenseManager's ActivationTransport). Production uses
// ChildProcessBackend; tests use a fake, and inject the poster because the
// headless suite never pumps the JUCE message loop.
//
// Stale results: every request() and cancel() bumps a generation counter. A
// result is delivered only if its generation is still current and the runner is
// still alive, so onDone is never called after destruction.

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

struct CopilotRequest
{
    juce::File executable;
    juce::File sessionLog;
    int timeoutMs = 10000;
};

struct CopilotRunResult
{
    enum class Status { ok, sidecarMissing, timedOut, failed };

    Status status = Status::failed;
    int exitCode = -1;
    juce::String stdoutText;
};

class CopilotBackend
{
public:
    virtual ~CopilotBackend() = default;

    /** Synchronous. Called from a background thread. */
    virtual CopilotRunResult run (const CopilotRequest&) = 0;

    /** Asks a run in progress to stop soon. May be called from another thread. */
    virtual void cancel() {}
};

/** `<directory of the running app exe>/copilot/HandsFreeCopilot/HandsFreeCopilot.exe` */
juce::File defaultCopilotExecutable();

/** Production backend: juce::ChildProcess with a watchdog for the timeout. */
class ChildProcessBackend : public CopilotBackend
{
public:
    CopilotRunResult run (const CopilotRequest&) override;
    void cancel() override;

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool cancelRequested_ = false;
};

class SidecarRunner
{
public:
    using Poster = std::function<void (std::function<void()>)>;

    /** `poster` runs a callback on the message thread; the default is
        juce::MessageManager::callAsync. */
    explicit SidecarRunner (std::unique_ptr<CopilotBackend> backend = std::make_unique<ChildProcessBackend>(),
                            Poster poster = {});
    ~SidecarRunner();

    SidecarRunner (const SidecarRunner&) = delete;
    SidecarRunner& operator= (const SidecarRunner&) = delete;

    /** Returns at once. False (request ignored) while a run is in progress. */
    bool request (CopilotRequest request, std::function<void (CopilotRunResult)> onDone);

    /** Drops the pending result and asks the backend to stop. */
    void cancel();

    bool isBusy() const noexcept;

private:
    struct Shared
    {
        std::atomic<juce::uint64> generation { 0 };
        std::atomic<bool> dead { false };
        std::atomic<bool> busy { false };
    };

    class Worker;

    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    std::unique_ptr<CopilotBackend> backend_;
    Poster poster_;
    std::unique_ptr<Worker> worker_;
};
