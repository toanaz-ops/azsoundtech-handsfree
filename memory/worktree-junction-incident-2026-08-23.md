# Worktree removal follows junctions — the trap fired a third time

**Date:** 2026-08-23 · **Caused by:** agent session cleaning merged worktrees

## What happened

Three lane worktrees under `.claude/worktrees/` were fully merged and cleaned
up with `git worktree remove --force`. Two of them contained an
`external/JUCE` **directory junction** pointing at the main repo's shared JUCE
checkout (`docs/superpowers/runbooks/parallel-lanes.md`, third trap). The
removal followed the junction and deleted the main repo's `external/JUCE`
contents. `git submodule status` showed the leading `-` (uninitialized) and
the directory was empty.

The runbook already documented this exact failure ("that has already happened
once on this project"). It fired again because the cleanup session checked
`git status` inside each worktree but never checked **for junctions before
removing**. The runbook's warning lives in a lane report; the cleanup step
never reads lane reports.

## Recovery

```
git submodule update --init --recursive   # re-clones from github.com/juce-framework/JUCE
cmake --build build --config Release      # both targets
ctest -C Release                          # 198/198 passed
```

Nothing was lost: JUCE is vendored read-only code pinned at `e18f7f5`. Cost:
one re-clone plus a near-full rebuild (~10 min).

## The rule going forward

Before ANY `git worktree remove` in this repo:

1. `Get-Item <worktree>\external\JUCE | Select LinkType` — if `LinkType` is
   `Junction`, delete the junction FIRST:
   `cmd /c rmdir "<worktree>\external\JUCE"` (rmdir on a junction removes only
   the link, never the target).
2. Then `git worktree remove`.

Never trust that "clean working tree" means safe to remove. A junction is not
visible in `git status`.
