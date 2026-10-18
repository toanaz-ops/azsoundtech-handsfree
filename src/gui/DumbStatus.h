// Lane U (spec 2026-10-18 section 3.3): what the Dumb-mode screen says, as a
// PURE function of the console's state -- one table, testable row by row,
// with no component, engine or controller in sight.
//
// The headline is a sentence a person says, never a number in dB or Hz
// (lane M decision Q10). The only number allowed is a count of points.

#pragma once

#include <juce_core/juce_core.h>

namespace gui
{

struct DumbInputs
{
    // SoundcheckPanel::Mode, one to one: Hidden, Running, Results, Applied.
    enum class Phase { Idle, Measuring, Results, Applied };
    enum class Risk  { Unavailable, Low, Rising, Critical };

    bool  configured   = false;   ///< Device open, no error, slot 0 valid for it.
    bool  bypassed     = false;   ///< AudioEngine::Mode::Bypass.
    bool  autoMode     = false;   ///< AudioEngine::Mode::Auto (not passive Soundcheck).
    bool  measureAllowed = false; ///< updateMeasureEnabled's predicate, verbatim.
    Phase phase        = Phase::Idle;
    int   resultCount  = 0;       ///< Candidates on the strip (Results) or placed (Applied).
    Risk  risk         = Risk::Unavailable;  ///< WORST slot, not the shown one.
    int   notchCount   = 0;       ///< Active notches across every enabled slot.
};

struct DumbStatus
{
    enum class Lamp { Grey, Cyan, Red, Amber, Green, DimGreen };

    // The two left-hand buttons change job with the phase; TẮT never does.
    enum class Left   { Measure, Stop, Apply };   // Applied keeps Measure, disabled
    enum class Middle { Enable, Dismiss };

    Lamp         lamp = Lamp::Grey;
    bool         pulse = false;      ///< Measuring: the lamp breathes.
    juce::String headline;
    juce::String subline;
    bool         lampOpensWizard = false;

    Left   left   = Left::Measure;
    Middle middle = Middle::Enable;
    bool   leftEnabled   = true;
    bool   middleEnabled = true;
    bool   middleOn      = false;    ///< BẬT drawn as the selected state.
    bool   offOn         = false;    ///< TẮT drawn as the selected state.

    // TẮT is the button that saves a show, and it is live in every row EXCEPT
    // Measuring -- exactly where Geek disables BYPASS
    // (setModeControlsEnabled(!measuring)), with DỪNG beside it (decision Q10).
    bool   offEnabled    = true;
};

DumbStatus dumbStatusFor (const DumbInputs& in);

} // namespace gui
