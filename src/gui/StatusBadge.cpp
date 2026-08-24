#include "gui/StatusBadge.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

StatusBadge::StatusBadge()
    : monoFont_ (az::theme::monoFont())
{
}

void StatusBadge::setState (ProtectionState newState)
{
    if (state == newState)
        return;

    state = newState;
    repaint();
}

void StatusBadge::paint (juce::Graphics& g)
{
    using namespace az::theme;

    auto colour = dim;
    juce::String label = "IDLE";

    switch (state)
    {
        case ProtectionState::Protecting: colour = ok;   label = "PROTECTING"; break;
        case ProtectionState::Idle:       colour = dim;  label = "IDLE";       break;
        case ProtectionState::Bypassed:   colour = warn; label = "BYPASSED";   break;
    }

    constexpr float dotSize = 8.0f;

    g.setColour (colour);
    g.fillEllipse ((float) getLocalBounds().getX(),
                   (float) getHeight() * 0.5f - dotSize * 0.5f,
                   dotSize, dotSize);

    g.setFont (monoFont_);
    g.drawText (label,
                getLocalBounds().toFloat().withTrimmedLeft (dotSize + spacing),
                juce::Justification::centredLeft);
}

} // namespace gui
