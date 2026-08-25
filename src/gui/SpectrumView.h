// SpectrumView -- Task 1 of the GUI Console redesign (spec section 2).
//
// The main canvas: a log-frequency plot of the detector's published spectrum
// with orange v markers at every active notch. It reads the model EXACTLY
// through NotchController::copySnapshot() (design section 4: the GUI never
// reaches past that mutex into detector internals, and nothing here touches
// the audio thread).
//
// Paint-path discipline (binding for Task 21):
//   paint() performs no heap allocation. Everything it draws comes from
//   members pre-sized in the constructor:
//     - snapshot_:        fixed std::arrays, copied wholesale by refresh
//     - spectrumPoints_:  reserved once for kNumBins entries; rebuilds use
//                         clear(), which keeps the capacity
//     - markerPath_:      one juce::Path reused across paints -- Path::clear()
//                         resets its element count without releasing storage,
//                         so re-adding triangles writes into owned memory
//   Geometry is rebuilt in refreshFromSnapshot() (the message thread), and
//   points are stored NORMALISED to the plot rectangle, so a window resize
//   stays correct without a rebuild inside paint().
//
// Axes:
//   X: logarithmic 20 Hz..20 kHz, labelled ticks at 100 Hz / 1 kHz / 10 kHz
//      drawn in the theme's monospaced font.
//   Y: fixed -90..0 dB window. Documented scale choice: raw FFT magnitudes
//      converted with 20*log10 and clamped into the window. A fixed window
//      keeps the plot stable frame-to-frame instead of autoscaling on every
//      transient; raw detector magnitudes routinely reach or exceed 0 dB on
//      feedback tones, which then clamp at the top edge -- visible and
//      honest rather than rescaled away.
//
// Empty state (spec section 5): sequence == 0 draws grid + dim "no signal",
// no spectrum line, no markers.

#pragma once

// Module headers rather than <JuceHeader.h>: same reason as ModeBar.h --
// JuceHeader.h only exists for juce_add_* targets, so including it here would
// break compilation into the plain add_executable test target.
#include <juce_gui_basics/juce_gui_basics.h>
#include <melatonin_blur/melatonin_blur.h>

#include "app/NotchController.h"
#include "dsp/Detector.h"
#include "gui/RtaProcessing.h"

#include <array>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <map>
#include <vector>

namespace gui
{

class SpectrumView : public juce::Component,
                     private juce::Timer
{
public:
    // Axis ranges. Kept here so tests and future overlays share one truth.
    static constexpr float kMinHz = 20.0f;
    static constexpr float kMaxHz = 20000.0f;
    static constexpr float kMinDb = -90.0f;
    static constexpr float kMaxDb = 0.0f;

    explicit SpectrumView (const NotchController& controller);
    ~SpectrumView() override;

    // Copies one consistent frame out of the controller. When the frame is
    // new (sequence advanced past the last drawn one) the cached geometry is
    // rebuilt and a repaint requested; an unchanged sequence costs one mutex-
    // guarded struct copy and nothing else (spec section 2: skip repaint).
    void refreshFromSnapshot();

    // Point this view at a different slot's detector. Everything derived from
    // the OLD slot is dropped: the age ledger (a notch at 1 kHz on slot 1 is
    // not the notch at 1 kHz on slot 2), the polylines, and the sequence
    // watermark that suppresses redundant rebuilds. Without that reset the
    // first frame after a switch would either be skipped as "unchanged" or
    // drawn with the previous slot's notch ages.
    void setController (const NotchController& controller);

    // Sequence number of the most recently COPIED snapshot (whether or not it
    // was new). Test hook and cheap liveness probe.
    [[nodiscard]] std::uint64_t getSequenceSeen() const { return seenSequence_; }

    // TEST ACCESSORS ONLY -- they exist so the no-allocation-in-paint rule is
    // provable: 100 paints on an unchanged buffer must leave both the size
    // and the capacity of the point cache untouched.
    [[nodiscard]] std::size_t spectrumPointSizeForTest() const { return spectrumPoints_.size(); }
    [[nodiscard]] std::size_t spectrumPointCapacityForTest() const { return spectrumPoints_.capacity(); }
    [[nodiscard]] juce::Point<float> spectrumPointForTest (std::size_t index) const
    {
        return spectrumPoints_[index];
    }

    //==========================================================================
    // RING RISK -- how close the room is to ringing right now.
    //
    // THE DATA SOURCE DOES NOT EXIST YET. NotchController::SnapshotBuffer
    // publishes magnitudes and placed notches, and nothing that scores how
    // close the room is to howling. Wiring this to something derived GUI-side
    // would put a SECOND, different peakiness number on screen next to the
    // detector's own, which is worse than showing nothing.
    //
    // So the control is built, laid out and painted, and `ringRiskProvider`
    // is null until the DSP side publishes a score. Null renders as
    // Unavailable -- an explicit "n/a", never a reassuring "low".
    //
    // Contract for whoever fills this in: docs/spec-ring-risk.md.
    enum class RingRisk { Unavailable, Low, Rising, Critical };

    // Polled once per frame by the same timer that refreshes the plot. Null
    // means Unavailable. Must not block: it runs on the message thread.
    std::function<RingRisk()> ringRiskProvider;

    // What the readout currently shows. Exposed so a headless test can assert
    // the honest default without reaching into paint().
    [[nodiscard]] RingRisk getRingRisk() const { return ringRisk_; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    // Rebuilds the normalised polyline from snapshot_. Message thread only.
    void rebuildGeometry();

    // NOTCH AGE, GUI-side (controller ruling R-2: SnapshotBuffer is not
    // extended). Same ledger shape NotchListPanel keeps, and deliberately the
    // same identity key: a stem in the plot and a row in the table must agree
    // about how old the notch they both describe is, because both colour
    // themselves from it through az::theme::notchColour.
    static std::uint64_t notchKey (std::uint8_t channel, std::uint8_t index, float hz);
    void updateNotchAges();

    // How long ONE notch of the current snapshot has been held, from the
    // ledger above. 0 for a notch this frame is the first to carry.
    [[nodiscard]] double ageMsOf (const NotchController::SnapshotNotch& notch,
                                  double nowMs) const;

    // Re-derives bandCenterHz_ / edges for the current bandMode_ (message
    // thread: constructor and chip clicks only, never paint).
    void applyBandMode();

    // The chip's word and colour for a state, in one place so paint() reads as
    // layout rather than as a switch wrapped around drawing calls.
    [[nodiscard]] static juce::String ringRiskLabel (RingRisk);
    [[nodiscard]] static juce::Colour ringRiskColour (RingRisk);

    RingRisk ringRisk_ = RingRisk::Unavailable;

    // Reserved at the toolbar's left, after the ANALYSER caption.
    static constexpr int kRiskLegendWidth = 74;
    static constexpr int kRiskChipWidth   = 78;

    juce::Rectangle<int> riskChipArea_;

    // Lights exactly one chip of a segmented group and clears the rest.
    // Explicit rather than leaning on the radio group, for the reason
    // ModeRail::setDisplayedMode documents: the group's own clearing runs
    // through the notification path this is trying not to fire.
    template <std::size_t N>
    static void reflectChips (std::array<juce::TextButton, N>& chips, int selected);

    // Log-frequency / dB mappings into a given plot rectangle.
    static float xForHz (float hz, const juce::Rectangle<float>& plot);
    static float yForDb (float db, const juce::Rectangle<float>& plot);

    // A POINTER, not a reference: the view follows whichever routing slot the
    // masthead's selector is on, and a reference cannot be re-seated. Never
    // null -- the constructor takes a reference and setController takes one.
    const NotchController* controller_;

    // One consistent frame, copied under the controller's snapshot mutex.
    NotchController::SnapshotBuffer snapshot_ {};

    // Normalised [0..1] x/y of the spectrum polyline, pre-sized in the ctor.
    std::vector<juce::Point<float>> spectrumPoints_;

    // All four reused every paint and cleared rather than rebuilt: Path::clear
    // resets the element count without releasing storage, so re-adding writes
    // into memory this object already owns. That is the no-allocation
    // guarantee the paint path is held to.
    juce::Path markerPath_;
    juce::Path tracePath_;
    juce::Path fillPath_;

    // Tuning-toolbar state (display-only: changing these never restarts the
    // audio engine, detector or notch chain).
    rta::BandMode bandMode_       { rta::BandMode::Line };
    rta::AverageMode avgMode_     { rta::AverageMode::Off };
    bool peakHold_                = false;

    // Per-frame display buffers, reserved once in the constructor. newDb_ is
    // this frame's raw dB; displayDb_ is what gets drawn (raw or EMA-smoothed
    // towards it); peakDb_ is the decaying peak-hold trace; displayLin_
    // mirrors displayDb_ back to linear so rta::bandLevelsDb can power-sum
    // it; bandLevelsDb_ holds one level per active octave band.
    std::vector<float> newDb_;
    std::vector<float> displayDb_;
    std::vector<float> peakDb_;
    std::vector<float> displayLin_;
    std::vector<float> bandLevelsDb_;

    // Band geometry for the octave-bar modes, recomputed on mode change only:
    // centers plus each band's lower/upper -3 dB-style edges in Hz.
    std::vector<float> bandCenterHz_;
    std::vector<float> bandEdgeLowHz_;
    std::vector<float> bandEdgeHighHz_;

    // Peak-hold trace, normalised exactly like spectrumPoints_. Empty unless
    // peak hold is on.
    std::vector<juce::Point<float>> peakPoints_;
    bool peakHoldWasOn_ = false;          // seeds peakDb_ on the rising edge

    // Toolbar controls. The view paints its plot around them; they live in a
    // thin strip across the top (kToolbarHeight px, reserved in resized()).
    static constexpr int kToolbarHeight = 32;

    // Reserved at the toolbar's left for the ANALYSER legend, by BOTH paint()
    // (which draws it) and resized() (which must not lay a control over it).
    static constexpr int kCaptionWidth  = 78;
    // Segmented chips, not dropdowns. These are DISPLAY options a soundman
    // flips while looking at the plot, so the current one has to be readable
    // without opening anything, and the next one has to be one click away.
    // A 26 px combo is neither.
    juce::Label      bandwidthLabel_;
    std::array<juce::TextButton, 3> bandChips_ { juce::TextButton { "Line" },
                                                 juce::TextButton { "1/1 oct" },
                                                 juce::TextButton { "1/3 oct" } };
    juce::Label      averageLabel_;
    std::array<juce::TextButton, 4> avgChips_ { juce::TextButton { "Off" },
                                                juce::TextButton { "1s" },
                                                juce::TextButton { "3s" },
                                                juce::TextButton { "10s" } };
    // A chip like the segmented groups beside it, not a tick box: it belongs to
    // the same row of display options and is hit the same way.
    juce::TextButton peakHoldButton_ { "Peak hold" };

    juce::Font tickFont_;                 // mono: axis numbers
    juce::Font bodyFont_;                 // "no signal" text
    juce::String xTickLabels_[7];         // 50 Hz .. 10 kHz
    juce::String yTickLabels_[4];         // 0 / -30 / -60 / -90 dB
    juce::String noSignalLabel_;

    std::uint64_t seenSequence_     = 0;  // last copied frame
    std::uint64_t drawnSequence_    = 0;  // last frame geometry was built from

    // identity -> first-seen ms (see updateNotchAges). Bounded by the notch
    // capacity of the snapshot; stale identities are dropped each refresh.
    std::map<std::uint64_t, double> firstSeenMs_;

    // THE fresh-notch halo. A real gaussian rather than a radial gradient: a
    // gradient halo bands visibly around a small bright shape on a near-black
    // ground, which is exactly the case here. melatonin caches the blur, and
    // the path is only rebuilt while a notch is still hot -- typically zero
    // or one of them, for a couple of seconds after it fires.
    melatonin::DropShadow notchGlow_ { juce::Colours::transparentBlack, 26 };
    juce::Path glowPath_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumView)
};

} // namespace gui
