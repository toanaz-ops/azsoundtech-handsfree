#include "gui/theme/AzTheme.h"

#include <BinaryData.h>

#include <cmath>

namespace az::theme
{

namespace
{
// Tracking for the silkscreen legends. juce::Font has no letter-spacing
// property, so the wide DIN look comes from an extra kerning factor -- a
// fraction of the font height added between every pair of glyphs.
constexpr float kLegendTracking = 0.16f;

// A switch that is latched ON sits one value step brighter than an idle one,
// with a brighter edge. Small numbers on purpose: the lamp does the shouting.
constexpr float kOnLift        = 0.035f;
constexpr float kHoverLift     = 0.05f;

// How much of the switch face the lamp's halo bleeds into.
constexpr float kLampGlowSpan  = 22.0f;

// How far a legend may be condensed before it is allowed to ellipsise. A
// tracked uppercase legend that overruns its cell by a few percent should
// squeeze; "CLEAR A..." on the destructive control is not an acceptable
// fallback.
constexpr float kLegendMinScale = 0.8f;

// The component property a caller sets to opt a button out of the default
// switch treatment. Set it with:  button.getProperties().set (azStyleProperty, "danger");
const juce::Identifier azStyleProperty { "azStyle" };
const juce::String     styleDanger     { "danger" };
const juce::String     styleGhost      { "ghost" };
const juce::String     styleSegment    { "segment" };

juce::String styleOf (const juce::Button& b)
{
    return b.getProperties().getWithDefault (azStyleProperty, juce::String()).toString();
}
} // namespace

//==============================================================================
// Fonts.

namespace
{
// Each face is turned into a Typeface ONCE and kept. createSystemTypefaceFor
// parses the whole font file, so doing it per Font would re-parse a 100 kB
// TTF on every drawText.
juce::Typeface::Ptr embedded (const void* data, const int size)
{
    return juce::Typeface::createSystemTypefaceFor (data, (std::size_t) size);
}

const juce::Typeface::Ptr& legendSemiBold()
{
    static const juce::Typeface::Ptr face = embedded (BinaryData::SairaCondensedSemiBold_ttf,
                                                      BinaryData::SairaCondensedSemiBold_ttfSize);
    return face;
}

const juce::Typeface::Ptr& legendBold()
{
    static const juce::Typeface::Ptr face = embedded (BinaryData::SairaCondensedBold_ttf,
                                                      BinaryData::SairaCondensedBold_ttfSize);
    return face;
}

const juce::Typeface::Ptr& bodyRegular()
{
    static const juce::Typeface::Ptr face = embedded (BinaryData::IBMPlexSansRegular_ttf,
                                                      BinaryData::IBMPlexSansRegular_ttfSize);
    return face;
}

const juce::Typeface::Ptr& monoRegular()
{
    static const juce::Typeface::Ptr face = embedded (BinaryData::IBMPlexMonoRegular_ttf,
                                                      BinaryData::IBMPlexMonoRegular_ttfSize);
    return face;
}

const juce::Typeface::Ptr& monoMedium()
{
    static const juce::Typeface::Ptr face = embedded (BinaryData::IBMPlexMonoMedium_ttf,
                                                      BinaryData::IBMPlexMonoMedium_ttfSize);
    return face;
}
} // namespace

juce::Font baseFont (const float height)
{
    return juce::Font (juce::FontOptions (bodyRegular()).withHeight (height));
}

juce::Font monoFont (const float height, const bool medium)
{
    return juce::Font (juce::FontOptions (medium ? monoMedium() : monoRegular())
                           .withHeight (height));
}

juce::Font legendFont (const float height, const bool bold, const float tracking)
{
    auto font = juce::Font (juce::FontOptions (bold ? legendBold() : legendSemiBold())
                                .withHeight (height));
    return font.withExtraKerningFactor (tracking);
}

//==============================================================================
// The ramp: sodium at birth, ice once it has held.

float notchHeat (const double ageMs)
{
    const auto t = (float) juce::jlimit (0.0, 1.0, ageMs / kNotchCoolMs);

    // Smoothstep rather than linear: a fresh notch stays unmistakably hot for
    // its first couple of seconds instead of starting to fade immediately.
    const float eased = t * t * (3.0f - 2.0f * t);
    return 1.0f - eased;
}

juce::Colour notchColour (const double ageMs)
{
    const float heat = notchHeat (ageMs);

    // Two segments through the pale-steel midpoint -- see the header for why a
    // single sodium-to-ice lerp is not usable.
    if (heat >= 0.5f)
        return cooling.interpolatedWith (marker, (heat - 0.5f) * 2.0f);

    return settled.interpolatedWith (cooling, heat * 2.0f);
}

//==============================================================================
// Shared drawing primitives.

void drawEngravedDivider (juce::Graphics& g, const juce::Rectangle<int> band)
{
    // A groove is milled, not drawn on: one dark line, one faint light line
    // under it. Two 1 px rows is the whole trick, and it is what stops the
    // layout reading as a stack of flat boxes.
    const auto y = (float) band.getBottom();

    g.setColour (shade);
    g.fillRect ((float) band.getX(), y - 1.0f, (float) band.getWidth(), 1.0f);

    g.setColour (sheen);
    g.fillRect ((float) band.getX(), y, (float) band.getWidth(), 1.0f);
}

void drawWell (juce::Graphics& g, const juce::Rectangle<float> bounds, const bool focused)
{
    g.setColour (well);
    g.fillRoundedRectangle (bounds, cornerRadius);

    // A hairline of shadow along the top inside edge: the cheap read of
    // "recessed" without paying for a real inner blur on every combo.
    g.setColour (shade.withAlpha (0.6f));
    g.fillRect (bounds.getX() + cornerRadius, bounds.getY() + 1.0f,
                bounds.getWidth() - 2.0f * cornerRadius, 1.0f);

    g.setColour (focused ? accent : border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), cornerRadius, 1.0f);
}

void drawCaption (juce::Graphics& g, const juce::String& caption,
                  const juce::Rectangle<int> bounds, const juce::Colour colour)
{
    g.setColour (colour);
    g.setFont (legendFont (captionFontSize, true, trackingCaption));
    g.drawText (caption.toUpperCase(), bounds, juce::Justification::centredLeft, false);
}

//==============================================================================
// The palette is installed as LookAndFeel COLOURS rather than painted ad hoc:
// components that only call findColour() pick it up without knowing about the
// theme, and the overrides below handle the shapes (switch lamps, engraved
// wells, hardware toggles) that colour ids cannot express.

AzLookAndFeel::AzLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, background);

    // A switch face is `raise`; the lamp colour rides on buttonOnColourId so a
    // component can say what its ON state MEANS (green = protecting) without
    // knowing how the switch is drawn.
    setColour (juce::TextButton::buttonColourId,    raise);
    setColour (juce::TextButton::buttonOnColourId,  accent);
    setColour (juce::TextButton::textColourOffId,   dim);
    setColour (juce::TextButton::textColourOnId,    text);

    setColour (juce::Label::textColourId,           text);

    setColour (juce::ComboBox::backgroundColourId,  well);
    setColour (juce::ComboBox::outlineColourId,     border);
    setColour (juce::ComboBox::textColourId,        text);
    setColour (juce::ComboBox::arrowColourId,       faded);
    setColour (juce::ComboBox::buttonColourId,      well);
    setColour (juce::ComboBox::focusedOutlineColourId, accent);

    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId,       text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, raise);
    setColour (juce::PopupMenu::highlightedTextColourId,       accent);

    setColour (juce::ToggleButton::textColourId,    text);
    setColour (juce::ToggleButton::tickColourId,    accent);

    setColour (juce::ScrollBar::thumbColourId,      border);
    setColour (juce::ScrollBar::trackColourId,      background);

    setColour (juce::TooltipWindow::backgroundColourId, panel);
    setColour (juce::TooltipWindow::textColourId,       text);
    setColour (juce::TooltipWindow::outlineColourId,    border);

    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId,       text);
    setColour (juce::AlertWindow::outlineColourId,    border);
}

//==============================================================================

void AzLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                          const juce::Colour& backgroundColour,
                                          const bool shouldDrawButtonAsHighlighted,
                                          const bool shouldDrawButtonAsDown)
{
    // Non-const: the lamp branch below carves its bar off the top with
    // removeFromTop, which mutates.
    auto       bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto style  = styleOf (button);
    const bool on     = button.getToggleState();

    // Destructive: never a filled slab. A CLEAR ALL that looks like every
    // other switch is a CLEAR ALL somebody eventually hits by accident, so it
    // is drawn as an outline that only fills once the pointer is on it.
    if (style == styleDanger)
    {
        if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
        {
            g.setColour (danger.withAlpha (shouldDrawButtonAsDown ? 0.22f : 0.13f));
            g.fillRoundedRectangle (bounds, cornerRadius);
        }

        g.setColour (danger.withAlpha (shouldDrawButtonAsHighlighted ? 1.0f : 0.45f));
        g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);
        return;
    }

    // Segment: one option inside a SegmentedControl. It paints a FILL and
    // nothing else -- the box around the group and the dividers between the
    // options belong to the group, which draws them itself. A per-button
    // border here is exactly what made the toolbar read as several separate
    // controls instead of one choice.
    if (style == styleSegment)
    {
        if (on)
        {
            g.setColour (raise);
            g.fillRect (button.getLocalBounds());
        }
        else if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
        {
            g.setColour (raise.withAlpha (0.45f));
            g.fillRect (button.getLocalBounds());
        }
        return;
    }

    // Ghost: the small chips in a toolbar. Flat, no lamp -- they are display
    // options, not transport, and must not compete with the transport.
    //
    // A ghost chip that is ON takes its edge from buttonOnColourId, so a chip
    // whose state HAS a colour elsewhere on screen (peak hold, whose trace is
    // drawn in `peak`) can say so, while a plain segmented chip just brightens.
    if (style == styleGhost)
    {
        if (on || shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
        {
            g.setColour (on ? raise : raise.withAlpha (0.55f));
            g.fillRoundedRectangle (bounds, cornerRadius);
        }

        const auto edge = on ? backgroundColour.withAlpha (0.75f) : border;
        g.setColour (edge);
        g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);
        return;
    }

    // The default: a latching switch. Softer corner than a field -- see the
    // two radii in the header.
    auto face = raise;
    if (on)                                 face = face.brighter (kOnLift);
    if (shouldDrawButtonAsHighlighted)      face = face.brighter (kHoverLift);
    if (shouldDrawButtonAsDown)             face = face.darker   (0.10f);

    g.setColour (face);
    g.fillRoundedRectangle (bounds, switchRadius);

    g.setColour (on ? border.brighter (0.35f) : border);
    g.drawRoundedRectangle (bounds, switchRadius, 1.0f);

    if (! on)
        return;

    // The lamp: a lit bar across the top edge, and the light it spills onto
    // the face below it. backgroundColour is what JUCE resolved from
    // buttonOnColourId, so the caller's meaning arrives here for free.
    const auto lamp = backgroundColour;
    const auto top  = bounds.removeFromTop ((float) lampHeight);

    juce::Path bar;
    bar.addRoundedRectangle (top.getX(), top.getY(), top.getWidth(), top.getHeight(),
                             switchRadius, switchRadius, true, true, false, false);
    g.setColour (lamp);
    g.fillPath (bar);

    juce::ColourGradient spill (lamp.withAlpha (0.22f), top.getCentreX(), top.getBottom(),
                                lamp.withAlpha (0.0f),  top.getCentreX(), top.getBottom() + kLampGlowSpan,
                                false);
    g.setGradientFill (spill);
    g.fillRect (top.getX(), top.getBottom(), top.getWidth(), kLampGlowSpan);
}

void AzLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                    const bool shouldDrawButtonAsHighlighted, bool)
{
    const auto style = styleOf (button);
    const bool on    = button.getToggleState();

    auto colour = style == styleDanger
                      ? danger.withAlpha (shouldDrawButtonAsHighlighted ? 1.0f : 0.8f)
                      : button.findColour (on ? juce::TextButton::textColourOnId
                                              : juce::TextButton::textColourOffId);

    if (! button.isEnabled())
        colour = colour.withAlpha (0.4f);

    // Segments and chips are set in the NUMERIC face, in SENTENCE CASE, and
    // untracked: they sit directly beside the data they modify and must not
    // shout over it. Only switches and the destructive control are uppercase
    // tracked legends.
    const auto buttonStyle = styleOf (button);

    if (buttonStyle == styleSegment || buttonStyle == styleGhost)
    {
        g.setColour (button.findColour (button.getToggleState()
                                            ? juce::TextButton::textColourOnId
                                            : juce::TextButton::textColourOffId)
                         .withAlpha (button.isEnabled() ? 1.0f : 0.4f));
        g.setFont (getTextButtonFont (button, button.getHeight()));
        g.drawFittedText (button.getButtonText(), button.getLocalBounds(),
                          juce::Justification::centred, 1, 0.9f);
        return;
    }

    const auto hint = button.getProperties()
                            .getWithDefault (hintProperty, juce::String()).toString();

    auto area = button.getLocalBounds().reduced (gap, spacing);

    // A switch with a hint splits its face: legend on top, hint beneath. The
    // legend keeps the optical centre, so the pair does not read as having
    // slid upward -- the hint band is carved off the bottom AFTER the legend
    // has been given the middle.
    juce::Rectangle<int> hintArea;
    if (hint.isNotEmpty() && area.getHeight() >= 40)
        hintArea = area.removeFromBottom (14);

    g.setColour (colour);
    g.setFont (getTextButtonFont (button, button.getHeight()));

    // Legends are silkscreen: uppercase, tracked. The tracking already comes
    // from the font, so the only job here is the case and the fit.
    g.drawFittedText (button.getButtonText().toUpperCase(), area,
                      juce::Justification::centred, 1, kLegendMinScale);

    if (hintArea.isEmpty())
        return;

    g.setColour (faded.withAlpha (button.isEnabled() ? 1.0f : 0.4f));
    g.setFont (monoFont (10.0f));
    g.drawFittedText (hint, hintArea, juce::Justification::centred, 1, 0.8f);
}

juce::Font AzLookAndFeel::getTextButtonFont (juce::TextButton& button, const int buttonHeight)
{
    // Every size and tracking below is transcribed from the design study --
    // docs/spec-ui-mockup.md section 1. They are NOT one scale: a segment is
    // set in the numeric face and a switch in the tracked legend face, and
    // using one for both is what made the toolbar shout over the plot.
    const auto style = styleOf (button);

    if (style == styleSegment)
        return monoFont (segmentFontSize);

    if (style == styleGhost)
        return monoFont (chipFontSize);

    if (style == styleDanger)
        return legendFont (dangerFontSize, true, trackingSwitch);

    // A transport switch, or a smaller default button in a strip. The 40 px
    // split is the boundary between "a switch" and "a control in a row".
    return buttonHeight >= 40 ? legendFont (switchFontSize, true, trackingSwitch)
                              : legendFont (columnFontSize, true, trackingColumn);
}

//==============================================================================

juce::Font AzLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    // Combo contents are values -- rates, channel names, dB. Mono so a column
    // of them lines up and never jitters as the selection changes.
    return monoFont (baseFontSize - 1.0f);
}

void AzLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    // The label is given a ONE-LINE height, centred, not the combo's full
    // height. juce::Label fits as many lines as its height allows, so a 26 px
    // combo holding a 13 px face wrapped "Analogue 1" onto two lines and
    // showed "Analogue" over "1". A value that wraps is a value you cannot
    // read at a glance, which is the entire job of this control.
    const auto font = getComboBoxFont (box);
    const int  lineHeight = juce::roundToInt (font.getHeight()) + 2;

    label.setBounds (gap, (box.getHeight() - lineHeight) / 2,
                     juce::jmax (1, box.getWidth() - gap - (int) (box.getHeight() * 0.6f)),
                     lineHeight);
    label.setFont (font);
    label.setMinimumHorizontalScale (0.85f);
}

void AzLookAndFeel::drawComboBox (juce::Graphics& g, const int width, const int height,
                                  const bool isButtonDown, const int buttonX, int,
                                  const int buttonW, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);

    drawWell (g, bounds, isButtonDown || box.hasKeyboardFocus (false));

    // A small caret, centred in the button area -- no JUCE default chrome.
    juce::Path caret;
    const float cx = (float) buttonX + ((float) buttonW * 0.5f);
    const float cy = (float) height * 0.5f;
    const float r  = 3.5f;
    caret.addTriangle (cx - r, cy - r * 0.55f, cx + r, cy - r * 0.55f, cx, cy + r * 0.65f);

    g.setColour (box.findColour (box.isEnabled() ? juce::ComboBox::arrowColourId
                                                 : juce::ComboBox::arrowColourId)
                     .withAlpha (box.isEnabled() ? 1.0f : 0.35f));
    g.fillPath (caret);
}

//==============================================================================

void AzLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                      const bool shouldDrawButtonAsHighlighted, bool)
{
    // A hardware toggle: a recessed track with a knob that travels. A JUCE
    // tick box is 18 px of ambiguity at arm's length; this reads as on/off
    // from its SHAPE, which is what survives a glance in a dark room.
    constexpr float trackW = 32.0f;
    constexpr float trackH = 16.0f;
    constexpr float inset  = 2.0f;

    const bool on = button.getToggleState();
    const auto track = juce::Rectangle<float> (trackW, trackH)
                           .withCentre ({ (float) inset + trackW * 0.5f,
                                          (float) button.getHeight() * 0.5f });

    g.setColour (on ? accent.withAlpha (0.28f) : well);
    g.fillRoundedRectangle (track, trackH * 0.5f);

    g.setColour (on ? accent.withAlpha (0.8f)
                    : (shouldDrawButtonAsHighlighted ? border.brighter (0.3f) : border));
    g.drawRoundedRectangle (track.reduced (0.5f), trackH * 0.5f, 1.0f);

    const float knobD = trackH - 2.0f * inset;
    const float knobX = on ? track.getRight() - inset - knobD : track.getX() + inset;

    g.setColour (on ? accent : dim);
    g.fillEllipse (knobX, track.getY() + inset, knobD, knobD);

    if (button.getButtonText().isNotEmpty())
    {
        g.setColour (button.findColour (juce::ToggleButton::textColourId)
                         .withAlpha (button.isEnabled() ? 1.0f : 0.4f));
        g.setFont (baseFont());
        g.drawText (button.getButtonText(),
                    button.getLocalBounds().withTrimmedLeft ((int) (track.getRight() + gap)),
                    juce::Justification::centredLeft, false);
    }
}

//==============================================================================

void AzLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    g.setColour (label.findColour (juce::Label::textColourId)
                     .withAlpha (label.isEnabled() ? 1.0f : 0.4f));
    g.setFont (getLabelFont (label));

    auto textArea = label.getBorderSize().subtractedFrom (label.getLocalBounds());
    g.drawFittedText (label.getText(), textArea,
                      label.getJustificationType(),
                      juce::jmax (1, (int) ((float) textArea.getHeight() / label.getFont().getHeight())),
                      label.getMinimumHorizontalScale());
}

juce::Font AzLookAndFeel::getLabelFont (juce::Label& label)
{
    // A label that a component has already given a font keeps it -- panels set
    // mono on their numeric readouts and legend on their captions, and this
    // must not overwrite that decision.
    return label.getFont();
}

//==============================================================================

void AzLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, const int width, const int height)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);

    g.setColour (panel);
    g.fillRoundedRectangle (bounds, cornerRadius);
    g.setColour (border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), cornerRadius, 1.0f);
}

juce::Font AzLookAndFeel::getPopupMenuFont()
{
    return monoFont (baseFontSize - 1.0f);
}

//==============================================================================

void AzLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&,
                                   const int x, const int y, const int width, const int height,
                                   const bool isScrollbarVertical,
                                   const int thumbStartPosition, const int thumbSize,
                                   const bool isMouseOver, const bool isMouseDown)
{
    if (thumbSize <= 0)
        return;

    auto thumb = isScrollbarVertical
                     ? juce::Rectangle<int> (x, thumbStartPosition, width, thumbSize).reduced (3, 1)
                     : juce::Rectangle<int> (thumbStartPosition, y, thumbSize, height).reduced (1, 3);

    g.setColour (isMouseDown ? accent.withAlpha (0.7f)
                             : (isMouseOver ? border.brighter (0.5f) : border));
    g.fillRoundedRectangle (thumb.toFloat(), thumb.toFloat().getWidth() * 0.5f);
}

} // namespace az::theme
