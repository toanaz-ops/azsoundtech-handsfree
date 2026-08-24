// ModeRail -- GUI console redesign Task 2 (spec 2026-08-23 section 2).
// Three mode cells (SOUNDCHECK / AUTO / BYPASS), a CLEAR ALL button that is
// ALWAYS guarded by a confirmation hook before onClearAllConfirmed fires
// (controller decision R-3), and a soundcheck countdown readout fed from
// getSoundcheckRemainingMs(). Two orientations, same component: Vertical for
// the L2 performance rail, Horizontal for the L1 bottom strip.

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModeRail)
};

} // namespace gui
