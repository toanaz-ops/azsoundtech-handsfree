// CopilotPanel -- lane L2a (spec 2026-11-09-copilot-panel-design.md section 5).
//
// A PASSIVE view of what the copilot sidecar found in this show's session log:
// a caption, one button (PHAN TICH), one status sentence and a scrollable list of
// findings. It owns no thread and no SidecarRunner -- MainComponent runs the
// sidecar and tells this panel what happened through setState()/setFindings().
// It never opens a dialog: every outcome is one sentence on the status line.
//
// Row heights are variable. Advice is wrapped Vietnamese prose, so each row is
// measured with the real font through juce::TextLayout at the width it will be
// drawn at; a fixed row height clipped diacritic-heavy lines in earlier panels.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

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
        failed
    };

    CopilotPanel();
    ~CopilotPanel() override;

    /** Any state other than `findings` also clears the list. */
    void setState (State newState);

    /** Sets `findings` (or `clean` when the list is empty) and rebuilds the rows. */
    void setFindings (std::vector<CopilotFinding> findings);

    [[nodiscard]] State state() const noexcept { return state_; }

    /** Called on the message thread when the user presses the button. */
    std::function<void()> onAnalyse;

    // Geometry shared with MainComponent's layout and the tests.
    static constexpr int kCaptionHeight = 26;
    static constexpr int kStatusHeight  = 22;
    static constexpr int kMinListHeight = 96;   // room for about three short rows
    static constexpr int kMinHeight     = kCaptionHeight + kStatusHeight + kMinListHeight;

    /** The one sentence a state shows. `count` is used by `findings` only. */
    static juce::String statusTextFor (State s, int count = 0);

    // TEST ACCESSORS
    [[nodiscard]] juce::String getStatusTextForTest() const { return statusText_; }
    [[nodiscard]] int getRowCountForTest() const;
    [[nodiscard]] int getRowHeightForTest (int row) const;
    [[nodiscard]] int getContentHeightForTest() const;
    [[nodiscard]] juce::TextButton& getAnalyseButtonForTest() { return analyseButton_; }
    [[nodiscard]] juce::Rectangle<int> getListBoundsForTest() const { return viewport_.getBounds(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ListContent;

    void refreshStatus();

    State state_ = State::idle;
    int findingCount_ = 0;
    juce::String statusText_;

    juce::TextButton analyseButton_;
    juce::Viewport   viewport_;
    std::unique_ptr<ListContent> content_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CopilotPanel)
};

/** The one mapping from a sidecar run to the panel's state: sidecarMissing and
    timedOut map to themselves; exit code 2 (log unreadable) -> noLog; any other
    failure, or output that is not a findings array -> failed; ok -> the parsed
    findings. */
void applyCopilotResult (CopilotPanel&, const CopilotRunResult&);

} // namespace gui
