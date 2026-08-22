# Lane report — Licensing (Tasks 27, 28, 29)

**Branch:** `claude_desk/lane-b-licensing-c8a3a2` (worktree lane).
**Base:** `main` @ `01db1f0`. **Brief:** [lane-licensing.md](lane-licensing.md).

## Verification — run, not read

```
cmake -S . -B D:/hf-lanes/bld-lane-b-clean -G "Visual Studio 18 2026" -A x64
-- Configuring done (87.5s)          CONFIGURE_EXIT=0     # clean dir, deleted first

cmake --build D:/hf-lanes/bld-lane-b-clean --config Release
BUILD_EXIT=0                         # both targets; "AZ Soundtech Hands-free.exe" produced

ctest --test-dir D:/hf-lanes/bld-lane-b-clean -C Release
100% tests passed, 0 tests failed out of 120
```

Baseline before this lane, measured on the same worktree: **75/75**.
After: **120/120**. **45 tests are new**, all in `tests/test_licensemanager.cpp`.

## What was built

| File | |
|---|---|
| `src/app/LicenseManager.h` | new — three seams, `LicenseState`, `ActivationResult`, `UrlActivationTransport` |
| `src/app/LicenseManager.cpp` | new |
| `tests/test_licensemanager.cpp` | new — 45 tests |
| `CMakeLists.txt` | +3 lines (2 sources, 1 module) |
| `tests/CMakeLists.txt` | +3 lines (2 sources, 1 module) |

Three injected seams, exactly as the brief asked: `ActivationTransport`,
`MachineIdSource`, `LicenseClock`. Production implementations
(`UrlActivationTransport`, `SystemMachineIdSource`, `SystemLicenseClock`) live
in the same header; tests wire fakes.

## The new tests, and what breaks each

45 tests in five groups. Rather than list all 45, here is the production change
that kills each group — **verified by actually making two of these mutations,
rebuilding, and watching precisely the predicted tests fail**:

| Group | n | Production change that fails it |
|---|---|---|
| `LicenseKeyFormat` | 10 | Loosen any of length / `AZHF` prefix / group structure / separator / charset. Dropping the case check alone fails `RejectsLowerCase`. |
| `LicenseMachineId` | 2 | Send `machineId_.rawId()` instead of its SHA-256, or hash something else. |
| `LicenseActivation` | 12 | Collapse any rejection branch into a single generic failure; accept a 3-segment check on a 6-segment token; treat a missing `exp` as invalid; POST before validating the key format. |
| `LicenseStorage` | 8 | **MUTATION RUN:** disabling the XOR failed exactly `TheKeyIsNotStoredAsReadableText` and `AFileFromAnotherMachineDoesNotLoad`; the other 6 stayed green. |
| `LicenseGrace` | 13 | **MUTATION RUN:** deleting the backwards-clock branch failed exactly `AClockWoundWellBackIsReportedAsTampering` and `TamperingIsNotAWayToKeepUsingAnExpiredLicence`; the other 11 stayed green. Changing either `>` to `>=` fails the day-7 / day-10 boundary tests. |

## The XOR storage — what it does and does not achieve, plainly

The plan asks for `license.key` "encrypted with simple XOR + machine hash".
That is what is implemented. Stated without varnish:

- **It does** stop the file being read or edited in a text editor.
- **It does** stop the file working after being copied to another machine —
  de-obfuscating with a different machine hash yields bytes that fail the
  key-format check, so `load()` returns false. This is tested.
- **It does not** resist anyone who opens the binary. The mask is
  `SHA256(getUniqueDeviceID())`, derived on the customer's own machine, so
  anyone willing to call that function themselves reproduces it in a minute.
- **It is obfuscation, not encryption.** Nothing here survives a determined
  attacker, and the code says so at the function.

That is very likely the right trade for this product. It is recorded here as a
**decision**, not slipped through as an implied claim of security. Making it
stronger is the owner's call; this lane did not invent anything beyond the plan.

## Decisions this lane had to make

**Backwards clock leads to `LicenseState::ClockTampered`, with a 24-hour
tolerance.** A naive `now - stored` goes negative when the user winds the clock
back, and a negative duration compares as comfortably inside every threshold —
which is exactly how a date check is defeated. The sign is therefore checked
*before* any division. The tolerance is not zero on purpose: NTP corrections and
dead-CMOS fixes move a clock back by seconds or hours, and locking out a paying
customer for that is a support call caused by our own strictness. 24h is
generous for a real correction and far below the 7-day grace, so it cannot be
chained to extend the window.

**A JWT with no `exp` claim is accepted.** Spec §1 sells this product
*"theo perpetual license"*, so a token without an expiry is the normal case,
not a malformed one. A present `exp` is enforced against the injected clock.

## What the plan or the brief got wrong

1. **`juce::SHA256` is in `juce_cryptography`, not `juce_core`.** The brief's
   "needs no new dependency" is true of `juce::JSON` but not of the hash the
   same task requires. Both targets needed `juce::juce_cryptography` added, so
   the "`CMakeLists.txt` — one line: your new sources" instruction is one line
   short, in two files.

2. **"Inject the clock, exactly as the bridge design does (§4)" would produce a
   broken grace period.** The bridge's `ClockSource` is `nowMs()` backed by
   `juce::Time::getMillisecondCounterHiRes()` — a *monotonic* counter for
   sub-second detector timing that resets when the machine reboots. A licence
   measures days *across application restarts*. Sharing that seam literally
   gives a grace period that restarts at every reboot. A separate wall-clock
   seam (`LicenseClock::nowUtcMs()`) is used instead, and the header says why so
   that nobody "unifies" the two later.

3. **Spec and plan disagree on the grace numbers, and nobody has picked.**
   Spec §10.2 and §8 both say *"offline grace 7 ngày"* — blocked after 7 days.
   Plan Task 29 says warn at 7, disable at 10. Implemented **the plan's 7/10**,
   because that is the task under execution and the brief's acceptance criteria
   test it. **This needs an owner decision**, because the two differ on whether
   a customer on day 8 can still run a show.

4. **A JWT signature cannot be verified client-side in this build.** The brief
   asks for a test of "a JWT whose signature or expiry does not check out". No
   public key ships in this repo and no server exists to have signed anything,
   so only *structure* and *expiry* are verified. Signature verification is
   deliberately **not** faked. Until it exists, a forged token with valid
   structure and no `exp` will be accepted — this is the single largest gap in
   the licence system, and it cannot be closed from the client alone.

5. **Confirmed, not overturned:** `wmic` really is absent
   (`command -v wmic` gives NOT FOUND, and there is no `System32\wbem\WMIC.exe`),
   and `getUniqueDeviceID()` really is at `juce_SystemStats.h:179` with the
   deprecated `getDeviceIdentifiers()` right above it. The brief was right on
   both counts.

6. **The stale-build-directory trap fired again, during this lane.** After
   adding a source file that did not exist yet, `cmake --build` returned
   **exit 0** while `cmake` *configure* had failed with
   `Cannot find source file: ../src/app/LicenseManager.cpp` — the build silently
   used the previous solution. The runbook warns about this shape; it is worth
   repeating that **a green `--build` proves nothing if the configure output was
   not read.**

## What this lane did NOT do, and why

- **Nothing touches `AudioEngine`.** No kill switch, as the brief requires.
  `LicenseManager` reports state and holds no reference to the audio path.
- **No GUI wiring.** Out of lane; the GUI lane is live in `MainComponent`.
- **⚠ Nothing constructs a `LicenseManager` yet.** The class is built and
  tested, but no production code instantiates it — the *identical shape* to the
  `AudioEngine` defect the briefs were written to expose. This is correct for
  the lane's scope, and is called out here so it is not later mistaken for a
  working licence check. **The shipped app enforces no licence at all.**
- **No activation server.** `https://license.azsoundtech.com/activate` is still
  presumed not to exist. `UrlActivationTransport` is the only untested code in
  the lane, by design: it holds the JUCE call and no decisions.

## What the GUI lane will need to call

```cpp
LicenseManager mgr { transport, machineId, clock, LicenseManager::defaultStorageFile() };

mgr.load();                       // at startup; false == no usable licence
switch (mgr.state()) { ... }      // NotActivated / Active / GraceWarning / Expired / ClockTampered
mgr.daysSinceLastValidation();    // for the "N days remaining" banner
mgr.activate (typedKey);          // returns ActivationResult; every failure is distinct
mgr.deactivate();                 // the [Deactivate] button in spec section 6.1
```

`ActivationResult` distinguishes `InvalidKeyFormat`, `TransportFailure`,
`ServerRejected`, `MalformedResponse`, `MalformedToken` and `ExpiredToken`
precisely so the dialog can say which one happened instead of "activation
failed".
