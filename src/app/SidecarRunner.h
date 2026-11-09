// SidecarRunner: runs the frozen copilot sidecar off the message thread.
//
// Spec: docs/superpowers/specs/2026-11-09-copilot-panel-design.md section 4.2.
//
// Seam: CopilotBackend is the injected, synchronous "run a process" step (same
// idea as LicenseManager's ActivationTransport). Production uses
// ChildProcessBackend; tests use a fake, and inject the poster because the
// headless suite never pumps the JUCE message loop.
//
// Cancellation: each request() gets its OWN CopilotCancelToken, created and
// published together with the new generation under one lock. cancel() and the
// destructor cancel the token of the run in flight, and a token stays
// cancelled, so a cancel that lands before the backend's run() has even started
// is still seen the moment run() looks. (The first version kept a flag inside
// the backend and reset it at the start of run(), which lost exactly that
// cancel and left the destructor waiting out its whole stopThread timeout.)
//
// Stale results: every request() and cancel() bumps a generation counter. A
// result is delivered only if its generation is still current and the runner is
// still alive, so onDone is never called after destruction.
//
// Supervision (lane L3a): the AI supervisor uses a SECOND SidecarRunner
// instance, never the one the ask-the-AI panel owns. A runner serves one
// request at a time and cancel() is per instance, so sharing one would let a
// supervision tick refuse (or cancel) a person's question and the other way
// round. A request with `supervise` set builds its argv with
// buildCopilotSuperviseArguments(); the ask and findings paths are unchanged.

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

// Supervision deadlines (lane L3a). ONE place: buildCopilotSuperviseArguments
// builds --timeout / --deadline-s from these and AiSupervisor's watchdog is
// checked against them. The sidecar gives up on a single provider request after
// kSuperviseRequestTimeoutS and on the whole call after kSuperviseDeadlineS.
constexpr int kSuperviseRequestTimeoutS = 30;
constexpr int kSuperviseDeadlineS = 90;

struct CopilotRequest
{
    juce::File executable;
    juce::File sessionLog;
    int timeoutMs = 10000;

    // "Ask the AI" mode (lane L2b). An empty `question` means findings mode and
    // the other three fields are ignored.
    juce::String question;
    juce::String provider; // "anthropic" | "openai_compatible"
    juce::String model;
    juce::String baseUrl;  // openai_compatible only

    // Supervision mode (lane L3a). Takes precedence over `question`. `provider`,
    // `model` and `baseUrl` follow the ask rules; `statePath` is the
    // supervisor-state file the sidecar reads.
    bool supervise = false;
    juce::File statePath;
};

/** Exit code of a run that was refused before any process started because the
    ask request was invalid (same meaning as the sidecar's bad-arguments code). */
constexpr int kCopilotInvalidRequestExitCode = 5;

/** Control characters and line breaks -> space, `"` -> `'`, whitespace runs
    collapsed, trimmed, trailing backslashes stripped (a trailing `\` before the
    closing quote breaks Windows argv quoting), capped at 2000 characters. */
juce::String sanitiseCopilotQuestion (const juce::String& question);

/** 1..128 characters from [A-Za-z0-9._:/-]. */
bool isValidCopilotModel (const juce::String& model);

/** http:// or https://, at most 512 characters, no whitespace, quote or control
    character, and no userinfo (user:pass@host) in the authority. */
bool isValidCopilotBaseUrl (const juce::String& url);

/** The sidecar argv (executable first). Findings mode: `<exe> <log> --json
    --low-priority`. Ask mode: `<exe> <log> --ask=<q> --json --low-priority
    --credentials-only --provider=<p> [--model=<m>] [--base-url=<u>] --timeout=100`
    (value flags are one "=" element; --credentials-only makes the sidecar ignore
    API keys from environment variables). EMPTY when an
    ask request is invalid (empty sanitised question, unknown provider, invalid
    model, openai_compatible without a valid model and base URL, a base URL
    given for anthropic). */
juce::StringArray buildCopilotArguments (const CopilotRequest& request);

/** The supervise argv (executable first): `<exe> <log> --supervise --state=<statePath>
    --json --low-priority --credentials-only --provider=<p> [--model=<m>]
    [--base-url=<u>] --timeout=50`. Same provider / model / base-URL validation as
    the ask builder; EMPTY when it fails, when `statePath` is empty, or when the
    path holds a quote or control character. buildCopilotArguments() calls this
    for a request with `supervise` set. */
juce::StringArray buildCopilotSuperviseArguments (const CopilotRequest& request);

struct CopilotRunResult
{
    enum class Status { ok, sidecarMissing, timedOut, failed };

    Status status = Status::failed;
    int exitCode = -1;
    juce::String stdoutText;
};

/** One per run. Sticky: once cancelled it stays cancelled. Thread-safe. */
class CopilotCancelToken
{
public:
    void cancel();
    bool isCancelled() const;

    /** Blocks until cancelled, `stopEarly()` is true (re-checked after every
        notify()), or `timeoutMs` passes. Returns isCancelled(). Bounded. */
    bool waitFor (int timeoutMs, const std::function<bool()>& stopEarly = {});

    /** Wakes a waitFor() so it re-evaluates its stopEarly predicate. */
    void notify();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool cancelled_ = false;
};

class CopilotBackend
{
public:
    virtual ~CopilotBackend() = default;

    /** Synchronous. Called from a background thread. A well-behaved backend
        returns within a bounded time once `cancel` is cancelled, including when
        it was cancelled before run() was entered. */
    virtual CopilotRunResult run (const CopilotRequest&, CopilotCancelToken& cancel) = 0;
};

/** `<directory of the running app exe>/copilot/HandsFreeCopilot/HandsFreeCopilot.exe` */
juce::File defaultCopilotExecutable();

/** Production backend: juce::ChildProcess with a watchdog for the timeout. */
class ChildProcessBackend : public CopilotBackend
{
public:
    CopilotRunResult run (const CopilotRequest&, CopilotCancelToken& cancel) override;
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

    /** Drops the pending result and cancels the run in flight. */
    void cancel();

    bool isBusy() const noexcept;

private:
    struct Shared
    {
        std::atomic<juce::uint64> generation { 0 };
        std::atomic<bool> dead { false };
        std::atomic<bool> busy { false };

        // Guards the pair (generation, token): they are published together so a
        // cancel() always reaches the token of the generation it supersedes.
        std::mutex tokenMutex;
        std::shared_ptr<CopilotCancelToken> token;
    };

    class Worker;

    void cancelCurrent();

    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    std::unique_ptr<CopilotBackend> backend_;
    Poster poster_;
    std::unique_ptr<Worker> worker_;
};
