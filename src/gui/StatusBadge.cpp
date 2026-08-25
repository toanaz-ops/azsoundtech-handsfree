#include "gui/StatusBadge.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

namespace
{
// The indicator. 9 px is big enough to carry a halo that reads at six feet
// and small enough that the legend beside it stays the thing you read.
constexpr float kLedSize   = 9.0f;
constexpr float kLedInset  = 10.0f;
constexpr float kGlowAlpha = 0.85f;
} // namespace

StatusBadge::StatusBadge()
{
    setInterceptsMouseClicks (false, false);
}

void StatusBadge::setState (const ProtectionState newState)
{
    if (state == newState)
        return;

    state = newState;
    repaint();
}

juce::Colour StatusBadge::stateColour() const
{
    using namespace az::theme;

    switch (state)
    {
        case ProtectionState::Protecting: return ok;
        case ProtectionState::Bypassed:   return danger;
        case ProtectionState::Idle:       break;
    }
    return dim;
}

juce::String StatusBadge::stateLabel() const
{
    switch (state)
    {
        case ProtectionState::Protecting: return "PROTECTING";
        case ProtectionState::Bypassed:   return "BYPASSED";
        case ProtectionState::Idle:       break;
    }
    return "IDLE";
}

void StatusBadge::resized()
{
    // See the header: the path is built HERE so melatonin's cached blur
    // survives from frame to frame.
    ledPath_.clear();
    ledPath_.addEllipse (kLedInset,
                         (float) getHeight() * 0.5f - kLedSize * 0.5f,
                         kLedSize, kLedSize);
}

void StatusBadge::paint (juce::Graphics& g)
{
    using namespace az::theme;

    const auto colour = stateColour();
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    // The bezel: a recessed well, so the LED reads as SET INTO the panel.
    g.setColour (well);
    g.fillRoundedRectangle (bounds, cornerRadius);
    g.setColour (border);
    g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);

    // The halo, then the LED on top of it. An IDLE badge gets no halo at all:
    // a grey glow is just a smudge, and "nothing is happening" should not draw
    // the eye the way a live state does.
    if (state != ProtectionState::Idle)
    {
        ledGlow_.setColor (colour.withAlpha (kGlowAlpha));
        ledGlow_.render (g, ledPath_);
    }

    g.setColour (colour);
    g.fillPath (ledPath_);

    g.setColour (colour);
    g.setFont (legendFont (11.5f));
    g.drawText (stateLabel(),
                getLocalBounds().withTrimmedLeft ((int) (kLedInset + kLedSize) + gap),
                juce::Justification::centredLeft, false);
}

} // namespace gui
