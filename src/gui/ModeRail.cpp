#include "gui/ModeRail.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

namespace
{
// Any non-zero id makes the three mode buttons mutually exclusive and keeps
// the pressed one ON when clicked again rather than toggling off.
constexpr int kModeRadioGroupId = 1;
} // namespace

ModeRail::ModeRail (Orientation orientation)
    : orientation_ (orientation)
{
    using namespace az::theme;

    for (auto* button : { &soundcheckButton, &autoButton, &bypassButton })
    {
        button->setClickingTogglesState (true);
        button->setRadioGroupId (kModeRadioGroupId);

        // Spec section 2: the active mode gets an ok-green background tint so
        // "is it protecting?" reads from across the room. Text switches to the
        // background colour for contrast on that tint.
        button->setColour (juce::TextButton::buttonOnColourId, ok);
        button->setColour (juce::TextButton::textColourOnId,   background);

        addAndMakeVisible (*button);
    }

    // Destructive action: danger colour, dark text for contrast.
    clearAllButton.setColour (juce::TextButton::buttonColourId,  danger);
    clearAllButton.setColour (juce::TextButton::textColourOffId, background);
    addAndMakeVisible (clearAllButton);

    // R-5: the LIST cell is an INDEPENDENT toggle (no radio group -- it does
    // not fight the mode cells and may be clicked twice). Accent tint while
    // the strip is out, matching "active / selected" in the theme.
    listToggleButton.setClickingTogglesState (true);
    listToggleButton.setColour (juce::TextButton::buttonOnColourId, accent);
    listToggleButton.setColour (juce::TextButton::textColourOnId,   background);
    addAndMakeVisible (listToggleButton);
    listToggleButton.onClick = [this]
    {
        if (onToggleNotchList != nullptr)
            onToggleNotchList (listToggleButton.getToggleState());
    };

    countdownLabel.setFont (monoFont());
    countdownLabel.setColour (juce::Label::textColourId, warn);
    countdownLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (countdownLabel);

    // The radio group deselects the previous mode through setToggleState with
    // a notification, which would re-fire the OLD mode's callback -- so each
    // handler fires only when its own button ended up ON. Clicking an
    // already-active button stays ON (radio groups never untoggle) and still
    // re-issues the request, matching ModeBar's behaviour.
    auto makeModeHandler = [this] (juce::TextButton& source, const std::function<void()>& sink)
    {
        source.onClick = [&source, sink]
        {
            if (! source.getToggleState())
                return;
            if (sink != nullptr)
                sink();
        };
    };
    makeModeHandler (soundcheckButton, [this]
    {
        if (onSoundcheck != nullptr) onSoundcheck();
    });
    makeModeHandler (autoButton, [this]
    {
        if (onAuto != nullptr) onAuto();
    });
    makeModeHandler (bypassButton, [this]
    {
        if (onBypass != nullptr) onBypass();
    });
    clearAllButton.onClick = [this] { handleClearAllClicked(); };

    // Default confirmation: native asynchronous box (R-3 -- no modal loops).
    // showAsync invokes its callback exactly once with the button's return
    // value; Clear maps to true, Cancel to false.
    confirmHook = [] (std::function<void (bool)> done)
    {
        // Button 1 (Clear) returns 1, button 2 (Cancel) returns 0.
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel (
            juce::MessageBoxIconType::WarningIcon,
            "CLEAR ALL",
            "Remove every notch filter? This cannot be undone.",
            "Clear", "Cancel", nullptr);
        juce::NativeMessageBox::showAsync (options,
            [done = std::move (done)] (int result) { done (result == 1); });
    };

    startTimerHz (4);
}

ModeRail::~ModeRail()
{
    stopTimer();
}

void ModeRail::updateCountdown()
{
    const double remainingMs = (getSoundcheckRemainingMs != nullptr)
                                   ? getSoundcheckRemainingMs()
                                   : 0.0;
    const int seconds = remainingMs > 0.0
                            ? (int) std::ceil (remainingMs / 1000.0)
                            : 0;

    countdownLabel.setText (seconds > 0 ? juce::String ("SOUNDCHECK ") + juce::String (seconds) + "s"
                                        : juce::String(),
                            juce::dontSendNotification);
}

void ModeRail::timerCallback()
{
    updateCountdown();
}

void ModeRail::handleClearAllClicked()
{
    if (confirmHook == nullptr)
        return;

    // The guard is the whole point of this button: nothing reaches
    // onClearAllConfirmed until the hook reports back confirmed == true.
    //
    // The dialog can outlive this component -- e.g. the window closes while
    // the native box is still open. The decision therefore arrives through a
    // SafePointer and is dropped if the rail is gone by then; capturing raw
    // `this` would invoke freed memory.
    juce::Component::SafePointer<ModeRail> safeThis { this };
    confirmHook ([safeThis] (bool confirmed)
    {
        if (safeThis == nullptr)
            return;

        if (confirmed && safeThis->onClearAllConfirmed != nullptr)
            safeThis->onClearAllConfirmed();
    });
}

void ModeRail::setListToggleVisible (const bool visible)
{
    if (listToggleButton.isVisible() == visible)
        return;

    listToggleButton.setVisible (visible);
    resized();   // the freed cell must return to the rail immediately
}

void ModeRail::resized()
{
    using namespace az::theme;

    constexpr float cellW  = (float) buttonCellWidth;
    constexpr float cellH  = (float) buttonCellHeight;
    constexpr float gapPx  = (float) gap;
    constexpr float labelH = 20.0f;

    juce::FlexBox fb;
    fb.flexDirection = orientation_ == Orientation::Vertical
                           ? juce::FlexBox::Direction::column
                           : juce::FlexBox::Direction::row;

    auto cellOf = [&cellW, &cellH, &gapPx, orientation = orientation_] (juce::Component& c)
    {
        auto item = juce::FlexItem (c).withWidth (cellW).withHeight (cellH);
        if (orientation == Orientation::Vertical)
            item.withMargin ({ 0.0f, 0.0f, gapPx, 0.0f }); // trailing gap keeps cells on the 8px grid
        else
            item.withMargin ({ 0.0f, gapPx, 0.0f, 0.0f });
        return item;
    };

    auto labelItem = juce::FlexItem (countdownLabel).withHeight (labelH);
    if (orientation_ == Orientation::Vertical)
    {
        labelItem.withWidth (cellW).withMargin ({ 0.0f, 0.0f, gapPx, 0.0f });
        fb.items.add (cellOf (soundcheckButton));
        fb.items.add (labelItem);
        fb.items.add (cellOf (autoButton), cellOf (bypassButton),
                      juce::FlexItem().withFlex (1.0f), // push CLEAR ALL + LIST to the rail's end
                      cellOf (clearAllButton));

        // R-5: the LIST cell sits below CLEAR ALL -- but only when the owner
        // says the layout wants it (L2; L1's fixed strip has no toggle).
        if (listToggleButton.isVisible())
            fb.items.add (cellOf (listToggleButton));
    }
    else
    {
        // Horizontal: the countdown takes the leftover width between the
        // mode cells and CLEAR ALL.
        labelItem.withFlex (1.0f).withMargin ({ 0.0f, gapPx, 0.0f, 0.0f });
        fb.items.add (cellOf (soundcheckButton), cellOf (autoButton),
                      cellOf (bypassButton), labelItem, cellOf (clearAllButton));
        if (listToggleButton.isVisible())
            fb.items.add (cellOf (listToggleButton));
    }

    fb.performLayout (getLocalBounds());
}

} // namespace gui
