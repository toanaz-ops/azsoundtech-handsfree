#include "gui/SlotPanel.h"

#include "gui/TuningPanel.h"
#include "gui/theme/AzTheme.h"

namespace gui
{

namespace
{
constexpr int kMonoItemId   = 1;
constexpr int kStereoItemId = 2;

constexpr int kGlobalItemId = 1;
constexpr int kCustomItemId = 2;

const char* const kLinkTooltip =
    "LINK: one side rings, both get cut. INDEP: only the ringing side is cut. "
    "Switching does not copy existing notches.";

// Column metrics, shared by the caption row and the 8 data rows so the two
// can never drift apart.
// Narrowed with the 2026-08-25 rebuild. This table used to be a full-width
// band; it now lives in the rig column beside the notch table, and the old
// 604 px of fixed columns overflowed that column at the minimum window size.
// Every value here is the narrowest that still shows a full channel name.
// The slot number and its enable toggle together fill the rig column's shared
// legend gutter, so the table's first CONTROL column starts at the same x as
// every field above it (DEVICE, RATE, RESPONSE, NOTCH, TRIGGER).
constexpr int kNumberColumn = 24;
constexpr int kEnableColumn = az::theme::gutterWidth - kNumberColumn;
constexpr int kWidthColumn  = 84;   // fits "Stereo" whole -- see elideMiddle

// Wide enough for a real channel name. Interfaces report things like
// "Analogue 1", and at 76 px that came back as "Analogue" stacked over "1" --
// the combo's label had room for a second line and took it. The wrapping
// itself is fixed in the LookAndFeel; this is the width that lets the name be
// READ rather than merely fit on one line with an ellipsis.
constexpr int kLaneColumn   = 98;
constexpr int kLedColumn    = 26;
// Between LED and Tune: wide enough for the two-segment LINK/INDEP control at
// its own preferred width without eliding either label.
constexpr int kLinkColumn   = 92;
constexpr int kTuneColumn   = 46;   // a combo needs its caret AND its value

// The section legend, drawn in paint() over the caption row's left gutter --
// the columns there label nothing, so the section name costs no width.
constexpr int kSectionCaptionWidth = az::theme::gutterWidth;

constexpr int kDetailLabelWidth = 46;

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

    // A lit LED and a dark one, not a green dot and a grey dot: the OFF state
    // is an empty ring, so "this slot is doing nothing" reads as absence
    // rather than as another coloured thing to interpret.
    const auto area = getLocalBounds().toFloat().withSizeKeepingCentre (9.0f, 9.0f);

    if (on)
    {
        g.setColour (ok.withAlpha (0.22f));
        g.fillEllipse (area.expanded (3.5f));
        g.setColour (ok);
        g.fillEllipse (area);
    }
    else
    {
        g.setColour (well);
        g.fillEllipse (area);
        g.setColour (border);
        g.drawEllipse (area.reduced (0.5f), 1.0f);
    }
}

//==============================================================================
// SlotPanel.

SlotPanel::SlotPanel (AudioEngine& engine)
    : engine_ (engine)
{
    using namespace az::theme;

    for (auto* label : { &widthCaption_, &inACaption_, &inBCaption_,
                         &outACaption_, &outBCaption_, &ledCaption_,
                         &linkCaption_, &tuneCaption_ })
    {
        // Column captions are silkscreen: tracked uppercase, quiet.
        label->setText (label->getText().toUpperCase(), juce::dontSendNotification);
        label->setFont (legendFont (columnFontSize, true, trackingColumn));
        label->setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (*label);
    }

    // The five custom-tuning combos share TuningPanel's choice lists and id
    // mappings, so a custom value and its global-strip twin are the same
    // vocabulary (riseMsForId etc.).

    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& row = rows_[(std::size_t) i];
        auto& detail = details_[(std::size_t) i];

        // Zero-padded so a one-digit and a two-digit slot number occupy the
        // same width -- the column must not shuffle when the 10th row appears.
        row.number.setText (juce::String (i + 1).paddedLeft ('0', 2),
                            juce::dontSendNotification);
        row.number.setFont (monoFont (baseFontSize - 2.0f));
        row.number.setColour (juce::Label::textColourId, faded);
        addAndMakeVisible (row.number);

        // ClickingTogglesState like every other boolean button in this GUI.
        row.enable.setClickingTogglesState (true);
        addAndMakeVisible (row.enable);

        row.width.addItem ("Mono",   kMonoItemId);
        row.width.addItem ("Stereo", kStereoItemId);
        addAndMakeVisible (row.width);

        row.tune.addItem ("G", kGlobalItemId);
        row.tune.addItem ("C", kCustomItemId);
        row.tune.setSelectedId (kGlobalItemId, juce::dontSendNotification);
        addAndMakeVisible (row.tune);

        for (int b = 1; b <= 6; ++b)
            detail.persist.addItem (juce::String (b), b);
        for (int c = 1; c <= 5; ++c)
        {
            detail.rise .addItem (juce::String (TuningPanel::riseMsForId (c)) + " ms", c);
            detail.q    .addItem (juce::String (TuningPanel::qForId (c)), c);
            detail.thr  .addItem (juce::String (TuningPanel::thresholdForId (c), 1), c);
            if (c <= 4)   // Depth offers four values: -6/-12/-18/-24 dB.
                detail.depth.addItem (juce::String (TuningPanel::depthDbForId (c)) + " dB", c);
        }

        for (auto* label : { &detail.riseLabel, &detail.persistLabel,
                             &detail.depthLabel, &detail.qLabel, &detail.thrLabel })
        {
            label->setText (label->getText().toUpperCase(), juce::dontSendNotification);
            label->setFont (legendFont (legendFontSize - 2.0f));
            label->setColour (juce::Label::textColourId, dim);
            addAndMakeVisible (*label);
        }
        for (auto* box : { &detail.rise, &detail.persist, &detail.depth,
                           &detail.q, &detail.thr })
            addAndMakeVisible (*box);

        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            addAndMakeVisible (row.inLanes[lane]);
            addAndMakeVisible (row.outLanes[lane]);
        }

        addAndMakeVisible (row.led);

        addAndMakeVisible (row.link);
        row.link.setWantsKeyboardFocus (false);
        row.link.getSegmentForTest (0).setTooltip (kLinkTooltip);
        row.link.getSegmentForTest (1).setTooltip (kLinkTooltip);
        row.link.onSelected = [this, i] (int idx)
        {
            if (onSlotLinkChanged != nullptr)
                onSlotLinkChanged (i, idx == 0);
        };

        // One handler per row: any control change reports the whole config,
        // exactly as DevicePanel funnels every combo through one restart path.
        // The toggle uses onClick -- ToggleButton has no onChange hook.
        const auto onChange = [this, i] { handleRowChanged (i); };

        row.enable.onClick  = onChange;
        row.width.onChange  = onChange;
        row.tune.onChange   = [this, i] { handleTuneChanged (i); };

        const auto onDetailChange = [this] { handleDetailChanged(); };
        detail.rise.onChange    = onDetailChange;
        detail.persist.onChange = onDetailChange;
        detail.depth.onChange   = onDetailChange;
        detail.q.onChange       = onDetailChange;
        detail.thr.onChange     = onDetailChange;

        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            row.inLanes[lane].onChange  = onChange;
            row.outLanes[lane].onChange = onChange;
        }
    }

    // Reveals one more row per click. The parent re-runs resized() so the
    // Viewport re-sizes this panel from getPreferredHeight() (same pattern
    // DeviceDrawer::setOpen uses to force the parent's layout pass).
    addButton_.onClick = [this]
    {
        setVisibleRowCount (juce::jmin (visibleRows_ + 1, kMaxSlots));
    };
    addButton_.setButtonText ("+ Add slot");
    // Ghost, not a switch: revealing a row is a table affordance, and a lit
    // lamp on it would claim a state this button does not have.
    addButton_.getProperties().set ("azStyle", "ghost");
    addAndMakeVisible (addButton_);

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

        // Reflect the slot's tuning mode (Global or Custom) without firing:
        // refresh() reports reality, it never invents a change.
        const SlotTuning tuning = slotTuningProvider != nullptr
                                      ? slotTuningProvider (i)
                                      : SlotTuning();
        row.tune.setSelectedId (tuning.usesGlobal ? kGlobalItemId : kCustomItemId,
                                juce::dontSendNotification);

        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            row.inLanes[lane].setEnabled (haveChannels);
            row.outLanes[lane].setEnabled (haveChannels);
        }

        // Lane-1 combos exist only when the slot is stereo.
        const bool stereo = (config.width == 2);
        row.inLanes[1] .setVisible (stereo);
        row.outLanes[1].setVisible (stereo);

        // LINK/INDEP means nothing on a mono row -- there is no "other
        // channel" to link. dontSendNotification: refresh() reflects reality,
        // it never invents a click.
        row.link.setVisible (stereo);
        row.link.setSelectedIndex ((slotLinkedProvider && slotLinkedProvider (i)) ? 0 : 1);

        row.led.on = config.enabled;
        row.led.repaint();
    }

    // The open editor re-seeds too: the receiver may have applied new values
    // to the slot's controller since it opened.
    if (openDetailSlot_ >= 0)
        seedDetailFrom (openDetailSlot_);

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

//==============================================================================
// Per-slot tuning (brief 2026-08-24).

void SlotPanel::seedDetailFrom (int slotIndex)
{
    const SlotTuning t = slotTuningProvider != nullptr
                             ? slotTuningProvider (slotIndex)
                             : SlotTuning();

    auto& d = details_[(std::size_t) slotIndex];

    // dontSendNotification: seeding is reflection -- a notification would
    // report the values straight back as if the user had typed them.
    d.rise  .setSelectedId (TuningPanel::idForRiseMs ((int) t.riseMs),
                            juce::dontSendNotification);
    d.persist.setSelectedId (t.persist, juce::dontSendNotification);
    d.depth .setSelectedId (TuningPanel::idForDepthDb ((int) t.depthDb),
                            juce::dontSendNotification);
    d.q     .setSelectedId (TuningPanel::idForQ ((int) t.q),
                            juce::dontSendNotification);
    d.thr   .setSelectedId (TuningPanel::idForThreshold ((float) t.thr),
                            juce::dontSendNotification);
}

SlotPanel::SlotTuning SlotPanel::currentTuning() const
{
    SlotTuning t;

    if (openDetailSlot_ < 0)
        return t;

    const auto& d = details_[(std::size_t) openDetailSlot_];

    t.usesGlobal = false;
    t.riseMs  = TuningPanel::riseMsForId (d.rise.getSelectedId());
    t.persist = d.persist.getSelectedId();
    t.depthDb = TuningPanel::depthDbForId (d.depth.getSelectedId());
    t.q       = TuningPanel::qForId (d.q.getSelectedId());
    t.thr     = TuningPanel::thresholdForId (d.thr.getSelectedId());
    return t;
}

void SlotPanel::handleDetailChanged()
{
    if (onSlotTuningChanged == nullptr || openDetailSlot_ < 0)
        return;

    onSlotTuningChanged (openDetailSlot_, currentTuning());
}

void SlotPanel::handleTuneChanged (int slotIndex)
{
    if (rows_[(std::size_t) slotIndex].tune.getSelectedId() == kCustomItemId)
        openDetailFor (slotIndex);
    else
        closeDetail();
}

void SlotPanel::openDetailFor (int slotIndex)
{
    // Only ONE editor at a time: opening this slot's detail collapses any
    // other -- but that slot STAYS Custom (its combo keeps showing C and its
    // controller keeps its values); only the editor moved.
    const bool reopening = (openDetailSlot_ == slotIndex);

    openDetailSlot_ = slotIndex;
    seedDetailFrom (slotIndex);

    // First switch to C reports the seeded set immediately: the receiver
    // stores the flag and the values in one atomic snapshot.
    if (! reopening && onSlotTuningChanged != nullptr)
    {
        auto t = currentTuning();
        t.usesGlobal = false;
        onSlotTuningChanged (slotIndex, t);
    }

    notifyPreferredHeightChanged();   // one extra row below the slot's row
}

void SlotPanel::closeDetail()
{
    if (openDetailSlot_ < 0)
        return;

    const int closedSlot = openDetailSlot_;

    // Report Global with the values still showing, so the receiver flips the
    // flag without losing the custom numbers for the next C.
    auto t = currentTuning();
    t.usesGlobal = true;

    openDetailSlot_ = -1;

    if (onSlotTuningChanged != nullptr)
        onSlotTuningChanged (closedSlot, t);

    notifyPreferredHeightChanged();
}

void SlotPanel::notifyPreferredHeightChanged()
{
    if (onPreferredHeightChanged != nullptr)
        onPreferredHeightChanged();
    else if (auto* parent = getParentComponent())
        parent->resized();
    else
        resized();
}

void SlotPanel::setVisibleRowCount (int n)
{
    const auto clamped = juce::jlimit (1, kMaxSlots, n);

    if (clamped == visibleRows_)
        return;

    visibleRows_ = clamped;

    // The owner's layout pass owns this panel's size (it hosts the Viewport),
    // so the height change has to reach IT -- the Viewport parent itself
    // never re-sizes its content.
    if (onPreferredHeightChanged != nullptr)
        onPreferredHeightChanged();
    else if (auto* parent = getParentComponent())
        parent->resized();
    else
        resized();
}

int SlotPanel::getPreferredHeight() const
{
    using namespace az::theme;

    // theme margins (reduced gap/spacing top+bottom) + caption + spacing
    // between caption and rows, then one kRowHeight per visible row plus the
    // Add row while any of the 8 is still hidden -- and one more while a
    // slot's custom-tuning detail row is open below its row.
    const int rows = visibleRows_ + (visibleRows_ < kMaxSlots ? 1 : 0)
                   + (openDetailSlot_ >= 0 ? 1 : 0);

    return 2 * spacing + kCaptionHeight + spacing + rows * kRowHeight;
}

void SlotPanel::paint (juce::Graphics& g)
{
    using namespace az::theme;

    // This panel is a Viewport's content, so it paints its own ground -- the
    // parent's band stops at the Viewport's edge.
    g.fillAll (panel);

    // The section legend rides in the caption row's left gutter: those columns
    // label nothing, so naming the section costs no width.
    const auto captionRow = getLocalBounds().reduced (gap, spacing)
                                            .removeFromTop (kCaptionHeight);
    drawCaption (g, "Routing",
                 captionRow.withWidth (kSectionCaptionWidth), dim);

    g.setColour (border);
    g.fillRect (captionRow.getX(), captionRow.getBottom(), captionRow.getWidth(), 1);

    // One hairline per row boundary, taken from the row's OWN laid-out number
    // cell rather than recomputed here -- so the separators can never drift
    // out of step with resized().
    for (int i = 1; i < visibleRows_; ++i)
    {
        const auto cell = rows_[(std::size_t) i].number.getBounds();
        if (cell.isEmpty())
            continue;

        g.setColour (shade);
        g.fillRect (captionRow.getX(), cell.getY() - 1, captionRow.getWidth(), 1);
    }
}

int SlotPanel::getPreferredWidth() const
{
    using namespace az::theme;

    // Both lane-B columns are ALWAYS reserved (see resized()), so the width is
    // the same whether any slot is stereo or not -- the table must not change
    // width when a slot's width combo moves.
    return 2 * gap + kNumberColumn + kEnableColumn + kWidthColumn
         + 4 * kLaneColumn + kLedColumn + kLinkColumn + kTuneColumn;
}

void SlotPanel::resized()
{
    using namespace az::theme;

    auto area = getLocalBounds().reduced (gap, spacing);

    // FIXED-WIDTH GRID: both lane-B slots are always reserved, so every row
    // and the caption line share one set of columns no matter how many rows
    // are currently mono. A hidden lane-B combo gets an EMPTY rect at its own
    // slot -- nothing downstream shifts left.
    bool anyStereo = false;

    for (const auto& row : rows_)
        anyStereo = anyStereo || row.inLanes[1].isVisible();

    auto captions = area.removeFromTop (kCaptionHeight);

    // The gutter the section legend is painted into (see paint()).
    captions.removeFromLeft (kSectionCaptionWidth);
    widthCaption_.setBounds (captions.removeFromLeft (kWidthColumn));

    const auto inARect  = captions.removeFromLeft (kLaneColumn);
    const auto inBRect  = captions.removeFromLeft (kLaneColumn);
    const auto outARect = captions.removeFromLeft (kLaneColumn);
    const auto outBRect = captions.removeFromLeft (kLaneColumn);

    inACaption_ .setBounds (inARect);
    outACaption_.setBounds (outARect);

    // A caption over a fully-mono table would label nothing.
    inBCaption_ .setBounds (anyStereo ? inBRect  : juce::Rectangle<int>());
    outBCaption_.setBounds (anyStereo ? outBRect : juce::Rectangle<int>());

    ledCaption_.setBounds (captions.removeFromLeft (kLedColumn));
    ledCaption_.setJustificationType (juce::Justification::centredLeft);

    linkCaption_.setBounds (captions.removeFromLeft (kLinkColumn));
    linkCaption_.setJustificationType (juce::Justification::centredLeft);

    tuneCaption_.setBounds (captions.removeFromLeft (kTuneColumn));
    tuneCaption_.setJustificationType (juce::Justification::centredLeft);

    area.removeFromTop (spacing);

    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& row = rows_[(std::size_t) i];

        // Rows past the visible count keep their controls alive (tests drive
        // them directly) but get an empty rect -- nothing painted, no hits.
        if (i >= visibleRows_)
        {
            row.number.setBounds ({});
            row.enable .setBounds ({});
            row.width  .setBounds ({});
            row.tune   .setBounds ({});
            row.inLanes[0].setBounds ({});  row.inLanes[1] .setBounds ({});
            row.outLanes[0].setBounds ({}); row.outLanes[1].setBounds ({});
            row.led.setBounds ({});
            row.link.setBounds ({});

            auto& hiddenDetail = details_[(std::size_t) i];
            for (auto* c : { &hiddenDetail.rise, &hiddenDetail.persist,
                             &hiddenDetail.depth, &hiddenDetail.q,
                             &hiddenDetail.thr })
                c->setBounds ({});
            continue;
        }

        auto rowArea = area.removeFromTop (kRowHeight);

        row.number.setBounds (rowArea.removeFromLeft (kNumberColumn));
        row.number.setJustificationType (juce::Justification::centredRight);

        row.enable.setBounds (rowArea.removeFromLeft (kEnableColumn).reduced (2, 1));
        row.width .setBounds (rowArea.removeFromLeft (kWidthColumn).reduced (2, 1));

        row.inLanes[0].setBounds (rowArea.removeFromLeft (kLaneColumn).reduced (2, 1));

        const auto inB = rowArea.removeFromLeft (kLaneColumn);
        row.inLanes[1].setBounds (row.inLanes[1].isVisible()
                                      ? inB.reduced (2, 1)
                                      : juce::Rectangle<int>());

        row.outLanes[0].setBounds (rowArea.removeFromLeft (kLaneColumn).reduced (2, 1));

        const auto outB = rowArea.removeFromLeft (kLaneColumn);
        row.outLanes[1].setBounds (row.outLanes[1].isVisible()
                                       ? outB.reduced (2, 1)
                                       : juce::Rectangle<int>());

        row.led.setBounds (rowArea.removeFromLeft (kLedColumn));

        // Fixed column even on a mono row -- so the Tune combo never shifts
        // between a mono row and the stereo one above or below it.
        const auto linkRect = rowArea.removeFromLeft (kLinkColumn);
        row.link.setBounds (row.link.isVisible()
                                ? linkRect.reduced (2, 3)
                                : juce::Rectangle<int>());

        row.tune.setBounds (rowArea.removeFromLeft (kTuneColumn).reduced (2, 1));

        // The custom-tuning editor occupies the extra row DIRECTLY below this
        // slot's row -- and only for the one slot whose Tune combo reads C.
        auto& detail = details_[(std::size_t) i];

        if (openDetailSlot_ == i)
        {
            auto detailArea = area.removeFromTop (kRowHeight);

            const std::pair<juce::Label*, juce::ComboBox*> pairs[] = {
                { &detail.riseLabel,    &detail.rise },
                { &detail.persistLabel, &detail.persist },
                { &detail.depthLabel,   &detail.depth },
                { &detail.qLabel,       &detail.q },
                { &detail.thrLabel,     &detail.thr },
            };

            const int cellWidth = juce::jmax (80, detailArea.getWidth() / 5);

            for (const auto& pair : pairs)
            {
                auto cell = detailArea.removeFromLeft (cellWidth).reduced (2, 1);
                pair.first->setBounds (cell.removeFromLeft (kDetailLabelWidth));
                pair.second->setBounds (cell);
            }
        }
        else
        {
            for (auto* c : { &detail.rise, &detail.persist, &detail.depth,
                             &detail.q, &detail.thr })
                c->setBounds ({});
            for (auto* l : { &detail.riseLabel, &detail.persistLabel,
                             &detail.depthLabel, &detail.qLabel,
                             &detail.thrLabel })
                l->setBounds ({});
        }
    }

    // The Add row occupies the slot right after the last visible row and
    // disappears once all 8 are on screen.
    if (visibleRows_ < kMaxSlots)
    {
        auto addArea = area.removeFromTop (kRowHeight);
        addButton_.setBounds (addArea.removeFromLeft (110).reduced (2, 1));
    }
    else
        addButton_.setBounds ({});
}

} // namespace gui
