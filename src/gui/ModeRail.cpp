#include "gui/ModeRail.h"

#include "gui/theme/AzTheme.h"

#include <cmath>
#include <utility>

namespace gui
{

namespace
{
// Any non-zero id makes the three mode buttons mutually exclusive and keeps
// the pressed one ON when clicked again rather than toggling off.
constexpr int kModeRadioGroupId = 1;

// Room reserved above the countdown number for its painted caption.
constexpr int kCountdownCaptionHeight = 14;
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
    // The hint is the second line on each switch. A legend names the mode; the
    // hint says what it DOES, which is what somebody who has not read a manual
    // actually needs at the moment they are deciding which one to hit.
    struct Mode { juce::TextButton* button; juce::Colour lamp; const char* hint; };

    const Mode modes[] = {
        { &soundcheckButton, warn,   "sweep the room" },
        { &autoButton,       ok,     "catch and hold" },
        { &bypassButton,     danger, "filters out"    },
    };

    for (const auto& mode : modes)
    {
        mode.button->setClickingTogglesState (true);
        mode.button->setRadioGroupId (kModeRadioGroupId);
        mode.button->setColour (juce::TextButton::buttonOnColourId, mode.lamp);
        mode.button->getProperties().set (hintProperty, mode.hint);
        addAndMakeVisible (*mode.button);
    }

    // CLEAR ALL is destructive and is NOT drawn as a switch: an outline that
    // only fills under the pointer. A filled red slab beside three filled mode
    // slabs is a slab somebody eventually hits by accident, in the dark.
    clearAllButton.getProperties().set ("azStyle", "danger");
    addAndMakeVisible (clearAllButton);

    // LANE M -- DO. A MOMENTARY control, deliberately outside the radio group:
    // it does not latch, it does not un-light whichever mode the engine is
    // actually in, and a second press while a run is in flight is impossible
    // because setMeasureEnabled(false) takes it out of reach (F10).
    //
    // Its face is the ACCENT rather than a mode lamp, because it is the only
    // control on this rail that MAKES SOUND COME OUT OF THE PA. The hint says
    // so in the operator's own language: the legend names the control, the
    // hint says what it does to the room.
    measureButton.getProperties().set (hintProperty,
        // "phat tin hieu" -- explicit UTF-8 bytes; see ModeRail.h on the
        // measureButton declaration for why nothing here is a source literal.
        juce::String::fromUTF8 ("ph\xc3\xa1t t\xc3\xadn hi\xe1\xbb\x87u"));
    // The ACCENT on the legend, not on buttonOnColourId. That colour id only
    // paints the lamp of a LATCHED switch, and this button never latches -- so
    // the line it replaces set a colour that could not be reached, and the
    // cell that makes sound come out of the PA was drawn exactly like the
    // three that do not. Same route AP DUNG takes on the results strip.
    measureButton.setColour (juce::TextButton::textColourOffId, accent);
    measureButton.onClick = [this] { if (onMeasure != nullptr) onMeasure(); };
    addAndMakeVisible (measureButton);

    // MEASURED, not estimated: the widest string this cell must print, set in
    // the font the LookAndFeel will actually draw it in. A cell narrower than
    // its legend does not fail a test -- it ships a truncated button
    // (memory/stereo-lane-lessons-2026-09-05.md).
    //
    // Two strings, because the switch face carries both: the uppercase legend
    // in the tracked silkscreen face, and the hint underneath in mono at
    // hintFontSize. drawButtonText() reduces the face by `gap` on each side
    // before fitting either, so that padding is added back here.
    {
        const auto legendW = stringWidth (legendFont (switchFontSize, true, trackingSwitch),
                                          measureButton.getButtonText().toUpperCase());
        const auto hintW   = stringWidth (monoFont (hintFontSize),
                                          measureButton.getProperties()
                                              .getWithDefault (hintProperty, juce::String())
                                              .toString());

        measureCellWidth_ = juce::jmax (touchTarget,
                                        (int) std::ceil (juce::jmax (legendW, hintW))
                                            + 2 * gap);
    }

    // The countdown is a NUMBER, so it is mono and it is big: it is read at a
    // glance from across a room while the room is being swept. Its caption is
    // painted above it (see paint()).
    countdownLabel.setFont (monoFont (26.0f));
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

    // Seed the readout NOW rather than leaving it blank until the first timer
    // tick 250 ms later -- and, more usefully, so a headless render (which
    // pumps no timers at all) shows the real resting state.
    updateCountdown();

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

void ModeRail::setModeControlsEnabled (const bool enabled)
{
    // DO is NOT in this list. The two locks have different lifetimes: the mode
    // switches and CLEAR ALL come back the moment the run ends, while DO stays
    // dead through the whole Results window -- starting a second measurement
    // on top of an unanswered proposal set would throw the proposals away.
    soundcheckButton.setEnabled (enabled);
    autoButton      .setEnabled (enabled);
    bypassButton    .setEnabled (enabled);
    clearAllButton  .setEnabled (enabled);
}

void ModeRail::setMeasureEnabled (const bool enabled)
{
    measureButton.setEnabled (enabled);
}

void ModeRail::updateCountdown()
{
    using namespace az::theme;

    const double remainingMs = (getSoundcheckRemainingMs != nullptr)
                                   ? getSoundcheckRemainingMs()
                                   : 0.0;
    const int seconds = remainingMs > 0.0
                            ? (int) std::ceil (remainingMs / 1000.0)
                            : 0;

    // An em dash rather than an empty string when nothing is counting: a cell
    // that empties itself reads as a control that vanished. A dash reads as a
    // readout with nothing to report, which is what it is.
    const bool counting = seconds > 0;

    countdownLabel.setText (counting ? juce::String (seconds) + " s"
                                     : juce::String::charToString ((juce::juce_wchar) 0x2014),
                            juce::dontSendNotification);
    countdownLabel.setColour (juce::Label::textColourId, counting ? warn : faded);
    repaint();
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

void ModeRail::paint (juce::Graphics& g)
{
    using namespace az::theme;

    // The countdown's caption. Painted rather than given its own Label because
    // it never changes, and a Label for a constant string is a component and a
    // layout pass bought for nothing.
    if (countdownCaptionArea_.isEmpty())
        return;

    drawCaption (g, "Soundcheck", countdownCaptionArea_, dim);
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

    // LANE M -- DO, immediately after the three modes and before the slack.
    // Narrower than a mode switch on purpose: a two-character legend in a
    // 168 px cell reads as a mode somebody forgot to name. In the vertical
    // orientation every cell is railWidth, so it follows suit there.
    fb.items.add (cell (measureButton,
                        vertical ? cellW : (float) measureCellWidth_,
                        cellH));

    // The countdown takes the slack between the modes and CLEAR ALL, so the
    // destructive control is always pinned at the far end of the transport --
    // as far from the three switches a hand reaches for as the row allows.
    // A BARE spacer, not the countdown label. The countdown is a two-part
    // block -- a caption over a number -- and FlexBox lays out one component
    // per item, so the block is positioned by hand out of the gap the spacer
    // leaves behind. Doing it the other way (label in the flex, caption
    // offset from the label's bounds) is what put the caption outside the
    // rail entirely.
    fb.items.add (juce::FlexItem().withFlex (1.0f)
                      .withMargin (vertical ? juce::FlexItem::Margin { 0.0f, 0.0f, gapPx, 0.0f }
                                            : juce::FlexItem::Margin { 0.0f, gapPx, 0.0f, 0.0f }));

    fb.items.add (juce::FlexItem (clearAllButton)
                      .withWidth ((float) (vertical ? railWidth : clearCellWidth))
                      .withHeight (cellH));

    fb.performLayout (getLocalBounds());

    // The gap the spacer left, between the last mode switch and CLEAR ALL.
    // The gap now opens after DO rather than after BYPASS: the spacer flex item
    // sits between them, and reading it off the wrong neighbour would lay the
    // countdown block straight over the new cell.
    auto block = vertical
                     ? juce::Rectangle<int> (0, measureButton.getBottom() + gap,
                                             railWidth,
                                             juce::jmax (0, clearAllButton.getY() - gap
                                                            - (measureButton.getBottom() + gap)))
                     : juce::Rectangle<int> (measureButton.getRight() + gap, 0,
                                             juce::jmax (0, clearAllButton.getX() - gap
                                                            - (measureButton.getRight() + gap)),
                                             getHeight());

    if (block.getWidth() <= 0 || block.getHeight() <= 0)
    {
        countdownCaptionArea_ = {};
        countdownLabel.setBounds ({});
        return;
    }

    // Vertically centred as a PAIR inside that gap, so the block sits on the
    // switches' optical centre line rather than hanging from the rail's top.
    constexpr int numberHeight = 32;
    const int blockHeight = kCountdownCaptionHeight + numberHeight;

    auto stacked = block.withSizeKeepingCentre (block.getWidth(),
                                                juce::jmin (block.getHeight(), blockHeight));

    countdownCaptionArea_ = stacked.removeFromTop (kCountdownCaptionHeight);
    countdownLabel.setBounds (stacked);
}

} // namespace gui
