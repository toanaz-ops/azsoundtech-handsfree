# Task 7 report — `applySoundcheckResults`

**Status:** DONE. Commits `96c2827` (feature) and `b7965e0` (comment
line-citation fix). Branch `feat/lane-m-active-soundcheck`, worktree
`.claude/worktrees/lane-m-soundcheck-0915`.

**Model actually used:** Opus 5 (1M context), id `claude-opus-5[1m]`. The
commits are stamped `Co-Authored-By: Claude Fable 5.1` because the dispatching
brief required that trailer verbatim; this line is the accurate record.

**Suite:** 652/652 Release, up from the 638 baseline at `3c9563a` (+14). The
brief estimated 619 from a 608 baseline — both numbers were stale.

---

## What was built

`applySoundcheckResults (NotchController&, const std::vector<OutputResult>&)`
at the end of `src/app/SoundcheckController.cpp`, under a MESSAGE THREAD ONLY
banner, with file-local helpers in a second anonymous namespace
(`firstFreeIndexTopDown`, `withinOneBinOfALiveNotch`, `flatSlot`).
`SoundcheckApplyStats` gained one field, `skippedLive`.

Behaviour, in the order the function does it:

1. **Thread guard.** Refuses and reports if a `MessageManager` exists and this
   is not its thread. Not a `jassert` — see divergence 2.
2. **Replace pass.** One entry snapshot; every `Origin::Soundcheck` notch in
   it, **both lanes**, cleared with `ClearReason::SoundcheckReplace` (I-12).
   Each cleared `(lane,index)` is recorded in `clearedThisCall`.
3. **Per candidate:** re-read the snapshot (detector guard, not the allocator)
   → derive LINKED from `snap.linked || snap.laneCount < 2` → invariant-15
   ±1-bin test → allocate top-down via `takenThisCall` → place.
4. **LINKED** = two `setNotch` at one index, all-or-nothing, unwound with
   `ClearReason::PartialApplyUnwind`. **INDEP** = stop at the first `false`,
   no rollback.

### Invariant 15 is enforced here, and it is new work

Task 6 passes `SoundcheckCandidates::pick` an **empty** `liveNotchHz` because
inv 17 forbids the lane M thread reading a `NotchController`. So nothing in the
pipeline was enforcing "a proposal must never land within ±1 bin of a live
notch on that lane" until now. `withinOneBinOfALiveNotch` compares
`LoopGainEstimator::hzToBin` of the candidate against every live notch on the
lanes about to be written, using `snap.sampleRate`. A hit bumps `skippedLive`
**and** `refused` (`skippedLive` is a subset: it says WHY nothing was placed)
and `continue`s — skip, not stop, because the next candidate is a different
frequency.

**Notches this call just cleared are excluded from the test.** The entry
snapshot goes on listing them for up to one hop (~10.7 ms), and a second
soundcheck of the same room finds the same frequencies — that is the normal
case, not the odd one. Without the exclusion every re-run would refuse to
re-place anything it had just removed, and the operator would be told the room
had changed when nothing had. `ARerunMayRePlaceAtTheSameFrequencyItJustCleared`
pins it.

---

## Divergences from the brief (the header won every time)

1. **`SoundcheckApplyStats` and `applySoundcheckResults` were already
   declared** in `src/app/SoundcheckController.h:376-381` — Task 6 pre-declared
   them so Task 7 would add a definition, not a second surface. So the brief
   Step 2 expected failure (`'applySoundcheckResults': identifier not found`)
   is not what RED looks like. The real RED was
   `error LNK2019: unresolved external symbol "struct SoundcheckApplyStats
   __cdecl applySoundcheckResults(class NotchController &, ...)"` followed by
   `LNK1120: 1 unresolved externals`.

2. **The brief `ApplyRunsOnTheMessageThread` is a tautology and cannot fail.**
   It captured `std::this_thread::get_id()`, called the function synchronously
   on that same thread, and asserted the id was unchanged. No implementation —
   on any thread policy — could make that go red. Replaced with a test that
   calls the function from a `std::thread` and asserts nothing was written,
   which required the function to HAVE a guard:

       if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
           && ! juce::MessageManager::existsAndIsCurrentThread())

   The first clause matters: most tests in this TU never create a
   `MessageManager` (only `Rig` carries a `ScopedJuceInitialiser_GUI`), and
   with none there is no message thread to be off. A `jassert` was rejected
   deliberately — a Debug-only trap fires long after the dangling coefficient
   write it is meant to prevent, and this is a refusal a caller can see.

3. **`firstFreeIndexTopDown` lost its fourth parameter.** The brief passed
   `laneCount`, said the scan does not use it, and invited a reviewer to ask
   for it to be dropped. Dropped: `lane < 0` already carries the whole "free on
   every lane" meaning, and the snapshot never lists a lane the slot does not
   drive.

4. **`SoundcheckApplyStats` gained `skippedLive`** — not in the brief, required
   by the invariant-15 instruction in the task.

5. **m-21 include list was already satisfied.** The test TU already includes
   `<thread>`, `<memory>`, `<array>`, `<vector>`. Only `<array>` had to be
   added, to `SoundcheckController.cpp`.

6. **Step 4 (`Results` → `Idle`) was already done by Task 6.**
   `applyRequested()` is at `SoundcheckController.cpp:282`, `dismissRequested()`
   at `:293`. No work.

7. **The brief `NotchController.h` line numbers had drifted about six lines**
   after Tasks 4 and 6 edited that header: `effectiveLinked()` is `:223` not
   `:217`; `analysedLanes()` `:667` not `:655`; `SnapshotBuffer::laneCount`
   `:424` not `:412`; the message-thread banner `:225-230` not `:219-224`;
   `copySnapshot` `:459` not `:447`; `failNextSetNotchOnLaneForTest` `:304` not
   `:298`; the two new `ClearReason` enumerators `:69`/`:75` not `:67-70`.
   Every `.cpp` citation the brief quoted was still accurate (`setNotchImpl`
   `:199`/`:209`/`:243-245`, the snapshot gather `:577`, `firstFreeIndexLocked`
   `:1065`, `firstFreeIndexAllLanesLocked` `:1073`). Commit `b7965e0` carries
   the corrections into the code comments.

8. **The brief `PartialApplyStopsAndReportsRefused` set `candidates[1].q`
   twice** (30.0f, then 0.0f four lines later). Collapsed to one assignment.

9. **On the `effectiveLinked` warning in the task prompt.** The prompt flagged
   `snap.linked || snap.laneCount < 2` as wrong for placement. Read against
   `NotchController.h:223` and `:667` it is exactly right PROVIDED the linked
   branch iterates `0..laneCount-1` rather than a hardcoded two — which it
   does. A mono slot takes the linked branch and writes its ONE lane; nothing
   reaches a lane the slot does not drive.
   `LinkedIsDerivedFromLaneCountNotJustTheSwitch` asserts `activeForTest(1, 15)`
   is FALSE on a mono rig, so a hardcoded two goes red. The variable is named
   `linkedNow`, not `effectiveLinked`, so it is not confused with the
   controller own predicate.

---

## Commands, RED and GREEN

### RED — tests written, no definition

    cmake --build build --config Release --target HandsFreeTests
    ...
    test_soundcheckcontroller.obj : error LNK2019: unresolved external symbol
      "struct SoundcheckApplyStats __cdecl applySoundcheckResults(class
      NotchController &,class std::vector<struct
      SoundcheckController::OutputResult,...> const &)" referenced in function
      "...SoundcheckApply_ApplyRunsOnTheMessageThread_Test::TestBody(void)"
    HandsFreeTests.exe : fatal error LNK1120: 1 unresolved externals

### First run after implementing — one real failure, and not the test fault

`LinkedPairUnwindsWhenTheSecondLaneFails` failed with `placed` 1, `refused` 0,
lane 0 still active and no `PartialApplyUnwind` event. Cause: the test called
`setLinked(true)` AFTER `NotchRig`'s constructor had already pumped, and
`SnapshotBuffer::linked` is republished only inside `runOnce()`'s drain loop
(`NotchController.cpp:641`). The snapshot still read INDEP, the function took
the INDEP branch, and the unwind under test was never reached. Fixed in the
test with an explicit `ASSERT_TRUE (rig.snapshot().linked)` and a comment. See
Concern 1 — this is a real property of the API, not only a fixture detail.

### GREEN

    cd build && ctest -C Release -R "SoundcheckApply|SoundcheckController" --output-on-failure
    100% tests passed, 0 tests failed out of 34

(14 `SoundcheckApply` + 20 `SoundcheckController`.)

    cd build && ctest -C Release
    100% tests passed, 0 tests failed out of 652
    Total Test time (real) =  51.51 sec

Build is warning-clean for the changed files; the only warnings in a full
Release build are the four pre-existing `C4324` alignment-padding warnings in
`src/dsp/LockFreeRingBuffer.h:171,175`.

---

## Mutation results — six mutations, six reds

Each mutation was applied to a pristine copy, built, the named test run, then
the file restored byte-for-byte (`cmp` verified).

| # | Mutation | Test | Result |
|---|---|---|---|
| 1 | `takenThisCall` lookup in `firstFreeIndexTopDown` never fires | `ApplyReReadsTheSnapshotBeforeEachSetNotch` | FAILED — the second placement lands on 15 again |
| 2 | the `PartialApplyUnwind` loop deleted | `LinkedPairUnwindsWhenTheSecondLaneFails` | FAILED — `refused` 0, lane 0 still active, no Clear event |
| 3 | the `SoundcheckReplace` clear + counter deleted | `ARerun*` | FAILED (3) — `ARerunReplacesItsOwnPreviousProposals`, `ARerunReplacesTheWholeSlotNotJustOneLane`, `ARerunMayRePlaceAtTheSameFrequencyItJustCleared` |
| 4 | `withinOneBinOfALiveNotch` returns `false` immediately | `AProposalWithinOneBinOfALiveNotchIsSkipped` | FAILED |
| 5 | the message-thread guard inverted to unreachable | `ApplyRunsOnTheMessageThread` | FAILED — the off-thread call placed a notch |
| 6 | `NotchController.cpp:243` ternary → `origin != Origin::Preset` | `APreventiveNotchIsItsOwnCeiling` | FAILED |

Mutation 3 failing three tests rather than one is the point of
`ARerunMayRePlaceAtTheSameFrequencyItJustCleared`: it is the only one that
distinguishes "the clear happened" from "the clear was recorded".

---

## Self-review

- **Thread banner honoured.** The banner is on the definition, on the header
  declaration, and now on a runtime guard with a test that fails without it.
- **No `NotchController` call from any thread but the message thread.**
  `applySoundcheckResults` is a free function; `SoundcheckController` holds no
  `NotchController` pointer (header thread map, inv 17). Every `NotchController`
  call added in this change sits inside that function, behind the guard.
- **Every index handed out is marked.** `takenThisCall[index] = true` sits
  immediately after the allocation and before any `setNotch`, on both branches
  and on the failure paths.
- **Partial-apply outcomes exactly as ruled.** LINKED: unwind every lane
  already placed, `++refused`, `break`. INDEP: `++refused`, `break`, nothing
  removed. Both pinned by tests, both mutation-checked.
- **Pristine build.** `cmp` against the pre-mutation backups, then a full
  Release build and a full `ctest` before each commit.

---

## Concerns

1. **`SnapshotBuffer::linked` is up to one hop stale (~10.7 ms), and this
   function reads the operator LINK switch from it.** An operator who flips
   LINK and presses ÁP DỤNG inside that window gets the previous mode: a slot
   just switched to LINK would have its first proposal placed on one lane only.
   Not new to Task 7 — every snapshot reader has it — but Task 7 is the first
   reader where the consequence is a PLACEMENT, not a repaint. It is what made
   the unwind test fail on the first run. Closing it needs either a
   message-thread read of `isLinked()` (a second API surface, which Q7 forbids)
   or accepting the window. Flagged for the owner; in practice Task 9's APPLY
   button is about a second of human reaction time after any switch flip, which
   is a hundred hops.

2. **The B-1 residual race is still open and still acknowledged.** Between the
   last `copySnapshot` and the `setNotch` there is a microsecond window in
   which the detector can take the slot and be silently overwritten
   (`setNotchImpl` does not check `n.active`, `NotchController.cpp:226-245`).
   The detector cadence is at least 300 ms, so this is tiny but non-zero.
   Unchanged from the brief ruling; noted so it is not rediscovered as a bug.

3. **`skippedLive` bumps `refused` too.** The task wording ("count as `refused`
   with a distinct counter") admitted two readings. The chosen one: `refused`
   is "this candidate placed nothing", `skippedLive` is "and this is why", a
   subset rather than a second total. Task 9's GUI copy must therefore not add
   them, or it will double-count. Documented on the struct in the header.

4. **The ±1-bin rule uses the DETECTOR bin ruler, not a soundcheck-specific
   one.** `LoopGainEstimator::hzToBin` and `Detector` share
   `kFftSize`/`kNumBins`, so the two agree today, and `SoundcheckController`'s
   own FFT is explicitly built on `Detector::kFftOrder` for exactly this reason
   (header comment on `fft_`). If either geometry ever moves independently this
   comparison silently changes meaning, and no test would catch it.

5. **Nothing here is audible-verified.** Expected level change on ÁP DỤNG is
   spec §3: each proposed bin cut by −6/−12/−18/−24 dB or the preset ceiling,
   whichever is shallower, at most 6 bins per lane; on BỎ, 0 dB — nothing is
   placed. The depth comes from Task 6's candidates untouched, and
   `setNotchImpl` still clamps at `kMaxDepthDb` (−24 dB). No listen has
   happened; per the owner's 2026-08-27 decision that happens in alpha.

---
---

# Task 7 — fix report, review round 1 of 5

**Commit:** `ead1760`. **Model actually used:** Opus 5 (1M context),
`claude-opus-5[1m]`; the Fable trailer is the lane convention, this line is the
record.

**Suite:** `ctest -C Release` → **658/658**, up from 652 (+6 tests).
`ctest -C Release -R "SoundcheckApply|SoundcheckController"` → **54/54** (20
`SoundcheckApply` plus 34 others matching the filter).

**All 10 findings applied.** New signature:

    SoundcheckApplyStats applySoundcheckResults (NotchController& controller,
                                                 int slot,
                                                 const std::vector<OutputResult>& results,
                                                 SoundcheckApplyLedger& ledger);

## Changes

**C-1 — mutual separation between proposals of the SAME run.** The finding is
right and the consequence is the loud one: `pick()` has no mutual-separation
step, and the re-read snapshot cannot contain a notch written microseconds ago
on the message thread, so two adjacent-bin proposals from one run both landed
and cascaded — roughly −48 dB over one bin wherever the ceiling allows −24.
`placedThisCall` (a `std::vector<SoundcheckApplyLedger::Entry>`, so it doubles
as the next ledger) is appended on EVERY successful `setNotch` on BOTH
branches, and `withinOneBinOfALiveNotch` now tests three populations: the live
snapshot notches, MINUS what this call cleared, PLUS what this call placed.
One thing the finding did not name: the linked unwind must also strike those
entries back out (`placedThisCall.resize(...)` after the `clearNotch` loop) — a
frequency no longer on the chain must not go on blocking later proposals, and
must not reach the ledger as if it had survived. The index stays reserved,
because the snapshot still cannot show it gone.

**C-2 — the replace pass no longer clears by origin.** Verified the second
producer before changing anything: `placeConfirmed` stamps `Origin::Soundcheck`
whenever `soundcheckActive()` (`NotchController.cpp:1093`), and MainComponent
turns that on for `Mode::Soundcheck` (`MainComponent.cpp:802-803`). So AP DUNG
was deleting protection the operator locked in by hand. Implemented as ruled:
`SoundcheckApplyLedger` with `Entry { int lane, index; float hz; }`, one per
slot, owned by the caller. The replace clears a ledger entry only when the
notch still at that `(lane, index)` is still active, still `Origin::Soundcheck`
AND still within ±1 bin of the recorded frequency — anything else at that
address is somebody else's notch on a reused slot. The ledger is then
overwritten with what this call placed, INCLUDING when nothing was placed (the
old entries were just cleared, so remembering them would make the next apply
chase notches that are already gone). The thread-guard early return does NOT
touch the ledger.

**I-1 — `slot`.** New parameter; `result.slot != slot` gives
`++skippedOtherSlot` and `continue`. The guard refusal count is filtered by
slot too.

**I-2 — `effectiveLinked()`.** Read once at entry on the message thread.
`laneCount` still comes from the entry snapshot, read once rather than per
candidate — it is `analysedLanes()`, which only moves on `setWidth()`, and
`setWidth()` requires the detector thread stopped, so it is not volatile the
way the atomic LINK switch is. This CLOSES Concern 1 of the round-0 report: no
new API was needed, `effectiveLinked()` was already public at
`NotchController.h:223`. The `ASSERT_TRUE (rig.snapshot().linked)` that round 0
added to `LinkedPairUnwindsWhenTheSecondLaneFails` is replaced by
`ASSERT_TRUE (rig.controller->effectiveLinked())`, with the history kept in the
comment.

**I-3 — the re-read has no failing test, and the name is older than the
truth.** Correct: the detector notch in
`ApplyReReadsTheSnapshotBeforeEachSetNotch` is already in the ENTRY snapshot,
so the entry read alone would find it; what the test actually proves is
`takenThisCall`. The detector places on its own thread at ≥ 300 ms cadence and
no harness hook can force that interleaving. The test comment now says this in
full, and the mutation table below records the re-read as DEFENCE IN DEPTH WITH
NO FAILING TEST BEHIND IT rather than counting it as covered. The test name is
left alone because the review and the round-0 report both refer to it by name.

**M-5 — the depth is clamped to the live slider.**
`std::max ((double) cand.depthDb, controller.getNotchDepthDb())` at both
`setNotch` sites. Deeper is more negative, so `std::max` is the shallow
direction and the clamp is strictly one-directional — a proposal shallower than
the slider is placed as proposed, never deepened to meet it. The test asserts
both directions.

**M-2 — `takenThisCall` is `[kChannels][kSlots]`.** `lane < 0` means "free on
every DRIVEN lane", bounded by `laneCount` and NOT by `kChannels`: on a mono
slot lane 1 is never written and never marked, so scanning it would report
every index free and hand 15 out twice. That is why the `laneCount` parameter
the round-0 report had dropped from `firstFreeIndexTopDown` is back — it now
does real work, and the round-0 reasoning for dropping it no longer holds.

**M-1 / M-3 — units documented** on every `SoundcheckApplyStats` field:
`placed` counts LANE WRITES (a linked pair is 2), `refused` counts CANDIDATES,
`clearedPrevious` counts clears ISSUED against ledger entries (not confirmed
removals — `clearNotch` returns void and the model is observable only one hop
later), `skippedLive` is a subset of `refused`, `skippedOtherSlot` counts
RESULTS.

**M-4 — `ARoutingInvalidResultPlacesNothing`** added.

## New tests (6)

`TwoAdjacentBinProposalsPlaceOnlyOne` (C-1; 1000 Hz is bin 43 and 1031.25 Hz is
bin 44 exactly at 48 kHz / 2048), `LegacySoundcheckModeNotchesSurviveAnApply`
(C-2), `ResultsForAnotherSlotAreSkipped` (I-1),
`ApplyNeverPlacesDeeperThanTheCurrentSlider` (M-5, both directions),
`IndepStereoKeepsOneIndexSpacePerLane` (M-2; 6+6 lands on 15..10 of each lane
and index 9 stays free on both), `ARoutingInvalidResultPlacesNothing` (M-4).

`NotchRig` gained a `SoundcheckApplyLedger` member and an
`apply (results, slot = 0)` helper, so every test drives the real four-argument
signature and the re-run tests read the way a real second soundcheck does.

## Output

    cd build && ctest -C Release -R "SoundcheckApply|SoundcheckController"
    100% tests passed, 0 tests failed out of 54
    Total Test time (real) =   4.30 sec

    cd build && ctest -C Release
    100% tests passed, 0 tests failed out of 658
    Total Test time (real) =  50.72 sec

## Mutations — 12 run, 12 red

Driven by a script that restores both files from a byte-verified backup before
each mutation, builds, runs the named filter, and restores at the end.

| # | Mutation | Test | Result |
|---|---|---|---|
| 1 | `takenThisCall` never fires | `ApplyReReadsTheSnapshotBeforeEachSetNotch` | RED |
| 2 | the `PartialApplyUnwind` loop deleted | `LinkedPairUnwindsWhenTheSecondLaneFails` | RED |
| 3 | the ledger replace pass deleted | `ARerun*` | RED |
| 4 | the ±1-bin test always answers no | `AProposalWithinOneBinOfALiveNotchIsSkipped` | RED |
| 5 | the message-thread guard made unreachable | `ApplyRunsOnTheMessageThread` | RED |
| 6 | a preventive notch stops being its own ceiling | `APreventiveNotchIsItsOwnCeiling` | RED |
| 7 | C-1: `placedThisCall` not consulted | `TwoAdjacentBinProposalsPlaceOnlyOne` | RED |
| 8 | C-2: the replace clears by ORIGIN alone | `LegacySoundcheckModeNotchesSurviveAnApply` | RED |
| 9 | I-1: `result.slot` not compared | `ResultsForAnotherSlotAreSkipped` | RED |
| 10 | M-5: the depth clamp dropped | `ApplyNeverPlacesDeeperThanTheCurrentSlider` | RED |
| 11 | M-2: `takenThisCall` keyed by index alone | `IndepStereoKeepsOneIndexSpacePerLane` | RED |
| 12 | M-4: `routingInvalid` not honoured | `ARoutingInvalidResultPlacesNothing` | RED |

NOT COVERED BY ANY MUTATION, and said out loud (I-3): the per-`setNotch`
snapshot re-read. Deleting it leaves every test green. It is defence in depth
against a detector placement landing between entry and a write, and the
residual microsecond race it narrows is the one the B-1 ruling accepted.

Both mutated files were restored and verified byte-identical (`cmp`) against
the pre-mutation backups before the pristine rebuild and the final suite run.

## Notes for the coordinator

1. **Task 10 now owns a ledger per slot.** `SoundcheckApplyLedger` must live as
   long as MainComponent, not as long as a results window — a ledger recreated
   per apply makes the replace pass a no-op and the chain drains exactly as
   §4.6b warned.
2. **Round-0 Concern 1 is CLOSED** by I-2. The round-0 text above is left as
   written rather than edited, so the two rounds can be read against each other.
3. **A process note.** The mutation driver's first run reported all twelve
   mutations "STILL GREEN". That was the harness, not the code: `subprocess`
   with `shell=True` on Windows routed
   `build/tests/Release/HandsFreeTests.exe` through cmd.exe, which read the
   forward slashes as switches, so stdout came back empty and "no FAILED TEST
   in the output" scored as green. A green that comes from an empty string
   looks exactly like a green that comes from a passing run. The driver now
   refuses to score any run whose output has no `[==========]` banner in it.
   Worth remembering for any future scripted verification in this repo.
4. **One accident, caught and undone.** The same driver wrote its restores with
   LF endings, which silently converted the working copy of
   `src/app/NotchController.cpp` from CRLF. `git diff` showed nothing (autocrlf
   normalises), so only a byte-level `cmp` caught it. Rewritten as CRLF and
   verified byte-identical to the backup; that file is untouched in this
   commit. Global rule 6 exists for exactly this, and it applies to throwaway
   verification scripts too.
5. **Still no listen.** Expected level change is unchanged from round 0, with
   one addition: M-5 means the placed depth is now `max(proposal, live slider)`,
   so a run armed while the slider read −24 and applied after it was pulled to
   −12 places −12, not −24.

---
---

# Task 7 — fix report, review round 2 of 5

**Commit:** `5153694`. **Model actually used:** Opus 5 (1M context),
`claude-opus-5[1m]`; the Fable trailer is the lane convention.

**Suite:** `ctest -C Release` → **662/662**, up from 658 (+4).
`-R "SoundcheckApply|SoundcheckController"` → **58/58** (24 `SoundcheckApply`).
**15 mutations run, 15 red.**

## N-1 — the ledger could orphan the notches it exists to reclaim

**(a) rate unknown now counts as a PASS.** Address and origin are the strong
half of a ledger match; the frequency is the tie-breaker that catches a slot
reused by another Soundcheck-origin notch. Refusing to clear because the rate
is missing strands a notch that never auto-releases — the exact failure the
ledger prevents. Changed to `rateKnown && ... > 1 ⇒ break`.

**The test you named is not constructible, and I did not fake one.**
`LedgerStillClearsWhenTheSnapshotHasNoSampleRate` cannot be written through any
public API: `Detector::setSampleRate` rejects a non-positive rate outright
(`Detector.cpp:29-32`), so a PUBLISHED snapshot always carries a positive one,
and `latest_.sampleRate` is written in the same block as `notchCount` — an
UNPUBLISHED snapshot therefore carries no notches at all, and the address match
fails before the frequency test is ever reached. The branch is kept as defence
and is documented in the code as unreachable-today with no test, exactly the
way I-3 handles the per-`setNotch` re-read. If you want it covered, it needs a
seam (a test accessor that hands the ledger-match predicate a synthetic
`SnapshotBuffer`); say the word and it is a ten-line change.

**(b) unmatched entries are carried forward.** Implemented, with two details
the finding did not name:

- **They are appended AFTER the placement loop, never before.**
  `placedThisCall` doubles as invariant 15's "already placed on this lane"
  list, and a carried entry may name a notch that is no longer on the chain at
  all. Letting it block a proposal would mean refusing to protect the room on
  the strength of a notch nobody can find.
- **An entry whose `(lane, index)` THIS call reused is still dropped.** Two
  ledger rows for one address would have the next apply clear it twice and
  report the second as a real removal.

**And the test found something.** My first attempt at
`UnmatchedLedgerEntriesAreCarriedForward` applied twice inside one hop with a
real second proposal and expected the first notch to be orphaned. It is not
orphaned — it is **overwritten**. With a stale snapshot the allocator hands out
index 15 again and `setNotchImpl` does not check `n.active`
(`NotchController.cpp:226-245`), so the second apply replaces the first notch
in place. The bookkeeping stays honest (one ledger row for one address), so
this is benign for the chain, but it is worth knowing: **a re-run inside one
hop silently replaces rather than cleanly clearing, so no `SoundcheckReplace`
event reaches the session log for it.** The test was rewritten to isolate the
carry-forward — second apply with an unmeasured result (a re-run whose mic went
dead), which places nothing and reuses no address.

## N-2 — the linked unwind strike, now covered twice

`LinkedUnwindDoesNotBlockLaterProposals` is the test you specified (two
results — a linked failure stops the result it is in, so the second proposal
has to arrive in a result of its own; `placed == 2`, ledger holds the surviving
pair only). `LinkedUnwindFreesTheFrequencyItPlacedOn` adds the half that
actually reaches a PA: a proposal refused because an identical notch is
"already there", when that notch was rolled back, leaves the room open at
exactly the frequency it was howling at. Both go red on mutation 14.

## Cheap items

- **`result.lane` bounds-checked** against `laneCount` before it indexes
  `takenThisCall`. New `stats.skippedBadLane`, kept separate from
  `skippedOtherSlot`: "this slot has no such output" and "these results are not
  for this slot" are different faults with different words on screen. Covered
  by `AResultNamingALaneThisSlotDoesNotDriveIsSkipped` (mono rig, a lane-1
  result) and mutation 15.
- **The `linkedNow` / `laneCount` provenance sentence** is at the entry where
  both are established, since I-2 moved `linkedNow` out of the candidate loop:
  linkedNow is LIVE, laneCount is snapshot-sourced and may be one hop old, and
  that is safe only because `laneCount` moves solely on `setWidth()`, which
  requires the detector thread stopped.
- **Header** now carries both Task 9 warnings: CLEAR ALL does not reset a
  ledger (harmless — the match finds nothing active, the entries are carried
  forward and quietly stop matching), and `clearedPrevious > 0 && placed == 0`
  is a real outcome on a full chain in which **the operator ends up less
  protected than before they pressed the button**.

## Output

    cd build && ctest -C Release -R "SoundcheckApply|SoundcheckController"
    100% tests passed, 0 tests failed out of 58
    Total Test time (real) =   4.74 sec

    cd build && ctest -C Release
    100% tests passed, 0 tests failed out of 662
    Total Test time (real) =  56.51 sec

## Mutations — 15 run, 15 red

| # | Mutation | Test | Result |
|---|---|---|---|
| 1 | `takenThisCall` never fires | `ApplyReReadsTheSnapshotBeforeEachSetNotch` | RED |
| 2 | the `PartialApplyUnwind` loop deleted | `LinkedPairUnwindsWhenTheSecondLaneFails` | RED |
| 3 | the ledger replace pass deleted | `ARerunReplaces*` | RED |
| 4 | the ±1-bin test always answers no | `AProposalWithinOneBinOfALiveNotchIsSkipped` | RED |
| 5 | the message-thread guard made unreachable | `ApplyRunsOnTheMessageThread` | RED |
| 6 | a preventive notch stops being its own ceiling | `APreventiveNotchIsItsOwnCeiling` | RED |
| 7 | C-1: `placedThisCall` not consulted | `TwoAdjacentBinProposalsPlaceOnlyOne` | RED |
| 8 | C-2: the replace clears by ORIGIN alone | `LegacySoundcheckModeNotchesSurviveAnApply` | RED |
| 9 | I-1: `result.slot` not compared | `ResultsForAnotherSlotAreSkipped` | RED |
| 10 | M-5: the depth clamp dropped | `ApplyNeverPlacesDeeperThanTheCurrentSlider` | RED |
| 11 | M-2: `takenThisCall` keyed by index alone | `IndepStereoKeepsOneIndexSpacePerLane` | RED |
| 12 | M-4: `routingInvalid` not honoured | `ARoutingInvalidResultPlacesNothing` | RED |
| 13 | N-1(b): unmatched ledger entries dropped | `UnmatchedLedgerEntriesAreCarriedForward` | RED |
| 14 | N-2: the linked unwind strike dropped | `LinkedUnwind*` (2 tests) | RED |
| 15 | `result.lane` not bounds-checked | `AResultNamingALaneThisSlotDoesNotDriveIsSkipped` | RED |

**Still not covered by any mutation, both stated in the code:** the
per-`setNotch` snapshot re-read (I-3), and N-1(a)'s unknown-rate branch —
neither is reachable from a test through the public API.

Both files restored and verified byte-identical (`cmp`) to the pre-mutation
backups; `git status` shows `NotchController.cpp` untouched by this commit.

## Notes for the coordinator

1. **A re-run inside one hop OVERWRITES rather than replaces.** Found while
   writing the N-1(b) test, described above. Bookkeeping stays consistent and
   the chain does not drain, but the session log gets no `SoundcheckReplace`
   for that notch, so lane D's training labels would show a notch that simply
   changed frequency with no clear in between. Cheap fix if you want it: refuse
   to place at an index the ledger still names but the snapshot has not yet
   confirmed. Not done — it trades a logging gap for a placement refusal, and
   that is your call, not mine.
2. **N-1(a) has no test and cannot have one without a new seam.** Ten-line
   change if you want it (expose the ledger-match predicate to a test with a
   synthetic `SnapshotBuffer`); say so and I will add it in round 3.
3. **Two harness bugs bitten in two rounds, both now guarded.** Round 1: a
   `shell=True` subprocess on Windows swallowed the exe path and scored empty
   output as a pass (the driver now refuses any run with no gtest banner, and
   also refuses a filter that matched no test). Round 2: the tool's heredoc
   collapsed `\n` to `\n`, so a Python-generated mutation table came out with
   real newlines inside string literals and failed to parse. Both are recorded
   because the first one nearly published twelve false greens, and a false
   green in a mutation table is worse than no mutation table.
4. **Global rule 6 now holds inside the throwaway scripts too.** The round-1
   driver rewrote line endings on restore and silently converted
   `NotchController.cpp` to LF — invisible to `git diff`, caught only by a
   byte-level `cmp`. The driver reads and writes bytes now, and adapts each
   mutation's pattern to the file's own line endings.
5. **Still no listen.** No change from round 1: the placed depth is
   `max(proposal, live slider)`, at most 6 bins per lane, 0 dB on BỎ.
