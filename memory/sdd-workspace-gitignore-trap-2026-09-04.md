---
name: sdd-workspace-gitignore-trap
description: superpowers:subagent-driven-development's sdd-workspace script auto-writes .superpowers/sdd/.gitignore, silently defeating this repo's policy of tracking .superpowers/ as the audit trail
metadata:
  type: project
---

`scripts/sdd-workspace` in the `subagent-driven-development` skill
unconditionally writes `.superpowers/sdd/.gitignore` (content: bare `*`)
on every invocation — and `task-brief` / `review-package` call it
internally, so it comes back every time any of those three scripts run,
even right after you delete it. Its own comment says why: the skill's
generic default is "delete the workspace at finish, git history is the
record" — so it assumes the workspace should never be tracked at all.

**This repo overrides that default.** The root `.gitignore` (line ~60)
records an explicit owner decision from 2026-08-22: `.superpowers/` stays
TRACKED, because the untracked version of this exact mistake once meant
"the record of what was built, verified and disproved lived on one disk
and was never pushed." The 2026-08-19 SDD workspace
(`.superpowers/sdd/2026-08-19-az-soundtech-hands-free/`) is fully
committed, proving every session before this one already fought and won
this same fight — but apparently by deleting the rogue file silently
each time, without leaving a trace of why, so this session lost ~20
minutes of its own audit trail before noticing.

**Why:** the tool's default and this project's policy are opposite
defaults, and the tool wins by default because it runs after you.

**How to apply:** any SDD session in THIS repo — `git rm -f
.superpowers/sdd/.gitignore` (or plain `rm`, it's never tracked so no
`git rm` is even needed) right before your final commit of the plan
workspace, and check again if you called `sdd-workspace` / `task-brief`
/ `review-package` again afterward — they will silently recreate it. Do
not try to "fix" the skill script itself (it's shared, versioned
tooling outside this repo, `C:\Users\id_az\.claude\plugins\cache\...`)
— just remember to delete its output here every time. See
[[docs-drift-audit]] for the sibling lesson about this same session
(the openspec/ near-miss) — both are "a generic assumption silently
overrides a documented project-specific decision" failures.
