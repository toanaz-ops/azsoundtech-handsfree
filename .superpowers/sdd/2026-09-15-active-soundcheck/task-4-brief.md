### Task 4: the two additive `NotchController` changes, and the reason name that reaches the log

**Mức level dự kiến (spec §3):** **0 dB.** One new field on a snapshot struct and one new enumerator. No behaviour changes; `setNotch`, `clearNotch` and the release ladder are untouched.

**Why these two, and only these two** (spec §4.6, §6): `SnapshotNotch` today carries `frequency, Q, depthDB, deepestDb, channel, index` and no `origin` (`src/app/NotchController.h:394-406`), and `model_` is private. So there is **no route** by which anything outside the controller can tell which notch came from a soundcheck — which makes spec §4.6(b), "clear the previous run's preventive notches", impossible. Skipping that step is not an option either: `Origin::Soundcheck` notches are exempt from auto-release (`src/app/NotchController.cpp:729`, `:1322`), so the sixteen-slot chain would drain after a few soundchecks. And without `ClearReason::SoundcheckReplace` that cleanup logs `reason: manual` (`src/app/MainComponent.cpp:608` via `reasonName`) and is indistinguishable from an operator removing a notch by hand — exactly the mislabelling lane D exists to prevent.

**Files:**
- Modify: `src/app/NotchController.h` — the `ClearReason` enumerator list (anchor text: `Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind`, `src/app/NotchController.h:69`) and `struct SnapshotNotch` (anchor text: `std::uint8_t index   = 0;`, `src/app/NotchController.h:394-406`). Numbers are pre-Task-4.
- Modify: `src/app/NotchController.cpp` — the ONE aggregate initialiser that fills the snapshot list (anchor text: `notchList[notchCount++] = { (float) n.frequency, (float) n.Q,`, `src/app/NotchController.cpp:578-580`)
- Modify: `src/app/MainComponent.cpp` — `reasonName` (anchor text: `case NotchController::ClearReason::PartialApplyUnwind: return "partial_apply_unwind";`, `src/app/MainComponent.cpp:75` — m-2 corrected this from `:74`)
- Modify: `tools/logstats.py` — a `soundcheck_replace` tally and `--expect-soundcheck-replaced`
- Modify: `tests/fixtures/session-sample.jsonl` — one `notch_clear` carrying `reason: "soundcheck_replace"`
- Modify: `tests/CMakeLists.txt:113-117` — the `logstats_fixture` argument list
- Test: `tests/test_notchcontroller.cpp` (append at the end of the file, OUTSIDE any anonymous namespace — lane G m-E: a helper appended after `namespace { ... }` closes lands in global scope)
- Test: `tests/test_gui_wiring.cpp` — the `SoundcheckReplace` reason test goes **here**, not in `tests/test_sessionlogger.cpp` (B-3): `MainComponent::notchEventToVarForTest` (`src/app/MainComponent.h:194`) is used from nowhere else in the suite (`tests/test_gui_wiring.cpp:1337, 1360, 1365, 1383`), and the `makeClearEvent` helper plan rev 1 assumed **does not exist anywhere in the repo**

**Interfaces:**
- Produces:
  ```cpp
  enum class NotchController::ClearReason : std::uint8_t
  { Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind, SoundcheckReplace };

  struct NotchController::SnapshotNotch
  {
      float frequency, Q, depthDB, deepestDb;
      Origin       origin  = Origin::Detector;   // NEW
      std::uint8_t channel = 0, index = 0;
  };
  ```
  `reasonName (ClearReason::SoundcheckReplace) == "soundcheck_replace"`.
- Consumes: nothing new.

**The one thing an implementer must not get wrong here.** `SnapshotNotch` is filled by a **brace-elided aggregate initialiser** at `src/app/NotchController.cpp:578-580`. Adding a field in the middle silently re-binds every value after it. Put `origin` where the declaration above puts it — after `deepestDb`, before `channel` — and update the initialiser in the same edit:

```cpp
                    notchList[notchCount++] = { (float) n.frequency, (float) n.Q,
                                                (float) n.depthDB, (float) n.deepestDb,
                                                n.origin,
                                                (std::uint8_t) c, (std::uint8_t) i };
```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`:

```cpp
// RED IF: SnapshotNotch loses `origin`, or the aggregate initialiser at
// NotchController.cpp:578-580 is not updated alongside the struct -- brace
// elision would then silently shift channel/index by one field. Without this
// field lane M cannot find the previous run's preventive notches to clear, and
// they never auto-release (NotchController.cpp:729), so the 16-slot chain
// drains after a few soundchecks. F7.
TEST (NotchControllerSoundcheck, SnapshotCarriesOrigin)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 3, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Soundcheck));
    ASSERT_TRUE (h.controller.setNotch (0, 4, 2000.0, 30.0, -12.0,
                                        NotchController::Origin::Manual));

    // B-1(c): latest_ is written ONLY inside runOnce()'s drain loop
    // (NotchController.cpp:550-560 gathers, :568-582 builds the list, :617-650
    // publishes), and the loop body runs only when a block was actually drained
    // off the tap. Without this the snapshot stays empty forever and the whole
    // test passes vacuously. Precedent: tests/test_gui_wiring.cpp:1033-1035,
    // and memory/preset-save-roundtrip-2026-09-05.md.
    const std::vector<float> block (512, 0.0f);
    h.tap.write (block.data(), block.size());

    NotchController::SnapshotBuffer snap {};
    h.controller.runOnce();
    h.controller.copySnapshot (snap);
    ASSERT_GT (snap.notchCount, 0u) << "the snapshot never refreshed -- pump the tap";

    bool sawSoundcheck = false, sawManual = false;
    for (std::uint32_t i = 0; i < snap.notchCount; ++i)
    {
        const auto& n = snap.notches[i];
        if (n.index == 3)
        {
            EXPECT_EQ (n.origin, NotchController::Origin::Soundcheck);
            EXPECT_NEAR (n.frequency, 1000.0f, 0.5f);   // the fields after `origin` still line up
            EXPECT_EQ ((int) n.channel, 0);
            sawSoundcheck = true;
        }
        if (n.index == 4)
        {
            EXPECT_EQ (n.origin, NotchController::Origin::Manual);
            EXPECT_NEAR (n.frequency, 2000.0f, 0.5f);
            sawManual = true;
        }
    }
    EXPECT_TRUE (sawSoundcheck);
    EXPECT_TRUE (sawManual);
}

// RED IF: SoundcheckReplace is folded into Manual. The log could then not tell
// "the soundcheck replaced its own previous proposal" from "a human removed a
// notch" -- the exact mislabelling lane D exists to prevent. F21.
TEST (NotchControllerSoundcheck, SoundcheckReplaceIsItsOwnClearReason)
{
    // B-2, TWO corrections to plan rev 1, both from Recorder's own comment at
    // tests/test_notchcontroller.cpp:1466-1470:
    //   1. Recorder has NO operator(). The sink comes from rec.sink().
    //   2. The Recorder must be declared BEFORE the Harness it is wired to. The
    //      controller's destructor calls stop(), which flushes the remaining
    //      events through the sink -- into this object. Declaration order is the
    //      reverse of destruction order, so a Recorder declared after its
    //      Harness is already gone by then and the flush writes into a
    //      destroyed vector.
    Recorder rec;
    Harness  h;
    h.controller.setEventSink (rec.sink());

    ASSERT_TRUE (h.controller.setNotch (0, 2, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Soundcheck));
    h.controller.clearNotch (0, 2, NotchController::ClearReason::SoundcheckReplace);

    const std::vector<float> block (512, 0.0f);   // B-1(c)
    h.tap.write (block.data(), block.size());
    h.controller.runOnce();

    ASSERT_FALSE (rec.events.empty());
    const auto& last = rec.events.back();
    EXPECT_EQ (last.kind, NotchController::NotchEvent::Kind::Clear);
    EXPECT_EQ (last.reason, NotchController::ClearReason::SoundcheckReplace);
    EXPECT_NE (last.reason, NotchController::ClearReason::Manual);
    EXPECT_FALSE (h.controller.activeForTest (0, 2));   // B-3: `active`, never depth < 0
}
```

`Recorder` already exists in this file at `tests/test_notchcontroller.cpp:1471`, inside the anonymous namespace opened at `:1463`. **Put both new tests after that namespace closes at `:1487`** so `Recorder` and `Harness` are both visible, and so nothing is appended into a namespace that has already ended (lane G m-E).

Append to `tests/test_gui_wiring.cpp`, **beside `SetAndClearKeepTheirOwnEventNames` (`tests/test_gui_wiring.cpp:1356-1369`)** — not to `tests/test_sessionlogger.cpp`:

```cpp
// RED IF: reasonName gains no case for SoundcheckReplace. It falls through to
// "unknown" and the tester's log says nothing useful -- the same class of defect
// as lane G's retuneReasonName fallthrough (Task 9 review I2).
//
// B-3: this lives HERE because MainComponent::notchEventToVarForTest
// (MainComponent.h:194) is used from nowhere else in the suite
// (test_gui_wiring.cpp:1337, 1360, 1365, 1383), and the event is built INLINE --
// the makeClearEvent helper plan rev 1 assumed does not exist anywhere in the
// repo.
TEST (GuiWiring, SoundcheckReplaceReachesTheLogAsItsOwnReason)
{
    NotchController::NotchEvent clear;
    clear.kind   = NotchController::NotchEvent::Kind::Clear;
    clear.slot   = 0;
    clear.lane   = 0;
    clear.index  = 2;
    clear.hz     = 1000.0f;
    clear.q      = 30.0f;
    clear.depthDb = -12.0f;
    clear.origin = NotchController::Origin::Soundcheck;
    clear.reason = NotchController::ClearReason::SoundcheckReplace;

    const juce::var v = MainComponent::notchEventToVarForTest (clear);

    EXPECT_EQ (v["ev"].toString(), "notch_clear");
    EXPECT_EQ (v["reason"].toString(), "soundcheck_replace");
    EXPECT_NE (v["reason"].toString(), "manual");
    EXPECT_NE (v["reason"].toString(), "unknown");
}
```

- [ ] **Step 2: Run the two new suites and watch them fail**

```bash
cmake --build build --config Release
```
Expected: compile error — `'origin': is not a member of 'NotchController::SnapshotNotch'` and `'SoundcheckReplace': is not a member of 'NotchController::ClearReason'`.

- [ ] **Step 3: Make the two additive changes**

`src/app/NotchController.h`, the enumerator list:

```cpp
    enum class ClearReason : std::uint8_t
    {
        Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind,
        // Lane M: the active soundcheck replacing its OWN previous proposal on
        // this slot. A separate value because the alternative -- reusing Manual
        // -- makes an automatic cleanup indistinguishable from an operator
        // pulling a notch by hand, in the one file lane D built to tell them
        // apart (spec §4.6b, F21).
        SoundcheckReplace
    };
```

`src/app/NotchController.h`, `SnapshotNotch` — add after `deepestDb`:

```cpp
        // Lane M (spec §4.6, F7): which policy placed this notch. model_ is
        // private and SnapshotNotch is the only view outward, so without this
        // field nothing can find the preventive notches of a previous
        // soundcheck run in order to replace them -- and they never
        // auto-release (NotchController.cpp:729), so the chain drains.
        Origin origin = Origin::Detector;
```

`src/app/NotchController.cpp:578-580` — the aggregate initialiser, exactly as shown above.

`src/app/MainComponent.cpp`, `reasonName` — add before the closing brace of the switch:

```cpp
        case NotchController::ClearReason::SoundcheckReplace: return "soundcheck_replace";
```

- [ ] **Step 4: `tools/logstats.py` — the one branch lane M actually needs**

Spec §4.7 is explicit that the five `soundcheck_*` **`ev` names need no branch**: the `if/elif` chain at `tools/logstats.py:46-87` has no `else`, so an unknown `ev` falls through every branch and is ignored, and the comment at `:62-66` says exactly that (m-9 corrected both ranges). The real work is on the **clear reason** side.

Today a reason is only echoed into the `cleared by` column (`tools/logstats.py:132` — m-9). Add a count, so a fixture can pin it:

In `summarise()`, inside the `elif ev == "notch_clear":` branch (anchor text: `n = open_by_key.pop(key(e), None)`), nothing changes; in the returned dict (anchor text: `"retunes": sum(n["retunes"] for n in notches),`) add:

```python
        # Lane M: how many notches this session's soundchecks replaced with
        # their own next proposal. Counted here rather than left to the reason
        # column because "the soundcheck keeps replacing its own work" is a
        # pattern worth seeing at a glance, and because a fixture can then pin
        # that the reason name survives the whole C++ -> JSON -> reader path.
        "soundcheck_replaced": sum(1 for n in notches if n["reason"] == "soundcheck_replace"),
```

In `print_report()`, append to the summary line (anchor text: `f"  unjudged {s['unjudged']}`):

```python
    if s["soundcheck_replaced"]:
        print(f"soundcheck replaced {s['soundcheck_replaced']} notch(es) from an earlier run")
```

In `main()`, beside the other flags (anchor text: `ap.add_argument("--expect-retunes", type=int)`):

```python
    ap.add_argument("--expect-soundcheck-replaced", type=int)
```
and beside the other checks (anchor text: `if args.expect_retunes is not None`):

```python
    if (args.expect_soundcheck_replaced is not None
            and s["soundcheck_replaced"] != args.expect_soundcheck_replaced):
        failures.append(
            f"soundcheck-replaced {s['soundcheck_replaced']} != {args.expect_soundcheck_replaced}")
```

The file already opens with `encoding="utf-8"` (`tools/logstats.py:19` — m-9) — leave it (repo rule 6).

- [ ] **Step 5: The fixture**

`tests/fixtures/session-sample.jsonl` currently supports `--expect-notches 4 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2` (`tests/CMakeLists.txt:116-117`). Add **one** `notch_set` with `"origin": "soundcheck"` and its matching `notch_clear` with `"reason": "soundcheck_replace"`, at a `t` after the existing lines, on a `(slot, lane, index)` triple not already in the file, at a frequency **more than one bin (23.4375 Hz) away** from every existing `hz` so `--expect-recurrence-max` does not move.

**Insert the new lines BEFORE the last line.** `tests/fixtures/session-sample.jsonl` is 15 lines and line 15 is `{"ev":"session_end","t":60000.0,...}` (m-12); appending after it produces a log with events past the end of the session, which is not a shape the app can emit.

Then update `tests/CMakeLists.txt:113-117`:

```cmake
    add_test(NAME logstats_fixture
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/logstats.py
                ${CMAKE_SOURCE_DIR}/tests/fixtures/session-sample.jsonl
                --expect-notches 5 --expect-verdicts 3 --expect-false 1
                --expect-recurrence-max 2 --expect-retunes 2
                --expect-soundcheck-replaced 1)
```

`--expect-notches` goes 4 → 5 because one `notch_set` was added. **Re-derive the other four from `tools/logstats.py` before trusting that they are unchanged** — lane G's m-A is the precedent: a fixture that depicts a run the code cannot produce passes review and fails later.

Run it standalone first, before the C++ build:

```bash
python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 5 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2 --expect-soundcheck-replaced 1
```
Expected: the report prints and the exit code is 0.

- [ ] **Step 6: Grep for a duplicated literal**

The constants of Tasks 1–3 must each exist exactly once as a literal. Run:

```bash
grep -rn "0\.1f\|6000\.0\|10000\.0\|kSoundcheckMaxPeak\|kTrustedHighHz" src/ | grep -v "^src/dsp/SoundcheckSignal\|^src/dsp/LoopGainEstimator"
```
Expected: only alias lines of the form `= SoundcheckSignal::k...` / `= LoopGainEstimator::k...`. Anything else is a second literal and must become an alias (lane G m-D).

- [ ] **Step 7: Reconfigure, build, gate, commit**

Headers changed ⇒ full reconfigure.

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (577)` — 574 + 3. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.h src/app/NotchController.cpp src/app/MainComponent.cpp tools/logstats.py tests/test_notchcontroller.cpp tests/test_gui_wiring.cpp tests/fixtures/session-sample.jsonl tests/CMakeLists.txt
```
```bash
git commit -m "feat(lane-m): SnapshotNotch carries origin; ClearReason::SoundcheckReplace reaches the log"
```

---

