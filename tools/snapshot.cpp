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
//                      with something real behind them, plus a RING RISK chip
//                      STAGED to Critical (see the note where it is set)
//   console-soundcheck-running.png
//                      lane M mid-measurement: the DO cell locked, the strip
//                      showing which output channel is being swept and how
//                      long is left, and the big DUNG that stops it
//   console-soundcheck-results.png
//                      the same run finished -- the margin curve and its
//                      marked bins over the analyser, the low-confidence band
//                      above 6 kHz dimmed, and the results strip carrying a
//                      hot-spot count plus the three sentences that are NOT a
//                      hot-spot count (saturated / could not measure / bad
//                      routing)
//   console-preset-music.png
//                      console-live's frame with the notch defaults set to
//                      presets/Music.json's OFF-LIST pair (Q 25, depth -10 dB),
//                      so the DEPTH and Q cells render the fba2626 behaviour:
//                      a value that sits on no combo rung is shown as text
//                      rather than leaving the combo blank
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
#include "app/SoundcheckController.h"
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
        { 0, 0,  247.0, 30.0, -18.0, fast ? 0 : 12000 },   // lane G: the rung the oldest howl earned
        { 1, 1, 1240.0, 30.0, -12.0, fast ? 0 :  9000 },   // steep-rise placement, one rung down
        { 0, 2, 1920.0, 30.0,  -6.0, fast ? 0 :  1500 },   // just placed: the ladder's first rung
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

    // RING RISK, STAGED -- the one thing in this file that is not the real
    // path, and it is marked as such on stdout so no reviewer mistakes it for
    // a measurement.
    //
    // The chip is now wired (task R3): it reads riskForScore() off the
    // monitored slot's snapshot every timer tick. But this tool's programme
    // material is a musical composite fed with detection DISARMED, so the
    // detector scores nothing and the honest reading here is N/A -- which is
    // exactly what console-idle.png already shows. Lighting it for real would
    // mean arming detection and feeding a howl with real gaps between blocks
    // (CandidateScorer's rise axis needs ~200 ms of scorer history -- see
    // tests/test_gui_wiring.cpp), and the auto-placed notch that follows would
    // wreck the three staged ages this shot exists to show.
    //
    // So the DISPLAYED field is set directly, through the accessor R2 added
    // for exactly this kind of "put the component in a known on-screen state"
    // job. What the picture proves is what the chip LOOKS like when it is lit
    // -- legibility, colour, whether CRITICAL fits its box. That the chip
    // reaches Critical from a real detector is proven by a test, not here
    // (MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms).
    app.getSpectrumViewForTest().ringRiskForTest()
        = gui::SpectrumView::RingRisk::Critical;
    std::cout << "console-live: RING RISK chip STAGED to Critical "
                 "(not measured -- see the comment in tools/snapshot.cpp)"
              << std::endl;

    if (! shoot (app, outDir.getChildFile ("console-live.png")))
        return 1;

    //--------------------------------------------------------------------
    // LANE M (Task 9): the two soundcheck states.
    //
    // Both are STAGED, and said so on stdout. Task 9 builds the components and
    // the overlay API; nothing in it connects them to a live
    // SoundcheckController (that is Task 10), and a real run would need a
    // device, an output channel and 72 s of sweep into a PA. What these two
    // pictures prove is what the console LOOKS like in each state --
    // legibility, whether the Vietnamese renders, whether AP DUNG fits its
    // button, whether the markers bury the trace. That the panel is DRIVEN
    // correctly is proven by tests, not here.
    //
    // The numbers are a plausible OutputResult rather than round figures: a
    // margin curve built from a constant would be a straight line and would
    // say nothing about how the overlay reads against a real trace.
    {
        auto& panel = app.getSoundcheckPanelForTest();

        // RUNNING. Channel 2 of 4, 47.2 s left -- lane M's OWN countdown
        // (getRemainingMsInRun), never the passive 15 s window, which is
        // frozen while the taps are suspended.
        panel.setProgress (1, 4, 47200.0);
        panel.setMode (gui::SoundcheckPanel::Mode::Running);

        // F10: while a run is in flight every control that could change the
        // chain underneath it is out of reach, and so is DO itself.
        app.getModeRailForTest().setModeControlsEnabled (false);
        app.getModeRailForTest().setMeasureEnabled (false);
        app.resized();

        std::cout << "console-soundcheck-running: panel STAGED to Running "
                     "(no device, no sweep -- see the comment in tools/snapshot.cpp)"
                  << std::endl;

        if (! shoot (app, outDir.getChildFile ("console-soundcheck-running.png")))
            return 1;

        //----------------------------------------------------------------
        // RESULTS. A synthetic OutputResult pushed into the overlay and the
        // strip: two outputs' worth of findings, a handful of marked bins, one
        // saturated candidate and one channel that could not be measured.
        SoundcheckController::OutputResult result;
        result.slot = 0; result.lane = 0; result.outChannel = 0; result.inChannel = 0;
        result.measured = true;

        // A margin that dips towards zero around the partials the live shot's
        // programme material actually carries, so the curve peaks where the
        // trace does and a reviewer can see whether the two are separable.
        //
        // SNAPPED TO BIN CENTRES. Round 1 put the dips at the partials' exact
        // frequencies with a sigma of ~0.028 octaves -- about 5 Hz wide at
        // 247 Hz, against a 23.4 Hz bin. The dip fell BETWEEN two bins and was
        // never sampled, so the 247 Hz tick sat on the axis with no peak above
        // it and the picture showed a marker pointing at nothing.
        const auto snapToBin = [] (double hz)
        {
            const double bin = std::lround (hz * (double) Detector::kFftSize / kSampleRate);
            return bin * kSampleRate / (double) Detector::kFftSize;
        };

        const double hotHz[] = { snapToBin (247.0),  snapToBin (660.0),
                                 snapToBin (1240.0), snapToBin (1920.0),
                                 snapToBin (3400.0) };

        for (int k = 0; k < (int) result.marginDb.size(); ++k)
        {
            const double hz = (double) k * kSampleRate / (double) Detector::kFftSize;

            // Baseline headroom, falling with frequency the way a real room's
            // does, plus a notch of margin at each hot spot.
            double margin = 19.0 - 4.0 * std::log10 (juce::jmax (20.0, hz) / 100.0);
            for (const double f : hotHz)
            {
                const double octaves = std::log2 (juce::jmax (1.0, hz) / f);
                // sigma ~0.1 octaves, not ~0.028: a peak has to be wider than
                // the bin spacing to be VISIBLE as a peak once it is sampled
                // at 23.4 Hz, and at 247 Hz the round-1 figure was five times
                // narrower than one bin.
                margin -= 17.0 * std::exp (-(octaves * octaves) / 0.02);
            }

            result.marginDb[(std::size_t) k] = (float) margin;

            // LoopGainEstimator only trusts the swept band; everything else is
            // drawn as a gap rather than as a line nobody should believe.
            result.trusted[(std::size_t) k] =
                hz >= SoundcheckController::kSweepLowHz
                && hz <= SoundcheckController::kTrustedHighHz;
        }

        // The marked bins: the peak of each dip, which is what
        // SoundcheckCandidates would have picked.
        int marked = 0;
        for (const double f : hotHz)
        {
            const int bin = (int) std::lround (f * (double) Detector::kFftSize / kSampleRate);
            if (bin > 0 && bin < (int) result.marked.size())
            {
                result.marked[(std::size_t) bin] = true;
                ++marked;
            }
        }
        result.markedCount    = marked;
        result.candidateCount = marked;
        result.saturatedBins  = 1;      // one still over after the deepest rung

        app.getSpectrumViewForTest().setSoundcheckOverlay (
            result.marginDb.data(), result.marked.data(), result.trusted.data(),
            (int) result.marginDb.size(), kSampleRate);

        panel.setResultsSummary (/*hotSpots*/ result.candidateCount,
                                 /*saturated*/ result.saturatedBins,
                                 /*unmeasured*/ 1,
                                 /*routingInvalid*/ 1);
        panel.setMode (gui::SoundcheckPanel::Mode::Results);

        // The mode switches come back the moment the run ends; DO does NOT,
        // because a second measurement on top of an unanswered proposal set
        // would throw the proposals away.
        app.getModeRailForTest().setModeControlsEnabled (true);
        app.resized();

        std::cout << "console-soundcheck-results: " << marked
                  << " marked bins STAGED into the overlay, strip showing "
                     "hot spots + saturated + unmeasured + misrouted"
                  << std::endl;

        if (! shoot (app, outDir.getChildFile ("console-soundcheck-results.png")))
            return 1;

        // Back to a resting console, so the shot that follows is console-live's
        // frame and not this one with a strip across it.
        panel.setMode (gui::SoundcheckPanel::Mode::Hidden);
        app.getSpectrumViewForTest().clearSoundcheckOverlay();
        app.getModeRailForTest().setMeasureEnabled (true);
        app.resized();
    }

    //--------------------------------------------------------------------
    // The off-list-ceiling shot (final review, I-4).
    //
    // fba2626 changed what the tuning strip DISPLAYS when the values handed
    // to it do not sit on a combo's fixed rungs: before it, an off-list value
    // left the combo BLANK (idForValue returned 0), and the operator's next
    // touch on any of the five combos pushed Q 10 / -6 dB onto every Global
    // slot -- a ceiling nobody asked for, on a live rig. After it, the value
    // is shown as text and currentParams() reports it back unchanged.
    //
    // That is a change to what the console LOOKS like and nothing pictured
    // it. presets/Music.json ships exactly such a pair -- Q 25, depth -10 dB,
    // neither on a rung -- so this shot puts the console in the state loading
    // that preset produces. Everything else is console-live's frame: same
    // three staged ages, same verdicts, same chip.
    //
    // The defaults go straight to the controllers rather than through
    // loadPreset(): that path opens a file chooser and drives the device
    // restart cycle, and the ceiling is the only part of the preset this shot
    // is about. Both panels then re-read through their providers -- TuningPanel
    // via paramsProvider (which reads controller 0), SlotPanel via
    // slotTuningProvider (per row) -- so the strip and the routing table's
    // mini combos are showing the SAME reality the real load would leave.
    for (int i = 0; i < kMaxSlots; ++i)
        if (auto* c = app.getNotchControllerForTest (i))
            c->setNotchDefaults (25.0, -10.0);   // presets/Music.json

    app.getTuningPanel().refresh();
    slots.refresh();
    app.resized();

    std::cout << "console-preset-music: notch defaults set to Q 25 / -10 dB "
                 "(presets/Music.json) -- the off-list ceiling render"
              << std::endl;

    return shoot (app, outDir.getChildFile ("console-preset-music.png")) ? 0 : 1;
}
