#include "gui/SpectrumView.h"

#include "gui/theme/AzTheme.h"

#include <algorithm>
#include <cmath>

namespace gui
{

namespace
{
// THE SIGNATURE, drawn per active notch: a full-height hairline STEM at its
// frequency, a WEDGE hanging off the top whose length encodes the notch depth,
// and a small numbered FLAG. All three take their colour from the theme's
// hot-to-ice age ramp, so the plot carries the history of the show: sodium
// stems mean the room is fighting right now, a field of ice means it was
// solved and is holding.
// The wedge is a PIN, not a bar. It was first written as a triangle running
// from the plot's top edge down to 40-100% of its height, which is what the
// depth->height brief literally asked for -- and rendered, it was three
// full-height slabs of amber with the trace buried behind them. The data is
// the trace; the marker only has to say WHERE and HOW DEEP.
//
// So depth still drives the wedge's length, but over a short fixed span near
// the top of the plot rather than over the whole plot. -24 dB is 46 px, 0 dB
// is 18 px, and neither is ever tall enough to compete with the signal.
constexpr float kMarkerHalfWidth   = 3.5f;
constexpr float kMarkerDepthSpanDb = 24.0f;
constexpr float kWedgeMinLength    = 18.0f;  // a 0 dB notch still reads
constexpr float kWedgeMaxLength    = 46.0f;  // -24 dB, the deepest the DSP allows

constexpr float kFlagHeight        = 13.0f;
constexpr float kFlagPad           = 5.0f;
constexpr float kFlagDigitWidth    = 6.0f;   // one mono digit at 10 px
constexpr float kStemAlphaSettled  = 0.22f;  // an old stem is nearly a whisper
constexpr float kStemAlphaFresh    = 0.60f;
constexpr float kGlowRadius        = 16.0f;
constexpr float kGlowAlpha         = 0.40f;
constexpr float kGlowFloor         = 0.05f;  // below this heat, no blur at all

// Non-zero ids make each segmented group mutually exclusive.
constexpr int kBandRadioGroupId    = 11;
constexpr int kAverageRadioGroupId = 12;

// The signal's area fill. Sodium, and deliberately faint: it gives the trace
// body without turning the lower half of the plot into a solid block.
constexpr float kFillTopAlpha      = 0.24f;
constexpr float kFillMidAlpha      = 0.07f;

// A marker sits ON the trace, and now that the trace is sodium too, hue alone
// no longer separates them -- a fresh (sodium) notch drawn straight onto a
// lit sodium curve disappears into it. Every marker is therefore drawn over a
// dark keyline: the canvas colour, one pixel proud on each side. It is how a
// hardware analyser separates its cursors from its trace, and it works
// regardless of what colour the ramp has the marker at.
constexpr float kKeylineWidth      = 3.0f;

// A 1-2-5 ladder. rebuildTicks() keeps whichever rungs fall inside the
// operator's chosen window, so the labels stay on round numbers however the
// range is dragged instead of landing on 63.4 Hz.
constexpr float kTickLadder[] = {
    20.0f, 30.0f, 50.0f, 80.0f, 100.0f, 200.0f, 300.0f, 500.0f, 800.0f,
    1000.0f, 2000.0f, 3000.0f, 5000.0f, 8000.0f, 10000.0f, 15000.0f, 20000.0f
};

// How close to an edge a click has to be, in the axis gutter, to grab it.
constexpr int kEdgeGrabPx = 28;
constexpr float kDbGridLines[4]    = { 0.0f, -30.0f, -60.0f, -90.0f };

// Peak hold decays by this many dB per frame when no new peak arrives --
// slow enough to read as "recent maxima", fast enough to forget stale ones.
constexpr float kPeakDecayDbPerFrame = 0.5f;

// Timer rate; also the fps the averaging alpha is computed for (the timer IS
// the frame clock on the message thread).
constexpr float kFramesPerSecond = 30.0f;

// Notch highlight band: a translucent full-height strip at each active notch
// frequency plus a brighter centre sliver. Amber comes from the theme's
// marker token -- the same hue the notch markers use, so highlight + marker
// always agree on where the notch is.
constexpr float kHighlightAlpha    = 0.15f;
constexpr float kHighlightSliverW  = 1.5f;
constexpr float kHighlightMinWidth = 6.0f;
constexpr float kHighlightRelWidth = 0.004f;
} // namespace

SpectrumView::SpectrumView (const NotchController& controller)
    : controller_ (&controller)
    , tickFont_ (az::theme::monoFont())
    , bodyFont_ (az::theme::baseFont())
    , yTickLabels_ { juce::String ("0"), juce::String ("-30"),
                     juce::String ("-60"), juce::String ("-90") }
    , noSignalLabel_ ("Waiting for signal")
{
    // The one reservation that keeps paint() allocation-free forever after.
    spectrumPoints_.reserve ((std::size_t) Detector::kNumBins);
    peakPoints_.reserve ((std::size_t) Detector::kNumBins);

    // Display buffers: worst case is kNumBins entries (band buffers far less,
    // but one reserve of the max costs nothing and removes any doubt).
    newDb_.reserve       ((std::size_t) Detector::kNumBins);
    displayDb_.reserve   ((std::size_t) Detector::kNumBins);
    peakDb_.reserve      ((std::size_t) Detector::kNumBins);
    displayLin_.reserve  ((std::size_t) Detector::kNumBins);
    bandLevelsDb_.reserve ((std::size_t) 31);   // widest series is 1/3 oct
    bandCenterHz_.reserve    ((std::size_t) 31);
    bandEdgeLowHz_.reserve   ((std::size_t) 31);
    bandEdgeHighHz_.reserve  ((std::size_t) 31);

    // Sized for a dashed vertical stem over the tallest plot this view is
    // ever asked to draw (a 2000px-tall 4K plot, plus a 25% margin) -- see
    // kDashedStemReserveFloats's derivation comment in the header. One
    // preallocateSpace here keeps the paint path's first-frame growth off
    // the steady-state no-allocation guarantee.
    dashedStemPath_.preallocateSpace (kDashedStemReserveFloats);

    // Toolbar, in the study's order: the display groups sit LEFT next to the
    // section caption, ring risk sits far right. Nothing here reaches the
    // audio engine, the detector or the notch chain.
    bandGroup_.onSelected = [this] (int index)
    {
        bandMode_ = static_cast<rta::BandMode> (index);
        applyBandMode();
        repaint();   // stale data keeps showing until the next frame lands
    };

    // L/R lane picker: which of snapshot_.magnitudes[] rebuildGeometry() reads.
    // Disabled outright for a mono controller -- see refreshFromSnapshot().
    laneGroup_.onSelected = [this] (int index) { setDisplayLane (index); };

    avgGroup_.onSelected = [this] (int index)
    {
        // The segment index IS the mode: AverageMode is ordered Off, 0.1, 0.3,
        // 0.5, 1 -- see RtaProcessing.h.
        avgMode_ = static_cast<rta::AverageMode> (index);

        // The long-average combo is showing something else now.
        avgLongBox_.setSelectedId (0, juce::dontSendNotification);
        repaint();
    };

    avgLongBox_.addItem ("3 s",  1);
    avgLongBox_.addItem ("5 s",  2);
    avgLongBox_.addItem ("10 s", 3);
    avgLongBox_.setTextWhenNothingSelected ("3 s +");
    avgLongBox_.setWantsKeyboardFocus (false);
    avgLongBox_.onChange = [this]
    {
        switch (avgLongBox_.getSelectedId())
        {
            case 1:  avgMode_ = rta::AverageMode::S3;  break;
            case 2:  avgMode_ = rta::AverageMode::S5;  break;
            case 3:  avgMode_ = rta::AverageMode::S10; break;
            default: return;   // cleared by a segment click, not by the user
        }

        // Picking a long average deselects every quick segment: they are one
        // choice split across two controls, not two independent settings.
        avgGroup_.setSelectedIndex (-1);
        repaint();
    };
    addAndMakeVisible (avgLongBox_);

    // A one-segment group used as a latch: clicking the selected segment must
    // TOGGLE it, which a radio group on its own will not do.
    peakGroup_.getSegmentForTest (0).setRadioGroupId (0);
    peakGroup_.getSegmentForTest (0).setColour (juce::TextButton::textColourOnId,
                                                az::theme::peak);
    peakGroup_.onSelected = [this] (int)
    {
        peakHold_ = peakGroup_.getSegmentForTest (0).getToggleState();

        // The rising edge seeds peakDb_ inside rebuildGeometry, so turning it
        // on starts from the CURRENT frame instead of a decayed history.
        if (! peakHold_)
            peakPoints_.clear();

        repaint();
    };
    peakGroup_.getSegmentForTest (0).setToggleState (false, juce::dontSendNotification);
    peakGroup_.getSegmentForTest (0).onClick = [this]
    {
        peakHold_ = peakGroup_.getSegmentForTest (0).getToggleState();
        if (! peakHold_)
            peakPoints_.clear();
        repaint();
    };

    // RANGE. Two fields for a precise figure, and the axis gutter itself for a
    // fast one -- see mouseDrag. A soundman narrowing the view during a show
    // wants the drag; one setting the rig up beforehand wants to type 12k.
    rangeLabel_.setText ("RANGE", juce::dontSendNotification);
    rangeLabel_.setFont (az::theme::legendFont (az::theme::columnFontSize, true,
                                                az::theme::trackingColumn));
    rangeLabel_.setColour (juce::Label::textColourId, az::theme::dim);
    rangeLabel_.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (rangeLabel_);

    for (auto* field : { &lowField_, &highField_ })
    {
        field->setFont (az::theme::monoFont (az::theme::segmentFontSize));
        field->setJustification (juce::Justification::centred);
        field->setColour (juce::TextEditor::backgroundColourId, az::theme::well);
        field->setColour (juce::TextEditor::outlineColourId,    az::theme::border);
        field->setColour (juce::TextEditor::focusedOutlineColourId, az::theme::accent);
        field->setColour (juce::TextEditor::textColourId,       az::theme::text);
        field->setColour (juce::TextEditor::highlightColourId,  az::theme::accent.withAlpha (0.3f));
        field->setSelectAllWhenFocused (true);

        // Applied on Return and on losing focus -- never per keystroke, which
        // would rebuild the geometry while somebody is still typing "1" of
        // "16k" and briefly show a 1 Hz axis.
        field->onReturnKey = [this] { applyRangeFromEditors(); };
        field->onFocusLost = [this] { applyRangeFromEditors(); };

        addAndMakeVisible (*field);
    }

    pushRangeToEditors();
    rebuildTicks();

    bandGroup_.setSelectedIndex ((int) bandMode_);
    avgGroup_ .setSelectedIndex ((int) avgMode_);
    laneGroup_.setSelectedIndex (displayLane_);
    laneGroup_.setEnabled (false);   // mono until refreshFromSnapshot() says otherwise

    for (auto* group : { &bandGroup_, &avgGroup_, &peakGroup_, &laneGroup_ })
    {
        group->setWantsKeyboardFocus (false);
        addAndMakeVisible (*group);
    }

    applyBandMode();

    setOpaque (true);
    startTimerHz ((int) kFramesPerSecond);
}

SpectrumView::~SpectrumView()
{
    stopTimer();
}


void SpectrumView::resized()
{
    // Thin strip across the top; the plot paints below it (paint trims the
    // same kToolbarHeight off its bounds).
    using namespace az::theme;

    // The ANALYSER caption is painted at the strip's left (see paint()); the
    // controls start after it and sit at the strip's RIGHT end, so the eye
    // meets the section name first and the display options last.
    // Left to right, exactly as the study lays it out (spec section 3):
    //
    //   ANALYSER  [Line|1/1 oct|1/3 oct]  [Avg off|1 s|3 s|10 s]  [Peak hold]
    //                                     ...spacer...   RING RISK  [ N/A ]
    //
    // The first cut mirrored this -- display groups right, risk left -- and
    // wrapped each option in its own rounded chip.
    auto strip = getLocalBounds().removeFromTop (kToolbarHeight).reduced (gap + spacing, 0);

    const int controlH = kToolbarHeight - 2 * spacing;
    auto place = [&strip, controlH] (SegmentedControl& group)
    {
        group.setBounds (strip.removeFromLeft (group.getPreferredWidth())
                              .withSizeKeepingCentre (group.getPreferredWidth(), controlH));
        strip.removeFromLeft (gap);
    };

    strip.removeFromLeft (kCaptionWidth);

    place (bandGroup_);
    place (laneGroup_);
    place (avgGroup_);

    // The long-average combo butts against the averaging group it extends.
    constexpr int kAvgLongWidth = 74;
    avgLongBox_.setBounds (strip.removeFromLeft (kAvgLongWidth)
                                .withSizeKeepingCentre (kAvgLongWidth, controlH));
    strip.removeFromLeft (gap);

    place (peakGroup_);

    // RING RISK is pinned to the far right, its chip last.
    riskChipArea_ = strip.removeFromRight (kRiskChipWidth)
                         .withSizeKeepingCentre (kRiskChipWidth, controlH);
    strip.removeFromRight (kRiskLegendWidth + gap);

    // RANGE sits just left of it: also a readout about the ROOM rather than
    // about the display, and typed rarely enough to belong at that end.
    constexpr int kRangeFieldWidth = 54;

    highField_.setBounds (strip.removeFromRight (kRangeFieldWidth)
                               .withSizeKeepingCentre (kRangeFieldWidth, controlH));
    strip.removeFromRight (spacing);
    lowField_ .setBounds (strip.removeFromRight (kRangeFieldWidth)
                               .withSizeKeepingCentre (kRangeFieldWidth, controlH));
    strip.removeFromRight (spacing);
    rangeLabel_.setBounds (strip.removeFromRight (52));
}

float SpectrumView::parseFrequency (const juce::String& text)
{
    auto trimmed = text.trim().toLowerCase().removeCharacters (" hz");
    if (trimmed.isEmpty())
        return 0.0f;

    float scale = 1.0f;
    if (trimmed.endsWithChar ('k'))
    {
        scale = 1000.0f;
        trimmed = trimmed.dropLastCharacters (1);
    }

    // getFloatValue returns 0 for anything unparseable, which is exactly the
    // "keep what you had" signal the editors want -- no exception, no dialog.
    const float value = trimmed.getFloatValue() * scale;
    return value > 0.0f ? value : 0.0f;
}

juce::String SpectrumView::formatFrequency (const float hz)
{
    if (hz < 1000.0f)
        return juce::String (juce::roundToInt (hz));

    const float k = hz / 1000.0f;
    // 16k, not 16.0k; 1.25k keeps its decimals because dropping them would
    // move the number.
    return (std::abs (k - std::round (k)) < 0.05f
                ? juce::String (juce::roundToInt (k))
                : juce::String (k, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."))
         + "k";
}

void SpectrumView::setDisplayRange (float low, float high)
{
    if (low > high)
        std::swap (low, high);

    low  = juce::jlimit (kMinHz, kMaxHz, low);
    high = juce::jlimit (kMinHz, kMaxHz, high);

    // Enforce the minimum span by pushing the end that is NOT pinned to a
    // limit, so dragging one edge into the other stops rather than dragging
    // the whole window along with it.
    const float minSpan = std::exp2 (kMinSpanOctaves);
    if (high < low * minSpan)
    {
        if (high >= kMaxHz)
            low = high / minSpan;
        else
            high = low * minSpan;

        low  = juce::jlimit (kMinHz, kMaxHz, low);
        high = juce::jlimit (kMinHz, kMaxHz, high);
    }

    if (juce::approximatelyEqual (low, lowHz_) && juce::approximatelyEqual (high, highHz_))
        return;

    lowHz_  = low;
    highHz_ = high;

    // The stored polyline is normalised against the RANGE, and the octave-band
    // edges are derived from it, so both have to be rebuilt -- not just
    // repainted.
    rebuildTicks();
    applyBandMode();
    rebuildGeometry();
    pushRangeToEditors();
    repaint();
}

void SpectrumView::resetDisplayRange()
{
    setDisplayRange (kDefaultLowHz, kDefaultHighHz);
}

void SpectrumView::applyRangeFromEditors()
{
    const float low  = parseFrequency (lowField_.getText());
    const float high = parseFrequency (highField_.getText());

    // Either field left unreadable keeps its current value rather than
    // snapping the axis to something the operator did not ask for.
    setDisplayRange (low  > 0.0f ? low  : lowHz_,
                     high > 0.0f ? high : highHz_);

    // Unconditional: setDisplayRange returns early when nothing changed, and
    // the field still holds whatever was typed.
    pushRangeToEditors();
}

void SpectrumView::pushRangeToEditors()
{
    lowField_ .setText (formatFrequency (lowHz_),  juce::dontSendNotification);
    highField_.setText (formatFrequency (highHz_), juce::dontSendNotification);
}

void SpectrumView::rebuildTicks()
{
    tickHz_.clear();
    tickLabels_.clear();

    for (const float hz : kTickLadder)
    {
        if (hz < lowHz_ || hz > highHz_)
            continue;

        tickHz_.push_back (hz);
        tickLabels_.push_back (formatFrequency (hz));
    }
}

juce::Rectangle<int> SpectrumView::axisGutter() const
{
    const int labelH = juce::roundToInt (tickFont_.getHeight()) + 2 * az::theme::spacing;
    return getLocalBounds().removeFromBottom (labelH).withTrimmedLeft (kLeftGutter);
}

//==============================================================================
// Dragging the axis. The gutter under the plot IS the control: grab near an
// end and that end follows the pointer. Double-click puts it back.

void SpectrumView::mouseMove (const juce::MouseEvent& event)
{
    setMouseCursor (axisGutter().contains (event.getPosition())
                        ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::NormalCursor);
}

void SpectrumView::mouseDown (const juce::MouseEvent& event)
{
    draggingEdge_ = -1;

    const auto gutter = axisGutter();
    if (! gutter.contains (event.getPosition()))
        return;

    // Whichever end is nearer, provided the click is actually near one. A
    // click in the middle of the axis does nothing rather than jumping an edge
    // across the plot.
    const int fromLeft  = std::abs (event.x - gutter.getX());
    const int fromRight = std::abs (event.x - gutter.getRight());

    if (juce::jmin (fromLeft, fromRight) > kEdgeGrabPx)
        return;

    draggingEdge_ = fromLeft <= fromRight ? 0 : 1;
}

void SpectrumView::mouseDrag (const juce::MouseEvent& event)
{
    if (draggingEdge_ < 0)
        return;

    const auto plot = axisGutter().toFloat();
    if (plot.getWidth() <= 1.0f)
        return;

    const float hz = hzForX ((float) event.x, plot);

    if (draggingEdge_ == 0)
        setDisplayRange (hz, highHz_);
    else
        setDisplayRange (lowHz_, hz);
}

void SpectrumView::mouseUp (const juce::MouseEvent&)
{
    draggingEdge_ = -1;
}

void SpectrumView::mouseDoubleClick (const juce::MouseEvent& event)
{
    if (axisGutter().contains (event.getPosition()))
        resetDisplayRange();
}

void SpectrumView::applyBandMode()
{
    bandCenterHz_   = rta::bandCenters (bandMode_, lowHz_, highHz_);
    const int count = (int) bandCenterHz_.size();

    bandEdgeLowHz_.clear();
    bandEdgeHighHz_.clear();

    if (count > 0)
    {
        // N = 1 or 3 halves per octave -> edges at center * 2^(+/-1/(2N)).
        const float n = (bandMode_ == rta::BandMode::Octave1) ? 1.0f : 3.0f;
        const float halfWidthFactor = std::exp2 (0.5f / n);

        bandEdgeLowHz_.reserve ((std::size_t) count);
        bandEdgeHighHz_.reserve ((std::size_t) count);
        for (const float c : bandCenterHz_)
        {
            bandEdgeLowHz_.push_back  (c / halfWidthFactor);
            bandEdgeHighHz_.push_back (c * halfWidthFactor);
        }
    }

    bandLevelsDb_.assign ((std::size_t) count, rta::kSilenceDb);
}

std::uint64_t SpectrumView::notchKey (const std::uint8_t channel,
                                     const std::uint8_t index,
                                     const float hz)
{
    // Byte-for-byte the key NotchListPanel uses: quantised Hz, then channel,
    // then slot. A recycled slot moved to a new frequency is a NEW notch.
    const auto quantised = (std::uint64_t) std::llround ((double) hz);
    return (quantised << 16) | ((std::uint64_t) channel << 8) | (std::uint64_t) index;
}

void SpectrumView::setController (const NotchController& controller)
{
    if (controller_ == &controller)
        return;

    controller_ = &controller;

    // Everything below describes the slot we just left.
    firstSeenMs_.clear();
    spectrumPoints_.clear();
    peakPoints_.clear();
    peakDb_.clear();
    peakHoldWasOn_ = false;
    snapshot_ = {};
    seenSequence_  = 0;
    drawnSequence_ = 0;

    // The old slot's lane choice describes a controller we just left; a mono
    // slot has no R lane at all, and a fresh stereo one deserves a fresh L.
    displayLane_ = 0;
    laneGroup_.setSelectedIndex (0);

    refreshFromSnapshot();
    repaint();
}

void SpectrumView::setDisplayLane (const int lane)
{
    displayLane_ = juce::jlimit (0, 1, lane);
    laneGroup_.setSelectedIndex (displayLane_);

    // Force the next refresh (and this rebuild, right now) to actually
    // re-read the chosen lane rather than being skipped as "unchanged" --
    // the sequence itself has not moved, only which lane is being plotted.
    drawnSequence_ = 0;
    rebuildGeometry();
    repaint();
}

void SpectrumView::updateNotchAges()
{
    const double now = juce::Time::getMillisecondCounterHiRes();

    const std::uint32_t count = std::min<std::uint32_t> (
        snapshot_.notchCount, (std::uint32_t) snapshot_.notches.size());

    // Rebuild rather than age-out: a notch that is not in THIS frame is gone
    // from the plot, so its entry has no reader. Keeping a timeout here (as
    // the table does, to survive snapshot hiccups in a text column) would let
    // a stale identity resurrect at the wrong colour.
    std::map<std::uint64_t, double> stillLive;

    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto& notch = snapshot_.notches[i];
        const auto key = notchKey (notch.channel, notch.index, notch.frequency);

        const auto existing = firstSeenMs_.find (key);
        stillLive[key] = existing != firstSeenMs_.end() ? existing->second : now;
    }

    firstSeenMs_.swap (stillLive);
}

void SpectrumView::refreshFromSnapshot()
{
    controller_->copySnapshot (snapshot_);
    seenSequence_ = snapshot_.sequence;

    // The lane picker only makes sense for a stereo controller. Dropping back
    // to mono (a slot switch, or the room simply publishing fewer lanes)
    // forces the view back to L rather than plotting a lane that no longer
    // exists.
    const bool stereo = snapshot_.laneCount >= 2;
    laneGroup_.setEnabled (stereo);
    if (! stereo && displayLane_ != 0)
    {
        displayLane_ = 0;
        laneGroup_.setSelectedIndex (0);
    }

    updateNotchAges();

    if (snapshot_.sequence != drawnSequence_)
    {
        drawnSequence_ = snapshot_.sequence;
        rebuildGeometry();
        repaint();
        return;
    }

    // The colour ramp keeps moving even when the spectrum does not: a held
    // notch cooling from sodium to ice has to keep repainting or it freezes
    // at whatever heat the last new frame left it on.
    if (! firstSeenMs_.empty())
        repaint();
}

juce::String SpectrumView::ringRiskLabel (const RingRisk risk)
{
    switch (risk)
    {
        case RingRisk::Low:      return "LOW";
        case RingRisk::Rising:   return "RISING";
        case RingRisk::Critical: return "CRITICAL";
        case RingRisk::Unavailable: break;
    }

    // NOT "low". An unwired readout that reads reassuring is worse than one
    // that admits it has nothing, because a soundman would act on it.
    return "N/A";
}

juce::Colour SpectrumView::ringRiskColour (const RingRisk risk)
{
    using namespace az::theme;

    switch (risk)
    {
        case RingRisk::Low:      return dim;
        case RingRisk::Rising:   return warn;
        case RingRisk::Critical: return danger;
        case RingRisk::Unavailable: break;
    }
    return faded;
}

void SpectrumView::timerCallback()
{
    if (! isVisible())
        return;

    // Resolved on the plot's own poll: the readout describes the same instant
    // the trace does, and a provider that is null stays Unavailable forever
    // without any special casing further down.
    const auto risk = ringRiskProvider != nullptr ? ringRiskProvider()
                                                  : RingRisk::Unavailable;
    if (risk != ringRisk_)
    {
        ringRisk_ = risk;
        repaint (riskChipArea_.expanded (az::theme::gap));
    }

    refreshFromSnapshot();
}

void SpectrumView::rebuildGeometry()
{
    spectrumPoints_.clear();   // capacity survives: the no-growth guarantee
    peakPoints_.clear();

    if (! (snapshot_.sampleRate > 0.0))
        return;

    const float hzPerBin = static_cast<float> (snapshot_.sampleRate / (double) Detector::kFftSize);
    const std::size_t lane = (std::size_t) displayLane_;
    const std::size_t count = std::min<std::size_t> (snapshot_.magnitudeCount,
                                                     snapshot_.magnitudes[lane].size());

    // 1) Raw dB for this frame. resize() never allocates here: count is
    //    bounded by kNumBins, which every buffer reserved in the ctor.
    newDb_.resize (count);
    displayDb_.resize (count);
    displayLin_.resize (count);

    for (std::size_t bin = 0; bin < count; ++bin)
    {
        const float mag = std::max (snapshot_.magnitudes[lane][bin], 1.0e-9f);
        newDb_[bin] = 20.0f * std::log10 (mag);
    }

    // 2) Averaging: EMA of dB towards this frame's raw values.
    if (avgMode_ == rta::AverageMode::Off)
    {
        displayDb_ = newDb_;
    }
    else
    {
        const float alpha = rta::averageAlpha (avgMode_, kFramesPerSecond);
        for (std::size_t i = 0; i < count; ++i)
            displayDb_[i] += alpha * (newDb_[i] - displayDb_[i]);
    }

    // 3) Peak hold: decay the held maxima, then take whatever is louder now
    //    (or seed from this frame on the toggle's rising edge).
    if (peakHold_)
    {
        if (! peakHoldWasOn_)
            peakDb_ = newDb_;
        else
        {
            peakDb_.resize (count);
            for (std::size_t i = 0; i < count; ++i)
                peakDb_[i] = std::max (newDb_[i], peakDb_[i] - kPeakDecayDbPerFrame);
        }
        peakHoldWasOn_ = true;
    }
    else
    {
        peakHoldWasOn_ = false;
    }

    // 4) Normalised polylines, stored against the CURRENT plot rectangle so a
    //    resize stays correct without a rebuild inside paint().
    for (std::size_t bin = 0; bin < count; ++bin)
    {
        const float hz = static_cast<float> (bin) * hzPerBin;
        if (hz < lowHz_ || hz > highHz_)
            continue;

        const float nx = std::log10 (hz / lowHz_) / std::log10 (highHz_ / lowHz_);
        // Clamped: loud frames exceed 0 dB, and an unclamped ny would push the
        // line ABOVE the plot frame into the dB-label gutter.
        const float ny = juce::jlimit (0.0f, 1.0f,
                                       (kMaxDb - displayDb_[bin]) / (kMaxDb - kMinDb));

        spectrumPoints_.push_back ({ nx, ny });

        if (peakHold_)
        {
            const float py = juce::jlimit (0.0f, 1.0f,
                                           (kMaxDb - peakDb_[bin]) / (kMaxDb - kMinDb));
            peakPoints_.push_back ({ nx, py });
        }
    }

    // 5) Octave bands: power-sum the DISPLAYED spectrum so bandwidth mode and
    //    averaging compose instead of fighting.
    if (bandMode_ != rta::BandMode::Line && ! bandCenterHz_.empty())
    {
        for (std::size_t i = 0; i < count; ++i)
            displayLin_[i] = std::pow (10.0f, displayDb_[i] / 20.0f);

        bandLevelsDb_.resize (bandCenterHz_.size());   // capacity pre-reserved
        rta::bandLevelsDb (bandMode_, displayLin_.data(), (int) count,
                           hzPerBin, lowHz_, highHz_, bandLevelsDb_.data());
    }
}

float SpectrumView::xForHz (const float hz, const juce::Rectangle<float>& plot) const
{
    const float t = std::log10 (hz / lowHz_) / std::log10 (highHz_ / lowHz_);
    return plot.getX() + t * plot.getWidth();
}

float SpectrumView::hzForX (const float x, const juce::Rectangle<float>& plot) const
{
    if (plot.getWidth() <= 0.0f)
        return lowHz_;

    const float t = juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth());
    return lowHz_ * std::pow (highHz_ / lowHz_, t);
}

float SpectrumView::yForDb (const float db, const juce::Rectangle<float>& plot)
{
    const float t = (kMaxDb - db) / (kMaxDb - kMinDb);
    return plot.getY() + t * plot.getHeight();
}

double SpectrumView::ageMsOf (const NotchController::SnapshotNotch& notch,
                              const double nowMs) const
{
    const auto it = firstSeenMs_.find (notchKey (notch.channel, notch.index, notch.frequency));
    return it != firstSeenMs_.end() ? juce::jmax (0.0, nowMs - it->second) : 0.0;
}

void SpectrumView::paint (juce::Graphics& g)
{
    using namespace az::theme;

    g.fillAll (background);

    auto bounds            = getLocalBounds().toFloat();
    const float labelH     = tickFont_.getHeight() + 2.0f * (float) spacing;
    const float leftGutter = (float) kLeftGutter;

    //--------------------------------------------------------------------
    // Toolbar strip: a raised band carrying the section legend and the
    // display options (positioned in resized()).
    const auto toolbar = bounds.removeFromTop ((float) kToolbarHeight);
    g.setColour (panel);
    g.fillRect (toolbar);
    drawCaption (g, "Analyser",
                 toolbar.toNearestInt().withTrimmedLeft (gap).withWidth (kCaptionWidth),
                 dim);

    if (! riskChipArea_.isEmpty())
    {
        drawCaption (g, "Ring risk",
                     riskChipArea_.withX (riskChipArea_.getX() - kRiskLegendWidth)
                                  .withWidth (kRiskLegendWidth),
                     dim);

        const auto riskColour = ringRiskColour (ringRisk_);
        const auto chip = riskChipArea_.toFloat().reduced (0.5f);

        // An UNAVAILABLE chip gets no fill at all -- a hollow outline reads as
        // "nothing to report", where a filled chip in any colour reads as a
        // measurement.
        if (ringRisk_ != RingRisk::Unavailable)
        {
            g.setColour (riskColour.withAlpha (0.16f));
            g.fillRoundedRectangle (chip, cornerRadius);
        }

        g.setColour (riskColour.withAlpha (ringRisk_ == RingRisk::Unavailable ? 0.35f : 0.7f));
        g.drawRoundedRectangle (chip, cornerRadius, 1.0f);

        g.setColour (riskColour);
        g.setFont (monoFont (segmentFontSize));
        g.drawText (ringRiskLabel (ringRisk_), riskChipArea_,
                    juce::Justification::centred, false);
    }

    drawEngravedDivider (g, toolbar.toNearestInt());

    const auto plot   = bounds.withTrimmedLeft (leftGutter).withTrimmedBottom (labelH);
    const float plotW = plot.getWidth();
    const float plotH = plot.getHeight();

    if (plotW <= 1.0f || plotH <= 1.0f)
        return;

    //--------------------------------------------------------------------
    // The plot's ground is the CANVAS colour, not a separate well. The study
    // paints the analyser on the same black as the page, and the extra value
    // step of a well was enough to make the amber fill read as a brown slab
    // rather than a wash over the background.
    g.setColour (background);
    g.fillRect (plot);

    // The grid is READ, not felt: at the groove colour it was all but
    // invisible against the plot well, which is the whole reason a grid
    // exists. `grid` is its own token for exactly this.
    g.setColour (grid);
    for (const float hz : tickHz_)
    {
        const float x = std::round (xForHz (hz, plot)) + 0.5f;
        g.drawLine (x, plot.getY(), x, plot.getBottom(), 1.0f);
    }
    for (const float db : kDbGridLines)
    {
        const float y = std::round (yForDb (db, plot)) + 0.5f;
        g.drawLine (plot.getX(), y, plot.getRight(), y, 1.0f);
    }

    g.setColour (border);
    g.drawRect (plot, 1.0f);

    // Tick labels -- mono for every number (theme contract).
    g.setFont (tickFont_);
    g.setColour (faded);
    for (std::size_t i = 0; i < tickHz_.size(); ++i)
    {
        const float x = xForHz (tickHz_[i], plot);
        g.drawText (tickLabels_[i],
                    (int) (x - 30.0f), (int) plot.getBottom() + spacing,
                    60, (int) labelH, juce::Justification::centred);
    }

    // The two grips that say the axis can be dragged. Drawn at the ends of the
    // gutter, brighter while one is being dragged, so the affordance is
    // visible before anybody discovers it by accident.
    const auto gutter = axisGutter().toFloat();
    for (int edge = 0; edge < 2; ++edge)
    {
        const float gx = edge == 0 ? gutter.getX() + 1.0f : gutter.getRight() - 3.0f;

        g.setColour (draggingEdge_ == edge ? accent : dim.withAlpha (0.55f));
        g.fillRect (gx, gutter.getY() + 3.0f, 2.0f, gutter.getHeight() - 6.0f);
    }
    for (int i = 0; i < 4; ++i)
    {
        const float y = yForDb (kDbGridLines[i], plot);
        g.drawText (yTickLabels_[i],
                    0, (int) (y - 8.0f), (int) leftGutter - spacing, 16,
                    juce::Justification::centredRight);
    }

    //--------------------------------------------------------------------
    // Empty state (spec section 5): grid only, no trace, no markers. Copy
    // says what the app is doing, not what is absent.
    if (snapshot_.sequence == 0)
    {
        // Same reasoning as the notch table's empty state: a label, not prose.
        g.setColour (dim);
        g.setFont (legendFont (captionFontSize, true, trackingCaption));
        g.drawText (noSignalLabel_.toUpperCase(), plot, juce::Justification::centred);
        return;
    }

    //--------------------------------------------------------------------
    // The trace is MONOCHROME on purpose. Colour in this plot means one thing
    // -- a notch, on the hot-to-ice ramp -- so the signal itself is drawn in
    // the silkscreen white and never competes with it.
    if (bandMode_ == rta::BandMode::Line && spectrumPoints_.size() > 1)
    {
        tracePath_.clear();
        const std::size_t n = spectrumPoints_.size();

        tracePath_.startNewSubPath (plot.getX() + spectrumPoints_[0].x * plotW,
                                    plot.getY() + spectrumPoints_[0].y * plotH);
        for (std::size_t i = 1; i < n; ++i)
            tracePath_.lineTo (plot.getX() + spectrumPoints_[i].x * plotW,
                               plot.getY() + spectrumPoints_[i].y * plotH);

        // Area fill: the same path closed down to the floor. It gives the
        // trace body at a glance without adding a second colour.
        fillPath_ = tracePath_;
        fillPath_.lineTo (plot.getX() + spectrumPoints_[n - 1].x * plotW, plot.getBottom());
        fillPath_.lineTo (plot.getX() + spectrumPoints_[0].x * plotW,     plot.getBottom());
        fillPath_.closeSubPath();

        // Sodium, with the fill dying away well before the floor: the signal
        // reads as lit rather than as a grey plot line, and the plot still has
        // air in it. The notch markers stay legible on top because they are
        // SOLID and carry flags and stems, and because a settled one is ice --
        // which on an amber field is the strongest contrast on screen.
        juce::ColourGradient body (accent.withAlpha (kFillTopAlpha),
                                   plot.getX(), plot.getY(),
                                   accent.withAlpha (0.0f),
                                   plot.getX(), plot.getBottom(),
                                   false);
        body.addColour (0.55, accent.withAlpha (kFillMidAlpha));
        g.setGradientFill (body);
        g.fillPath (fillPath_);

        g.setColour (trace);
        g.strokePath (tracePath_, juce::PathStrokeType (1.4f));
    }
    else if (bandMode_ != rta::BandMode::Line)
    {
        const std::size_t nb = std::min (bandLevelsDb_.size(), bandEdgeLowHz_.size());
        const float bottom = plot.getBottom();

        for (std::size_t i = 0; i < nb; ++i)
        {
            if (bandLevelsDb_[i] <= rta::kSilenceDb)
                continue;

            const float x0 = xForHz (std::max (bandEdgeLowHz_[i],  lowHz_), plot);
            const float x1 = xForHz (std::min (bandEdgeHighHz_[i], highHz_), plot);
            const float y  = yForDb (juce::jlimit (kMinDb, kMaxDb, bandLevelsDb_[i]), plot);

            // A one-pixel gutter between bars: they read as discrete bands
            // rather than as a solid block with notches cut in it. Lit cap on
            // a dimmer body, same sodium the line trace uses.
            g.setColour (accent.withAlpha (0.26f));
            g.fillRect (x0, y, juce::jmax (1.0f, x1 - x0 - 1.0f), bottom - y);
            g.setColour (trace);
            g.fillRect (x0, y, juce::jmax (1.0f, x1 - x0 - 1.0f), 1.5f);
        }
    }

    // Peak hold: a thin dim trace above the signal, below the markers.
    if (peakHold_ && peakPoints_.size() > 1)
    {
        g.setColour (peak.withAlpha (0.9f));
        const std::size_t n = peakPoints_.size();
        for (std::size_t i = 1; i < n; ++i)
        {
            const auto a = peakPoints_[i - 1];
            const auto b = peakPoints_[i];
            g.drawLine (plot.getX() + a.x * plotW, plot.getY() + a.y * plotH,
                        plot.getX() + b.x * plotW, plot.getY() + b.y * plotH,
                        1.0f);
        }
    }

    //--------------------------------------------------------------------
    // THE MARKERS. Per notch: a stem, a depth wedge, and a numbered flag, all
    // on the age ramp. Paths are members and cleared rather than rebuilt, so
    // this allocates nothing in the steady state.
    const double now = juce::Time::getMillisecondCounterHiRes();
    const std::uint32_t notchCount = std::min<std::uint32_t> (
        snapshot_.notchCount, (std::uint32_t) snapshot_.notches.size());

    glowPath_.clear();
    float hottest = 0.0f;
    juce::Colour hottestColour = marker;

    // Pass 1: the hot ones' halo shapes, collected before anything is drawn so
    // the blur lands UNDER every marker rather than over the ones after it.
    for (std::uint32_t i = 0; i < notchCount; ++i)
    {
        const auto& notch = snapshot_.notches[i];
        if (notch.frequency < lowHz_ || notch.frequency > highHz_)
            continue;

        const float heat = notchHeat (ageMsOf (notch, now));
        if (heat <= kGlowFloor)
            continue;

        const float x = xForHz (notch.frequency, plot);
        glowPath_.addEllipse (x - kGlowRadius * 0.5f, plot.getY() + kFlagHeight,
                              kGlowRadius, kGlowRadius);

        if (heat > hottest)
        {
            hottest = heat;
            hottestColour = notchColour (ageMsOf (notch, now));
        }
    }

    if (hottest > kGlowFloor)
    {
        notchGlow_.setColor (hottestColour.withAlpha (kGlowAlpha * hottest));
        notchGlow_.render (g, glowPath_);
    }

    // Pass 2: stems and wedges.
    float lastFlagRight = -1.0e6f;

    // The flag-overlap skip further down compares each flag against the
    // PREVIOUS one only, which is correct only if the markers are visited left
    // to right. The snapshot orders its notches by (lane, index), and since
    // lane S that is no longer ascending in frequency -- an INDEP slot can
    // hold 1.2 kHz on R after 1.9 kHz on L, and the low flag was then dropped
    // as "overlapping" a flag it sits nowhere near. So the markers are visited
    // in frequency order. `order` carries each notch's ORIGINAL position,
    // which is the number the flag prints and the row the ACTIVE NOTCHES table
    // lists it under -- sorting the draw order must not renumber them. Fixed
    // size and sorted in place, so paint() allocates nothing: the array holds
    // kTotalSlots = 32 entries (2 lanes x 16 slots), which is every notch a
    // snapshot can carry -- not 16, which would be one lane's worth.
    std::array<std::uint32_t, NotchController::kTotalSlots> order {};
    for (std::uint32_t i = 0; i < notchCount; ++i)
        order[i] = i;
    std::sort (order.begin(), order.begin() + (std::ptrdiff_t) notchCount,
               [this] (std::uint32_t a, std::uint32_t b)
               {
                   return snapshot_.notches[a].frequency < snapshot_.notches[b].frequency;
               });

    for (std::uint32_t k = 0; k < notchCount; ++k)
    {
        const std::uint32_t i = order[k];
        const auto& notch = snapshot_.notches[i];
        const float hz = notch.frequency;
        if (hz < lowHz_ || hz > highHz_)
            continue;

        const double ageMs  = ageMsOf (notch, now);
        const float  heat   = notchHeat (ageMs);
        const auto   colour = notchColour (ageMs);

        // Depth 0..-24 dB -> wedge length kWedgeMinLength..kWedgeMaxLength.
        const float depth = juce::jlimit (0.0f, kMarkerDepthSpanDb, -notch.depthDB);
        const float length = kWedgeMinLength
                           + (kWedgeMaxLength - kWedgeMinLength) * (depth / kMarkerDepthSpanDb);

        const float x     = xForHz (hz, plot);
        const float top   = plot.getY() + kFlagHeight;
        const float apexY = juce::jmin (plot.getBottom(), top + length);

        // The stem runs the full plot height so the notch's frequency is
        // readable against the trace even where the trace is loud. Dark
        // keyline first, colour on top -- see kKeylineWidth.
        const float stemX = std::round (x) + 0.5f;

        g.setColour (background.withAlpha (0.85f));
        g.drawLine (stemX, top, stemX, plot.getBottom(), kKeylineWidth);

        g.setColour (colour.withAlpha (kStemAlphaSettled
                                       + (kStemAlphaFresh - kStemAlphaSettled) * heat));

        // Lane 1 (R) draws its colour stem dashed instead of solid -- the only
        // visual difference between an L and an R marker, so a stereo trace
        // reads which channel fired without a second legend. markerPath_ is
        // reused as scratch to describe the straight line createDashedStroke
        // needs as its source; it is cleared and rebuilt as the wedge triangle
        // a few lines below, so nothing here escapes this iteration.
        if (notch.channel == 1)
        {
            const float dashes[] = { 4.0f, 3.0f };
            markerPath_.clear();
            markerPath_.startNewSubPath (stemX, top);
            markerPath_.lineTo (stemX, plot.getBottom());

            dashedStemPath_.clear();
            juce::PathStrokeType (1.0f).createDashedStroke (dashedStemPath_, markerPath_, dashes, 2);
            g.strokePath (dashedStemPath_, juce::PathStrokeType (1.0f));
        }
        else
            g.drawLine (stemX, top, stemX, plot.getBottom(), 1.0f);

        markerPath_.clear();
        markerPath_.startNewSubPath (x - kMarkerHalfWidth, top);
        markerPath_.lineTo (x + kMarkerHalfWidth, top);
        markerPath_.lineTo (x, apexY);
        markerPath_.closeSubPath();

        g.setColour (background.withAlpha (0.85f));
        g.strokePath (markerPath_, juce::PathStrokeType (kKeylineWidth));

        g.setColour (colour);
        g.fillPath (markerPath_);

        // The flag carries the notch's ORDER, which is information: it is the
        // same index the ACTIVE NOTCHES table lists it under. Skipped when the
        // previous flag would overlap -- a smear of unreadable digits helps
        // nobody, and the stem still marks the frequency.
        // Fixed width, not measured: the label is always two monospaced
        // digits, so measuring it every frame for every notch would buy an
        // identical number at the cost of a text layout per marker.
        juce::String label = juce::String ((int) i + 1).paddedLeft ('0', 2);
        if (notch.channel == 1)
            label += "R";   // the same "which lane" cue the dashed stem gives
        g.setFont (monoFont (10.0f));
        const float flagW = 2.0f * kFlagDigitWidth + 2.0f * kFlagPad
                           + (notch.channel == 1 ? kFlagDigitWidth : 0.0f);

        if (x - flagW * 0.5f > lastFlagRight)
        {
            const juce::Rectangle<float> flag (x - flagW * 0.5f, plot.getY(),
                                               flagW, kFlagHeight);
            g.setColour (background.withAlpha (0.85f));
            g.fillRect (flag.expanded (1.0f));
            g.setColour (colour);
            g.fillRect (flag);
            g.setColour (background);
            g.drawText (label, flag, juce::Justification::centred, false);

            lastFlagRight = flag.getRight() + 2.0f;
        }
    }
}

} // namespace gui
