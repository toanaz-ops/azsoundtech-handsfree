#include "gui/SegmentedControl.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

namespace
{
// The style tag the LookAndFeel reads: a segment paints its own fill when
// selected and NOTHING otherwise -- no border, no radius. The box and the
// dividers belong to the group, and are painted here.
const juce::String kSegmentStyle { "segment" };

// Distinct per instance so two groups in one toolbar do not share exclusivity.
int nextRadioGroupId()
{
    static int next = 900;
    return ++next;
}
} // namespace

SegmentedControl::SegmentedControl (const juce::StringArray& labels)
{
    const int groupId = nextRadioGroupId();

    for (int i = 0; i < labels.size(); ++i)
    {
        auto button = std::make_unique<juce::TextButton> (labels[i]);

        button->setClickingTogglesState (true);
        button->setRadioGroupId (groupId);
        button->getProperties().set ("azStyle", kSegmentStyle);
        button->setWantsKeyboardFocus (false);

        auto* raw = button.get();
        button->onClick = [this, raw, i]
        {
            // The radio group turns the PREVIOUS segment off with a
            // notification, which would otherwise report a selection the user
            // did not make. Same guard ModeRail's mode handlers use.
            if (! raw->getToggleState())
                return;

            selected_ = i;

            if (onSelected != nullptr)
                onSelected (i);
        };

        addAndMakeVisible (*button);
        buttons_.push_back (std::move (button));
    }

    reflect();
}

void SegmentedControl::setSelectedIndex (const int index)
{
    // -1 clears the group. A segmented control that EXTENDS into another
    // control (the analyser's long-average combo) has to be able to show that
    // the choice currently lives over there, rather than lighting a segment
    // that is not what the plot is doing.
    if (index != -1 && ! juce::isPositiveAndBelow (index, (int) buttons_.size()))
        return;

    selected_ = index;
    reflect();
}

void SegmentedControl::reflect()
{
    for (std::size_t i = 0; i < buttons_.size(); ++i)
        buttons_[i]->setToggleState ((int) i == selected_, juce::dontSendNotification);
}

int SegmentedControl::getPreferredWidth() const
{
    if (buttons_.empty())
        return 0;

    // Every segment gets the width of the WIDEST label, so the dividers sit on
    // a regular rhythm. Measured through GlyphArrangement because juce::Font
    // lost getStringWidth in this version.
    const auto font = az::theme::monoFont (az::theme::segmentFontSize);

    float widest = 0.0f;
    for (const auto& button : buttons_)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (font, button->getButtonText(), 0.0f, 0.0f);
        widest = juce::jmax (widest, glyphs.getBoundingBox (0, -1, true).getWidth());
    }

    const int segment = juce::roundToInt (widest) + 2 * kSegmentPadding;
    return segment * (int) buttons_.size();
}

void SegmentedControl::paint (juce::Graphics& g)
{
    using namespace az::theme;

    // The group's own box. Drawn UNDER the buttons so a selected segment's
    // fill meets the border without a seam.
    g.setColour (border);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), cornerRadius, 1.0f);
}

void SegmentedControl::paintOverChildren (juce::Graphics& g)
{
    using namespace az::theme;

    // The dividers go OVER the buttons: a selected segment fills its whole
    // rect, and a divider drawn underneath would be painted out by the
    // segment next to it.
    g.setColour (border);

    for (std::size_t i = 1; i < buttons_.size(); ++i)
    {
        const float x = (float) buttons_[i]->getX() - 0.5f;
        g.drawLine (x, 1.0f, x, (float) getHeight() - 1.0f, 1.0f);
    }

    // The selection's edge, drawn LAST so no divider and no neighbouring
    // segment can clip it. Drawn here rather than by the button's own
    // LookAndFeel for exactly that reason -- there it lost whichever side a
    // divider landed on.
    if (juce::isPositiveAndBelow (selected_, (int) buttons_.size()))
    {
        auto edge = buttons_[(std::size_t) selected_]->getBounds().toFloat();

        // Nudged inside the group's own border on the outer segments, so the
        // accent sits beside the frame rather than on top of it.
        edge = edge.getUnion (edge).reduced (0.5f, 0.5f);

        g.setColour (accent.withAlpha (0.75f));
        g.drawRect (edge, 1.0f);
    }
}

void SegmentedControl::resized()
{
    if (buttons_.empty())
        return;

    // Divided from the LEFT with the remainder handed to the last segment, so
    // the group always fills its rect exactly -- a rounding gap on the right
    // would show as a bright sliver inside the border.
    auto area = getLocalBounds();
    segmentWidth_ = area.getWidth() / (int) buttons_.size();

    for (std::size_t i = 0; i < buttons_.size(); ++i)
        buttons_[i]->setBounds (i + 1 == buttons_.size()
                                    ? area
                                    : area.removeFromLeft (segmentWidth_));
}

} // namespace gui
