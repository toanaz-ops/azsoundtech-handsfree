// CopilotPanel -- lane L2a (spec 2026-11-09-copilot-panel-design.md section 5).
//
// A PASSIVE view of what the copilot sidecar found in this show's session log:
// a caption with two buttons (PHAN TICH, CAI AI), one status sentence, a
// scrollable area and, lane L2b, an ask row (one-line editor + HOI/HUY).
// The scrollable area shows EITHER the findings rows OR one text block (the AI's
// answer, the fixed "blocked" sentence, or an error reason).
// It owns no thread, no SidecarRunner, no key store -- MainComponent runs the
// sidecar and tells this panel what happened through setState()/setFindings()/
// setAnswer(). It never opens a dialog: every outcome is one sentence on the
// status line.
//
// Row heights are variable. Advice is wrapped Vietnamese prose, so each row is
// measured with the real font through juce::TextLayout at the width it will be
// drawn at; a fixed row height clipped diacritic-heavy lines in earlier panels.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/CopilotAnswer.h"
#include "app/CopilotFindings.h"
#include "app/SidecarRunner.h"

#include <functional>
#include <vector>

namespace gui
{

class CopilotPanel : public juce::Component
{
public:
    enum class State
    {
        idle,
        running,
        findings,
        clean,
        noLog,
        sidecarMissing,
        timedOut,
        failed,
        // Lane L2b "ask the AI".
        asking,
        answer,
        blocked,
        askFailed,
        needsSetup,
        cancelled,
        busy            // a click was refused because a run is still unwinding
    };

    CopilotPanel();
    ~CopilotPanel() override;

    /** Any state other than `findings` also clears the list. */
    void setState (State newState);

    /** Sets `findings` (or `clean` when the list is empty) and rebuilds the rows. */
    void setFindings (std::vector<CopilotFinding> findings);

    /** Maps an answer to a state: answer -> answer, blocked -> blocked, error ->
        askFailed (the reason is the text block), invalid -> askFailed with a
        fixed sentence. */
    void setAnswer (const CopilotAnswer& answer);

    /** State askFailed with `reason` as the text block (one sentence). */
    void setAskFailed (const juce::String& reason);

    [[nodiscard]] State state() const noexcept { return state_; }

    /** Called on the message thread when the user presses the button. */
    std::function<void()> onAnalyse;

    // Lane L2b. All are called on the message thread.
    std::function<void()> onOpenSetup;
    std::function<void (juce::String)> onAsk;   // trimmed, non-empty question
    std::function<void()> onCancelAsk;

    // Geometry shared with MainComponent's layout and the tests.
    static constexpr int kCaptionHeight = 26;
    static constexpr int kStatusHeight  = 22;
    static constexpr int kMinListHeight = 96;   // room for about three short rows
    static constexpr int kAskHeight     = 34;   // ask row: 22 px controls + padding
    static constexpr int kMinHeight     = kCaptionHeight + kStatusHeight + kMinListHeight + kAskHeight;

    /** The one sentence a state shows. `count` is used by `findings` only. */
    static juce::String statusTextFor (State s, int count = 0);

    // TEST ACCESSORS
    [[nodiscard]] juce::String getStatusTextForTest() const { return statusText_; }
    [[nodiscard]] int getRowCountForTest() const;
    [[nodiscard]] int getRowHeightForTest (int row) const;
    [[nodiscard]] int getContentHeightForTest() const;
    [[nodiscard]] juce::TextButton& getAnalyseButtonForTest() { return analyseButton_; }
    [[nodiscard]] juce::Rectangle<int> getListBoundsForTest() const { return viewport_.getBounds(); }
    [[nodiscard]] juce::TextButton& getSetupButtonForTest() { return setupButton_; }
    [[nodiscard]] juce::TextButton& getAskButtonForTest() { return askButton_; }
    [[nodiscard]] juce::TextEditor& getAskEditorForTest() { return askEditor_; }
    [[nodiscard]] juce::String getBlockTextForTest() const;
    [[nodiscard]] juce::String getFooterTextForTest() const;
    [[nodiscard]] juce::Rectangle<int> getAskRowBoundsForTest() const { return askRowBounds_; }
    [[nodiscard]] bool isCautionVisibleForTest() const { return ! cautionBounds_.isEmpty(); }
    [[nodiscard]] juce::Rectangle<int> getCautionBoundsForTest() const { return cautionBounds_; }
    [[nodiscard]] static juce::String cautionTextForTest();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ListContent;

    void refreshStatus();
    void layoutList();
    void submitQuestion();
    void setBlock (const juce::String& text, const juce::String& footer, juce::Colour colour);

    State state_ = State::idle;
    int findingCount_ = 0;
    juce::String statusText_;

    juce::TextButton analyseButton_;
    juce::TextButton setupButton_;
    juce::TextEditor askEditor_;
    juce::TextButton askButton_;
    juce::Rectangle<int> askRowBounds_;
    juce::Rectangle<int> listArea_;        // list + caution strip, set by resized()
    juce::Rectangle<int> cautionBounds_;   // fixed strip under an AI answer; empty otherwise
    juce::Viewport   viewport_;
    std::unique_ptr<ListContent> content_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CopilotPanel)
};

/** The one mapping from a sidecar run to the panel's state: sidecarMissing and
    timedOut map to themselves; exit code 2 (log unreadable) -> noLog; any other
    failure, or output that is not a findings array -> failed; ok -> the parsed
    findings. */
void applyCopilotResult (CopilotPanel&, const CopilotRunResult&);

/** Same idea for an "ask the AI" run (lane L2b): sidecarMissing / timedOut map to
    themselves; exit code 5 (the sidecar refused the arguments) -> askFailed with
    "setup not valid, press CAI AI"; exit code 2 -> noLog; anything else parses
    stdout with parseCopilotAnswer and goes through setAnswer(). */
void applyCopilotAskResult (CopilotPanel&, const CopilotRunResult&);

} // namespace gui
