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

#include <cstddef>
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
    // thread: constructor and ComboBox onChange only, never paint).
    void applyBandMode();

    // Log-frequency / dB mappings into a given plot rectangle.
    static float xForHz (float hz, const juce::Rectangle<float>& plot);
    static float yForDb (float db, const juce::Rectangle<float>& plot);

    const NotchController& controller_;

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
    juce::Label      bandwidthLabel_;
    juce::ComboBox   bandwidthBox_;
    juce::Label      averageLabel_;
    juce::ComboBox   averageBox_;
    juce::ToggleButton peakHoldButton_;

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
