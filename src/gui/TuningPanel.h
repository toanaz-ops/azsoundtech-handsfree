// TuningPanel: the DETECTION section of the rig column -- runtime control of
// every detector tuning parameter.
//
// Pattern: this mirrors SlotPanel exactly. The panel READS the current values
// through paramsProvider (refresh()) and REPORTS changes through
// onTuningChanged as one complete Params struct -- it never calls a mutating
// controller entry point itself. MainComponent owns what a change means: the
// loop over all eight NotchControllers.
//
// LAYOUT (rebuilt 2026-08-25). Four rows on the shared legend gutter:
//
//   DETECTION                                        section caption
//   RESPONSE   [ SAFE ][ BALANCED ][ AGGRESSIVE ]    the curated presets
//   NOTCH      DEPTH [-18 dB]  Q [30]                what the filter IS
//   TRIGGER    RISE [250 ms]  HOLD [3]  THR [10.0]   when it FIRES
//
// The five parameters are grouped by what they mean rather than laid out as
// five identical inline pairs: depth and Q describe the filter that gets
// placed, while rise, hold and threshold describe what makes one get placed
// at all. Those are two different questions and a soundman asks them at
// different times.
//
// RESPONSE is a segmented control, not a dropdown. It is the control that
// MOVES the other five, it has exactly three curated values, and its state
// has to be readable without opening anything -- all three of which a
// dropdown does badly. It is also a far bigger target than a 26 px combo.
//
// Threading: message thread only, like every other panel here. The setters
// it ultimately reaches are atomic-safe while the detector runs.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "gui/theme/AzTheme.h"

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

    // One field row plus the gap under it, taken from the theme rather than
    // re-typed here -- so a change to the field height moves this section's
    // rhythm with the rest of the rig column instead of drifting from it.
    static constexpr int kFieldRowHeight = az::theme::fieldHeight + az::theme::spacing;

    // Height the hosting layout should give this section: the caption plus
    // three field rows.
    static constexpr int kPanelHeight = 4 * kFieldRowHeight;

    //==========================================================================
    // RESPONSE -- the curated presets.
    //
    // The ids are the ones the ONE KNOB combo this replaced used, so the
    // preset table, refresh() and every test keep speaking the same
    // vocabulary: 1 SAFE, 2 BALANCED, 3 AGGRESSIVE, 4 CUSTOM.
    static constexpr int kPresetSafeId       = 1;
    static constexpr int kPresetBalancedId   = 2;
    static constexpr int kPresetAggressiveId = 3;
    static constexpr int kPresetCustomId     = 4;

    // Which preset is showing. CUSTOM whenever the five parameters do not
    // match any curated set -- which is what any manual edit produces.
    [[nodiscard]] int getSelectedPresetId() const { return presetId_; }

    // Lights the matching segment and clears the others. With a notification
    // other than dontSendNotification, a curated id ALSO applies its five
    // parameters and reports them through onTuningChanged -- exactly what
    // clicking the segment does. CUSTOM only ever reflects; there is no
    // "custom preset" to apply.
    void setSelectedPresetId (int presetId, juce::NotificationType notification);

    // Public because they ARE this component's interface, the same way
    // ModeRail's buttons are: wiring and headless tests drive them directly.
    juce::TextButton safeButton       { "Safe" };
    juce::TextButton balancedButton   { "Balanced" };
    juce::TextButton aggressiveButton { "Aggressive" };

    //==========================================================================
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
    [[nodiscard]] juce::ComboBox& getRiseComboForTest()    { return rise_; }
    [[nodiscard]] juce::ComboBox& getPersistComboForTest() { return persist_; }
    [[nodiscard]] juce::ComboBox& getDepthComboForTest()   { return depth_; }
    [[nodiscard]] juce::ComboBox& getQComboForTest()       { return q_; }
    [[nodiscard]] juce::ComboBox& getThrComboForTest()     { return thr_; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void handleChanged();
    void applyPreset (int presetId);
    void updatePresetFor (const Params& p);
    void reflectPresetButtons();

    Params currentParams() const;

    int presetId_ = kPresetBalancedId;

    juce::Label responseLabel_ { {}, "Response" };
    juce::Label notchLabel_    { {}, "Notch" };
    juce::Label triggerLabel_  { {}, "Trigger" };

    juce::Label riseLabel_    { {}, "Rise" };
    juce::Label persistLabel_ { {}, "Hold" };
    juce::Label depthLabel_   { {}, "Depth" };
    juce::Label qLabel_       { {}, "Q" };
    juce::Label thrLabel_     { {}, "Thr" };

    juce::ComboBox rise_;
    juce::ComboBox persist_;
    juce::ComboBox depth_;
    juce::ComboBox q_;
    juce::ComboBox thr_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuningPanel)
};

} // namespace gui
