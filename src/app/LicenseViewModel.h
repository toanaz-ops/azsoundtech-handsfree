// LicenseViewModel: the licence-state-to-UI-decision mapping, with no
// juce::Component anywhere -- same shape as gui/DeviceViewModel, for the same
// reason. The test target links juce_core only, so every rule here is testable
// with no window and no message loop.
//
// What this deliberately is NOT
// ============================
// This is not the wiring. Nothing here constructs a LicenseManager, reads a
// file or touches a dialog -- that belongs to MainComponent/main.cpp, which
// the bridge lane owns right now (see the 2026-08-23 coordination note). When
// wiring lands, it reads state() once per poll on the message thread and asks
// this namespace what to show.

#pragma once

// juce_core only, deliberately.
#include <juce_core/juce_core.h>

#include "LicenseManager.h"

namespace license
{

// Which screen the application shows. Locked means audio processing must not
// run; RunWithWarning means it runs WITH a visible banner; the distinction is
// plan Task 29's warn-at-7 / stop-at-10 pair.
enum class LicenseScreen
{
    RunNormally,
    RunWithWarning,
    ShowActivation,
    Locked
};

/** The screen for a state, plus the banner text to show on it.

    The second value is non-empty only for RunWithWarning today; ShowActivation
    and Locked screens may also carry explanatory text later without this
    signature changing.
*/
std::pair<LicenseScreen, juce::String> screenFor (LicenseState state);

/** The grace banner: names days used AND the limit, because "reactivate soon"
    cannot tell a soundman whether tonight's show is inside the window.
    `daysSinceValidation` comes from LicenseManager::daysSinceLastValidation().
*/
juce::String graceBannerText (int daysSinceValidation);

/** One distinct, honest message per ActivationResult. Seven outcomes collapsed
    into "activation failed" would throw away exactly the information the
    enum exists to carry.
*/
juce::String activationFailureMessage (ActivationResult result);

} // namespace license
