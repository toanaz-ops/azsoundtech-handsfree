// NotchListPanel -- Task 4 of the GUI Console redesign (spec section 2).
//
// A read-only table of the notches currently in the model: # / FREQ / DEPTH /
// Q / STATUS(age). It reads the model EXACTLY through
// NotchController::copySnapshot() (design section 4), exactly like
// SpectrumView -- display only, it NEVER issues commands and never touches
// the audio thread.
//
// Layout discipline (ruling R-1 / spec section 3): this component knows
// NOTHING about the window layout. The parent sets its visibility and bounds
// only; in the single Classic layout (the L1/L2 dual-layout was removed in
// commit 3a04200) MainComponent places it as the fixed left column of the
// bottom floor strip.
//
// Notch AGE (controller ruling R-2): computed GUI-side. The panel tracks a
// steady-clock FIRST-SEEN time per notch identity (channel, index, frequency)
// as snapshots stream in through refreshFromSnapshot(). SnapshotBuffer is NOT
// extended. An identity not seen for kTrackingTimeoutMs (60 s -- generous
// against snapshot hiccups, yet far below the 30 s auto-release horizon plus
// margin where confusion could matter... chosen simply as "long enough that
// no live notch is ever forgotten") is dropped from tracking; if the same
// identity reappears later its age restarts from zero.
//
// Determinism: the clock is injected (ClockFn returning ms). Production uses
// std::chrono::steady_clock; tests substitute a controllable fake.
//
// Paint-path discipline mirrors SpectrumView: all strings are built in
// refreshFromSnapshot() on the message thread; paint() draws pre-built
// members only. Row click -> detail popover is OUT OF SCOPE v1 (spec §7).
//
// Formatting contract (binding):
//   freq  < 1000 Hz : "987 Hz"        >= 1000 Hz : "2.4 kHz"
//   depth           : "−12.0 dB"      (U+2212 minus, mono)
//   Q               : "4.0"
//   age < 60 s      : "12s"           >= 60 s    : "2m ago"

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/NotchController.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace gui
{

class NotchListPanel : public juce::Component,
                       private juce::Timer
{
public:
    // Returns "now" in milliseconds. Default: steady_clock (production).
    using ClockFn = std::function<double()>;

    explicit NotchListPanel (const NotchController& controller,
                             ClockFn nowMs = {});
    ~NotchListPanel() override;

    // Copies one frame out of the controller, updates first-seen tracking,
    // rebuilds the row strings, requests a repaint. Message thread only --
    // driven by the internal timer and by visibilityChanged().
    void refreshFromSnapshot();

    // Point this table at a different slot's detector. The first-seen ledger
    // and the built rows both describe the OLD slot and are dropped: an
    // identity key is only unique WITHIN one detector, so carrying it across
    // would age a new slot's notch from the previous slot's sighting.
    void setController (const NotchController& controller);

    // Which slot the caption names. Display only -- it does not change what is
    // read; setController does that.
    void setDisplayedSlot (int slotIndex);

    // Hosts the slot selector in this panel's caption row (ownership stays
    // with the caller; layout tolerates null). The selector is not owned here
    // because selecting a slot re-points the ANALYSER as well, and this panel
    // has no business knowing that.
    void setSlotTabs (juce::Component* tabsOrNull);

    // Shared formatting truth -- the panel paints these and the tests assert
    // them, so there is exactly one definition of each format.
    static juce::String formatFrequency (float hz);
    static juce::String formatDepthDb (float depthDb);
    static juce::String formatQ (float q);
    static juce::String formatAgeMs (double ageMs);

    // How long an UNSEEN notch identity stays tracked (see header comment).
    static constexpr double kTrackingTimeoutMs = 60000.0;

    // Geometry shared with tests / parent sizing decisions.
    static constexpr int kCaptionHeight = 26;   // section legend + live count
    static constexpr int kHeaderHeight  = 22;   // column captions
    static constexpr int kRowHeight     = 26;

    // TEST ACCESSORS -- the model behind the painted table.
    struct RowText
    {
        juce::String id, freq, depth, q, status;

        // How long this notch has been held, in milliseconds. Carried on the
        // row (not recomputed in paint) because the age drives the row's
        // COLOUR through az::theme::notchColour as well as its status text --
        // and the two must never disagree about how old a notch is.
        double ageMs = 0.0;
    };

    [[nodiscard]] int rowCountForTest() const { return (int) rows_.size(); }
    [[nodiscard]] RowText rowForTest (int index) const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void visibilityChanged() override;

    // Identity key for first-seen tracking: quantised frequency (1 Hz) plus
    // channel and slot. Frequency participates per ruling R-2: a recycled
    // slot set to a new frequency is a NEW notch.
    static std::uint64_t identityKey (std::uint8_t channel, std::uint8_t index, float frequencyHz);

    struct Sighting
    {
        double firstSeenMs = 0.0;
        double lastSeenMs  = 0.0;
    };

    // A POINTER for the same reason SpectrumView's is: the table follows the
    // masthead's slot selector. Never null.
    const NotchController* controller_;
    ClockFn nowMs_;

    NotchController::SnapshotBuffer snapshot_ {};

    // Pre-built row strings, rebuilt every refresh (message thread).
    std::vector<RowText> rows_;

    // An empty table is not an error and not a void: with the detector armed
    // it means the room is behaving. Say that, rather than reporting absence.
    juce::String noNotchesLabel_ { "Nothing ringing" };
    juce::Font   tableFont_;              // mono: every cell holds numbers

    // Rebuilt by setDisplayedSlot so paint() never formats a string.
    juce::String caption_ { "Active notches" };

    juce::Component* slotTabs_ = nullptr;   // NOT owned

    // First-seen ledger keyed by identity (ruling R-2).
    std::map<std::uint64_t, Sighting> sightings_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NotchListPanel)
};

} // namespace gui
