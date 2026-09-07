# Task 8 report — room memory

**Status:** DONE. Commit `c570372` on `claude_desk/lane-g-brainstorm-sdd-f3c568`.

**Expected level change (owner-facing):** at a bin that howled and auto-released
within the last 5 minutes, the next placement lands at that bin's previous
deepest depth (clamped to the current ceiling, at most −24 dB) on the first
block, instead of −6 (or −12 on a steep rise). Up to **18 dB deeper, sooner, at
that one bin**. **0 dB everywhere else** — no other bin, no other lane, no
change to Soundcheck, Preset or Manual notches.

---

## What was implemented

Per-lane fixed ring of 16 `MemoryEntry { frequencyHz, deepestDb, clearedAtMs, used }`.

| Concern | Where | Behaviour |
|---|---|---|
| Write | `NotchController.cpp:801`, the release ladder's shallowest-rung branch | `rememberReleaseLocked (c, n.frequency, n.deepestDb, effectiveLinked())` immediately before `pushClearLocked (c, i, ClearReason::AutoRelease)`. The ONLY write site. |
| Read | `NotchController.cpp:1071`, inside `placeConfirmed`'s index-lookup `modelMutex_` block | `takeRememberedDepthLocked(...)`, guarded by `index >= 0` so a placement that bails on a full chain does not spend the entry. |
| Bin match | `takeRememberedDepthLocked` | `std::lround (f / binWidthHz)` equal, ±0 (Q10). |
| TTL | same | `liveMs_ - clearedAtMs > kMemoryTtlMs` (300 000 ms) ⇒ entry dropped, not used. |
| Consume | same | `e.used = false` on every match — one use per release. |
| LINKED | both helpers | `bothLanes` walks lanes `0..width_-1`; write merges onto an existing same-frequency entry taking the DEEPER (`std::min`); read takes the deepest across lanes. |
| Depth clamp | `placeConfirmed` step 4 | `depthDb = std::max (depthDb, ceiling)` — unchanged line, now also the only thing done to a remembered depth. No snap-to-rung (Q13/m-8). |
| Soundcheck | `placeConfirmed`, last statement of the depth choice | `if (origin == Origin::Soundcheck) depthDb = ceiling;` still LAST. Its comment was rewritten: its position is now genuinely load-bearing, not merely reserved for a future task. |
| Wipe | `setWidth` (`:125`), `clearAll` (`:443`), `setSampleRate` (`:1508`) | `clearRoomMemoryLocked()`. |

Storage is `std::array<std::array<MemoryEntry, 16>, 2>` plus `std::array<int, 2>`
heads — **no allocation** added on any path.

`kMemoryTtlMs` and `kMemoryEntriesPerLane` already existed in the header from
Task 4's constant block; only the struct, three helper declarations and the two
members were added.

## Tests

Eight new `NotchControllerLadder` cases in `tests/test_notchcontroller.cpp`,
plus the `firstActiveIndex` helper placed **inside the anonymous namespace**,
immediately after `pumpQuietFor` (line 366) — not appended after `} // namespace`.

1. `AHowlReturningToTheSameBinStartsAtTheRememberedDepth` — climbs to −24, 62 s quiet to Clear, returns ⇒ placed at −24.
2. `OneBinAwayIsANewHowlAndStartsFreshNotFromMemory` — asserts exactly −12 on both `depthDbForTest` and `deepestDbForTest`.
3. `RoomMemoryExpiresAfterFiveMinutes` — `kMemoryTtlMs + 1000` of quiet ⇒ `>= -12`.
4. `ARememberedEntryIsUsedOnlyOnce` — first return −24, Manual clear (no new entry), second return `>= -12`.
5. `ARememberedDepthIsStillCappedByTheCeiling` — −24 memory under a −12 ceiling ⇒ −12.
6. `AnOffRungRememberedDepthComesBackVerbatim` — Manual −9, 42 s quiet ⇒ comes back −9.
7. `SetWidthClearAllAndSetSampleRateWipeRoomMemory` — all three boundaries, `which=0..2`.
8. `LinkedReleaseWritesAndConsumesBothLanes` — StereoHarness, both lanes −24.

All liveness probes use `activeForTest` / `firstActiveIndex`, never a depth
probe (B-3). Quiet pumps: 62 000 ms from −24, 42 000 ms from −9 (B-4).

### TDD evidence

**Red, before implementation** (tests + helper compiled against the untouched
controller):

```
[  FAILED  ] AHowlReturningToTheSameBinStartsAtTheRememberedDepth
  h.controller.depthDbForTest (0, placed)  Which is: -12
  deepest                                  Which is: -24
[  FAILED  ] ARememberedEntryIsUsedOnlyOnce            (-12 vs -24)
[  FAILED  ] AnOffRungRememberedDepthComesBackVerbatim (-12 vs  -9)
[  FAILED  ] LinkedReleaseWritesAndConsumesBothLanes   (-12 vs -24, both lanes)
[       OK ] OneBinAwayIsANewHowlAndStartsFreshNotFromMemory
[       OK ] RoomMemoryExpiresAfterFiveMinutes
[       OK ] ARememberedDepthIsStillCappedByTheCeiling
[       OK ] SetWidthClearAllAndSetSampleRateWipeRoomMemory
```

The four that require memory to be USED failed; the four negative controls
passed, as they must before the feature exists.

**Green, after implementation:**

```
build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchController*
[==========] 119 tests from 14 test suites ran. (8767 ms total)
[  PASSED  ] 119 tests.

cd build && ctest -C Release
100% tests passed, 0 tests failed out of 525
Total Test time (real) = 274.49 sec
```

525 = 517 baseline + 8 new.

## Deviations from the brief

**One, in the test fixtures only — no implementation deviation.**

Seven of the brief's eight tests, as written, failed with `placed == -1`: nothing
ever re-placed. Cause: `processSpectrumForDetection` returns early while
`detectionActive_` is false (`NotchController.cpp:1098`), so the 62 s quiet pump
that runs the release ladder leaves the CandidateScorer with **no history at
all**. The subsequent tone could not confirm. Only
`AHowlReturningToTheSameBinStartsAtTheRememberedDepth` was unaffected, because it
enables detection before `primeAndPlace`.

Fix: after each `setDetectionActive (true)` that follows a quiet pump, warm the
scorer the way `primeAndPlace` already does —

```cpp
for (int i = 0; i < kWarmupBlocks; ++i)
    pump (h, quiet.hop());       // pumpStereo in the LINKED test
```

`kWarmupBlocks` is 64 blocks ≈ 683 ms of live time, negligible against the
300 s TTL and inside every window the tests assert on. Nothing about what the
tests assert changed.

Two comment rewrites that the brief implied but did not spell out:
- `placeConfirmed`'s step-4 comment now records that the clamp is also the only
  thing done to a remembered depth (m-8).
- The Soundcheck comment no longer says "stays LAST through Task 8" / "only
  becomes test-visible once that step exists"; that step now exists.

## Self-review

- `grep -n rememberReleaseLocked src/app/*.cpp` → exactly one call site, line
  801, directly above `pushClearLocked (c, i, ClearReason::AutoRelease)`. No
  other `ClearReason` path touches the memory.
- `grep -n takeRememberedDepthLocked` → exactly one call site, line 1071, inside
  the `const std::lock_guard<std::mutex> lock (modelMutex_)` block in
  `placeConfirmed`. The helper sets `e.used = false` on every match ⇒ consumed.
- No `new`, `malloc`, `push_back`, `resize` or container growth added. Storage is
  `std::array` members with a modulo head index; `clearRoomMemoryLocked` assigns
  a value-initialised `MemoryEntry` in place.
- Soundcheck override is the last statement of the depth choice; verified by
  extracting the block between step 3 and the assignment — the three `depthDb =`
  statements appear in order remembered → clamp → ceiling.
- No lock nesting introduced: `setWidth`'s pre-existing `modelMutex_` scope
  closes before the new wipe scope opens; `setSampleRate`'s wipe is its own
  scope; `clearAll`'s wipe is inside the lock it already held.
  `snapshotMutex_` is never taken inside `modelMutex_`.
- No clamp, guard or bounds check removed. `takeRememberedDepthLocked` refuses a
  non-positive `binWidthHz` and returns NaN, which the caller treats as "no
  memory".

## Concerns for the next task / review

1. **`effectiveLinked()` is read at the write site, `linkedNow` at the read
   site.** They can disagree if the operator toggles LINK between the release
   and the next placement. Consequence is benign in both directions (a stale
   pair entry is either consumed together or the unused twin ages out at the
   TTL), but it is not symmetric by construction — worth a line in the spec.
2. **Mono harnesses carry `width_ == 2` by default** (`taps_[1] == nullptr`), so
   `effectiveLinked()` is true and a mono release writes an entry into lane 1's
   bank as well. Harmless — nothing reads lane 1 while only lane 0 is analysed —
   but it means the ring holds one dead entry per release in the mono case.
3. **Not persisted, by design.** Confirm this stays out of `savePreset`; a
   preset that silently deep-notches an idle frequency is the failure mode the
   header comment warns about.
4. The `docs/GIOI-THIEU.md` / `docs/KY-THUAT-CHONG-HU.md` update that CLAUDE.md
   §"Definition of done" item 5 requires for user-visible behaviour changes has
   NOT been done here — it belongs to the lane's doc pass, not to this task's
   three permitted files.

---

## Fix round 1

Five findings from the Task 8 review. All five addressed; three permitted files
only (`src/app/NotchController.h`, `src/app/NotchController.cpp`,
`tests/test_notchcontroller.cpp`).

**Expected level change (owner-facing), REVISED.** Unchanged where the memory
holds a deep entry: a bin that howled and auto-released within 5 minutes is
placed at that depth on the first block, up to 18 dB deeper than a fresh -6.
What changes with I-1: the memory can no longer make a placement SHALLOWER than
the policy would have chosen on its own. At a bin whose last notch never had to
dig (a -6 that never reinforced, an operator's Manual -3 left to age out), the
placement is now the policy's own -6/-12 instead of the remembered -3. That is
**up to 9 dB DEEPER** than 1.1.3-plus-c570372 at such a bin, and equal
everywhere else. No other bin, lane, Preset, Manual or Soundcheck notch moves.

### I-1 - room memory may only DEEPEN (owner ruling Q14)

`src/app/NotchController.cpp:1138`, `placeConfirmed` step 3:
`depthDb = remembered;` becomes `depthDb = std::min (depthDb, remembered);`.
The ceiling clamp (`:1150`) and the Soundcheck override (`:1161`) are unchanged
and still in that order. The comment block was rewritten to carry the ruling
and its reason; the header contract on `takeRememberedDepthLocked` now says the
caller may only deepen with the result.

New test `NotchControllerLadder.RoomMemoryNeverShallowsAPlacement`
(`tests/test_notchcontroller.cpp:3557`): a Manual -3 auto-releases (-3 is
already at the shallowest rung, so Clear at 30 s; 32 s of quiet), then a
hard-onset `SineSource` returns to the same bin and must place at -12.

RED, before the min change (the test alone, against c570372's controller):

```
[ RUN      ] NotchControllerLadder.RoomMemoryNeverShallowsAPlacement
tests/test_notchcontroller.cpp(3513): error: Expected equality of these values:
  h.controller.depthDbForTest (0, placed)
    Which is: -3
  -12.0
    Which is: -12
the remembered -3 clamped the steep-rise placement shallower
tests/test_notchcontroller.cpp(3515): error: Expected equality of these values:
  h.controller.deepestDbForTest (0, placed)
    Which is: -3
  -12.0
    Which is: -12
a shallower remembered depth leaked into deepestDb
[  FAILED  ] NotchControllerLadder.RoomMemoryNeverShallowsAPlacement (95 ms)
```

GREEN after the change.
`AHowlReturningToTheSameBinStartsAtTheRememberedDepth` still asserts the
remembered -24 beats the fresh -12, so the deepening direction is untouched.

**One consequential test change the finding did not name.**
`AnOffRungRememberedDepthComesBackVerbatim` used a Manual **-9**, which under
Q14 is now correctly ignored (min against the fixture's -12), so as written it
could no longer say anything about rung snapping. Retuned to a Manual **-15**
(release: -12 at 30 s, -6 at 40 s, Clear at 50 s, so 52 s of quiet; `deepestDb`
stays -15) and it asserts -15 comes back verbatim: still RED if the read path
quantises (-12 or -18), now inside the half of the range where the memory
actually decides the depth. m-8 stays pinned.

### M-1 - a Soundcheck placement no longer spends an entry

`src/app/NotchController.cpp:1108`: the lookup is gated
`if (index >= 0 && origin != Origin::Soundcheck)`. `origin` is resolved above
the lock, so nothing moved.

New test `NotchControllerLadder.ASoundcheckPlacementDoesNotSpendTheRoomMemory`
(`:3596`): entry written by an auto-released -24; a Soundcheck placement at
that bin (asserted at the slider, KD-7); the notch cleared by hand; soundcheck
allowed to expire; then a DETECTOR placement at the same bin, which must still
get -24.

RED with the gate removed (`if (index >= 0)`):

```
[ RUN      ] NotchControllerLadder.ASoundcheckPlacementDoesNotSpendTheRoomMemory
  probeMemoryAt (h, quiet, 1000.0)   Which is: -12
  -24.0                              Which is: -24
the soundcheck placement consumed the entry, so the detector had to start over
at the steep-rise rung
[  FAILED  ] (194 ms)
```

### M-2 - the ring reclaims consumed slots

`rememberReleaseLocked` (`:341-409`) now finds, in the SAME pass that looks for
a merge target, the first slot holding nothing; a write refills that hole and
leaves the head alone, and only when all 16 are LIVE does the head evict the
oldest write. `takeRememberedDepthLocked` (`:440`, `:444`) empties a slot WHOLE
(`e = MemoryEntry {}`) on consume and on TTL expiry instead of only lowering
`used`. The header comment was corrected: it claimed "oldest entry
overwritten", with no mention of reclaim.

New test
`NotchControllerLadder.TheMemoryRingReclaimsAConsumedSlotBeforeEvictingALiveOne`
(`:3650`) pins both halves of the contract in one fixture: 16 Manual -18
notches at 16 distinct bins auto-release together (slot k holds bin 40+k, head
back at 0); a 17th write with nothing free evicts the OLDEST (bin 40 then
probes fresh at -12, bin 42 still answers -18); probing bin 42 consumes it,
leaving a hole away from the head; an 18th write must land in that hole, so
bin 41 - which the head points at - must still answer -18.

RED with the reclaim branch disabled:

```
  probeMemoryAt (h, quiet, 41.0 * binHz)   Which is: -12
  -18.0                                    Which is: -18
the head evicted a LIVE entry while a consumed slot sat empty
[  FAILED  ] (372 ms)
```

### M-3 - the three overclaiming RED IF comments

`RoomMemoryExpiresAfterFiveMinutes` (`:3308`), `ARememberedEntryIsUsedOnlyOnce`
second half (`:3340`) and `SetWidthClearAllAndSetSampleRateWipeRoomMemory`
(`:3474`): each comment now says what the test actually pins - "memory did NOT
fire", a negative control that also passes with the feature deleted and is
therefore not evidence that room memory works - and all three
`EXPECT_GE (depth, -12.0)` are now `EXPECT_DOUBLE_EQ (depth, -12.0)`, which a
bug returning some OTHER remembered depth can no longer slip past.

### M-4 - dead lane guards

The two `if (l < 0) continue;` in `rememberReleaseLocked` /
`takeRememberedDepthLocked` are gone, replaced by `jassert (first >= 0)`
(`:349`, `:420`) with a comment: every caller reaches these from a lane index
the release/detection loops already bounded to `[0, width_)`, and a negative one
would index the array out of range rather than be worth skipping. Debug-only,
no Release cost, no check weakened.

### M-5 - report reference

The Deviations section above cites `NotchController.cpp:1098` for
`processSpectrumForDetection`'s `if (! detectionActive_...) return;`. That was
wrong: at the time of the review the line was **:1223**, and after this round's
edits it is **:1273**.

### A fixture trap this round uncovered

The new probe helper `probeMemoryAt` (`tests/test_notchcontroller.cpp:400`)
clears the placed notch on EVERY lane, not just lane 0. A mono `Harness` has
`taps_[1] == nullptr`, so `effectiveLinked()` is true and a placement writes the
pair; clearing lane 0 alone leaves lane 1's notch standing, and those leftovers
pile into the harmonic-penalty `locked` list built in
`processSpectrumForDetection` (KD-3), where they silently stop later probes at
OTHER bins from confirming at all. Measured with a scratch fixture: a probe
sequence 40, 41, 42, 60 placed three times and then went dead for 80 blocks,
with `activeBefore L1=3` while lane 0 was clean. Any future test that places,
clears and places again on a mono harness needs the same two-lane clear;
`ASoundcheckPlacementDoesNotSpendTheRoomMemory` was given it for the same
reason.

### Verification

```
cmake --build build --config Release          (0 errors)

build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchController*
[==========] 122 tests from 14 test suites ran. (6481 ms total)
[  PASSED  ] 122 tests.

cd build && ctest -C Release
100% tests passed, 0 tests failed out of 528
Total Test time (real) =  46.32 sec
```

528 = 525 after c570372 + 3 new cases.

### Concerns after this round

1. **The write side of spec 4.6 is still origin-blind.** Q14 fixes the READ
   side, so a shallow entry is now harmless - but the ring still spends one of
   its 16 slots on every auto-released notch whose depth can never influence a
   placement (anything shallower than the rung the policy picks anyway).
   Option 3 of Q14 ("only remember what is deeper than -6, and only from
   Origin::Detector") would reclaim that capacity. Not done: the owner chose
   option 1, and narrowing the write side is a behaviour change past the ruling.
2. **Concerns 1-4 of the original report still stand** - the
   `effectiveLinked()` / `linkedNow` asymmetry, the mono harness writing a dead
   lane-1 entry per release, "not persisted, by design", and the
   `docs/GIOI-THIEU.md` / `docs/KY-THUAT-CHONG-HU.md` pass that belongs to the
   lane's doc wave. Concern 2 changes character with M-2: the dead lane-1 twin
   now costs capacity only in the bank nothing reads.
3. **`RoomMemoryNeverShallowsAPlacement` proves the rule with a Manual -3.**
   The Detector-side route to the same defect (a -6 notch that never
   reinforces, auto-releases and remembers -6) is not covered directly:
   producing one needs a tone that confirms and then never re-confirms, which
   this fixture set has no source for. It is the same `min()` line for both.
