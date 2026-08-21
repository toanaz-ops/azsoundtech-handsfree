# Fix pass 1: defects found in the independent review of Tasks 8-10

## Where these came from

An independent reviewer audited commits `7c1718a`, `0f7d480`, `fe126cb` and
returned CHANGES_REQUIRED with execution evidence. The orchestrator
independently re-verified the three most severe findings before writing this
brief; all three are real. This brief fixes them plus four cheap ones that
would otherwise become bugs the moment Tasks 13-14 wire things together.

Full review is in the session record. What matters is below.

**Two commits, in this order.** Part A is safety and must land even if Part B
runs into trouble. Do not combine them.

---

# PART A — Safety (commit 1)

## A1. The notch chain enters a permanent denormal limit cycle on silence

**Where:** `src/app/AudioEngine.cpp`, the body of
`audioDeviceIOCallbackWithContext`. Root cause is `src/dsp/Biquad.cpp:48-55`.

**What happens:** a notch biquad at Q=10 has poles at radius ~0.993. On digital
silence the Direct-Form-II-transposed state decays exponentially into the
subnormal range and then *never reaches zero* — the feedback term
`z1_ = b1*x - a1_*y + z2_` keeps rounding it back into the subnormals. x87/SSE
subnormal arithmetic traps to microcode and costs ~50x.

The reviewer measured it, 16 active notches, this project's exact MSVC Release
flags: 1.7 ms per second of audio for the first 3 seconds, then **94 ms per
second from second 4 onward, permanently**. Only `Biquad::reset()` escapes it,
and that is only called on device restart.

This is reachable in normal use: engineer mutes the mic for four seconds after
soundcheck has placed notches. At a 32-sample ASIO buffer it eats ~37% of the
callback budget and xruns become likely.

**Fix:** add `juce::ScopedNoDenormals noDenormals;` as the first statement of
`audioDeviceIOCallbackWithContext`. It sets FTZ/DAZ in MXCSR for the scope and
costs nothing per sample.

Confirm first that no `/fp:fast` or FTZ flag is already set anywhere in the
build — the reviewer checked and found none (`CMAKE_CXX_FLAGS_RELEASE` is
`/O2 /Ob2 /DNDEBUG`; `juce_recommended_config_flags` adds only `/Ox /MP /EHsc`),
but verify rather than take that on trust, because if FTZ were already on, the
finding would be wrong and you should say so instead of applying the fix.

**Test (`tests/test_biquad.cpp`, add to the existing file):**

`Biquad.SilenceDoesNotLeaveDenormalState` — this test must prove the mechanism,
not just that a line of code exists.

1. Construct a notch Biquad at 1 kHz, Q=10, 48 kHz.
2. Feed 4800 samples of a full-scale 1 kHz sine to charge the state.
3. Feed 240000 samples of exact `0.0` (5 seconds).
4. Inside a `juce::ScopedNoDenormals` scope, assert the output of the final
   sample is exactly `0.0` and that `std::fpclassify(out) != FP_SUBNORMAL`.

Then, as a second assertion in the same test or a sibling test, do the same run
**without** `ScopedNoDenormals` and assert the state DOES go subnormal. That
second half documents why the fix is needed and will start failing loudly if a
future compiler flag change makes it moot — at which point someone can delete
it deliberately rather than discovering the hazard again from scratch.

If the "without" half turns out not to reproduce on this toolchain, say so
plainly in your report. Do not delete the test quietly and do not keep the
`ScopedNoDenormals` change unmentioned — the orchestrator needs to know the
finding did not reproduce.

## A2. `setSampleRate` can silently turn a valid notch into a divergent oscillator

**Where:** `src/dsp/Biquad.cpp:21-46` (`setNotchFilter`, no validation at all)
and `src/dsp/NotchChain.cpp:80` (the retarget replays stored `NotchInfo`
against the new rate with no re-validation).

**The maths:** the normalised pole radius is `sqrt((1-alpha)/(1+alpha))` where
`alpha = sin(omega)/(2Q)` and `omega = 2*pi*freq/sampleRate`. If
`freq >= sampleRate/2` then `omega >= pi`, `sin(omega) <= 0`, `alpha < 0`, so
the numerator exceeds 1 and the denominator falls below 1 — **pole radius > 1,
the filter diverges**. The orchestrator re-derived this independently; it is
not a hypothetical.

The reviewer's measured case: notch at 30 kHz set legally at 96 kHz, then
`setSampleRate(44100.0)`. Output exceeds 1e3 within 208 samples (4.7 ms) and
reaches 3.7e77.

**Failure scenario in the product:** soundcheck at 96 kHz, detector parks a
notch at 30 kHz (nothing bounds the detector's bin range today), engineer
switches the device to 44.1 kHz for the show. `audioDeviceAboutToStart` calls
`setSampleRate(44100)` and the chain becomes an oscillator feeding full-scale
garbage into the PA within 5 ms. For a live-sound product this is a
hearing-and-driver-damage failure, not a glitch.

Two more holes in the same function: `Q <= 0` divides by zero giving
`alpha = +-inf` and all-NaN coefficients (NaN output forever, and NaN compares
false against every check so nothing downstream notices); `freq == sampleRate/2`
exactly gives `alpha == 0`, poles ON the unit circle, undamped ringing.

**Fix, two layers:**

1. `Biquad::setNotchFilter` — reject invalid parameters and leave the filter
   untouched rather than computing garbage. Reject when any of these hold:
   `sampleRate <= 0`, `Q <= 0`, `freq <= 0`, `freq >= sampleRate / 2`.
   Return `bool` (`true` = applied) so callers can act on rejection. Update the
   declaration in `src/dsp/Biquad.h` and every existing call site.

2. `NotchChain::setSampleRate` — for each Active notch, if its stored frequency
   no longer satisfies `0 < freq < newRate/2`, **deactivate that notch**
   (set it Idle, retain its stored `NotchInfo` so it can come back if the rate
   goes back up). Do not clamp it to just below the new Nyquist: a notch that
   silently moves to a different frequency is worse than an absent one, because
   the engineer sees a notch placed on a frequency that never rang and loses
   trust in the whole product. Anything above 22 kHz was not audible feedback
   anyway.

Keep both allocation-free. `setNotchFilter` may be reached from the audio
thread via a future `setNotch` call, so no logging, no exceptions, no
`juce::String`.

## A3. Spec §8 currently forbids the behaviour we are keeping — amend it

The project auditor found this and the orchestrator verified it at the source.
`docs/superpowers/specs/2026-08-19-az-soundtech-hands-free-design.md` line 182,
under "8. Lỗi & Edge cases", reads:

> Sample rate thay đổi: clear notch, re-init.

Task 9 implemented the opposite — it preserves every Active notch and recomputes
its coefficients so the notch keeps its frequency in Hz — and
`NotchChain.SetSampleRateKeepsActiveNotchFrequency` in `tests/test_notchchain.cpp`
is a green test asserting that preservation is correct. So the repo currently has
a passing test locking in behaviour the spec forbids.

**The project owner has ruled: keep the notches, guard Nyquist.** That is the
behaviour A2 above specifies, and it is what the code already does once A2's
guard is added. A notch survives a rate change; a notch that no longer fits under
the new Nyquist goes Idle.

Your job here is to make the spec say that, so nobody re-litigates it:

- Amend line 182 to state that a sample-rate change **retargets** active notches
  to preserve their frequency in Hz, and that any notch whose frequency is no
  longer below the new Nyquist is deactivated (retaining its stored parameters).
- Add one sentence of rationale: the acoustic environment has not changed when
  the audio device does, so a notch that was correct before the rate change is
  still correct after it.
- Keep the edit surgical. Do not restructure §8 or touch its other bullets.

Do NOT weaken or delete `SetSampleRateKeepsActiveNotchFrequency` — it now tests
sanctioned behaviour. Add the deactivation tests from A2 alongside it.

**Tests (`tests/test_biquad.cpp` and `tests/test_notchchain.cpp`):**

Work out every expected value before asserting it, and put the arithmetic in
the comment.

- `Biquad.RejectsFrequencyAtOrAboveNyquist` — at 48 kHz, `setNotchFilter(24000, 10, 48000)`
  and `setNotchFilter(30000, 10, 48000)` both return false and leave the filter
  a pass-through (feed an impulse, assert the response is unchanged from before
  the call).
- `Biquad.RejectsNonPositiveQ` — `Q = 0.0` and `Q = -1.0` return false, no NaN
  reaches the output. Assert `std::isfinite` on the response.
- `Biquad.AcceptsFrequencyJustBelowNyquist` — the guard must not be
  over-eager: a notch at `0.49 * sampleRate` still returns true and still
  attenuates. Pin the boundary from both sides or the guard is untested.
- `NotchChain.RetargetDeactivatesNotchAboveNewNyquist` — the reviewer's exact
  case. Notch at 30 kHz active at 96 kHz; `setSampleRate(44100.0)`; assert that
  notch is no longer Active, and — the assertion that actually matters — feed a
  unit impulse followed by 1000 zeros and assert the peak absolute output stays
  below 2.0. Without the fix this reaches ~3.7e77, so the bound has enormous
  headroom and will never be flaky.
- `NotchChain.RetargetKeepsNotchThatStillFits` — notch at 1 kHz active at
  96 kHz, `setSampleRate(44100.0)`, assert it is still Active and still
  attenuates 1 kHz. The deactivation must be surgical, not a blanket clear.

---

# PART B — Plumbing that Tasks 13-14 will otherwise trip over (commit 2)

## B1. `AudioEngine::getTapBuffer()` is declared and never defined

`src/app/AudioEngine.h:64` declares it; there is no definition in
`AudioEngine.cpp`. The orchestrator confirmed this by grep. It links today only
because nothing calls it — the first line of Task 13 that does will fail with
LNK2019. Commit `0f7d480`'s entire stated purpose was to hand the tap to the
detector, and the tap is currently unreachable from outside the class.

**Fix:** define it. While you are there, consider that handing out a non-const
`LockFreeRingBuffer<float>&` puts nothing between a future caller and a second
`write()`, which would break the SPSC contract that the whole design rests on.
If you can express a consumer-only view without inventing a large abstraction,
do; if not, define the accessor as declared and note the risk. Do not build a
type hierarchy for this.

**Test:** any test that actually *calls* `engine.getTapBuffer()` would have
caught this. Adding one is awkward because `AudioEngine` constructs a
`juce::AudioDeviceManager`. If constructing an `AudioEngine` in a unit test
without an audio device turns out to be viable, add the call. If it is not,
say so in your report and explain what you tried — do not fake a test.

## B2. Stale audio is spliced across a sample-rate change

`stop()` (`AudioEngine.cpp:57-69`) and `audioDeviceAboutToStart`
(`AudioEngine.cpp:194-217`) neither drain `tapBuffer_`, and `Detector` has no
way to clear `history_`.

**Consequence:** stop at 48 kHz with ~4000 samples in the ring, restart at
96 kHz. The detector's next `Spectrum` reports `sampleRate = 96000` over a
window whose first half is 48 kHz audio. Every bin→Hz conversion in Tasks 11-14
is wrong by up to an octave for the first several blocks, so the first notches
placed after a rate change land on frequencies that were never ringing.

**Fix:** add a way to drain the ring buffer (a `clear()` on
`LockFreeRingBuffer` is only safe if called when neither thread is running —
`audioDeviceAboutToStart` qualifies, per the Task 8 review's finding that JUCE
inserts the callback only after that returns; document that precondition at the
declaration). Add `Detector::reset()` that zeroes `history_`. Call the drain
from `audioDeviceAboutToStart`. Wiring `Detector::reset()` into the device path
belongs to Task 14 — just provide it and test it here.

**Tests:** `LockFreeRingBuffer.ClearDiscardsPendingData`, and
`Detector.ResetClearsAnalysisWindow` (prime the window with DC 1.0, `reset()`,
feed one hop of DC, and assert the spectrum matches a freshly constructed
detector fed the same single hop).

## B3. Tap overruns are silent and uncounted

`AudioEngine.cpp:190` discards the return of `tapBuffer_.write(...)`.

Dropping is the correct *behaviour* on the audio thread. Discarding the *fact*
is not. A truncated write does not create a gap the detector can detect — it
creates a **splice**, sample N followed immediately by sample N+k. Through a
1024-point Hann window that step is broadband energy in every bin, which is
exactly what Task 11's peakiness scorer is built to react to. A notch would get
placed on a frequency that never fed back, and nothing anywhere would record
that a drop was the cause.

**Fix:** `std::atomic<uint64_t> tapDropCount_` incremented with
`memory_order_relaxed` when `written < numSamples`. One relaxed RMW, no lock,
no allocation. Expose `getTapDropCount()`. The GUI work (Tasks 16-24) can
surface it later.

## B4. `Detector::sampleRate_` is a plain `double` on a cross-thread path

`src/dsp/Detector.h:94`. Written by `setSampleRate` from the device thread,
read by `processLatestBlock` on the detector thread. That is a data race the
moment Task 14 wires it. Not reachable today, so fix it before it is:
`std::atomic<double>`, relaxed ordering is sufficient (it is a single scalar
with no other state depending on it).

Note the irony worth avoiding: `Spectrum::sampleRate` exists specifically so
consumers never read a torn pair — the tear was one level down.

## B5. The short-hop test does not test the short hop

`tests/test_detector.cpp` `PartialReadIsReportedAsPartial` asserts only
`readCount == 200`. It never checks where the samples landed.

The reviewer demonstrated the gap: change `Detector.cpp:54` to slide by the
constant instead of the variable —
`std::memmove(history_.data(), history_.data() + kHopSize, retained * sizeof(float));`
— and **all 27 tests still pass**, while 312 stale samples are left at
`history_[712..1023]` corrupting every spectrum whenever the tap under-runs.

**Fix:** strengthen the test so that mutation fails it. The natural way is to
feed a ramp (sample value == sample index) so every position in the window is
identifiable, then after a full hop followed by a short hop, assert the window
is contiguous and right-aligned. That needs visibility into `history_`; the
cleanest route is to assert on the spectrum of a known signal instead, or to
add a narrow test accessor. Choose one and justify it in your report — do not
add a broad public API just for a test.

**Verify your fix the honest way:** apply the reviewer's exact mutation
yourself, confirm your new test FAILS, then revert the mutation and confirm it
passes. Report both results. A test that catches a mutation you never ran is a
test you are guessing about.

---

## Build and test

Use your own build directory so you do not race other agents:
`C:/Users/id_az/AppData/Local/Temp/claude/D--DEV-CAVE-EP3-PROJECT005-AZ-handsfree/2dded5f4-ffa2-42a0-b637-e29e82dd0995/scratchpad/build-fixpass1`

`python` is available; `numpy` is NOT — use `math`/`cmath`.

All previously passing tests must still pass. Report the exact ctest summary.

## Commits

1. `fix: guard notch coefficients and suppress denormals in the audio callback`
2. `fix: define tap accessor, drain tap on restart, and count tap drops`

Both ending with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.

## Report contract

`.superpowers/sdd/2026-08-19-az-soundtech-hands-free/fix-pass-1-report.md`

- Model used, status, both commit hashes, real ctest summary
- For A1: did the "without ScopedNoDenormals" half reproduce? Numbers.
- For A2: the measured peak output before and after the fix, for the
  30 kHz-at-96 kHz-retargeted-to-44.1 kHz case.
- For B5: the result of running the reviewer's mutation against your new test,
  both before and after reverting it.
- Anything in this brief you concluded was wrong. Two consecutive briefs in
  this project shipped real errors; the reviewer also explicitly cleared several
  things the orchestrator suspected. Being right beats being compliant.
