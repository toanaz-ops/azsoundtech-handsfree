// Lane L2a: SidecarRunner with a fake backend (spec 2026-11-09 section 4.2, 6).
//
// The headless suite does not pump the JUCE message loop, so the runner gets an
// injected poster that queues callbacks; tests drain it by hand.

#include <gtest/gtest.h>

#include "app/SidecarRunner.h"
#include "app/CopilotFindings.h"

#include <chrono>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <thread>
#include <mutex>

namespace
{
using Status = CopilotRunResult::Status;

struct PostQueue
{
    std::mutex m;
    std::deque<std::function<void()>> q;

    void post (std::function<void()> fn)
    {
        std::lock_guard<std::mutex> l (m);
        q.push_back (std::move (fn));
    }

    // Waits (bounded) until at least `n` callbacks are queued.
    bool waitFor (size_t n, int timeoutMs = 5000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                std::lock_guard<std::mutex> l (m);
                if (q.size() >= n)
                    return true;
            }
            juce::Thread::sleep (2);
        }
        return false;
    }

    void drain()
    {
        std::deque<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> l (m);
            local.swap (q);
        }
        for (auto& f : local)
            f();
    }
};

// Returns a canned result; optionally blocks until the run's token is cancelled
// (or release()). `gate` holds run() at its entry until openGate(), which lets a
// test cancel the runner at the moment between "request accepted" and "run()
// looks at its token".
class FakeBackend : public CopilotBackend
{
public:
    explicit FakeBackend (CopilotRunResult r, bool block = false, bool gate = false)
        : result_ (std::move (r)), block_ (block), gated_ (gate) {}

    CopilotRunResult run (const CopilotRequest&, CopilotCancelToken& cancel) override
    {
        {
            std::unique_lock<std::mutex> l (m_);
            entered_ = true;
            cv_.notify_all();
            cv_.wait_for (l, std::chrono::seconds (10), [&] { return ! gated_; });
        }

        if (block_)
        {
            const auto t0 = std::chrono::steady_clock::now();
            // Sliced so release() (which does not touch the token) is noticed too.
            for (int i = 0; i < 500 && ! releasedFlag(); ++i)
                if (cancel.waitFor (20))
                    break;
            waitedMs_ = (int) std::chrono::duration_cast<std::chrono::milliseconds> (
                            std::chrono::steady_clock::now() - t0).count();
        }
        return result_;
    }

    void release()
    {
        std::lock_guard<std::mutex> l (m_);
        released_ = true;
        cv_.notify_all();
    }

    void openGate()
    {
        std::lock_guard<std::mutex> l (m_);
        gated_ = false;
        cv_.notify_all();
    }

    bool waitEntered()
    {
        std::unique_lock<std::mutex> l (m_);
        return cv_.wait_for (l, std::chrono::seconds (5), [&] { return entered_; });
    }

    int waitedMs() const { return waitedMs_; }

private:
    bool releasedFlag()
    {
        std::lock_guard<std::mutex> l (m_);
        return released_;
    }

    std::mutex m_;
    std::condition_variable cv_;
    CopilotRunResult result_;
    bool block_;
    bool gated_;
    bool entered_ = false;
    bool released_ = false;
    std::atomic<int> waitedMs_ { -1 };
};

CopilotRunResult make (Status s, int code, const char* out = "")
{
    CopilotRunResult r;
    r.status = s;
    r.exitCode = code;
    r.stdoutText = out;
    return r;
}

struct Harness
{
    PostQueue queue;
    FakeBackend* backend = nullptr; // owned by runner
    std::unique_ptr<SidecarRunner> runner;
    int calls = 0;
    CopilotRunResult last;

    explicit Harness (CopilotRunResult r, bool block = false, bool gate = false)
    {
        auto b = std::make_unique<FakeBackend> (std::move (r), block, gate);
        backend = b.get();
        runner = std::make_unique<SidecarRunner> (std::move (b),
                                                  [this] (std::function<void()> fn) { queue.post (std::move (fn)); });
    }

    bool go()
    {
        return runner->request ({}, [this] (CopilotRunResult r) { ++calls; last = std::move (r); });
    }
};
}

TEST (SidecarRunner, SuccessIsDeliveredOnce)
{
    Harness h (make (Status::ok, 0, "[]"));
    ASSERT_TRUE (h.go());
    ASSERT_TRUE (h.queue.waitFor (1));
    h.queue.drain();

    EXPECT_EQ (h.calls, 1);
    EXPECT_EQ (h.last.status, Status::ok);
    EXPECT_EQ (h.last.stdoutText, "[]");
}

TEST (SidecarRunner, ExitCodeTwoIsDeliveredAsFailedWithCode)
{
    Harness h (make (Status::failed, 2));
    ASSERT_TRUE (h.go());
    ASSERT_TRUE (h.queue.waitFor (1));
    h.queue.drain();

    EXPECT_EQ (h.calls, 1);
    EXPECT_EQ (h.last.status, Status::failed);
    EXPECT_EQ (h.last.exitCode, 2);
}

TEST (SidecarRunner, TimedOutAndMissingPassThrough)
{
    {
        Harness h (make (Status::timedOut, 0));
        ASSERT_TRUE (h.go());
        ASSERT_TRUE (h.queue.waitFor (1));
        h.queue.drain();
        EXPECT_EQ (h.last.status, Status::timedOut);
    }
    {
        Harness h (make (Status::sidecarMissing, -1));
        ASSERT_TRUE (h.go());
        ASSERT_TRUE (h.queue.waitFor (1));
        h.queue.drain();
        EXPECT_EQ (h.last.status, Status::sidecarMissing);
    }
}

TEST (SidecarRunner, SecondRequestWhileBusyIsRefused)
{
    Harness h (make (Status::ok, 0), true);
    ASSERT_TRUE (h.go());
    ASSERT_TRUE (h.backend->waitEntered());
    EXPECT_FALSE (h.go());

    h.backend->release();
    ASSERT_TRUE (h.queue.waitFor (1));
    h.queue.drain();
    EXPECT_EQ (h.calls, 1);
}

TEST (SidecarRunner, StaleGenerationIsDropped)
{
    Harness h (make (Status::ok, 0), true);
    ASSERT_TRUE (h.go());
    ASSERT_TRUE (h.backend->waitEntered());

    h.runner->cancel();          // bumps generation and cancels the token; the fake finishes now
    ASSERT_TRUE (h.queue.waitFor (1)); // the worker still posts; the callback must self-drop
    h.queue.drain();

    EXPECT_EQ (h.calls, 0);
}

TEST (SidecarRunner, DestroyWhileBackendBlockedDoesNotHangOrDeliver)
{
    Harness h (make (Status::ok, 0), true);
    ASSERT_TRUE (h.go());
    ASSERT_TRUE (h.backend->waitEntered());

    const auto start = std::chrono::steady_clock::now();
    h.runner.reset(); // the destructor cancels the run's token; it joins with a bounded wait
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - start).count();
    EXPECT_LT (ms, 4000);

    h.queue.drain(); // anything queued must not call onDone
    EXPECT_EQ (h.calls, 0);
}

TEST (SidecarRunner, ChildProcessBackendReportsMissingExecutable)
{
    ChildProcessBackend backend;
    CopilotRequest req;
    req.executable = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("handsfree-no-such-dir")
                         .getChildFile ("HandsFreeCopilot.exe");
    CopilotCancelToken token;
    const auto r = backend.run (req, token);
    EXPECT_EQ (r.status, Status::sidecarMissing);
}

// The lost-cancel race. The old backend reset its cancel flag at the START of
// run(), so a cancel() that landed after the request was accepted but before
// run() looked was forgotten: the run then waited out its whole timeout and the
// destructor reached its forced thread kill. The token is per run and sticky.
TEST (SidecarRunner, CancelBeforeRunLooksIsNotLost)
{
    Harness h (make (Status::ok, 0), true, true);
    ASSERT_TRUE (h.go());
    ASSERT_TRUE (h.backend->waitEntered()); // worker is inside run(), held at the gate

    h.runner->cancel();     // lands before run() has looked at its token
    h.backend->openGate();  // run() now looks: it must see the cancel at once

    // The fake blocks up to 10 s unless cancelled; the result is posted when it returns.
    ASSERT_TRUE (h.queue.waitFor (1, 3000));
    EXPECT_LT (h.backend->waitedMs(), 2000);
    h.queue.drain();
    EXPECT_EQ (h.calls, 0); // and the cancelled result is dropped
}

TEST (SidecarRunner, DestroyBeforeRunLooksEndsInBoundedTime)
{
    Harness h (make (Status::ok, 0), true, true);
    ASSERT_TRUE (h.go());
    ASSERT_TRUE (h.backend->waitEntered());

    // Destroying the runner while run() is still at its entry: the destructor's
    // stopThread must not have to wait out the fake's 10 s block.
    std::thread opener ([&] { juce::Thread::sleep (50); h.backend->openGate(); });
    const auto start = std::chrono::steady_clock::now();
    h.runner.reset();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - start).count();
    opener.join();
    EXPECT_LT (ms, 2000);
}

TEST (SidecarRunner, ChildProcessBackendDoesNotSpawnWhenAlreadyCancelled)
{
    ChildProcessBackend backend;
    CopilotCancelToken token;
    token.cancel();

    CopilotRequest req;
    // Any file that exists: it must never be launched.
    req.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    req.timeoutMs = 60000;

    const auto start = std::chrono::steady_clock::now();
    const auto r = backend.run (req, token);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ (r.status, Status::failed);
    EXPECT_LT (ms, 2000);
}

// Opt-in: runs the REAL frozen sidecar (juce::ChildProcess -> Python -> UTF-8
// stdout). Skipped when build/copilot/HandsFreeCopilot has not been built, which
// is the case on CI machines.
namespace
{
juce::File realSidecarExe()
{
    return juce::File (juce::String (HANDSFREE_SOURCE_DIR))
        .getChildFile ("build/copilot/HandsFreeCopilot/HandsFreeCopilot.exe");
}
}

TEST (ChildProcessBackendIntegration, RealSidecarReturnsFixtureFindings)
{
    const auto exe = realSidecarExe();
    if (! exe.existsAsFile())
        GTEST_SKIP() << "sidecar not built: " << exe.getFullPathName().toStdString();

    CopilotRequest req;
    req.executable = exe;
    req.sessionLog = juce::File (juce::String (HANDSFREE_SOURCE_DIR))
                         .getChildFile ("tests/fixtures/session-sample.jsonl");
    ASSERT_TRUE (req.sessionLog.existsAsFile());
    req.timeoutMs = 60000;

    ChildProcessBackend backend;
    CopilotCancelToken token;
    const auto r = backend.run (req, token);

    ASSERT_EQ (r.status, Status::ok);
    EXPECT_EQ (r.exitCode, 0);

    const auto findings = parseCopilotFindings (r.stdoutText);
    ASSERT_TRUE (findings.has_value());
    ASSERT_EQ (findings->size(), 3u);
    EXPECT_EQ ((*findings)[0].severity, "alert");
    // "ch\u1EA1m tr\u1EA7n" as escaped UTF-8 bytes (MSVC here builds without /utf-8).
    const auto& title = (*findings)[0].title;
    EXPECT_TRUE (title.contains (juce::String::fromUTF8 ("ch\xE1\xBA\xA1m tr\xE1\xBA\xA7n"))) << title;
    EXPECT_TRUE (title.contains ("slot 01")) << title;
}

TEST (ChildProcessBackendIntegration, RealSidecarMissingLogExitsTwo)
{
    const auto exe = realSidecarExe();
    if (! exe.existsAsFile())
        GTEST_SKIP() << "sidecar not built: " << exe.getFullPathName().toStdString();

    CopilotRequest req;
    req.executable = exe;
    req.sessionLog = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("handsfree-no-such-dir/no-such-session.jsonl");
    req.timeoutMs = 60000;

    ChildProcessBackend backend;
    CopilotCancelToken token;
    const auto r = backend.run (req, token);

    EXPECT_EQ (r.status, Status::failed);
    EXPECT_EQ (r.exitCode, 2);
}
