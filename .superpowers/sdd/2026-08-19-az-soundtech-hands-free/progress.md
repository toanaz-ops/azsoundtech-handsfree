# SDD ledger — plan: D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\docs\superpowers\plans\2026-08-19-az-soundtech-hands-free.md



## Task 1: CMake Project Setup & JUCE Integration

Status: complete (commits cabe715..833d72c, review clean)
- Initial implementation: 4a31fb7
- Fix round 1/5 (1 addressed, 0 open - missing ~* pattern; commit 833d72c)
- Files: CMakeLists.txt, README.md, .gitignore, .github/workflows/build.yml
- Review: Spec ✅, Task quality approved

## Task 2: JUCE & ASIO SDK Integration

Status: complete (commit 833d72c..69d61e5, review clean after clarification)
- Implementation: 69d61e5 
- Reviewer flagged JUCE 6.0.8 < 7.0+ requirement
- Implementer clarified: git notation misleading, actual version is 9.0.1
- Independent verification confirmed: JUCE 9.0.1 ✅ satisfies 7.0+ requirement
- Files: .gitmodules, external/JUCE (submodule), CMakeLists.txt, README.md, .gitignore
- MinGW verification blocked (expected - spec requires MSVC)
- Ruling: Review finding invalid, no fix needed

## Task 3: Minimal JUCE Application Stub

Status: complete (commit 69d61e5..510f93d, review clean)
- Implementation: 510f93d
- Files: src/main.cpp, src/app/MainComponent.{h,cpp}, CMakeLists.txt
- Review: Spec ✅, Task quality approved
- Ruling: Build verification blocked by MinGW vs MSVC (environment constraint per spec). Code reviewed as correct. Runtime verification deferred until MSVC available.

## Task 4: Lock-Free Ring Buffer (SPSC)

Status: complete (commit 510f93d..3910752, review APPROVED)
- Implementation: 3910752
- Files: src/dsp/LockFreeRingBuffer.h (155 lines), tests/test_ringbuffer.cpp (133 lines), tests/CMakeLists.txt (32 lines)
- Tests: 6/6 PASS (WriteAndReadSingleSample, AvailableSpace, Wraparound, MultipleWritesAndReads, ReadEmptyReturnsZero, WriteFullStopsAtCapacity)
- Review: Spec ✅, Task quality approved
- Memory ordering: textbook SPSC-correct (Lamport pattern, acquire/release)
- Reviewer note: Concurrent stress test deferred to Task 5/8 per brief

## Task 8: AudioEngine Class - ASIO I/O

Status: complete (commit f0edbd3..14bb32c)
- Implementer model: opencode/deepseek-v4-flash-free (Tier 1, no escalation)
- Files: src/app/AudioEngine.h (90 lines), src/app/AudioEngine.cpp (208 lines), CMakeLists.txt
- Build verified on MSVC: 18/18 DSP tests pass, 0 warnings
- Key finding: JUCE 9 API uses udioDeviceIOCallbackWithContext (6 args) instead of brief's 5-arg - implementer adapted correctly
- Forward-looking concern: NotchChain needs setSampleRate() API for Task 9+
- Review: Spec ✅, Task quality APPROVED (reviewer verified against JUCE 9 headers directly)
- Real-time safety: verified zero alloc, zero locks, atomics correct, JUCE output contract honored
- 4 Minor issues (non-blocking): error-path asymmetry, magic numbers, doc drift, report inaccuracy
- REQUIRED FOR TASK 9: NotchChain needs setSampleRate() or reconstruction in audioDeviceAboutToStart

## Task 9: Sample-rate retarget + audio tap

Status: complete (commits 72dfb94..0f7d480, reviewed by orchestrator)
- Implementer model: opencode/deepseek-v4-flash-free (Tier 1, no escalation)
- Commits: 7c1718a (rate retarget + 3 tests), 0f7d480 (post-notch tap)
- Tests: 21/21 PASS - verified by orchestrator running HandsFreeTests.exe directly, exit 0
  (not taken from the report: independently re-run)

Ruling: task review performed by the orchestrator, not a fresh reviewer subagent.
Why: the implement subagent returned empty 3 consecutive times (Task 9 impl, review,
review retry). The implementation was salvageable because its commits landed; both
review attempts produced no verdict and no artifact. Rather than dispatch a third
variation into a channel that is not returning, I read the diff directly.
Cost if wrong: this task lacks the independent-reviewer check the other tasks got.
The final whole-branch review must re-examine commits 7c1718a and 0f7d480 with fresh eyes.

Orchestrator review findings (from reading the diff, not the report):
- Spec compliant. Part A guards non-positive rate, recomputes only Active notches from
  stored NotchInfo, then reset()s state. Idempotent. Allocation-free.
- Part C retargets both chains in audioDeviceAboutToStart, before JUCE inserts the
  callback - no synchronization needed, matches the Task 8 review's finding.
- Part B tap: single bulk write from outputChannelData[0] after the channel loop, so it
  is post-notch in DSP mode and the bypass copy in Bypass. No alloc, no lock, return
  value discarded so short writes are tolerated. Does not mutate the output.
- Important (deferred to Task 10): the tap write is gated on input channel 0 being
  non-null, so an input dropout silently writes nothing while the detector keeps reading.
  The detector's timeline would then splice discontinuous audio across an FFT block
  boundary. Task 10 must not assume tap samples are contiguous in time; Task 14's
  auto-release cadence depends on the 512-sample hop being real elapsed time.
- Minor: getTapBuffer() hands out a mutable reference to an SPSC buffer, which invites a
  second writer. Task 10 must only read from it.

## Task 10: Detector Thread - FFT Pipeline

Status: complete (branch `feat/task-10-detector-fft`, commits de21d98..fe126cb)
- Implementer model: claude-opus-5 in the main loop, NOT a subagent. The
  `implement` channel had returned empty 4x in the prior session; re-dispatching
  had no evidence behind it, so the task was done under TDD in-loop instead.
- Commits: de21d98 (JuceHeader fix), fe126cb (Task 10)
- Files: src/dsp/Detector.h (+113), src/dsp/Detector.cpp (+70),
  tests/test_detector.cpp (+154), tests/CMakeLists.txt (+7), CMakeLists.txt (+7)
- Tests: 27/27 PASS on MSVC Release, verified by running ctest directly.
  Both targets build; the app exe links.

Three errors in the v2 brief were found and corrected (full detail in
task-10-report.md):
1. `timeBuffer_` sized kFftSize. juce_FFT.h requires 2 * getSize() for both
   forward entry points, so the brief's buffer overruns by 4 KB every call.
   Implemented as `fftBuffer_` of 2 * kFftSize.
2. The brief's hot path read kFftSize per call (hop 1024) while declaring
   kHopSize = 512, contradicting plan line 114 ("50% overlap -> hop 512").
   Implemented real 50% overlap via a sliding kFftSize `history_` window.
   CONSEQUENCE: `Spectrum::readCount` is now 0..kHopSize, NOT 0..kFftSize.
   Tasks 11/12/14 must compare against kHopSize.
3. Brief test 4 asserted `mag[0] < 0.5*(mag[1]+mag[2]+mag[3])` for DC input.
   JUCE's symmetric normalised Hann gives mag[0]=1024, mag[1]=512.75, so the
   assertion is `1024 < 256`, unpassable. Replaced with the ratio test
   `mag[1]/mag[0] ~= 0.5007`, which a rectangular window fails (ratio 0).

Also fixed a PRE-EXISTING defect: the `HandsFree` app target did not build at
HEAD. `juce_generate_juce_header(HandsFree)` was never in CMakeLists.txt
(`git log -S` finds no commit adding it), so the `<JuceHeader.h>` includes in
main.cpp / MainComponent.h / AudioEngine.h could not resolve from a clean
configure. `build-task8-msvc/` held an uncommitted generated header, which is
why Task 8 could report a verified MSVC build over a broken HEAD.

Ruling: no independent reviewer ran on this task either. Tasks 9 and 10 both
need fresh eyes in the whole-branch review.

## Tasks 5-7: Command queue, Biquad, NotchChain

Status: BACKFILLED LEDGER ENTRY, 2026-08-21. These tasks closed with a report
(`tasks-5-7-report.md`) but NO progress.md section, so nothing recorded that they
were reviewed. The independent audit of 2026-08-21 found the gap and one false
claim inside the report.

- Task 6 (Biquad) and Task 7 (NotchChain) are real: 5 and 10 tests respectively,
  all passing, and the ledger's cumulative test counts (18 -> 21 -> 27) reconcile
  exactly against the actual suite at each point.
- Task 5 (command queue) is NOT delivered. `src/dsp/NotchCommand.h` defines the
  struct and nothing else in the repo references it -- verified by
  `grep -rn NotchCommand src/ tests/ CMakeLists.txt`, which finds it only in its
  own header and in the root CMakeLists source list. The plan requires a queue
  "using LockFreeRingBuffer<NotchCommand>"; no such queue is instantiated anywhere.
- `tasks-5-7-report.md` claims NotchCommand "is exercised by the existing
  LockFreeRingBuffer tests". This is FALSE: `tests/test_ringbuffer.cpp`
  instantiates `LockFreeRingBuffer<int>` only. The struct has zero coverage.

CONSEQUENCE: Task 13 (notch controller) has no control path to build on. Treat
Task 5 as open, not complete.

## Independent review of Tasks 8-10 (2026-08-21)

Two independent reviewers audited the work. Both were told to report only
findings they could attach a concrete failure scenario to, and both explicitly
cleared things the orchestrator suspected -- the ring buffer's memory ordering
(40M-item two-thread torture test, power-of-two AND non-power-of-two capacity),
the Detector's index arithmetic, and the honesty of the two Detector tests
(recomputed independently: Hamming 0.4265, Blackman 0.5959, rectangular 1.15e-16
all fail the 0.01 tolerance, so the Hann test genuinely discriminates).

Verdict: CHANGES_REQUIRED. The orchestrator independently re-verified the four
most severe findings before acting on any of them; all four are real.

CRITICAL 1 -- No `juce::ScopedNoDenormals` anywhere in `src/`. After ~4 s of
digital silence the notch chain enters a permanent denormal limit cycle:
measured 1.703 ms -> 94 ms per second of audio, 16 notches, this project's exact
MSVC Release flags, and it never recovers without `Biquad::reset()`. Reachable by
muting a mic after soundcheck.

CRITICAL 2 -- `Biquad::setNotchFilter` validates nothing. Pole radius is
`sqrt((1-alpha)/(1+alpha))`; when `freq >= sampleRate/2` then `alpha < 0` and the
radius exceeds 1. Measured: notch at 30 kHz set legally at 96 kHz, then
`setSampleRate(44100)` -> output exceeds 1e3 within 208 samples and reaches
3.7e77. `Q <= 0` divides by zero into all-NaN coefficients.

HIGH 3 -- `AudioEngine::getTapBuffer()` is declared at AudioEngine.h:64 and
DEFINED NOWHERE. Found independently by both reviewers and confirmed by the
orchestrator. Task 9 built the tap, Task 10 built its consumer, both are marked
complete, and they cannot be connected: Task 13's first wiring line fails with
LNK2019.

SPEC CONFLICT 4 -- Spec line 182 says "Sample rate thay doi: clear notch,
re-init." Task 9 implements the opposite and a green test locks it in. RULED by
the project owner 2026-08-21: keep the notches and guard Nyquist; the spec is to
be amended to match (see fix-pass-1-brief.md section A3).

Also found, feeding fix-pass-1: stale tap audio spliced across a rate change;
tap overruns silent and uncounted (a splice reads as broadband energy to Task 11);
`Detector::sampleRate_` is a plain double on a cross-thread path; and the
short-hop test does not test the short hop -- the reviewer demonstrated a
one-word mutation of `Detector.cpp:54` that leaves 312 stale samples in the
window while all 27 tests still pass.

AUDIT FINDINGS ABOUT THE LEDGER ITSELF -- the per-task "N/N PASS" figures in this
file are CUMULATIVE SUITE TOTALS, not per-task evidence. Task 9 records "21/21
PASS" while no test anywhere instantiates `AudioEngine`: `tests/CMakeLists.txt`
compiles Biquad, NotchChain and Detector only, so the entire audio I/O layer --
device lifecycle, passthrough contract, null-channel guards, bypass path, tap
write -- has ZERO automated coverage. Spec 9.1's required concurrent ring-buffer
test was deferred to Tasks 5/8, both of which closed without it. `depthDB` is
stored by NotchChain and applied by nothing, so every notch is a full-depth null
and spec 5.1's 6-24 dB range is unimplemented. `origin/main` is 18 commits
behind: the CI in `.github/workflows/build.yml` has NEVER run, which is why the
missing `juce_generate_juce_header` survived from Task 3 to Task 10.

================================================================================
SESSION 2026-08-22 -- Lane D infra, Lanes B and C, depth fix, spec 9.1 test
================================================================================

Suite: 55/55 at session start -> 66/66 at session end. Per this ledger's own
audit finding, that total is NOT the evidence. The 11 NEW tests and the
production change that makes each fail are named below.

INFRA (Lane D) -- all four owner decisions taken and executed. Recorded with
their rejected alternatives in owner-decisions.md (D-01..D-04).

  - main pushed: cabe715..9eabb52, 24 commits. origin was 24 behind, not 22.
  - .superpowers/ un-ignored and committed. The .gitignore line is replaced by
    a comment saying why it must not come back.
  - shared/handoff/ and opencode-harness-diagram.html committed.
  - Four stale build dirs deleted: 553 MB, not the ~1 GB estimated
    (build-review 114M, build-task8 2.0M, build-task8-msvc 435M,
    build-verify 2.0M). build/ kept.
  - Three fully-merged branches deleted.

CI RAN FOR THE FIRST TIME, AND FAILED IN 27 SECONDS.

    CMake Error: Generator "Visual Studio 17 2022"
      could not find any instance of Visual Studio.

  windows-latest is now image windows-2025-vs2026. The workflow pinned a
  toolchain that is not on the runner. Nothing in this repo's code was wrong.
  Fixed by dropping -G so CMake picks the newest VS present; a comment on the
  step says why it must not be re-pinned. Second run: SUCCESS in 10m48s -- the
  first green CI in this project's history. Also bumped checkout v3 -> v4 after
  the run warned v3 is being force-migrated off Node 20.

  Worth noting against this ledger's own history: the local build was green
  throughout. Only CI's clean submodule checkout caught this.

LANE C -- GUI interface audit. docs/superpowers/audits/2026-08-22-gui-interface-audit.md
  21 missing methods across Tasks 16-26. Three are design defects, not merely
  unwritten code:
    1. getCurrentSampleRate() returns the display string "48000 Hz". Task 17's
       ComboBox cannot select from it; Task 25's JSON needs it numeric.
    2. No setSampleRate/setBufferSize exist at all. Task 17 -- one line in the
       plan -- is the most blocked task in the GUI lane.
    3. Task 22's "Locked Time" column has no backing field anywhere.
  Verified Task 30's artefact path against a real build: it is
  build/HandsFree_artefacts/Release/AZ Soundtech Hands-free.exe. The plan says
  build\Release\HandsFree.exe -- wrong folder, wrong stem, and the real name
  has spaces the plan's NSIS File line does not quote.
  Conclusion: 11 AudioEngine methods depend on nothing in Lane B and can land
  now, unblocking Tasks 16/17/18/23 ahead of the bridge.

LANE B -- bridge design. DRAFT, AWAITING OWNER APPROVAL. No bridge code written.
  docs/superpowers/specs/2026-08-22-audio-detector-bridge-design.md
  Governing principle: lock-free machinery is a cost paid for the audio
  thread's benefit; paying it between two non-real-time threads is complexity
  with no payer. Two lock-free channels total, both touching the audio thread;
  mutex everywhere else. This contradicts the plan's Task 19 wording.
  Hole 2 (notch read-back) closes by DELETION: under D-05 the detector is sole
  command author and needs to remember what it commanded, not read it back.
  Departs from plan Task 13 (NotchController as an AudioEngine member):
  ownership cycle, and it would make every behaviour untestable without an
  audio device.

DEFECT FIXED -- depthDB had no path into the filter.
  The handoff said "stored and applied by nothing". It was worse:
  Biquad::setNotchFilter(freq, Q, sampleRate) takes NO depth argument. It is
  the RBJ pure notch, an infinite-depth null. NotchChain's own comment admitted
  depth was "a hint (currently unused at the biquad level)".
  Added the four-argument form: RBJ peakingEQ with negative gain, |H(w0)| = A^2
  = 10^(dB/20) exactly.
  EXPECTED LEVEL CHANGE, stated per CLAUDE.md: residual at the notch frequency
  rises from ~0 to 0.251 of input at the -12.0 dB every caller passes. Strictly
  LESS attenuation than before, never more, so no frequency can be louder than
  the previous build. A human should still confirm at low volume.
  Also refuses positive depthDB: the peaking form is symmetric, so a sign error
  upstream would BOOST the ringing frequency -- a feedback amplifier.

  NEW TESTS (8), each with the production change that breaks it:
   1 Biquad.DepthAttenuatesByExactlyTheRequestedDecibels
       fails if the 4-arg form stops honouring depth
   2 Biquad.DepthIsHonouredAcrossTheSpecifiedRange (-6/-18/-24)
       fails if depth is clamped to one constant -- a single-depth test would not
   3 Biquad.DepthLeavesOffTargetContentAlone
       fails if the finite-depth form widens the affected band
   4 Biquad.RejectsPositiveDepthBecauseItWouldBoostTheRingingFrequency
       fails if the depthDB > 0 guard is removed
   5 Biquad.DepthFormAppliesTheSameParameterGuardsAsThePureNotch
       fails if any of the four stability guards is dropped from the new overload
   6 NotchChain.SetNotchAppliesTheRequestedDepthRatherThanAFullNull
       fails if setNotch reverts to the 3-arg call
   7 NotchChain.RetargetingToANewSampleRateKeepsTheDepth
       fails if setSampleRate retargets through the 3-arg call -- a bug that
       would only surface after a device reopen
   8 NotchChain.SetNotchRejectsAPositiveDepthAndLeavesTheSlotIdle
       fails if the chain activates a slot the biquad refused
  All 8 watched failing first (C2660: function does not take 4 arguments).

TEST DEBT CLOSED -- spec 9.1's concurrent ring-buffer test now exists in the repo.
  NEW TESTS (3): ConcurrentSpsc{LosesNothingWithPowerOfTwoCapacity,
  LosesNothingWithNonPowerOfTwoCapacity, SurvivesACapacityOfOne}.
  Two threads, monotonic sequence, capacities 64/100/1.
  Verified the tests CAN fail rather than assuming it: a one-item-drop mutation
  in the producer was caught by the capacity-100 case ("consumer saw 100 of
  1000000"). The other two stayed green under the same mutation because a
  64-item chunk into a 64-slot ring is an all-or-nothing write -- only the
  non-power-of-two capacity drives partial writes. Recorded in the test.
  The count assertion is load-bearing: without EXPECT_EQ(consumed, count) the
  test is vacuous, since the failure flag stays false if the consumer never ran.
  States plainly what it does NOT prove: on x86-64 weakening the memory
  orderings would still pass here and fail on ARM.

STILL OPEN AFTER THIS SESSION
  - Bridge design needs owner approval before any bridge code.
  - AudioEngine still has no coverage of the passthrough contract (plan Task 9's
    specified loopback test). Needs the Wave 1 accessors to be testable.
  - CLAUDE.md is untracked and not ignored; it appeared this session.
  - Tasks 5, 12-32 remain open. Task 12's stated blocker is dissolved by D-05,
    pending approval of the design that says so.
