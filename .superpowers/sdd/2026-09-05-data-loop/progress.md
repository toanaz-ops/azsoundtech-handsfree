# SDD ledger — plan: docs/superpowers/plans/2026-09-05-data-loop.md

Base: main 797bca1 (v1.1.1). Branch claude_desk/lane-d-data-loop-239597. Plan commits 0889084 + follow-up. Baseline suite 402/402 (worktree, 2026-09-05 11:05).

2026-09-05 11:20 — INCIDENT: session 'handsfree: main branch' pruned this live worktree (dir reused an old chore name). Re-added with git worktree add; build/, submodules, asiosdk restored. main moved to 9735a78 (lane T1 merged); NOT merged into this branch (merge needs owner).
2026-09-05 11:20 — COLLISION: second running session 'HANDSFREE: Data loop lane D implementation' on branch claude_desk/data-loop-lane-d-43f3d9 (no commits). Messaged both sessions; owner to pick one. This session proceeds on its committed plan unless told otherwise.

Task 1: dispatched (BASE 834701f, implementer sonnet, brief task-1-brief.md)
Task 1: review 1 (opus) on 834701f..34ed811 — spec OK; Important x2: (1) log() racing stop() loses lines silently, breaks lines+dropped==calls+2; (2) log() mutates caller's var through const& (stamps "t" on shared DynamicObject). Fix round 1 dispatched.
Task 1: minor (deferred): failed start() leaves currentFile() at previous session (SessionLogger.cpp:39-44)
Task 1: minor (deferred): keepFiles=0 means "only current file", undocumented; release script's -Keep 0 means "no prune"
Task 1: minor (deferred): write/delete/stopThread failures swallowed; suggest sticky writeFailed_ folded into session_end
Task 1: minor (deferred, plan-mandated): EXPECT_GT(dropped,0) timing-dependent if writer wakes mid-burst (test_sessionlogger.cpp:118)
Task 1: minor (deferred): filename uniqueness relies on ms suffix; no collision guard in start()
Task 1: minor (deferred): stop() twice / start() twice / log() after stop / dtor-without-stop untested

2026-09-05 11:50 — MERGE (owner decision "gộp thành quả, dừng cái yếu hơn"): session A archived. Lane D continues in session B on branch feat/data-loop, worktree peakiness-sweep-tool-c81129, BASE = main 9735a78. Cherry-picked: plan 0889084+834701f → 7cf5b71+ebc961d; Task 1 34ed811 → 5da3b2a. Rebuilt here: 406/406. Task 1 review (task-1-review.md): NEEDS FIXES — 2 Important → fix round next. plan-session-B-draft.md kept for reference only.
Task 1: fix round 1/5 dispatched (fresh implementer sonnet — original lived in archived session A; FIX_BASE = HEAD after merge commit; findings: Important 1 log()/stop() race, Important 2 const-var mutation, + Minor 3/7/8 per B-1)
Task 1: fix round 1/5 (5 addressed, 1 open — NEW Important: tests/test_sessionlogger.cpp LogRacingStopNeverLosesALine flaky ~27% on a false invariant and never reaches the race; commits 7515085..418f11c)
Task 1: minor (deferred): session_end.dropped_events can under-count a producer that loses the queueMutex_ race after stop()'s read — droppedEvents() is authoritative; document, no test under concurrency
Task 1: minor (deferred): filename collision suffix _10+ breaks lexical order (needs 10 sessions in one ms)
Task 1: minor (deferred): file_ read unsynchronised by currentFile(); stopThread timeout -> killThread inside drainToFile lock would deadlock (pre-existing); review Minors 4/5/6 still open
Task 1: fix round 2/5 (1 addressed, 0 open; commits 418f11c..a3a24cb; re-review 20/20 repeat clean)
Task 1: minor (deferred): SessionLogger.h:1-10 still states the strict "lines + dropped == calls + 2" invariant as fact; SessionLogger.cpp:131-140 documents the under-count — align the header comment in the final pass
Task 1: complete (commits 5da3b2a..a3a24cb, review clean after 2 fix rounds)
Task 2: dispatched (BASE 4fca63f, implementer sonnet, brief task-2-brief.md)
Task 2: minor (deferred, plan-mandated): test_candidatescorer.cpp:402-403 asserts kHarmonicPenalty for every above-threshold candidate; only true because the synthetic signal has no peaky candidate outside 1.4x-4.1x of 300 Hz — key it off frequencyHz in the final pass
Task 2: complete (commits 4fca63f..ca390d4, review clean)
Task 3: dispatched (BASE ca390d4, implementer sonnet, brief task-3-brief.md)
Task 3: review (opus) NEEDS FIXES — Important 1 (plan-mandated A-9 widen-reset): Detector::reset on setWidth 1->2 leaves CandidateScorer history/EMA stale then zero-saturates rNorm/mNorm for ~200 ms on the returning lane; Important 2: header says sink runs on the detector thread but stop() delivers on the caller's thread; Minors 3-6.
Task 3: CONTROLLER RULING on Important 1 — REMOVE the widen-reset behaviour from lane D (revert the setWidth 1->2 block + WideningResetsLaneOneDetectorState). Reason: owner constraint "lane D does not touch the audio/detection path, 0 dB"; A-9 is a lane-S loose end outside spec D; the reset as written is MORE permissive than the shipped 1.1.1 behaviour. Open for the OWNER: whether lane S (or a follow-up) gates detection on a re-entering lane for riseReferenceMs after a widen. Reverted code is preserved in git at 4cad420.
Task 3: fix round 1/5 dispatched (fresh implementer sonnet; FIX_BASE 4cad420; findings: Important 1 (revert), Important 2, Minor 3, 4, 5, 6)
Task 3: fix round 1/5 (6 addressed, 0 open; commits 4cad420..00d8792; re-review clean, 421/421 reproduced)
Task 3: minor (deferred): stop()'s 8-pass drain can strand the last event of a sink that re-enters on EVERY delivery (not counted as dropped) — document in the final pass
Task 3: complete (commits ca390d4..00d8792, review clean after 1 fix round; widen-reset REMOVED by controller ruling — owner decision pending)
Task 4: dispatched (BASE bea2d03, implementer sonnet, brief task-4-brief.md)
Task 4: complete (commits bea2d03..ae3cfa3, review clean)
Task 5: dispatched (BASE ae3cfa3, implementer sonnet, brief task-5-brief.md)
Task 5: minor (deferred, plan-mandated): ensureButtonsFor(key, notch) ignores `notch` — drop the parameter in the final pass
Task 5: complete (commits ae3cfa3..5fee378, review clean; controller read shots/console-live.png: VERDICT header + GOOD/FALSE legible on all rows, HELD intact)
