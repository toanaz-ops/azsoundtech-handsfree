// SpectrumView tests -- Task 1 of the GUI Console redesign (spec section 6).
//
// Same headless-component pattern as test_gui_wiring.cpp:
// juce::ScopedJuceInitialiser_GUI brings up the MessageManager; a Component
// never added to the desktop needs nothing else, and paint() is invoked
// directly on a Graphics backed by an Image -- no message loop is pumped
// (JUCE_MODAL_LOOPS_PERMITTED is off).
//
// The snapshot the view consumes comes from a REAL NotchController driven by
// synchronous runOnce() calls (the pattern of StartStopCycleJoinsCleanly in
// test_gui_wiring.cpp), so what reaches the view is the genuinely published
// struct rather than a hand-written lookalike.

#include <gtest/gtest.h>

#include "app/NotchController.h"
#include "dsp/ClockSource.h"
#include "dsp/Detector.h"
#include "dsp/LockFreeRingBuffer.h"
#include "gui/SpectrumView.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

// A controller optionally fed a 1 kHz sine plus one manual notch, then pumped
// until it has published real frames. No thread started, fully synchronous.
struct FedController
{
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tap, commands, clock };

    FedController (const bool withNotch, const bool feedAudio)
    {
        if (withNotch)
            controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                 NotchController::Origin::Manual);

        if (! feedAudio)
            return;

        std::vector<float> hop ((std::size_t) Detector::kHopSize);
        for (std::size_t i = 0; i < hop.size(); ++i)
            hop[i] = std::sin (2.0f * juce::MathConstants<float>::pi
                               * 1000.0f * (float) i / 48000.0f);

        for (int block = 0; block < 8; ++block)
        {
            tap.write (hop.data(), hop.size());
            controller.runOnce();
        }
    }
};

void paintHeadless (juce::Component& component, const int width, const int height)
{
    juce::Image image (juce::Image::ARGB, width, height, true);
    juce::Graphics g (image);
    component.setSize (width, height);
    component.paint (g);   // must simply not crash
}

} // namespace

//==============================================================================
// Test (a): fake snapshot with 513 bins + 1 notch -> refreshFromSnapshot()
// records the sequence and paint does not crash.

TEST (SpectrumView, RefreshSeesThePublishedSequenceAndPaintDoesNotCrash)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    FedController fed (true, true);

    NotchController::SnapshotBuffer snap {};
    fed.controller.copySnapshot (snap);
    ASSERT_EQ (snap.magnitudeCount, (std::uint32_t) Detector::kNumBins);
    ASSERT_EQ (snap.notchCount, 1u);
    ASSERT_GT (snap.sequence, 0u);

    gui::SpectrumView view (fed.controller);
    view.refreshFromSnapshot();

    EXPECT_EQ (view.getSequenceSeen(), snap.sequence);

    // A second refresh on an UNCHANGED buffer must not disturb the recorded
    // sequence (spec section 2: unchanged sequence -> no repaint work).
    view.refreshFromSnapshot();
    EXPECT_EQ (view.getSequenceSeen(), snap.sequence);

    paintHeadless (view, 480, 320);
    SUCCEED();
}

//==============================================================================
// Spec section 5: sequence == 0 draws grid + "no signal", NO markers.

TEST (SpectrumView, EmptyStateWithSequenceZeroPaintsWithoutCrashing)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    FedController idle (false, false);   // no audio fed -> nothing ever published

    NotchController::SnapshotBuffer snap {};
    idle.controller.copySnapshot (snap);
    ASSERT_EQ (snap.sequence, 0u);

    gui::SpectrumView view (idle.controller);
    view.refreshFromSnapshot();

    EXPECT_EQ (view.getSequenceSeen(), 0u);

    paintHeadless (view, 480, 320);
    SUCCEED();
}

//==============================================================================
// Test (b): 100 paints over an unchanged buffer must not grow any member
// container -- the proof that paint() performs no per-frame allocation.

TEST (SpectrumView, HundredPaintsOnAnUnchangedBufferDoNotGrowMemberContainers)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    FedController fed (true, true);

    gui::SpectrumView view (fed.controller);
    view.refreshFromSnapshot();

    // The cache was actually populated -- otherwise the assertions below
    // would pass vacuously against an empty vector.
    ASSERT_GT (view.spectrumPointSizeForTest(), (std::size_t) 0);

    const auto sizeBefore     = view.spectrumPointSizeForTest();
    const auto capacityBefore = view.spectrumPointCapacityForTest();

    juce::Image image (juce::Image::ARGB, 480, 320, true);
    juce::Graphics g (image);
    view.setSize (480, 320);

    for (int paint = 0; paint < 100; ++paint)
        view.paint (g);

    EXPECT_EQ (view.spectrumPointSizeForTest(), sizeBefore);
    EXPECT_EQ (view.spectrumPointCapacityForTest(), capacityBefore);
}

//==============================================================================
// Review fix 1: raw magnitudes above 0 dB must clamp to the TOP of the frame,
// never produce normalised Y < 0 (line escaping into the dB-label gutter).

TEST (SpectrumView, LoudMagnitudesAboveZeroDbClampInsideThePlotFrame)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    // A near-full-scale sine: the FFT is unnormalised, so its peak bin lands
    // far ABOVE 0 dB and would previously have driven ny negative.
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tap, commands, clock };

    std::vector<float> hop ((std::size_t) Detector::kHopSize);
    for (std::size_t i = 0; i < hop.size(); ++i)
        hop[i] = 0.9f * std::sin (2.0f * juce::MathConstants<float>::pi
                                  * 1000.0f * (float) i / 48000.0f);

    for (int block = 0; block < 8; ++block)
    {
        tap.write (hop.data(), hop.size());
        controller.runOnce();
    }

    gui::SpectrumView view (controller);
    view.refreshFromSnapshot();

    ASSERT_GT (view.spectrumPointSizeForTest(), (std::size_t) 0);

    bool sawTopClamp = false;
    for (std::size_t i = 0; i < view.spectrumPointSizeForTest(); ++i)
    {
        const auto p = view.spectrumPointForTest (i);
        EXPECT_GE (p.y, 0.0f) << "point " << i << " escaped ABOVE the frame";
        EXPECT_LE (p.y, 1.0f) << "point " << i << " fell BELOW the frame";
        if (p.y == 0.0f)
            sawTopClamp = true;
    }

    // The clamp actually ENGAGED for this loud signal -- without it at least
    // one point would sit strictly above 0.
    EXPECT_TRUE (sawTopClamp);
}
