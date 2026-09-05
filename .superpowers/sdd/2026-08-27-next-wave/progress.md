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
