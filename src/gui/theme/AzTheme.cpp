#include "gui/theme/AzTheme.h"

namespace az::theme
{

juce::Font baseFont()
{
    return juce::Font (juce::FontOptions ("Segoe UI", baseFontSize, juce::Font::plain));
}

juce::Font monoFont (float height)
{
    return juce::Font (juce::FontOptions ("Consolas", height, juce::Font::plain)
                           .withFallbacks ({ "Cascadia Mono", "Courier New" }));
}

//==============================================================================
// The palette is installed as LookAndFeel COLOURS rather than painted ad hoc:
// components that only call findColour() pick it up without knowing about the
// theme, and the overrides below handle the shapes (rounded panels, hairline
// borders) that colour ids cannot express.

AzLookAndFeel::AzLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, background);

    setColour (juce::TextButton::buttonColourId,    panel);
    setColour (juce::TextButton::buttonOnColourId,  accent);
    setColour (juce::TextButton::textColourOffId,   text);
    setColour (juce::TextButton::textColourOnId,    background);

    setColour (juce::Label::textColourId,           text);

    setColour (juce::ComboBox::backgroundColourId,  panel);
    setColour (juce::ComboBox::outlineColourId,     border);
    setColour (juce::ComboBox::textColourId,        text);
    setColour (juce::ComboBox::arrowColourId,       dim);
    setColour (juce::ComboBox::buttonColourId,      panel);
    setColour (juce::ComboBox::focusedOutlineColourId, accent);

    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId,       text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
    setColour (juce::PopupMenu::highlightedTextColourId,       background);
}

void AzLookAndFeel::applyTo (juce::Component* component)
{
    if (component != nullptr)
        component->setLookAndFeel (this);
}

void AzLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                          const juce::Colour& /*backgroundColour*/,
                                          bool shouldDrawButtonAsHighlighted,
                                          bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);

    // Toggled = the active mode: filled with the accent so it reads at a
    // glance on a dark stage. Idle = a flat cell with a hairline border.
    const bool toggled = button.getToggleState();
    auto fill = toggled ? accent : panel;

    if (! toggled && (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown))
        fill = fill.brighter (0.06f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, cornerRadius);

    if (! toggled)
    {
        g.setColour (border);
        g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);
    }
}

juce::Font AzLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    // Buttons stay at the theme's base size; mono is reserved for numbers.
    juce::ignoreUnused (buttonHeight);
    return baseFont();
}

void AzLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                                  int buttonX, int /*buttonY*/, int buttonW, int /*buttonH*/,
                                  juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (1.0f);

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, cornerRadius);

    g.setColour (box.findColour (isButtonDown ? juce::ComboBox::focusedOutlineColourId
                                              : juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);

    // A small dim arrow, centred in the button area -- no JUCE default chrome.
    juce::Path arrow;
    const float cx = (float) buttonX + ((float) buttonW * 0.5f);
    const float cy = (float) height * 0.5f;
    const float r  = 4.0f;
    arrow.addTriangle (cx - r, cy - r * 0.5f, cx + r, cy - r * 0.5f, cx, cy + r * 0.5f);

    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.fillPath (arrow);
}

void AzLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    g.setColour (label.findColour (juce::Label::textColourId));
    g.setFont (getLabelFont (label));

    auto textArea = label.getBorderSize().subtractedFrom (label.getLocalBounds());
    g.drawFittedText (label.getText(), textArea,
                      label.getJustificationType(),
                      juce::jmax (1, (int) ((float) textArea.getHeight() / label.getFont().getHeight())),
                      label.getMinimumHorizontalScale());
}

juce::Font AzLookAndFeel::getLabelFont (juce::Label&)
{
    return baseFont();
}

//==============================================================================
// Function-local static: constructed on first use, destroyed after main()
// returns. A LookAndFeel is not a Component, so JUCE's leak detector has
// nothing to say about it.

void AzTheme::applyTo (juce::Component* component)
{
    static AzLookAndFeel sharedLookAndFeel;

    if (component != nullptr)
        component->setLookAndFeel (&sharedLookAndFeel);
}

} // namespace az::theme
