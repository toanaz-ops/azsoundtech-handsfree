// SoundcheckPanel -- lane M Task 9. The interactive chrome of an active
// soundcheck: the progress overlay with its big DUNG, and the results strip
// with AP DUNG / BO.
//
// WHY THIS IS NOT PART OF SpectrumView
// ====================================
// SpectrumView.h is already ~500 lines carrying its own toolbar, a ring-risk
// chip and a notch age ledger. Two modal-ish states with three buttons and a
// five-line summary would make a big file bigger for no reuse. SpectrumView
// gains only the DATA overlay (the margin curve and its markers); everything a
// finger touches lives here.
//
// THIS COMPONENT IS DRIVEN BY PLAIN DATA, NEVER BY A CONTROLLER
// =============================================================
// It holds no pointer to SoundcheckController and calls nothing on one. The
// owner (Task 10) pushes a Mode, a progress triple and a Model; the panel
// formats and draws. That is what lets every state in here be rendered
// headless -- by a test, and by tools/snapshot.cpp -- without a device, a
// thread or a run.
//
// It also means the DIALOG STATES ARE NOT SoundcheckController::State. That
// machine never enters Preflight / Confirm / Arm (SoundcheckController.h:124-129)
// -- those three live entirely on the message thread, so a GUI that switched on
// getState() would show nothing at all while the operator was reading the
// confirmation. Mode below is the GUI's own vocabulary, deliberately smaller.
//
// THE COUNTDOWN IS LANE M'S OWN (F12)
// ===================================
// setProgress takes a remainingMs the owner reads from
// SoundcheckController::getRemainingMsInRun(). It must NEVER be fed
// NotchController::getSoundcheckRemainingMs(), which reports the PASSIVE 15 s
// window and measures it in liveMs_ -- frozen while the taps are suspended, so
// it would show a number that stands still or reads 0 for the whole run.
//
// DUNG IS THE OFFICIAL STOP; Esc IS BEST EFFORT (F14)
// ===================================================
// keyPressed only sees a key when this component has focus, and the preset name
// field takes keys too (MainComponent.cpp:255-290). So the button is the path
// that is always there, and Esc is the shortcut for whoever happens to have
// focus here. Esc is refused outside Running: once the run is over the only two
// answers are AP DUNG and BO, and an Esc that silently meant one of them would
// be a guess about which.
//
// EVERY VIETNAMESE STRING IS EXPLICIT UTF-8 BYTES. This build passes no /utf-8
// to MSVC and only one file in the tree carries a BOM, so a source literal is
// decoded with whatever the machine's active codepage happens to be. That is
// the mojibake middle dot all over again; src/gui/DeviceViewModel.cpp:13 is the
// precedent being followed.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>
#include <functional>

namespace gui
{

class SoundcheckPanel : public juce::Component
{
public:
    SoundcheckPanel();
    ~SoundcheckPanel() override = default;

    // The strip's height. Fixed rather than derived: it is an OVERLAY over the
    // analyser, and a band whose height moved with the window would cover a
    // different amount of the plot at every size.
    static constexpr int kPanelHeight = 104;

    // Applied is where SoundcheckApplyStats lands (Task 10). It exists NOW, and
    // empty, because the one outcome that must never be silent belongs in it:
    // clearedPrevious > 0 with placed == 0 means the run removed working
    // notches and put nothing back, so the room is measurably WORSE than
    // before APPLY was pressed. With nowhere to report that, Task 10's obvious
    // move is to hide the strip on success and say nothing -- which is exactly
    // how that outcome would ship silently onto a PA.
    enum class Mode { Hidden, Running, Results, Applied };

    void setMode (Mode mode);
    [[nodiscard]] Mode getMode() const { return mode_; }

    // channelIndex is 0-BASED (it comes straight from
    // SoundcheckController::getCurrentTargetIndex()); the caption prints it
    // 1-based, because "channel 0 of 4" is not a thing an operator counts.
    void setProgress (int channelIndex, int channelCount, double remainingMs);

    // What the results strip says. Every field is a COUNT of a different
    // thing, and they are deliberately not summable:
    //
    //   hotSpots       candidates the run would place if AP DUNG is pressed
    //   saturatedBins  bins still over after the DEEPEST cut on the ladder --
    //                  a filter cannot fix these, the gain structure has to
    //   unmeasured     OutputResult::measured == false. "Could not measure",
    //                  which is a VALID result (Q8) and NOT "the room is clean"
    //   routingInvalid OutputResult::routingInvalid. A DIFFERENT sentence from
    //                  unmeasured: it asks the operator to fix the patch, not
    //                  to look at the room (F26)
    //   cannotPropose  OutputResult::ceilingMissing || ladderMissing. pick()
    //                  had nothing to propose FROM. Reported as an ERROR and
    //                  never as an empty proposal list, because an empty list
    //                  reads as an excellent room (Task 3 I-3)
    //   placed / clearedPrevious
    //                  SoundcheckApplyStats, AFTER the operator pressed AP
    //                  DUNG. `placed` counts LANE-WRITES (2 per linked pair),
    //                  never candidates -- the two numbers differ and reading
    //                  one as the other would report a linked stereo pair as
    //                  twice the work it was.
    struct Model
    {
        int hotSpots       = 0;
        int saturatedBins  = 0;
        int unmeasured     = 0;
        int routingInvalid = 0;
        int cannotPropose  = 0;
        int placed         = 0;
        int clearedPrevious = 0;

        // THE outcome that must never be shown as success: the apply removed
        // notches that were holding and placed nothing in their place, so the
        // room is worse than it was before the operator touched anything.
        //
        // DERIVED, not a field the caller sets. A bool beside the two counts it
        // is computed from is a bool that can be set to disagree with them, and
        // the disagreement would be invisible until a show.
        [[nodiscard]] bool worseOff() const
        {
            return clearedPrevious > 0 && placed == 0;
        }
    };

    void setResults (const Model& model);

    // What this strip needs to show every line it is holding WITHOUT cutting
    // one off. The owner asks rather than assuming kPanelHeight, because the
    // summary's line count is data-dependent and a fault sentence that is
    // half-drawn is a fault the operator never reads. Never less than
    // kPanelHeight, so the Running layout is unaffected.
    [[nodiscard]] int preferredHeight() const;

    // The figure preferredHeight() and resized() both count in. Exposed so a
    // test asserts against the number the code actually uses rather than a
    // second copy of it that is free to drift.
    static constexpr int kSummaryLineHeightForTest = 19;

    // The four-field convenience the brief names. Kept because it is the
    // common case; it fills a Model and forwards.
    void setResultsSummary (int hotSpots, int saturatedBins,
                            int unmeasured, int routingInvalid);

    std::function<void()> onStop, onApply, onDismiss;

    // Fired by setMode AFTER the new mode is in force. The owner re-lays its
    // own console out: this strip is not an overlay ON the analyser, it TAKES
    // a band off the bottom of it. Laid over the plot instead, it buried the
    // frequency axis and the overlay's own marked-bin rake -- which no test
    // noticed and one render did (CLAUDE.md, 2026-08-25).
    std::function<void()> onModeChanged;

    bool keyPressed (const juce::KeyPress& key) override;
    void paint (juce::Graphics& g) override;
    void resized() override;

    // Public: they ARE this component's interface, exactly as ModeRail's are.
    // Headless tests and the snapshot tool drive them directly, and onClick()
    // is invoked rather than triggerClick() -- the latter POSTS a message and
    // this suite pumps no loop (memory/data-loop-lessons-2026-09-05.md).
    //
    // The two-argument juce::TextButton(name, tooltip) is the trap here, and
    // the mechanism is worth stating exactly, because the one-line version of
    // it is wrong: param 2 IS the tooltip (juce_TextButton.cpp:46-49). What
    // ships blank is `{ {}, "LABEL" }` -- an empty NAME with the legend put in
    // the tooltip slot, which renders a button with no text while the build
    // stays green. So the defence is not "use one argument", it is "assert the
    // exact label", which the tests do
    // (memory/juce9-api-traps-2026-08-25.md; its rule is right).
    juce::TextButton stopButton    { juce::String::fromUTF8 ("D\xe1\xbb\xaaNG") };
    juce::TextButton applyButton   { juce::String::fromUTF8 ("\xc3\x81P D\xe1\xbb\xa4NG") };
    juce::TextButton dismissButton { juce::String::fromUTF8 ("B\xe1\xbb\x8e") };

    // TEST ACCESSORS ONLY.
    [[nodiscard]] juce::String summaryTextForTest() const;
    [[nodiscard]] juce::String progressTextForTest() const  { return progressCaption_; }
    [[nodiscard]] juce::String countdownTextForTest() const { return countdownLabel_.getText(); }
    [[nodiscard]] juce::Rectangle<int> summaryBoundsForTest() const { return summaryArea_; }
    [[nodiscard]] bool hasErrorForTest() const { return hasError_; }

private:
    // At most: the headline, the cannot-propose error, the saturation warning,
    // the unmeasured sentence and the routing sentence. A fixed array rather
    // than a vector so nothing in the summary path can grow.
    // Headline, cannot-propose, saturation, unmeasured, routing -- and the
    // worse-off line, which can appear beside any of them.
    static constexpr std::size_t kMaxSummaryLines = 6;

    struct SummaryLine
    {
        juce::String text;
        juce::Colour colour;
    };

    void rebuildSummary();
    void addSummaryLine (const juce::String& text, juce::Colour colour);

    Mode mode_ = Mode::Hidden;

    Model model_ {};
    bool  hasError_ = false;

    std::array<SummaryLine, kMaxSummaryLines> summaryLines_ {};
    std::size_t summaryLineCount_ = 0;

    // The caption and the number are TWO things, not one string. The number is
    // read across a room and wants to be large; the caption only has to be
    // findable, and one juce::Label cannot be two sizes -- the same reasoning
    // already written at ModeRail.h:66-72. So the caption is PAINTED (it is
    // re-drawn per frame anyway) and only the number gets a Label.
    juce::String progressCaption_;
    juce::Label  countdownLabel_;

    // Computed in resized(), read by paint(). Layout never happens inside
    // paint(): the same discipline SpectrumView is held to.
    juce::Rectangle<int> captionArea_;
    juce::Rectangle<int> summaryArea_;
    juce::Rectangle<int> accentBar_;

    // MEASURED in the constructor with the font the LookAndFeel will actually
    // use, not estimated -- a button narrower than its legend does not fail a
    // test, it ships truncated (memory/stereo-lane-lessons-2026-09-05.md).
    int stopWidth_ = 0, applyWidth_ = 0, dismissWidth_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundcheckPanel)
};

} // namespace gui
