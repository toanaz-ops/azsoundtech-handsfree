### Task 9: the GUI — the `ĐO` button, the progress overlay, the results strip, the margin curve

**Mức level dự kiến:** **0 dB.** No audio path touched. But this task is the only thing that lets an operator STOP a run, so `DỪNG` is a safety control, not a convenience.

**Ends with a rendered picture that has been read back** (CLAUDE.md standing instruction, 2026-08-25). Every visual defect found during the console rebuild — a button with no text, a truncated CLEAR ALL, markers burying the trace, a mojibake middle dot — **passed the whole test suite**. A green build says nothing about whether the thing is legible.

**What goes where, and why not all of it in `SpectrumView`:**

- `gui::ModeRail` gains `measureButton { "ĐO" }` and `onMeasure`. Q16 chose a **separate** button: "wait 15 s for the room to howl" and "play a signal into the PA for 72 s" are different enough that one button meaning both, on live-sound equipment, is a hazard. The two real labels today are `soundcheckButton { "SOUNDCHECK" }` (`src/gui/ModeRail.h:81`, visible) and `gui::ModeBar::soundcheckButton { "Run Soundcheck (15s)" }` (`src/gui/ModeBar.h:43`, hidden — `modeBar_` is not made visible, `src/app/MainComponent.cpp:211`). **There is no "sweep the room" placeholder anywhere in this repo**; the original lane M prompt was wrong about that.
- `gui::SoundcheckPanel` is a **new** component holding the interactive chrome: the progress overlay and the results strip. `SpectrumView.h` is already ~500 lines with its own toolbar, hysteresis chip and age ledger; adding two modal-ish states to it would make a big file bigger for no reuse.
- `gui::SpectrumView` gains only a data overlay: the margin curve, the markers and the dimmed 6–10 kHz band.

**The countdown must be lane M's own** (F12). `getSoundcheckRemainingMs` (`src/app/MainComponent.cpp:247-251` → `src/app/NotchController.cpp:1016-1023`) reports the **passive** 15 s window and measures it in `liveMs_` — which is frozen because the taps are suspended. Using it would show a number that stands still or reads 0. `ModeRail::countdownLabel` is not touched.

**The overlay draws lane M's own data, never `copySnapshot()`.** The publish happens inside the drain loop (`src/app/NotchController.cpp:568-582` builds the list, `:617-650` publishes), so with the taps suspended nothing is drained and `copySnapshot()` returns a **frozen** frame for the whole run.

**Controls locked while `state != Idle`** (F10): `SOUNDCHECK`, `AUTO`, `BYPASS`, `CLEAR ALL` (`src/app/MainComponent.cpp:241-245`), `PRESET LOAD` and `PRESET SAVE` (the chooser lambdas at `src/app/MainComponent.cpp:258-290`), and every slot enable / width / routing control. **`PRESET LOAD` is locked in `Results` as well**, because `adoptPreset` uses the FILE's indices and overwrites without checking `n.active` (`src/app/NotchController.cpp:487-506` → `setNotchImpl` `:226-245`) — it would erase the pending proposals. A device restart **cannot** be locked out (it can happen by itself), so it takes the abort path instead (Task 10).

**Files:**
- Modify: `src/gui/ModeRail.h` (anchor: `juce::TextButton soundcheckButton { "SOUNDCHECK" };`, `:81`), `src/gui/ModeRail.cpp` (`resized()` and the button wiring)
- Create: `src/gui/SoundcheckPanel.h`, `src/gui/SoundcheckPanel.cpp`
- Modify: `src/gui/SpectrumView.h` (anchor: `void setDisplayLane (int lane);`, `:202`), `src/gui/SpectrumView.cpp` (`paint`)
- Modify: `CMakeLists.txt` (`HANDSFREE_CORE_SOURCES`), `tools/snapshot.cpp`
- Test: `tests/test_moderail.cpp`, `tests/test_spectrumview.cpp`, and a new block in `tests/test_gui_wiring.cpp`

**Interfaces:**
- Consumes: `SoundcheckController::State` / `OutputResult` / `getCurrentTargetIndex` / `getTargetCount` / `getRemainingMsInRun` / `copyResults` (Task 6). The overlay is fed `OutputResult::marginDb`, `::trusted` and **`::marked`** — the per-bin array I-3 added, without which the view has the marker COUNT but no way to know which bins they are.
- Produces:
  ```cpp
  // gui::ModeRail
  juce::TextButton measureButton { "ĐO" };
  std::function<void()> onMeasure;
  void setMeasureEnabled (bool enabled);

  // gui::SoundcheckPanel : public juce::Component
  enum class Mode { Hidden, Running, Results };
  void setMode (Mode mode);
  void setProgress (int channelIndex, int channelCount, double remainingMs);
  void setResultsSummary (int hotSpots, int saturatedBins, int unmeasured, int routingInvalid);
  std::function<void()> onStop, onApply, onDismiss;
  bool keyPressed (const juce::KeyPress& key) override;   // Esc -> onStop
  juce::TextButton stopButton { "DỪNG" }, applyButton { "ÁP DỤNG" }, dismissButton { "BỎ" };

  // gui::SpectrumView
  void setSoundcheckOverlay (const float* marginDb, const bool* marked, const bool* trusted,
                             int numBins, double sampleRate);
  void clearSoundcheckOverlay();
  [[nodiscard]] bool hasSoundcheckOverlay() const;
  static constexpr float kLowConfidenceAboveHz = LoopGainEstimator::kTrustedHighHz;
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_moderail.cpp`:

```cpp
// B-9 (all three ModeRail tests): the enumerator is `Vertical`, not `vertical`
// (src/gui/ModeRail.h:23), and the existing tests construct with parentheses
// (tests/test_moderail.cpp:28, 41). Rev 1 would not have compiled.
//
// RED IF: the button is constructed as TextButton(name, tooltip). In JUCE 9 the
// SECOND argument of that two-argument constructor is NOT the tooltip, so the
// button renders with NO TEXT AT ALL -- and the build stays green. This is
// memory/juce9-api-traps-2026-08-25.md, and it shipped once already.
TEST (ModeRail, MeasureButtonHasItsLabel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail (gui::ModeRail::Orientation::Vertical);

    EXPECT_EQ (rail.measureButton.getButtonText(), juce::String::fromUTF8 ("ĐO"));
    EXPECT_NE (rail.measureButton.getButtonText(), rail.soundcheckButton.getButtonText());
}

// RED IF: the ĐO button is folded into SOUNDCHECK. Q16 option 1: one button that
// can either listen for 15 s or play a signal into a PA for 72 s is a hazard on
// live-sound equipment.
TEST (ModeRail, MeasureIsItsOwnButtonAndItsOwnCallback)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail (gui::ModeRail::Orientation::Vertical);

    int measures = 0, soundchecks = 0;
    rail.onMeasure    = [&measures]    { ++measures; };
    rail.onSoundcheck = [&soundchecks] { ++soundchecks; };

    // onClick(), not triggerClick(): triggerClick is async and the headless
    // suite pumps no message loop (memory/data-loop-lessons-2026-09-05.md).
    rail.measureButton.onClick();
    EXPECT_EQ (measures, 1);
    EXPECT_EQ (soundchecks, 0);
}

// RED IF: the rail lays the new button off the bottom of its own bounds, which
// is invisible in every test and obvious in a render.
TEST (ModeRail, MeasureButtonIsInsideTheRail)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::ModeRail rail (gui::ModeRail::Orientation::Vertical);
    rail.setSize (140, 620);
    rail.resized();   // JUCE headless setSize() has no peer, so resized() must be
                      // called by hand (memory/gui-console-lessons-2026-08-24.md)

    EXPECT_TRUE (rail.getLocalBounds().contains (rail.measureButton.getBounds()));
    EXPECT_GT (rail.measureButton.getWidth(), 0);
    EXPECT_GT (rail.measureButton.getHeight(), 0);
    EXPECT_FALSE (rail.measureButton.getBounds().intersects (rail.soundcheckButton.getBounds()));
}
```

Create the `SoundcheckPanel` tests in `tests/test_gui_wiring.cpp` (it already carries the console-level wiring tests; `tests/test_gui_wiring.cpp:1016-1041` is the shape to follow — `juce::ScopedJuceInitialiser_GUI` first, real objects, explicit `resized()`):

```cpp
// RED IF: DỪNG stops being wired, or the panel is left non-focusable so Esc can
// never arrive. DỪNG is the OFFICIAL stop path; Esc is best-effort only, because
// a key press only reaches the component that currently has focus and the preset
// name field also takes keys (MainComponent.cpp:255-290). F14.
TEST (SoundcheckPanel, StopIsTheOfficialPathAndEscIsTheSecondOne)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::SoundcheckPanel panel;
    panel.setSize (900, 120);
    panel.resized();

    int stops = 0;
    panel.onStop = [&stops] { ++stops; };

    panel.setMode (gui::SoundcheckPanel::Mode::Running);
    EXPECT_TRUE (panel.getWantsKeyboardFocus());

    panel.stopButton.onClick();
    EXPECT_EQ (stops, 1);

    EXPECT_TRUE (panel.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
    EXPECT_EQ (stops, 2);
}

// RED IF: the three buttons carry no text (the JUCE 9 two-argument trap again),
// or ÁP DỤNG is shown while a run is still going.
TEST (SoundcheckPanel, ModesShowTheRightControlsWithRealLabels)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::SoundcheckPanel panel;
    panel.setSize (900, 120);

    EXPECT_EQ (panel.stopButton.getButtonText(),    juce::String::fromUTF8 ("DỪNG"));
    EXPECT_EQ (panel.applyButton.getButtonText(),   juce::String::fromUTF8 ("ÁP DỤNG"));
    EXPECT_EQ (panel.dismissButton.getButtonText(), juce::String::fromUTF8 ("BỎ"));

    panel.setMode (gui::SoundcheckPanel::Mode::Running);
    panel.resized();
    EXPECT_TRUE  (panel.stopButton.isVisible());
    EXPECT_FALSE (panel.applyButton.isVisible());

    panel.setMode (gui::SoundcheckPanel::Mode::Results);
    panel.resized();
    EXPECT_FALSE (panel.stopButton.isVisible());
    EXPECT_TRUE  (panel.applyButton.isVisible());
    EXPECT_TRUE  (panel.dismissButton.isVisible());

    panel.setMode (gui::SoundcheckPanel::Mode::Hidden);
    EXPECT_FALSE (panel.isVisible());
}

// RED IF: an unmeasured channel or a mis-routed one is folded into the headline
// count. They are DIFFERENT sentences for the operator: "could not measure" and
// "the routing is wrong" ask for different actions (F26).
TEST (SoundcheckPanel, UnmeasuredAndMisroutedAreSeparateSentences)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::SoundcheckPanel panel;
    panel.setSize (900, 120);
    panel.setMode (gui::SoundcheckPanel::Mode::Results);
    panel.setResultsSummary (/*hotSpots*/ 3, /*saturated*/ 1,
                             /*unmeasured*/ 2, /*routingInvalid*/ 1);
    panel.resized();

    const juce::String text = panel.summaryTextForTest();
    EXPECT_TRUE (text.contains ("3"));
    EXPECT_TRUE (text.containsIgnoreCase (juce::String::fromUTF8 ("không đo được")));
    EXPECT_TRUE (text.containsIgnoreCase (juce::String::fromUTF8 ("định tuyến")));
}
```

Append to `tests/test_spectrumview.cpp`:

```cpp
// RED IF: the overlay is fed from copySnapshot(). The publish sits inside the
// drain loop (NotchController.cpp:617-650), so with the taps suspended the
// snapshot is FROZEN for the whole run and the overlay would show the frame from
// before the run started. §4.1.
TEST (SpectrumView, SoundcheckOverlayCarriesLaneMData)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> cmds { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tap, cmds, clock };

    gui::SpectrumView view { controller };
    view.setSize (900, 420);
    view.resized();

    EXPECT_FALSE (view.hasSoundcheckOverlay());

    std::vector<float> margin (Detector::kNumBins, 12.0f);
    std::vector<char>  marked (Detector::kNumBins, 0);
    std::vector<char>  trusted (Detector::kNumBins, 1);
    marked[300] = 1;

    view.setSoundcheckOverlay (margin.data(),
                               reinterpret_cast<const bool*> (marked.data()),
                               reinterpret_cast<const bool*> (trusted.data()),
                               Detector::kNumBins, 48000.0);
    EXPECT_TRUE (view.hasSoundcheckOverlay());

    view.clearSoundcheckOverlay();
    EXPECT_FALSE (view.hasSoundcheckOverlay());
}

// RED IF: paint() starts allocating when the overlay is on. The whole view is
// built on "paint allocates nothing" (SpectrumView.h:10, :119-138) and the
// overlay must not be the exception.
TEST (SpectrumView, SoundcheckOverlayPaintsWithoutAllocating)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> cmds { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tap, cmds, clock };

    gui::SpectrumView view { controller };
    view.setSize (900, 420);
    view.resized();

    std::vector<float> margin (Detector::kNumBins, 3.0f);
    std::vector<char>  marked (Detector::kNumBins, 0);
    std::vector<char>  trusted (Detector::kNumBins, 1);
    for (int k = 100; k < 700; k += 37) marked[(std::size_t) k] = 1;

    view.setSoundcheckOverlay (margin.data(),
                               reinterpret_cast<const bool*> (marked.data()),
                               reinterpret_cast<const bool*> (trusted.data()),
                               Detector::kNumBins, 48000.0);

    juce::Image img { juce::Image::ARGB, 900, 420, true };
    { juce::Graphics g { img }; view.paint (g); }

    const auto elementsAfterFirst = view.soundcheckOverlayPathElementCountForTest();
    const auto pointCapacity      = view.spectrumPointCapacityForTest();

    for (int i = 0; i < 100; ++i) { juce::Graphics g { img }; view.paint (g); }

    EXPECT_EQ (view.soundcheckOverlayPathElementCountForTest(), elementsAfterFirst);
    EXPECT_EQ (view.spectrumPointCapacityForTest(), pointCapacity);
}
```

`spectrumPointCapacityForTest` already exists (`src/gui/SpectrumView.h:123`).
`soundcheckOverlayPathElementCountForTest` is **new**, and it is modelled on
`dashedStemPathElementCountForTest` (`src/gui/SpectrumView.h:145` — m-10 corrected this from `:144`), including that
accessor's honest caveat: `juce::Path` exposes no capacity getter, so the element
count walked with `Path::Iterator` is the closest available proxy for "did not
grow". Reserve the overlay path in the constructor with `preallocateSpace`, and
remember that `preallocateSpace` counts **floats, not elements** — roughly three
per `lineTo` (`memory/stereo-lane-lessons-2026-09-05.md`, and the derivation
already written above `kDashedStemReserveFloats` at `src/gui/SpectrumView.h:185`).

- [ ] **Step 2: Run and watch them fail**

```bash
cmake --build build --config Release
```

- [ ] **Step 3: `gui::ModeRail`**

Declare `measureButton` **immediately after** `soundcheckButton` (`src/gui/ModeRail.h:81`) and `onMeasure` beside `onSoundcheck`. Construct it with the **single-argument** `juce::TextButton` constructor:

```cpp
    // ONE argument. juce::TextButton(name, tooltip) changed the meaning of its
    // second parameter in JUCE 9, so the two-argument form renders a button with
    // NO TEXT while the build stays green
    // (memory/juce9-api-traps-2026-08-25.md). Set a tooltip with
    // setTooltip() if one is wanted.
    juce::TextButton measureButton { juce::String::fromUTF8 ("ĐO") };
```

In `resized()`, give it its own row/column in the same flow as the other three, sized so `ĐO` is not truncated at the rail's narrowest layout. **Measure the text with the real font and the widest string the formatter can print**, not with an estimate (`memory/stereo-lane-lessons-2026-09-05.md`).

- [ ] **Step 4: `gui::SoundcheckPanel`**

A plain `juce::Component` holding the three buttons, a progress line and a summary `juce::Label`. `setMode` sets visibility and calls `setWantsKeyboardFocus (mode == Mode::Running)` plus `grabKeyboardFocus()`. `keyPressed` returns `true` and calls `onStop` for `escapeKey`, `false` for everything else — so a key the panel does not own still reaches whatever else wants it.

The progress line reads `ĐANG ĐO · kênh 2/4` plus lane M's own countdown, from `setProgress`. Do not concatenate the caption and the number into one `Label` — the number is read across a room and wants to be large, the caption only has to be findable, and one `Label` cannot be two sizes (the reasoning already written at `src/gui/ModeRail.h:66-72`).

The results strip reads `tìm thấy N điểm dễ hú`, and when `saturatedBins > 0` adds *"còn vượt X dB sau khi cắt sâu nhất — chỉnh gain, hạ trần, hoặc đổi vị trí mic"*. `unmeasured` and `routingInvalid` get their **own two sentences** — never a flat 0 dB line, which reads as "an excellent room".

Colours come from `src/gui/theme/`; **no hex literal may appear outside that directory** — `tests/test_aztheme.cpp` greps `src/gui` as text at run time and will fail otherwise (`tests/CMakeLists.txt:85-87`).

- [ ] **Step 5: `gui::SpectrumView` overlay**

Add the two setters plus private storage — `std::array<float, Detector::kNumBins>` for `marginDb`, `std::array<bool, Detector::kNumBins>` for `marked` and for `trusted`, a `bool hasOverlay_`, and one reused `juce::Path` following the existing `markerPath_` pattern (`src/gui/SpectrumView.h:439`). In `paint`, after the spectrum polyline and before the notch stems:

1. the margin curve, as a polyline on the same log-frequency axis;
2. a marker at every `marked[k]`;
3. a dimmed rectangle over `[kLowConfidenceAboveHz, kMaxHz]` with the label **"độ tin cậy thấp"**.

**Markers must not bury the trace** — that exact defect was found by a render during the console rebuild (`memory/ui-rebuild-sodium-rack-2026-08-25.md`). Numbers appear on hover only.

Do **not** interpolate between the amber and cyan theme colours for the margin curve: the midpoints come out mud (same memory note).

- [ ] **Step 6: The snapshot scene**

`tools/snapshot.cpp` has **no scene registry** (m-11): `main()` (`tools/snapshot.cpp:156`) is a linear sequence of `shoot()` calls — `:212` renders `console-idle.png`, `:357` renders `console-live.png` and `:395` renders `console-preset-music.png`. Lane M adds **a fourth `shoot()` block** in the same shape. (The cross-check called it "the third"; there are three today, so this is the fourth — the substance of m-11, that there is no registry to register with, is what matters and is correct.)

The new block builds a `MainComponent`, pushes a synthetic `OutputResult` — a handful of marked bins, one saturated candidate, one "không đo được" channel — into the panel and the overlay, and renders `console-soundcheck-results.png`. Then:

```bash
cmake --build build --config Release
```
```bash
build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast
```

**READ the images back** — `shots/console-soundcheck-results.png` and `shots/console-live.png` — and check, by eye:

- `ĐO` is present, legible, and not truncated;
- the results strip's Vietnamese renders (no mojibake — the middle-dot bug is the precedent);
- the margin curve is distinguishable from the spectrum trace, and the markers do not bury it;
- the 6–10 kHz band is visibly dimmed and its label is readable;
- `ÁP DỤNG` and `BỎ` both fit their buttons.

Send both images to the owner. **GUI-visible behaviour is not reported without a picture** (CLAUDE.md).

- [ ] **Step 7: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (633)` — 625 + **8**. ESTIMATE (m-15 recounted this; rev 1 said 9).

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/gui/ModeRail.h src/gui/ModeRail.cpp src/gui/SoundcheckPanel.h src/gui/SoundcheckPanel.cpp src/gui/SpectrumView.h src/gui/SpectrumView.cpp
```
```bash
git add tools/snapshot.cpp CMakeLists.txt tests/test_moderail.cpp tests/test_spectrumview.cpp tests/test_gui_wiring.cpp
```
```bash
git commit -m "feat(lane-m): DO button, progress overlay with DUNG, results strip, margin overlay on the analyser"
```

---

