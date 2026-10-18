#include "gui/DumbView.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

namespace
{
    juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }

    const char* kMeasure = "\xc4\x90O PH\xc3\x92NG";
    const char* kStop    = "D\xe1\xbb\xaaNG";
    const char* kApply   = "\xc3\x81P D\xe1\xbb\xa4NG";
    const char* kEnable  = "B\xe1\xba\xac\x54 CH\xe1\xbb\x90NG H\xc3\x9a";
    const char* kDismiss = "B\xe1\xbb\x8e";
    const char* kOff     = "T\xe1\xba\xae\x54";

    juce::Colour lampColour (DumbStatus::Lamp lamp)
    {
        using namespace az::theme;
        switch (lamp)
        {
            case DumbStatus::Lamp::Grey:     return dim;
            case DumbStatus::Lamp::Cyan:     return settled;   // ICE: the measurement, not the signal
            case DumbStatus::Lamp::Red:      return danger;
            case DumbStatus::Lamp::Amber:    return warn;
            case DumbStatus::Lamp::Green:    return ok;
            case DumbStatus::Lamp::DimGreen: return ok.withMultipliedAlpha (0.5f);
        }
        return dim;
    }

    // The three switches are the whole console here, so they are sized for a
    // thumb on a dark stage, not for a mouse: well over touchTarget.
    constexpr int kSwitchHeight = 120;
    constexpr int kSwitchMaxWidth = 340;
    constexpr int kGeekWidth = 96;
    constexpr float kHeadlineSize = 54.0f;
    constexpr float kSublineSize  = 20.0f;
    constexpr float kMessageSize  = 16.0f;
    constexpr int kLampDiameter = 44;
}

DumbView::DumbView()
{
    for (auto* b : { &leftButton, &middleButton, &offButton })
    {
        addAndMakeVisible (*b);
        b->setColour (juce::TextButton::buttonOnColourId, az::theme::accent);
    }

    geekButton.setButtonText ("GEEK");
    geekButton.getProperties().set ("azStyle", "ghost");
    addAndMakeVisible (geekButton);

    leftButton.onClick = [this]
    {
        const auto& f = status_.left == DumbStatus::Left::Stop  ? onStop
                      : status_.left == DumbStatus::Left::Apply ? onApply
                                                                : onMeasure;
        if (f) f();
    };
    middleButton.onClick = [this]
    {
        const auto& f = status_.middle == DumbStatus::Middle::Dismiss ? onDismiss : onEnable;
        if (f) f();
    };
    offButton.onClick  = [this] { if (onDisable) onDisable(); };
    geekButton.onClick = [this] { if (onSwitchToGeek) onSwitchToGeek(); };

    // A first frame with something honest on it, before any push arrives.
    setInputs ({}, {});
}

void DumbView::setInputs (const DumbInputs& inputs, const juce::String& message)
{
    const auto next = dumbStatusFor (inputs);

    const bool same = hasStatus_
                   && next.lamp == status_.lamp && next.pulse == status_.pulse
                   && next.headline == status_.headline && next.subline == status_.subline
                   && next.left == status_.left && next.middle == status_.middle
                   && next.leftEnabled == status_.leftEnabled
                   && next.middleEnabled == status_.middleEnabled
                   && next.middleOn == status_.middleOn && next.offOn == status_.offOn
                   && next.offEnabled == status_.offEnabled
                   && next.lampOpensWizard == status_.lampOpensWizard
                   && message == message_;

    status_    = next;
    message_   = message;
    hasStatus_ = true;

    // The pulse is the one thing that moves on its own; it repaints only the
    // lamp, every push, while it lasts.
    if (same)
    {
        if (status_.pulse)
            repaint (lampArea_);
        return;
    }

    applyStatus();
    repaint();
}

void DumbView::applyStatus()
{
    leftButton.setButtonText (utf8 (status_.left == DumbStatus::Left::Stop  ? kStop
                                  : status_.left == DumbStatus::Left::Apply ? kApply
                                                                            : kMeasure));
    middleButton.setButtonText (utf8 (status_.middle == DumbStatus::Middle::Dismiss ? kDismiss
                                                                                    : kEnable));
    offButton.setButtonText (utf8 (kOff));

    leftButton  .setEnabled (status_.leftEnabled);
    middleButton.setEnabled (status_.middleEnabled);
    offButton   .setEnabled (status_.offEnabled);

    middleButton.setColour (juce::TextButton::buttonOnColourId, az::theme::ok);
    offButton   .setColour (juce::TextButton::buttonOnColourId, az::theme::warn);
    middleButton.setToggleState (status_.middleOn, juce::dontSendNotification);
    offButton   .setToggleState (status_.offOn,    juce::dontSendNotification);

    setMouseCursor (status_.lampOpensWizard ? juce::MouseCursor::PointingHandCursor
                                            : juce::MouseCursor::NormalCursor);
}

void DumbView::paint (juce::Graphics& g)
{
    using namespace az::theme;

    g.fillAll (background);

    auto area = lampArea_;
    const auto colour = lampColour (status_.lamp);

    // The lamp. Breathing while a measurement runs: the alpha follows wall
    // time, so it needs no timer of its own -- the owner's push repaints it.
    float alpha = 1.0f;
    if (status_.pulse)
    {
        const double t = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        alpha = 0.55f + 0.45f * (float) (0.5 + 0.5 * std::sin (t * juce::MathConstants<double>::twoPi * 0.8));
    }

    auto dotArea = area.removeFromLeft (kLampDiameter + 2 * gap)
                       .withSizeKeepingCentre (kLampDiameter, kLampDiameter).toFloat();
    g.setColour (colour.withMultipliedAlpha (0.18f * alpha));
    g.fillEllipse (dotArea.expanded (10.0f));
    g.setColour (colour.withMultipliedAlpha (alpha));
    g.fillEllipse (dotArea);

    auto textArea = area.reduced (gap, 0);
    // Sentence case, as written -- never toUpperCase(): JUCE maps case through
    // towupper, which leaves most Vietnamese letters alone outside a
    // Vietnamese locale, and the render read "ĐANG CHặN 3 điểM". A sentence a
    // person says is sentence case anyway (lane M Q10).
    const auto headlineFont = legendFont (kHeadlineSize, true, 0.02f);
    g.setColour (text);
    g.setFont (headlineFont);
    g.drawFittedText (status_.headline,
                      textArea.removeFromTop (juce::roundToInt (kHeadlineSize * 1.25f)),
                      juce::Justification::centredLeft, 1, 0.8f);

    g.setColour (dim);
    g.setFont (baseFont (kSublineSize));
    g.drawFittedText (status_.subline,
                      textArea.removeFromTop (juce::roundToInt (kSublineSize * 1.6f)),
                      juce::Justification::centredLeft, 2, 0.9f);

    if (message_.isNotEmpty())
    {
        g.setColour (warn);
        g.setFont (baseFont (kMessageSize));
        g.drawFittedText (message_, textArea.removeFromTop (juce::roundToInt (kMessageSize * 2.6f)),
                          juce::Justification::topLeft, 2, 0.9f);
    }
}

void DumbView::resized()
{
    using namespace az::theme;

    auto area = getLocalBounds().reduced (3 * gap);

    auto top = area.removeFromTop (touchTarget);
    geekButton.setBounds (top.removeFromRight (kGeekWidth));

    auto switches = area.removeFromBottom (kSwitchHeight);
    const int w = juce::jmin (kSwitchMaxWidth, (switches.getWidth() - 2 * 3 * gap) / 3);
    auto row = switches.withSizeKeepingCentre (3 * w + 2 * 3 * gap, kSwitchHeight);
    leftButton  .setBounds (row.removeFromLeft (w));
    row.removeFromLeft (3 * gap);
    middleButton.setBounds (row.removeFromLeft (w));
    row.removeFromLeft (3 * gap);
    offButton   .setBounds (row.removeFromLeft (w));

    // The lamp block sits centred in what is left, as wide as the switch row.
    const int blockHeight = juce::roundToInt (kHeadlineSize * 1.25f + kSublineSize * 1.6f
                                              + kMessageSize * 2.6f);
    lampArea_ = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), 3 * w + 6 * gap),
                                            juce::jmin (area.getHeight(), blockHeight));
}

void DumbView::mouseUp (const juce::MouseEvent& e)
{
    if (status_.lampOpensWizard && lampArea_.contains (e.getPosition()) && onOpenWizard)
        onOpenWizard();
}

} // namespace gui
