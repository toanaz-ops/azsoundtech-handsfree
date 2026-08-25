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
//                      design is built around is actually visible
//
// The live shot uses the REAL path: audio is written into slot 0's tap ring
// and the controller's runOnce() publishes genuine snapshots, exactly as
// tests/test_spectrumview.cpp does. Nothing here hand-writes a lookalike
// struct, so a snapshot that looks right is evidence the real pipeline is.
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
std::vector<float> makeHop (int hopIndex)
{
    std::vector<float> hop ((std::size_t) Detector::kHopSize);

    static const double partials[] = { 82.0, 164.0, 247.0, 392.0, 660.0,
                                       1240.0, 1920.0, 2600.0, 5200.0 };
    static const double gains[]    = { 0.28, 0.22, 0.30, 0.18, 0.12,
                                       0.16, 0.20, 0.08, 0.04 };

    juce::Random random ((juce::int64) (hopIndex + 1) * 7919);

    for (std::size_t i = 0; i < hop.size(); ++i)
    {
        const double t = (double) (hopIndex * Detector::kHopSize + (int) i) / kSampleRate;

        double sample = 0.0;
        for (int p = 0; p < (int) std::size (partials); ++p)
            sample += gains[p] * std::sin (2.0 * juce::MathConstants<double>::pi
                                           * partials[p] * t);

        sample += 0.05 * (random.nextDouble() * 2.0 - 1.0);   // broadband floor

        // Scaled to land the trace in the MIDDLE of the -90..0 dB window. At
        // unity the partials clamp along the top edge, which is honest
        // behaviour but makes a poor picture of how the plot reads.
        hop[i] = (float) (sample * 0.012);
    }

    return hop;
}

void pump (LockFreeRingBuffer<float>& tap, NotchController& controller,
           int blocks, int& hopCounter)
{
    for (int block = 0; block < blocks; ++block)
    {
        const auto hop = makeHop (hopCounter++);
        tap.write (hop.data(), hop.size());
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

    auto& tap = app.getAudioEngine().getTapBuffer (0);
    controller->setSampleRate (kSampleRate);

    int hop = 0;
    pump (tap, *controller, 8, hop);

    // Three notches, placed at staggered wall-clock times so the ramp is
    // visible in ONE frame: the first has cooled to ice, the second is
    // mid-way, the third has just fired and is still sodium.
    struct Placement { int index; double hz; double q; double depthDb; int waitMs; };
    const Placement placements[] = {
        { 0,  247.0, 30.0, -18.0, fast ? 0 : 12000 },
        { 1, 1240.0, 30.0, -12.0, fast ? 0 :  9000 },
        { 2, 1920.0, 30.0, -21.0, fast ? 0 :  1500 },
    };

    for (const auto& placement : placements)
    {
        controller->setNotch (0, placement.index, placement.hz, placement.q,
                              placement.depthDb, NotchController::Origin::Manual);
        pump (tap, *controller, 2, hop);

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

    pump (tap, *controller, 4, hop);
    app.getSpectrumViewForTest().refreshFromSnapshot();
    app.getNotchListPanelForTest().refreshFromSnapshot();

    return shoot (app, outDir.getChildFile ("console-live.png")) ? 0 : 1;
}
