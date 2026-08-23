#include "app/LicenseViewModel.h"

namespace license
{

std::pair<LicenseScreen, juce::String> screenFor (LicenseState state)
{
    switch (state)
    {
        case LicenseState::Active:
            return { LicenseScreen::RunNormally, {} };

        case LicenseState::GraceWarning:
            // The banner itself is built by graceBannerText() once the caller
            // has the day count; here it would need a second query, so the
            // caller composes it.
            return { LicenseScreen::RunWithWarning, {} };

        case LicenseState::NotActivated:
            return { LicenseScreen::ShowActivation, {} };

        case LicenseState::Expired:
            return { LicenseScreen::Locked,
                     "Licence offline for more than 10 days. Reactivate to continue." };

        case LicenseState::ClockTampered:
            return { LicenseScreen::Locked,
                     "The system clock appears to have been moved back. "
                     "Correct the date and time, then reactivate." };
    }
    return { LicenseScreen::RunNormally, {} };
}

juce::String graceBannerText (int daysSinceValidation)
{
    const int remaining = LicenseManager::kGraceExpiryDays - daysSinceValidation;
    return "Offline licence check: day " + juce::String (daysSinceValidation)
         + " of " + juce::String (LicenseManager::kGraceExpiryDays)
         + ". " + juce::String (remaining) + " day(s) left to reactivate.";
}

juce::String activationFailureMessage (ActivationResult result)
{
    switch (result)
    {
        case ActivationResult::Success:          return {};  // not a failure
        case ActivationResult::InvalidKeyFormat: return "That key is not in the AZHF-XXXX-XXXX-XXXX-XXXX format. Check for typos and upper case.";
        case ActivationResult::TransportFailure: return "Could not reach the activation server. Check the internet connection and try again.";
        case ActivationResult::ServerRejected:   return "The activation server rejected this key for this machine.";
        case ActivationResult::MalformedResponse:return "The activation server sent an unreadable answer. Try again; if it persists, contact support.";
        case ActivationResult::MalformedToken:   return "The activation server returned an invalid licence. Contact support.";
        case ActivationResult::ExpiredToken:     return "This licence has expired. Renew it at azsoundtech.com.";
    }
    return {};
}

} // namespace license
