# SDD ledger — plan: docs/superpowers/plans/2026-08-27-next-wave.md

Scope this session: LANE C only (Task C1, C2). Lanes P/R/T out of scope.

Task C2: complete (commit 7c91ee6) — owner gated decision resolved: delete
skills-lock.json (git rm), KEEP openspec/config.yaml (found live OpenCode
dependency via .opencode/skills/openspec-*). Also fixed stale CLAUDE.md
claim + memory/docs-drift-audit-2026-08-27.md. No code review needed —
doc/config-only change, not implementer-dispatched.

Task C1: implementer done (commit c65173d). Task reviewer verdict: spec ✅
compliant, but Needs fixes — Important #1 (final "pruned" Write-Host still
fires after a confirmed terminal delete failure, contradicting the two WARN
lines just printed) and Important #2 (report never pasted the actual
simulation script, and never exercised the terminal-failure branch). Minor:
inline duplication (deferred, not blocking).
Task C1: fix round 1/5 dispatched (fresh implementer, prior agent not
resumable) — findings sent verbatim, brief+report paths given.
Task C1: fix round 1/5 (2 addressed, 0 open; commits c65173d..c5e46d9).
Scoped re-review: all findings ADDRESSED, no new breakage.
Task C1: complete (commits ec4e3a1..c5e46d9 across c65173d + c5e46d9, review clean after 1 fix round).

Lane C session scope (Task C1, C2, + out-of-plan gitignore chore) complete.
Task C3 is human-only (buy code-signing cert) — no agent action, out of scope.
Final whole-branch review (opus, commits ec4e3a1..c5e46d9): Ready to merge
"With fixes". Important #1: plan doc (next-wave.md:322-329) still orders
`git rm openspec/config.yaml`, the deletion the owner blocked — landmine
for next session. Important #2: commit c5e46d9's message body is empty but
task-C1-report.md pastes a fabricated multi-paragraph body for it. Minor:
retry Remove-Item can abort the whole prune loop on a race (add
-ErrorAction SilentlyContinue to retry only); dangling skills-lock.json
ref in memory/juce9-api-traps-2026-08-25.md:44; memory note overstates how
many openspec-* skills read config.yaml.
Fix wave dispatched (all 5 findings, one dispatch per skill's "no
per-finding fixers" rule) — no amend (policy), report corrected to match
actual git history instead.
Fix wave done: c2231fc (Minor #3, -ErrorAction SilentlyContinue on retry
Remove-Item only) + 633e481 (Important #1 plan rewrite + tick C1/C2, Minor
#4, Minor #5). Important #2 (report commit-message correction) applied to
disk but NOT in either commit — caught separately: a rogue, never-tracked
.superpowers/sdd/.gitignore (bare `*`) had been silently blanket-ignoring
this ENTIRE session's SDD workspace since ~23:2x, contradicting this
repo's OWN root .gitignore comment (line 60: ".superpowers/ is
deliberately TRACKED ... do not re-add it here"). Removed the rogue file,
committed the whole workspace in 6cba3b1 (unrelated to the fix-wave
findings; separate self-caught infra issue).
Scoped re-review of fix wave (c5e46d9..633e481, findings 1/3/4/5 from diff,
finding 2 from live task-C1-report.md) dispatched.
Scoped re-review verdict: all 5 findings ADDRESSED, no new breakage.

FINAL REVIEW CLEAN. Branch commits (ec4e3a1..6cba3b1):
c65173d (C1 impl) c5e46d9 (C1 fix rd1) 7c91ee6 (C2) 8eb285d (gitignore
chore) c2231fc (final-review fix: retry SilentlyContinue) 633e481
(final-review fix: plan/memory docs) 6cba3b1 (recovered SDD audit trail,
removed rogue nested .gitignore).

NOTE deviating from generic skill default: did NOT `rm -rf` this plan's
workspace at finish — this project's root .gitignore explicitly records
`.superpowers/` as permanently TRACKED audit trail (owner decision
2026-08-22), and this workspace is now committed (6cba3b1). Deleting the
working copy of tracked, committed files is not what that skill step
means for a project with this override; git history already had that role
in the generic case, but here the intent is the files stay in the tree too.
Leaving `.superpowers/sdd/2026-08-27-next-wave/` in place.

Next: superpowers:finishing-a-development-branch.

---
## LANE R — 2026-09-06, session "merge-branches-subagent" (worktree chore-infra-prune-orphans-948d0e, branch claude_desk/merge-branches-subagent-c76c7f, base d58eac0 = main tip)
Scope this session: LANE R (Task R1, R2, R3). Lanes P/C done earlier; T1 done (lane T1 merged 9735a78); T2/C3 need a human.
Baseline: ctest -C Release 432/432 at d58eac0 (controller ran it; logs baseline-*.log).
Pre-flight: plan R verified against post-S/post-D code by read-only agent (opus) — 3 blocking mismatches (publish before detection; score units 0..1 vs kConfirmScore, not peakiness 10.0; two LaneAnalysis per slot). CONTROLLER RULINGS A-R1..A-R10 written into the plan as "Amendment lane R — 2026-09-06" (before Task R1). Owner may overturn: stereo max-over-lanes (A-R1/A-R8), post-asym score (A-R1), publish moved below detection (A-R2), bands vs kConfirmScore (A-R3), valid = detectionActive && scorer history (A-R4), hold-750 ms hysteresis (A-R5), keep spec §4 lambda (A-R6), release by controller only (A-R10).
Parked for OWNER (out of lane R scope, placement behaviour): CandidateScorer.cpp:51 gates on compile-time kDefaultThreshold, not the live analyzer threshold (A-R7).
Expected level change: 0 dB (readout only).
Task R1: dispatched (BASE 8951e6a, implementer opus, brief task-R1-brief.md)
Task R1: implementer DONE — f8c6f19 (code+tests), 6dea381 (report); 438/438; A-R2 primary option (publish block moved below detection pass, modelMutex_ gather left in place so notchCount is bit-identical and the score leads the notch list)
Task R1: review (opus) NEEDS FIXES — Important 1: A-R4 "since last reset" unimplemented — CandidateScorer::hasHistory() is "ever committed"; setSampleRate resets only Detector, setWidth resets nothing, so ringRiskValid publishes true on the first frame after a device/SR change over cross-rate history. Fix = blocksSinceReset counter in LaneAnalysis, zeroed in setSampleRate/setWidth (the alternative A-R4 itself names).
Task R1: CONTROLLER RULING on Important 1 — A-R4 governs (not "mirror placement"): the chip must go N/A after SR/device/width change until the lane has committed a block again. Counter in LaneAnalysis; no scorer arithmetic touched; 0 dB.
Task R1: minor (deferred): ScoreIsMaxOverBothLanesOfTheSlot only asserts peak > 0 — strengthen to > threshold or vs a lane-0-only baseline (tests/test_notchcontroller.cpp:1596-1620)
Task R1: minor (deferred): no test that score falls back to 0 on the frame after a high score (clear at NotchController.cpp:304-305 is correct by construction, unpinned)
Task R1: minor (deferred): redundant EXPECT_LT after EXPECT_FLOAT_EQ(score, 0) at tests/test_notchcontroller.cpp:1566-1567
Task R1: minor (deferred → R2/R3 decision): `if (!any) break;` (NotchController.cpp:271) exits before publish, so a dead tap freezes ringRiskScore/ringRiskValid at their last values (incl. a frozen Critical); consistent with frozen magnitudes; lastDataMs_ could gate the chip in R2/R3
Task R1: minor (deferred → lane closeout): memory/ note — hasHistory()/counter semantics; LINKED skip path contributes no score for the skipped bin (benign: placing lane scored it earlier in the same iteration)
Task R1: fix round 1/5 dispatched (resume implementer; finding: Important 1)
Task R1: fix round 1/5 (1 addressed, 0 open; commits 6dea381..afc4dfc; re-review (sonnet) clean: counter written only under the documented "detector stopped" precondition, saturating, increment beside commitBlock for every lane incl. LINKED, valid read before this frame's commit; 440/440)
Task R1: parked for OWNER (pre-existing, out of lane R scope): NotchController::setSampleRate has NO production caller — the detector never follows the device sample rate (device/SR change goes through setWidth via MainComponent.cpp:316-327 only). Placement-behaviour question; nobody in lane R touches it.
Task R1: complete (commits 8951e6a..afc4dfc, review clean after 1 fix round)
Task R2: dispatched (BASE afc4dfc, implementer opus, brief task-R2-brief.md)
Task R2: implementer DONE_WITH_CONCERNS — 7fe7636 (code+tests), f17a46b (report); 447/447; concerns: no screenshot (paint untouched, provider null → R3), extra guard thr<=0/non-finite → Unavailable, hold expiry drops straight to raw band, spec §2/§3 text → R3
Task R2: review (opus) NEEDS FIXES — Important 1: hold re-arms only on a STRICT step up (SpectrumView.cpp:706-711), so a sustained borderline score blinks Critical→Rising ~33 ms every 750 ms (Acceptance 4 violated); the flicker test stops before the hold expires so it cannot see it. Important 2: setController (SpectrumView.cpp:519-543) clears every per-slot field but not ringRiskHold_ → new slot shows the old slot's Critical for up to 750 ms.
Task R2: minor (deferred): dead `stepUpMs_ = nowMs` in the Unavailable branch (SpectrumView.cpp:698)
Task R2: minor (deferred): nowMs running backwards holds forever (SpectrumView.cpp:718) — add `|| nowMs < stepUpMs_`; clock is monotonic in practice
Task R2: minor (deferred → R3): header block (SpectrumView.h:214-221) never names riskForScore as the function R3's lambda must call
Task R2: minor (deferred): boundary test restates 0.55f*thr (tautological on its own; discriminating rows at thr=0.42 do the real work)
Task R2: fix round 1/5 dispatched (fresh implementer opus — harness cannot resume; FIX_BASE f17a46b; findings: Important 1, Important 2)
Task R2: fix round 1/5 (1 addressed, 1 partially — Important 2 residual: setController resets ringRiskHold_ but not the painted ringRisk_ (SpectrumView.cpp:519-548 vs timerCallback :750-754), so the old slot's badge can paint for ≤1 frame (33 ms) after a switch; commits f17a46b..e0badb5; 450/450)
Task R2: minor (deferred): tests/test_spectrumview.cpp:742 `EXPECT_GT (now - 100.0, 4.0 * kHoldMs)` sits on the exact FP boundary (passes by ~3e-12) — use 3.9×
Task R2: fix round 2/5 dispatched (fresh implementer sonnet; FIX_BASE e0badb5; finding: Important 2 residual)
Task R2: fix round 2/5 (1 addressed, 0 open; commits e0badb5..730b986; re-review (sonnet) clean: ringRisk_ reset beside the hold in setController, displayed-field test mutation-checked, FP self-check margin 3.9×; 451/451)
Task R2: complete (commits bbc1cfb..730b986, review clean after 2 fix rounds)
Task R3: dispatched (BASE 730b986, implementer opus, brief task-R3-brief.md)
Task R3: implementer DONE — 4342ec4 (wiring + 2 tests + tickForTest), 767d9d7 (snapshot tool stages CRITICAL via ringRiskForTest, printed+commented), bdc6265 (docs: spec/GIOI-THIEU/KY-THUAT/1.1.3 note/memory), 4cf0464 (report); 453/453; controller READ both PNGs: live = filled red CRITICAL chip, idle = hollow N/A, both legible; sent to owner.
Task R3: review (opus) NEEDS FIXES — Important 1: Acceptance 1 "PROVEN" cites the pinned test, which never calls tickForTest (headless suite pumps no timer) so it asserts the default field, not the wired idle path; console-idle.png likewise. Real coverage exists only as the last EXPECT in RingRiskFollowsTheMonitoredSlotAcrossASwitch. Fix = ticking companion test + point the citations at it; pinned assertion untouched.
Task R3: CONTROLLER RULING on Minor 2 (pinned test name now permanently satisfied): keep the name — spec and brief cite it; the companion test carries the wired-idle meaning.
Task R3: minor (fix in round 1, docs): GIOI-THIEU + 1.1.3 note say CRITICAL = "đang hoặc vừa đặt notch" — placement also needs capacity/guard-bin, so a full table can hold Critical with nothing placed; soften wording. KY-THUAT pending-list line unwrapped.
Task R3: minor (deferred): spec-ring-risk.md status header mixes Vietnamese into an English doc; two copySnapshot per tick is A-R6-mandated and documented
Task R3: fix round 1/5 dispatched (fresh implementer sonnet; FIX_BASE 4cf0464; findings: Important 1 + docs minors 3, 4)
Task R3: fix round 1/5 (2 addressed + Important 1 partially — companion ticking test 7deea20 mutation-checked, spec Acceptance 1 corrected 6b71dc7, GIOI-THIEU/1.1.3 CRITICAL wording softened, KY-THUAT re-wrapped; residual: task-R3-report.md §4 row 1 and §6 still carry the disproven "measured no-data" claim, only §8 explains it; commits 4cf0464..3247b16; 454/454)
Task R3: minor (deferred → final review): GUI bands Critical at score >= threshold (SpectrumView.cpp:679-684) while placement needs score > kConfirmScore strictly (NotchController.cpp:817) — chip may read Critical one tick before placement at the exact boundary; float equality, negligible in practice
Task R3: fix round 2/5 dispatched (fresh implementer sonnet; FIX_BASE 3247b16; finding: Important 1 residual — report §4/§6 in-place correction)
Task R3: fix round 2/5 (1 addressed, 0 open; commit 3247b16..a710b87 report-only; re-review (sonnet) clean)
Task R3: complete (commits cbce71f..a710b87, review clean after 2 fix rounds)
LANE R: final whole-branch review (opus, package review-d58eac0..a710b87.diff) + independent read-only verifier (az-harness:verifier) dispatched in parallel; deferred-minor and parked lines above are the triage list.
LANE R: verifier (opus, read-only) 10/10 CONFIRMED — 454/454 re-run, src/dsp diff EMPTY, only removed src lines are the moved dispatch block + one provider line, publish at NotchController.cpp:339-372 after detection :324-330, no hook in production, version still 1.1.2, no mojibake, tester note claims no rig acceptance.
LANE R: final whole-branch review (opus) — READY WITH FIXES. 0 Critical. Important: I-1 SpectrumView.h:214-221 still says "ringRiskProvider is still null ... task R3" and cites a task brief as the contract; I-2 installer/TESTER-NOTES.md:17 still tells testers the chip is dead (plan R3-4 named it); I-3 dead tap / stopped detector freezes the chip on the last band incl. CRITICAL (NotchController.cpp:282 `if (!any) break;` above publish; hold re-arms on the unchanged raw band) while spec §3 claims the opposite — decision required; I-4 plan R1/R2/R3 checkboxes still unticked. Minors: memory note says 451→453 (is 454); three different files cited as "the amendment"; persistence streak omitted from the CRITICAL wording; rest SHIP AS IS per triage.
LANE R: CONTROLLER RULING on I-3 — smallest honest change: the provider lambda in MainComponent returns Unavailable while the detectors are not running (the realistic frozen case: device stopped/restarting), so a stopped rig can never show a stale CRITICAL; the residual "tap alive but silent while running keeps the last band" is written into spec §3 as a stated exception with the lastDataMs_ timeout gate named as the follow-up for the owner. No DSP change, 0 dB.
LANE R: fix wave dispatched (ONE implementer, opus; FIX_BASE ac3195f; findings I-1, I-2, I-3 (per ruling), I-4 + minors: memory count, amendment citation, persistence-streak wording)
LANE R: fix wave done (commits ac3195f..fe67a13: 4a43b45 header comment + citations, 2ae9304 docs/plan ticks/tester note/memory, fe67a13 report; 454/454). I-1, I-2, I-4, minors DONE. I-3: implementer built the `!engine_.isRunning()` guard and MEASURED it wrong — no headless test opens a device, so isRunning() is false in every test and the guard made the chip permanently N/A (2 green tests went red); reverted; docs now state both freeze cases (rig stopped; tap alive-but-silent) as OPEN.
LANE R: CONTROLLER RULING — I-3 PARKED for OWNER (not load-bearing; spec/tester note now truthful). Recommended fix, next tools pass: GUI-side staleness gate — if snapshot.sequence has not advanced for > N ms (e.g. 500), the chip reads Unavailable; fixes stopped rig AND dead tap, no DSP change, headless-testable with tickForTest + a frozen FedController. Owner picks N.
LANE R: scoped re-review of the fix wave dispatched (sonnet; package review-ac3195f..fe67a13.diff)
LANE R: fix-wave re-review (sonnet) — I-1, I-2, I-4, minors ADDRESSED; I-3 parked per ruling with docs truthful in all 5 places; src/ changes are comment-only; no mojibake. Out of scope: spec Acceptance 1 "detector stopped" wording ambiguous vs "rig stopped" (future one-liner); installer/TESTER-NOTES.md header (v1.0.5/368/SHA) is release-step owned.
LANE R: READY — next: installer/release-alpha.ps1 (patch 1.1.2 -> 1.1.3, ctest gate), drop-folder TESTER-NOTES v1.1.3 section, commit bump + docs, handoff, merge --no-ff into main (owner said "merge" this session).
