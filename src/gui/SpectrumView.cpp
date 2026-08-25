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

constexpr float kFreqTicksHz[7]    = { 50.0f, 100.0f, 250.0f, 1000.0f,
                                       2500.0f, 5000.0f, 10000.0f };
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
    : controller_ (controller)
    , tickFont_ (az::theme::monoFont())
    , bodyFont_ (az::theme::baseFont())
    , xTickLabels_ { juce::String ("50"),   juce::String ("100"), juce::String ("250"),
                     juce::String ("1k"),   juce::String ("2.5k"), juce::String ("5k"),
                     juce::String ("10k") }
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

    // Toolbar: two combos and a toggle, pure display state.
    bandwidthLabel_.setText ("BAND", juce::dontSendNotification);
    bandwidthBox_.addItem ("Line",    1);   // ids match rta::BandMode order
    bandwidthBox_.addItem ("1/1 Oct", 2);
    bandwidthBox_.addItem ("1/3 Oct", 3);
    bandwidthBox_.setSelectedItemIndex (0, juce::dontSendNotification);
    bandwidthBox_.onChange = [this]
    {
        bandMode_ = static_cast<rta::BandMode> (bandwidthBox_.getSelectedId() - 1);
        applyBandMode();
        repaint();   // stale data keeps showing until the next frame lands
    };

    averageLabel_.setText ("AVG", juce::dontSendNotification);
    averageBox_.addItem ("Off", 1);         // ids match rta::AverageMode order
    averageBox_.addItem ("1s",  2);
    averageBox_.addItem ("3s",  3);
    averageBox_.addItem ("10s", 4);
    averageBox_.setSelectedItemIndex (0, juce::dontSendNotification);
    averageBox_.onChange = [this]
    {
        avgMode_ = static_cast<rta::AverageMode> (averageBox_.getSelectedId() - 1);
        repaint();
    };

    peakHoldButton_.setButtonText ("Peak hold");
    peakHoldButton_.setClickingTogglesState (true);
    peakHoldButton_.setToggleState (false, juce::dontSendNotification);
    peakHoldButton_.onClick = [this]
    {
        peakHold_ = peakHoldButton_.getToggleState();
        // The rising edge seeds peakDb_ inside rebuildGeometry, so turning it
        // on starts from the CURRENT frame instead of a decayed history.
        if (! peakHold_)
            peakPoints_.clear();
        repaint();
    };

    for (auto* c : { (juce::Component*) &bandwidthLabel_, (juce::Component*) &bandwidthBox_,
                     (juce::Component*) &averageLabel_,   (juce::Component*) &averageBox_,
                     (juce::Component*) &peakHoldButton_ })
    {
        c->setWantsKeyboardFocus (false);
        addAndMakeVisible (*c);
    }
    // Toolbar legends are silkscreen, and the peak toggle is a chip rather
    // than a switch: these change what the plot SHOWS, never what the DSP does,
    // so none of them may carry a lit lamp.
    for (auto* label : { &bandwidthLabel_, &averageLabel_ })
    {
        label->setFont (az::theme::legendFont (az::theme::legendFontSize - 2.0f));
        label->setColour (juce::Label::textColourId, az::theme::faded);
        label->setJustificationType (juce::Justification::centredLeft);
    }
    peakHoldButton_.getProperties().set ("azStyle", "ghost");

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
    auto strip = getLocalBounds().removeFromTop (kToolbarHeight).reduced (gap, spacing);
    strip.removeFromLeft (kCaptionWidth);

    constexpr int controlH = 22;

    peakHoldButton_.setBounds (strip.removeFromRight (96)
                                    .withSizeKeepingCentre (96, controlH));
    strip.removeFromRight (gap);
    averageBox_    .setBounds (strip.removeFromRight (72)
                                    .withSizeKeepingCentre (72, controlH));
    averageLabel_  .setBounds (strip.removeFromRight (32));
    strip.removeFromRight (gap);
    bandwidthBox_  .setBounds (strip.removeFromRight (86)
                                    .withSizeKeepingCentre (86, controlH));
    bandwidthLabel_.setBounds (strip.removeFromRight (38));
}

void SpectrumView::applyBandMode()
{
    bandCenterHz_   = rta::bandCenters (bandMode_, kMinHz, kMaxHz);
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
    controller_.copySnapshot (snapshot_);
    seenSequence_ = snapshot_.sequence;

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

void SpectrumView::timerCallback()
{
    if (isVisible())
        refreshFromSnapshot();
}

void SpectrumView::rebuildGeometry()
{
    spectrumPoints_.clear();   // capacity survives: the no-growth guarantee
    peakPoints_.clear();

    if (! (snapshot_.sampleRate > 0.0))
        return;

    const float hzPerBin = static_cast<float> (snapshot_.sampleRate / (double) Detector::kFftSize);
    const std::size_t count = std::min<std::size_t> (snapshot_.magnitudeCount,
                                                     snapshot_.magnitudes.size());

    // 1) Raw dB for this frame. resize() never allocates here: count is
    //    bounded by kNumBins, which every buffer reserved in the ctor.
    newDb_.resize (count);
    displayDb_.resize (count);
    displayLin_.resize (count);

    for (std::size_t bin = 0; bin < count; ++bin)
    {
        const float mag = std::max (snapshot_.magnitudes[bin], 1.0e-9f);
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
        if (hz < kMinHz || hz > kMaxHz)
            continue;

        const float nx = std::log10 (hz / kMinHz) / std::log10 (kMaxHz / kMinHz);
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
                           hzPerBin, kMinHz, kMaxHz, bandLevelsDb_.data());
    }
}

float SpectrumView::xForHz (const float hz, const juce::Rectangle<float>& plot)
{
    const float t = std::log10 (hz / kMinHz) / std::log10 (kMaxHz / kMinHz);
    return plot.getX() + t * plot.getWidth();
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
    const float leftGutter = 44.0f;

    //--------------------------------------------------------------------
    // Toolbar strip: a raised band carrying the section legend and the
    // display options (positioned in resized()).
    const auto toolbar = bounds.removeFromTop ((float) kToolbarHeight);
    g.setColour (panel);
    g.fillRect (toolbar);
    drawCaption (g, "Analyser",
                 toolbar.toNearestInt().withTrimmedLeft (gap).withWidth (kCaptionWidth),
                 dim);
    drawEngravedDivider (g, toolbar.toNearestInt());

    const auto plot   = bounds.withTrimmedLeft (leftGutter).withTrimmedBottom (labelH);
    const float plotW = plot.getWidth();
    const float plotH = plot.getHeight();

    if (plotW <= 1.0f || plotH <= 1.0f)
        return;

    //--------------------------------------------------------------------
    // The plot well. A hair darker than the canvas so the trace sits INSIDE
    // something, and so the grid has a ground of its own to be faint against.
    g.setColour (well);
    g.fillRect (plot);

    g.setColour (shade);
    for (const float hz : kFreqTicksHz)
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
    for (int i = 0; i < 7; ++i)
    {
        const float x = xForHz (kFreqTicksHz[i], plot);
        g.drawText (xTickLabels_[i],
                    (int) (x - 30.0f), (int) plot.getBottom() + spacing,
                    60, (int) labelH, juce::Justification::centred);
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
        g.setColour (faded);
        g.setFont (bodyFont_);
        g.drawText (noSignalLabel_, plot, juce::Justification::centred);
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

        g.setGradientFill ({ text.withAlpha (0.16f), plot.getX(), plot.getY(),
                             text.withAlpha (0.0f),  plot.getX(), plot.getBottom(),
                             false });
        g.fillPath (fillPath_);

        g.setColour (text.withAlpha (0.88f));
        g.strokePath (tracePath_, juce::PathStrokeType (1.3f));
    }
    else if (bandMode_ != rta::BandMode::Line)
    {
        const std::size_t nb = std::min (bandLevelsDb_.size(), bandEdgeLowHz_.size());
        const float bottom = plot.getBottom();

        for (std::size_t i = 0; i < nb; ++i)
        {
            if (bandLevelsDb_[i] <= rta::kSilenceDb)
                continue;

            const float x0 = xForHz (std::max (bandEdgeLowHz_[i],  kMinHz), plot);
            const float x1 = xForHz (std::min (bandEdgeHighHz_[i], kMaxHz), plot);
            const float y  = yForDb (juce::jlimit (kMinDb, kMaxDb, bandLevelsDb_[i]), plot);

            // A one-pixel gutter between bars: they read as discrete bands
            // rather than as a solid block with notches cut in it.
            g.setColour (text.withAlpha (0.30f));
            g.fillRect (x0, y, juce::jmax (1.0f, x1 - x0 - 1.0f), bottom - y);
            g.setColour (text.withAlpha (0.75f));
            g.fillRect (x0, y, juce::jmax (1.0f, x1 - x0 - 1.0f), 1.5f);
        }
    }

    // Peak hold: a thin dim trace above the signal, below the markers.
    if (peakHold_ && peakPoints_.size() > 1)
    {
        g.setColour (dim.withAlpha (0.8f));
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
        if (notch.frequency < kMinHz || notch.frequency > kMaxHz)
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

    for (std::uint32_t i = 0; i < notchCount; ++i)
    {
        const auto& notch = snapshot_.notches[i];
        const float hz = notch.frequency;
        if (hz < kMinHz || hz > kMaxHz)
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
        // readable against the trace even where the trace is loud.
        g.setColour (colour.withAlpha (kStemAlphaSettled
                                       + (kStemAlphaFresh - kStemAlphaSettled) * heat));
        g.drawLine (std::round (x) + 0.5f, top, std::round (x) + 0.5f, plot.getBottom(), 1.0f);

        markerPath_.clear();
        markerPath_.startNewSubPath (x - kMarkerHalfWidth, top);
        markerPath_.lineTo (x + kMarkerHalfWidth, top);
        markerPath_.lineTo (x, apexY);
        markerPath_.closeSubPath();

        g.setColour (colour);
        g.fillPath (markerPath_);

        // The flag carries the notch's ORDER, which is information: it is the
        // same index the ACTIVE NOTCHES table lists it under. Skipped when the
        // previous flag would overlap -- a smear of unreadable digits helps
        // nobody, and the stem still marks the frequency.
        // Fixed width, not measured: the label is always two monospaced
        // digits, so measuring it every frame for every notch would buy an
        // identical number at the cost of a text layout per marker.
        const juce::String label = juce::String ((int) i + 1).paddedLeft ('0', 2);
        g.setFont (monoFont (10.0f));
        constexpr float flagW = 2.0f * kFlagDigitWidth + 2.0f * kFlagPad;

        if (x - flagW * 0.5f > lastFlagRight)
        {
            const juce::Rectangle<float> flag (x - flagW * 0.5f, plot.getY(),
                                               flagW, kFlagHeight);
            g.setColour (colour);
            g.fillRect (flag);
            g.setColour (background);
            g.drawText (label, flag, juce::Justification::centred, false);

            lastFlagRight = flag.getRight() + 2.0f;
        }
    }
}

} // namespace gui
