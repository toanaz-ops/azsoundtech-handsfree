#include "gui/TuningPanel.h"

#include "gui/theme/AzTheme.h"

#include <iterator>
#include <utility>

namespace gui
{

namespace
{
// Combo item ids are 1-based positions in the fixed choice lists below.
constexpr int kRiseChoices[]    = { 100, 250, 500, 750, 1000 };
constexpr int kDepthChoices[]   = { -6, -12, -18, -24 };
constexpr int kQChoices[]       = { 10, 20, 30, 40, 50 };
constexpr float kThrChoices[]   = { 6.0f, 8.0f, 10.0f, 12.0f, 15.0f };

// Any non-zero id makes the three RESPONSE segments mutually exclusive.
constexpr int kResponseRadioGroupId = 7;

template <typename T, std::size_t N>
int idForValue (const T (&choices)[N], const T& value)
{
    for (std::size_t i = 0; i < N; ++i)
        if (choices[i] == value)
            return (int) i + 1;
    return 0;   // no such item -- refresh() leaves the combo untouched
}

template <typename T, std::size_t N>
T valueForId (const T (&choices)[N], int id)
{
    if (id >= 1 && id <= (int) N)
        return choices[(std::size_t) (id - 1)];
    return choices[0];   // no selection -> first item
}

// Select `value`'s item, or -- when it is not on the list -- SHOW it as text.
//
// setSelectedId(0) empties the combo, which is what an off-list value used to
// produce: a strip that could not say what the notch ceiling was. Off-list
// values are reachable since loadPreset started installing a file's ceiling
// (Q13 keeps the exact number; presets/Music.json carries Q 25 / -10 dB).
// ComboBox::setText matches an existing item by text first, so a value that
// IS on the list can never end up as a text-only label by this route.
template <typename T, std::size_t N>
void selectOrShow (juce::ComboBox& box, const T (&choices)[N], const T& value,
                   const juce::String& text)
{
    const int id = idForValue (choices, value);

    if (id != 0)
        box.setSelectedId (id, juce::dontSendNotification);
    else
        box.setText (text, juce::dontSendNotification);
}

// RESPONSE preset table (brief 2026-08-24): item id == array position + 1.
constexpr TuningPanel::Params kPresets[] = {
    { 500, 4, -12, 40, 12.0f },   // 1 SAFE
    { 250, 3, -18, 30, 10.0f },   // 2 BALANCED (matches the param defaults)
    { 100, 1, -24, 20,  8.0f },   // 3 AGGRESSIVE
};

bool paramsEqual (const TuningPanel::Params& a, const TuningPanel::Params& b)
{
    return a.riseReferenceMs   == b.riseReferenceMs
        && a.persistenceBlocks == b.persistenceBlocks
        && a.depthDb           == b.depthDb
        && a.q                 == b.q
        && a.peakinessThreshold == b.peakinessThreshold;
}
} // namespace

//==============================================================================
// Id <-> value mappings.

int   TuningPanel::riseMsForId      (int id) { return valueForId (kRiseChoices, id); }
int   TuningPanel::depthDbForId     (int id) { return valueForId (kDepthChoices, id); }
int   TuningPanel::qForId           (int id) { return valueForId (kQChoices, id); }
float TuningPanel::thresholdForId   (int id) { return valueForId (kThrChoices, id); }
int   TuningPanel::idForRiseMs      (int ms) { return idForValue (kRiseChoices, ms); }
int   TuningPanel::idForDepthDb     (int db) { return idForValue (kDepthChoices, db); }
int   TuningPanel::idForQ           (int q)  { return idForValue (kQChoices, q); }
int   TuningPanel::idForThreshold   (float t){ return idForValue (kThrChoices, t); }

//==============================================================================

TuningPanel::TuningPanel()
{
    using namespace az::theme;

    // RESPONSE: three segments in one radio group. Ghost style -- these choose
    // what the DETECTOR does, but they are still a display of a curated
    // choice, and a lit lamp on them would compete with the transport, which
    // is the only place in this app a lamp means "this is the live state".
    const std::pair<juce::TextButton*, int> segments[] = {
        { &safeButton,       kPresetSafeId },
        { &balancedButton,   kPresetBalancedId },
        { &aggressiveButton, kPresetAggressiveId },
    };

    for (const auto& [button, id] : segments)
    {
        button->setClickingTogglesState (true);
        button->setRadioGroupId (kResponseRadioGroupId);
        button->getProperties().set ("azStyle", "ghost");

        // Guarded exactly like ModeRail's mode handlers: the radio group turns
        // the PREVIOUS segment off with a notification, which would otherwise
        // re-fire that segment's handler.
        button->onClick = [this, button, id]
        {
            if (button->getToggleState())
                setSelectedPresetId (id, juce::sendNotificationSync);
        };

        addAndMakeVisible (*button);
    }

    for (int i = 0; i < (int) std::size (kRiseChoices); ++i)
        rise_.addItem (juce::String (kRiseChoices[i]) + " ms", i + 1);
    for (int i = 1; i <= 6; ++i)
        persist_.addItem (juce::String (i), i);
    for (int i = 0; i < (int) std::size (kDepthChoices); ++i)
        depth_.addItem (juce::String (kDepthChoices[i]) + " dB", i + 1);
    for (int i = 0; i < (int) std::size (kQChoices); ++i)
        q_.addItem (juce::String (kQChoices[i]), i + 1);
    for (int i = 0; i < (int) std::size (kThrChoices); ++i)
        thr_.addItem (juce::String (kThrChoices[i], 1), i + 1);

    const auto onParamChange = [this]
    {
        // Any manual edit leaves the curated preset space.
        presetId_ = kPresetCustomId;
        reflectPresetButtons();
        handleChanged();
    };
    rise_.onChange    = onParamChange;
    persist_.onChange = onParamChange;
    depth_.onChange   = onParamChange;
    q_.onChange       = onParamChange;
    thr_.onChange     = onParamChange;

    for (auto* box : { &rise_, &persist_, &depth_, &q_, &thr_ })
        addAndMakeVisible (*box);

    for (auto* label : { &responseLabel_, &notchLabel_, &triggerLabel_,
                         &riseLabel_, &persistLabel_, &depthLabel_,
                         &qLabel_, &thrLabel_ })
        addAndMakeVisible (*label);

    // Every legend here is silkscreen: tracked uppercase, quiet, and never
    // louder than the value beside it. The gutter legends sit one step
    // brighter than the inline ones -- they name the ROW, not one field.
    for (auto* label : { &responseLabel_, &notchLabel_, &triggerLabel_ })
    {
        label->setText (label->getText().toUpperCase(), juce::dontSendNotification);
        label->setFont (legendFont (captionFontSize - 1.0f, true, trackingColumn));
        label->setColour (juce::Label::textColourId, dim);
        label->setJustificationType (juce::Justification::centredLeft);
    }

    for (auto* label : { &riseLabel_, &persistLabel_, &depthLabel_,
                         &qLabel_, &thrLabel_ })
    {
        label->setText (label->getText().toUpperCase(), juce::dontSendNotification);
        label->setFont (legendFont (columnFontSize, true, trackingColumn));
        label->setColour (juce::Label::textColourId, dim);
        label->setJustificationType (juce::Justification::centredLeft);
    }

    // Select the defaults without firing the callback: nothing has changed.
    refresh();
}

//==============================================================================
// RESPONSE.

void TuningPanel::reflectPresetButtons()
{
    // Set explicitly rather than leaning on the radio group: the group's own
    // clearing runs through the notification path these calls exist to avoid.
    safeButton      .setToggleState (presetId_ == kPresetSafeId,       juce::dontSendNotification);
    balancedButton  .setToggleState (presetId_ == kPresetBalancedId,   juce::dontSendNotification);
    aggressiveButton.setToggleState (presetId_ == kPresetAggressiveId, juce::dontSendNotification);
}

void TuningPanel::setSelectedPresetId (const int presetId,
                                       const juce::NotificationType notification)
{
    presetId_ = presetId;
    reflectPresetButtons();

    const bool curated = presetId >= kPresetSafeId && presetId <= kPresetAggressiveId;

    if (curated && notification != juce::dontSendNotification)
        applyPreset (presetId);
    else
        repaint();
}

void TuningPanel::applyPreset (const int presetId)
{
    const Params p = kPresets[(std::size_t) (presetId - 1)];

    // A curated preset is entirely on-list, so it also becomes what the panel
    // was last handed -- keeping provided_ from outliving the values on screen.
    provided_ = p;

    // Applied silently: the segment click is the user gesture, the five combos
    // just follow it -- otherwise each would loop back through the manual-edit
    // handler and knock the panel straight back to CUSTOM.
    rise_.setSelectedId    (idForRiseMs (p.riseReferenceMs), juce::dontSendNotification);
    persist_.setSelectedId (p.persistenceBlocks,             juce::dontSendNotification);
    depth_.setSelectedId   (idForDepthDb (p.depthDb),        juce::dontSendNotification);
    q_.setSelectedId       (idForQ (p.q),                    juce::dontSendNotification);
    thr_.setSelectedId     (idForThreshold (p.peakinessThreshold),
                            juce::dontSendNotification);

    // One callback per parameter, each carrying the complete snapshot -- the
    // same contract a manual combo edit has.
    if (onTuningChanged != nullptr)
        for (int i = 0; i < 5; ++i)
            onTuningChanged (p);

    repaint();
}

//==============================================================================

void TuningPanel::refresh()
{
    const Params p = paramsProvider != nullptr ? paramsProvider() : Params();

    // Remember the whole snapshot: currentParams() reports these values back
    // for any combo an off-list value left without a list selection.
    provided_ = p;

    selectOrShow (rise_,  kRiseChoices,  p.riseReferenceMs,
                  juce::String (p.riseReferenceMs) + " ms");
    selectOrShow (depth_, kDepthChoices, p.depthDb,
                  juce::String (p.depthDb) + " dB");
    selectOrShow (q_,     kQChoices,     p.q,
                  juce::String (p.q));
    selectOrShow (thr_,   kThrChoices,   p.peakinessThreshold,
                  juce::String (p.peakinessThreshold, 1));

    // HOLD's item id IS its value (1..6), so it has no choices array to look
    // through -- the same off-list guard, written out.
    if (p.persistenceBlocks >= 1 && p.persistenceBlocks <= 6)
        persist_.setSelectedId (p.persistenceBlocks, juce::dontSendNotification);
    else
        persist_.setText (juce::String (p.persistenceBlocks), juce::dontSendNotification);

    updatePresetFor (p);
}

void TuningPanel::updatePresetFor (const Params& p)
{
    for (int i = 0; i < (int) std::size (kPresets); ++i)
        if (paramsEqual (p, kPresets[i]))
        {
            presetId_ = i + 1;
            reflectPresetButtons();
            return;
        }

    presetId_ = kPresetCustomId;
    reflectPresetButtons();
}

TuningPanel::Params TuningPanel::currentParams() const
{
    // Start from what the panel was last HANDED, not from the choice lists.
    //
    // A combo with no list selection is showing an off-list value as text (see
    // refresh()), and valueForId would turn that id 0 into choices[0]. Every
    // combo reports the complete snapshot, so a touch on RISE would then push
    // Q 10 / -6 dB onto every Global slot -- a ceiling change nobody asked
    // for, on a live rig. Re-report the value instead; a combo that DOES have
    // a selection still wins below.
    Params p = provided_;

    if (const int id = rise_.getSelectedId(); id != 0)
        p.riseReferenceMs = riseMsForId (id);
    if (const int id = persist_.getSelectedId(); id != 0)
        p.persistenceBlocks = id;
    if (const int id = depth_.getSelectedId(); id != 0)
        p.depthDb = depthDbForId (id);
    if (const int id = q_.getSelectedId(); id != 0)
        p.q = qForId (id);
    if (const int id = thr_.getSelectedId(); id != 0)
        p.peakinessThreshold = thresholdForId (id);

    return p;
}

void TuningPanel::handleChanged()
{
    if (onTuningChanged != nullptr)
        onTuningChanged (currentParams());
}

//==============================================================================

void TuningPanel::paint (juce::Graphics& g)
{
    using namespace az::theme;

    const auto caption = getLocalBounds().removeFromTop (kFieldRowHeight);

    drawCaption (g, "Detection", caption, dim);

    // CUSTOM has no segment of its own: with none of the three lit, the row
    // would read as "nothing chosen" rather than "your own settings". A chip
    // at the row's right end says which it is, in the accent, because leaving
    // the curated presets is a state worth noticing.
    if (presetId_ == kPresetCustomId)
    {
        const auto chip = caption.withTrimmedTop (spacing)
                                 .removeFromRight (74)
                                 .withHeight (fieldHeight - 6);

        g.setColour (accent.withAlpha (0.35f));
        g.drawRoundedRectangle (chip.toFloat().reduced (0.5f), cornerRadius, 1.0f);

        g.setColour (accent);
        g.setFont (legendFont (legendFontSize - 2.0f));
        g.drawText ("CUSTOM", chip, juce::Justification::centred, false);
    }
}

void TuningPanel::resized()
{
    using namespace az::theme;

    auto area = getLocalBounds();
    area.removeFromTop (kFieldRowHeight);   // the caption, painted above

    // One legend+field cell, taking `width` off the row it is handed.
    auto cell = [] (juce::Rectangle<int>& row, int width,
                    juce::Label& legend, juce::ComboBox& field)
    {
        auto slot = row.removeFromLeft (width);
        slot.removeFromRight (gap);
        legend.setBounds (slot.removeFromLeft (inlineLegendWidth - 18));
        field .setBounds (slot.withSizeKeepingCentre (slot.getWidth(), fieldHeight));
    };

    //--------------------------------------------------------------------
    // RESPONSE: three equal segments filling the row after the gutter.
    auto responseRow = area.removeFromTop (kFieldRowHeight);
    responseLabel_.setBounds (responseRow.removeFromLeft (gutterWidth));

    auto segments = responseRow.withSizeKeepingCentre (responseRow.getWidth(), fieldHeight);
    const int segmentWidth = segments.getWidth() / 3;

    safeButton      .setBounds (segments.removeFromLeft (segmentWidth).withTrimmedRight (spacing));
    balancedButton  .setBounds (segments.removeFromLeft (segmentWidth).withTrimmedRight (spacing));
    aggressiveButton.setBounds (segments);

    // Both field rows use the SAME three-column grid, even though NOTCH only
    // fills two of them: DEPTH lines up with RISE and Q lines up with HOLD.
    // Splitting NOTCH into halves instead would put its two fields at x values
    // that match nothing else in the column.
    const int cellWidth = juce::jmax (1, (area.getWidth() - gutterWidth) / 3);

    //--------------------------------------------------------------------
    // NOTCH: what the filter IS. The third cell stays empty.
    auto notchRow = area.removeFromTop (kFieldRowHeight);
    notchLabel_.setBounds (notchRow.removeFromLeft (gutterWidth));
    cell (notchRow, cellWidth, depthLabel_, depth_);
    cell (notchRow, cellWidth, qLabel_, q_);

    //--------------------------------------------------------------------
    // TRIGGER: what makes one FIRE.
    auto triggerRow = area.removeFromTop (kFieldRowHeight);
    triggerLabel_.setBounds (triggerRow.removeFromLeft (gutterWidth));
    cell (triggerRow, cellWidth, riseLabel_, rise_);
    cell (triggerRow, cellWidth, persistLabel_, persist_);
    cell (triggerRow, triggerRow.getWidth(), thrLabel_, thr_);
}

} // namespace gui
