### Task 8: the five `soundcheck_*` log events

**Mức level dự kiến:** **0 dB.** Log only.

**The dispatch key is `ev`, not `kind`** (lane G B-3). `tools/logstats.py` reads `e.get("ev")` (`tools/logstats.py:45`) and its `if/elif` chain has no `else` (`:45-83`), with a comment at `:66-71` saying an unknown `ev` falls through every branch. So — **contrary to spec rev 1, and as spec §4.7 corrects** — the five new `ev` names need **no logstats branch at all**, and a test asserts that they are ignored rather than miscounted. The only real logstats work was the clear reason, done in Task 4.

| `ev` | When | Fields |
|---|---|---|
| `soundcheck_start` | entering `Arm` | `outputs`, `peak_dbfs`, `sweep_ms`, `total_ms`, `mode_before`, `ring_risk` (a number when `ringRiskValid`, **null** otherwise) |
| `soundcheck_output` | end of `Analyse` | `slot`, `lane`, `out_ch`, `in_ch`, `ok`, `routing_invalid`, `snr_db`, `saturated`, `candidates[]` each with `hz`, `margin_db`, `depth_db`, `residual_db` |
| `soundcheck_result` | entering `Results` | `outputs_ok`, `outputs_failed`, `marked_total`, `candidates_total` |
| `soundcheck_apply` | after placing | `placed`, `refused`, `cleared_previous` |
| `soundcheck_abort` | entering `Abort` | `reason` (one of `user_stop` / `esc` / `engine_stopped` / `device_error` / `device_changed` / `mic_hot` / `room_ringing` / `capture_drop`), `at_output`, `elapsed_ms` |

**Every real number is rounded to 3 significant figures before it enters a `juce::var`** (`memory/data-loop-lessons-2026-09-05.md`: `juce::JSON` prints a double to 18 digits otherwise, and a log line nobody can read is a log line nobody reads).

**Files:**
- Modify: `src/app/SoundcheckController.cpp` — the five `logEvent (...)` call sites, each building its `juce::var` with `SessionLogger::makeEvent` (`src/app/SessionLogger.h:50`)
- Modify: `src/app/MainComponent.cpp` — hand `sessionLogger_.log` to `soundcheck_.logEvent` (Task 10 does the wiring; this task only proves the shapes)
- Test: `tests/test_sessionlogger.cpp` (append)
- Modify: `tests/fixtures/session-sample.jsonl` — five `soundcheck_*` lines
- Modify: `tests/CMakeLists.txt` — no new flags beyond Task 4's

**Interfaces:**
- Consumes: `SessionLogger::makeEvent (const juce::String&)` (`src/app/SessionLogger.h:50`, defined `.cpp:25`), `SoundcheckController::logEvent` (Task 6).
- Produces: a file-local helper in `SoundcheckController.cpp`'s anonymous namespace:
  ```cpp
  double round3sf (double v);              // 3 significant figures, 0 stays 0
  const char* abortReasonName (SoundcheckController::AbortReason r);
  ```
  `abortReasonName` gets a `default`-less switch plus a trailing `return "unknown";`, exactly like `originName` / `reasonName` / `retuneReasonName` (`src/app/MainComponent.cpp:54`, `:66`, `:84`) — an enumerator added without a name here must read as `unknown`, never masquerade as another reason (lane G Task 9 review I2).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_sessionlogger.cpp`:

```cpp
// RED IF: a soundcheck event is written under an `ev` name logstats DOES branch
// on (notch_set / notch_clear / notch_retune / verdict). That closes or mutates
// a notch record that has nothing to do with the soundcheck -- lane G's B-3
// defect, from the other direction.
TEST (SessionLoggerSoundcheck, TheFiveEventNamesAreTheirOwn)
{
    for (const char* name : { "soundcheck_start", "soundcheck_output", "soundcheck_result",
                              "soundcheck_apply", "soundcheck_abort" })
    {
        const auto v = SessionLogger::makeEvent (name);
        ASSERT_NE (v.getDynamicObject(), nullptr) << name;
        EXPECT_EQ (v.getDynamicObject()->getProperty ("ev").toString(), name);

        for (const char* reserved : { "notch_set", "notch_clear", "notch_retune", "verdict" })
            EXPECT_NE (juce::String (name), juce::String (reserved));
    }
}

// RED IF: doubles reach juce::var unrounded. juce::JSON prints 18 digits and the
// log stops being readable (memory/data-loop-lessons-2026-09-05.md).
//
// I-8: the rounder is a STATIC MEMBER of SoundcheckController forwarding to the
// file-local round3sf, and is called qualified, so the test rounds with exactly
// the function the writer uses. A free shim declared "somewhere" was rev 1's
// version and had no home.
TEST (SessionLoggerSoundcheck, RealNumbersAreRoundedToThreeSignificantFigures)
{
    auto v = SessionLogger::makeEvent ("soundcheck_output");
    auto* o = v.getDynamicObject();
    o->setProperty ("snr_db",
        SoundcheckController::roundToThreeSignificantFiguresForTest (18.4732918273));

    const juce::String json = juce::JSON::toString (v);
    EXPECT_TRUE (json.contains ("18.5")) << json;
    EXPECT_FALSE (json.contains ("18.4732")) << json;
}

// RED IF: an abort reason loses its name and falls through to another reason's
// string. Same fallthrough discipline as originName/reasonName/retuneReasonName
// (MainComponent.cpp:54, :66, :84).
//
// I-7: rev 1 built a std::set of eight string LITERALS and asserted its size --
// which proves that eight literals differ, and says nothing about the mapper.
// This loops the ENUMERATORS through the real function.
TEST (SessionLoggerSoundcheck, EveryAbortReasonHasItsOwnName)
{
    using R = SoundcheckController::AbortReason;
    const R all[] = { R::UserStop, R::Esc, R::EngineStopped, R::DeviceError,
                      R::DeviceChanged, R::MicHot, R::RoomRinging, R::CaptureDrop };

    std::set<juce::String> seen;
    for (R r : all)
    {
        const juce::String name = SoundcheckController::abortReasonNameForTest (r);
        EXPECT_NE (name, "unknown") << "an enumerator has no case in abortReasonName";
        EXPECT_TRUE (name.isNotEmpty());
        seen.insert (name);
    }
    EXPECT_EQ (seen.size(), 8u) << "two reasons share a name";
}
```

`RingRiskIsNullWhenInvalid` from rev 1 is **deleted** (I-7): it set a property and asserted `juce::JSON` prints `null` for a void `var` — a test of JUCE, not of lane M. `RefusesWhenRingRiskIsRising` (Task 6) already asserts that `soundcheck_start` carries `ring_risk` as void when the score is not valid, which is the behaviour that matters.

Includes for this file: `<set>` and `<algorithm>` (I-7), plus `app/SoundcheckController.h` (I-8).

Append to `tests/test_soundcheckcontroller.cpp`:

```cpp
// RED IF: a run stops emitting one of the five events, or emits them out of
// order. The order is what makes the log readable: start, one output per
// channel, one result.
TEST (SoundcheckController, AFullRunEmitsTheFiveEventsInOrder)
{
    Rig r;                                 // B-4: the ctor already runs the engine
    r.micSource = whiteNoise (16384, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (12000.0);
    ASSERT_EQ (r.sc.getState(), SoundcheckController::State::Results);

    std::vector<juce::String> names;
    for (const auto& v : r.log) names.push_back (evOf (v));

    ASSERT_GE (names.size(), 4u);
    EXPECT_EQ (names.front(), "soundcheck_start");
    EXPECT_EQ (names.back(),  "soundcheck_result");
    EXPECT_EQ (std::count (names.begin(), names.end(), juce::String ("soundcheck_output")), 2);
    EXPECT_EQ (std::count (names.begin(), names.end(), juce::String ("soundcheck_abort")), 0);
}

// RED IF: soundcheck_abort stops carrying WHERE and WHEN it stopped. Both fields
// are required by spec §4.7 and neither was asserted anywhere in rev 1; without
// them a tester's "it cut out" report cannot be tied to a channel or a moment.
TEST (SoundcheckController, AbortEventCarriesAtOutputAndElapsed)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));

    r.pump (6000.0);                       // into the SECOND channel (4.5 s each)
    r.sc.requestStop (SoundcheckController::AbortReason::UserStop);
    r.pump (100.0);

    ASSERT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    for (const auto& v : r.log)
        if (evOf (v) == "soundcheck_abort")
        {
            auto* o = v.getDynamicObject();
            EXPECT_EQ ((int) o->getProperty ("at_output"), 1) << "second channel is index 1";
            EXPECT_GT ((double) o->getProperty ("elapsed_ms"), 5000.0);
            EXPECT_EQ (o->getProperty ("reason").toString(), "user_stop");
        }
}

// RED IF: soundcheck_start stops carrying the numbers that let a later reader
// reconstruct what the room was told to do.
TEST (SoundcheckController, StartCarriesTheLevelAndTheDuration)
{
    Rig r;                                 // B-4: the ctor already runs the engine
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (20.0);

    ASSERT_FALSE (r.log.empty());
    auto* o = r.log.front().getDynamicObject();
    ASSERT_NE (o, nullptr);
    EXPECT_EQ ((int) o->getProperty ("outputs"), 2);
    EXPECT_NEAR ((double) o->getProperty ("peak_dbfs"), -20.0, 0.1);
    EXPECT_NEAR ((double) o->getProperty ("sweep_ms"), 3000.0, 1.0);
    // 2 channels x 4.5 s
    EXPECT_NEAR ((double) o->getProperty ("total_ms"), 9000.0, 50.0);
}
```

- [ ] **Step 2: Implement, then run**

```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R "SessionLogger|SoundcheckController" --output-on-failure
```

- [ ] **Step 3: The fixture and the "no branch needed" proof**

Add the five `soundcheck_*` lines to `tests/fixtures/session-sample.jsonl` (after Task 4's two lines). They must change **none** of the `--expect-*` totals — that is the whole point: an `ev` logstats does not know about is ignored, not miscounted. Confirm by running the same command Task 4 ran, unchanged:

```bash
python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 5 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2 --expect-soundcheck-replaced 1
```
Expected: exit 0, with the same totals as before the five lines were added. **If any total moves, an `ev` name has collided with a branch — fix the name, not the expectation.**

- [ ] **Step 4: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (625)` — 619 + 6. ESTIMATE. (`RingRiskIsNullWhenInvalid` is deleted per I-7 and `AbortEventCarriesAtOutputAndElapsed` takes its place, so the count is unchanged and the coverage is better.)

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/SoundcheckController.h src/app/SoundcheckController.cpp tests/test_sessionlogger.cpp tests/test_soundcheckcontroller.cpp tests/fixtures/session-sample.jsonl
```
```bash
git commit -m "feat(lane-m): five soundcheck_* session events, with ev as the dispatch key"
```

---

