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

// ONE KNOB preset table (brief 2026-08-24): item id == array position + 1.
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
// TuningPanel.

TuningPanel::TuningPanel()
{
    using namespace az::theme;

    addAndMakeVisible (caption_);
    caption_.setFont (monoFont());

    addAndMakeVisible (oneKnobLabel_);
    oneKnobLabel_.setFont (monoFont());
    oneKnob_.addItem ("SAFE", 1);
    oneKnob_.addItem ("BALANCED", 2);
    oneKnob_.addItem ("AGGRESSIVE", 3);
    oneKnob_.addItem ("CUSTOM", kPresetCustomId);

    for (auto* label : { &riseLabel_, &persistLabel_, &depthLabel_,
                         &qLabel_, &thrLabel_ })
        addAndMakeVisible (*label);

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

    oneKnob_.onChange = [this] { handleOneKnobChanged(); };

    const auto onParamChange = [this]
    {
        // Any manual param edit leaves the curated preset space.
        oneKnob_.setSelectedId (kPresetCustomId, juce::dontSendNotification);
        handleChanged();
    };
    rise_.onChange    = onParamChange;
    persist_.onChange = onParamChange;
    depth_.onChange   = onParamChange;
    q_.onChange       = onParamChange;
    thr_.onChange     = onParamChange;

    for (auto* box : { &oneKnob_, &rise_, &persist_, &depth_, &q_, &thr_ })
        addAndMakeVisible (*box);

    // Select the defaults without firing the callback: nothing has changed.
    refresh();
}

void TuningPanel::refresh()
{
    const Params p = paramsProvider != nullptr ? paramsProvider() : Params();

    rise_.setSelectedId    (idForRiseMs (p.riseReferenceMs), juce::dontSendNotification);
    persist_.setSelectedId (p.persistenceBlocks,             juce::dontSendNotification);
    depth_.setSelectedId   (idForDepthDb (p.depthDb),        juce::dontSendNotification);
    q_.setSelectedId       (idForQ (p.q),                    juce::dontSendNotification);
    thr_.setSelectedId     (idForThreshold (p.peakinessThreshold),
                            juce::dontSendNotification);

    updateOneKnobFor (p);
}

void TuningPanel::updateOneKnobFor (const Params& p)
{
    for (int i = 0; i < (int) std::size (kPresets); ++i)
        if (paramsEqual (p, kPresets[i]))
        {
            oneKnob_.setSelectedId (i + 1, juce::dontSendNotification);
            return;
        }
    oneKnob_.setSelectedId (kPresetCustomId, juce::dontSendNotification);
}

void TuningPanel::handleOneKnobChanged()
{
    const int id = oneKnob_.getSelectedId();
    if (id >= 1 && id < kPresetCustomId)
        applyPreset (id);
}

void TuningPanel::applyPreset (int presetId)
{
    const Params p = kPresets[(std::size_t) (presetId - 1)];

    // Apply silently: the knob change is the user gesture, the five combos
    // just follow it -- otherwise refresh() would loop back through
    // handleChanged() five more times.
    rise_.setSelectedId    (idForRiseMs (p.riseReferenceMs), juce::dontSendNotification);
    persist_.setSelectedId (p.persistenceBlocks,             juce::dontSendNotification);
    depth_.setSelectedId   (idForDepthDb (p.depthDb),        juce::dontSendNotification);
    q_.setSelectedId       (idForQ (p.q),                    juce::dontSendNotification);
    thr_.setSelectedId     (idForThreshold (p.peakinessThreshold),
                            juce::dontSendNotification);

    // One callback per parameter, each carrying the complete snapshot --
    // same contract as a manual combo edit.
    if (onTuningChanged != nullptr)
        for (int i = 0; i < 5; ++i)
            onTuningChanged (p);

    repaint();
}

TuningPanel::Params TuningPanel::currentParams() const
{
    Params p;
    p.riseReferenceMs    = riseMsForId (rise_.getSelectedId());
    p.persistenceBlocks  = persist_.getSelectedId();
    p.depthDb            = depthDbForId (depth_.getSelectedId());
    p.q                  = qForId (q_.getSelectedId());
    p.peakinessThreshold = thresholdForId (thr_.getSelectedId());
    return p;
}

void TuningPanel::handleChanged()
{
    if (onTuningChanged != nullptr)
        onTuningChanged (currentParams());
}

void TuningPanel::resized()
{
    auto area = getLocalBounds();

    const int labelWidth = 44;

    // ONE KNOB sits at the far left, then the DETECTION caption.
    auto knobCell = area.removeFromLeft (96).reduced (2);
    oneKnobLabel_.setBounds (knobCell.removeFromLeft (labelWidth));
    oneKnob_.setBounds (knobCell);

    caption_.setBounds (area.removeFromLeft (72).reduced (2));

    // Five label+combo pairs share what is left evenly.
    const int pairWidth = area.getWidth() / 5;

    const std::pair<juce::Label*, juce::ComboBox*> pairs[] = {
        { &riseLabel_, &rise_ },   { &persistLabel_, &persist_ },
        { &depthLabel_, &depth_ }, { &qLabel_, &q_ },
        { &thrLabel_, &thr_ },
    };

    for (const auto& pair : pairs)
    {
        auto cell = area.removeFromLeft (pairWidth).reduced (2);
        pair.first->setBounds (cell.removeFromLeft (labelWidth));
        pair.second->setBounds (cell);
    }
}

} // namespace gui
