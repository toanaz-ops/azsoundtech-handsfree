### Task 6: `MainComponent` owns the logger; `main.cpp` version and start

**Files:**
- Modify: `src/app/MainComponent.h` (includes, public API, members before `systemClock_`), `src/app/MainComponent.cpp` (ctor wiring, dtor order, `requestMode`, tuning lambdas, `loadPreset`, new methods), `src/main.cpp:14-17, 69`
- Test: `tests/test_gui_wiring.cpp` (append)

**Interfaces:**
- Consumes: `SessionLogger` (Task 1), `NotchController::{NotchEvent, EventSink, ClearReason, setEventSink, adoptPreset(…, int*)}` (Task 3), `NotchListPanel::onVerdict` (Task 5).
- Produces:
  ```cpp
  void setAppVersion (const juce::String& version);     // default "0.0.0-unset"
  bool startSessionLog (const juce::File& directory);   // builds the header from engine_ + slots, starts the logger
  void stopSessionLog();
  [[nodiscard]] juce::File sessionLogFileForTest() const;
  [[nodiscard]] int lastLoadSkippedNotchesForTest() const;
  ```
  Events written by `MainComponent` (field names are the log contract; `tools/logstats.py` reads them):

  | `ev` | fields |
  |---|---|
  | `session_start` | `app_version`, `os`, `device`, `sample_rate`, `buffer_size`, `slots: [{index, enabled, width, in: [..], out: [..], linked}]` |
  | `mode` | `mode`: `bypass` / `auto` / `soundcheck` |
  | `tuning` | `slot` (−1 global), `uses_global` (per-slot only), `rise_ms`, `persist`, `q`, `depth_db`, `thr` |
  | `notch_set` | `slot`, `lane`, `index`, `hz`, `q`, `depth_db`, `origin` (`detector`/`preset`/`manual`/`soundcheck`); when scored: `confirmed_lane`, `score`, `peakiness`, `p_norm`, `rise`, `novelty`, `penalty`, `asymmetry`, `persist_needed`, `thr`, `ctx: {bins, bin_hz, ref_age_ms, now[], ref[]?, other_lane_now[]?}` |
  | `notch_clear` | `slot`, `lane`, `index`, `hz`, `origin`, `reason` (`manual`/`clear_all`/`auto_release`/`width_change`/`verdict_false`/`partial_apply_unwind`), `age_ms` |
  | `verdict` | `slot`, `lane`, `index`, `hz`, `verdict` (`good`/`false`), `age_ms` |

  Spectra are rounded to 3 significant digits (D-3) by `roundSig3 (double)`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_gui_wiring.cpp`:

```cpp
//==============================================================================
// Lane D (data loop): the session log through the real wiring.

namespace
{
juce::File freshLogDir (const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("az-handsfree-sessionlog").getChildFile (name);
    dir.deleteRecursively();
    return dir;
}

std::vector<juce::var> parsedLines (const juce::File& file)
{
    juce::StringArray lines;
    lines.addLines (file.loadFileAsString());
    lines.removeEmptyStrings();
    std::vector<juce::var> out;
    for (const auto& l : lines)
    {
        juce::var v;
        EXPECT_TRUE (juce::JSON::parse (l, v).wasOk()) << l;
        out.push_back (v);
    }
    return out;
}

const juce::var* firstEvent (const std::vector<juce::var>& events, const juce::String& name)
{
    for (const auto& e : events)
        if (e["ev"].toString() == name)
            return &e;
    return nullptr;
}
} // namespace

// Spec test 14. Red if FALSE stops clearing with VerdictFalse, or the logger
// stops receiving verdict / notch_clear / session_start in order.
TEST (GuiWiring, FalseVerdictLogsVerdictThenClearsWithVerdictFalse)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    app.setAppVersion ("9.9.9-test");
    const auto dir = freshLogDir ("verdict");
    ASSERT_TRUE (app.startSessionLog (dir));
    const auto file = app.sessionLogFileForTest();

    auto* c0 = app.getNotchControllerForTest (0);
    ASSERT_NE (c0, nullptr);
    ASSERT_TRUE (c0->setNotch (0, 0, 1234.0, 30.0, -12.0, NotchController::Origin::Manual));
    std::vector<float> hop (512, 0.1f);
    app.getAudioEngine().getTapBuffer (0, 0).write (hop.data(), hop.size());
    app.getAudioEngine().getTapBuffer (0, 1).write (hop.data(), hop.size());
    c0->runOnce();   // publishes the snapshot AND flushes the Set event

    auto& panel = app.getNotchListPanelForTest();
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 1);
    panel.falseButtonForTest (0)->onClick();

    app.getAudioEngine().getTapBuffer (0, 0).write (hop.data(), hop.size());
    app.getAudioEngine().getTapBuffer (0, 1).write (hop.data(), hop.size());
    c0->runOnce();   // flushes the Clear event; republishes without the notch
    NotchController::SnapshotBuffer snap {};
    c0->copySnapshot (snap);
    EXPECT_EQ (snap.notchCount, 0u);

    app.stopSessionLog();
    const auto events = parsedLines (file);
    ASSERT_GE (events.size(), 5u);
    EXPECT_EQ (events.front()["ev"].toString(), "session_start");
    EXPECT_EQ (events.front()["app_version"].toString(), "9.9.9-test");
    EXPECT_TRUE (events.front()["slots"].isArray());
    EXPECT_EQ (events.back()["ev"].toString(), "session_end");

    const auto* set = firstEvent (events, "notch_set");
    ASSERT_NE (set, nullptr);
    EXPECT_EQ ((*set)["origin"].toString(), "manual");
    EXPECT_NEAR ((double) (*set)["hz"], 1234.0, 0.5);
    EXPECT_FALSE (set->hasProperty ("ctx"));

    const auto* verdict = firstEvent (events, "verdict");
    ASSERT_NE (verdict, nullptr);
    EXPECT_EQ ((*verdict)["verdict"].toString(), "false");
    EXPECT_EQ ((int) (*verdict)["slot"], 0);
    EXPECT_EQ ((int) (*verdict)["lane"], 0);
    EXPECT_EQ ((int) (*verdict)["index"], 0);

    const auto* clear = firstEvent (events, "notch_clear");
    ASSERT_NE (clear, nullptr);
    EXPECT_EQ ((*clear)["reason"].toString(), "verdict_false");

    // Order: verdict is written before the clear it causes.
    EXPECT_LT (verdict - events.data(), clear - events.data());
}

// Mode and tuning land in the log. Red if requestMode / onTuningChanged stop
// logging, or the field names drift from the logstats contract.
TEST (GuiWiring, ModeAndTuningChangesAreLogged)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    const auto dir = freshLogDir ("mode");
    ASSERT_TRUE (app.startSessionLog (dir));
    const auto file = app.sessionLogFileForTest();

    app.requestMode (AudioEngine::Mode::Auto);
    gui::TuningPanel::Params p;
    p.riseReferenceMs = 300; p.persistenceBlocks = 4; p.q = 25.0f; p.depthDb = -10.0f; p.peakinessThreshold = 12.0f;
    app.getTuningPanel().onTuningChanged (p);   // MainComponent.h:151

    app.stopSessionLog();
    const auto events = parsedLines (file);
    const auto* mode = firstEvent (events, "mode");
    ASSERT_NE (mode, nullptr);
    EXPECT_EQ ((*mode)["mode"].toString(), "auto");
    const auto* tuning = firstEvent (events, "tuning");
    ASSERT_NE (tuning, nullptr);
    EXPECT_EQ ((int) (*tuning)["slot"], -1);
    EXPECT_EQ ((int) (*tuning)["persist"], 4);
    EXPECT_NEAR ((double) (*tuning)["depth_db"], -10.0, 1e-6);
}

// Spec test 15. Red if MainComponent's teardown order lets a detector thread
// deliver an event into a logger that is already gone (or vice versa).
TEST (GuiWiring, DestroyingTheAppWhileADetectorIsPlacingNotchesDoesNotCrash)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    for (int run = 0; run < 20; ++run)
    {
        const auto dir = freshLogDir ("teardown-" + juce::String (run));
        MainComponent app;
        ASSERT_TRUE (app.startSessionLog (dir));
        auto* c0 = app.getNotchControllerForTest (0);
        ASSERT_NE (c0, nullptr);
        c0->setDetectionActive (true);
        c0->start();   // real detector thread

        // A loud 1 kHz tone into both lanes for ~1 s of audio, then die.
        std::vector<float> hop (512);
        double n = 0.0;
        for (int block = 0; block < 90; ++block)
        {
            for (auto& s : hop) { s = std::sin (2.0 * 3.14159265358979 * 1000.0 * n / 48000.0); n += 1.0; }
            app.getAudioEngine().getTapBuffer (0, 0).write (hop.data(), hop.size());
            app.getAudioEngine().getTapBuffer (0, 1).write (hop.data(), hop.size());
            if (block % 8 == 7) juce::Thread::sleep (1);
        }
        // `app` is destroyed here, mid-placement, with the thread running.
    }
    SUCCEED();
}

// Lane S loose end (A-9). Red if loadPreset stops counting a lane-1 notch a
// mono slot cannot take.
TEST (GuiWiring, LoadPresetCountsNotchesSkippedByAMonoSlot)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    const juce::String json =
        R"({"version":"1.0","device":"","sampleRate":48000,"bufferSize":256,)"
        R"("slots":[{"index":0,"enabled":true,"width":1,"inputChannels":[0,1],"outputChannels":[0,1]}],)"
        R"("notches":[{"slot":0,"lane":1,"index":0,"freq":482.0,"Q":30.0,"depth":-12.0},)"
        R"({"slot":0,"lane":0,"index":1,"freq":982.0,"Q":30.0,"depth":-12.0}]})";
    auto presetFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("az-handsfree-d-skipped.json");
    ASSERT_TRUE (presetFile.replaceWithText (json));
    ASSERT_TRUE (app.loadPreset (presetFile));
    presetFile.deleteFile();
    EXPECT_EQ (app.lastLoadSkippedNotchesForTest(), 1);
}
```

Verified against the code at plan time: the `slots` entry keys are `index`, `enabled`, `width`, `inputChannels`, `outputChannels`, `linked` (`PresetManager.cpp:298-309`); `PresetNotch::lane` defaults to `-1` (`PresetManager.h:103`); the tuning accessor is `getTuningPanel()` (`MainComponent.h:151`).

- [ ] **Step 2: Run to verify failure** — build; expected compile errors (`setAppVersion`, `startSessionLog`).

- [ ] **Step 3: Header** — in `MainComponent.h` add `#include "app/SessionLogger.h"`, the public methods listed under Interfaces, and the members **before** `systemClock_`:

```cpp
    // Lane D (data loop). Declared before systemClock_ and the controllers:
    // declaration order is destruction order, so the logger outlives every
    // detector thread that can still hand it an event while it joins.
    SessionLogger sessionLogger_;
    juce::String  appVersion_ { "0.0.0-unset" };   // main.cpp sets the real one (D-8)
    int           lastLoadSkipped_ = 0;

    juce::var sessionHeader() const;
    static juce::var notchEventToVar (const NotchController::NotchEvent& e);
```

- [ ] **Step 4: Implementation** — in `MainComponent.cpp`:

```cpp
namespace
{
double roundSig3 (double v)
{
    if (v == 0.0 || ! std::isfinite (v)) return 0.0;
    const double e = std::floor (std::log10 (std::abs (v)));
    const double scale = std::pow (10.0, 2.0 - e);
    return std::round (v * scale) / scale;
}

juce::var spectrumVar (const std::array<float, Detector::kNumBins>& bins)
{
    juce::Array<juce::var> out;
    out.ensureStorageAllocated (Detector::kNumBins);
    for (float b : bins)
        out.add (roundSig3 ((double) b));
    return out;
}

const char* originName (NotchController::Origin o)
{
    switch (o)
    {
        case NotchController::Origin::Detector:   return "detector";
        case NotchController::Origin::Preset:     return "preset";
        case NotchController::Origin::Manual:     return "manual";
        case NotchController::Origin::Soundcheck: return "soundcheck";
    }
    return "unknown";
}

const char* reasonName (NotchController::ClearReason r)
{
    switch (r)
    {
        case NotchController::ClearReason::Manual:             return "manual";
        case NotchController::ClearReason::ClearAll:           return "clear_all";
        case NotchController::ClearReason::AutoRelease:        return "auto_release";
        case NotchController::ClearReason::WidthChange:        return "width_change";
        case NotchController::ClearReason::VerdictFalse:       return "verdict_false";
        case NotchController::ClearReason::PartialApplyUnwind: return "partial_apply_unwind";
    }
    return "unknown";
}

const char* modeName (AudioEngine::Mode m)
{
    switch (m)
    {
        case AudioEngine::Mode::Bypass:     return "bypass";
        case AudioEngine::Mode::Auto:       return "auto";
        case AudioEngine::Mode::Soundcheck: return "soundcheck";
    }
    return "unknown";
}
} // namespace

juce::var MainComponent::notchEventToVar (const NotchController::NotchEvent& e)
{
    using Ev = NotchController::NotchEvent;
    auto v = SessionLogger::makeEvent (e.kind == Ev::Kind::Set ? "notch_set" : "notch_clear");
    auto* o = v.getDynamicObject();
    o->setProperty ("slot", e.slot);
    o->setProperty ("lane", e.lane);
    o->setProperty ("index", e.index);
    o->setProperty ("hz", (double) e.hz);
    o->setProperty ("q", (double) e.q);
    o->setProperty ("depth_db", (double) e.depthDb);
    o->setProperty ("origin", originName (e.origin));
    if (e.kind == Ev::Kind::Clear)
    {
        o->setProperty ("reason", reasonName (e.reason));
        o->setProperty ("age_ms", e.ageMs);
        return v;
    }
    if (! e.hasScore)
        return v;
    o->setProperty ("confirmed_lane", e.confirmedLane);
    o->setProperty ("score", (double) e.score);
    o->setProperty ("peakiness", (double) e.peakiness);
    o->setProperty ("p_norm", (double) e.pNorm);
    o->setProperty ("rise", (double) e.rise);
    o->setProperty ("novelty", (double) e.novelty);
    o->setProperty ("penalty", (double) e.penalty);
    o->setProperty ("asymmetry", (double) e.asymmetry);
    o->setProperty ("persist_needed", e.persistNeeded);
    o->setProperty ("thr", (double) e.thr);
    if (e.ctx != nullptr)
    {
        auto* c = new juce::DynamicObject();
        c->setProperty ("bins", e.ctx->bins);
        c->setProperty ("bin_hz", e.ctx->binHz);
        c->setProperty ("ref_age_ms", e.ctx->refAgeMs);
        c->setProperty ("now", spectrumVar (e.ctx->now));
        if (e.ctx->hasRef)   c->setProperty ("ref", spectrumVar (e.ctx->ref));
        if (e.ctx->hasOther) c->setProperty ("other_lane_now", spectrumVar (e.ctx->other));
        o->setProperty ("ctx", juce::var (c));
    }
    return v;
}

juce::var MainComponent::sessionHeader() const
{
    auto v = SessionLogger::makeEvent ("session_start");
    auto* o = v.getDynamicObject();
    o->setProperty ("app_version", appVersion_);
    o->setProperty ("os", juce::SystemStats::getOperatingSystemName());
    o->setProperty ("device", engine_.getCurrentDeviceName());
    o->setProperty ("sample_rate", engine_.getCurrentSampleRateHz());
    o->setProperty ("buffer_size", engine_.getCurrentBufferSize());
    juce::Array<juce::var> slots;
    for (int i = 0; i < kMaxSlots; ++i)
    {
        const auto cfg = engine_.getSlotConfig (i);
        auto* s = new juce::DynamicObject();
        s->setProperty ("index", i);
        s->setProperty ("enabled", cfg.enabled);
        s->setProperty ("width", cfg.width);
        juce::Array<juce::var> in, out;
        for (int l = 0; l < cfg.width; ++l) { in.add (cfg.inputChannels[l]); out.add (cfg.outputChannels[l]); }
        s->setProperty ("in", in);
        s->setProperty ("out", out);
        s->setProperty ("linked", slotLinked_[(std::size_t) i]);
        slots.add (juce::var (s));
    }
    o->setProperty ("slots", slots);
    return v;
}

void MainComponent::setAppVersion (const juce::String& version) { appVersion_ = version; }

bool MainComponent::startSessionLog (const juce::File& directory)
{
    return sessionLogger_.start (directory, sessionHeader());
}

void MainComponent::stopSessionLog() { sessionLogger_.stop(); }

juce::File MainComponent::sessionLogFileForTest() const { return sessionLogger_.currentFile(); }
int MainComponent::lastLoadSkippedNotchesForTest() const { return lastLoadSkipped_; }
```

Constructor (right after `setLookAndFeel (&azLookAndFeel_)`): attach the sink to every controller — threads are not running yet, the precondition holds:

```cpp
    for (auto& controller : notchControllers_)
        controller->setEventSink ([this] (const NotchController::NotchEvent& e)
        {
            sessionLogger_.log (notchEventToVar (e));
        });
```

Verdict wiring (after `addAndMakeVisible (notchListPanel_)`):

```cpp
    notchListPanel_.onVerdict = [this] (int slot, int lane, int index, float hz, bool good, double ageMs)
    {
        auto v = SessionLogger::makeEvent ("verdict");
        auto* o = v.getDynamicObject();
        o->setProperty ("slot", slot); o->setProperty ("lane", lane); o->setProperty ("index", index);
        o->setProperty ("hz", (double) hz);
        o->setProperty ("verdict", good ? "good" : "false");
        o->setProperty ("age_ms", ageMs);
        sessionLogger_.log (v);   // written BEFORE the clear it causes
        if (! good && slot >= 0 && slot < kMaxSlots)
            notchControllers_[(std::size_t) slot]->clearNotch (lane, index, NotchController::ClearReason::VerdictFalse);
    };
```

`requestMode`: after `engine_.setMode (mode);` add

```cpp
    {
        auto v = SessionLogger::makeEvent ("mode");
        v.getDynamicObject()->setProperty ("mode", modeName (engine_.getMode()));
        sessionLogger_.log (v);
    }
```

`tuningPanel_.onTuningChanged`: after the loop, log `tuning` with `slot = -1`, `rise_ms`, `persist`, `q`, `depth_db`, `thr` from `p`. `slotPanel_.onSlotTuningChanged`: log `tuning` with `slot = slotIndex`, `uses_global = t.usesGlobal`, and the five values from `t`.

Destructor (A-3):

```cpp
MainComponent::~MainComponent()
{
    stopTimer();
    setLookAndFeel (nullptr);
    // §6.5: every detector thread must be dead before the engine tears down.
    // Each stop() flushes its last events into the logger (still alive).
    for (auto& controller : notchControllers_)
        controller->stop (1000);
    sessionLogger_.stop();   // session_end, then the file closes
    for (auto& controller : notchControllers_)
        controller->setEventSink (nullptr);   // threads are joined: precondition holds
    engine_.stop();
}
```

`loadPreset`: replace the adopt loop body with

```cpp
    lastLoadSkipped_ = result.skippedNotchCount;
    for (int s = 0; s < kMaxSlots; ++s)
    {
        // ... collect notchesForSlot as before ...
        if (! notchesForSlot.empty())
        {
            int skipped = 0;
            notchControllers_[(std::size_t) s]->adoptPreset (notchesForSlot, &skipped);
            if (skipped > 0)
            {
                lastLoadSkipped_ += skipped;
                juce::Logger::writeToLog ("preset \"" + file.getFileName() + "\": slot "
                    + juce::String (s + 1) + " is mono, skipped " + juce::String (skipped)
                    + " lane-R notch(es)");
            }
        }
    }
```

and before `return true;` add `slotPanel_.refresh();   // the routing table shows what the file just changed (lane S loose end)`.

Update the stale `savePreset` header comment ("Notches mirrored across both lanes are deduped" is no longer true since the lane-S merge; say "one PresetNotch per (slot, lane, index)").

`main.cpp`:

```cpp
    const juce::String getApplicationVersion() override
    {
        return JUCE_APPLICATION_VERSION_STRING;   // the one place the number lives: CMakeLists project()
    }
    ...
            auto* content = new MainComponent();
            content->setAppVersion (JUCE_APPLICATION_VERSION_STRING);
            setContentOwned (content, true);
            ...
            content->startAudio();
            // Lane D: the session log opens AFTER the device, so its header
            // names the device actually in use. A failed start (no %APPDATA%)
            // is not fatal -- the app runs without a log.
            content->startSessionLog (SessionLogger::defaultDirectory());
```

Add `#include "app/SessionLogger.h"` to `main.cpp`.

- [ ] **Step 5: Build and run** — `cmake --build build --config Release` (header touched → `cmake -B build ...` first), `cd build && ctest -C Release --output-on-failure -R GuiWiring`. Expected: all pass. The teardown test runs 20 app constructions; if it takes more than ~20 s, reduce the hop count, not the run count.

- [ ] **Step 6: Run the real app once** — `build/HandsFree_artefacts/Release/HandsFree.exe` (path per `CMakeLists.txt`; check `ls build/*_artefacts/Release/`), let it open, close it. Then confirm a file exists under `%APPDATA%\AZSoundtech\HandsFree\logs\` whose first line has `"app_version":"1.1.1"`. Paste the first line into the report. If the machine has no audio device, the header shows an empty device — still a pass.

- [ ] **Step 7: Full suite** — `100% tests passed` (424).

- [ ] **Step 8: Commit**

```bash
git add src/app/MainComponent.h src/app/MainComponent.cpp src/main.cpp tests/test_gui_wiring.cpp
git commit -m "feat(app): session log wired through MainComponent; verdict clears with VerdictFalse; version from CMake"
```

---

