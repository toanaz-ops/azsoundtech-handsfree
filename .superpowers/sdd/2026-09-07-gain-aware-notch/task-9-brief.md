### Task 9: the log and the preset learn about retunes

**Mức level dự kiến (spec §3):** **0 dB** on the audio path. `savePreset` changes what a saved file CONTAINS: a notch is written at its `deepestDb` instead of its current rung, so reloading a preset saved during a quiet moment now restores the depth the room actually needed — up to **18 dB deeper on reload** than a 1.1.3-format save of the same moment would have been, but never deeper than the ceiling on the next tick. And `notchDefaults` is now written, so a reloaded preset keeps its ceiling instead of silently falling back to `PresetNotchDefaults`' −12 dB (`PresetManager.h:146`) — which on a rig tuned at −18 was capping every detector notch two rungs too shallow.

**Files:**
- Modify: `src/app/MainComponent.cpp:54-77` (a **free** `retuneReasonName` in the same anonymous namespace as `originName` (`:54`) and `reasonName` (`:66`) — m-5: it is not a `MainComponent::` member, and declaring it as one will not link), `:559-612` (`notchEventToVar`), `:950-962` (`savePreset`'s notch loop), `:983-985` (`notchDefaults`)
- Modify: `src/app/MainComponent.h:188` (the `notchEventToVarForTest` wrapper goes in the **public** section, next to `getNotchControllerForTest`; m-4: `savePreset` is declared at `:103` and `notchEventToVar` is a private `static` at `:256`, so the wrapper cannot live beside either)
- Modify: `tools/logstats.py:43-64` (the event dispatch), `:91-118` (`print_report`), `:124-127` (the `--expect-*` flags in `main`)
- Modify: `tests/fixtures/session-sample.jsonl`, `tests/CMakeLists.txt:113-117` (m-1: `add_test(NAME logstats_fixture` starts on line **113**, inside the `if(Python3_Interpreter_FOUND)` opened at 112, and closes at 117 — verified by grep in this worktree on 2026-09-07)
- Test: `tests/test_gui_wiring.cpp`, `tests/test_presetmanager.cpp`

**Interfaces:**
- Consumes (Tasks 4, 7, 8): `NotchEvent::Kind::Retune`, `RetuneReason`, `fromDepthDb`, `SnapshotNotch::deepestDb`.
- Produces: the log schema `ev: "notch_retune"` with keys `slot`, `lane`, `index`, `hz`, `q`, `depth_db`, `origin`, `reason` (`deepen|release|reclamp|ceiling`), `from_db`, `age_ms`. Task 10 documents it.

- [ ] **Step 1: Write the failing tests**

In `tests/test_gui_wiring.cpp`, after `savePreset`'s existing lane test (around line 1129).

**M-1 — use this file's real helpers.** `TempDir`, `pumpOneBlockThroughSlotZero` and `notchControllerForTest` do not exist anywhere in the repo; the v1 plan invented all three and the tests would not have compiled. `SavePresetWritesAFileThatLoadPresetReopensIdentically` at `:1016-1041` is the shape to copy, and it is copied literally below:

- `const juce::ScopedJuceInitialiser_GUI juceInit;` is the **first line** of the test — constructing `MainComponent` without it is undefined;
- the accessor is `app.getNotchControllerForTest (0)` (declared `MainComponent.h:188`), and it returns a pointer that must be null-checked;
- there is no `TempDir`: the file is an explicit `juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (...)`, `deleteFile()`d before the save and again after the load;
- a snapshot is published by hand — `controller0->setSampleRate (48000.0)`, then `app.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size())`, then `controller0->runOnce()`. The rate matters: `saveToFile` validates each notch's freq against the published rate's Nyquist, and `savePreset` refuses a preset whose rate is 0 (memory/preset-save-roundtrip-2026-09-05.md).

```cpp
// RED IF savePreset writes the RUNNING depth instead of deepestDb (Q11,
// M-10). A preset saved during a quiet moment must record what the room
// needed, not the rung the release ladder had wound back to.
TEST (GuiWiring, SavePresetRecordsTheDeepestDepthNotTheRestingOne)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    auto* controller0 = app.getNotchControllerForTest (0);
    ASSERT_NE (controller0, nullptr);
    controller0->setSampleRate (48000.0);

    // A notch placed deep, then wound back the way the release ladder does.
    ASSERT_TRUE (controller0->setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    ASSERT_TRUE (controller0->retuneForTest (0, 0, -6.0,
                                             NotchController::RetuneReason::Release));

    std::vector<float> hop (512, 0.25f);
    app.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size());
    controller0->runOnce();          // publish a snapshot carrying deepestDb

    auto outFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("az-handsfree-laneg-deepest.json");
    outFile.deleteFile();
    ASSERT_TRUE (app.savePreset (outFile));

    const auto result = PresetManager::loadFromFile (outFile);
    outFile.deleteFile();

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    // One entry per (slot, lane, index) since lane S, and setNotch named lane
    // 0 only -- so exactly one notch comes back.
    ASSERT_EQ (result.preset.notches.size(), 1u);
    EXPECT_DOUBLE_EQ (result.preset.notches[0].depthDB, -18.0);
}

// RED IF notchDefaults stops round-tripping (Q11). Without it a reloaded
// preset caps every detector notch at PresetNotchDefaults' -12 dB, silently
// undoing the ceiling the show was tuned at.
TEST (GuiWiring, SavePresetRoundTripsTheCeilingThroughNotchDefaults)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;
    auto* controller0 = app.getNotchControllerForTest (0);
    ASSERT_NE (controller0, nullptr);
    controller0->setSampleRate (48000.0);
    controller0->setNotchDefaults (44.0, -24.0);
    ASSERT_TRUE (controller0->setNotch (0, 0, 1000.0, 44.0, -24.0,
                                        NotchController::Origin::Detector));

    std::vector<float> hop (512, 0.25f);
    app.getAudioEngine().getTapBuffer (0).write (hop.data(), hop.size());
    controller0->runOnce();

    auto outFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("az-handsfree-laneg-ceiling.json");
    outFile.deleteFile();
    ASSERT_TRUE (app.savePreset (outFile));

    const auto result = PresetManager::loadFromFile (outFile);
    outFile.deleteFile();

    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.Q,       44.0);
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.depthDB, -24.0);
}

// RED IF a Retune event is serialised as notch_clear (B-3 of the spec's own
// critique list). The `ev` key is what logstats.py dispatches on, and a
// mislabelled retune closes the notch's record at the first 300 ms deepening.
// No MainComponent is needed: the serialiser is static.
TEST (GuiWiring, RetuneEventsAreLoggedUnderTheirOwnEventName)
{
    NotchController::NotchEvent e;
    e.kind = NotchController::NotchEvent::Kind::Retune;
    e.slot = 0; e.lane = 1; e.index = 3;
    e.hz = 1007.8f; e.q = 30.0f;
    e.fromDepthDb = -12.0f; e.depthDb = -18.0f;
    e.origin = NotchController::Origin::Detector;
    e.retuneReason = NotchController::RetuneReason::Deepen;
    e.ageMs = 612.5;

    const juce::var v = MainComponent::notchEventToVarForTest (e);
    EXPECT_EQ (v["ev"].toString(), "notch_retune");
    EXPECT_EQ (v["reason"].toString(), "deepen");
    EXPECT_DOUBLE_EQ ((double) v["from_db"], -12.0);
    EXPECT_DOUBLE_EQ ((double) v["depth_db"], -18.0);
    EXPECT_DOUBLE_EQ ((double) v["age_ms"], 612.5);
    EXPECT_EQ ((int) v["lane"], 1);
    EXPECT_EQ ((int) v["index"], 3);
}

// RED IF a reason name is dropped or renamed -- logstats.py and the tester
// notes both read these four strings.
TEST (GuiWiring, EveryRetuneReasonHasItsOwnName)
{
    using RR = NotchController::RetuneReason;
    const RR reasons[] = { RR::Deepen, RR::Release, RR::Reclamp, RR::Ceiling };
    const char* names[] = { "deepen", "release", "reclamp", "ceiling" };
    for (int i = 0; i < 4; ++i)
    {
        NotchController::NotchEvent e;
        e.kind = NotchController::NotchEvent::Kind::Retune;
        e.retuneReason = reasons[i];
        EXPECT_EQ (MainComponent::notchEventToVarForTest (e)["reason"].toString(),
                   juce::String (names[i]));
    }
}
```

`notchEventToVar` is a private `static` at `MainComponent.h:256`; expose it the way the file already exposes other internals — in the **public** section, next to `getNotchControllerForTest` at `:188` (m-4):

```cpp
    NotchController* getNotchControllerForTest (int slot);

    // TEST ACCESSOR ONLY -- the serialiser is otherwise reachable only through
    // a live detector thread and a real log file. Public here, next to the
    // controller accessor, because notchEventToVar itself is private (:256)
    // and stays that way.
    static juce::var notchEventToVarForTest (const NotchController::NotchEvent& e)
        { return notchEventToVar (e); }
```

In `tests/test_presetmanager.cpp`, after `NotchDefaultsSurviveTheRoundTrip` — which starts at line **496**, not 497 (grepped in this worktree on 2026-09-07; the cross-check's m-2 was one off):

```cpp
// RED IF the deepest rung of the lane-G ladder stops round-tripping. -24 dB
// is a legal ceiling and a legal notch depth, and a preset that cannot carry
// it caps the app two rungs shallower than the operator asked for.
TEST (PresetManager, TheFullLadderRangeSurvivesTheRoundTrip)
{
    Preset p;
    p.sampleRate = 48000.0;
    p.notchDefaults.Q       = 30.0;
    p.notchDefaults.depthDB = -24.0;
    PresetNotch n;
    n.index = 0; n.freq = 1000.0; n.Q = 30.0; n.depthDB = -24.0; n.slot = 0; n.lane = 0;
    p.notches.push_back (n);

    const auto result = PresetManager::fromJSON (PresetManager::toJSON (p));
    ASSERT_TRUE (result.ok) << result.errors.joinIntoString ("; ");
    EXPECT_DOUBLE_EQ (result.preset.notchDefaults.depthDB, -24.0);
    ASSERT_EQ (result.preset.notches.size(), 1u);
    EXPECT_DOUBLE_EQ (result.preset.notches[0].depthDB, -24.0);
}
```

- [ ] **Step 2: Extend the log fixture and its test**

**M-7 — the fixture has to tell a story that could actually have happened.** The v1 lines said `from_db:-12, depth_db:-18, reason:deepen` for the lane-1 index-0 notch, whose `notch_set` on line 4 already reads `depth_db:-18`: it deepens FROM a depth it never held, TO the depth it was placed at. A fixture that contradicts itself is worse than none — it is what the next reader will copy. Make the whole life of that notch consistent with the lane-G ladder:

1. **Edit line 4** (`notch_set`, `t: 5001.0`, lane 1, index 0): `"depth_db":-18` → `"depth_db":-6`. That is a lane-G placement — the shallowest rung. Nothing else on the line changes; `rise:1` is rNorm, which saturates, so it is consistent with either band.
2. **Insert after line 4** — a deepen 300 ms later, the `kDeepenAfterMs` gate exactly:

```
{"ev":"notch_retune","t":5301.0,"slot":0,"lane":1,"index":0,"hz":1007.8,"q":30,"depth_db":-12,"from_db":-6,"origin":"detector","reason":"deepen","age_ms":300.0}
```

3. **Insert after line 11** (the `verdict` at `t: 31000.0`), so the file stays in ascending `t` order — the first release:

```
{"ev":"notch_retune","t":35400.0,"slot":0,"lane":1,"index":0,"hz":1007.8,"q":30,"depth_db":-6,"from_db":-12,"origin":"detector","reason":"release","age_ms":30399.0}
```

**m-A — why 35400 and not the 31000 rev 2 used.** The first release costs
`kReleaseFirstMs` = **30 000 ms of quiet measured from the last depth change**, and
the last depth change is the deepen at `t: 5301`. `31000 − 5301 = 25 699 ms`: the
rev-2 fixture released 4.3 s before the ladder allows it, i.e. it depicted a run the
shipped code cannot produce. Anything at or after `5301 + 30 000 = 35 301` is legal;
**35400** is used for a round number with 99 ms of slack. `age_ms` on that line is
measured from the SET, not from the deepen: `35400 − 5001 = 30 399.0`.

4. **Edit line 12** (the existing `auto_release`): the next rung costs
   `kReleaseStepMs` = 10 000 ms, so the Clear moves from `t: 41000.0` to exactly
   `35400 + 10 000 =` **`45400.0`**, and its `age_ms` from `36000.0` to
   `45400 − 5001 =` **`40399.0`**:

```
{"ev":"notch_clear","t":45400.0,"slot":0,"lane":1,"index":0,"hz":1007.8,"origin":"detector","reason":"auto_release","age_ms":40399.0}
```

`session_end` stays at `t: 60000.0`, so the file is still in ascending `t` order and
still ends after every notch.

The notch now reads: placed −6 at 5001, deepened to −12 at 5301 (exactly
`kDeepenAfterMs` later), released back to −6 at 35400 (exactly `kReleaseFirstMs`
after the deepen), cleared by `auto_release` at 45400 (exactly `kReleaseStepMs`
after the release), held 40.4 s. A reader that CLOSES on a retune loses that clear
and the fixture's counts move, which is what the `--expect-*` flags below catch.

**The `--expect-*` numbers do not move**, and that is worth checking rather than
assuming (`tools/logstats.py:43-63, 80-88`): `notches` counts `notch_set` lines
(still 4 — the retunes are not sets); `verdicts` counts notches whose `verdict` is
`good`/`false` (still 3) and `false` (still 1); recurrence groups by Hz within one
bin, so 1007.8 / 2437.5 / 1007.8 / 482.0 still gives a max of 2. None of these read
`t` or `age_ms` at all, so moving two timestamps changes none of them; only the new
`--expect-retunes 2` is new.

Then extend the fixture test in `tests/CMakeLists.txt:113-117`:

```cmake
    add_test(NAME logstats_fixture
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/logstats.py
                ${CMAKE_SOURCE_DIR}/tests/fixtures/session-sample.jsonl
                --expect-notches 4 --expect-verdicts 3 --expect-false 1
                --expect-recurrence-max 2 --expect-retunes 2)
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --config Release 2>&1 | tail -5`
Expected: compile error — `notchEventToVarForTest` and `retuneForTest` unresolved on `MainComponent`.

Run: `python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 4 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2`
Expected: FAIL — `unrecognized arguments: --expect-retunes`. Then, with that flag removed, it exits 0 and reports nothing new: m-7 — the current reader does **not** miscount, it **ignores** the two lines. `summarise` is an `if/elif` chain on `ev` with no `else` (`tools/logstats.py:45-63`), so an unknown name simply falls through: `notches` stays 4 and every other total is unchanged. That is the correct behaviour for an old reader meeting a new log, and the branch added in Step 5 must preserve it. The defect being fixed is not a wrong number — it is a **blind spot**: the depth column shows the placement depth forever, and the retunes are invisible.

- [ ] **Step 4: Implement in `src/app/MainComponent.cpp`**

Add next to `reasonName` (line 66) — m-5: a **free** function in the same anonymous namespace as `originName` (`:54`) and `reasonName` (`:66`), with no `MainComponent::` qualifier. `notchEventToVar` calls it unqualified from the same translation unit; writing it as a member would need a header declaration it does not have and would not link.

```cpp
const char* retuneReasonName (NotchController::RetuneReason r)
{
    switch (r)
    {
        case NotchController::RetuneReason::Deepen:  return "deepen";
        case NotchController::RetuneReason::Release: return "release";
        case NotchController::RetuneReason::Reclamp: return "reclamp";
        case NotchController::RetuneReason::Ceiling: return "ceiling";
    }
    return "deepen";
}
```

Replace the head of `notchEventToVar` (lines 559-577):

```cpp
juce::var MainComponent::notchEventToVar (const NotchController::NotchEvent& e)
{
    using Ev = NotchController::NotchEvent;
    // B-3: `ev` is the key tools/logstats.py dispatches on, and the ternary
    // this replaced would have written every Retune as a notch_clear -- which
    // closes the notch's record at its first 300 ms deepening and makes every
    // deepened notch look like a 300 ms false positive.
    const char* evName = e.kind == Ev::Kind::Set    ? "notch_set"
                       : e.kind == Ev::Kind::Retune ? "notch_retune"
                                                    : "notch_clear";
    auto v = SessionLogger::makeEvent (evName);
    auto* o = v.getDynamicObject();
    o->setProperty ("slot", e.slot);
    o->setProperty ("lane", e.lane);
    o->setProperty ("index", e.index);
    o->setProperty ("hz", (double) e.hz);
    o->setProperty ("q", (double) e.q);
    o->setProperty ("depth_db", (double) e.depthDb);
    o->setProperty ("origin", originName (e.origin));

    if (e.kind == Ev::Kind::Retune)
    {
        o->setProperty ("reason", retuneReasonName (e.retuneReason));
        o->setProperty ("from_db", (double) e.fromDepthDb);
        o->setProperty ("age_ms", e.ageMs);
        return v;   // no score, no ctx: a retune is not a placement decision
    }

    if (e.kind == Ev::Kind::Clear)
    {
        o->setProperty ("reason", reasonName (e.reason));
        o->setProperty ("age_ms", e.ageMs);
        return v;
    }
```

In `savePreset`, line 958:

```cpp
            // Q11 / M-10: the depth the room NEEDED, not the rung the release
            // ladder happens to be resting on when SAVE was pressed. A preset
            // saved during a quiet stretch would otherwise reload two rungs
            // too shallow and let the same howl come back.
            pn.depthDB = sn.deepestDb;
```

and after `preset.sampleRate = presetRate;` (line 985):

```cpp
    // Q11: the CEILING round-trips too. Without this a reloaded preset falls
    // back to PresetNotchDefaults' -12 dB (PresetManager.h:146) and caps every
    // detector notch two rungs shallower than the show was tuned at -- a bug
    // that predates lane G and that lane G's ladder makes load-bearing.
    preset.notchDefaults.Q       = notchControllers_[0]->getNotchQ();
    preset.notchDefaults.depthDB = notchControllers_[0]->getNotchDepthDb();
```

- [ ] **Step 5: Implement in `tools/logstats.py`**

In `summarise`, extend the `notch_set` record and add the branch (lines 48-63):

```python
        if ev == "notch_set":
            n = {"slot": e.get("slot"), "lane": e.get("lane"), "index": e.get("index"),
                 "hz": float(e.get("hz", 0.0)), "origin": e.get("origin", "?"),
                 "set_t": float(e.get("t", 0.0)), "clear_t": None, "reason": None,
                 "verdict": None, "score": e.get("score"),
                 # lane G: the depth a notch is RUNNING at, and how many times
                 # it moved. Both start at the placement values.
                 "depth_db": e.get("depth_db"), "deepest_db": e.get("depth_db"),
                 "retunes": 0}
            notches.append(n)
            open_by_key[key(e)] = n
        elif ev == "notch_retune":
            # lane G: a retune UPDATES the open record. It must never close it
            # -- a deepening 300 ms after placement would otherwise read as a
            # 300 ms notch, and every deepened howl in the log would look like
            # a false positive. An unknown ev name falls through every branch
            # here, so a NEW event added later cannot corrupt an old reader
            # either; that is why this is an if/elif chain and not a lookup
            # that raises.
            n = open_by_key.get(key(e))
            if n is not None:
                n["depth_db"] = e.get("depth_db")
                if n["deepest_db"] is None or (e.get("depth_db") is not None
                                               and float(e["depth_db"]) < float(n["deepest_db"])):
                    n["deepest_db"] = e.get("depth_db")
                n["retunes"] += 1
        elif ev == "verdict":
```

Add the total to the returned dict (line 82-88):

```python
    return {
        "header": header, "duration_ms": duration, "modes": modes, "notches": notches,
        "groups": sorted(groups, key=lambda g: -g["count"]),
        "verdicts": len(judged), "false": false_count,
        "unjudged": len(notches) - len(judged),
        "retunes": sum(n["retunes"] for n in notches),
        "dropped": end.get("dropped_events"),
    }
```

In `print_report`, widen the table (lines 98-103):

```python
    print(f"{'#':>3} {'slot':>4} {'lane':>4} {'hz':>8} {'origin':<10} {'depth':>7} {'deep':>6} "
          f"{'rt':>3} {'held':>8} {'verdict':<8} {'cleared by':<20}")
    for i, n in enumerate(s["notches"], 1):
        held = (n["clear_t"] if n["clear_t"] is not None else s["duration_ms"]) - n["set_t"]
        lane = "R" if n["lane"] == 1 else "L"
        depth = "?" if n["depth_db"] is None else f"{float(n['depth_db']):.0f}dB"
        deep = "?" if n["deepest_db"] is None else f"{float(n['deepest_db']):.0f}dB"
        print(f"{i:>3} {n['slot']:>4} {lane:>4} {n['hz']:>8.1f} {n['origin']:<10} "
              f"{depth:>7} {deep:>6} {n['retunes']:>3} {fmt_ms(held):>8} "
              f"{(n['verdict'] or '-'):<8} {(n['reason'] or 'still active'):<20}")
```

and add the total to the summary line (line 114-116):

```python
    print(f"notches {total}  retunes {s['retunes']}  judged {v}  false {s['false']}"
          f"  false-rate {(s['false'] / v * 100 if v else 0):.0f}%"
          f"  unjudged {s['unjudged']} ({(s['unjudged'] / total * 100 if total else 0):.0f}%)")
```

In `main`, add the flag and its check:

```python
    ap.add_argument("--expect-recurrence-max", type=int)
    ap.add_argument("--expect-retunes", type=int)
```

```python
    if args.expect_retunes is not None and s["retunes"] != args.expect_retunes:
        failures.append(f"retunes {s['retunes']} != {args.expect_retunes}")
```

The file is already opened with `encoding="utf-8"` in `load()` (line 19) and writes nothing back — repo rule 6 is satisfied; do not change that call.

- [ ] **Step 6: Run the log tool by hand, then the suites**

Run: `python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 4 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2`
Expected: exit 0, and the table's first row shows `-6dB` running / `-12dB` deepest / `2` retunes with `held` ~40.4 s (5001 → 45400, m-A) — the record stayed OPEN across both retunes.

Run:
```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release -R "GuiWiring|PresetManager|logstats" --output-on-failure
```
Expected: PASS.

- [ ] **Step 7: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (526).

- [ ] **Step 8: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/MainComponent.h src/app/MainComponent.cpp tools/logstats.py tests/fixtures/session-sample.jsonl tests/CMakeLists.txt tests/test_gui_wiring.cpp tests/test_presetmanager.cpp
```
```bash
git commit -m "feat(app): log notch_retune, save deepestDb, round-trip the notch ceiling"
```

---

