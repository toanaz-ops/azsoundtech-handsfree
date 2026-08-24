#include "gui/SpectrumView.h"

#include "gui/theme/AzTheme.h"

#include <algorithm>
#include <cmath>

namespace gui
{

namespace
{
// Marker shape: a v whose height encodes the notch depth. Ruling R-2: in v1
// EVERY active notch is drawn in the theme marker colour -- the "new notch"
// highlight and its age are later GUI-side concerns, deliberately not
// modelled in SnapshotBuffer.
constexpr float kMarkerHalfWidth   = 5.0f;
constexpr float kMarkerDepthSpanDb = 24.0f;  // -24 dB maps to full height
constexpr float kMinHeightFraction = 0.4f;   // 0 dB still gets 40% height

constexpr float kFreqTicksHz[3]    = { 100.0f, 1000.0f, 10000.0f };
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
    , xTickLabels_ { juce::String ("100"), juce::String ("1k"), juce::String ("10k") }
    , yTickLabels_ { juce::String ("0"), juce::String ("-30"),
                     juce::String ("-60"), juce::String ("-90") }
    , noSignalLabel_ ("no signal")
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
    bandwidthLabel_.setText ("BW", juce::dontSendNotification);
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

    averageLabel_.setText ("Avg", juce::dontSendNotification);
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

    peakHoldButton_.setButtonText ("Peak");
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
    bandwidthLabel_.setFont (bodyFont_);
    averageLabel_.setFont (bodyFont_);

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
    auto strip = getLocalBounds().removeFromTop (kToolbarHeight).reduced (az::theme::spacing);

    bandwidthLabel_ .setBounds (strip.removeFromLeft (24));
    bandwidthBox_   .setBounds (strip.removeFromLeft (86).withSizeKeepingCentre (86, 22));
    strip.removeFromLeft (az::theme::gap);
    averageLabel_   .setBounds (strip.removeFromLeft (28));
    averageBox_     .setBounds (strip.removeFromLeft (70).withSizeKeepingCentre (70, 22));
    strip.removeFromLeft (az::theme::gap);
    peakHoldButton_ .setBounds (strip.removeFromLeft (64).withSizeKeepingCentre (64, 22));
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

void SpectrumView::refreshFromSnapshot()
{
    controller_.copySnapshot (snapshot_);
    seenSequence_ = snapshot_.sequence;

    if (snapshot_.sequence != drawnSequence_)
    {
        drawnSequence_ = snapshot_.sequence;
        rebuildGeometry();
        repaint();
    }
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

void SpectrumView::paint (juce::Graphics& g)
{
    g.fillAll (az::theme::background);

    auto bounds            = getLocalBounds().toFloat();
    const float labelH     = tickFont_.getHeight() + 2.0f * (float) az::theme::spacing;
    const float leftGutter = 44.0f;

    // Toolbar strip: raised panel so the controls read as chrome, not data.
    const auto toolbar = bounds.removeFromTop ((float) kToolbarHeight);
    g.setColour (az::theme::panel);
    g.fillRect (toolbar);

    const auto plot        = bounds.withTrimmedLeft (leftGutter)
                                  .withTrimmedBottom (labelH);
    const float plotW      = plot.getWidth();
    const float plotH      = plot.getHeight();

    // Frame and grid.
    g.setColour (az::theme::border);
    g.drawRect (plot, 1.0f);

    for (const float hz : kFreqTicksHz)
    {
        const float x = xForHz (hz, plot);
        g.drawLine (x, plot.getY(), x, plot.getBottom());
    }

    for (const float db : kDbGridLines)
    {
        const float y = yForDb (db, plot);
        g.drawLine (plot.getX(), y, plot.getRight(), y);
    }

    // Tick labels -- mono font for every number (theme contract).
    g.setFont (tickFont_);
    g.setColour (az::theme::dim);
    for (int i = 0; i < 3; ++i)
    {
        const float x = xForHz (kFreqTicksHz[i], plot);
        g.drawText (xTickLabels_[i],
                    (int) (x - 30.0f), (int) plot.getBottom() + az::theme::spacing,
                    60, (int) labelH,
                    juce::Justification::centred);
    }

    for (int i = 0; i < 4; ++i)
    {
        const float y = yForDb (kDbGridLines[i], plot);
        g.drawText (yTickLabels_[i],
                    0, (int) (y - 8.0f),
                    (int) leftGutter - az::theme::spacing, 16,
                    juce::Justification::centredRight);
    }

    // Empty state (spec section 5): grid only, dim text, NO markers.
    if (snapshot_.sequence == 0)
    {
        g.setFont (bodyFont_);
        g.drawText (noSignalLabel_, plot, juce::Justification::centred);
        return;
    }

    // Notch highlight bands -- drawn FIRST of the data layers (under the
    // spectrum line and the markers): a translucent full-height amber strip
    // at every active notch frequency, with a brighter centre sliver. Same
    // notch list the marker loop below consumes.
    const std::uint32_t highlightCount = std::min<std::uint32_t> (
        snapshot_.notchCount, (std::uint32_t) snapshot_.notches.size());
    const float highlightW = juce::jmax (kHighlightMinWidth, plotW * kHighlightRelWidth);

    for (std::uint32_t i = 0; i < highlightCount; ++i)
    {
        const float hz = snapshot_.notches[i].frequency;
        if (hz < kMinHz || hz > kMaxHz)
            continue;

        const float x = xForHz (hz, plot);
        g.setColour (az::theme::marker.withAlpha (kHighlightAlpha));
        g.fillRect (x - highlightW * 0.5f, plot.getY(), highlightW, plotH);

        g.setColour (az::theme::marker.withAlpha (kHighlightAlpha * 2.5f));
        g.fillRect (x - kHighlightSliverW * 0.5f, plot.getY(),
                    kHighlightSliverW, plotH);
    }

    // Spectrum: either the fine polyline (Line mode) or one bar per octave
    // band. Both read from buffers rebuilt on the message thread.
    if (bandMode_ == rta::BandMode::Line)
    {
        g.setColour (az::theme::accent);
        const std::size_t n = spectrumPoints_.size();
        for (std::size_t i = 1; i < n; ++i)
        {
            const auto a = spectrumPoints_[i - 1];
            const auto b = spectrumPoints_[i];
            g.drawLine (plot.getX() + a.x * plotW, plot.getY() + a.y * plotH,
                        plot.getX() + b.x * plotW, plot.getY() + b.y * plotH,
                        1.2f);
        }
    }
    else
    {
        g.setColour (az::theme::accent.withAlpha (0.45f));
        const std::size_t nb = std::min (bandLevelsDb_.size(), bandEdgeLowHz_.size());
        const float bottom = plot.getBottom();
        for (std::size_t i = 0; i < nb; ++i)
        {
            if (bandLevelsDb_[i] <= rta::kSilenceDb)
                continue;

            const float x0 = xForHz (std::max (bandEdgeLowHz_[i],  kMinHz), plot);
            const float x1 = xForHz (std::min (bandEdgeHighHz_[i], kMaxHz), plot);
            const float y  = yForDb (juce::jlimit (kMinDb, kMaxDb, bandLevelsDb_[i]), plot);
            g.fillRect (x0, y, x1 - x0, bottom - y);
        }
    }

    // Peak hold trace: a thin dim line above everything but the markers.
    if (peakHold_ && ! peakPoints_.empty())
    {
        g.setColour (az::theme::dim);
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

    // Active-notch markers. Rebuilt into the reused member Path each paint:
    // clear() keeps the Path's storage, so this allocates nothing steady-state.
    markerPath_.clear();
    const std::uint32_t notchCount = std::min<std::uint32_t> (snapshot_.notchCount,
                                                              (std::uint32_t) snapshot_.notches.size());
    for (std::uint32_t i = 0; i < notchCount; ++i)
    {
        const auto& notch = snapshot_.notches[i];
        const float hz = notch.frequency;
        if (hz < kMinHz || hz > kMaxHz)
            continue;

        // Depth -24..0 dB -> marker height 40..100% of the plot (brief).
        const float depth = juce::jlimit (0.0f, kMarkerDepthSpanDb, -notch.depthDB);
        const float fraction = kMinHeightFraction
                             + (1.0f - kMinHeightFraction) * (depth / kMarkerDepthSpanDb);

        const float x     = xForHz (hz, plot);
        const float apexY = plot.getY() + plotH * fraction;

        markerPath_.startNewSubPath (x - kMarkerHalfWidth, plot.getY());
        markerPath_.lineTo (x + kMarkerHalfWidth, plot.getY());
        markerPath_.lineTo (x, apexY);
        markerPath_.closeSubPath();
    }

    g.setColour (az::theme::marker);
    g.fillPath (markerPath_);
}

} // namespace gui
