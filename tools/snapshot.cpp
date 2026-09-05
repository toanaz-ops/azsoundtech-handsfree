// HandsFreeSnapshot -- renders the console to PNG files WITHOUT a window.
//
// Why this exists
// ===============
// Reviewing a GUI change used to mean launching the app and capturing the
// screen, which is unreliable for reasons that have nothing to do with the
// app: another window drifts in front of the capture, the desktop's DPI
// scaling rescales what comes back, and a resize is not finished rendering by
// the time the capture fires.
//
// juce::Component::createComponentSnapshot() renders a component tree into an
// Image directly. No window, no desktop peer, no compositor -- so the output
// is exactly what the paint code drew, at whatever size is asked for, and it
// is identical every run. That makes it usable in review, in a doc, and (if
// it is ever wanted) as the input to an image-diff test.
//
// What it renders
// ===============
//   console-idle.png   the state the app opens in -- no signal, no notches
//   console-live.png   a real published spectrum with three notches placed at
//                      staggered ages, so the sodium-to-ice ramp that the whole
//                      design is built around is actually visible -- two of
//                      them on lane 0 (L) and one on lane 1 (R), so the LANE
//                      column, the dashed R stem and the L/R picker all render
//                      with something real behind them
//
// The live shot uses the REAL path: audio is written into BOTH of slot 0's tap
// rings and the controller's runOnce() publishes genuine two-lane snapshots,
// exactly as tests/test_spectrumview.cpp does. Nothing here hand-writes a
// lookalike struct, so a snapshot that looks right is evidence the real
// pipeline is. Slot 1 is put in LINK so the routing table shows both states of
// the per-slot ring-risk control side by side.
//
// Usage:
//   HandsFreeSnapshot <out-dir> [width] [height] [--fast]
//
// --fast skips the real-time waits that age the notches. The ramp needs wall
// time to be visible (a notch cools over ~22 s), so the default run is slow on
// purpose; --fast is for checking layout, where age does not matter.

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/MainComponent.h"
#include "dsp/Detector.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
constexpr double kSampleRate = 48000.0;

// A plausible programme rather than a test tone: a pink-ish floor with a few
// musical partials on it. A single sine produces one spike and a flat line,
// which tells a reviewer nothing about how the trace actually reads.
std::vector<float> makeHop (int hopIndex, int lane)
{
    std::vector<float> hop ((std::size_t) Detector::kHopSize);

    static const double partials[] = { 82.0, 164.0, 247.0, 392.0, 660.0,
                                       1240.0, 1920.0, 2600.0, 5200.0 };
    static const double gains[]    = { 0.28, 0.22, 0.30, 0.18, 0.12,
                                       0.16, 0.20, 0.08, 0.04 };

    juce::Random random ((juce::int64) (hopIndex + 1) * 7919 + (juce::int64) lane * 104729);

    for (std::size_t i = 0; i < hop.size(); ++i)
    {
        const double t = (double) (hopIndex * Detector::kHopSize + (int) i) / kSampleRate;

        double sample = 0.0;
        for (int p = 0; p < (int) std::size (partials); ++p)
        {
            // Lane 1 carries the same programme with a darker tilt -- more low
            // end, less top. Without it the L and R traces are identical and
            // the analyser's new L/R picker would redraw the same curve, which
            // proves nothing about which lane is being plotted.
            const double gain = gains[p] * (lane == 1 ? (p < 5 ? 1.25 : 0.55) : 1.0);
            sample += gain * std::sin (2.0 * juce::MathConstants<double>::pi
                                       * partials[p] * t);
        }

        sample += 0.05 * (random.nextDouble() * 2.0 - 1.0);   // broadband floor

        // Scaled to land the trace in the MIDDLE of the -90..0 dB window. At
        // unity the partials clamp along the top edge, which is honest
        // behaviour but makes a poor picture of how the plot reads.
        // Lands the trace across roughly -25..-70 dB, where real programme
        // material sits. At the previous 0.012 it ran along the top of the
        // window and the area fill covered most of the plot, which said more
        // about the test signal than about the design.
        hop[i] = (float) (sample * 0.0016);
    }

    return hop;
}

// Both lanes of the slot, written by the same loop: the controller drains one
// block from EACH tap per runOnce(), so feeding only lane 0 would publish a
// two-lane snapshot whose R half is silence.
void pump (LockFreeRingBuffer<float>& tapLane0, LockFreeRingBuffer<float>& tapLane1,
           NotchController& controller, int blocks, int& hopCounter)
{
    for (int block = 0; block < blocks; ++block)
    {
        const auto hopL = makeHop (hopCounter, 0);
        const auto hopR = makeHop (hopCounter, 1);
        ++hopCounter;
        tapLane0.write (hopL.data(), hopL.size());
        tapLane1.write (hopR.data(), hopR.size());
        controller.runOnce();
    }
}

bool writePng (const juce::Image& image, const juce::File& file)
{
    if (file.existsAsFile() && ! file.deleteFile())
        return false;

    juce::FileOutputStream stream (file);
    if (! stream.openedOk())
        return false;

    juce::PNGImageFormat png;
    return png.writeImageToStream (image, stream);
}

bool shoot (MainComponent& app, const juce::File& file)
{
    // The whole tree, rendered into an Image. `true` = include child
    // components; without it only MainComponent's own paint() would land.
    const auto image = app.createComponentSnapshot (app.getLocalBounds(), true);

    if (! writePng (image, file))
    {
        std::cerr << "could not write " << file.getFullPathName() << std::endl;
        return false;
    }

    std::cout << "wrote " << file.getFullPathName()
              << " (" << image.getWidth() << " x " << image.getHeight() << ")"
              << std::endl;
    return true;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: HandsFreeSnapshot <out-dir> [width] [height] [--fast]"
                  << std::endl;
        return 2;
    }

    bool fast = false;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--fast")
            fast = true;

    const juce::File outDir = juce::File::getCurrentWorkingDirectory()
                                  .getChildFile (juce::String (argv[1]));
    const auto created = outDir.createDirectory();
    if (created.failed())
    {
        std::cerr << created.getErrorMessage() << std::endl;
        return 1;
    }

    const int width  = argc > 2 && juce::String (argv[2]).containsOnly ("0123456789")
                           ? juce::String (argv[2]).getIntValue()
                           : MainComponent::kDefaultWidth;
    const int height = argc > 3 && juce::String (argv[3]).containsOnly ("0123456789")
                           ? juce::String (argv[3]).getIntValue()
                           : MainComponent::kDefaultHeight;

    // Brings up the MessageManager. No device is opened: startAudio() is a
    // deliberate separate call, which is exactly what lets the whole
    // application object be built on a machine with no interface attached.
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    // No device is open, so the routing table would render with every channel
    // combo empty -- and an empty table cannot show whether a real channel
    // name survives its column. Inject the names a typical interface reports.
    const auto channels = []
    {
        juce::StringArray names;
        for (int i = 1; i <= 8; ++i)
            names.add ("Analogue " + juce::String (i));
        return names;
    }();

    auto& slots = app.getSlotPanelForTest();
    slots.inputChannelNamesProvider  = [channels] { return channels; };
    slots.outputChannelNamesProvider = [channels] { return channels; };
    slots.refresh();

    app.setSize (width, height);
    app.resized();   // headless: no peer, so drive the layout pass directly

    if (! shoot (app, outDir.getChildFile ("console-idle.png")))
        return 1;

    //--------------------------------------------------------------------
    // The live shot.
    auto* controller = app.getNotchControllerForTest (0);
    if (controller == nullptr)
    {
        std::cerr << "slot 0 has no controller" << std::endl;
        return 1;
    }

    auto& tapL = app.getAudioEngine().getTapBuffer (0, 0);
    auto& tapR = app.getAudioEngine().getTapBuffer (0, 1);
    controller->setSampleRate (kSampleRate);

    // Slot 0 stays INDEP (the default), slot 1 is made STEREO and switched to
    // LINK, so the routing table renders both states of the per-slot control
    // rather than a column of one repeated word. Slot 1 opens MONO, and the
    // LINK/INDEP control is deliberately hidden on a mono row -- a policy
    // about "the other channel" means nothing there.
    //
    // The config goes straight to the engine rather than through
    // changeSlotConfig(): that path runs the device restart cycle, which stops
    // and starts every controller's detector thread, and this tool drives
    // runOnce() by hand from the main thread. Engine slot config is plain
    // relaxed atomics and needs no restart (AudioEngine::setSlotConfig).
    {
        SlotConfig stereoSlot;
        stereoSlot.enabled = true;
        stereoSlot.width   = 2;
        app.getAudioEngine().setSlotConfig (1, stereoSlot);
    }
    app.setSlotLinked (1, true);
    slots.refresh();
    app.resized();

    int hop = 0;
    pump (tapL, tapR, *controller, 8, hop);

    // Three notches, placed at staggered wall-clock times so the ramp is
    // visible in ONE frame: the first has cooled to ice, the second is
    // mid-way, the third has just fired and is still sodium.
    // Lane 1 takes the middle placement: with INDEP the two lanes hold
    // DIFFERENT frequencies, which is the whole point of the mode and the only
    // way the LANE column, the dashed stem and the R flag tag can be checked.
    struct Placement { int lane; int index; double hz; double q; double depthDb; int waitMs; };
    const Placement placements[] = {
        { 0, 0,  247.0, 30.0, -18.0, fast ? 0 : 12000 },
        { 1, 1, 1240.0, 30.0, -12.0, fast ? 0 :  9000 },
        { 0, 2, 1920.0, 30.0, -21.0, fast ? 0 :  1500 },
    };

    for (const auto& placement : placements)
    {
        controller->setNotch (placement.lane, placement.index, placement.hz,
                              placement.q, placement.depthDb,
                              NotchController::Origin::Manual);
        pump (tapL, tapR, *controller, 2, hop);

        // The ledgers record a notch's first sighting on the refresh that
        // first SEES it, so each placement has to be observed before the wait
        // that ages it begins.
        app.getSpectrumViewForTest().refreshFromSnapshot();
        app.getNotchListPanelForTest().refreshFromSnapshot();

        if (placement.waitMs > 0)
        {
            std::cout << "ageing notch at " << placement.hz << " Hz for "
                      << placement.waitMs << " ms" << std::endl;
            juce::Thread::sleep (placement.waitMs);
        }
    }

    pump (tapL, tapR, *controller, 4, hop);
    app.getSpectrumViewForTest().refreshFromSnapshot();
    app.getNotchListPanelForTest().refreshFromSnapshot();

    // Lane D: one row unjudged, one GOOD, one FALSE -- the FALSE row is
    // still in this (stale) snapshot because nothing is pumped after the
    // click, which is exactly the frame an operator sees for the ~250 ms
    // before the panel's next refresh drops it. Rows are found by their
    // displayed frequency text rather than by index: placement order in the
    // loop above is an implementation detail, not a promise about row order.
    {
        auto& list = app.getNotchListPanelForTest();
        int goodRow = -1, falseRow = -1;
        for (int i = 0; i < list.rowCountForTest(); ++i)
        {
            const auto freq = list.rowForTest (i).freq;
            if (freq == "247 Hz")
                goodRow = i;
            else if (freq == "1.9 kHz")
                falseRow = i;
        }
        if (goodRow < 0 || falseRow < 0)
        {
            std::cerr << "snapshot: could not find 247 Hz / 1.9 kHz rows to "
                          "click verdicts on (goodRow=" << goodRow
                       << " falseRow=" << falseRow << ")" << std::endl;
            return 1;
        }
        // M-11: both accessors return nullptr for a row without buttons (an
        // identity that left tracking, or a row index past the table). A
        // snapshot that cannot stage the verdict state is a snapshot nobody
        // should send to the owner -- say so and stop, do not dereference.
        auto* goodButton  = list.goodButtonForTest (goodRow);
        auto* falseButton = list.falseButtonForTest (falseRow);
        if (goodButton == nullptr || falseButton == nullptr)
        {
            std::cerr << "no verdict buttons for rows " << goodRow << "/" << falseRow
                      << " -- console-live.png would not show the VERDICT column"
                      << std::endl;
            return 1;
        }
        goodButton->onClick();     // 247 Hz  -> GOOD
        falseButton->onClick();    // 1.9 kHz -> FALSE, clears the notch
    }

    return shoot (app, outDir.getChildFile ("console-live.png")) ? 0 : 1;
}
