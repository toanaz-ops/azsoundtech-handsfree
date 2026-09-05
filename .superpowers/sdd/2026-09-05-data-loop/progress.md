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
Task 6: dispatched (BASE 03a9afd, implementer sonnet, brief task-6-brief.md)
Task 6: minor (deferred): MainComponent.cpp sink builds the full notch var (3x1025 roundSig3 + arrays) even when the logger is inactive — wrap in `if (sessionLogger_.isActive())`
Task 6: minor (deferred): global vs per-slot `tuning` events emit int vs double for rise_ms/q/depth_db — cast the global ones to double
Task 6: minor (deferred): no `mode` recorded at session start unless requestMode is called — log the current mode right after session_start in startSessionLog
Task 6: minor (deferred): new tests leave %TEMP%\az-handsfree-sessionlog\* dirs behind (self-cleaning next run); sessionHeader()/notchEventToVar() declared mid data-member block (plan-mandated); lastLoadSkipped_ not reset on a failed load
Task 6: complete (commits 03a9afd..2279109, review clean; extra read-only verifier 11/11 PASS)
Task 7: dispatched (BASE 04239d1, implementer sonnet, brief task-7-brief.md)
Task 7: review NEEDS FIXES — Important (plan-mandated): fixture ctx.bin_hz=12000 collapses recurrence into one x4 group; fix = 23.4375 (48000/2048). Minors: no recurrence coverage in the --expect gate, dead defaultdict import, chaining in first-match grouping, verdict-after-clear key reuse (theoretical).
Task 7: fix round 1/5 dispatched (fresh implementer sonnet; FIX_BASE c27f724; findings: Important fixture bin_hz + add --expect-recurrence-max gate + drop dead import)
Task 7: fix round 1/5 (3 addressed, 0 open; commits c27f724..d14e581; controller ran the tool: 1007.8 Hz x2 only, 4-flag gate exit 0)
Task 7: minor (deferred): first-match grouping can chain across adjacent bins; verdict-after-clear on a reused (slot,lane,index) key is theoretical
Task 7: complete (commits 04239d1..d14e581, review clean after 1 fix round)
Task 8: dispatched (BASE 1ca01e9, implementer sonnet, brief task-8-brief.md)
Task 8: review — 2 Important: memory note lacks the frontmatter (sibling sdd-workspace-gitignore-trap has it); release note bullet "slot mono → stereo không dò lại đuôi audio cũ của làn R" describes the REMOVED widen-reset (plan-mandated text, now false). Minor: KY-THUAT top-matter still v1.1.0.
Task 8: fix round 1/5 dispatched (fresh implementer sonnet; FIX_BASE fd95769; findings: frontmatter, drop the false bullet, top-matter version)
Task 8: fix round 1/5 (3 addressed, 0 open; commits fd95769..600f1b2)
Task 8: complete (commits 1ca01e9..600f1b2, review clean after 1 fix round; PNGs sent to owner)
Task 9: dispatched — independent read-only verifier (az-harness:verifier) against spec §4 tests 1-16 + final whole-branch review (opus) with the deferred-minor list
Task 9: verifier (opus, read-only) VERDICT FINDINGS — 3: (V1) test_gui_wiring DestroyingTheAppWhileADetectorIsPlacingNotchesDoesNotCrash never places a notch (21 ms session; scorer needs >=112.5 ms history) — spec test 15 not exercised; (V2) test_candidatescorer EXPECT_EQ(plain, b.score) tautological (delegate) — spec test 5 "bit-exact with the OLD scorer" needs a golden/analytic vector; (V3) roadmap row D says "release 1.1.2 alpha" while CMakeLists still 1.1.1 — wording must say pending. Non-blocking: failNextSetNotchOnLaneForTest is a production-compiled hook (default off); try_lock-from-owner in the mutex test is UB if it ever fails (re-entrant clearNotch is the real teeth). Checks 1,2,4-8,10 PASS; 430/430 reproduced; audio path byte-identical (no outbox_/NotchCommand line changed).
Task 9: final whole-branch review (opus) — READY AFTER FIX WAVE. Important: I-1 NotchListPanel re-placed notch inherits the previous verdict for 60 s (buttons_ keyed by identity, state never reset on re-entry); I-2 sink builds the full var when logger inactive; I-3 main.cpp ignores a failed startSessionLog (silent). Minors M-1..M-13 (write failures swallowed -> write_failed in session_end; verdict.age_ms wall vs notch_clear.age_ms live — document; ref_age_ms outside hasRef guard; mono-skip count never logged -> preset_load event; Recorder declared after Harness (latent UAF in tests); memory note §1-3 inaccuracies; SessionLogger.h invariant comment; tuning int/double; ensureButtonsFor unused param; statusW360 name; snapshot null deref; size numbers ~23 KB/notch_set, LINK double; killThread low). Triage: FIX NOW = M-1, M-7, T2 penalty assertion, M-9, I-2, M-8, mode-at-start. Also: spec §3.4 onVerdict has 6 params now (add A-11).
Task 9: FIX WAVE dispatched (ONE implementer, opus; FIX_BASE = HEAD after this ledger commit; findings V1 V2 V3 + I-1 I-2 I-3 + M-1..M-11 + triage FIX NOW + A-11)
Task 9: fix wave done (commits 48f3db7..ae6ddbe: e468e76 code, 9b59a1f tests, ae6ddbe docs; 432/432; teardown test places 14-30 notches/round). Scoped re-review (opus) + verifier re-check (V1/V2/V3) dispatched.
Task 9: fix-wave re-review (opus): 20/20 ADDRESSED, no new breakage; verifier re-check (sonnet): V1/V2/V3 PASS, VERDICT CLEAN, 432/432, teardown rounds 14-30 notch_set each, every log ends with session_end.
Task 9: parked — I-1 residual: a notch cleared and re-placed BETWEEN two 4 Hz refreshes still inherits the verdict — ruling: real but bounded (<250 ms), strictly better than before; lane C must treat verdict-then-immediate-re-place as suspect; owner may lower the window later.
Task 9: parked — showMessage() is sticky for the session (log-failure text replaces the rig readout) — ruling: matches the existing onMessage precedent; acceptable for alpha.
Task 9: parked — logstats.py does not surface write_failed — ruling: add in the next tools pass; the field is in the file.
Task 9: parked — spec §3.2 opening still says "~12-18 KB"; superseded by the next sentence — ruling: nit, spec is historical.
Task 9: complete — lane D READY; next: release-alpha.ps1 (1.1.1 -> 1.1.2), docs status flip, handoff.
