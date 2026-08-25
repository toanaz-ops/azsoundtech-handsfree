// TuningPanel: the DETECTION strip (brief 2026-08-24) -- runtime control of
// every detector tuning parameter.
//
// Pattern: this mirrors SlotPanel exactly. The panel READS the current values
// through paramsProvider (refresh()) and REPORTS changes through
// onTuningChanged as one complete Params struct -- it never calls a mutating
// controller entry point itself. MainComponent owns what a change means: the
// loop over all eight NotchControllers.
//
// One thin horizontal row (~34 px): a DETECTION caption and five combos --
// rise reference ms, persistence blocks, notch depth dB, notch Q, peakiness
// threshold. Every combo reports on ANY change so the receiver can treat
// Params as a single atomic snapshot rather than tracking which box moved.
//
// Threading: message thread only, like every other panel here. The setters
// it ultimately reaches are atomic-safe while the detector runs.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gui
{

class TuningPanel : public juce::Component
{
public:
    // One complete tuning snapshot. Defaults match kDefault* in the DSP layer
    // (rise 250 ms post-brief, persistence 3, depth -18 dB post-brief).
    struct Params
    {
        int   riseReferenceMs   = 250;
        int   persistenceBlocks = 3;
        int   depthDb           = -18;
        int   q                 = 30;
        float peakinessThreshold = 10.0f;
    };

    TuningPanel();

    // Re-select what the provider reports (or the defaults). Safe to call any
    // time; selections are applied dontSendNotification so refresh() can
    // never loop back through onTuningChanged.
    void refresh();

    // Every combo change lands here as a COMPLETE new Params. The receiver
    // applies it and calls refresh() so the panel re-reads reality.
    std::function<void (const Params&)> onTuningChanged;

    // Overridable source. Tests inject a fake; when null the defaults stand.
    std::function<Params()> paramsProvider;

    // Height the hosting layout should give this strip.
    static constexpr int kPanelHeight = 34;

    // Combo item id -> value mappings, exposed because the test drives ids
    // and asserts values, and because refresh() needs the inverse.
    static int   riseMsForId      (int id);
    static int   depthDbForId     (int id);
    static int   qForId           (int id);
    static float thresholdForId   (int id);
    static int   idForRiseMs      (int ms);
    static int   idForDepthDb     (int db);
    static int   idForQ           (int q);
    static int   idForThreshold   (float t);

    // TEST ACCESSORS ONLY -- drive the controls directly, like SlotPanel's
    // getRowForTest().
    [[nodiscard]] juce::ComboBox& getOneKnobComboForTest() { return oneKnob_; }
    [[nodiscard]] juce::ComboBox& getRiseComboForTest()    { return rise_; }
    [[nodiscard]] juce::ComboBox& getPersistComboForTest() { return persist_; }
    [[nodiscard]] juce::ComboBox& getDepthComboForTest()   { return depth_; }
    [[nodiscard]] juce::ComboBox& getQComboForTest()       { return q_; }
    [[nodiscard]] juce::ComboBox& getThrComboForTest()     { return thr_; }

    void resized() override;

private:
    void handleChanged();
    void handleOneKnobChanged();
    void applyPreset (int presetId);
    void updateOneKnobFor (const Params& p);

    Params currentParams() const;

    // ONE KNOB combo item ids: 1 SAFE, 2 BALANCED, 3 AGGRESSIVE, 4 CUSTOM.
    static constexpr int kPresetCustomId = 4;

    juce::Label oneKnobLabel_ { {}, "ONE KNOB" };
    juce::Label caption_ { {}, "DETECTION" };
    juce::Label riseLabel_    { {}, "Rise" };
    juce::Label persistLabel_ { {}, "Persist" };
    juce::Label depthLabel_   { {}, "Depth" };
    juce::Label qLabel_       { {}, "Q" };
    juce::Label thrLabel_     { {}, "Thr" };

    juce::ComboBox oneKnob_;
    juce::ComboBox rise_;
    juce::ComboBox persist_;
    juce::ComboBox depth_;
    juce::ComboBox q_;
    juce::ComboBox thr_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuningPanel)
};

} // namespace gui
