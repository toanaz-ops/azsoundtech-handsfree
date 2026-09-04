#include "gui/NotchListPanel.h"

#include "gui/theme/AzTheme.h"

#include <algorithm>
#include <cmath>

namespace gui
{

namespace
{
// The typographic minus of the depth format. Built from a code point rather
// than a string literal so the compiler's execution charset can never mangle
// it (repo rule: UTF-8 survives every read-modify-write).
juce::String minusSign()
{
    return juce::String::charToString ((juce::juce_wchar) 0x2212);
}
} // namespace

NotchListPanel::NotchListPanel (const NotchController& controller, ClockFn nowMs)
    : controller_ (&controller),
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
    controller_->copySnapshot (snapshot_);

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

        rows_.push_back ({ juce::String (i + 1).paddedLeft ('0', 2),
                           notch.channel == 1 ? "R" : "L",
                           formatFrequency (notch.frequency),
                           formatDepthDb (notch.depthDB),
                           formatQ (notch.Q),
                           formatAgeMs (juce::jmax (0.0, ageMs)),
                           juce::jmax (0.0, ageMs) });
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

void NotchListPanel::setController (const NotchController& controller)
{
    if (controller_ == &controller)
        return;

    controller_ = &controller;

    sightings_.clear();
    rows_.clear();

    refreshFromSnapshot();
}

void NotchListPanel::setSlotTabs (juce::Component* tabsOrNull)
{
    slotTabs_ = tabsOrNull;

    if (slotTabs_ != nullptr)
        addAndMakeVisible (*slotTabs_);

    resized();
}

void NotchListPanel::resized()
{
    if (slotTabs_ == nullptr)
        return;

    // Right-aligned in the caption band, opposite the section name. The count
    // chip moves left of it -- see paint().
    auto caption = getLocalBounds().removeFromTop (kCaptionHeight);

    const int wanted = slotTabs_->getWidth() > 0
                           ? slotTabs_->getWidth()
                           : caption.getWidth() / 2;

    slotTabs_->setBounds (caption.removeFromRight (juce::jmin (wanted, caption.getWidth()))
                                 .withSizeKeepingCentre (juce::jmin (wanted, caption.getWidth()),
                                                         az::theme::fieldHeight - 4));
}

void NotchListPanel::setDisplayedSlot (const int slotIndex)
{
    // Named in the caption rather than shown as a separate field: the table
    // has one subject, and "which slot" is part of what it is, not a property
    // of it. The separator is a middle dot, not a colon -- these are two
    // labels, not a label and a value.
    // The separator is built from a CODE POINT, not written into the literal.
    // MSVC reads a source literal through the execution charset, and a middle
    // dot written inline comes back as "A-circumflex, middle dot" on screen --
    // the same trap minusSign() above exists to dodge. Repo rule: UTF-8 has to
    // survive every read-modify-write, and that includes the compiler's read.
    const auto separator = juce::String ("  ")
                         + juce::String::charToString ((juce::juce_wchar) 0x00b7)
                         + juce::String ("  ");

    caption_ = juce::String ("Active notches") + separator
             + juce::String ("Slot ")
             + juce::String (slotIndex + 1).paddedLeft ('0', 2);

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

float NotchListPanel::statusWidthFor (const float frameWidth)
{
    // Mirrors the x4 derivation in paint() exactly (see the geometry
    // comment on the column constants in the header): the fixed columns
    // plus BOTH margins come off the frame, and STATUS/HELD gets whatever
    // remains. Position-independent -- callers pass the panel's WIDTH, not
    // its bounds, so paint() and the test share one formula.
    const float fixedColumnsWidth = kColIdW + kColLaneW + kColFreqW + kColDepthW + kColQW;
    return juce::jmax (0.0f, frameWidth - (2.0f * kLeftPad) - fixedColumnsWidth);
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
    using namespace az::theme;

    g.fillAll (panel);

    auto area = getLocalBounds();

    //--------------------------------------------------------------------
    // Caption band: the section name, and the live count as a chip. The count
    // is the number a soundman actually wants off this panel at a distance --
    // "how many is it holding" -- so it gets the accent and its own outline.
    auto caption = area.removeFromTop (kCaptionHeight);
    drawCaption (g, caption_, caption.withTrimmedLeft ((int) kLeftPad), dim);

    // The count chip sits left of the slot selector when one is hosted here.
    if (slotTabs_ != nullptr)
        caption.removeFromRight (slotTabs_->getWidth() + gap);

    if (! rows_.empty())
    {
        const auto chip = caption.removeFromRight (46).withSizeKeepingCentre (34, 17);

        g.setColour (accent.withAlpha (0.35f));
        g.drawRoundedRectangle (chip.toFloat().reduced (0.5f), cornerRadius, 1.0f);

        g.setColour (accent);
        g.setFont (monoFont (baseFontSize - 2.0f));
        g.drawText (juce::String ((int) rows_.size()), chip,
                    juce::Justification::centred, false);
    }

    drawEngravedDivider (g, caption.withTrimmedBottom (spacing));

    //--------------------------------------------------------------------
    // Column origins. FREQ carries the age dot, so its text starts inset.
    const auto frame = area.toFloat();
    const float x0 = frame.getX() + kLeftPad;
    const float x1 = x0 + kColIdW + kColLaneW;
    const float x2 = x1 + kColFreqW;
    const float x3 = x2 + kColDepthW;
    const float x4 = x3 + kColQW;
    const float statusW = statusWidthFor (frame.getWidth());

    auto header = area.removeFromTop (kHeaderHeight);
    g.setColour (dim);
    g.setFont (legendFont (columnFontSize, true, trackingColumn));
    g.drawText ("#",     (int) x0, header.getY(), (int) kColIdW,    header.getHeight(), juce::Justification::centredLeft);
    g.drawText ("LANE",  (int) (x0 + kColIdW), header.getY(), (int) kColLaneW, header.getHeight(), juce::Justification::centredLeft);
    g.drawText ("FREQ",  (int) x1, header.getY(), (int) kColFreqW,  header.getHeight(), juce::Justification::centredLeft);
    g.drawText ("DEPTH", (int) x2, header.getY(), (int) kColDepthW, header.getHeight(), juce::Justification::centredLeft);
    g.drawText ("Q",     (int) x3, header.getY(), (int) kColQW,     header.getHeight(), juce::Justification::centredLeft);
    g.drawText ("HELD",  (int) x4, header.getY(), (int) statusW,    header.getHeight(), juce::Justification::centredLeft);

    g.setColour (border);
    g.fillRect (area.getX(), header.getBottom(), area.getWidth(), 1);

    //--------------------------------------------------------------------
    // Empty state: an invitation, not a void.
    if (rows_.empty())
    {
        // The legend face, not the body face. This is a LABEL for an empty
        // table, sitting among tracked uppercase legends and monospaced
        // numbers -- setting it in the prose face made it the only sentence on
        // screen and it read as a stray piece of another design.
        g.setColour (dim);
        g.setFont (legendFont (captionFontSize, true, trackingCaption));
        g.drawText (noNotchesLabel_.toUpperCase(), area,
                    juce::Justification::centred, false);
        return;
    }

    //--------------------------------------------------------------------
    // Rows. Every row carries its own age colour on the dot before FREQ, and
    // the HELD column is dim -- the frequency is what gets read, the age only
    // qualifies it.
    float rowTop = (float) area.getY();

    for (const auto& row : rows_)
    {
        if (rowTop + (float) kRowHeight > frame.getBottom())
            break;   // a short column clips rather than painting past its edge

        const auto ageColour = notchColour (row.ageMs);

        g.setFont (tableFont_);

        g.setColour (faded);
        g.drawText (row.id, x0, rowTop, kColIdW - 4.0f, (float) kRowHeight,
                    juce::Justification::centredLeft);
        g.drawText (row.lane, x0 + kColIdW, rowTop, kColLaneW - 4.0f, (float) kRowHeight,
                    juce::Justification::centredLeft);

        // THE RAMP, on a 7 px dot: sodium the instant it fires, ice once it
        // has held. A fresh one also gets a soft ring, so a notch landing
        // during a show catches the eye without anything animating.
        const float heat = notchHeat (row.ageMs);
        const auto  dot  = juce::Rectangle<float> (kDotSize, kDotSize)
                               .withCentre ({ x1 + kDotSize * 0.5f,
                                              rowTop + (float) kRowHeight * 0.5f });
        if (heat > 0.0f)
        {
            g.setColour (ageColour.withAlpha (0.30f * heat));
            g.fillEllipse (dot.expanded (3.0f * heat));
        }
        g.setColour (ageColour);
        g.fillEllipse (dot);

        g.setColour (text);
        g.drawText (row.freq, x1 + kDotGap + kDotSize, rowTop,
                    kColFreqW - kDotGap - kDotSize - 4.0f, (float) kRowHeight,
                    juce::Justification::centredLeft);
        g.drawText (row.depth, x2, rowTop, kColDepthW - 4.0f, (float) kRowHeight,
                    juce::Justification::centredLeft);

        g.setColour (dim);
        g.drawText (row.q, x3, rowTop, kColQW - 4.0f, (float) kRowHeight,
                    juce::Justification::centredLeft);
        g.drawText (row.status, x4, rowTop, statusW - 4.0f, (float) kRowHeight,
                    juce::Justification::centredLeft);

        rowTop += (float) kRowHeight;

        g.setColour (shade);
        g.fillRect (frame.getX(), rowTop, frame.getWidth(), 1.0f);
    }
}

} // namespace gui
