#include "gui/NotchListPanel.h"

#include "gui/theme/AzTheme.h"

#include <algorithm>
#include <cmath>

namespace gui
{

namespace
{
// Column widths, left to right. STATUS takes whatever remains. Chosen so the
// narrowest sensible panel (~360 px) still fits every formatted cell without
// truncation at the theme's 13 px mono face.
constexpr float kLeftPad    = 6.0f;
constexpr float kColIdW     = 30.0f;
constexpr float kColFreqW   = 88.0f;
constexpr float kColDepthW  = 96.0f;
constexpr float kColQW      = 44.0f;

// The typographic minus of the depth format. Built from a code point rather
// than a string literal so the compiler's execution charset can never mangle
// it (repo rule: UTF-8 survives every read-modify-write).
juce::String minusSign()
{
    return juce::String::charToString ((juce::juce_wchar) 0x2212);
}
} // namespace

NotchListPanel::NotchListPanel (const NotchController& controller, ClockFn nowMs)
    : controller_ (controller),
      nowMs_ (nowMs ? std::move (nowMs)
                    : ClockFn { [] {
                          return std::chrono::duration<double,
                                                     std::milli> (
                                     std::chrono::steady_clock::now().time_since_epoch())
                              .count();
                      } }),
      tableFont_ (az::theme::monoFont())
{
    // Upper bound of the table; reserved once so steady-state refreshes
    // never grow the vector.
    rows_.reserve ((std::size_t) NotchController::kTotalSlots);

    setOpaque (true);
    startTimerHz (4);   // ages tick in seconds; 4 Hz is plenty for new frames
}

NotchListPanel::~NotchListPanel()
{
    stopTimer();
}

std::uint64_t NotchListPanel::identityKey (const std::uint8_t channel,
                                           const std::uint8_t index,
                                           const float frequencyHz)
{
    const auto hz = (std::uint64_t) std::llround ((double) frequencyHz);
    return (hz << 16) | ((std::uint64_t) channel << 8) | (std::uint64_t) index;
}

void NotchListPanel::refreshFromSnapshot()
{
    controller_.copySnapshot (snapshot_);

    const double now = nowMs_();
    const std::uint32_t count = std::min<std::uint32_t> (
        snapshot_.notchCount, (std::uint32_t) snapshot_.notches.size());

    // 1. Record sightings: first-seen is kept for an identity seen again
    //    within kTrackingTimeoutMs (age keeps counting across brief drops).
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto& notch = snapshot_.notches[i];
        const auto key = identityKey (notch.channel, notch.index, notch.frequency);
        const auto [it, inserted] =
            sightings_.try_emplace (key, Sighting { now, now });
        if (inserted)
            it->second.firstSeenMs = now;   // redundant with try_emplace, explicit for clarity
        it->second.lastSeenMs = now;
    }

    // 2. Rebuild the row strings (paint() never allocates).
    rows_.clear();
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto& notch = snapshot_.notches[i];
        const auto key = identityKey (notch.channel, notch.index, notch.frequency);

        double ageMs = 0.0;
        if (const auto it = sightings_.find (key); it != sightings_.end())
            ageMs = now - it->second.firstSeenMs;

        rows_.push_back ({ juce::String (i + 1),
                           formatFrequency (notch.frequency),
                           formatDepthDb (notch.depthDB),
                           formatQ (notch.Q),
                           formatAgeMs (juce::jmax (0.0, ageMs)) });
    }

    // 3. Expire identities not seen recently (documented timeout above).
    for (auto it = sightings_.begin(); it != sightings_.end();)
    {
        if (now - it->second.lastSeenMs > kTrackingTimeoutMs)
            it = sightings_.erase (it);
        else
            ++it;
    }

    repaint();
}

juce::String NotchListPanel::formatFrequency (const float hz)
{
    // The Hz branch rounds to whole hertz, so the branch itself has to test
    // the ROUNDED value: 999.5 rounds to 1000 and must come out as "1.0 kHz",
    // not "1000 Hz".
    if (hz + 0.5f < 1000.0f)
        return juce::String ((int) (hz + 0.5f)) + " Hz";
    return juce::String (hz / 1000.0f, 1) + " kHz";
}

juce::String NotchListPanel::formatDepthDb (const float depthDb)
{
    const float magnitude = juce::jmax (0.0f, -depthDb);
    return minusSign() + juce::String (magnitude, 1) + " dB";
}

juce::String NotchListPanel::formatQ (const float q)
{
    return juce::String (q, 1);
}

juce::String NotchListPanel::formatAgeMs (const double ageMs)
{
    if (ageMs < 60000.0)
        return juce::String ((int) (ageMs / 1000.0)) + "s";
    return juce::String ((int) (ageMs / 60000.0)) + "m ago";
}

NotchListPanel::RowText NotchListPanel::rowForTest (const int index) const
{
    return rows_[(std::size_t) index];
}

void NotchListPanel::timerCallback()
{
    // Ages change even when the sequence does not, so unlike SpectrumView
    // this refresh is unconditional -- but only while anyone can see it.
    if (isVisible())
        refreshFromSnapshot();
}

void NotchListPanel::visibilityChanged()
{
    // Becoming visible must show CURRENT data immediately, not up to one
    // timer period of stale rows.
    if (isVisible())
        refreshFromSnapshot();
}

void NotchListPanel::paint (juce::Graphics& g)
{
    g.fillAll (az::theme::background);

    const auto frame = getLocalBounds().toFloat();
    g.setColour (az::theme::border);
    g.drawRect (frame, 1.0f);

    // Column origins.
    const float x0 = frame.getX() + kLeftPad;
    const float x1 = x0 + kColIdW;
    const float x2 = x1 + kColFreqW;
    const float x3 = x2 + kColDepthW;
    const float x4 = x3 + kColQW;

    g.setFont (tableFont_);

    // Header -- mono, dim (binding spec).
    g.setColour (az::theme::dim);
    const float headerY = frame.getY();
    g.drawText ("#",      x0, headerY, kColIdW    - 4.0f, (float) kHeaderHeight, juce::Justification::centredLeft);
    g.drawText ("FREQ",   x1, headerY, kColFreqW  - 4.0f, (float) kHeaderHeight, juce::Justification::centredLeft);
    g.drawText ("DEPTH",  x2, headerY, kColDepthW - 4.0f, (float) kHeaderHeight, juce::Justification::centredLeft);
    g.drawText ("Q",      x3, headerY, kColQW     - 4.0f, (float) kHeaderHeight, juce::Justification::centredLeft);
    g.drawText ("STATUS", x4, headerY, frame.getRight() - x4 - kLeftPad, (float) kHeaderHeight, juce::Justification::centredLeft);

    const float headerBottom = headerY + (float) kHeaderHeight;
    g.drawLine (frame.getX(), headerBottom, frame.getRight(), headerBottom);

    if (rows_.empty())
    {
        g.drawText (noNotchesLabel_,
                    frame.reduced (kLeftPad, 0.0f).withTop (headerBottom),
                    juce::Justification::centred);
        return;
    }

    // Rows separated by border hairlines; cells in the primary text colour.
    float rowTop = headerBottom;
    for (const auto& row : rows_)
    {
        g.setColour (az::theme::border);
        g.drawLine (frame.getX(), rowTop, frame.getRight(), rowTop);

        g.setColour (az::theme::text);
        g.drawText (row.id,     x0, rowTop, kColIdW    - 4.0f, (float) kRowHeight, juce::Justification::centredLeft);
        g.drawText (row.freq,   x1, rowTop, kColFreqW  - 4.0f, (float) kRowHeight, juce::Justification::centredLeft);
        g.drawText (row.depth,  x2, rowTop, kColDepthW - 4.0f, (float) kRowHeight, juce::Justification::centredLeft);
        g.drawText (row.q,      x3, rowTop, kColQW     - 4.0f, (float) kRowHeight, juce::Justification::centredLeft);
        g.drawText (row.status, x4, rowTop, frame.getRight() - x4 - kLeftPad, (float) kRowHeight, juce::Justification::centredLeft);

        rowTop += (float) kRowHeight;
    }
}

} // namespace gui
