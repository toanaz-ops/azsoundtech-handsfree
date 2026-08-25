// ModeRail -- GUI console redesign Task 2 (spec 2026-08-23 section 2).
// Three mode cells (SOUNDCHECK / AUTO / BYPASS), a CLEAR ALL button that is
// ALWAYS guarded by a confirmation hook before onClearAllConfirmed fires
// (controller decision R-3), and a soundcheck countdown readout fed from
// getSoundcheckRemainingMs(). Two orientations, same component; the single
// remaining layout always uses Horizontal.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gui
{

class StatusBadge;

class ModeRail : public juce::Component,
                 private juce::Timer
{
public:
    enum class Orientation { Vertical, Horizontal };

    explicit ModeRail (Orientation orientation = Orientation::Vertical);
    ~ModeRail() override;

    // L1/L2 switching re-parents the SAME rail into a differently oriented
    // slot (spec section 3: components stay layout-agnostic), so the
    // orientation is mutable after construction.
    void setOrientation (Orientation newOrientation) { orientation_ = newOrientation; }
    [[nodiscard]] Orientation getOrientation() const { return orientation_; }

    // R-3: injectable so tests run headless (JUCE_MODAL_LOOPS_PERMITTED off).
    // The default opens a native asynchronous confirmation box and invokes
    // its callback EXACTLY ONCE with the user's choice. Only a callback of
    // true reaches onClearAllConfirmed.
    std::function<void (std::function<void (bool)>)> confirmHook;

    // Polled for the countdown readout (milliseconds); 0 / unset hides it.
    std::function<double()> getSoundcheckRemainingMs;

    std::function<void()> onSoundcheck;
    std::function<void()> onAuto;
    std::function<void()> onBypass;
    std::function<void()> onClearAllConfirmed;

    // Which switch is lit. Named here rather than taken as an AudioEngine::Mode
    // so the rail keeps knowing nothing about the engine -- the same separation
    // every other panel in this GUI holds to.
    enum class Mode { Soundcheck, Auto, Bypass };

    // Lights the switch for `mode` and clears the other two, WITHOUT invoking
    // onSoundcheck/onAuto/onBypass.
    //
    // This exists because the engine's mode can change from somewhere other
    // than these buttons -- it starts in Bypass, a preset can set it, and the
    // detector ends Soundcheck on its own. Before this the rail only ever
    // reflected the user's own clicks, so a freshly-launched window showed
    // BYPASSED on the status badge with no switch lit at all: the lamp, which
    // is the primary state signal in this design, was simply wrong.
    void setDisplayedMode (Mode mode);

    // Polls getSoundcheckRemainingMs now ("SOUNDCHECK 7s" while > 0). The rail
    // also runs a low-rate timer calling this; owners may call it directly too.
    void updateCountdown();

    void resized() override;

    // Public: they ARE this component's interface; wiring and headless tests
    // drive them directly.
    juce::TextButton soundcheckButton { "SOUNDCHECK" };
    juce::TextButton autoButton       { "AUTO" };
    juce::TextButton bypassButton     { "BYPASS" };
    juce::TextButton clearAllButton   { "CLEAR ALL" };
    juce::Label countdownLabel;

private:
    void timerCallback() override;
    void handleClearAllClicked();

    Orientation orientation_;

    // True while a confirmation is still unanswered: a second CLEAR ALL
    // click must not stack another dialog (and so fire two confirms).
    bool confirmPending_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModeRail)
};

} // namespace gui
