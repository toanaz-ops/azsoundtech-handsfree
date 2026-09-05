### Task 5: `NotchListPanel` GOOD / FALSE buttons

**Files:**
- Modify: `src/gui/NotchListPanel.h` (constants, `RowText`, callback, accessors, private state), `src/gui/NotchListPanel.cpp` (refresh, resized, paint, statusWidthFor, setDisplayedSlot, setController)
- Test: `tests/test_notchlistpanel.cpp` (append + the 360 → 440 change at lines 388 and 421)

**Interfaces:**
- Produces:
  ```cpp
  enum class Verdict : std::uint8_t { None, Good, False };
  // slot = the slot named by setDisplayedSlot(); lane = SnapshotNotch::channel.
  std::function<void (int slot, int lane, int index, float hz, bool good, double ageMs)> onVerdict;
  static constexpr float kColVerdictW     = 92.0f;
  static constexpr int   kVerdictButtonW  = 40;
  static constexpr int   kVerdictButtonH  = 20;
  static constexpr int   kVerdictGap      = 4;
  // TEST ACCESSORS
  juce::TextButton* goodButtonForTest (int row);
  juce::TextButton* falseButtonForTest (int row);
  Verdict verdictForTest (int row) const;
  int     displayedSlotForTest() const;
  ```
  `RowText` gains `std::uint64_t key = 0; Verdict verdict = Verdict::None; juce::String verdictText;`.
  `statusWidthFor (w) = max (0, w − 2·kLeftPad − (kColIdW + kColLaneW + kColFreqW + kColDepthW + kColQW + kColVerdictW))`.
  Column order: `# | LANE | FREQ | DEPTH | Q | HELD | VERDICT`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_notchlistpanel.cpp`:

```cpp
//==============================================================================
// Lane D (data loop): the VERDICT column.

// Spec test 12. Red if FALSE stops reporting good=false, GOOD stops reporting
// good=true, or the row stops switching from buttons to a verdict word.
TEST (NotchListPanelVerdict, ClickingFalseOrGoodReportsTheVerdictAndTheRowShowsIt)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TwoNotchController fed;
    FakeClock clock;
    gui::NotchListPanel panel (fed.controller, clock);
    panel.setDisplayedSlot (3);
    panel.setSize (640, 200);
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 2);

    struct Seen { int slot, lane, index; float hz; bool good; double ageMs; };
    std::vector<Seen> seen;
    panel.onVerdict = [&] (int slot, int lane, int index, float hz, bool good, double ageMs)
    {
        seen.push_back ({ slot, lane, index, hz, good, ageMs });
    };

    ASSERT_NE (panel.falseButtonForTest (0), nullptr);
    panel.falseButtonForTest (0)->onClick();
    ASSERT_EQ (seen.size(), 1u);
    EXPECT_EQ (seen[0].slot, 3);
    EXPECT_EQ (seen[0].lane, 0); EXPECT_EQ (seen[0].index, 0);
    EXPECT_NEAR (seen[0].hz, 987.0f, 0.5f);
    EXPECT_FALSE (seen[0].good);
    EXPECT_EQ (panel.verdictForTest (0), gui::NotchListPanel::Verdict::False);
    EXPECT_EQ (panel.rowForTest (0).verdictText, "FALSE");
    EXPECT_FALSE (panel.falseButtonForTest (0)->isVisible());
    EXPECT_FALSE (panel.goodButtonForTest (0)->isVisible());

    panel.goodButtonForTest (1)->onClick();
    ASSERT_EQ (seen.size(), 2u);
    EXPECT_EQ (seen[1].lane, 1); EXPECT_EQ (seen[1].index, 3);
    EXPECT_TRUE (seen[1].good);
    EXPECT_EQ (panel.verdictForTest (1), gui::NotchListPanel::Verdict::Good);

    // GOOD does not touch the notch: the panel is display-only, the row stays.
    panel.refreshFromSnapshot();
    EXPECT_EQ (panel.rowCountForTest(), 2);
    EXPECT_EQ (panel.verdictForTest (1), gui::NotchListPanel::Verdict::Good);
    paintHeadless (panel, 640, 200);
}

// Spec test 13. Red if refreshFromSnapshot() rebuilds the buttons (a click
// mid-rebuild would be swallowed), or if an identity that leaves tracking
// keeps its buttons alive.
TEST (NotchListPanelVerdict, ButtonsPersistAcrossRefreshesAndDieWithTheirIdentity)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    TwoNotchController fed;
    FakeClock clock;
    gui::NotchListPanel panel (fed.controller, clock);
    panel.setSize (640, 200);
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 2);

    auto* good0  = panel.goodButtonForTest (0);
    auto* false0 = panel.falseButtonForTest (0);
    ASSERT_NE (good0, nullptr);
    for (int i = 0; i < 10; ++i)
    {
        clock.nowMs += 250.0;
        panel.refreshFromSnapshot();
    }
    EXPECT_EQ (panel.goodButtonForTest (0), good0);
    EXPECT_EQ (panel.falseButtonForTest (0), false0);
    EXPECT_EQ (panel.getNumChildComponents(), 4);   // two rows x two buttons

    // Clear notch 0 in the model, republish, and age the identity past the
    // tracking timeout: its buttons must be gone.
    fed.controller.clearNotch (0, 0);
    fed.republish();
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 1);
    clock.nowMs += gui::NotchListPanel::kTrackingTimeoutMs + 1000.0;
    panel.refreshFromSnapshot();
    EXPECT_EQ (panel.getNumChildComponents(), 2);
    EXPECT_NE (panel.goodButtonForTest (0), good0);   // the surviving row is the other identity
}

// Column budget: the VERDICT column fits two buttons and its own caption.
// Red if kColVerdictW shrinks below the buttons, or the caption ellipsises.
TEST (NotchListPanelVerdict, VerdictColumnFitsItsButtonsAndCaption)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    using P = gui::NotchListPanel;
    EXPECT_GE (P::kColVerdictW, (float) (2 * P::kVerdictButtonW + P::kVerdictGap + 4));
    const auto headerFont = az::theme::legendFont (az::theme::columnFontSize, true, az::theme::trackingColumn);
    EXPECT_LE (juce::GlyphArrangement::getStringWidth (headerFont, "VERDICT"), P::kColVerdictW);
    const auto buttonFont = az::theme::monoFont (az::theme::chipFontSize);
    EXPECT_LE (juce::GlyphArrangement::getStringWidth (buttonFont, "FALSE") + 6.0f, (float) P::kVerdictButtonW);
}
```

Then in `EveryColumnFitsItsWidestCellAtTheNarrowestPanel` change both `360.0f` (lines 388 and 421) to `440.0f` and update the comment on the STATUS block: "440 px is below the narrowest width the app ever gives this panel — `kNotchColumnFraction` (0.44) of `kMinimumWidth − 2·kEdgePad` = 517 px — and the smallest frame at which the fixed columns plus a two-button VERDICT cell leave HELD its widest cell."

Note `TwoNotchController::republish()` exists at `tests/test_notchlistpanel.cpp:~72`; `FakeClock` at `:38`. If `az::theme::chipFontSize` is not reachable from the test, include `gui/theme/AzTheme.h` (already included at line 27).

- [ ] **Step 2: Run to verify failure** — build; expected compile errors (`Verdict`, `onVerdict`, `falseButtonForTest`).

- [ ] **Step 3: Implement the header** — add to `NotchListPanel.h` public section (after `setSlotTabs`):

```cpp
    // Lane D (data loop): the operator's verdict on a row. GOOD only reports;
    // FALSE reports AND the owner clears the notch -- the panel still issues
    // no command itself (display-only discipline, spec §3.4). slot is what
    // setDisplayedSlot() named; lane is SnapshotNotch::channel.
    enum class Verdict : std::uint8_t { None, Good, False };
    std::function<void (int slot, int lane, int index, float hz, bool good, double ageMs)> onVerdict;

    static constexpr float kColVerdictW    = 92.0f;   // two buttons + gap + inset, and "VERDICT"
    static constexpr int   kVerdictButtonW = 40;
    static constexpr int   kVerdictButtonH = 20;
    static constexpr int   kVerdictGap     = 4;

    // TEST ACCESSORS -- null when `row` has no buttons (out of range).
    [[nodiscard]] juce::TextButton* goodButtonForTest (int row);
    [[nodiscard]] juce::TextButton* falseButtonForTest (int row);
    [[nodiscard]] Verdict verdictForTest (int row) const;
    [[nodiscard]] int displayedSlotForTest() const { return displayedSlot_; }
```

`RowText`: add `std::uint64_t key = 0; Verdict verdict = Verdict::None; juce::String verdictText;`.

Private:

```cpp
    // Lane D: the ONE deliberate exception to "paint draws prebuilt members
    // only" -- a click needs a real child Component. Keyed by the same
    // identity the sightings ledger uses, created when an identity is first
    // seen, destroyed when it leaves tracking; NEVER rebuilt per refresh, so
    // a click cannot land on a button that no longer exists.
    struct RowButtons
    {
        std::unique_ptr<juce::TextButton> good, bad;
        Verdict state = Verdict::None;
    };
    std::map<std::uint64_t, RowButtons> buttons_;
    int displayedSlot_ = 0;

    void ensureButtonsFor (std::uint64_t key, const NotchController::SnapshotNotch& notch);
    void layoutButtons();
    void reportVerdict (std::uint64_t key, bool good);
```

- [ ] **Step 4: Implement the .cpp** — key pieces:

```cpp
float NotchListPanel::statusWidthFor (const float frameWidth)
{
    const float fixedColumnsWidth = kColIdW + kColLaneW + kColFreqW + kColDepthW + kColQW + kColVerdictW;
    return juce::jmax (0.0f, frameWidth - (2.0f * kLeftPad) - fixedColumnsWidth);
}

void NotchListPanel::setDisplayedSlot (const int slotIndex)
{
    displayedSlot_ = slotIndex;
    // ... existing caption code unchanged ...
}

void NotchListPanel::ensureButtonsFor (const std::uint64_t key, const NotchController::SnapshotNotch& notch)
{
    if (buttons_.find (key) != buttons_.end())
        return;
    RowButtons rb;
    rb.good = std::make_unique<juce::TextButton> ("GOOD");    // ONE argument (JUCE 9 trap)
    rb.bad  = std::make_unique<juce::TextButton> ("FALSE");
    for (auto* b : { rb.good.get(), rb.bad.get() })
    {
        b->getProperties().set (juce::Identifier ("azStyle"), "ghost");   // AzTheme.cpp:33
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    rb.good->onClick = [this, key] { reportVerdict (key, true); };
    rb.bad->onClick  = [this, key] { reportVerdict (key, false); };
    juce::ignoreUnused (notch);
    buttons_.emplace (key, std::move (rb));
}

void NotchListPanel::reportVerdict (const std::uint64_t key, const bool good)
{
    const auto it = buttons_.find (key);
    if (it == buttons_.end() || it->second.state != Verdict::None)
        return;
    it->second.state = good ? Verdict::Good : Verdict::False;

    // Find the row for the payload; the row strings are already built.
    for (auto& row : rows_)
    {
        if (row.key != key)
            continue;
        row.verdict     = it->second.state;
        row.verdictText = good ? "GOOD" : "FALSE";
        const auto& notch = snapshot_.notches[(std::size_t) (&row - rows_.data())];
        if (onVerdict != nullptr)
            onVerdict (displayedSlot_, (int) notch.channel, (int) notch.index, notch.frequency, good, row.ageMs);
        break;
    }
    layoutButtons();
    repaint();
}
```

`refreshFromSnapshot()`: in loop 1, after `sightings_` upkeep, call `ensureButtonsFor (key, notch)`; in loop 2, set `row.key = key` and copy `row.verdict`/`row.verdictText` from `buttons_[key].state`; in loop 3, when erasing a sighting also `buttons_.erase (it->first)`; finally `layoutButtons()` before `repaint()`. Rows are pushed in snapshot order, so `rows_[i]` ↔ `snapshot_.notches[i]` — `reportVerdict` relies on that.

`layoutButtons()` (called from `refreshFromSnapshot`, `resized`, `reportVerdict`): every button is hidden first; then for each `rows_[i]` with `verdict == None`, place `good` at `x = x5 + 2`, `bad` at `x = x5 + 2 + kVerdictButtonW + kVerdictGap`, `y = rowTop + (kRowHeight − kVerdictButtonH) / 2`, where `x5 = kLeftPad + kColIdW + kColLaneW + kColFreqW + kColDepthW + kColQW + statusWidthFor (width)` and `rowTop = kCaptionHeight + kHeaderHeight + i · kRowHeight`; rows past the panel bottom stay hidden (mirror the `break` in `paint`).

`paint()`: header gains `g.drawText ("VERDICT", (int) x5, ...)` with `x5 = x4 + statusW`; row loop draws `row.verdictText` at `x5` in `accent` for GOOD and `danger` for FALSE (colours from `az::theme`), only when non-empty. `setController()` also clears `buttons_`.

Accessors: `goodButtonForTest (row)` → `row` in range ? `buttons_[rows_[row].key].good.get()` : nullptr; likewise for `bad`; `verdictForTest (row)` → `rows_[row].verdict`.

- [ ] **Step 5: Build and run** — `cmake -B build ... && cmake --build build --config Release && cd build && ctest -C Release --output-on-failure -R NotchListPanel`. Expected: all pass including the 3 new ones and the amended column test.

- [ ] **Step 6: Render and READ the image** — `build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast`, then open `shots/console-live.png` (Read tool). The VERDICT header must read in full, both buttons on every row must show their word untruncated, and HELD must still show its age. Fix before committing; this is the check the tests cannot do (memory: ui-rebuild-sodium-rack).

- [ ] **Step 7: Full suite** — `100% tests passed` (420).

- [ ] **Step 8: Commit**

```bash
git add src/gui/NotchListPanel.h src/gui/NotchListPanel.cpp tests/test_notchlistpanel.cpp
git commit -m "feat(gui): GOOD / FALSE verdict buttons per notch row, keyed by identity"
```

---

