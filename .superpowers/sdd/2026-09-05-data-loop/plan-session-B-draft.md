# Data Loop (Lane D) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every notch the app places or clears is written to a per-session JSONL log with the spectrum the scorer actually looked at, and the operator can stamp each live notch GOOD or FALSE from the ACTIVE NOTCHES table (FALSE also clears it).

**Architecture:** `NotchController` gains an event outbox (`NotchEvent`) that is written under `modelMutex_` and delivered to an `EventSink` **outside** the lock from the detector thread's `flushOutbox()`. `CandidateScorer` gains a `scoreCandidateDetailed()` overload exposing the score components and the exact history frame the rise axis compared against. A new `SessionLogger` (own writer thread, bounded deque, drop-and-count) writes JSONL to `%APPDATA%\AZSoundtech\HandsFree\logs\`. `MainComponent` owns the logger, wires every controller's sink into it, and turns the panel's verdict callback into a `verdict` log line plus `clearNotch(..., VerdictFalse)`. `NotchListPanel` grows a VERDICT column with two real child `TextButton`s per notch identity. A stdlib-only Python tool summarises a log.

**Tech Stack:** C++17, JUCE (juce_core `JSON`/`Thread`/`FileOutputStream`, juce_gui_basics), GoogleTest via ctest, CMake + Visual Studio 18 2026 generator, MSVC, Python 3 (stdlib only) for the summariser.

**Spec:** `docs/superpowers/specs/2026-09-05-data-loop-design.md` (read it first; decisions D-1..D-9 are binding). This plan is written against **main tip `9735a78`** (lane S already merged: `NotchListPanel` has the LANE column, `NotchController` is two-lane). Spec line numbers (`CandidateScorer.cpp:64-95`, `NotchController.cpp:41-46`, `NotchListPanel.cpp:90-106`) are pre-merge and are superseded by the ranges given per task below.

## Global Constraints

- **Audio path untouched. Expected level change: 0 dB.** Nothing in `AudioEngine`, `NotchChain`, `Biquad` or the audio callback changes. `NotchController` changes are bookkeeping around existing model writes; `scoreCandidateDetailed().score` is bit-exact with `scoreCandidate()`.
- **No audio is ever logged.** Only magnitudes (1025 floats, 3 significant figures), scores, tuning, slot config, mode, verdicts. No tester name, no upload.
- **The sink is never invoked while `modelMutex_` is held** (spec D-6). Events are queued under the lock and delivered by `flushEvents()` outside it.
- **`SessionLogger::log()` never blocks the caller** (spec D-4): bounded deque (default 4096), drop and count.
- **Never call `SessionLogger::log()` or the sink from the audio thread.** The sink runs on the detector thread (via `flushOutbox()`) or on whichever thread calls `stop()`.
- `JUCE_APPLICATION_VERSION_STRING` is defined only for the `HandsFree` target. `MainComponent` never references it (spec D-8); `main.cpp` pushes it in via `setAppVersion()`.
- `NotchListPanel` stays "display only": it never calls a controller. Commands go through `MainComponent` (spec §3.4).
- Button labels are plain ASCII `GOOD` / `FALSE` — no ✓ ✗ glyphs (mojibake lesson 2026-08-25). Build a `juce::TextButton` with the ONE-argument ctor and call `setButtonText()` explicitly (JUCE 9 trap: the two-argument ctor's second parameter is not the text — `memory/juce9-api-traps-2026-08-25.md`).
- Every new test states in a comment which production change turns it red (repo convention).
- UTF-8 without BOM on every file written (log files, docs). Any script that rewrites a repo file sets UTF-8 explicitly.
- Build commands (from the worktree root `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\peakiness-sweep-tool-c81129`, branch `feat/data-loop`):
  ```
  cmake -B build -G "Visual Studio 18 2026" -A x64        # only after a header or CMakeLists change
  cmake --build build --config Release
  cd build && ctest -C Release --output-on-failure -R <Filter>
  ```
  Full-suite gate before every commit that touches `src/`, `tools/` or `tests/`: `ctest -C Release` from `build/` must report `100% tests passed`. Baseline on this branch before Task 1: **402/402**.
- Commit with explicit paths. Never `git add -A`. Never commit `build/`, `shots/`, `external/asiosdk/`.
- Do not create `.superpowers/sdd/.gitignore` in a commit — the SDD scripts regenerate it; delete it before the final commit (`memory/sdd-workspace-gitignore-trap-2026-09-04.md`).

## Plan-level decisions (deviations from the spec, each justified)

| # | Decision | Why |
|---|---|---|
| P-1 | `SessionLogger::start (const juce::File& directory, const juce::var& header)` takes the directory at start time; `Config` keeps `keepFiles`, `maxQueued`, `flushIntervalMs`. | `MainComponent` must hold the logger **by value** (declared before `notchControllers_`) so the detector-thread sink never dereferences a pointer that the message thread might be creating. The directory is only known at `startAudio()`/test time. |
| P-2 | `SessionLogger` has `log (const juce::var&)` **and** `logLine (const juce::String& jsonObject)`. The logger prepends `"t"` itself by rewriting the leading `{`. `ctx` arrays are pre-formatted by `SessionLogger::magnitudesToJson()` with `%.3g`. | `juce::JSON` serialises a double with up to 18 decimal places (`serialiseDouble`, juce_String.cpp:2287), so a `var` holding 0.00123 can print as `0.001229999999999999`. Spec D-3 wants 3 significant figures and readable text. |
| P-3 | The writer thread wakes every `flushIntervalMs` (default 1000 ms) **or** on `stop()`; `log()` does **not** signal it. | Spec §3.1 says "thức mỗi 1 s hoặc khi stop()". This also makes the drop test deterministic (Task 2 test 2). |
| P-4 | `ClearReason::PartialApplyUnwind` is exercised through a **test-only** hook `setLaneRejectForTest (int lane)` that makes `setNotch` fail on one lane. | `setNotch` validates against `lanes_[0].detector.getSampleRate()` for every lane, so the spec's "make lane 1 fail via sample rate" cannot happen in production code. The unwind path is real; the trigger is not, and the test says so. |
| P-5 | `NotchListPanel::onVerdict` signature is `(int slot, int lane, int index, float hz, bool good, double ageMs)`. | The `verdict` event carries `age_ms` (spec §3.2) and only the panel knows the GUI-side age. |
| P-6 | The logger is started by `MainComponent::startAudio()` (production, `defaultLogDirectory()`) or by `startSessionLog (dir)` (tests). Never in the constructor. | 26 existing tests and the snapshot tool construct `MainComponent`; starting the logger in the ctor would write to — and **prune** — the developer's real `%APPDATA%` logs. |
| P-7 | A `device` event (same fields as `session_start` minus `app_version`/`os`) is logged from the after-restart hook. | The device, rate and buffer can change mid-session from the INTERFACE drawer; `session_start` alone would describe the wrong rig. |
| P-8 | The column-budget test's "narrowest sensible panel" rises from 360 px to `NotchListPanel::kMinUsefulWidth = 440` px, and a wiring test pins that `MainComponent` gives the table at least that at `kMinimumWidth`. | A 92 px VERDICT column leaves 0 px for HELD at 360 px. The real panel is ~510 px at the 1200 px minimum window; 360 was a historical figure, not a layout fact. |
| P-9 | `setEventSink()` may only be called while the detector thread is stopped (jassert). | The sink is a `std::function` read on the detector thread; guarding it with another mutex for a set-once value is complexity for nothing. `MainComponent` sets it in its constructor, before `start()`. |

## File Structure

| File | Responsibility after this plan |
|---|---|
| `src/dsp/CandidateScorer.h/.cpp` | `ScoreBreakdown`, `scoreCandidateDetailed()`; `scoreCandidate()` forwards to it |
| `src/app/SessionLogger.h/.cpp` (**new**) | JSONL session writer: own thread, bounded deque, prune to `keepFiles`, `session_start`/`session_end` written directly |
| `src/app/NotchController.h/.cpp` | `ClearReason`, `NotchEvent`, `EventSink`, `setEventSink`, `eventOutbox_`, `flushEvents()`, `clearNotch(..., reason)`, `setNotchInternal(..., ctx)`, `droppedEvents()`, test hooks |
| `src/gui/NotchListPanel.h/.cpp` | VERDICT column, per-identity `RowButtons`, `onVerdict`, verdict state painted after a click |
| `src/app/MainComponent.h/.cpp` | Owns `SessionLogger`, wires sinks, `onVerdict` → log + `clearNotch(VerdictFalse)`, `mode`/`tuning`/`device` events, `setAppVersion`, `startSessionLog`, `defaultLogDirectory` |
| `src/main.cpp` | `setAppVersion (JUCE_APPLICATION_VERSION_STRING)`; `getApplicationVersion()` stops returning a literal |
| `tools/snapshot.cpp` | `console-live.png` shows one unrated row, one GOOD row, one FALSE row |
| `tools/logstats.py`, `tools/test_logstats.py`, `tests/fixtures/session-sample.jsonl` (**new**) | Summariser + its unittest + fixture |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | `SessionLogger.cpp/.h` in `HANDSFREE_CORE_SOURCES`; `test_sessionlogger.cpp`; Python ctest with SKIP fallback |
| `tests/test_candidatescorer.cpp`, `tests/test_sessionlogger.cpp` (**new**), `tests/test_notchcontroller.cpp`, `tests/test_notchlistpanel.cpp`, `tests/test_gui_wiring.cpp` | Spec §4 tests 1–16 |
| `docs/KY-THUAT-CHONG-HU.md`, `docs/GIOI-THIEU.md`, `docs/release-notes/1.1.2-alpha.md`, roadmap status, `memory/` | Docs in the same commit as behaviour; tester note; roadmap row |

---

### Task 1: `CandidateScorer::scoreCandidateDetailed`

**Files:**
- Modify: `src/dsp/CandidateScorer.h:55-80` (public API), `src/dsp/CandidateScorer.cpp:36-125` (`scoreCandidate` body becomes `scoreCandidateDetailed`)
- Test: `tests/test_candidatescorer.cpp` (append)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  ```cpp
  struct CandidateScorer::ScoreBreakdown
  {
      float rawPeakiness = 0.0f;     // Candidate::peakiness, unclamped
      float pNorm = 0.0f, rNorm = 0.0f, mNorm = 0.0f;
      float penalty = 1.0f;          // 1.0 or kHarmonicPenalty
      float score = 0.0f;            // pNorm * rNorm * mNorm * penalty (bit-exact with scoreCandidate)
      const float* refFrame = nullptr;   // the history frame the rise axis compared against; nullptr when none
      double refAgeMs = 0.0;             // clockMs_ - that frame's timeMs; 0 when refFrame == nullptr
  };
  ScoreBreakdown scoreCandidateDetailed (const PeakinessAnalyzer::Candidate&, const float* magnitudes,
                                         const LockedFrequencyView&);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_candidatescorer.cpp` (inside the file, after the existing tests; the anonymous-namespace `Rig`, `makeToneInNoise`, `kSampleRate`, `kHop`, `kFrameMs` are already defined at the top of that file):

```cpp
//==============================================================================
// Lane D (data loop), spec §4 tests 5-6.

// Spec test 5. Red if scoreCandidateDetailed() computes its product in a
// different order (or from different inputs) than scoreCandidate(), or if
// scoreCandidate() stops forwarding to it.
TEST (CandidateScorerBreakdown, DetailedScoreIsBitExactWithTheScalarOverload)
{
    Rig rig;
    const auto signal = makeToneInNoise (1000.0, 0.8f, 0.05f, 4242u, (std::size_t) kHop * 80);

    int compared = 0;
    for (std::size_t offset = 0; offset + (std::size_t) kHop <= signal.size(); offset += (std::size_t) kHop)
    {
        ASSERT_EQ (rig.tap.write (signal.data() + offset, (std::size_t) kHop), (std::size_t) kHop);
        const auto spectrum = rig.detector.processLatestBlock (rig.tap);
        rig.scorer.beginBlock (kSampleRate);
        if (spectrum.magnitudes != nullptr)
        {
            const auto result = rig.analyzer.analyse (spectrum);
            const std::vector<double> locked { 500.0 };   // exercises the harmonic penalty branch too
            for (std::size_t i = 0; i < result.count; ++i)
            {
                const auto& cand = result.candidates[i];
                const CandidateScorer::LockedFrequencyView view { locked.data(), locked.size() };
                const float scalar = rig.scorer.scoreCandidate (cand, spectrum.magnitudes, view);
                const auto  b      = rig.scorer.scoreCandidateDetailed (cand, spectrum.magnitudes, view);
                EXPECT_EQ (scalar, b.score);                                       // bit-exact
                EXPECT_EQ (b.pNorm * b.rNorm * b.mNorm * b.penalty, b.score);      // same expression order
                EXPECT_FLOAT_EQ (b.rawPeakiness, cand.peakiness);
                EXPECT_TRUE (b.penalty == 1.0f || b.penalty == CandidateScorer::kHarmonicPenalty);
                ++compared;
            }
            rig.scorer.commitBlock (spectrum.magnitudes, kFrameMs);
        }
    }
    EXPECT_GT (compared, 0) << "rig produced no candidates -- test has no teeth";
}

// Spec test 6 (D-7). Red if refFrame/refAgeMs stop describing the frame the
// rise axis ACTUALLY used (newest frame at least 0.45 x riseReferenceMs old).
TEST (CandidateScorerBreakdown, RefFrameIsTheNewestFrameAtLeastMinAgeOld)
{
    CandidateScorer scorer;
    scorer.setRiseReferenceMs (250.0);   // min age = 112.5 ms
    scorer.beginBlock (kSampleRate);

    std::array<float, CandidateScorer::kBins> frame {};
    auto commitConstant = [&] (float value, double elapsedMs)
    {
        frame.fill (value);
        scorer.commitBlock (frame.data(), elapsedMs);
    };
    // Frame times: A@0, B@180, C@200, D@300. Ages at clock 300: A=300, B=120, C=100, D=0.
    commitConstant (1.0f, 0.0);
    commitConstant (2.0f, 180.0);
    commitConstant (3.0f, 20.0);
    commitConstant (4.0f, 100.0);

    PeakinessAnalyzer::Candidate cand;
    cand.bin         = 43;
    cand.frequencyHz = 43.0 * kSampleRate / Detector::kFftSize;
    cand.peakiness   = PeakinessAnalyzer::kDefaultThreshold * 5.0f;   // well over the gate

    frame.fill (8.0f);
    const auto b = scorer.scoreCandidateDetailed (cand, frame.data(), {});
    ASSERT_NE (b.refFrame, nullptr);
    EXPECT_DOUBLE_EQ (b.refAgeMs, 120.0);          // B, not C (100 < 112.5) and not A (older than B)
    EXPECT_FLOAT_EQ (b.refFrame[cand.bin], 2.0f);
}

// Red if a scorer with NO history reports a reference frame it never had.
TEST (CandidateScorerBreakdown, NoHistoryMeansNoRefFrame)
{
    CandidateScorer scorer;
    scorer.beginBlock (kSampleRate);
    PeakinessAnalyzer::Candidate cand;
    cand.bin = 43; cand.frequencyHz = 1000.0; cand.peakiness = 50.0f;
    std::array<float, CandidateScorer::kBins> frame {};
    frame.fill (1.0f);
    const auto b = scorer.scoreCandidateDetailed (cand, frame.data(), {});
    EXPECT_EQ (b.refFrame, nullptr);
    EXPECT_DOUBLE_EQ (b.refAgeMs, 0.0);
    EXPECT_FLOAT_EQ (b.rNorm, 1.0f);   // "everything is rising" on the first frames
}
```

Add `#include <array>` at the top of the test file if not present.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Release --target HandsFreeTests` — expected: **compile error** `scoreCandidateDetailed is not a member of CandidateScorer`. That is the RED state for a new API.

- [ ] **Step 3: Implement**

In `src/dsp/CandidateScorer.h`, after `struct LockedFrequencyView`, add:

```cpp
    // Lane D: the score with its axes exposed, and the exact history frame the
    // rise axis divided by. The logger records THAT frame as "ref" (spec D-7)
    // -- not a frame looked up by riseReferenceMs, which is not what was
    // compared (the scan below takes the newest frame at least 45 % of the
    // rise reference old, so with the 250 ms default the reference can be as
    // young as ~112 ms).
    struct ScoreBreakdown
    {
        float rawPeakiness = 0.0f;
        float pNorm = 0.0f, rNorm = 0.0f, mNorm = 0.0f;
        float penalty = 1.0f;
        float score = 0.0f;
        const float* refFrame = nullptr;   // points into history_; valid until the next commitBlock()
        double refAgeMs = 0.0;
    };

    // Pure read, like scoreCandidate(). scoreCandidate() is exactly
    // scoreCandidateDetailed(...).score -- the product is computed ONCE, here.
    ScoreBreakdown scoreCandidateDetailed (const PeakinessAnalyzer::Candidate& candidate,
                                           const float* magnitudes,
                                           const LockedFrequencyView& lockedFrequencies);
```

In `src/dsp/CandidateScorer.cpp`, rename the existing `scoreCandidate` body to `scoreCandidateDetailed` returning `ScoreBreakdown`, keeping every arithmetic line identical, and add the forwarding scalar overload:

```cpp
float CandidateScorer::scoreCandidate (const PeakinessAnalyzer::Candidate& candidate,
                                       const float* magnitudes,
                                       const LockedFrequencyView& lockedFrequencies)
{
    return scoreCandidateDetailed (candidate, magnitudes, lockedFrequencies).score;
}

CandidateScorer::ScoreBreakdown
CandidateScorer::scoreCandidateDetailed (const PeakinessAnalyzer::Candidate& candidate,
                                         const float* magnitudes,
                                         const LockedFrequencyView& lockedFrequencies)
{
    ScoreBreakdown out;
    out.rawPeakiness = candidate.peakiness;

    // (existing early-out, unchanged in meaning: score 0, no axes computed)
    if (candidate.peakiness <= PeakinessAnalyzer::kDefaultThreshold)
        return out;

    // ... existing pNorm block unchanged ...
    out.pNorm = pNorm;

    // ... existing rNorm block, with TWO additions inside the history scan:
    //     when `reference` is chosen:  out.refFrame = reference;
    //                                   out.refAgeMs = clockMs_ - history_[idx].timeMs;
    out.rNorm = rNorm;

    // ... existing mNorm block unchanged ...
    out.mNorm = static_cast<float> (mNorm);

    // ... existing penalty block unchanged ...
    out.penalty = penalty;

    // Same expression, same order, as the pre-lane-D return statement.
    out.score = pNorm * rNorm * static_cast<float> (mNorm) * penalty;
    return out;
}
```

Keep the comments that explain the axes. Do not touch `commitBlock`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --config Release --target HandsFreeTests && cd build && ctest -C Release --output-on-failure -R CandidateScorer`
Expected: all `CandidateScorer*` tests pass, including the three new ones.

- [ ] **Step 5: Full suite, then commit**

Run: `cd build && ctest -C Release` → `100% tests passed` (402 + 3 = 405).

```bash
git add src/dsp/CandidateScorer.h src/dsp/CandidateScorer.cpp tests/test_candidatescorer.cpp
git commit -m "feat(scorer): scoreCandidateDetailed exposes axes and the rise reference frame (lane D)"
```

---

### Task 2: `SessionLogger`

**Files:**
- Create: `src/app/SessionLogger.h`, `src/app/SessionLogger.cpp`
- Create: `tests/test_sessionlogger.cpp`
- Modify: `CMakeLists.txt` (`HANDSFREE_CORE_SOURCES`: add the two files), `tests/CMakeLists.txt` (add `test_sessionlogger.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  class SessionLogger
  {
  public:
      struct Config
      {
          int keepFiles       = 30;     // newest N session files survive start()
          int maxQueued       = 4096;   // deque cap; beyond it log() drops and counts
          int flushIntervalMs = 1000;   // writer wake period
      };
      explicit SessionLogger (Config c = {});
      ~SessionLogger();                                      // stop()

      bool start (const juce::File& directory, const juce::var& sessionHeader);   // false: dir/file unusable -> everything is a no-op
      void stop();                                           // idempotent; writes session_end, joins the thread
      bool isStarted() const;

      void log (const juce::var& event);                     // serialised with JSON::toString, no spacing
      void logLine (const juce::String& jsonObject);         // pre-serialised "{...}"; "t" is prepended by the logger

      juce::File currentFile() const;                        // juce::File() when not started
      std::uint64_t droppedEvents() const;

      static juce::String magnitudesToJson (const float* values, int count);   // "[1.23e-05,0.0456,...]" %.3g, non-finite -> 0
      static juce::String stampLine (const juce::String& jsonObject, std::int64_t tMs);   // exposed for tests
  };
  ```

- [ ] **Step 1: Write the failing tests**

Create `tests/test_sessionlogger.cpp`:

```cpp
// SessionLogger tests -- lane D (data loop), spec §4 tests 1-4.
//
// Each test uses its own temp directory under the system temp dir and deletes
// it at the end. No JUCE message loop is needed: SessionLogger owns a plain
// juce::Thread and juce::File/FileOutputStream work without one.

#include <gtest/gtest.h>

#include "app/SessionLogger.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace
{
struct TempDir
{
    juce::File dir;
    TempDir()
    {
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("hf-sessionlogger-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
    }
    ~TempDir() { dir.deleteRecursively(); }
};

juce::StringArray nonEmptyLines (const juce::File& f)
{
    juce::StringArray lines;
    f.readLines (lines);
    lines.removeEmptyStrings();
    return lines;
}

juce::var header()
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("app_version", "test");
    return juce::var (o);
}

juce::var event (const char* ev)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("ev", ev);
    return juce::var (o);
}
} // namespace

// Spec test 1. Red if session_start is not the first line, session_end not
// the last, any line fails JSON::parse, or "t" is missing from a line.
TEST (SessionLogger, StartWritesSessionStartFirstAndStopWritesSessionEndLast)
{
    TempDir tmp;
    SessionLogger logger;
    ASSERT_TRUE (logger.start (tmp.dir, header()));
    ASSERT_TRUE (logger.isStarted());
    logger.log (event ("mode"));
    logger.log (event ("tuning"));
    logger.logLine ("{\"ev\":\"notch_set\",\"hz\":1000}");
    logger.stop();
    EXPECT_FALSE (logger.isStarted());

    const auto lines = nonEmptyLines (logger.currentFile());
    ASSERT_EQ (lines.size(), 5);
    std::vector<juce::String> evs;
    for (const auto& line : lines)
    {
        const auto v = juce::JSON::parse (line);
        ASSERT_TRUE (v.isObject()) << line;
        EXPECT_TRUE (v.hasProperty ("t")) << line;
        EXPECT_TRUE (v.hasProperty ("ev")) << line;
        evs.push_back (v["ev"].toString());
    }
    EXPECT_EQ (evs[0], "session_start");
    EXPECT_EQ (evs[1], "mode");
    EXPECT_EQ (evs[2], "tuning");
    EXPECT_EQ (evs[3], "notch_set");
    EXPECT_EQ (evs[4], "session_end");
    EXPECT_EQ (juce::JSON::parse (lines[0])["app_version"].toString(), "test");
    EXPECT_EQ ((int) juce::JSON::parse (lines[4])["dropped_events"], 0);
    EXPECT_TRUE (logger.currentFile().getFileName().startsWith ("session-"));
    EXPECT_TRUE (logger.currentFile().hasFileExtension (".jsonl"));
}

// Spec test 2 (D-4). Red if log() blocks on I/O, if the deque grows without
// bound, or if dropped events are not counted. With the writer asleep for
// 60 s the drop count is exactly 5000 - maxQueued.
TEST (SessionLogger, FiveThousandLogsNeverBlockAndEveryEventIsAccountedFor)
{
    TempDir tmp;
    SessionLogger::Config cfg;
    cfg.maxQueued       = 4096;
    cfg.flushIntervalMs = 60000;
    SessionLogger logger (cfg);
    ASSERT_TRUE (logger.start (tmp.dir, header()));

    double worstMs = 0.0;
    for (int i = 0; i < 5000; ++i)
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        logger.log (event ("x"));
        worstMs = juce::jmax (worstMs, juce::Time::getMillisecondCounterHiRes() - t0);
    }
    EXPECT_LT (worstMs, 5.0) << "log() must never block the caller (spec says ~1 ms; 5 ms is the CI-safe bound)";
    EXPECT_EQ (logger.droppedEvents(), 5000u - 4096u);

    logger.stop();
    const auto lines = nonEmptyLines (logger.currentFile());
    EXPECT_EQ ((std::uint64_t) lines.size() + logger.droppedEvents(), 5000u + 2u);
}

// Spec test 3. Red if pruning keeps the wrong files, prunes BEFORE the new
// file is written, or forgets to prune at all.
TEST (SessionLogger, KeepFilesPrunesToTheNewestSessions)
{
    TempDir tmp;
    SessionLogger::Config cfg;
    cfg.keepFiles = 3;

    std::vector<juce::File> created;
    for (int i = 0; i < 5; ++i)
    {
        SessionLogger logger (cfg);
        ASSERT_TRUE (logger.start (tmp.dir, header()));
        created.push_back (logger.currentFile());
        logger.stop();
    }

    juce::Array<juce::File> remaining;
    tmp.dir.findChildFiles (remaining, juce::File::findFiles, false, "session-*.jsonl");
    ASSERT_EQ (remaining.size(), 3);
    for (int i = 2; i < 5; ++i)
        EXPECT_TRUE (created[(std::size_t) i].existsAsFile()) << created[(std::size_t) i].getFullPathName();
    for (int i = 0; i < 2; ++i)
        EXPECT_FALSE (created[(std::size_t) i].existsAsFile()) << created[(std::size_t) i].getFullPathName();
}

// Spec test 4. Red if an unwritable directory throws, crashes, or leaves the
// logger claiming it is started.
TEST (SessionLogger, UnwritableDirectoryMakesEverythingANoOp)
{
    TempDir tmp;
    const auto blocker = tmp.dir.getChildFile ("blocker");
    ASSERT_TRUE (blocker.replaceWithText ("not a directory"));
    const auto impossible = blocker.getChildFile ("logs");   // a child of a FILE

    SessionLogger logger;
    EXPECT_FALSE (logger.start (impossible, header()));
    EXPECT_FALSE (logger.isStarted());
    EXPECT_EQ (logger.currentFile(), juce::File());
    EXPECT_NO_THROW (logger.log (event ("mode")));
    EXPECT_NO_THROW (logger.logLine ("{\"ev\":\"x\"}"));
    EXPECT_NO_THROW (logger.stop());
    EXPECT_EQ (logger.droppedEvents(), 0u);
}

// Red if magnitudesToJson stops producing 3-significant-figure JSON numbers
// or lets a NaN/Inf through (JSON has no spelling for those).
TEST (SessionLogger, MagnitudesToJsonUsesThreeSignificantFiguresAndNoNonFinite)
{
    const float values[] = { 0.0f, 1.23456e-5f, 0.0456789f, 1234.5f, std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity() };
    const auto text = SessionLogger::magnitudesToJson (values, 6);
    EXPECT_EQ (text.toStdString(), "[0,1.23e-05,0.0457,1.23e+03,0,0]");
    const auto parsed = juce::JSON::parse (text);
    ASSERT_TRUE (parsed.isArray());
    EXPECT_EQ (parsed.size(), 6);
}

// Red if stampLine stops prepending "t" as the FIRST property, or mangles an
// empty object.
TEST (SessionLogger, StampLinePrependsTimeAsTheFirstProperty)
{
    EXPECT_EQ (SessionLogger::stampLine ("{\"ev\":\"mode\"}", 42).toStdString(), "{\"t\":42,\"ev\":\"mode\"}");
    EXPECT_EQ (SessionLogger::stampLine ("{}", 7).toStdString(), "{\"t\":7}");
    EXPECT_EQ (SessionLogger::stampLine ("  {\"a\":1}", 1).toStdString(), "{\"t\":1,\"a\":1}");
}
```

Add `#include <limits>` and `#include <cmath>` at the top.

Register the test in `tests/CMakeLists.txt`: add `test_sessionlogger.cpp` to the `add_executable(HandsFreeTests ...)` list (after `test_presetsfirstrun.cpp`). Add to `CMakeLists.txt` `HANDSFREE_CORE_SOURCES` (after the `NotchController.h` line):

```cmake
    ${CMAKE_SOURCE_DIR}/src/app/SessionLogger.cpp
    ${CMAKE_SOURCE_DIR}/src/app/SessionLogger.h
```

- [ ] **Step 2: Reconfigure and verify RED**

Run: `cmake -B build -G "Visual Studio 18 2026" -A x64 && cmake --build build --config Release --target HandsFreeTests`
Expected: fails — `SessionLogger.h` not found / `SessionLogger.cpp` missing from the source list. (CMake errors on the missing file: that IS the red state. Create empty files only if you need the configure step to pass first, then the build fails on the missing class.)

- [ ] **Step 3: Implement `SessionLogger`**

`src/app/SessionLogger.h`:

```cpp
// SessionLogger -- lane D (data loop). One JSONL file per app run under
// %APPDATA%\AZSoundtech\HandsFree\logs\, written by THIS class's own thread.
//
// Contract (spec 2026-09-05-data-loop-design.md §3.1):
//   * log()/logLine() are thread-safe and never block the caller: a bounded
//     deque under a mutex; past maxQueued the event is dropped and counted.
//   * Never call from the audio thread (mutex + allocation). Detector thread
//     and message thread are fine.
//   * session_start / session_end go straight to the file, bypassing the
//     deque, so "lines in file + droppedEvents == log() calls + 2" always
//     holds.
//   * Every line is one JSON object whose FIRST property is "t" (ms since
//     start, steady clock) and which carries "ev".
//   * After the new file is opened, older session-*.jsonl files are deleted
//     until keepFiles remain (prune AFTER the new file exists, like the
//     release script).
//   * UTF-8, no BOM.

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

class SessionLogger : private juce::Thread
{
public:
    struct Config
    {
        int keepFiles       = 30;
        int maxQueued       = 4096;
        int flushIntervalMs = 1000;
    };

    explicit SessionLogger (Config c = {});
    ~SessionLogger() override;

    bool start (const juce::File& directory, const juce::var& sessionHeader);
    void stop();
    bool isStarted() const { return started_.load (std::memory_order_acquire); }

    void log (const juce::var& event);
    void logLine (const juce::String& jsonObject);

    juce::File currentFile() const;
    std::uint64_t droppedEvents() const { return dropped_.load (std::memory_order_relaxed); }

    static juce::String magnitudesToJson (const float* values, int count);
    static juce::String stampLine (const juce::String& jsonObject, std::int64_t tMs);

private:
    void run() override;
    void drain();                                   // writer thread, or stop()
    void writeDirect (const juce::String& line);    // holds fileMutex_
    void pruneOldFiles (const juce::File& directory, const juce::File& keep);
    std::int64_t nowMs() const;

    Config config_;
    std::atomic<bool> started_ { false };
    double t0_ = 0.0;

    mutable std::mutex fileMutex_;                  // file_, stream_
    juce::File file_;
    std::unique_ptr<juce::FileOutputStream> stream_;

    std::mutex queueMutex_;
    std::deque<juce::String> queue_;
    std::atomic<std::uint64_t> dropped_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionLogger)
};
```

`src/app/SessionLogger.cpp`:

```cpp
#include "app/SessionLogger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

SessionLogger::SessionLogger (Config c)
    : juce::Thread ("AZSessionLogger"), config_ (c)
{
    config_.keepFiles       = std::max (1, config_.keepFiles);
    config_.maxQueued       = std::max (1, config_.maxQueued);
    config_.flushIntervalMs = std::max (10, config_.flushIntervalMs);
}

SessionLogger::~SessionLogger()
{
    stop();
}

std::int64_t SessionLogger::nowMs() const
{
    return (std::int64_t) std::llround (juce::Time::getMillisecondCounterHiRes() - t0_);
}

juce::String SessionLogger::stampLine (const juce::String& jsonObject, std::int64_t tMs)
{
    // The line must be a JSON object. "t" goes FIRST so a reader that only
    // looks at the head of a line still finds the timestamp.
    const auto body = jsonObject.trim();
    jassert (body.startsWithChar ('{') && body.endsWithChar ('}'));
    const auto rest = body.substring (1).trimStart();
    const juce::String head = "{\"t\":" + juce::String (tMs);
    if (rest.startsWithChar ('}'))
        return head + "}";
    return head + "," + rest;
}

juce::String SessionLogger::magnitudesToJson (const float* values, int count)
{
    std::string out;
    out.reserve ((std::size_t) std::max (0, count) * 10 + 2);
    out += '[';
    char buf[32];
    for (int i = 0; i < count; ++i)
    {
        const float v = std::isfinite (values[i]) ? values[i] : 0.0f;
        std::snprintf (buf, sizeof (buf), "%.3g", (double) v);
        if (i > 0) out += ',';
        out += buf;
    }
    out += ']';
    return juce::String (out);
}

bool SessionLogger::start (const juce::File& directory, const juce::var& sessionHeader)
{
    if (isStarted())
        return true;

    if (! directory.createDirectory() || ! directory.isDirectory())
        return false;

    const auto stamp = juce::Time::getCurrentTime().formatted ("session-%Y%m%d-%H%M%S");
    auto candidate = directory.getChildFile (stamp + ".jsonl");
    // Two sessions in one second (tests do this): a numbered sibling that still
    // sorts AFTER the first in lexical order, which the prune relies on.
    for (int n = 2; candidate.exists() && n < 1000; ++n)
        candidate = directory.getChildFile (stamp + "_" + juce::String (n) + ".jsonl");

    auto stream = std::make_unique<juce::FileOutputStream> (candidate);
    if (stream->failedToOpen())
        return false;

    {
        const std::lock_guard<std::mutex> lock (fileMutex_);
        file_   = candidate;
        stream_ = std::move (stream);
    }
    t0_ = juce::Time::getMillisecondCounterHiRes();
    dropped_.store (0, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock (queueMutex_);
        queue_.clear();
    }

    // session_start: the header's properties behind "t" and "ev".
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("ev", "session_start");
    if (auto* h = sessionHeader.getDynamicObject())
        for (const auto& p : h->getProperties())
            obj->setProperty (p.name, p.value);
    writeDirect (stampLine (juce::JSON::toString (juce::var (obj), juce::JSON::FormatOptions()
                                                      .withSpacing (juce::JSON::Spacing::none)),
                            0));

    pruneOldFiles (directory, candidate);

    started_.store (true, std::memory_order_release);
    startThread();
    return true;
}

void SessionLogger::stop()
{
    if (! isStarted())
        return;

    started_.store (false, std::memory_order_release);   // log() becomes a no-op from here
    stopThread (config_.flushIntervalMs + 5000);           // signalThreadShouldExit + notify + join
    drain();                                               // whatever the thread did not get to

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("ev", "session_end");
    obj->setProperty ("dropped_events", (juce::int64) dropped_.load (std::memory_order_relaxed));
    writeDirect (stampLine (juce::JSON::toString (juce::var (obj), juce::JSON::FormatOptions()
                                                      .withSpacing (juce::JSON::Spacing::none)),
                            nowMs()));

    const std::lock_guard<std::mutex> lock (fileMutex_);
    if (stream_ != nullptr)
        stream_->flush();
    stream_.reset();
    // file_ is kept so currentFile() still names the finished session.
}

void SessionLogger::log (const juce::var& event)
{
    if (! isStarted())
        return;
    logLine (juce::JSON::toString (event, juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::none)));
}

void SessionLogger::logLine (const juce::String& jsonObject)
{
    if (! isStarted())
        return;

    auto line = stampLine (jsonObject, nowMs());
    const std::lock_guard<std::mutex> lock (queueMutex_);
    if ((int) queue_.size() >= config_.maxQueued)
    {
        dropped_.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    queue_.push_back (std::move (line));
}

juce::File SessionLogger::currentFile() const
{
    const std::lock_guard<std::mutex> lock (fileMutex_);
    return file_;
}

void SessionLogger::run()
{
    while (! threadShouldExit())
    {
        wait (config_.flushIntervalMs);   // returns early on notify() from stopThread()
        drain();
    }
}

void SessionLogger::drain()
{
    std::deque<juce::String> pending;
    {
        const std::lock_guard<std::mutex> lock (queueMutex_);
        pending.swap (queue_);
    }
    if (pending.empty())
        return;

    const std::lock_guard<std::mutex> lock (fileMutex_);
    if (stream_ == nullptr)
        return;
    for (const auto& line : pending)
    {
        stream_->write (line.toRawUTF8(), line.getNumBytesAsUTF8());
        stream_->writeByte ('\n');
    }
    stream_->flush();
}

void SessionLogger::writeDirect (const juce::String& line)
{
    const std::lock_guard<std::mutex> lock (fileMutex_);
    if (stream_ == nullptr)
        return;
    stream_->write (line.toRawUTF8(), line.getNumBytesAsUTF8());
    stream_->writeByte ('\n');
    stream_->flush();
}

void SessionLogger::pruneOldFiles (const juce::File& directory, const juce::File& keep)
{
    juce::Array<juce::File> files;
    directory.findChildFiles (files, juce::File::findFiles, false, "session-*.jsonl");
    std::vector<juce::File> sorted (files.begin(), files.end());
    // The file name embeds the timestamp, so lexical order IS chronological.
    std::sort (sorted.begin(), sorted.end(),
               [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
    int excess = (int) sorted.size() - config_.keepFiles;
    for (const auto& f : sorted)
    {
        if (excess <= 0) break;
        if (f == keep) continue;
        f.deleteFile();
        --excess;
    }
}
```

`juce::Thread::wait (int ms)` returns early when `notify()` is called; `stopThread()` calls `signalThreadShouldExit()` then `notify()` then joins, so a 60 s `flushIntervalMs` does not delay `stop()`.

- [ ] **Step 4: Build and run the new tests**

Run: `cmake --build build --config Release --target HandsFreeTests && cd build && ctest -C Release --output-on-failure -R SessionLogger`
Expected: 6 tests pass. If `FiveThousandLogs...` fails on `droppedEvents == 904`, the writer woke during the loop: check that `run()` waits BEFORE draining and that `log()` does not call `notify()`.

- [ ] **Step 5: Full suite and commit**

Run: `cd build && ctest -C Release` → `100% tests passed` (405 + 6 = 411).

```bash
git add src/app/SessionLogger.h src/app/SessionLogger.cpp tests/test_sessionlogger.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(log): SessionLogger writes per-run JSONL on its own thread, bounded and pruned (lane D)"
```

---

### Task 3: `NotchController` event outbox and clear reasons

**Files:**
- Modify: `src/app/NotchController.h` (public API after `enum class Origin`; private section near `outbox_`), `src/app/NotchController.cpp` (`setWidth`, `setNotch`, `pushClearLocked`, `clearNotch`, `clearAll`, `adoptPreset`, `runOnce` step 3, `placeConfirmed`, `processSpectrumForDetection`, `flushOutbox`, `stop`)
- Test: `tests/test_notchcontroller.cpp` (append)

**Interfaces:**
- Consumes: `CandidateScorer::ScoreBreakdown`, `scoreCandidateDetailed` (Task 1).
- Produces:
  ```cpp
  enum class NotchController::ClearReason { Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind };
  static const char* NotchController::clearReasonName (ClearReason);  // "manual","clear_all","auto_release","width_change","verdict_false","partial_apply_unwind"
  static const char* NotchController::originName (Origin);            // "detector","preset","manual","soundcheck"

  struct NotchController::NotchEvent
  {
      enum class Kind { Set, Clear };
      Kind kind = Kind::Set;
      int slot = 0, lane = 0, index = 0;
      double hz = 0.0, q = 0.0, depthDb = 0.0;
      Origin origin = Origin::Manual;
      ClearReason reason = ClearReason::Manual;   // Clear only
      double ageMs = 0.0;                          // Clear only: liveMs at clear - lockedAtMs
      // Set from the detector only (hasDetection == true):
      bool   hasDetection = false;
      float  score = 0.0f, peakiness = 0.0f, pNorm = 0.0f, rise = 0.0f, novelty = 0.0f, penalty = 1.0f, asym = 1.0f;
      int    persistNeeded = 0;
      float  thr = 0.0f;
      double riseRefMs = 0.0, binHz = 0.0, refAgeMs = 0.0;
      bool   hasRef = false, hasOther = false;
      std::array<float, Detector::kNumBins> now {}, ref {}, other {};
  };
  using NotchController::EventSink = std::function<void (const NotchEvent&)>;
  void NotchController::setEventSink (EventSink sink);          // thread must be stopped; nullptr = off
  std::uint64_t NotchController::droppedEvents() const;
  static constexpr int NotchController::kEventOutboxCapacity = 64;
  void NotchController::clearNotch (int channel, int index, ClearReason reason = ClearReason::Manual);
  // TEST HOOKS
  std::size_t NotchController::eventOutboxSizeForTest() const;
  void NotchController::setLaneRejectForTest (int lane);        // -1 = none; makes setNotch fail on that lane
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp` (after the last existing test; `Harness`, `StereoHarness`, `pump`, `pumpStereo`, `SineSource`, `NoiseSource`, `kWarmupBlocks`, `drain`, `onePresetNotch` are already defined in the file):

```cpp
//==============================================================================
// Lane D (data loop): event sink. Spec §4 tests 7-11.

namespace
{
using Ev = NotchController::NotchEvent;

struct EventCollector
{
    std::vector<Ev> events;   // 12 KB each; a test collects at most a few dozen
    NotchController::EventSink sink()
    {
        return [this] (const Ev& e) { events.push_back (e); };
    }
    std::vector<Ev> ofKind (Ev::Kind k) const
    {
        std::vector<Ev> out;
        for (const auto& e : events) if (e.kind == k) out.push_back (e);
        return out;
    }
};

// Same warm-up as primeAndPlace(), but returns once the SINK has seen a Set.
void primeAndPlaceWithSink (Harness& h, EventCollector& sink)
{
    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource tone;
    for (int i = 0; i < 40 && sink.ofKind (Ev::Kind::Set).empty(); ++i)
        pump (h, tone.hop());
}
} // namespace

// Spec test 7. Red if notch_set stops carrying the analysed frame as ctx.now,
// the scorer's reference frame as ctx.ref, or the score components.
TEST (NotchControllerEvents, DetectorSetCarriesTheFrameAndTheReferenceItScored)
{
    Harness h;
    EventCollector sink;
    h.controller.setEventSink (sink.sink());
    h.controller.setDetectionActive (true);
    ASSERT_NO_FATAL_FAILURE (primeAndPlaceWithSink (h, sink));

    const auto sets = sink.ofKind (Ev::Kind::Set);
    ASSERT_GE (sets.size(), 1u);
    const auto& e = sets.front();
    EXPECT_EQ (e.slot, 0);
    EXPECT_EQ (e.origin, NotchController::Origin::Detector);
    EXPECT_TRUE (e.hasDetection);
    EXPECT_GT (e.score, CandidateScorer::kConfirmScore);
    EXPECT_GT (e.peakiness, PeakinessAnalyzer::kDefaultThreshold);
    EXPECT_EQ (e.persistNeeded, NotchController::kPersistenceBlocks);
    EXPECT_FLOAT_EQ (e.thr, PeakinessAnalyzer::kDefaultThreshold);
    EXPECT_DOUBLE_EQ (e.riseRefMs, CandidateScorer::kDefaultRiseReferenceMs);
    EXPECT_NEAR (e.binHz, kTestSr / Detector::kFftSize, 1e-9);
    EXPECT_NEAR (e.hz, 1000.0, 0.5 * kTestSr / Detector::kFftSize);

    // ctx.now IS the frame that was analysed: one hop per pump, so the last
    // published snapshot is that frame.
    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_EQ (std::memcmp (e.now.data(), snap.magnitudes[0].data(), sizeof (float) * Detector::kNumBins), 0);

    // ctx.ref is the frame the rise axis used (D-7): present after warm-up,
    // at least 0.45 x rise reference old, and a DIFFERENT frame from now.
    ASSERT_TRUE (e.hasRef);
    EXPECT_GE (e.refAgeMs, 0.45 * CandidateScorer::kDefaultRiseReferenceMs);
    EXPECT_LE (e.refAgeMs, CandidateScorer::kDefaultRiseReferenceMs + 3.0 * kBlockMs);
    EXPECT_NE (std::memcmp (e.ref.data(), e.now.data(), sizeof (float) * Detector::kNumBins), 0);
    EXPECT_FALSE (e.hasOther);   // legacy one-tap harness: no other lane
}

// Spec test 8 (part 1): five of the six clear reasons on a stereo controller.
// Red if any path stops naming its reason.
TEST (NotchControllerEvents, ClearReasonsNameTheirPath)
{
    StereoHarness h;
    EventCollector sink;
    h.controller.setEventSink (sink.sink());

    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.clearNotch (0, 0);                                            // default = Manual
    ASSERT_TRUE (h.controller.setNotch (0, 1, 1100.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.clearNotch (0, 1, NotchController::ClearReason::VerdictFalse);
    ASSERT_TRUE (h.controller.setNotch (0, 2, 1200.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.clearAll();
    ASSERT_TRUE (h.controller.setNotch (1, 3, 1300.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.setWidth (1);                                                 // lane 1 leaves the slot
    h.controller.runOnce();                                                    // flush

    const auto clears = sink.ofKind (Ev::Kind::Clear);
    ASSERT_EQ (clears.size(), 4u);
    EXPECT_EQ (clears[0].reason, NotchController::ClearReason::Manual);       EXPECT_EQ (clears[0].index, 0);
    EXPECT_EQ (clears[1].reason, NotchController::ClearReason::VerdictFalse); EXPECT_EQ (clears[1].index, 1);
    EXPECT_EQ (clears[2].reason, NotchController::ClearReason::ClearAll);     EXPECT_EQ (clears[2].index, 2);
    EXPECT_EQ (clears[3].reason, NotchController::ClearReason::WidthChange);  EXPECT_EQ (clears[3].lane, 1);
    for (const auto& c : clears)
        EXPECT_GT (c.hz, 0.0) << "a clear must name the frequency it removed";

    EXPECT_STREQ (NotchController::clearReasonName (NotchController::ClearReason::AutoRelease), "auto_release");
    EXPECT_STREQ (NotchController::clearReasonName (NotchController::ClearReason::PartialApplyUnwind), "partial_apply_unwind");
    EXPECT_STREQ (NotchController::originName (NotchController::Origin::Soundcheck), "soundcheck");
}

// Spec test 8 (part 2): auto-release names itself and carries the notch's age.
TEST (NotchControllerEvents, AutoReleaseClearCarriesReasonAndAge)
{
    Harness h;
    EventCollector sink;
    h.controller.setEventSink (sink.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 7000; ++i)              // 35 s of live audio, same recipe as LiveTapReleasesAfter30s
    {
        h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    const auto clears = sink.ofKind (Ev::Kind::Clear);
    ASSERT_EQ (clears.size(), 1u);
    EXPECT_EQ (clears[0].reason, NotchController::ClearReason::AutoRelease);
    EXPECT_GT (clears[0].ageMs, NotchController::kAutoReleaseMs);
}

// Spec test 8 (part 3, plan decision P-4): the internal unwind is NOT an
// operator action and must not be logged as "manual". The trigger is a
// test-only hook because setNotch's validation is identical on both lanes.
TEST (NotchControllerEvents, PartialApplyUnwindIsNamedNotManual)
{
    StereoHarness h;
    EventCollector sink;
    h.controller.setEventSink (sink.sink());
    h.controller.setLaneRejectForTest (1);

    PresetNotch p;
    p.index = 4; p.freq = 900.0; p.Q = 30.0; p.depthDB = -12.0; p.lane = -1;   // both lanes wanted
    EXPECT_EQ (h.controller.adoptPreset ({ p }), 0);
    h.controller.runOnce();

    const auto sets   = sink.ofKind (Ev::Kind::Set);
    const auto clears = sink.ofKind (Ev::Kind::Clear);
    ASSERT_EQ (sets.size(), 1u);   EXPECT_EQ (sets[0].lane, 0);   EXPECT_EQ (sets[0].origin, NotchController::Origin::Preset);
    ASSERT_EQ (clears.size(), 1u); EXPECT_EQ (clears[0].lane, 0); EXPECT_EQ (clears[0].index, 4);
    EXPECT_EQ (clears[0].reason, NotchController::ClearReason::PartialApplyUnwind);
}

// Spec test 9 (D-6). Red if the sink is ever invoked under modelMutex_: the
// re-entrant clearNotch below would deadlock (std::mutex is not recursive).
TEST (NotchControllerEvents, SinkRunsOutsideTheModelMutex)
{
    Harness h;
    int clearsSeen = 0;
    h.controller.setEventSink ([&] (const Ev& e)
    {
        if (e.kind == Ev::Kind::Set)
            h.controller.clearNotch (e.lane, e.index);   // takes modelMutex_
        else
            ++clearsSeen;
    });
    ASSERT_TRUE (h.controller.setNotch (0, 5, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.runOnce();   // delivers Set -> sink clears -> Clear queued
    h.controller.runOnce();   // delivers Clear
    EXPECT_EQ (clearsSeen, 1);
}

// Spec test 10. Red if events are queued with no sink attached.
TEST (NotchControllerEvents, NoSinkMeansNoQueue)
{
    Harness h;
    for (int i = 0; i < 16; ++i)
        ASSERT_TRUE (h.controller.setNotch (0, i, 500.0 + i, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.clearAll();
    EXPECT_EQ (h.controller.eventOutboxSizeForTest(), 0u);
    EXPECT_EQ (h.controller.droppedEvents(), 0u);
}

// D-4 for the controller: past kEventOutboxCapacity the outbox drops and
// counts rather than growing. Red if the cap is removed.
TEST (NotchControllerEvents, OutboxDropsAndCountsPastCapacity)
{
    Harness h;
    EventCollector sink;
    h.controller.setEventSink (sink.sink());
    const int total = NotchController::kEventOutboxCapacity + 10;
    for (int i = 0; i < total; ++i)
    {
        ASSERT_TRUE (h.controller.setNotch (0, i % 16, 500.0 + i, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.clearNotch (0, i % 16);
    }
    EXPECT_EQ (h.controller.eventOutboxSizeForTest(), (std::size_t) NotchController::kEventOutboxCapacity);
    EXPECT_EQ (h.controller.droppedEvents(), (std::uint64_t) (2 * total - NotchController::kEventOutboxCapacity));
    h.controller.runOnce();
    EXPECT_EQ (sink.events.size(), (std::size_t) NotchController::kEventOutboxCapacity);
    EXPECT_EQ (h.controller.eventOutboxSizeForTest(), 0u);
}

// Spec test 11. Red if stop() joins without flushing the last events, both
// with a thread that never started and with a running one.
TEST (NotchControllerEvents, StopFlushesPendingEvents)
{
    {
        Harness h;
        EventCollector sink;
        h.controller.setEventSink (sink.sink());
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.stop (1000);   // thread never ran
        ASSERT_EQ (sink.events.size(), 1u);
        EXPECT_EQ (sink.events[0].kind, Ev::Kind::Set);
    }
    {
        Harness h;
        EventCollector sink;
        h.controller.setEventSink (sink.sink());
        h.controller.start();
        ASSERT_TRUE (h.controller.setNotch (0, 1, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.stop (2000);
        ASSERT_GE (sink.events.size(), 1u);
        EXPECT_EQ (sink.events.back().kind, Ev::Kind::Set);
    }
}

// Stereo: ctx.other_lane_now is present only when the other lane produced a
// block in the same drain iteration. Red if the stereo path forgets it.
TEST (NotchControllerEvents, StereoSetCarriesTheOtherLanesFrame)
{
    StereoHarness h;
    EventCollector sink;
    h.controller.setEventSink (sink.sink());
    h.controller.setDetectionActive (true);
    NoiseSource quietL, quietR;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());
    SineSource tone;
    for (int i = 0; i < 40 && sink.ofKind (Ev::Kind::Set).empty(); ++i)
        pumpStereo (h, tone.hop(), quietR.hop());
    const auto sets = sink.ofKind (Ev::Kind::Set);
    ASSERT_GE (sets.size(), 1u);
    EXPECT_EQ (sets[0].lane, 0);
    EXPECT_TRUE (sets[0].hasOther);
    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_EQ (std::memcmp (sets[0].other.data(), snap.magnitudes[1].data(), sizeof (float) * Detector::kNumBins), 0);
}
```

Add `#include <cstring>` at the top of the test file for `std::memcmp`.

- [ ] **Step 2: Verify RED**

Run: `cmake --build build --config Release --target HandsFreeTests`
Expected: compile errors on `NotchEvent`, `setEventSink`, `ClearReason`.

- [ ] **Step 3: Implement the header changes**

In `src/app/NotchController.h`, add `#include <functional>` and, right after `enum class Origin`:

```cpp
    // Lane D: why a notch left the model. The logger writes the name; the
    // two internal unwinds are NOT operator actions and must never read as
    // "manual" in training data (spec §3.3).
    enum class ClearReason { Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind };
    static const char* clearReasonName (ClearReason r);
    static const char* originName (Origin o);
```

After `static constexpr double kDefaultNotchDepthDb`, add the event API:

```cpp
    // ---- Lane D: notch events for the session log (spec §3.3) ----------
    //
    // Every model write (setNotch / any clear) appends ONE NotchEvent to
    // eventOutbox_ while modelMutex_ is held; flushEvents() -- called from
    // flushOutbox() on the detector thread, and from stop() -- swaps the
    // outbox out and invokes the sink OUTSIDE the lock (D-6). A sink may
    // therefore call clearNotch() re-entrantly, and the message thread never
    // waits for logger I/O.
    //
    // Set events placed by the detector carry the spectrum context: the frame
    // analysed, the frame the rise axis compared against (D-7), and the
    // other lane's frame when there was one this drain. The arrays are fixed
    // members so queueing is a copy, never an allocation; the outbox holds at
    // most kEventOutboxCapacity events and drops-and-counts beyond that.
    struct NotchEvent
    {
        enum class Kind { Set, Clear };
        Kind kind = Kind::Set;
        int slot = 0, lane = 0, index = 0;
        double hz = 0.0, q = 0.0, depthDb = 0.0;
        Origin origin = Origin::Manual;
        ClearReason reason = ClearReason::Manual;   // Clear only
        double ageMs = 0.0;                          // Clear only: liveMs at clear - lockedAtMs
        bool   hasDetection = false;                 // Set from the detector (score fields + ctx valid)
        float  score = 0.0f, peakiness = 0.0f, pNorm = 0.0f, rise = 0.0f, novelty = 0.0f, penalty = 1.0f, asym = 1.0f;
        int    persistNeeded = 0;
        float  thr = 0.0f;
        double riseRefMs = 0.0, binHz = 0.0, refAgeMs = 0.0;
        bool   hasRef = false, hasOther = false;
        std::array<float, Detector::kNumBins> now {}, ref {}, other {};
    };
    using EventSink = std::function<void (const NotchEvent&)>;
    static constexpr int kEventOutboxCapacity = 64;

    // Message thread, and ONLY while the detector thread is stopped (the sink
    // is read unlocked on that thread). nullptr disables event queueing
    // entirely -- with no sink the outbox never grows.
    void setEventSink (EventSink sink);
    std::uint64_t droppedEvents() const { return droppedEvents_.load (std::memory_order_relaxed); }

    // TEST HOOKS ONLY.
    std::size_t eventOutboxSizeForTest() const;
    // Makes setNotch() fail on `lane` (-1 = off), the only way to reach the
    // PartialApplyUnwind path: production validation is lane-invariant.
    void setLaneRejectForTest (int lane) { laneRejectForTest_ = lane; }
```

Change the `clearNotch` declaration:

```cpp
    void clearNotch (int channel, int index, ClearReason reason = ClearReason::Manual);
```

In the private section, replace `void pushClearLocked (int channel, int index);` with the new helpers and state:

```cpp
    // What placeConfirmed knows about the confirm that setNotchInternal must
    // stamp onto the Set event. Pointers are valid for the duration of the
    // call (block.magnitudes until the next processLatestBlock; refFrame until
    // the next commitBlock -- both later than the copy into the outbox).
    struct DetectionContext
    {
        const CandidateScorer::ScoreBreakdown* breakdown = nullptr;
        float asym = 1.0f;
        const float* now = nullptr;
        const float* other = nullptr;   // nullptr when the other lane had no block this drain
        int persistNeeded = 0;
        float thr = 0.0f;
        double riseRefMs = 0.0;
        double binHz = 0.0;
    };

    bool setNotchInternal (int channel, int index, double frequency, double Q, double depthDB,
                           Origin origin, const DetectionContext* ctx);
    void pushClearLocked (int channel, int index, ClearReason reason);
    // Appends to eventOutbox_ (caller holds modelMutex_). No-op without a
    // sink; drop-and-count at capacity.
    void queueEventLocked (const NotchEvent& e);
    void flushEvents();

    EventSink eventSink_;
    std::vector<NotchEvent> eventOutbox_;      // guarded by modelMutex_; reserved in setEventSink
    std::atomic<std::uint64_t> droppedEvents_ { 0 };
    int laneRejectForTest_ = -1;
```

Change `placeConfirmed`'s declaration to take the context:

```cpp
    void placeConfirmed (int lane, const PeakinessAnalyzer::Candidate& cand, bool linkedNow,
                         const DetectionContext& ctx);
```

- [ ] **Step 4: Implement the .cpp changes**

`src/app/NotchController.cpp`:

```cpp
const char* NotchController::clearReasonName (ClearReason r)
{
    switch (r)
    {
        case ClearReason::Manual:             return "manual";
        case ClearReason::ClearAll:           return "clear_all";
        case ClearReason::AutoRelease:        return "auto_release";
        case ClearReason::WidthChange:        return "width_change";
        case ClearReason::VerdictFalse:       return "verdict_false";
        case ClearReason::PartialApplyUnwind: return "partial_apply_unwind";
    }
    return "manual";
}

const char* NotchController::originName (Origin o)
{
    switch (o)
    {
        case Origin::Detector:   return "detector";
        case Origin::Preset:     return "preset";
        case Origin::Manual:     return "manual";
        case Origin::Soundcheck: return "soundcheck";
    }
    return "manual";
}

void NotchController::setEventSink (EventSink sink)
{
    jassert (! isThreadRunning());   // read unlocked on the detector thread
    const std::lock_guard<std::mutex> lock (modelMutex_);
    eventSink_ = std::move (sink);
    if (eventSink_)
        eventOutbox_.reserve ((std::size_t) kEventOutboxCapacity);
    else
        std::vector<NotchEvent>().swap (eventOutbox_);
}

std::size_t NotchController::eventOutboxSizeForTest() const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return eventOutbox_.size();
}

void NotchController::queueEventLocked (const NotchEvent& e)
{
    if (! eventSink_)
        return;
    if (eventOutbox_.size() >= (std::size_t) kEventOutboxCapacity)
    {
        droppedEvents_.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    eventOutbox_.push_back (e);
}

void NotchController::flushEvents()
{
    std::vector<NotchEvent> pending;
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        if (eventOutbox_.empty())
            return;
        pending.swap (eventOutbox_);
    }
    // OUTSIDE the lock, by contract (D-6).
    if (eventSink_)
        for (const auto& e : pending)
            eventSink_ (e);

    // Hand the reserved buffer back so steady state never re-allocates.
    pending.clear();
    const std::lock_guard<std::mutex> lock (modelMutex_);
    if (eventOutbox_.empty())
        eventOutbox_.swap (pending);
}
```

`stop()`:

```cpp
void NotchController::stop (int timeoutMs)
{
    if (isThreadRunning())
        stopThread (timeoutMs);
    // Events queued after the last poll (or with no thread at all -- tests,
    // and adoptPreset/setWidth which run with the thread stopped) still
    // reach the sink before the caller proceeds.
    flushEvents();
}
```

`setWidth`: change `pushClearLocked (c, i);` to `pushClearLocked (c, i, ClearReason::WidthChange);`.

`setNotch` becomes a forwarder and the body moves to `setNotchInternal`:

```cpp
bool NotchController::setNotch (int channel, int index, double frequency, double Q, double depthDB, Origin origin)
{
    return setNotchInternal (channel, index, frequency, Q, depthDB, origin, nullptr);
}

bool NotchController::setNotchInternal (int channel, int index, double frequency, double Q, double depthDB,
                                        Origin origin, const DetectionContext* ctx)
{
    // ... existing validation unchanged ...
    if (channel == laneRejectForTest_)              return false;   // TEST HOOK (P-4)

    const NotchCommand cmd { ... unchanged ... };
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        auto& n = model_[slotOf (channel, index)];
        // ... existing model write unchanged ...
        outbox_.push_back (cmd);

        if (eventSink_)
        {
            NotchEvent e;
            e.kind = NotchEvent::Kind::Set;
            e.slot = slotId_; e.lane = channel; e.index = index;
            e.hz = frequency; e.q = Q; e.depthDb = depthDB; e.origin = origin;
            if (ctx != nullptr && ctx->breakdown != nullptr && ctx->now != nullptr)
            {
                e.hasDetection  = true;
                e.score         = ctx->breakdown->score * ctx->asym;   // the number that crossed kConfirmScore
                e.peakiness     = ctx->breakdown->rawPeakiness;
                e.pNorm         = ctx->breakdown->pNorm;
                e.rise          = ctx->breakdown->rNorm;
                e.novelty       = ctx->breakdown->mNorm;
                e.penalty       = ctx->breakdown->penalty;
                e.asym          = ctx->asym;
                e.persistNeeded = ctx->persistNeeded;
                e.thr           = ctx->thr;
                e.riseRefMs     = ctx->riseRefMs;
                e.binHz         = ctx->binHz;
                std::copy_n (ctx->now, Detector::kNumBins, e.now.data());
                if (ctx->breakdown->refFrame != nullptr)
                {
                    e.hasRef   = true;
                    e.refAgeMs = ctx->breakdown->refAgeMs;
                    std::copy_n (ctx->breakdown->refFrame, Detector::kNumBins, e.ref.data());
                }
                if (ctx->other != nullptr)
                {
                    e.hasOther = true;
                    std::copy_n (ctx->other, Detector::kNumBins, e.other.data());
                }
            }
            queueEventLocked (e);
        }
    }
    return true;
}
```

(`NotchEvent e` is ~12 KB on the stack: fine on both the message thread and the detector thread. Do NOT make it `static`.)

`pushClearLocked` / `clearNotch` / `clearAll`:

```cpp
void NotchController::pushClearLocked (int channel, int index, ClearReason reason)
{
    auto& n = model_[slotOf (channel, index)];
    if (! n.active)
        return;
    if (eventSink_)
    {
        NotchEvent e;
        e.kind = NotchEvent::Kind::Clear;
        e.slot = slotId_; e.lane = channel; e.index = index;
        e.hz = n.frequency; e.q = n.Q; e.depthDb = n.depthDB; e.origin = n.origin;
        e.reason = reason;
        e.ageMs  = liveMs_ - n.lockedAtMs;
        queueEventLocked (e);
    }
    n.active = false;
    outbox_.push_back ({ NotchCommandType::Clear, (std::uint8_t) channel, (std::uint8_t) index,
                         0.0f, 0.0f, 0.0f, slotId_ });
}

void NotchController::clearNotch (int channel, int index, ClearReason reason)
{
    if (channel < 0 || channel >= width_ || index < 0 || index >= kSlots)
        return;
    const std::lock_guard<std::mutex> lock (modelMutex_);
    pushClearLocked (channel, index, reason);
}

void NotchController::clearAll()
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    for (int c = 0; c < kChannels; ++c)
        for (int i = 0; i < kSlots; ++i)
            pushClearLocked (c, i, ClearReason::ClearAll);
}
```

`adoptPreset`: the unwind loop becomes `clearNotch (lane, p.index, ClearReason::PartialApplyUnwind);`.

`runOnce` step 3 (auto-release): `pushClearLocked (c, i, ClearReason::AutoRelease);`.

`processSpectrumForDetection`: replace the scoring lines with

```cpp
        const auto breakdown = la.scorer.scoreCandidateDetailed (cand, block.magnitudes,
                                                                 { locked.data(), locked.size() });
        const float asym = asymmetryMultiplier (block.magnitudes, otherLaneMagnitudes, cand.bin,
                                                laneAsymmetryBonus_.load (std::memory_order_relaxed));
        float score = breakdown.score;
        score *= asym;   // same two-step arithmetic as before this lane
```

and at the confirm site build the context:

```cpp
                DetectionContext ctx;
                ctx.breakdown     = &breakdown;
                ctx.asym          = asym;
                ctx.now           = block.magnitudes;
                ctx.other         = otherLaneMagnitudes;
                ctx.persistNeeded = (int) requiredBlocks;
                ctx.thr           = la.analyzer.getThreshold();
                ctx.riseRefMs     = la.scorer.getRiseReferenceMs();
                ctx.binHz         = block.sampleRate / (double) Detector::kFftSize;
                placeConfirmed (lane, cand, linkedNow, ctx);
```

`placeConfirmed (int lane, const Candidate& cand, bool linkedNow, const DetectionContext& ctx)`: `setNotch (l, index, ...)` becomes `setNotchInternal (l, index, cand.frequencyHz, q, depthDb, origin, &ctx)`; the unwind loop becomes `clearNotch (l, index, ClearReason::PartialApplyUnwind);`.

`flushOutbox()`: append `flushEvents();` as the last statement (after the command-ring write and retry handling). It already runs outside `modelMutex_` at that point.

- [ ] **Step 5: Build, run the controller tests**

Run: `cmake --build build --config Release --target HandsFreeTests && cd build && ctest -C Release --output-on-failure -R NotchController`
Expected: every `NotchController*` test passes (existing + 9 new). If `SinkRunsOutsideTheModelMutex` hangs, the sink is being called under the lock — check `flushEvents()` and that `pushClearLocked` never calls the sink directly.

- [ ] **Step 6: Full suite and commit**

Run: `cd build && ctest -C Release` → `100% tests passed` (411 + 9 = 420).

```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp
git commit -m "feat(controller): notch events with clear reasons and spectrum context, delivered outside the model lock (lane D)"
```

---

### Task 4: VERDICT buttons in `NotchListPanel`

**Files:**
- Modify: `src/gui/NotchListPanel.h` (column constants, `RowText`, `onVerdict`, test accessors, private state), `src/gui/NotchListPanel.cpp` (`refreshFromSnapshot`, `setController`, `resized`, `statusWidthFor`, `paint`, new `layoutRowButtons`/`handleVerdict`)
- Test: `tests/test_notchlistpanel.cpp` (append + amend `EveryColumnFitsItsWidestCellAtTheNarrowestPanel`)

**Interfaces:**
- Produces:
  ```cpp
  enum class gui::NotchListPanel::Verdict { None, Good, False };
  std::function<void (int slot, int lane, int index, float hz, bool good, double ageMs)> gui::NotchListPanel::onVerdict;
  static constexpr float gui::NotchListPanel::kColVerdictW     = 92.0f;
  static constexpr int   gui::NotchListPanel::kVerdictButtonW  = 40;
  static constexpr int   gui::NotchListPanel::kVerdictButtonH  = 18;
  static constexpr int   gui::NotchListPanel::kVerdictGap      = 4;
  static constexpr float gui::NotchListPanel::kMinUsefulWidth  = 440.0f;
  // RowText gains:  Verdict verdict; juce::String verdictText;
  // TEST ACCESSORS
  juce::TextButton* goodButtonForTest (int row);    // nullptr when the row has a verdict already
  juce::TextButton* falseButtonForTest (int row);
  Verdict verdictForTest (int row) const;
  int verdictButtonPairCountForTest() const;        // buttons_.size()
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchlistpanel.cpp` (the file already has `FakeClock`, `TwoNotchController`, `paintHeadless`):

```cpp
//==============================================================================
// Lane D: verdict buttons. Spec §4 tests 12-13.

namespace
{
struct VerdictCall { int slot, lane, index; float hz; bool good; double ageMs; };

int rowOf (const gui::NotchListPanel& panel, const juce::String& freqText)
{
    for (int i = 0; i < panel.rowCountForTest(); ++i)
        if (panel.rowForTest (i).freq == freqText)
            return i;
    return -1;
}
} // namespace

// Spec test 12. Red if FALSE stops reporting good=false, if GOOD stops
// reporting good=true, if the panel starts commanding the controller itself
// (the GOOD row must survive), or if the VERDICT cell does not change state.
TEST (NotchListPanelVerdict, ButtonsReportTheVerdictAndTheCellChangesState)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TwoNotchController fed;
    FakeClock clock;
    gui::NotchListPanel panel (fed.controller, [&] { return clock.nowMs; });
    panel.setDisplayedSlot (3);
    std::vector<VerdictCall> calls;
    panel.onVerdict = [&] (int slot, int lane, int index, float hz, bool good, double ageMs)
    {
        calls.push_back ({ slot, lane, index, hz, good, ageMs });
    };

    panel.setSize (520, 200);
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 2);
    const int r987 = rowOf (panel, "987 Hz");
    const int r24k = rowOf (panel, "2.4 kHz");
    ASSERT_GE (r987, 0); ASSERT_GE (r24k, 0);

    ASSERT_NE (panel.falseButtonForTest (r987), nullptr);
    ASSERT_NE (panel.goodButtonForTest (r24k), nullptr);
    EXPECT_EQ (panel.falseButtonForTest (r987)->getButtonText().toStdString(), "FALSE");
    EXPECT_EQ (panel.goodButtonForTest (r24k)->getButtonText().toStdString(), "GOOD");

    clock.nowMs += 4000.0;
    panel.refreshFromSnapshot();
    // triggerClick() posts a command message this headless suite never pumps
    // (test_slotpanel.cpp:147) -- invoke the handler directly.
    panel.falseButtonForTest (r987)->onClick();
    panel.goodButtonForTest (r24k)->onClick();

    ASSERT_EQ (calls.size(), 2u);
    EXPECT_EQ (calls[0].slot, 3); EXPECT_EQ (calls[0].lane, 0); EXPECT_EQ (calls[0].index, 0);
    EXPECT_NEAR (calls[0].hz, 987.0f, 0.01f); EXPECT_FALSE (calls[0].good);
    EXPECT_NEAR (calls[0].ageMs, 4000.0, 1.0);
    EXPECT_EQ (calls[1].slot, 3); EXPECT_EQ (calls[1].lane, 1); EXPECT_EQ (calls[1].index, 3);
    EXPECT_TRUE (calls[1].good);

    EXPECT_EQ (panel.verdictForTest (r987), gui::NotchListPanel::Verdict::False);
    EXPECT_EQ (panel.verdictForTest (r24k), gui::NotchListPanel::Verdict::Good);
    EXPECT_EQ (panel.rowForTest (r987).verdictText.toStdString(), "FALSE");
    EXPECT_EQ (panel.rowForTest (r24k).verdictText.toStdString(), "GOOD");
    EXPECT_EQ (panel.falseButtonForTest (r987), nullptr);   // hidden after the verdict
    EXPECT_EQ (panel.goodButtonForTest (r24k), nullptr);

    // Display only: the panel itself cleared nothing.
    fed.republish();
    panel.refreshFromSnapshot();
    EXPECT_EQ (panel.rowCountForTest(), 2);
    EXPECT_EQ (panel.verdictForTest (rowOf (panel, "987 Hz")), gui::NotchListPanel::Verdict::False);
    paintHeadless (panel, 520, 200);   // must not allocate-crash or draw past the frame
}

// Spec test 13. Red if refreshFromSnapshot() rebuilds the buttons (a click
// would land on a dead component), or if the pair outlives its identity's
// tracking window.
TEST (NotchListPanelVerdict, ButtonsPersistAcrossRefreshesAndDieWithTheIdentity)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TwoNotchController fed;
    FakeClock clock;
    gui::NotchListPanel panel (fed.controller, [&] { return clock.nowMs; });
    panel.setSize (520, 200);
    panel.refreshFromSnapshot();
    const int r = rowOf (panel, "987 Hz");
    ASSERT_GE (r, 0);
    juce::TextButton* good  = panel.goodButtonForTest (r);
    juce::TextButton* bad   = panel.falseButtonForTest (r);
    ASSERT_NE (good, nullptr); ASSERT_NE (bad, nullptr);
    EXPECT_EQ (panel.verdictButtonPairCountForTest(), 2);

    for (int i = 0; i < 10; ++i)
    {
        clock.nowMs += 250.0;
        panel.refreshFromSnapshot();
    }
    EXPECT_EQ (panel.goodButtonForTest (rowOf (panel, "987 Hz")), good);
    EXPECT_EQ (panel.falseButtonForTest (rowOf (panel, "987 Hz")), bad);
    EXPECT_EQ (panel.verdictButtonPairCountForTest(), 2);

    fed.controller.clearNotch (0, 0);
    fed.republish();
    panel.refreshFromSnapshot();
    EXPECT_EQ (panel.rowCountForTest(), 1);
    EXPECT_EQ (panel.verdictButtonPairCountForTest(), 2);   // identity still tracked (60 s window)
    EXPECT_FALSE (good->isVisible());                        // but no row to sit on

    clock.nowMs += gui::NotchListPanel::kTrackingTimeoutMs + 1000.0;
    panel.refreshFromSnapshot();
    EXPECT_EQ (panel.verdictButtonPairCountForTest(), 1);   // 987 Hz forgotten -> pair destroyed
}

// Red if the button geometry no longer fits the VERDICT column, or the
// column caption is ellipsised, at the panel's real fonts.
TEST (NotchListPanelVerdict, ButtonsAndCaptionFitTheVerdictColumn)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    using P = gui::NotchListPanel;
    EXPECT_LE (2 * P::kVerdictButtonW + P::kVerdictGap + 4, (int) P::kColVerdictW);

    const auto headerFont = az::theme::legendFont (az::theme::columnFontSize, true, az::theme::trackingColumn);
    EXPECT_LE (juce::GlyphArrangement::getStringWidth (headerFont, "VERDICT"), P::kColVerdictW);

    az::theme::AzLookAndFeel lnf;
    juce::TextButton probe ("FALSE");
    probe.setButtonText ("FALSE");
    probe.getProperties().set ("azStyle", "ghost");
    const auto buttonFont = lnf.getTextButtonFont (probe, P::kVerdictButtonH);
    EXPECT_LE (juce::GlyphArrangement::getStringWidth (buttonFont, "FALSE") + 6.0f, (float) P::kVerdictButtonW);
}

// P-8: the app never gives the table less than kMinUsefulWidth, so the
// column budget below is a layout fact, not a wish. Red if MainComponent's
// floor split or the minimum window shrinks the table under it.
TEST (NotchListPanelWiring, TableIsNeverNarrowerThanItsUsefulMinimum)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    app.setSize (MainComponent::kMinimumWidth, MainComponent::kMinimumHeight);
    app.resized();
    EXPECT_GE ((float) app.notchListBoundsForTest().getWidth(), gui::NotchListPanel::kMinUsefulWidth);
}
```

Amend `EveryColumnFitsItsWidestCellAtTheNarrowestPanel`: replace every `360.0f` with `gui::NotchListPanel::kMinUsefulWidth`, and update the comment block above it ("~360 px" → "kMinUsefulWidth, 440 px — the VERDICT column took the slack the 360 figure relied on; TableIsNeverNarrowerThanItsUsefulMinimum pins the app to it").

- [ ] **Step 2: Verify RED**

Run: `cmake --build build --config Release --target HandsFreeTests` → compile errors on `onVerdict`, `Verdict`, `goodButtonForTest`, `kColVerdictW`.

- [ ] **Step 3: Implement the header**

`src/gui/NotchListPanel.h`. Update the file comment: the table is still display-only, but lane D adds a VERDICT column with two child `TextButton`s per notch identity — a deliberate exception to "paint draws pre-built members only" (spec D-9): allocation happens only in `refreshFromSnapshot()`, and the buttons' lifetime follows `sightings_`.

Add after `using ClockFn`:

```cpp
    enum class Verdict { None, Good, False };

    // Lane D. Fired on the message thread when GOOD or FALSE is clicked. The
    // panel records the verdict for display and reports it -- it does NOT
    // clear the notch; MainComponent does (spec §3.4 keeps the panel
    // display-only). ageMs is the GUI-side age at the click.
    std::function<void (int slot, int lane, int index, float hz, bool good, double ageMs)> onVerdict;
```

Column constants — add after `kColQW`:

```cpp
    // VERDICT: two buttons ("GOOD" / "FALSE", ghost style, 40 x 18) plus the
    // gap between them and the same 4 px inset every column carries. Sits
    // AFTER the HELD column; HELD shrinks by this amount (spec §3.4).
    static constexpr float kColVerdictW    = 92.0f;
    static constexpr int   kVerdictButtonW = 40;
    static constexpr int   kVerdictButtonH = 18;
    static constexpr int   kVerdictGap     = 4;
    // The narrowest panel at which every column, VERDICT included, still fits
    // its widest cell. MainComponent's floor split gives the table ~510 px at
    // the 1200 px minimum window; a wiring test pins that it never drops
    // under this.
    static constexpr float kMinUsefulWidth = 440.0f;
```

`RowText` gains:

```cpp
        Verdict      verdict = Verdict::None;
        juce::String verdictText;   // "" / "GOOD" / "FALSE", painted once the buttons are gone
```

Test accessors, after `rowForTest`:

```cpp
    // Lane D test accessors: the live button pair behind a row, or nullptr
    // once that row carries a verdict (the pair is hidden then).
    [[nodiscard]] juce::TextButton* goodButtonForTest (int row);
    [[nodiscard]] juce::TextButton* falseButtonForTest (int row);
    [[nodiscard]] Verdict verdictForTest (int row) const;
    [[nodiscard]] int verdictButtonPairCountForTest() const { return (int) buttons_.size(); }
```

Private additions:

```cpp
    // Lane D: one button pair per notch IDENTITY (same key as sightings_),
    // created when an identity is first seen, destroyed when its sighting
    // expires. Never rebuilt on refresh, so a click cannot land on a
    // component that a 4 Hz rebuild just deleted.
    struct RowButtons
    {
        std::unique_ptr<juce::TextButton> good, bad;
        Verdict state = Verdict::None;
        int   lane  = 0;
        int   index = 0;
        float hz    = 0.0f;
    };
    std::map<std::uint64_t, RowButtons> buttons_;
    std::vector<std::uint64_t> rowKeys_;   // parallel to rows_: the identity behind each row
    int displayedSlot_ = 0;

    void layoutRowButtons();               // refreshFromSnapshot() and resized()
    void handleVerdict (std::uint64_t key, bool good);
```

Add `#include <memory>`.

- [ ] **Step 4: Implement the .cpp**

`src/gui/NotchListPanel.cpp` changes:

Constructor: `rowKeys_.reserve ((std::size_t) NotchController::kTotalSlots);`.

`refreshFromSnapshot()`, step 1 (sightings loop) — after `it->second.lastSeenMs = now;` create the pair on first sight:

```cpp
        if (buttons_.find (key) == buttons_.end())
        {
            RowButtons rb;
            rb.lane = notch.channel; rb.index = notch.index; rb.hz = notch.frequency;
            rb.good = std::make_unique<juce::TextButton> ("GOOD");
            rb.bad  = std::make_unique<juce::TextButton> ("FALSE");
            for (auto* b : { rb.good.get(), rb.bad.get() })
            {
                b->setButtonText (b->getName());          // JUCE 9: never rely on the ctor alone
                b->getProperties().set ("azStyle", "ghost");
                b->setMouseClickGrabsKeyboardFocus (false);
                addAndMakeVisible (*b);
            }
            rb.good->onClick = [this, key] { handleVerdict (key, true); };
            rb.bad ->onClick = [this, key] { handleVerdict (key, false); };
            buttons_.emplace (key, std::move (rb));
        }
```

Step 2 (rows): also push the key and the verdict:

```cpp
    rows_.clear();
    rowKeys_.clear();
    for (...)
    {
        ...
        Verdict verdict = Verdict::None;
        if (const auto bt = buttons_.find (key); bt != buttons_.end())
            verdict = bt->second.state;
        rows_.push_back ({ ..., formatAgeMs (...), juce::jmax (0.0, ageMs),
                           verdict,
                           verdict == Verdict::Good ? juce::String ("GOOD")
                         : verdict == Verdict::False ? juce::String ("FALSE") : juce::String() });
        rowKeys_.push_back (key);
    }
```

Step 3 (expiry): when a sighting is erased, also `buttons_.erase (it->first);` (the `unique_ptr` destructor removes the child from this component).

After step 3: `layoutRowButtons();` then `repaint();`.

`setController()`: add `buttons_.clear(); rowKeys_.clear();` next to the existing clears.

`setDisplayedSlot()`: add `displayedSlot_ = slotIndex;`.

`resized()`: remove the early `return` when `slotTabs_ == nullptr`; keep the tabs block under `if (slotTabs_ != nullptr) { ... }`; end with `layoutRowButtons();`.

`statusWidthFor()`: `fixedColumnsWidth` now includes `kColVerdictW`.

New members:

```cpp
void NotchListPanel::layoutRowButtons()
{
    // Every pair starts hidden; the ones with a live, unrated row get bounds.
    for (auto& [key, rb] : buttons_)
    {
        rb.good->setVisible (false);
        rb.bad ->setVisible (false);
    }

    const auto frame = getLocalBounds();
    const float x0 = (float) frame.getX() + kLeftPad;
    const float x4 = x0 + kColIdW + kColLaneW + kColFreqW + kColDepthW + kColQW;
    const float x5 = x4 + statusWidthFor ((float) frame.getWidth());
    int rowTop = kCaptionHeight + kHeaderHeight;

    for (std::size_t i = 0; i < rowKeys_.size(); ++i, rowTop += kRowHeight)
    {
        if (rowTop + kRowHeight > frame.getBottom())
            break;   // paint() clips the same rows
        const auto it = buttons_.find (rowKeys_[i]);
        if (it == buttons_.end() || it->second.state != Verdict::None)
            continue;
        const int y = rowTop + (kRowHeight - kVerdictButtonH) / 2;
        it->second.good->setBounds ((int) x5, y, kVerdictButtonW, kVerdictButtonH);
        it->second.bad ->setBounds ((int) x5 + kVerdictButtonW + kVerdictGap, y, kVerdictButtonW, kVerdictButtonH);
        it->second.good->setVisible (true);
        it->second.bad ->setVisible (true);
    }
}

void NotchListPanel::handleVerdict (const std::uint64_t key, const bool good)
{
    const auto it = buttons_.find (key);
    if (it == buttons_.end() || it->second.state != Verdict::None)
        return;
    it->second.state = good ? Verdict::Good : Verdict::False;
    it->second.good->setVisible (false);
    it->second.bad ->setVisible (false);

    double ageMs = 0.0;
    for (std::size_t i = 0; i < rowKeys_.size(); ++i)
        if (rowKeys_[i] == key)
        {
            rows_[i].verdict     = it->second.state;
            rows_[i].verdictText = good ? "GOOD" : "FALSE";
            ageMs = rows_[i].ageMs;
        }

    if (onVerdict)
        onVerdict (displayedSlot_, it->second.lane, it->second.index, it->second.hz, good, ageMs);
    repaint();
}

juce::TextButton* NotchListPanel::goodButtonForTest (const int row)
{
    if (row < 0 || (std::size_t) row >= rowKeys_.size()) return nullptr;
    const auto it = buttons_.find (rowKeys_[(std::size_t) row]);
    return (it != buttons_.end() && it->second.state == Verdict::None) ? it->second.good.get() : nullptr;
}

juce::TextButton* NotchListPanel::falseButtonForTest (const int row)
{
    if (row < 0 || (std::size_t) row >= rowKeys_.size()) return nullptr;
    const auto it = buttons_.find (rowKeys_[(std::size_t) row]);
    return (it != buttons_.end() && it->second.state == Verdict::None) ? it->second.bad.get() : nullptr;
}

NotchListPanel::Verdict NotchListPanel::verdictForTest (const int row) const
{
    return rows_[(std::size_t) row].verdict;
}
```

`paint()`: add `const float x5 = x4 + statusW;` after `statusW`; draw the header `g.drawText ("VERDICT", (int) x5, header.getY(), (int) kColVerdictW, header.getHeight(), juce::Justification::centredLeft);`; in the row loop, after the HELD text:

```cpp
        if (row.verdictText.isNotEmpty())
        {
            // GOOD reads as the settled ice of a held notch; FALSE is a
            // correction and takes the accent so it is seen from a distance.
            g.setColour (row.verdict == Verdict::False ? accent : settled);
            g.setFont (legendFont (columnFontSize, true, trackingColumn));
            g.drawText (row.verdictText, x5, rowTop, kColVerdictW - 4.0f, (float) kRowHeight,
                        juce::Justification::centredLeft);
        }
```

(`legendFont` returns a `juce::Font` by value — that is a font lookup, not a heap allocation of a string; it is the same call `paint()` already makes for the header row, so the no-allocation discipline is unchanged in kind.)

- [ ] **Step 5: Build and run the panel tests**

Run: `cmake --build build --config Release --target HandsFreeTests && cd build && ctest -C Release --output-on-failure -R NotchListPanel`
Expected: all pass. If `ButtonsAndCaptionFitTheVerdictColumn` fails on the "FALSE" width, raise `kVerdictButtonW` (and `kColVerdictW` by twice the difference) — never shrink the font.

- [ ] **Step 6: Full suite and commit**

Run: `cd build && ctest -C Release` → `100% tests passed` (420 + 4 = 424).

```bash
git add src/gui/NotchListPanel.h src/gui/NotchListPanel.cpp tests/test_notchlistpanel.cpp
git commit -m "feat(gui): GOOD/FALSE verdict buttons per notch identity in the ACTIVE NOTCHES table (lane D)"
```

---

### Task 5: `MainComponent` wiring — logger, sinks, verdict, mode/tuning/device events

**Files:**
- Modify: `src/app/MainComponent.h` (includes, public API, member `sessionLogger_` right after `systemClock_`, private helpers), `src/app/MainComponent.cpp` (ctor: sinks + `onVerdict` + tuning/mode logging; dtor order; `startAudio`; after-restart hook; `requestMode`; `setSlotLinked`)
- Modify: `src/main.cpp` (`setAppVersion`, `getApplicationVersion`)
- Test: `tests/test_gui_wiring.cpp` (append)

**Interfaces:**
- Consumes: `SessionLogger` (Task 2), `NotchController::NotchEvent`/`setEventSink`/`ClearReason` (Task 3), `NotchListPanel::onVerdict` (Task 4).
- Produces:
  ```cpp
  void MainComponent::setAppVersion (const juce::String& v);        // default "0.0.0-unset"
  juce::String MainComponent::getAppVersion() const;
  static juce::File MainComponent::defaultLogDirectory();           // %APPDATA%/AZSoundtech/HandsFree/logs
  bool MainComponent::startSessionLog (const juce::File& directory);   // idempotent; used by startAudio() and tests
  SessionLogger& MainComponent::getSessionLoggerForTest();
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_gui_wiring.cpp`:

```cpp
//==============================================================================
// Lane D: session log wiring. Spec §4 tests 14-15.

namespace
{
struct LogTempDir
{
    juce::File dir;
    LogTempDir()
    {
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("hf-mainlog-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
    }
    ~LogTempDir() { dir.deleteRecursively(); }
};

std::vector<juce::var> parsedLines (const juce::File& f)
{
    juce::StringArray lines;
    f.readLines (lines);
    lines.removeEmptyStrings();
    std::vector<juce::var> out;
    for (const auto& l : lines)
        out.push_back (juce::JSON::parse (l));
    return out;
}

int indexOfEvent (const std::vector<juce::var>& v, const char* ev, int from = 0)
{
    for (int i = from; i < (int) v.size(); ++i)
        if (v[(std::size_t) i]["ev"].toString() == ev)
            return i;
    return -1;
}
} // namespace

// Spec test 14. Red if FALSE no longer reaches clearNotch with VerdictFalse,
// if the verdict line is not logged, or if it is logged AFTER the clear.
TEST (MainComponentSessionLog, FalseVerdictLogsThenClearsWithVerdictFalse)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LogTempDir tmp;
    MainComponent app;
    ASSERT_TRUE (app.startSessionLog (tmp.dir));
    EXPECT_EQ (app.getAppVersion().toStdString(), "0.0.0-unset");   // D-8: no macro in shared code

    auto* controller = app.getNotchControllerForTest (0);
    ASSERT_NE (controller, nullptr);
    ASSERT_TRUE (controller->setNotch (0, 2, 1200.0, 30.0, -12.0, NotchController::Origin::Manual));
    std::vector<float> hop ((std::size_t) Detector::kHopSize, 0.1f);
    auto& tapL = app.getAudioEngine().getTapBuffer (0, 0);
    auto& tapR = app.getAudioEngine().getTapBuffer (0, 1);
    for (int i = 0; i < 6; ++i)
    {
        tapL.write (hop.data(), hop.size()); tapR.write (hop.data(), hop.size());
        controller->runOnce();
    }
    auto& panel = app.getNotchListPanelForTest();
    panel.setSize (520, 200);
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 1);
    ASSERT_NE (panel.falseButtonForTest (0), nullptr);
    panel.falseButtonForTest (0)->onClick();
    controller->runOnce();   // flushes the Clear event to the sink

    app.getSessionLoggerForTest().stop();
    const auto v = parsedLines (app.getSessionLoggerForTest().currentFile());
    ASSERT_GE (v.size(), 4u);
    EXPECT_EQ (v.front()["ev"].toString(), "session_start");
    EXPECT_EQ (v.front()["app_version"].toString(), "0.0.0-unset");

    const int set = indexOfEvent (v, "notch_set");
    ASSERT_GE (set, 0);
    EXPECT_EQ (v[(std::size_t) set]["origin"].toString(), "manual");
    EXPECT_EQ ((int) v[(std::size_t) set]["slot"], 0);
    EXPECT_EQ ((int) v[(std::size_t) set]["index"], 2);
    EXPECT_FALSE (v[(std::size_t) set].hasProperty ("ctx"));   // manual: no detection context

    const int verdict = indexOfEvent (v, "verdict");
    const int clear   = indexOfEvent (v, "notch_clear");
    ASSERT_GE (verdict, 0); ASSERT_GE (clear, 0);
    EXPECT_LT (verdict, clear);
    EXPECT_EQ (v[(std::size_t) verdict]["verdict"].toString(), "false");
    EXPECT_EQ ((int) v[(std::size_t) verdict]["lane"], 0);
    EXPECT_EQ ((int) v[(std::size_t) verdict]["index"], 2);
    EXPECT_EQ (v[(std::size_t) clear]["reason"].toString(), "verdict_false");
    EXPECT_EQ (v.back()["ev"].toString(), "session_end");

    // And the model agrees: the notch is gone.
    NotchController::SnapshotBuffer snap;
    tapL.write (hop.data(), hop.size()); tapR.write (hop.data(), hop.size());
    controller->runOnce();
    controller->copySnapshot (snap);
    EXPECT_EQ (snap.notchCount, 0u);
}

// A GOOD verdict logs and clears nothing. Red if GOOD starts clearing.
TEST (MainComponentSessionLog, GoodVerdictLogsAndKeepsTheNotch)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LogTempDir tmp;
    MainComponent app;
    ASSERT_TRUE (app.startSessionLog (tmp.dir));
    auto* controller = app.getNotchControllerForTest (0);
    ASSERT_TRUE (controller->setNotch (1, 0, 800.0, 30.0, -12.0, NotchController::Origin::Manual));
    std::vector<float> hop ((std::size_t) Detector::kHopSize, 0.1f);
    for (int i = 0; i < 6; ++i)
    {
        app.getAudioEngine().getTapBuffer (0, 0).write (hop.data(), hop.size());
        app.getAudioEngine().getTapBuffer (0, 1).write (hop.data(), hop.size());
        controller->runOnce();
    }
    auto& panel = app.getNotchListPanelForTest();
    panel.setSize (520, 200);
    panel.refreshFromSnapshot();
    ASSERT_NE (panel.goodButtonForTest (0), nullptr);
    panel.goodButtonForTest (0)->onClick();
    controller->runOnce();
    app.getSessionLoggerForTest().stop();
    const auto v = parsedLines (app.getSessionLoggerForTest().currentFile());
    const int verdict = indexOfEvent (v, "verdict");
    ASSERT_GE (verdict, 0);
    EXPECT_EQ (v[(std::size_t) verdict]["verdict"].toString(), "good");
    EXPECT_EQ ((int) v[(std::size_t) verdict]["lane"], 1);
    EXPECT_EQ (indexOfEvent (v, "notch_clear"), -1);
}

// Mode and tuning changes are logged so a log is self-describing. Red if
// requestMode or the DETECTION strip stop emitting their events.
TEST (MainComponentSessionLog, ModeAndTuningChangesAreLogged)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LogTempDir tmp;
    MainComponent app;
    ASSERT_TRUE (app.startSessionLog (tmp.dir));
    app.requestMode (AudioEngine::Mode::Auto);
    gui::TuningPanel::Params p;
    p.riseReferenceMs = 300; p.persistenceBlocks = 4; p.depthDb = -12; p.q = 20; p.peakinessThreshold = 12.0f;
    ASSERT_TRUE (app.getTuningPanel().onTuningChanged != nullptr);
    app.getTuningPanel().onTuningChanged (p);
    app.getSessionLoggerForTest().stop();
    const auto v = parsedLines (app.getSessionLoggerForTest().currentFile());
    const int mode = indexOfEvent (v, "mode");
    ASSERT_GE (mode, 0);
    EXPECT_EQ (v[(std::size_t) mode]["mode"].toString(), "auto");
    const int tuning = indexOfEvent (v, "tuning");
    ASSERT_GE (tuning, 0);
    EXPECT_EQ ((int) v[(std::size_t) tuning]["slot"], -1);
    EXPECT_EQ ((int) v[(std::size_t) tuning]["rise_ms"], 300);
    EXPECT_EQ ((int) v[(std::size_t) tuning]["persist"], 4);
    EXPECT_EQ ((int) v[(std::size_t) tuning]["q"], 20);
    EXPECT_EQ ((int) v[(std::size_t) tuning]["depth_db"], -12);
    EXPECT_DOUBLE_EQ ((double) v[(std::size_t) tuning]["thr"], 12.0);
}

// Spec test 15. Red if destruction order lets a detector thread hand an
// event to a dead logger (sessionLogger_ must be declared before
// notchControllers_, and the dtor must stop controllers before the logger).
TEST (MainComponentSessionLog, DestroyingTheAppWhileTheDetectorPlacesNotchesDoesNotCrash)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    for (int round = 0; round < 20; ++round)
    {
        LogTempDir tmp;
        MainComponent app;
        ASSERT_TRUE (app.startSessionLog (tmp.dir));
        auto* controller = app.getNotchControllerForTest (0);
        controller->setDetectionActive (true);
        controller->setPersistenceBlocks (1);
        controller->start();

        // A loud tone into both taps while the thread runs (SPSC: this thread
        // is the only writer, the detector the only reader).
        std::vector<float> hop ((std::size_t) Detector::kHopSize);
        double phase = 0.0;
        for (int block = 0; block < 90; ++block)
        {
            for (auto& s : hop) { s = 0.9f * (float) std::sin (phase); phase += 2.0 * 3.14159265358979 * 1000.0 / 48000.0; }
            app.getAudioEngine().getTapBuffer (0, 0).write (hop.data(), hop.size());
            app.getAudioEngine().getTapBuffer (0, 1).write (hop.data(), hop.size());
            juce::Thread::sleep (1);
        }
        // app goes out of scope here, thread still running.
    }
    SUCCEED();
}
```

Add `#include "app/SessionLogger.h"`, `#include <cmath>`, `#include <vector>` at the top if missing.

- [ ] **Step 2: Verify RED**

Run: `cmake --build build --config Release --target HandsFreeTests` → compile errors on `startSessionLog`, `getSessionLoggerForTest`, `getAppVersion`.

- [ ] **Step 3: Implement `MainComponent.h`**

Add `#include "app/SessionLogger.h"`. Public additions (after `savePreset`):

```cpp
    // Lane D. The running app's version, pushed in by main.cpp from
    // JUCE_APPLICATION_VERSION_STRING -- that macro exists only for the app
    // target, and this file is compiled into HandsFreeTests too (spec D-8).
    void setAppVersion (const juce::String& v) { appVersion_ = v; }
    [[nodiscard]] juce::String getAppVersion() const { return appVersion_; }

    // Lane D session log. startAudio() starts it in defaultLogDirectory();
    // tests start it in a temp dir. Idempotent. Never started by the ctor --
    // 26 tests and the snapshot tool build this object and must not touch,
    // let alone prune, the developer's real %APPDATA% logs.
    static juce::File defaultLogDirectory();
    bool startSessionLog (const juce::File& directory);
    [[nodiscard]] SessionLogger& getSessionLoggerForTest() { return sessionLogger_; }
```

Private additions:

```cpp
    // Builds the session_start / device payload from the engine's CURRENT
    // state. Message thread.
    [[nodiscard]] juce::var rigDescription() const;
    void logMode (AudioEngine::Mode mode);
    void logTuning (int slot, double riseMs, int persist, double q, double depthDb, double thr);
    void logDeviceEvent();

    juce::String appVersion_ { "0.0.0-unset" };
```

Member placement — immediately after `JuceMonotonicClock systemClock_;` and BEFORE `notchControllers_`:

```cpp
    // Lane D. Declared BEFORE notchControllers_ for the same reason
    // systemClock_ is: members declared first are destroyed LAST, so the
    // logger is alive while ~NotchController joins each detector thread and
    // flushes its last events into the sink (spec §3.1, "thứ tự hủy").
    SessionLogger sessionLogger_;
```

- [ ] **Step 4: Implement `MainComponent.cpp`**

In the anonymous namespace add the serialiser:

```cpp
// The notch_set / notch_clear line for the session log. A juce::var carries
// the scalar fields (JSON::toString), and the three spectrum arrays are
// spliced in pre-formatted at 3 significant figures (plan decision P-2).
juce::String formatNotchEvent (const NotchController::NotchEvent& e)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("ev", e.kind == NotchController::NotchEvent::Kind::Set ? "notch_set" : "notch_clear");
    o->setProperty ("slot", e.slot);
    o->setProperty ("lane", e.lane);
    o->setProperty ("index", e.index);
    o->setProperty ("hz", e.hz);
    if (e.kind == NotchController::NotchEvent::Kind::Clear)
    {
        o->setProperty ("reason", NotchController::clearReasonName (e.reason));
        o->setProperty ("age_ms", e.ageMs);
        return juce::JSON::toString (juce::var (o), juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::none));
    }
    o->setProperty ("q", e.q);
    o->setProperty ("depth_db", e.depthDb);
    o->setProperty ("origin", NotchController::originName (e.origin));
    auto body = juce::JSON::toString (juce::var (o), juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::none));
    if (! e.hasDetection)
        return body;

    auto* d = new juce::DynamicObject();
    d->setProperty ("score", (double) e.score);
    d->setProperty ("peakiness", (double) e.peakiness);
    d->setProperty ("p_norm", (double) e.pNorm);
    d->setProperty ("rise", (double) e.rise);
    d->setProperty ("novelty", (double) e.novelty);
    d->setProperty ("penalty", (double) e.penalty);
    d->setProperty ("asym", (double) e.asym);
    d->setProperty ("persist_needed", e.persistNeeded);
    d->setProperty ("thr", (double) e.thr);
    d->setProperty ("rise_ref_ms", e.riseRefMs);
    d->setProperty ("ref_age_ms", e.refAgeMs);
    const auto detection = juce::JSON::toString (juce::var (d), juce::JSON::FormatOptions()
                                                     .withSpacing (juce::JSON::Spacing::none)
                                                     .withMaxDecimalPlaces (4));

    juce::String ctx = "{\"bins\":" + juce::String (Detector::kNumBins)
                     + ",\"bin_hz\":" + juce::String (e.binHz, 4)
                     + ",\"now\":" + SessionLogger::magnitudesToJson (e.now.data(), Detector::kNumBins);
    if (e.hasRef)
        ctx += ",\"ref\":" + SessionLogger::magnitudesToJson (e.ref.data(), Detector::kNumBins);
    if (e.hasOther)
        ctx += ",\"other_lane_now\":" + SessionLogger::magnitudesToJson (e.other.data(), Detector::kNumBins);
    ctx += "}";

    // body = {...}, detection = {...}: splice as {body..., detection..., "ctx":{...}}
    return body.dropLastCharacters (1) + "," + detection.substring (1).dropLastCharacters (1)
         + ",\"ctx\":" + ctx + "}";
}

const char* modeName (AudioEngine::Mode m)
{
    switch (m)
    {
        case AudioEngine::Mode::Bypass:     return "bypass";
        case AudioEngine::Mode::Auto:       return "auto";
        case AudioEngine::Mode::Soundcheck: return "soundcheck";
    }
    return "bypass";
}
```

Constructor body, right after `setLookAndFeel (&azLookAndFeel_);`:

```cpp
    // Lane D: every controller's events go to the session log. Set before any
    // start(): the sink is read unlocked on the detector thread. The lambda
    // captures `this`; sessionLogger_ outlives every controller thread by
    // declaration order and by the explicit stop sequence in ~MainComponent.
    for (auto& controller : notchControllers_)
        controller->setEventSink ([this] (const NotchController::NotchEvent& e)
        {
            sessionLogger_.logLine (formatNotchEvent (e));
        });
```

After `notchListPanel_.setSlotTabs (&slotTabs_);`:

```cpp
    // Lane D: the panel reports a verdict; THIS object logs it and, for
    // FALSE, clears the notch with the reason that names why (spec D-2).
    notchListPanel_.onVerdict = [this] (int slot, int lane, int index, float hz, bool good, double ageMs)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("ev", "verdict");
        o->setProperty ("slot", slot);
        o->setProperty ("lane", lane);
        o->setProperty ("index", index);
        o->setProperty ("hz", (double) hz);
        o->setProperty ("verdict", good ? "good" : "false");
        o->setProperty ("age_ms", ageMs);
        sessionLogger_.log (juce::var (o));

        if (! good && slot >= 0 && slot < kMaxSlots)
            notchControllers_[(std::size_t) slot]->clearNotch (lane, index, NotchController::ClearReason::VerdictFalse);
    };
```

In `slotPanel_.onSlotTuningChanged` (the per-slot lambda), after the four setters inside `if (! t.usesGlobal)`: `logTuning (slotIndex, t.riseMs, t.persist, t.q, t.depthDb, t.thr);`.
In `tuningPanel_.onTuningChanged`, after the loop: `logTuning (-1, (double) p.riseReferenceMs, p.persistenceBlocks, (double) p.q, (double) p.depthDb, (double) p.peakinessThreshold);`.
In `devicePanel_.onAfterRestart`, after the controllers restart: `logDeviceEvent();`.

Destructor — new order:

```cpp
MainComponent::~MainComponent()
{
    stopTimer();
    setLookAndFeel (nullptr);
    // §6.5: every detector thread must be dead before the engine tears down.
    // Each stop() also flushes that controller's last events into the logger,
    // which is why the logger is stopped only AFTER this loop.
    for (auto& controller : notchControllers_)
        controller->stop (1000);
    sessionLogger_.stop();
    engine_.stop();
}
```

`startAudio()`, after the controller loop and before `devicePanel_.refresh();`: `startSessionLog (defaultLogDirectory());`.

`requestMode()`, after `engine_.setMode (mode);`: `logMode (engine_.getMode());`.

New functions:

```cpp
juce::File MainComponent::defaultLogDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("AZSoundtech").getChildFile ("HandsFree").getChildFile ("logs");
}

juce::var MainComponent::rigDescription() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("device", engine_.getCurrentDeviceName());
    o->setProperty ("sample_rate", engine_.getCurrentSampleRateHz());
    o->setProperty ("buffer_size", engine_.getCurrentBufferSize());
    juce::Array<juce::var> slots;
    for (int i = 0; i < kMaxSlots; ++i)
    {
        const auto c = engine_.getSlotConfig (i);
        auto* s = new juce::DynamicObject();
        s->setProperty ("index", i);
        s->setProperty ("enabled", c.enabled);
        s->setProperty ("width", c.width);
        s->setProperty ("in",  juce::Array<juce::var> { c.inputChannels[0],  c.inputChannels[1] });
        s->setProperty ("out", juce::Array<juce::var> { c.outputChannels[0], c.outputChannels[1] });
        s->setProperty ("linked", slotLinked_[(std::size_t) i]);
        slots.add (juce::var (s));
    }
    o->setProperty ("slots", slots);
    return juce::var (o);
}

bool MainComponent::startSessionLog (const juce::File& directory)
{
    if (sessionLogger_.isStarted())
        return true;
    auto header = rigDescription();
    header.getDynamicObject()->setProperty ("app_version", appVersion_);
    header.getDynamicObject()->setProperty ("os", juce::SystemStats::getOperatingSystemName());
    if (! sessionLogger_.start (directory, header))
        return false;
    logMode (engine_.getMode());
    const auto& c = *notchControllers_[0];
    logTuning (-1, c.getRiseReferenceMs(), c.getPersistenceBlocks(), c.getNotchQ(), c.getNotchDepthDb(),
               (double) c.getPeakinessThreshold());
    return true;
}

void MainComponent::logMode (AudioEngine::Mode mode)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("ev", "mode");
    o->setProperty ("mode", modeName (mode));
    sessionLogger_.log (juce::var (o));
}

void MainComponent::logTuning (int slot, double riseMs, int persist, double q, double depthDb, double thr)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("ev", "tuning");
    o->setProperty ("slot", slot);
    o->setProperty ("rise_ms", riseMs);
    o->setProperty ("persist", persist);
    o->setProperty ("q", q);
    o->setProperty ("depth_db", depthDb);
    o->setProperty ("thr", thr);
    sessionLogger_.log (juce::var (o));
}

void MainComponent::logDeviceEvent()
{
    auto v = rigDescription();
    v.getDynamicObject()->setProperty ("ev", "device");
    sessionLogger_.log (v);
}
```

(`ev` lands after the rig properties in the `device` line; the reader keys on the property, not its position. `t` is still first.)

`src/main.cpp`: `getApplicationVersion()` returns `JUCE_APPLICATION_VERSION_STRING`; in `MainWindow`'s ctor, before `content->startAudio();`: `content->setAppVersion (JUCE_APPLICATION_VERSION_STRING);`.

- [ ] **Step 5: Build everything, run the wiring tests**

Run: `cmake --build build --config Release && cd build && ctest -C Release --output-on-failure -R "MainComponentSessionLog|MainComponent|GuiWiring"`
Expected: all pass. The whole build (app + tests + tools) must compile — `main.cpp` is only in the app target.

- [ ] **Step 6: Full suite and commit**

Run: `cd build && ctest -C Release` → `100% tests passed` (424 + 4 = 428).

```bash
git add src/app/MainComponent.h src/app/MainComponent.cpp src/main.cpp tests/test_gui_wiring.cpp
git commit -m "feat(app): session log owned by MainComponent; verdicts log then clear; mode/tuning/device events; app version from main.cpp (lane D)"
```

---

### Task 6: `tools/logstats.py`, fixture, unittest, ctest registration

**Files:**
- Create: `tools/logstats.py`, `tools/test_logstats.py`, `tests/fixtures/session-sample.jsonl`
- Modify: `tests/CMakeLists.txt` (append the Python test)

**Interfaces:**
- Produces: `logstats.summarise(path) -> dict` with keys `duration_ms, app_version, device, sample_rate, modes (list of [t, mode]), notches (list of dict: slot, lane, index, hz, origin, set_t, clear_t, clear_reason, verdict, age_ms), recurrence (list of [hz_center, count], count desc), verdict_total, false_count, false_ratio, unrated_count, unrated_ratio, dropped_events`; `main(argv)` prints the tables.

- [ ] **Step 1: Write the fixture and the failing unittest**

`tests/fixtures/session-sample.jsonl` (bins deliberately tiny — the tool must not assume 1025):

```
{"t":0,"ev":"session_start","app_version":"1.1.2","os":"Windows 11","device":"Fixture ASIO","sample_rate":48000,"buffer_size":256,"slots":[{"index":0,"enabled":true,"width":2,"in":[0,1],"out":[0,1],"linked":false}]}
{"t":1,"ev":"mode","mode":"bypass"}
{"t":2,"ev":"tuning","slot":-1,"rise_ms":250,"persist":3,"q":30,"depth_db":-18,"thr":10}
{"t":5000,"ev":"mode","mode":"auto"}
{"t":8000,"ev":"notch_set","slot":0,"lane":0,"index":0,"hz":1007.8,"q":30,"depth_db":-18,"origin":"detector","score":0.91,"peakiness":22.5,"p_norm":0.14,"rise":1,"novelty":0.8,"penalty":1,"asym":1,"persist_needed":3,"thr":10,"rise_ref_ms":250,"ref_age_ms":117.3,"ctx":{"bins":8,"bin_hz":23.4375,"now":[0.01,0.02,0.9,0.02,0.01,0.01,0.01,0.01],"ref":[0.01,0.01,0.05,0.01,0.01,0.01,0.01,0.01],"other_lane_now":[0.01,0.01,0.02,0.01,0.01,0.01,0.01,0.01]}}
{"t":9000,"ev":"verdict","slot":0,"lane":0,"index":0,"hz":1007.8,"verdict":"good","age_ms":1000}
{"t":12000,"ev":"notch_set","slot":0,"lane":1,"index":0,"hz":2500,"q":30,"depth_db":-18,"origin":"detector","score":0.75,"peakiness":12.1,"p_norm":0.02,"rise":1,"novelty":0.6,"penalty":1,"asym":1,"persist_needed":3,"thr":10,"rise_ref_ms":250,"ref_age_ms":128.0,"ctx":{"bins":8,"bin_hz":23.4375,"now":[0.01,0.02,0.02,0.6,0.01,0.01,0.01,0.01],"ref":[0.01,0.01,0.01,0.4,0.01,0.01,0.01,0.01]}}
{"t":14000,"ev":"verdict","slot":0,"lane":1,"index":0,"hz":2500,"verdict":"false","age_ms":2000}
{"t":14005,"ev":"notch_clear","slot":0,"lane":1,"index":0,"hz":2500,"reason":"verdict_false","age_ms":2005}
{"t":20000,"ev":"notch_set","slot":0,"lane":0,"index":1,"hz":3200,"q":30,"depth_db":-18,"origin":"manual"}
{"t":51000,"ev":"notch_clear","slot":0,"lane":0,"index":1,"hz":3200,"reason":"auto_release","age_ms":31000}
{"t":60000,"ev":"notch_set","slot":0,"lane":0,"index":1,"hz":1010.2,"q":30,"depth_db":-18,"origin":"detector","score":0.8,"peakiness":15,"p_norm":0.05,"rise":1,"novelty":0.7,"penalty":1,"asym":1,"persist_needed":3,"thr":10,"rise_ref_ms":250,"ref_age_ms":120.0,"ctx":{"bins":8,"bin_hz":23.4375,"now":[0.01,0.02,0.7,0.02,0.01,0.01,0.01,0.01],"ref":[0.01,0.01,0.04,0.01,0.01,0.01,0.01,0.01]}}
{"t":65000,"ev":"session_end","dropped_events":0}
```

`tools/test_logstats.py`:

```python
"""Lane D, spec §4 test 16: logstats on the checked-in fixture.

Red if summarise() miscounts notches, verdicts, the false ratio, the unrated
ratio, or the recurrence grouping (1007.8 Hz and 1010.2 Hz are the same
23.4375 Hz bin and must group together).
"""
import os
import unittest

import logstats

FIXTURE = os.path.join(os.path.dirname(__file__), "..", "tests", "fixtures", "session-sample.jsonl")


class LogStatsFixture(unittest.TestCase):
    def setUp(self):
        self.s = logstats.summarise(FIXTURE)

    def test_header(self):
        self.assertEqual(self.s["app_version"], "1.1.2")
        self.assertEqual(self.s["device"], "Fixture ASIO")
        self.assertEqual(self.s["sample_rate"], 48000)
        self.assertEqual(self.s["duration_ms"], 65000)
        self.assertEqual(self.s["modes"], [[1, "bypass"], [5000, "auto"]])

    def test_notch_table(self):
        n = self.s["notches"]
        self.assertEqual(len(n), 4)
        self.assertEqual([x["hz"] for x in n], [1007.8, 2500, 3200, 1010.2])
        self.assertEqual(n[0]["verdict"], "good")
        self.assertIsNone(n[0]["clear_reason"])
        self.assertEqual(n[1]["verdict"], "false")
        self.assertEqual(n[1]["clear_reason"], "verdict_false")
        self.assertEqual(n[2]["origin"], "manual")
        self.assertEqual(n[2]["clear_reason"], "auto_release")
        self.assertEqual(n[2]["age_ms"], 31000)
        self.assertIsNone(n[3]["verdict"])

    def test_ratios(self):
        self.assertEqual(self.s["verdict_total"], 2)
        self.assertEqual(self.s["false_count"], 1)
        self.assertAlmostEqual(self.s["false_ratio"], 0.5)
        self.assertEqual(self.s["unrated_count"], 2)
        self.assertAlmostEqual(self.s["unrated_ratio"], 0.5)

    def test_recurrence_groups_by_bin(self):
        top = self.s["recurrence"][0]
        self.assertEqual(top[1], 2)
        self.assertAlmostEqual(top[0], 1009.0, delta=15.0)

    def test_main_prints_without_error(self):
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = logstats.main([FIXTURE])
        self.assertEqual(rc, 0)
        self.assertIn("notches: 4", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
```

`tests/CMakeLists.txt` — append:

```cmake
# Lane D: the log summariser runs against its fixture when a Python 3 is on
# the machine; otherwise the test is reported as SKIPPED, not silently absent.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME LogStats.FixtureSummary
             COMMAND ${Python3_EXECUTABLE} -m unittest -v test_logstats
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools)
else()
    add_test(NAME LogStats.FixtureSummary COMMAND ${CMAKE_COMMAND} -E echo "SKIP: no Python 3 interpreter found")
    set_tests_properties(LogStats.FixtureSummary PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP")
endif()
```

- [ ] **Step 2: Verify RED**

Run: `cd tools && python -m unittest -v test_logstats` → `ModuleNotFoundError: No module named 'logstats'`.

- [ ] **Step 3: Implement `tools/logstats.py`**

```python
#!/usr/bin/env python3
"""Summarise a Hands-free session log (lane D, spec §3.5). Stdlib only.

    python tools/logstats.py <session-YYYYMMDD-HHMMSS.jsonl>

Prints: session header and mode timeline; one row per notch (slot, lane, Hz,
origin, held age, verdict, clear reason); recurrence (Hz grouped to +-1 bin);
and the false / rated / unrated ratios. Nothing here reads audio -- there is
none in the log.
"""
import json
import sys
from collections import Counter

DEFAULT_BIN_HZ = 48000.0 / 2048.0


def _read(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                yield json.loads(line)


def summarise(path):
    events = list(_read(path))
    s = {"duration_ms": 0, "app_version": None, "device": None, "sample_rate": None,
         "modes": [], "notches": [], "recurrence": [], "verdict_total": 0,
         "false_count": 0, "false_ratio": 0.0, "unrated_count": 0, "unrated_ratio": 0.0,
         "dropped_events": 0}
    bin_hz = DEFAULT_BIN_HZ
    open_by_key = {}   # (slot, lane, index) -> notch dict currently active

    for e in events:
        ev = e.get("ev")
        t = e.get("t", 0)
        s["duration_ms"] = max(s["duration_ms"], t)
        if ev == "session_start":
            s["app_version"] = e.get("app_version")
            s["device"] = e.get("device")
            s["sample_rate"] = e.get("sample_rate")
            if s["sample_rate"]:
                bin_hz = float(s["sample_rate"]) / 2048.0
        elif ev == "mode":
            s["modes"].append([t, e.get("mode")])
        elif ev == "notch_set":
            key = (e.get("slot"), e.get("lane"), e.get("index"))
            n = {"slot": e.get("slot"), "lane": e.get("lane"), "index": e.get("index"),
                 "hz": e.get("hz"), "origin": e.get("origin"), "set_t": t, "clear_t": None,
                 "clear_reason": None, "verdict": None, "age_ms": None,
                 "score": e.get("score")}
            ctx = e.get("ctx") or {}
            if ctx.get("bin_hz"):
                bin_hz = float(ctx["bin_hz"])
            s["notches"].append(n)
            open_by_key[key] = n
        elif ev == "verdict":
            key = (e.get("slot"), e.get("lane"), e.get("index"))
            n = open_by_key.get(key)
            if n is not None:
                n["verdict"] = e.get("verdict")
        elif ev == "notch_clear":
            key = (e.get("slot"), e.get("lane"), e.get("index"))
            n = open_by_key.pop(key, None)
            if n is not None:
                n["clear_t"] = t
                n["clear_reason"] = e.get("reason")
                n["age_ms"] = e.get("age_ms")
        elif ev == "session_end":
            s["dropped_events"] = e.get("dropped_events", 0)

    for n in s["notches"]:
        if n["age_ms"] is None and n["clear_t"] is not None:
            n["age_ms"] = n["clear_t"] - n["set_t"]

    counts = Counter()
    centres = {}
    for n in s["notches"]:
        if n["hz"] is None:
            continue
        b = int(round(float(n["hz"]) / bin_hz))
        counts[b] += 1
        centres.setdefault(b, []).append(float(n["hz"]))
    s["recurrence"] = [[sum(centres[b]) / len(centres[b]), c]
                       for b, c in counts.most_common()]

    rated = [n for n in s["notches"] if n["verdict"] in ("good", "false")]
    s["verdict_total"] = len(rated)
    s["false_count"] = sum(1 for n in rated if n["verdict"] == "false")
    s["false_ratio"] = (s["false_count"] / s["verdict_total"]) if s["verdict_total"] else 0.0
    s["unrated_count"] = len(s["notches"]) - s["verdict_total"]
    s["unrated_ratio"] = (s["unrated_count"] / len(s["notches"])) if s["notches"] else 0.0
    return s


def _fmt_ms(ms):
    if ms is None:
        return "-"
    return "%.1fs" % (ms / 1000.0)


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    s = summarise(argv[0])
    print("session  app %s  device %s  rate %s  duration %s  dropped %s" % (
        s["app_version"], s["device"], s["sample_rate"], _fmt_ms(s["duration_ms"]), s["dropped_events"]))
    print("modes    " + "  ".join("%s@%s" % (m, _fmt_ms(t)) for t, m in s["modes"]))
    print("notches: %d" % len(s["notches"]))
    print("  %-4s %-4s %-9s %-10s %-8s %-7s %s" % ("slot", "lane", "hz", "origin", "held", "verdict", "cleared by"))
    for n in s["notches"]:
        print("  %-4s %-4s %-9s %-10s %-8s %-7s %s" % (
            n["slot"], "L" if n["lane"] == 0 else "R", "%.1f" % float(n["hz"]), n["origin"],
            _fmt_ms(n["age_ms"]), n["verdict"] or "-", n["clear_reason"] or "-"))
    print("recurrence (Hz +-1 bin):")
    for hz, c in s["recurrence"]:
        if c > 1:
            print("  %.1f Hz  x%d" % (hz, c))
    print("verdicts: %d rated, %d FALSE (%.0f%%), %d unrated (%.0f%%)" % (
        s["verdict_total"], s["false_count"], 100.0 * s["false_ratio"],
        s["unrated_count"], 100.0 * s["unrated_ratio"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

- [ ] **Step 4: Run the unittest, then register in ctest**

Run: `cd tools && python -m unittest -v test_logstats` → 5 tests OK.
Run: `cmake -B build -G "Visual Studio 18 2026" -A x64 && cd build && ctest -C Release --output-on-failure -R LogStats` → `LogStats.FixtureSummary ... Passed`.
Also run the tool by hand on the fixture and paste the output into the task report: `python tools/logstats.py tests/fixtures/session-sample.jsonl`.

- [ ] **Step 5: Full suite and commit**

Run: `cd build && ctest -C Release` → `100% tests passed` (428 + 1 = 429).

```bash
git add tools/logstats.py tools/test_logstats.py tests/fixtures/session-sample.jsonl tests/CMakeLists.txt
git commit -m "feat(tools): logstats.py summarises a session log; fixture + unittest gated in ctest (lane D)"
```

---

### Task 7: Snapshot tool shows the three verdict states, screenshot read back

**Files:**
- Modify: `tools/snapshot.cpp:225-283` (the live scene)
- Output: `shots/console-live.png`, `shots/console-idle.png` (not committed; sent to the owner)

**Interfaces:**
- Consumes: `NotchListPanel::goodButtonForTest/falseButtonForTest` (Task 4), `MainComponent::onVerdict` wiring (Task 5).

- [ ] **Step 1: Amend the live scene**

After the final `pump (tapL, tapR, *controller, 4, hop);` + the two `refreshFromSnapshot()` calls and before `shoot (...console-live.png)`, add:

```cpp
    // Lane D: one row unrated, one GOOD, one FALSE just pressed. The FALSE
    // notch is cleared through MainComponent -> clearNotch(VerdictFalse) into
    // the controller's outbox; nothing pumps runOnce() after the click, so
    // the row is still in the snapshot and shows its verdict text -- exactly
    // the "notch đang biến mất" frame the spec asks for.
    {
        auto& panel = app.getNotchListPanelForTest();
        auto rowOf = [&] (const juce::String& freqText)
        {
            for (int i = 0; i < panel.rowCountForTest(); ++i)
                if (panel.rowForTest (i).freq == freqText) return i;
            return -1;
        };
        const int rowGood  = rowOf ("247 Hz");
        const int rowFalse = rowOf ("1.9 kHz");
        if (rowGood < 0 || rowFalse < 0 || panel.goodButtonForTest (rowGood) == nullptr
            || panel.falseButtonForTest (rowFalse) == nullptr)
        {
            std::cerr << "verdict rows not found in the notch table" << std::endl;
            return 1;
        }
        panel.goodButtonForTest (rowGood)->onClick();
        panel.falseButtonForTest (rowFalse)->onClick();
    }
```

Update the file's header comment (`console-live.png` description) to mention the verdict states.

- [ ] **Step 2: Build the tool and render**

Run:
```
cmake --build build --config Release --target HandsFreeSnapshot
build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast
```
Expected: exit 0, `shots/console-idle.png` and `shots/console-live.png` written.

- [ ] **Step 3: READ the image back**

Open `shots/console-live.png` with the Read tool and confirm, in the ACTIVE NOTCHES table: a VERDICT header; the 1.2 kHz row shows two buttons reading `GOOD` and `FALSE` (not blank — the JUCE 9 ctor trap); the 247 Hz row reads `GOOD`; the 1.9 kHz row reads `FALSE` in the accent colour; the HELD column still shows its age text untruncated; no column caption is ellipsised. If anything is wrong, fix the panel (Task 4 code) and re-render. Send both PNGs to the owner with `SendUserFile` — a GUI change is not reported without a picture (CLAUDE.md).

- [ ] **Step 4: Commit**

```bash
git add tools/snapshot.cpp
git commit -m "chore(snapshot): console-live shows unrated, GOOD and FALSE verdict rows (lane D)"
```

---

### Task 8: Docs, memory, roadmap, release note

**Files:**
- Modify: `docs/KY-THUAT-CHONG-HU.md` (new §8 before the status §7 becomes §9? — no: insert **§8 "Log session và nhãn"** AFTER §7 and renumber nothing else; update §7's test count and "Còn mở" list), `docs/GIOI-THIEU.md` (new section "Chấm notch để app học" after "Theo dõi khi chạy"; the "Tính năng chính" list gets one bullet), `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` (Trạng thái row D; the "Lane C mở khi ≥ 300 verdict từ ≥ 3 session" note in the lane table), `docs/superpowers/specs/2026-09-05-data-loop-design.md` (header status line → "đã thực thi, xem plan"; add a §8 "Sai lệch khi thực thi" listing P-1..P-9)
- Create: `docs/release-notes/1.1.2-alpha.md`, `memory/data-loop-lessons-2026-09-05.md`; index the memory in `memory/MEMORY.md`

- [ ] **Step 1: `docs/KY-THUAT-CHONG-HU.md` §8**

Insert after §7:

```markdown
## 8. Log session và nhãn oan/đúng (lane D, 1.1.2)

**Mức thay đổi level: 0 dB.** Lane này chỉ quan sát và ghi; không chạm
audio callback, không đổi hệ số lọc, không đổi chính sách đặt notch.

Mỗi lần chạy app ghi một file `%APPDATA%\AZSoundtech\HandsFree\logs\
session-YYYYMMDD-HHMMSS.jsonl` (giữ 30 file mới nhất). Mỗi dòng là một
object JSON có `t` (ms từ lúc mở device) và `ev`:

| `ev` | Khi nào | Nội dung chính |
|---|---|---|
| `session_start` | mở device | phiên bản app, OS, device, sample rate, buffer, cấu hình 8 slot (`width`, `in`, `out`, `linked`) |
| `device` | mỗi lần restart device | như trên, trừ phiên bản/OS |
| `mode` | đổi Bypass/Auto/Soundcheck | `mode` |
| `tuning` | đổi DETECTION toàn cục (`slot: -1`) hoặc per-slot | `rise_ms`, `persist`, `q`, `depth_db`, `thr` |
| `notch_set` | detector/preset/manual đặt notch | slot, làn, index, Hz, Q, depth, `origin`; với detector thêm điểm số tách trục (`score`, `peakiness`, `p_norm`, `rise`, `novelty`, `penalty`, `asym`) và **`ctx`**: `now` (phổ 1025 bin vừa phân tích), `ref` (phổ mà trục rise THỰC SỰ so sánh, kèm `ref_age_ms`), `other_lane_now` nếu slot stereo — mỗi số 3 chữ số có nghĩa |
| `notch_clear` | notch rời model | `reason`: `manual` / `clear_all` / `auto_release` / `width_change` / `verdict_false` / `partial_apply_unwind`, `age_ms` |
| `verdict` | bấm GOOD / FALSE | `verdict`, `age_ms`; FALSE kéo theo `notch_clear` với `reason: verdict_false` |
| `session_end` | đóng app | `dropped_events` |

**Không có audio thô trong log**, không có tên người test, không gửi đi
đâu. Phổ magnitude không nghịch đảo ra tiếng được.

Cơ chế: `NotchController` ghi sự kiện vào một outbox **dưới** `modelMutex_`
rồi giao cho sink **ngoài** lock ở `flushOutbox()` (detector thread) — nên
message thread bấm FALSE không bao giờ chờ I/O, và sink được phép gọi lại
`clearNotch`. `SessionLogger` có thread ghi riêng, hàng đợi 4096 dòng, quá
thì bỏ và đếm (không bao giờ chặn detector). `session_start`/`session_end`
ghi thẳng xuống file, không qua hàng đợi.

Đọc nhanh một file: `python tools/logstats.py <file.jsonl>` — thời lượng,
device, timeline mode, bảng notch (verdict, lý do clear), tần số tái phát
(gộp ±1 bin), tỉ lệ FALSE và tỉ lệ chưa chấm.
```

Update §7: the suite count (paste the real ctest total from Task 7's build), add "(lane D) nút GOOD/FALSE trên bảng ACTIVE NOTCHES + log session JSONL" to what landed, and the safety table gets a row: `| Logger chặn detector thread | Hàng đợi có cap, bỏ + đếm; sink gọi ngoài modelMutex_ |`.

- [ ] **Step 2: `docs/GIOI-THIEU.md`**

Add to "Tính năng chính": `- **Chấm notch GOOD/FALSE** ngay trên bảng ACTIVE NOTCHES; FALSE gỡ notch luôn. Mọi lần đặt/gỡ notch được ghi vào log session (phổ + điểm số, không có audio) để bản sau học từ dữ liệu thật.`

New section after "Theo dõi khi chạy":

```markdown
## Chấm notch để app học

Mỗi dòng trong **ACTIVE NOTCHES** có hai nút: bấm **FALSE** khi app cắt
oan một nốt nhạc (notch được gỡ ngay), bấm **GOOD** khi nó cắt đúng tiếng
hú. Không bắt buộc, nhưng mỗi cú bấm là một nhãn cho bộ phân loại ở bản
sau. Log nằm ở `%APPDATA%\AZSoundtech\HandsFree\logs\` — chỉ có phổ và điểm
số, **không có audio**; gửi file khi được hỏi.
```

- [ ] **Step 3: Release note `docs/release-notes/1.1.2-alpha.md`**

```markdown
# Hands-free 1.1.2 alpha — nút GOOD/FALSE và log session

**Mức thay đổi level dự kiến: 0 dB.** Bản này không đổi gì trên đường audio;
chỉ thêm quan sát và ghi.

## Có gì mới

- **Hai nút trên mỗi dòng ACTIVE NOTCHES**: bấm **FALSE** khi app cắt oan
  nốt nhạc — notch gỡ ngay; bấm **GOOD** khi cắt đúng hú. Bấm xong dòng hiện
  chữ GOOD/FALSE thay cho nút.
- **Log session** tại `%APPDATA%\AZSoundtech\HandsFree\logs\`, một file
  `.jsonl` mỗi lần mở app, giữ 30 file mới nhất. Ghi: device, mode, tuning,
  mỗi notch đặt/gỡ kèm phổ tại thời điểm quyết định và điểm số, và verdict.
  **Không có audio trong log**, không có tên ai, không gửi đi đâu.

## Kịch bản test (5 phút)

1. Mở app, chọn device, Auto. Gây hú → dòng xuất hiện → bấm **GOOD**.
2. Bật nhạc có nốt kéo dài cho tới khi app cắt oan → bấm **FALSE** → notch
   biến mất, nhạc trở lại.
3. Đóng app. Mở thư mục log, gửi file `session-...jsonl` mới nhất kèm mô tả
   ngắn (bài gì, mic gì) khi được hỏi.

Càng nhiều verdict càng tốt: bộ phân loại hú/nhạc ở bản sau mở khi có ≥ 300
verdict từ ≥ 3 buổi khác nhau.
```

- [ ] **Step 4: Roadmap status, spec status, memory note**

Roadmap `Trạng thái` row D: `| D | **đã làm xong** trên nhánh feat/data-loop (suite N/N, verifier độc lập sạch); release 1.1.2 alpha; lane C mở khi ≥ 300 verdict từ ≥ 3 session | 2026-09-05 |` (fill N from the real ctest run). Row C: `chờ D nhãn (≥ 300 verdict / ≥ 3 session)`.

Spec header: `**Trạng thái:** đã thực thi 2026-09-05 — plan `docs/superpowers/plans/2026-09-05-data-loop.md`, sai lệch có chủ ý P-1..P-9 ghi trong plan.`

`memory/data-loop-lessons-2026-09-05.md` — record what was non-obvious during execution (at minimum: `juce::JSON` double serialisation forced pre-formatted arrays; `triggerClick()` is async in the headless suite so tests call `onClick()`; the logger must not start in the ctor because tests would prune real logs; `stop()` flushes events after the join, which is what makes "thread never started" and "thread running" both deliver). Index it in `memory/MEMORY.md` with one line.

- [ ] **Step 5: Commit docs**

```bash
git add docs/KY-THUAT-CHONG-HU.md docs/GIOI-THIEU.md docs/release-notes/1.1.2-alpha.md docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md docs/superpowers/specs/2026-09-05-data-loop-design.md memory/data-loop-lessons-2026-09-05.md memory/MEMORY.md
git commit -m "docs: lane D — log session, verdict buttons, tester note 1.1.2, roadmap status, memory"
```

---

### Task 9 (main session, not a subagent): independent verification, release, closeout

- [ ] **Step 1: Independent verifier** — dispatch `az-harness:verifier` (read-only tools) with the spec's §4 list and this plan; it must read the real files and re-run `ctest -C Release` itself. Any finding goes back to a fresh implementer subagent, then the verifier re-checks.
- [ ] **Step 2: Release** — from the worktree root: `pwsh -File installer\release-alpha.ps1` (patch bump 1.1.1 → 1.1.2; the suite gates it). Paste the tail of its output. Commit the `CMakeLists.txt` version bump: `git add CMakeLists.txt && git commit -m "chore: release 1.1.2 to alpha (lane D)"`.
- [ ] **Step 3: Delete `.superpowers/sdd/.gitignore` if present**, commit the SDD workspace (`.superpowers/sdd/2026-09-05-data-loop/`) with explicit paths.
- [ ] **Step 4: Report** — build + ctest output pasted, "0 dB level change" stated, both PNGs sent, verifier verdict quoted, release path on `Z:\My Drive\RELEASE\ALPHA TEST` named. Merge only if the owner says "merge".

---

## Self-review

**Spec coverage.** §2 goals 1–4 → Tasks 4, 2, 3+5, 6. §3.1 `SessionLogger` → Task 2 (API deviation P-1/P-2/P-3 recorded). §3.1 version via `main.cpp` → Task 5. §3.1 destruction order → Task 5 dtor + test 15. §3.2 event table → Task 5 `formatNotchEvent`/`logMode`/`logTuning`/`rigDescription` (+ `device` event, P-7). `ScoreBreakdown` → Task 1. §3.3 sink/outbox/reasons/`setWidth` → Task 3 (setWidth already cleared lane-1 notches on main; only the reason is new). §3.4 GUI → Task 4, image → Task 7. §3.5 tool → Task 6. §3.6 docs → Task 8. §4 tests 1–16 → 1–4: Task 2; 5–6: Task 1; 7–11: Task 3; 12–13: Task 4; 14–15: Task 5; 16: Task 6. §5 release + roadmap → Tasks 8–9. §6 D-1..D-9 → all honoured (D-3 via `%.3g`; D-9 via `RowButtons`).

**Placeholder scan.** No TBD/TODO; every code step has code; the "..." inside Task 1/3 code blocks marks *existing lines that stay verbatim*, and each such block names the exact insertion/replacement points.

**Type consistency.** `NotchController::NotchEvent` fields used in Task 5's `formatNotchEvent` (`kind`, `slot`, `lane`, `index`, `hz`, `q`, `depthDb`, `origin`, `reason`, `ageMs`, `hasDetection`, `score`, `peakiness`, `pNorm`, `rise`, `novelty`, `penalty`, `asym`, `persistNeeded`, `thr`, `riseRefMs`, `binHz`, `refAgeMs`, `hasRef`, `hasOther`, `now`, `ref`, `other`) match Task 3's struct. `onVerdict` arity (6) matches between Task 4 header, Task 4 tests, Task 5 lambda, and Task 7. `SessionLogger::start (File, var)` matches Tasks 2 and 5. `scoreCandidateDetailed` name matches Tasks 1 and 3. `clearReasonName`/`originName` strings match the fixture and `logstats.py`.
