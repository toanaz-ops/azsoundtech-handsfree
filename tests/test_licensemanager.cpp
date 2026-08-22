// LicenseManager tests (Lane B -- plan Tasks 27, 28, 29).
//
// Why every one of these runs without a licence server, without a real clock
// and without the machine this was built on
// =====================================================================
// LicenseManager takes three seams by reference: an ActivationTransport (the
// HTTPS POST), a MachineIdSource (the machine-unique string) and a
// LicenseClock (wall-clock time). Production wires the JUCE-backed
// implementations; these tests wire fakes.
//
// That is not test scaffolding for its own sake. The cases that matter in a
// licence system are the REJECTIONS -- a 500, a timeout, a JWT that expired,
// a clock wound backwards -- and none of them can be provoked against a live
// server on demand. A licence check that can only be exercised against a
// happy-path server is a licence check whose failure paths never run until a
// customer hits them.

#include <gtest/gtest.h>

#include "app/LicenseManager.h"

namespace
{
// A syntactically valid key, used wherever the format is not what is under test.
const juce::String kGoodKey { "AZHF-1234-ABCD-5678-EFGH" };
} // namespace

// ---------------------------------------------------------------------------
// Task 27 -- key format
//
// The plan fixes the format as AZHF-XXXX-XXXX-XXXX-XXXX. The brief adds:
// "Validate the format strictly: length, the AZHF- prefix, the group
// structure, the permitted character set. Reject and report; never
// half-accept."
// ---------------------------------------------------------------------------

TEST (LicenseKeyFormat, AcceptsAWellFormedKey)
{
    EXPECT_TRUE (LicenseManager::isValidKeyFormat (kGoodKey));
}

TEST (LicenseKeyFormat, RejectsAnEmptyString)
{
    EXPECT_FALSE (LicenseManager::isValidKeyFormat (juce::String()));
}

TEST (LicenseKeyFormat, RejectsAKeyWithoutTheAzhfPrefix)
{
    // Same shape, same length, wrong product.
    EXPECT_FALSE (LicenseManager::isValidKeyFormat ("ZZZZ-1234-ABCD-5678-EFGH"));
}

TEST (LicenseKeyFormat, RejectsTooFewGroups)
{
    EXPECT_FALSE (LicenseManager::isValidKeyFormat ("AZHF-1234-ABCD-5678"));
}

TEST (LicenseKeyFormat, RejectsTooManyGroups)
{
    EXPECT_FALSE (LicenseManager::isValidKeyFormat ("AZHF-1234-ABCD-5678-EFGH-IJKL"));
}

TEST (LicenseKeyFormat, RejectsAGroupOfTheWrongLength)
{
    EXPECT_FALSE (LicenseManager::isValidKeyFormat ("AZHF-123-ABCD-5678-EFGH"));
    EXPECT_FALSE (LicenseManager::isValidKeyFormat ("AZHF-12345-ABCD-5678-EFGH"));
}

TEST (LicenseKeyFormat, RejectsLowerCase)
{
    // Accepting this would mean two spellings of one key, and the stored file
    // is keyed by the exact string.
    EXPECT_FALSE (LicenseManager::isValidKeyFormat ("AZHF-1234-abcd-5678-EFGH"));
}

TEST (LicenseKeyFormat, RejectsCharactersOutsideTheAllowedSet)
{
    EXPECT_FALSE (LicenseManager::isValidKeyFormat ("AZHF-1234-AB!D-5678-EFGH"));
    EXPECT_FALSE (LicenseManager::isValidKeyFormat ("AZHF-1234-AB D-5678-EFGH"));
}

TEST (LicenseKeyFormat, RejectsWrongSeparators)
{
    EXPECT_FALSE (LicenseManager::isValidKeyFormat ("AZHF_1234_ABCD_5678_EFGH"));
}

TEST (LicenseKeyFormat, RejectsSurroundingWhitespace)
{
    // Deliberate: the caller trims before validating, or it does not pass.
    // Silently trimming here would make the stored key differ from the typed one.
    EXPECT_FALSE (LicenseManager::isValidKeyFormat (" AZHF-1234-ABCD-5678-EFGH"));
}

// ---------------------------------------------------------------------------
// Fakes for the three seams.
// ---------------------------------------------------------------------------

namespace
{
/** Records what was posted and returns whatever the test told it to. */
struct FakeTransport : ActivationTransport
{
    std::optional<juce::String> response;   // nullopt == transport failure
    juce::String lastUrl, lastBody;
    int postCount = 0;

    std::optional<juce::String> post (const juce::String& url,
                                      const juce::String& jsonBody) override
    {
        ++postCount;
        lastUrl  = url;
        lastBody = jsonBody;
        return response;
    }
};

struct FakeMachineId : MachineIdSource
{
    juce::String id { "RAW-MACHINE-ID-0001" };
    juce::String rawId() const override { return id; }
};

/** A clock the test moves by hand. Starts at a fixed, arbitrary instant. */
struct FakeClock : LicenseClock
{
    juce::int64 ms = juce::Time (2026, 7, 1, 12, 0).toMilliseconds();  // 1 Aug 2026

    juce::int64 nowUtcMs() const override { return ms; }

    void advanceDays (double days)
    {
        ms += static_cast<juce::int64> (days * 24.0 * 60.0 * 60.0 * 1000.0);
    }
};

juce::String base64Url (const juce::String& text)
{
    juce::MemoryOutputStream out;
    juce::Base64::convertToBase64 (out, text.toRawUTF8(), text.getNumBytesAsUTF8());

    return out.toString().replaceCharacter ('+', '-')
                         .replaceCharacter ('/', '_')
                         .removeCharacters ("=");
}

/** A structurally valid JWT carrying `payloadJson`. The signature is a
    placeholder -- see the report: no public key ships with this build, so
    signatures are not verified client-side. */
juce::String makeJwt (const juce::String& payloadJson)
{
    return base64Url ("{\"alg\":\"HS256\",\"typ\":\"JWT\"}")
         + "." + base64Url (payloadJson)
         + "." + "c2lnbmF0dXJl";
}

/** Bundles a manager with its fakes so each test is one object. */
struct Harness
{
    FakeTransport   transport;
    FakeMachineId   machine;
    FakeClock       clock;
    juce::TemporaryFile temp { ".key" };
    LicenseManager  mgr { transport, machine, clock, temp.getFile() };

    /** Arms the transport with a successful, non-expiring activation. */
    void armSuccess()
    {
        transport.response = "{\"token\":\"" + makeJwt ("{\"sub\":\"perpetual\"}") + "\"}";
    }
};
} // namespace

// ---------------------------------------------------------------------------
// Task 28 -- the machine identifier
// ---------------------------------------------------------------------------

TEST (LicenseMachineId, IsTheSha256HexOfTheRawIdentifier)
{
    Harness h;
    const auto expected = juce::SHA256 (h.machine.id.toRawUTF8(),
                                        h.machine.id.getNumBytesAsUTF8()).toHexString();

    EXPECT_EQ (h.mgr.machineIdHash(), expected);
    EXPECT_EQ (h.mgr.machineIdHash().length(), 64);
}

TEST (LicenseMachineId, DiffersBetweenMachines)
{
    Harness a, b;
    b.machine.id = "RAW-MACHINE-ID-0002";

    EXPECT_NE (a.mgr.machineIdHash(), b.mgr.machineIdHash());
}

TEST (LicenseActivation, SendsTheHashAndNeverTheRawMachineId)
{
    // A machine identifier is personal data and this request leaves the user's
    // computer. The plan says "SHA256 of ..." -- this test is what keeps it so.
    Harness h;
    h.armSuccess();

    h.mgr.activate (kGoodKey);

    EXPECT_TRUE (h.transport.lastBody.contains (h.mgr.machineIdHash()));
    EXPECT_FALSE (h.transport.lastBody.contains (h.machine.id));
}

TEST (LicenseActivation, PostsToThePlannedEndpoint)
{
    Harness h;
    h.armSuccess();

    h.mgr.activate (kGoodKey);

    EXPECT_EQ (h.transport.lastUrl, juce::String ("https://license.azsoundtech.com/activate"));
}

TEST (LicenseActivation, PostsJsonCarryingTheKeyAndMachineId)
{
    Harness h;
    h.armSuccess();

    h.mgr.activate (kGoodKey);

    const auto parsed = juce::JSON::parse (h.transport.lastBody);
    ASSERT_TRUE (parsed.isObject());
    EXPECT_EQ (parsed.getProperty ("key", {}).toString(), kGoodKey);
    EXPECT_EQ (parsed.getProperty ("machineId", {}).toString(), h.mgr.machineIdHash());
}

TEST (LicenseActivation, RejectsAMalformedKeyWithoutTouchingTheNetwork)
{
    Harness h;
    h.armSuccess();

    EXPECT_EQ (h.mgr.activate ("not-a-key"), ActivationResult::InvalidKeyFormat);
    EXPECT_EQ (h.transport.postCount, 0);
}

TEST (LicenseActivation, SucceedsAndKeepsTheToken)
{
    Harness h;
    h.armSuccess();

    EXPECT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);
    EXPECT_TRUE (h.mgr.token().isNotEmpty());
}

TEST (LicenseActivation, ReportsTransportFailure)
{
    Harness h;
    h.transport.response = std::nullopt;   // timeout / no connection

    EXPECT_EQ (h.mgr.activate (kGoodKey), ActivationResult::TransportFailure);
    EXPECT_TRUE (h.mgr.token().isEmpty());
}

TEST (LicenseActivation, ReportsAMalformedResponseBody)
{
    Harness h;
    h.transport.response = juce::String ("<html>500 Internal Server Error</html>");

    EXPECT_EQ (h.mgr.activate (kGoodKey), ActivationResult::MalformedResponse);
    EXPECT_TRUE (h.mgr.token().isEmpty());
}

TEST (LicenseActivation, ReportsJsonWithNoTokenField)
{
    Harness h;
    h.transport.response = juce::String ("{\"status\":\"ok\"}");

    EXPECT_EQ (h.mgr.activate (kGoodKey), ActivationResult::MalformedResponse);
}

TEST (LicenseActivation, ReportsAServerRejection)
{
    Harness h;
    h.transport.response = juce::String ("{\"error\":\"unknown key\"}");

    EXPECT_EQ (h.mgr.activate (kGoodKey), ActivationResult::ServerRejected);
}

TEST (LicenseActivation, RejectsATokenThatIsNotAJwt)
{
    Harness h;
    h.transport.response = juce::String ("{\"token\":\"not.a.valid.jwt.at.all\"}");

    EXPECT_EQ (h.mgr.activate (kGoodKey), ActivationResult::MalformedToken);
}

TEST (LicenseActivation, RejectsAnAlreadyExpiredToken)
{
    Harness h;
    const auto expSeconds = h.clock.nowUtcMs() / 1000 - 60;   // one minute ago
    h.transport.response = "{\"token\":\"" + makeJwt ("{\"exp\":" + juce::String (expSeconds) + "}")
                         + "\"}";

    EXPECT_EQ (h.mgr.activate (kGoodKey), ActivationResult::ExpiredToken);
    EXPECT_TRUE (h.mgr.token().isEmpty());
}

TEST (LicenseActivation, AcceptsATokenWithNoExpiryBecauseTheLicenceIsPerpetual)
{
    // Spec section 1: "bán theo perpetual license". A JWT with no `exp` claim
    // is therefore valid, not malformed.
    Harness h;
    h.transport.response = "{\"token\":\"" + makeJwt ("{\"sub\":\"perpetual\"}") + "\"}";

    EXPECT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);
}

// ---------------------------------------------------------------------------
// Task 27 -- storage
//
// The plan says the file is "encrypted with simple XOR + machine hash". These
// tests pin down exactly what that buys, and nothing more:
//   - the key is not sitting in the file as readable text
//   - a file copied to another machine does not load
// They deliberately do NOT claim the file resists someone who opens the
// binary. See the report; that is a stated trade, not an oversight.
// ---------------------------------------------------------------------------

TEST (LicenseStorage, LoadFindsNothingBeforeAnyActivation)
{
    Harness h;
    EXPECT_FALSE (h.mgr.load());
    EXPECT_EQ (h.mgr.state(), LicenseState::NotActivated);
}

TEST (LicenseStorage, SuccessfulActivationWritesTheFile)
{
    Harness h;
    h.armSuccess();

    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);
    EXPECT_TRUE (h.temp.getFile().existsAsFile());
}

TEST (LicenseStorage, AFailedActivationWritesNothing)
{
    Harness h;
    h.transport.response = std::nullopt;

    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::TransportFailure);
    EXPECT_FALSE (h.temp.getFile().existsAsFile());
}

TEST (LicenseStorage, AFreshManagerLoadsTheStoredLicence)
{
    Harness h;
    h.armSuccess();
    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);

    // A second manager over the same file: this is what a restart looks like.
    LicenseManager restarted { h.transport, h.machine, h.clock, h.temp.getFile() };

    EXPECT_TRUE (restarted.load());
    EXPECT_EQ (restarted.token(), h.mgr.token());
    EXPECT_EQ (restarted.state(), LicenseState::Active);
}

TEST (LicenseStorage, TheKeyIsNotStoredAsReadableText)
{
    Harness h;
    h.armSuccess();
    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);

    juce::MemoryBlock raw;
    ASSERT_TRUE (h.temp.getFile().loadFileAsData (raw));

    const juce::String asText (static_cast<const char*> (raw.getData()), raw.getSize());
    EXPECT_FALSE (asText.contains (kGoodKey));
}

TEST (LicenseStorage, AFileFromAnotherMachineDoesNotLoad)
{
    // This is the whole point of mixing the machine hash into the XOR: copying
    // license.key to a second machine must not activate it.
    Harness h;
    h.armSuccess();
    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);

    FakeMachineId otherMachine;
    otherMachine.id = "A-COMPLETELY-DIFFERENT-MACHINE";
    LicenseManager onOtherMachine { h.transport, otherMachine, h.clock, h.temp.getFile() };

    EXPECT_FALSE (onOtherMachine.load());
    EXPECT_EQ (onOtherMachine.state(), LicenseState::NotActivated);
}

TEST (LicenseStorage, ACorruptFileDoesNotLoad)
{
    Harness h;
    h.temp.getFile().replaceWithData ("garbage that is not a licence record", 36);

    EXPECT_FALSE (h.mgr.load());
    EXPECT_EQ (h.mgr.state(), LicenseState::NotActivated);
}

TEST (LicenseStorage, DeactivateRemovesTheFileAndTheState)
{
    Harness h;
    h.armSuccess();
    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);

    h.mgr.deactivate();

    EXPECT_FALSE (h.temp.getFile().existsAsFile());
    EXPECT_EQ (h.mgr.state(), LicenseState::NotActivated);
    EXPECT_TRUE (h.mgr.token().isEmpty());
}

// ---------------------------------------------------------------------------
// Task 29 -- the offline grace period
//
// Every one of these would take between seven and eleven days of wall-clock
// waiting without the injected clock. That is the entire argument for the
// seam: the boundaries are where the bugs are, and the boundaries are days
// apart.
// ---------------------------------------------------------------------------

namespace
{
/** Activates, then moves the clock forward by `days`. */
LicenseState stateAfterDaysOffline (Harness& h, double days)
{
    h.armSuccess();
    EXPECT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);
    h.clock.advanceDays (days);

    return h.mgr.state();
}
} // namespace

TEST (LicenseGrace, IsActiveImmediatelyAfterActivation)
{
    Harness h;
    EXPECT_EQ (stateAfterDaysOffline (h, 0.0), LicenseState::Active);
}

TEST (LicenseGrace, IsStillActiveAtExactlySevenDays)
{
    // The plan says "if now - last_validated > 7 days" -- strictly greater.
    // Day seven on the dot must not warn.
    Harness h;
    EXPECT_EQ (stateAfterDaysOffline (h, 7.0), LicenseState::Active);
}

TEST (LicenseGrace, WarnsJustPastSevenDays)
{
    Harness h;
    EXPECT_EQ (stateAfterDaysOffline (h, 7.5), LicenseState::GraceWarning);
}

TEST (LicenseGrace, IsStillOnlyWarningAtExactlyTenDays)
{
    Harness h;
    EXPECT_EQ (stateAfterDaysOffline (h, 10.0), LicenseState::GraceWarning);
}

TEST (LicenseGrace, ExpiresJustPastTenDays)
{
    Harness h;
    EXPECT_EQ (stateAfterDaysOffline (h, 10.5), LicenseState::Expired);
}

TEST (LicenseGrace, StaysExpiredLongAfterwards)
{
    Harness h;
    EXPECT_EQ (stateAfterDaysOffline (h, 400.0), LicenseState::Expired);
}

TEST (LicenseGrace, ReportsHowManyDaysHavePassed)
{
    // What the GUI banner needs in order to say "N days remaining".
    Harness h;
    h.armSuccess();
    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);
    h.clock.advanceDays (8.0);

    EXPECT_EQ (h.mgr.daysSinceLastValidation(), 8);
}

TEST (LicenseGrace, ReactivatingClearsAnExpiredState)
{
    Harness h;
    ASSERT_EQ (stateAfterDaysOffline (h, 12.0), LicenseState::Expired);

    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);
    EXPECT_EQ (h.mgr.state(), LicenseState::Active);
}

TEST (LicenseGrace, TheGracePeriodSurvivesARestart)
{
    // The elapsed time must be measured from the STORED timestamp, not from
    // when this process started. A grace period that resets on every launch is
    // not a grace period.
    Harness h;
    h.armSuccess();
    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);
    h.clock.advanceDays (11.0);

    LicenseManager restarted { h.transport, h.machine, h.clock, h.temp.getFile() };
    ASSERT_TRUE (restarted.load());

    EXPECT_EQ (restarted.state(), LicenseState::Expired);
}

TEST (LicenseGrace, AClockWoundWellBackIsReportedAsTampering)
{
    // Setting the system clock back is the obvious way to defeat a date check.
    // A naive `now - stored` goes negative and reads as "plenty of time left".
    Harness h;
    h.armSuccess();
    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);

    h.clock.advanceDays (-30.0);

    EXPECT_EQ (h.mgr.state(), LicenseState::ClockTampered);
}

TEST (LicenseGrace, ASmallBackwardsCorrectionIsTolerated)
{
    // An NTP correction or a CMOS-battery fix moves the clock back by seconds
    // or hours, not weeks. Locking a paying customer out for that would be a
    // support call caused by our own strictness.
    Harness h;
    h.armSuccess();
    ASSERT_EQ (h.mgr.activate (kGoodKey), ActivationResult::Success);

    h.clock.advanceDays (-0.5);   // twelve hours

    EXPECT_EQ (h.mgr.state(), LicenseState::Active);
}

TEST (LicenseGrace, TamperingIsNotAWayToKeepUsingAnExpiredLicence)
{
    // Wind forward past expiry, then wind back before the activation date:
    // the result must not be Active.
    Harness h;
    ASSERT_EQ (stateAfterDaysOffline (h, 12.0), LicenseState::Expired);

    h.clock.advanceDays (-20.0);

    EXPECT_EQ (h.mgr.state(), LicenseState::ClockTampered);
}

TEST (LicenseGrace, AnUnactivatedManagerIsNeverInGrace)
{
    Harness h;
    h.clock.advanceDays (30.0);

    EXPECT_EQ (h.mgr.state(), LicenseState::NotActivated);
}
