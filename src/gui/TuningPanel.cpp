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

    const auto onChange = [this] { handleChanged(); };
    rise_.onChange    = onChange;
    persist_.onChange = onChange;
    depth_.onChange   = onChange;
    q_.onChange       = onChange;
    thr_.onChange     = onChange;

    for (auto* box : { &rise_, &persist_, &depth_, &q_, &thr_ })
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

    caption_.setBounds (area.removeFromLeft (96).reduced (2));

    // Five label+combo pairs share what is left evenly.
    const int pairWidth = area.getWidth() / 5;
    const int labelWidth = 44;

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
