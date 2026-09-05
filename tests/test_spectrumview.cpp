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
#include "gui/RtaProcessing.h"
#include "gui/SpectrumView.h"
#include "test_gui_helpers.h"

using gui_test::paintHeadless;

#include <cmath>
#include <cstdint>
#include <limits>
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

//==============================================================================
// RtaProcessing -- the display-side maths behind the tuning toolbar
// (bandwidth / averaging / peak hold). Header-only and GUI-free, so these
// run without the ScopedJuceInitialiser_GUI.

TEST (RtaProcessing, AverageAlphaOffIsOneAndLongerConstantsAreSmoother)
{
    // Off means "replace", not smooth: alpha 1.0.
    EXPECT_FLOAT_EQ (rta::averageAlpha (rta::AverageMode::Off, 30.0f), 1.0f);

    // 1 s at ~30 fps must be a gentle pull, not a near-copy...
    const float a1 = rta::averageAlpha (rta::AverageMode::S1, 30.0f);
    EXPECT_GT (a1, 0.0f);
    EXPECT_LT (a1, 0.2f);

    // ...and the longer the time constant, the smaller the per-frame weight.
    const float a3  = rta::averageAlpha (rta::AverageMode::S3, 30.0f);
    const float a10 = rta::averageAlpha (rta::AverageMode::S10, 30.0f);
    EXPECT_LT (a3, a1);
    EXPECT_LT (a10, a3);

    // A degenerate frame rate must not produce NaN or a negative weight.
    EXPECT_FLOAT_EQ (rta::averageAlpha (rta::AverageMode::S1, 0.0f), 1.0f);
}

TEST (RtaProcessing, BandCentersMatchTheStandardSeries)
{
    const auto oct1 = rta::bandCenters (rta::BandMode::Octave1, 20.0f, 20000.0f);
    ASSERT_EQ (oct1.size(), (std::size_t) 10);
    EXPECT_FLOAT_EQ (oct1.front(), 31.5f);
    EXPECT_FLOAT_EQ (oct1.back(), 16000.0f);

    const auto oct3 = rta::bandCenters (rta::BandMode::Octave3, 20.0f, 20000.0f);
    ASSERT_EQ (oct3.size(), (std::size_t) 31);
    EXPECT_FLOAT_EQ (oct3.front(), 20.0f);
    EXPECT_FLOAT_EQ (oct3.back(), 20000.0f);

    for (const auto* centers : { &oct1, &oct3 })
    {
        for (std::size_t i = 0; i < centers->size(); ++i)
        {
            EXPECT_GE ((*centers)[i], 20.0f) << "index " << i;
            EXPECT_LE ((*centers)[i], 20000.0f) << "index " << i;
            if (i > 0)
                EXPECT_GT ((*centers)[i], (*centers)[i - 1]) << "index " << i;
        }
    }

    // Line mode has no bands at all.
    EXPECT_TRUE (rta::bandCenters (rta::BandMode::Line, 20.0f, 20000.0f).empty());
}

TEST (RtaProcessing, BandLevelsSumPowerWithinABand)
{
    // hzPerBin = 10 Hz: bin 100 sits at exactly 1 kHz, inside the 1 kHz octave
    // band (edges 707..1414 Hz). Band index of the 1 kHz center is 5
    // (31.5 .. 16000 series).
    std::vector<float> mags (512, 0.0f);
    const float mag = 0.5f;   // -6.02 dB
    mags[100] = mag;

    std::vector<float> out ((std::size_t) 10, -999.0f);
    rta::bandLevelsDb (rta::BandMode::Octave1, mags.data(), (int) mags.size(),
                       10.0f, 20.0f, 20000.0f, out.data());

    const float singleBinDb = 20.0f * std::log10 (mag);
    EXPECT_NEAR (out[5], singleBinDb, 0.01f);

    // Bands with no energy report the documented silence floor, and bins
    // outside [minHz, maxHz] are ignored (bin 2100 -> 21 kHz > maxHz).
    EXPECT_FLOAT_EQ (out[0], rta::kSilenceDb);

    // A second equal bin in the SAME band adds in POWER: +10*log10(2) dB.
    mags[110] = mag;   // 1.1 kHz, still inside 707..1414 Hz
    rta::bandLevelsDb (rta::BandMode::Octave1, mags.data(), (int) mags.size(),
                       10.0f, 20.0f, 20000.0f, out.data());
    EXPECT_NEAR (out[5], singleBinDb + 10.0f * std::log10 (2.0f), 0.01f);
}

//==============================================================================
// THE DISPLAY RANGE (2026-08-25).
//
// The analyser used to be pinned to 20 Hz - 20 kHz. A room howls between
// roughly 100 Hz and 12 kHz, so most of the axis was spent on octaves nothing
// ever rings in and every notch crowded into the middle third. The window is
// now the operator's to choose, by dragging either end of the axis gutter or
// by typing into the two fields beside it.

TEST (SpectrumView, TheDefaultWindowIsTheRangeARoomActuallyRingsIn)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    FedController fed (false, false);
    gui::SpectrumView view (fed.controller);

    EXPECT_FLOAT_EQ (view.getDisplayLowHz(),  gui::SpectrumView::kDefaultLowHz);
    EXPECT_FLOAT_EQ (view.getDisplayHighHz(), gui::SpectrumView::kDefaultHighHz);

    // And it is a sub-range of the absolute axis, not equal to it.
    EXPECT_GT (view.getDisplayLowHz(),  gui::SpectrumView::kMinHz);
    EXPECT_LT (view.getDisplayHighHz(), gui::SpectrumView::kMaxHz);
}

TEST (SpectrumView, ARangeIsClampedOrderedAndKeptWideEnoughToRead)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    FedController fed (false, false);
    gui::SpectrumView view (fed.controller);

    // Past the absolute limits: clamped, not rejected.
    view.setDisplayRange (1.0f, 40000.0f);
    EXPECT_FLOAT_EQ (view.getDisplayLowHz(),  gui::SpectrumView::kMinHz);
    EXPECT_FLOAT_EQ (view.getDisplayHighHz(), gui::SpectrumView::kMaxHz);

    // Handed backwards: ordered rather than ignored. Dragging one edge past
    // the other is a normal gesture, not an error.
    view.setDisplayRange (8000.0f, 200.0f);
    EXPECT_LT (view.getDisplayLowHz(), view.getDisplayHighHz());
    EXPECT_FLOAT_EQ (view.getDisplayLowHz(),  200.0f);
    EXPECT_FLOAT_EQ (view.getDisplayHighHz(), 8000.0f);

    // Collapsed to nothing: pushed back apart. Below about an octave and a
    // half the log scale stops being readable and the notch stems merge.
    view.setDisplayRange (1000.0f, 1001.0f);
    const float span = view.getDisplayHighHz() / view.getDisplayLowHz();
    EXPECT_GE (span, std::exp2 (gui::SpectrumView::kMinSpanOctaves) - 0.01f);
}

TEST (SpectrumView, TheRangeFieldsAcceptWhatASoundmanWouldType)
{
    // Bare hertz, a k suffix, a decimal k, and the unit typed out -- all of
    // which somebody will type, none of which should need a manual.
    EXPECT_FLOAT_EQ (gui::SpectrumView::parseFrequency ("60"),     60.0f);
    EXPECT_FLOAT_EQ (gui::SpectrumView::parseFrequency ("16k"), 16000.0f);
    EXPECT_FLOAT_EQ (gui::SpectrumView::parseFrequency ("1.25k"), 1250.0f);
    EXPECT_FLOAT_EQ (gui::SpectrumView::parseFrequency (" 250 Hz "), 250.0f);

    // Unreadable input reports 0, which the editor reads as "keep what you
    // had" -- no dialog, no snapping the axis somewhere nobody asked for.
    EXPECT_FLOAT_EQ (gui::SpectrumView::parseFrequency (""),       0.0f);
    EXPECT_FLOAT_EQ (gui::SpectrumView::parseFrequency ("banana"), 0.0f);
    EXPECT_FLOAT_EQ (gui::SpectrumView::parseFrequency ("-400"),   0.0f);
}

TEST (SpectrumView, TheRangeFieldsShowRoundNumbersBack)
{
    EXPECT_EQ (gui::SpectrumView::formatFrequency (60.0f).toStdString(),    "60");
    EXPECT_EQ (gui::SpectrumView::formatFrequency (16000.0f).toStdString(), "16k");
    EXPECT_EQ (gui::SpectrumView::formatFrequency (1250.0f).toStdString(),  "1.25k");

    // Round trip: what the field shows must parse back to what it shows.
    for (const float hz : { 60.0f, 250.0f, 1250.0f, 12000.0f, 16000.0f })
        EXPECT_FLOAT_EQ (gui::SpectrumView::parseFrequency (
                             gui::SpectrumView::formatFrequency (hz)), hz);
}

//==============================================================================
// The averaging ladder gained short times (2026-08-25). A soundman chasing a
// ring needs 0.1-0.5 s; the long ones are for reading a room and moved behind
// a combo at the end of the group.

TEST (RtaProcessing, ShorterAveragingReactsFaster)
{
    // The property that matters is the ORDER, not the individual numbers: a
    // shorter time constant must always weight the newest frame more heavily,
    // or the labels on the toolbar lie about what they do.
    const float fps = 30.0f;

    const float off   = rta::averageAlpha (rta::AverageMode::Off,  fps);
    const float a0_1  = rta::averageAlpha (rta::AverageMode::S0_1, fps);
    const float a0_3  = rta::averageAlpha (rta::AverageMode::S0_3, fps);
    const float a0_5  = rta::averageAlpha (rta::AverageMode::S0_5, fps);
    const float a1    = rta::averageAlpha (rta::AverageMode::S1,   fps);
    const float a3    = rta::averageAlpha (rta::AverageMode::S3,   fps);
    const float a5    = rta::averageAlpha (rta::AverageMode::S5,   fps);
    const float a10   = rta::averageAlpha (rta::AverageMode::S10,  fps);

    EXPECT_FLOAT_EQ (off, 1.0f);          // Off takes the new frame whole
    EXPECT_GT (off,  a0_1);
    EXPECT_GT (a0_1, a0_3);
    EXPECT_GT (a0_3, a0_5);
    EXPECT_GT (a0_5, a1);
    EXPECT_GT (a1,   a3);
    EXPECT_GT (a3,   a5);
    EXPECT_GT (a5,   a10);

    // Every one of them is a usable weight, not a degenerate 0 or 1.
    for (const float alpha : { a0_1, a0_3, a0_5, a1, a3, a5, a10 })
    {
        EXPECT_GT (alpha, 0.0f);
        EXPECT_LT (alpha, 1.0f);
    }
}

TEST (RtaProcessing, TheFirstFiveModesAreTheToolbarsSegmentsInOrder)
{
    // SpectrumView casts a segment index straight to an AverageMode, so the
    // enum's first five entries ARE the five quick segments. If that ordering
    // ever changes, clicking "0.3 s" would silently select something else.
    EXPECT_EQ (static_cast<int> (rta::AverageMode::Off),  0);
    EXPECT_EQ (static_cast<int> (rta::AverageMode::S0_1), 1);
    EXPECT_EQ (static_cast<int> (rta::AverageMode::S0_3), 2);
    EXPECT_EQ (static_cast<int> (rta::AverageMode::S0_5), 3);
    EXPECT_EQ (static_cast<int> (rta::AverageMode::S1),   4);
}

//==============================================================================
// Task 9: the L/R display-lane selector (spec test 20). A stereo controller
// publishes per-lane magnitudes; the toolbar's laneGroup_ picks which lane
// rebuildGeometry() reads, and is disabled entirely for a mono controller.

TEST (SpectrumView, LaneSelectorPlotsTheChosenLaneAndIsDisabledForMono)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LockFreeRingBuffer<float> tapL { 8192 }, tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController stereo { tapL, &tapR, commands, clock };

    // Loud left, quiet right, a few hops so both detectors publish.
    std::vector<float> loud (Detector::kHopSize, 0.5f), quiet (Detector::kHopSize, 0.0005f);
    for (int i = 0; i < 6; ++i) { tapL.write (loud.data(), loud.size()); tapR.write (quiet.data(), quiet.size()); stereo.runOnce(); }

    gui::SpectrumView view (stereo);
    view.setSize (800, 400);
    view.refreshFromSnapshot();
    EXPECT_TRUE (view.getLaneGroupForTest().isEnabled());
    EXPECT_EQ (view.getDisplayLane(), 0);
    const auto leftPoint = view.spectrumPointForTest (0);

    view.setDisplayLane (1);
    view.refreshFromSnapshot();
    const auto rightPoint = view.spectrumPointForTest (0);
    // rebuildGeometry() maps ny = (kMaxDb - displayDb) / (kMaxDb - kMinDb), so
    // a QUIETER (more negative dB) lane produces a LARGER ny -- and y grows
    // downward in this normalised space, i.e. plots LOWER on screen.
    EXPECT_GT (rightPoint.y, leftPoint.y) << "quiet lane plots lower (y grows downward)";

    LockFreeRingBuffer<float> tapMono { 8192 };
    NotchController mono { tapMono, commands, clock };
    view.setController (mono);
    view.refreshFromSnapshot();
    EXPECT_FALSE (view.getLaneGroupForTest().isEnabled());
    EXPECT_EQ (view.getDisplayLane(), 0);
}

//==============================================================================
// Review finding on Task 9 (ad256c2): no test constructed a channel==1 notch
// and called paint(), so (1) dashedStemPath_ -- the scratch member
// createDashedStroke writes the lane-1 dashed stem into -- was never
// exercised by the no-alloc proof, and (2) the dashed-stem / widened-"R"-flag
// branch itself had no assertion behind it. This test closes both gaps.
//
// Red if the lane-1 stem path is rebuilt through a fresh juce::Path per
// paint, or if createDashedStroke grows the member past its reservation.
TEST (SpectrumView, LaneOneStemsPaintDashedWithoutGrowingTheDashedPath)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LockFreeRingBuffer<float> tapL { 8192 }, tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController stereo { tapL, &tapR, commands, clock };

    // One notch per lane, at DIFFERENT slot indices, so the snapshot really
    // carries a channel-1 notch alongside a channel-0 one rather than both
    // coincidentally landing in slot 0.
    ASSERT_TRUE (stereo.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    ASSERT_TRUE (stereo.setNotch (1, 3, 2000.0, 30.0, -12.0, NotchController::Origin::Manual));

    std::vector<float> hop ((std::size_t) Detector::kHopSize, 0.1f);
    for (int block = 0; block < 6; ++block)
    {
        tapL.write (hop.data(), hop.size());
        tapR.write (hop.data(), hop.size());
        stereo.runOnce();
    }

    gui::SpectrumView view (stereo);
    view.setSize (800, 400);
    view.refreshFromSnapshot();

    // The snapshot really carries a channel-1 notch -- otherwise every
    // assertion below would pass vacuously against a paint() that never took
    // the dashed-stem branch at all.
    ASSERT_EQ (view.snapshotNotchCountForTest(), 2u);
    bool sawChannelOne = false;
    for (std::uint32_t i = 0; i < view.snapshotNotchCountForTest(); ++i)
        sawChannelOne = sawChannelOne || (view.snapshotNotchForTest (i).channel == 1);
    ASSERT_TRUE (sawChannelOne);
    ASSERT_GT (view.spectrumPointSizeForTest(), (std::size_t) 0);

    juce::Image image (juce::Image::ARGB, 800, 400, true);
    juce::Graphics g (image);

    // Warm-up paint: this is what first populates dashedStemPath_'s content.
    view.paint (g);

    // The path was actually used -- otherwise the "unchanged after 100 more
    // paints" assertions below would pass vacuously against an empty path.
    const auto elementsBefore = view.dashedStemPathElementCountForTest();
    ASSERT_GT (elementsBefore, (std::size_t) 0);

    // REAL reservation assertion (round 1 of this task shipped
    // preallocateSpace(256), which round 2 found undersized by >10x --
    // measured 288 elements at this exact window size). The ctor now
    // reserves kDashedStemReserveFloats coords, sized from measured
    // elements-per-pixel-of-stem-height projected to a 2000px-tall plot
    // plus a 25% margin -- see the derivation comment on the constant in
    // SpectrumView.h. Dividing by 3 converts JUCE's coords back to the
    // element count this accessor reports (juce_Path.h's own doc comment:
    // preallocateSpace reserves ~3 floats per lineTo/startNewSubPath
    // element). RED if the reservation is cut below what a full-height stem
    // needs: this assertion fails the moment kDashedStemReserveFloats drops
    // back toward 256, exactly the bug this round fixes.
    EXPECT_LE (elementsBefore,
               (std::size_t) gui::SpectrumView::kDashedStemReserveFloats / 3);

    const auto boundsBefore   = view.dashedStemPathBoundsForTest();
    const auto sizeBefore     = view.spectrumPointSizeForTest();
    const auto capacityBefore = view.spectrumPointCapacityForTest();

    for (int paint = 0; paint < 100; ++paint)
        view.paint (g);

    // dashedStemPath_ redrew the identical dashed stem 100 more times without
    // its element count or extent moving -- the same "no growth" claim the
    // HundredPaints test above proves for spectrumPoints_, now proven for the
    // lane-1 scratch path too.
    EXPECT_EQ (view.dashedStemPathElementCountForTest(), elementsBefore);
    EXPECT_EQ (view.dashedStemPathBoundsForTest(), boundsBefore);
    EXPECT_EQ (view.spectrumPointSizeForTest(), sizeBefore);
    EXPECT_EQ (view.spectrumPointCapacityForTest(), capacityBefore);

    // Same reservation check again at the worst-case size the derivation
    // targets: a 2000px-tall (4K-monitor) plot. A resize forces
    // rebuildGeometry() and the dashed stem is redrawn full-height inside
    // this taller window on the very next paint -- if the reservation were
    // undersized for THIS height (not just 400px), this is where it would
    // show up as a real first-paint growth on the taller plot.
    view.setSize (800, 2000);
    view.refreshFromSnapshot();
    juce::Image tallImage (juce::Image::ARGB, 800, 2000, true);
    juce::Graphics tallG (tallImage);
    view.paint (tallG);

    const auto elementsAtFourK = view.dashedStemPathElementCountForTest();
    ASSERT_GT (elementsAtFourK, (std::size_t) 0);
    EXPECT_GT (elementsAtFourK, elementsBefore);   // taller plot, longer stem, more dashes
    EXPECT_LE (elementsAtFourK,
               (std::size_t) gui::SpectrumView::kDashedStemReserveFloats / 3);
}

//==============================================================================
// RING RISK -- score -> state banding, and the anti-flicker hold.
//
// riskForScore() is a pure function of a SnapshotBuffer, and the hysteresis
// takes "now" as an argument, so neither of these needs a component, a
// message loop, or a wall-clock sleep. The banding is deliberately exercised
// against a NON-DEFAULT threshold: an implementation that hardcoded 0.7 (or
// the old 10.0 from the pre-amendment spec) fails these.

namespace
{
// A snapshot carrying nothing but the three ring-risk fields -- everything
// riskForScore is allowed to look at.
NotchController::SnapshotBuffer riskSnapshot (const bool valid,
                                              const float score,
                                              const float threshold)
{
    NotchController::SnapshotBuffer snapshot {};
    snapshot.ringRiskValid     = valid;
    snapshot.ringRiskScore     = score;
    snapshot.ringRiskThreshold = threshold;
    return snapshot;
}

using Risk = gui::SpectrumView::RingRisk;
} // namespace

TEST (SpectrumView, RingRiskBandsAgainstThePublishedThresholdNotAHardcodedOne)
{
    // Deliberately NOT CandidateScorer::kConfirmScore: every expectation below
    // differs from what a hardcoded 0.7 would produce.
    const float thr  = 0.42f;
    const float rise = 0.55f * thr;   // same expression the mapping must use

    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, 0.0f, thr)),
               Risk::Low);
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, rise * 0.5f, thr)),
               Risk::Low);

    // Exact lower boundary belongs to Rising (>= 0.55x thr).
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, rise, thr)),
               Risk::Rising);
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, 0.3f, thr)),
               Risk::Rising);   // hardcoded 0.7 would call this Low

    // Exact threshold belongs to Critical (>= thr): "about to place a notch".
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, thr, thr)),
               Risk::Critical);
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, 0.5f, thr)),
               Risk::Critical);   // hardcoded 0.7 would call this Rising
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, 1.0f, thr)),
               Risk::Critical);

    // And it tracks a DIFFERENT published threshold with the same code path.
    const float thr2 = 0.7f;
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, 0.3f, thr2)),
               Risk::Low);
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, 0.5f, thr2)),
               Risk::Rising);
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, 0.7f, thr2)),
               Risk::Critical);
}

TEST (SpectrumView, RingRiskIsUnavailableWheneverTheNumberCannotBeTrusted)
{
    // Detector not scoring this frame: never render 0.0 as "low".
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (false, 0.0f, 0.7f)),
               Risk::Unavailable);
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (false, 0.95f, 0.7f)),
               Risk::Unavailable);

    // A NaN score bands into nothing; reporting it as Low would be a lie and
    // reporting it as Critical a false alarm.
    EXPECT_EQ (gui::SpectrumView::riskForScore (
                   riskSnapshot (true, std::numeric_limits<float>::quiet_NaN(), 0.7f)),
               Risk::Unavailable);

    // No usable band line published -> nothing to compare against. With a
    // threshold of 0 every score would read Critical, which is worse noise
    // than an honest N/A.
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, 0.1f, 0.0f)),
               Risk::Unavailable);
    EXPECT_EQ (gui::SpectrumView::riskForScore (riskSnapshot (true, 0.1f, -1.0f)),
               Risk::Unavailable);
    EXPECT_EQ (gui::SpectrumView::riskForScore (
                   riskSnapshot (true, 0.1f, std::numeric_limits<float>::quiet_NaN())),
               Risk::Unavailable);
}

TEST (SpectrumView, RingRiskStepsUpImmediately)
{
    gui::SpectrumView::RingRiskHysteresis hold;

    EXPECT_EQ (hold.apply (Risk::Low, 1000.0), Risk::Low);
    // Same millisecond: a rise must never wait. A warning that arrives late
    // is not a warning.
    EXPECT_EQ (hold.apply (Risk::Critical, 1000.0), Risk::Critical);
}

TEST (SpectrumView, RingRiskHoldsAStepDownForTheHoldTimeThenFalls)
{
    gui::SpectrumView::RingRiskHysteresis hold;

    ASSERT_EQ (hold.apply (Risk::Low, 1000.0), Risk::Low);
    ASSERT_EQ (hold.apply (Risk::Critical, 2000.0), Risk::Critical);

    // Inside the hold: the raw band collapses to Low, the readout does not.
    EXPECT_EQ (hold.apply (Risk::Low, 2001.0),   Risk::Critical);
    EXPECT_EQ (hold.apply (Risk::Rising, 2400.0), Risk::Critical);
    EXPECT_EQ (hold.apply (Risk::Low, 2749.0),   Risk::Critical);

    // At exactly the hold time it is free to fall, straight to the raw band.
    EXPECT_EQ (hold.apply (Risk::Low, 2750.0), Risk::Low);
}

TEST (SpectrumView, RingRiskDoesNotFlickerWhileAScoreOscillatesAcrossABoundary)
{
    gui::SpectrumView::RingRiskHysteresis hold;

    ASSERT_EQ (hold.apply (Risk::Rising, 0.0), Risk::Rising);
    ASSERT_EQ (hold.apply (Risk::Critical, 100.0), Risk::Critical);

    // 30 fps of a score sitting exactly on the Rising/Critical line for the
    // rest of the hold: 21 frames, not one state change.
    double now = 100.0;
    for (int frame = 0; frame < 21; ++frame)
    {
        now += 1000.0 / 30.0;                       // ~33.3 ms per frame
        const auto raw = (frame % 2 == 0) ? Risk::Rising : Risk::Critical;
        EXPECT_EQ (hold.apply (raw, now), Risk::Critical) << "frame " << frame;
    }
    EXPECT_LT (now, 100.0 + 750.0);                 // still inside the hold
}

TEST (SpectrumView, RingRiskUnavailableOverridesTheHold)
{
    gui::SpectrumView::RingRiskHysteresis hold;

    ASSERT_EQ (hold.apply (Risk::Critical, 5000.0), Risk::Critical);

    // The detector stopped scoring mid-hold. N/A is the honest answer and it
    // must not wait 750 ms to be told.
    EXPECT_EQ (hold.apply (Risk::Unavailable, 5010.0), Risk::Unavailable);

    // And coming back is immediate too.
    EXPECT_EQ (hold.apply (Risk::Low, 5020.0), Risk::Low);
}

TEST (SpectrumView, RingRiskStepUpRestartsTheHold)
{
    gui::SpectrumView::RingRiskHysteresis hold;

    ASSERT_EQ (hold.apply (Risk::Low, 0.0), Risk::Low);
    ASSERT_EQ (hold.apply (Risk::Rising, 100.0), Risk::Rising);   // hold from 100

    // A further step UP at 700 restarts the clock, so the drop that would
    // have been allowed at 850 is still held.
    ASSERT_EQ (hold.apply (Risk::Critical, 700.0), Risk::Critical);
    EXPECT_EQ (hold.apply (Risk::Low, 850.0),  Risk::Critical);
    EXPECT_EQ (hold.apply (Risk::Low, 1449.0), Risk::Critical);
    EXPECT_EQ (hold.apply (Risk::Low, 1450.0), Risk::Low);        // 700 + 750
}
