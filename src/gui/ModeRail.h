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

    // The rail stays layout-agnostic: the owner picks the orientation, the
    // rail just renders it. Since the L1/L2 dual-layout was removed (commit
    // 3a04200) the single Classic layout always uses Horizontal; Vertical and
    // this setter remain for tests and any future host that lays out
    // differently.
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

    // Lane M (Q16). A SEPARATE request from onSoundcheck, because the two
    // things are separate: SOUNDCHECK waits 15 s for the room to howl on its
    // own, DO plays a swept signal into the PA for ~72 s. One button meaning
    // both, on live-sound equipment, is a hazard -- so this is its own
    // callback on its own momentary button, never a fourth mode in the radio
    // group.
    std::function<void()> onMeasure;

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

    // Polls getSoundcheckRemainingMs now. The rail also runs a low-rate timer
    // calling this; owners may call it directly too.
    //
    // The label holds the NUMBER only ("7 s"), and an em dash when nothing is
    // counting. The word SOUNDCHECK is painted above it as a caption instead
    // of being concatenated into the string: the number is read across a room
    // and wants to be large, the caption only has to be findable, and one
    // Label cannot be two sizes.
    void updateCountdown();

    // Lane M F10. While a measurement is in flight every control that could
    // change what the filter chain is doing under it has to be out of reach:
    // the run is measuring that chain. DO itself is NOT in here -- it has its
    // own switch below, because it is also the control that has to stay
    // unreachable through the Results window while SOUNDCHECK and friends come
    // back.
    void setModeControlsEnabled (bool enabled);

    // Lane M. Off while a run or its Results window is open, and off whenever
    // the rig cannot be measured at all (no device, no enabled slot).
    void setMeasureEnabled (bool enabled);

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Public: they ARE this component's interface; wiring and headless tests
    // drive them directly.
    juce::TextButton soundcheckButton { "SOUNDCHECK" };
    // Lane M. "DO" with a crossed D: U+0110 U+004F, written as EXPLICIT UTF-8
    // BYTES rather than as a source literal. This build passes no /utf-8 to
    // MSVC, so a literal would be decoded with whatever the machine's active
    // codepage happens to be -- the mojibake that shipped the middle-dot bug
    // (src/gui/DeviceViewModel.cpp:13 is the precedent).
    //
    // The two-argument juce::TextButton(name, tooltip) is the trap here, and
    // the mechanism is worth stating exactly, because the one-line version of
    // it is wrong: param 2 IS the tooltip (juce_TextButton.cpp:46-49). What
    // ships blank is `{ {}, "LABEL" }` -- an empty NAME with the legend put in
    // the tooltip slot, which renders a button with no text while the build
    // stays green. So the defence is not "use one argument", it is "assert the
    // exact label", which the tests do
    // (memory/juce9-api-traps-2026-08-25.md; its rule is right).
    juce::TextButton measureButton    { juce::String::fromUTF8 ("\xc4\x90O") };
    juce::TextButton autoButton       { "AUTO" };
    juce::TextButton bypassButton     { "BYPASS" };
    juce::TextButton clearAllButton   { "CLEAR ALL" };
    juce::Label countdownLabel;

private:
    void timerCallback() override;
    void handleClearAllClicked();

    Orientation orientation_;

    // Where paint() draws the SOUNDCHECK caption. Computed in resized() and
    // stored, rather than derived from countdownLabel's bounds inside paint():
    // the label is positioned INSIDE this rect, so deriving one from the other
    // in the opposite direction is circular and drifted by exactly the
    // caption's height.
    juce::Rectangle<int> countdownCaptionArea_;

    // The DO cell's width, MEASURED in the constructor with the real legend
    // font and the real hint -- not estimated. A cell narrower than its legend
    // does not fail a test, it ships a truncated button
    // (memory/stereo-lane-lessons-2026-09-05.md). Measured once, in the ctor,
    // because neither string nor font ever changes: measuring in resized()
    // would buy the same number on every window drag.
    int measureCellWidth_ = 0;

    // True while a confirmation is still unanswered: a second CLEAR ALL
    // click must not stack another dialog (and so fire two confirms).
    bool confirmPending_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModeRail)
};

} // namespace gui
