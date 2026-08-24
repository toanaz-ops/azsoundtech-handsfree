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

#include "app/NotchController.h"
#include "dsp/Detector.h"

#include <cstddef>
#include <cstdint>
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

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    // Rebuilds the normalised polyline from snapshot_. Message thread only.
    void rebuildGeometry();

    // Log-frequency / dB mappings into a given plot rectangle.
    static float xForHz (float hz, const juce::Rectangle<float>& plot);
    static float yForDb (float db, const juce::Rectangle<float>& plot);

    const NotchController& controller_;

    // One consistent frame, copied under the controller's snapshot mutex.
    NotchController::SnapshotBuffer snapshot_ {};

    // Normalised [0..1] x/y of the spectrum polyline, pre-sized in the ctor.
    std::vector<juce::Point<float>> spectrumPoints_;
    juce::Path markerPath_;               // reused every paint, never grows

    juce::Font tickFont_;                 // mono: axis numbers
    juce::Font bodyFont_;                 // "no signal" text
    juce::String xTickLabels_[3];         // 100 Hz / 1k / 10k
    juce::String yTickLabels_[4];         // 0 / -30 / -60 / -90 dB
    juce::String noSignalLabel_;

    std::uint64_t seenSequence_     = 0;  // last copied frame
    std::uint64_t drawnSequence_    = 0;  // last frame geometry was built from

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumView)
};

} // namespace gui
