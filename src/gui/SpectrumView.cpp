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

    setOpaque (true);
    startTimerHz (30);
}

SpectrumView::~SpectrumView()
{
    stopTimer();
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

    if (! (snapshot_.sampleRate > 0.0))
        return;

    const float hzPerBin = static_cast<float> (snapshot_.sampleRate / (double) Detector::kFftSize);
    const std::size_t count = std::min<std::size_t> (snapshot_.magnitudeCount,
                                                     snapshot_.magnitudes.size());

    for (std::size_t bin = 0; bin < count; ++bin)
    {
        const float hz = static_cast<float> (bin) * hzPerBin;
        if (hz < kMinHz || hz > kMaxHz)
            continue;

        const float mag = std::max (snapshot_.magnitudes[bin], 1.0e-9f);
        const float db  = 20.0f * std::log10 (mag);

        // Stored normalised to the plot rectangle; paint() maps them with the
        // CURRENT bounds, so a resize is correct without a rebuild.
        const float nx = std::log10 (hz / kMinHz) / std::log10 (kMaxHz / kMinHz);
        const float ny = (kMaxDb - db) / (kMaxDb - kMinDb);

        spectrumPoints_.push_back ({ nx, ny });
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

    const auto bounds      = getLocalBounds().toFloat();
    const float labelH     = tickFont_.getHeight() + 2.0f * (float) az::theme::spacing;
    const float leftGutter = 44.0f;
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

    // Spectrum polyline from the pre-sized normalised point cache.
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
