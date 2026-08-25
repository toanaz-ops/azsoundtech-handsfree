#include "gui/SlotTabs.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

namespace
{
// Any non-zero id makes the chips mutually exclusive.
constexpr int kSlotRadioGroupId = 21;
} // namespace

SlotTabs::SlotTabs()
{
    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& chip = chips_[(std::size_t) i];

        // Zero-padded so slot 8 and a future slot 10 occupy the same width and
        // the strip does not shuffle as slots are revealed.
        chip.setButtonText (juce::String (i + 1).paddedLeft ('0', 2));
        chip.setClickingTogglesState (true);
        chip.setRadioGroupId (kSlotRadioGroupId);
        chip.getProperties().set ("azStyle", "ghost");
        chip.setWantsKeyboardFocus (false);

        chip.onClick = [this, i]
        {
            // The radio group turns the PREVIOUS chip off with a notification,
            // which would otherwise re-fire that chip's handler for a slot the
            // user did not pick. Same guard ModeRail's mode handlers use.
            if (! chips_[(std::size_t) i].getToggleState())
                return;

            selected_ = i;

            if (onSlotSelected != nullptr)
                onSlotSelected (i);
        };

        addAndMakeVisible (chip);
    }

    reflect();
}

void SlotTabs::setSlotCount (const int count)
{
    const int clamped = juce::jlimit (1, kMaxSlots, count);

    if (clamped == count_)
        return;

    count_ = clamped;

    // A selection that just became unreachable falls back to slot 0 rather
    // than pointing the display at a slot with no row in the routing table.
    if (selected_ >= count_)
        selected_ = 0;

    reflect();
    resized();
}

void SlotTabs::setSelected (const int slotIndex)
{
    if (! juce::isPositiveAndBelow (slotIndex, count_))
        return;

    selected_ = slotIndex;
    reflect();
}

void SlotTabs::reflect()
{
    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& chip = chips_[(std::size_t) i];

        // Set explicitly rather than leaning on the radio group: the group's
        // own clearing runs through the notification path this avoids.
        chip.setToggleState (i == selected_, juce::dontSendNotification);
        chip.setVisible (i < count_);
    }
}

int SlotTabs::getPreferredWidth() const
{
    return kLegendWidth + count_ * kChipWidth;
}

void SlotTabs::paint (juce::Graphics& g)
{
    az::theme::drawCaption (g, "Monitor",
                            getLocalBounds().withWidth (kLegendWidth),
                            az::theme::faded);
}

void SlotTabs::resized()
{
    auto area = getLocalBounds();
    area.removeFromLeft (kLegendWidth);

    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& chip = chips_[(std::size_t) i];

        // Hidden chips get an EMPTY rect, not a stale one: a chip past the
        // visible count must not be reachable by a click that lands on where
        // it used to be.
        chip.setBounds (i < count_
                            ? area.removeFromLeft (kChipWidth).withTrimmedRight (1)
                            : juce::Rectangle<int>());
    }
}

} // namespace gui
