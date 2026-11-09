// Lane L2a: SidecarRunner with a fake backend (spec 2026-11-09 section 4.2, 6).
//
// The headless suite does not pump the JUCE message loop, so the runner gets an
// injected poster that queues callbacks; tests drain it by hand.

#include <gtest/gtest.h>

#include "app/SidecarRunner.h"

#include <chrono>
#include <condition_variable>
#include <deque>
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

// Returns a canned result; optionally blocks until release() or cancel().
class FakeBackend : public CopilotBackend
{
public:
    explicit FakeBackend (CopilotRunResult r, bool block = false) : result_ (std::move (r)), block_ (block) {}

    CopilotRunResult run (const CopilotRequest&) override
    {
        std::unique_lock<std::mutex> l (m_);
        entered_ = true;
        cv_.notify_all();
        if (block_)
            cv_.wait_for (l, std::chrono::seconds (10), [&] { return released_; });
        return result_;
    }

    void cancel() override { release(); }

    void release()
    {
        std::lock_guard<std::mutex> l (m_);
        released_ = true;
        cv_.notify_all();
    }

    bool waitEntered()
    {
        std::unique_lock<std::mutex> l (m_);
        return cv_.wait_for (l, std::chrono::seconds (5), [&] { return entered_; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    CopilotRunResult result_;
    bool block_;
    bool entered_ = false;
    bool released_ = false;
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

    explicit Harness (CopilotRunResult r, bool block = false)
    {
        auto b = std::make_unique<FakeBackend> (std::move (r), block);
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

    h.runner->cancel();          // bumps generation; the fake finishes now
    h.backend->release();
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
    h.runner.reset(); // cancel() releases the fake; destructor joins with a bounded wait
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
    const auto r = backend.run (req);
    EXPECT_EQ (r.status, Status::sidecarMissing);
}
