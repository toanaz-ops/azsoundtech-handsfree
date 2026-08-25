#include "gui/ModeRail.h"

#include "gui/theme/AzTheme.h"

#include <utility>

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

    // Each mode is a latching switch, and the LAMP COLOUR is what its ON state
    // means -- not decoration, and not the same for all three:
    //
    //   SOUNDCHECK  amber   a timed state that will end on its own
    //   AUTO        green   the LED SIG green; the only green in the app
    //   BYPASS      red     filters out, which mid-show is a warning
    //
    // The theme's LookAndFeel reads buttonOnColourId to draw the lamp, so a
    // component says what ON means without knowing how a switch is drawn.
    const std::pair<juce::TextButton*, juce::Colour> modes[] = {
        { &soundcheckButton, warn   },
        { &autoButton,       ok     },
        { &bypassButton,     danger },
    };

    for (const auto& [button, lamp] : modes)
    {
        button->setClickingTogglesState (true);
        button->setRadioGroupId (kModeRadioGroupId);
        button->setColour (juce::TextButton::buttonOnColourId, lamp);
        addAndMakeVisible (*button);
    }

    // CLEAR ALL is destructive and is NOT drawn as a switch: an outline that
    // only fills under the pointer. A filled red slab beside three filled mode
    // slabs is a slab somebody eventually hits by accident, in the dark.
    clearAllButton.getProperties().set ("azStyle", "danger");
    addAndMakeVisible (clearAllButton);

    // The countdown is a NUMBER, so it is mono and it is big: it is read at a
    // glance from across a room while the room is being swept.
    countdownLabel.setFont (monoFont (18.0f));
    countdownLabel.setColour (juce::Label::textColourId, warn);
    countdownLabel.setJustificationType (juce::Justification::centred);
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

void ModeRail::setDisplayedMode (const Mode mode)
{
    // All three set EXPLICITLY rather than relying on the radio group to clear
    // the others: the group's own clearing runs through the notification path
    // this method exists to avoid.
    soundcheckButton.setToggleState (mode == Mode::Soundcheck, juce::dontSendNotification);
    autoButton      .setToggleState (mode == Mode::Auto,       juce::dontSendNotification);
    bypassButton    .setToggleState (mode == Mode::Bypass,     juce::dontSendNotification);
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
    if (confirmHook == nullptr || confirmPending_)
        return;

    // One dialog at a time: while the previous confirmation is still
    // unanswered, further clicks are dropped -- otherwise stacked dialogs
    // would each fire onClearAllConfirmed.
    confirmPending_ = true;

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
            return;   // rail gone -- nothing left to clear or notify

        safeThis->confirmPending_ = false;   // answered: allow the next ask

        if (confirmed && safeThis->onClearAllConfirmed != nullptr)
            safeThis->onClearAllConfirmed();
    });
}

void ModeRail::resized()
{
    using namespace az::theme;

    juce::FlexBox fb;
    fb.flexDirection = orientation_ == Orientation::Vertical
                           ? juce::FlexBox::Direction::column
                           : juce::FlexBox::Direction::row;
    fb.alignItems = juce::FlexBox::AlignItems::stretch;

    const bool vertical = orientation_ == Orientation::Vertical;
    const float cellW   = (float) (vertical ? railWidth : buttonCellWidth);
    const float cellH   = (float) buttonCellHeight;
    const float gapPx   = (float) gap;

    // A trailing gap on every cell but the last keeps the row on the 8 px grid
    // without the FlexBox gap property (which this JUCE version lacks).
    auto cell = [vertical, gapPx] (juce::Component& c, float w, float h)
    {
        auto item = juce::FlexItem (c).withWidth (w).withHeight (h);
        return vertical ? item.withMargin ({ 0.0f, 0.0f, gapPx, 0.0f })
                        : item.withMargin ({ 0.0f, gapPx, 0.0f, 0.0f });
    };

    fb.items.add (cell (soundcheckButton, cellW, cellH));
    fb.items.add (cell (autoButton,       cellW, cellH));
    fb.items.add (cell (bypassButton,     cellW, cellH));

    // The countdown takes the slack between the modes and CLEAR ALL, so the
    // destructive control is always pinned at the far end of the transport --
    // as far from the three switches a hand reaches for as the row allows.
    auto readout = juce::FlexItem (countdownLabel).withFlex (1.0f);
    readout = vertical ? readout.withWidth (cellW).withMargin ({ 0.0f, 0.0f, gapPx, 0.0f })
                       : readout.withHeight (cellH).withMargin ({ 0.0f, gapPx, 0.0f, 0.0f });
    fb.items.add (readout);

    fb.items.add (juce::FlexItem (clearAllButton)
                      .withWidth ((float) (vertical ? railWidth : clearCellWidth))
                      .withHeight (cellH));

    fb.performLayout (getLocalBounds());
}

} // namespace gui
