#include "gui/SlotPanel.h"

#include "gui/theme/AzTheme.h"

namespace gui
{

namespace
{
constexpr int kMonoItemId   = 1;
constexpr int kStereoItemId = 2;

// Column metrics, shared by the caption row and the 8 data rows so the two
// can never drift apart.
constexpr int kNumberColumn = 24;
constexpr int kEnableColumn = 40;
constexpr int kWidthColumn  = 84;
constexpr int kLaneColumn   = 96;
constexpr int kLedColumn    = 28;

// One channel combo = the device's names, item id = channel index + 1.
void fillChannelCombo (juce::ComboBox& box, const juce::StringArray& names, int selectedChannel)
{
    box.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        box.addItem (names[i], i + 1);

    if (juce::isPositiveAndBelow (selectedChannel, names.size()))
        box.setSelectedId (selectedChannel + 1, juce::dontSendNotification);
}
} // namespace

//==============================================================================
// Row LED.

void SlotPanel::Row::Led::paint (juce::Graphics& g)
{
    using namespace az::theme;

    auto area = getLocalBounds().toFloat().reduced (6.0f);

    g.setColour (on ? ok : dim);
    g.fillEllipse (area);

    g.setColour (border);
    g.drawEllipse (area, 1.0f);
}

//==============================================================================
// SlotPanel.

SlotPanel::SlotPanel (AudioEngine& engine)
    : engine_ (engine)
{
    using namespace az::theme;

    for (auto* label : { &widthCaption_, &inACaption_, &inBCaption_,
                         &outACaption_, &outBCaption_, &ledCaption_ })
        addAndMakeVisible (*label);

    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& row = rows_[(std::size_t) i];

        row.number.setText (juce::String (i + 1), juce::dontSendNotification);
        row.number.setFont (monoFont());
        addAndMakeVisible (row.number);

        // ClickingTogglesState like every other boolean button in this GUI.
        row.enable.setClickingTogglesState (true);
        addAndMakeVisible (row.enable);

        row.width.addItem ("Mono",   kMonoItemId);
        row.width.addItem ("Stereo", kStereoItemId);
        addAndMakeVisible (row.width);

        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            addAndMakeVisible (row.inLanes[lane]);
            addAndMakeVisible (row.outLanes[lane]);
        }

        addAndMakeVisible (row.led);

        // One handler per row: any control change reports the whole config,
        // exactly as DevicePanel funnels every combo through one restart path.
        // The toggle uses onClick -- ToggleButton has no onChange hook.
        const auto onChange = [this, i] { handleRowChanged (i); };

        row.enable.onClick  = onChange;
        row.width.onChange  = onChange;

        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            row.inLanes[lane].onChange  = onChange;
            row.outLanes[lane].onChange = onChange;
        }
    }

    refresh();
}

juce::StringArray SlotPanel::inputChannelNames() const
{
    return inputChannelNamesProvider != nullptr
               ? inputChannelNamesProvider()
               : engine_.getInputChannelNames();
}

juce::StringArray SlotPanel::outputChannelNames() const
{
    return outputChannelNamesProvider != nullptr
               ? outputChannelNamesProvider()
               : engine_.getOutputChannelNames();
}

SlotConfig SlotPanel::currentConfig (int slotIndex) const
{
    return slotConfigProvider != nullptr
               ? slotConfigProvider (slotIndex)
               : engine_.getSlotConfig (slotIndex);
}

void SlotPanel::refresh()
{
    const auto ins  = inputChannelNames();
    const auto outs = outputChannelNames();

    // Both empty with no device open: the honest display is EMPTY and
    // DISABLED, never a guessed channel list (see the header comment).
    const bool haveChannels = ! ins.isEmpty() && ! outs.isEmpty();

    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& row = rows_[(std::size_t) i];
        const SlotConfig config = currentConfig (i);

        // dontSendNotification throughout: we are REFLECTING state here, and a
        // notification would report it straight back as if the user had typed
        // it -- the same re-entry guard DevicePanel::refresh documents.
        row.enable.setToggleState (config.enabled, juce::dontSendNotification);
        row.width.setSelectedId (config.width == 2 ? kStereoItemId : kMonoItemId,
                                 juce::dontSendNotification);

        fillChannelCombo (row.inLanes[0],  ins, config.inputChannels[0]);
        fillChannelCombo (row.inLanes[1],  ins, config.inputChannels[1]);
        fillChannelCombo (row.outLanes[0], outs, config.outputChannels[0]);
        fillChannelCombo (row.outLanes[1], outs, config.outputChannels[1]);

        row.width.setEnabled (haveChannels);

        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            row.inLanes[lane].setEnabled (haveChannels);
            row.outLanes[lane].setEnabled (haveChannels);
        }

        // Lane-1 combos exist only when the slot is stereo.
        const bool stereo = (config.width == 2);
        row.inLanes[1] .setVisible (stereo);
        row.outLanes[1].setVisible (stereo);

        row.led.on = config.enabled;
        row.led.repaint();
    }

    resized();
}

void SlotPanel::handleRowChanged (int slotIndex)
{
    auto& row = rows_[(std::size_t) slotIndex];

    SlotConfig config = currentConfig (slotIndex);
    config.enabled = row.enable.getToggleState();

    const bool stereo = (row.width.getSelectedId() == kStereoItemId);

    if (stereo && config.width != 2)
    {
        config.width = 2;
        // Lane 1 gets its own channel, not a duplicate of lane 0: a stereo
        // pair routed to the same channel is mono twice. Clamped to what the
        // device actually offers; falls back to lane 0's channel when the
        // list is too short or not open yet.
        const auto ins  = inputChannelNames();
        const auto outs = outputChannelNames();

        if (config.inputChannels[1] == config.inputChannels[0])
            config.inputChannels[1] = juce::jmin (config.inputChannels[0] + 1,
                                                  juce::jmax (ins.size() - 1, 0));
        if (config.outputChannels[1] == config.outputChannels[0])
            config.outputChannels[1] = juce::jmin (config.outputChannels[0] + 1,
                                                   juce::jmax (outs.size() - 1, 0));
    }
    else if (! stereo && config.width != 1)
        config.width = 1;

    // A combo only speaks for a channel when it actually lists channels --
    // an empty/disabled combo (no device) must not overwrite the stored
    // mapping with nothing.
    const auto ins  = inputChannelNames();
    const auto outs = outputChannelNames();

    for (int lane = 0; lane < config.width; ++lane)
    {
        if (! ins.isEmpty())
        {
            const int id = row.inLanes[lane].getSelectedId();
            if (id > 0)
                config.inputChannels[lane] = id - 1;
        }

        if (! outs.isEmpty())
        {
            const int id = row.outLanes[lane].getSelectedId();
            if (id > 0)
                config.outputChannels[lane] = id - 1;
        }
    }

    if (onSlotConfigChanged != nullptr)
        onSlotConfigChanged (slotIndex, config);
}

void SlotPanel::resized()
{
    using namespace az::theme;

    auto area = getLocalBounds().reduced (gap, spacing);

    // Caption row over the columns that exist THIS refresh -- the B captions
    // follow their combos' visibility so nothing floats unanchored.
    auto captions = area.removeFromTop (18);

    widthCaption_.setBounds (captions.removeFromLeft (kNumberColumn + kEnableColumn + kWidthColumn));
    inACaption_ .setBounds (captions.removeFromLeft (kLaneColumn));

    if (rows_[0].inLanes[1].isVisible())
        inBCaption_.setBounds (captions.removeFromLeft (kLaneColumn));
    else
        inBCaption_.setBounds ({});

    outACaption_.setBounds (captions.removeFromLeft (kLaneColumn));

    if (rows_[0].outLanes[1].isVisible())
        outBCaption_.setBounds (captions.removeFromLeft (kLaneColumn));
    else
        outBCaption_.setBounds ({});

    ledCaption_.setBounds (captions.removeFromLeft (kLedColumn));
    ledCaption_.setJustificationType (juce::Justification::centredLeft);

    area.removeFromTop (spacing);

    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto rowArea = area.removeFromTop (kRowHeight);
        auto& row = rows_[(std::size_t) i];

        row.number.setBounds (rowArea.removeFromLeft (kNumberColumn));
        row.number.setJustificationType (juce::Justification::centredRight);

        row.enable.setBounds (rowArea.removeFromLeft (kEnableColumn).reduced (2, 1));
        row.width .setBounds (rowArea.removeFromLeft (kWidthColumn).reduced (2, 1));
        row.inLanes[0].setBounds (rowArea.removeFromLeft (kLaneColumn).reduced (2, 1));

        if (row.inLanes[1].isVisible())
            row.inLanes[1].setBounds (rowArea.removeFromLeft (kLaneColumn).reduced (2, 1));
        else
            row.inLanes[1].setBounds ({});

        row.outLanes[0].setBounds (rowArea.removeFromLeft (kLaneColumn).reduced (2, 1));

        if (row.outLanes[1].isVisible())
            row.outLanes[1].setBounds (rowArea.removeFromLeft (kLaneColumn).reduced (2, 1));
        else
            row.outLanes[1].setBounds ({});

        row.led.setBounds (rowArea.removeFromLeft (kLedColumn));
    }
}

} // namespace gui
