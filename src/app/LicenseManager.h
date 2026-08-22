// LicenseManager: key format, activation and the offline grace period.
//
// Plan Tasks 27, 28 and 29. Spec sections 10.2 and 8.
//
// Design note -- why three injected seams
// =======================================
// Every interesting branch in a licence system is a rejection, and none of the
// three rejection sources can be provoked on demand from production code:
//
//   - the network      (timeout, 500, malformed body)
//   - the machine      (a different machine's identifier)
//   - the calendar     (the tenth day of an offline grace period)
//
// So each is an abstract seam: ActivationTransport, MachineIdSource and
// LicenseClock. Production wires the JUCE-backed implementations below; tests
// wire fakes. The alternative -- juce::URL, juce::SystemStats and
// juce::Time::getCurrentTime() called inline -- yields a class whose failure
// paths cannot run until a customer runs them.
//
// Scope note -- this class does NOT stop audio
// ============================================
// It reports licence STATE. It has no reference to AudioEngine and must not
// acquire one: that file is the real-time spine, a licence check is a business
// rule, and the two do not belong on the same thread. The application layer
// reads state() and decides what to do about it.

#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include <optional>

//==============================================================================
/** Wall-clock source, injected so the grace-period logic is testable.

    Deliberately NOT the audio-detector bridge design's ClockSource. That one is
    `nowMs()` backed by juce::Time::getMillisecondCounterHiRes() -- a monotonic
    counter for sub-second detector timing, which resets when the machine
    reboots. A licence measures days across application restarts, so it needs
    calendar time. Sharing the bridge's seam here would produce a grace period
    that silently restarts at every reboot.
*/
struct LicenseClock
{
    virtual ~LicenseClock() = default;

    /** Milliseconds since the Unix epoch, UTC. */
    virtual juce::int64 nowUtcMs() const = 0;
};

/** Production clock. */
struct SystemLicenseClock : LicenseClock
{
    juce::int64 nowUtcMs() const override
    {
        return juce::Time::getCurrentTime().toMilliseconds();
    }
};

//==============================================================================
/** The machine-unique string. Never leaves the machine unhashed. */
struct MachineIdSource
{
    virtual ~MachineIdSource() = default;
    virtual juce::String rawId() const = 0;
};

/** Production machine ID.

    Plan Task 28 specifies "SHA256 of WMIC output". WMIC does not exist on this
    machine (Windows 11 build 10.0.26200) -- Microsoft removed it -- so a
    licence keyed to it would fail on every current Windows install.
    juce::SystemStats::getUniqueDeviceID() is the maintained replacement.

    Note that the adjacent getDeviceIdentifiers() is marked deprecated in JUCE's
    own header ("The identifiers produced by this function are not reliable").
    Do not substitute it.
*/
struct SystemMachineIdSource : MachineIdSource
{
    juce::String rawId() const override
    {
        return juce::SystemStats::getUniqueDeviceID();
    }
};

//==============================================================================
/** The HTTPS POST seam for online activation. */
struct ActivationTransport
{
    virtual ~ActivationTransport() = default;

    /** Returns the raw response body, or nullopt on transport failure
        (no connection, timeout, DNS failure, non-2xx with no body).
    */
    virtual std::optional<juce::String> post (const juce::String& url,
                                              const juce::String& jsonBody) = 0;
};

/** Production transport: an HTTPS POST via juce::URL, as plan Task 28 asks.

    Deliberately the only part of this file with no unit test, and that is the
    point of the seam: it contains no decisions, only the JUCE call. Every
    branch that used to live behind it -- timeout, 500, malformed body, expired
    token -- now lives in LicenseManager and is covered against a fake.

    A non-2xx response WITH a body is handed back rather than discarded, so the
    server's own error JSON reaches the user instead of being flattened into a
    generic "could not connect".
*/
struct UrlActivationTransport : ActivationTransport
{
    std::optional<juce::String> post (const juce::String& url,
                                      const juce::String& jsonBody) override;

    int connectionTimeoutMs = 10000;
};

//==============================================================================
/** What the application layer is allowed to do right now.

    This class reports the state; it does NOT act on it. Nothing here touches
    AudioEngine -- see the header note above.
*/
enum class LicenseState
{
    NotActivated,   ///< No usable record on disk. Show the activation dialog.
    Active,         ///< Validated within the grace window. Run normally.
    GraceWarning,   ///< Past 7 days offline: keep processing, show a banner.
    Expired,        ///< Past 10 days offline: stop processing, offer Reactivate.
    ClockTampered   ///< Stored timestamp is in the future. See checkGrace().
};

//==============================================================================
/** Outcome of an activation attempt. Every value is a branch a customer can
    hit, and every one of them is covered by a test against the fake transport.
*/
enum class ActivationResult
{
    Success,            ///< Server returned a usable token.
    InvalidKeyFormat,   ///< Rejected locally; the network was never touched.
    TransportFailure,   ///< No response at all: offline, DNS, timeout.
    ServerRejected,     ///< Server answered, and the answer was "no".
    MalformedResponse,  ///< Answer was not JSON, or carried no token.
    MalformedToken,     ///< Token is not a three-segment JWT.
    ExpiredToken        ///< Token parsed, but its `exp` claim is in the past.
};

class LicenseManager
{
public:
    /** All three seams are held by reference and must outlive this object.
        `storageFile` is where the licence record is persisted.
    */
    LicenseManager (ActivationTransport& transport,
                    MachineIdSource& machineId,
                    LicenseClock& clock,
                    juce::File storageFile);

    //==========================================================================
    /** Strict validation of the plan's AZHF-XXXX-XXXX-XXXX-XXXX format.

        Checks length, prefix, group structure, separators and character set.
        Rejects anything else outright -- including surrounding whitespace and
        lower case, so that one licence has exactly one spelling.
    */
    static bool isValidKeyFormat (const juce::String& key);

    /** Characters permitted inside a group. */
    static bool isAllowedKeyCharacter (juce::juce_wchar c);

    /** The endpoint from plan Task 28. */
    static juce::String activationUrl();

    /** `%APPDATA%/AZSoundtech/HandsFree/license.key` -- spec section 10.2. */
    static juce::File defaultStorageFile();

    //==========================================================================
    /** SHA-256, hex, of the raw machine identifier.

        This is what travels over the wire. The raw identifier never does: it is
        personal data, and hashing it costs nothing here.
    */
    juce::String machineIdHash() const;

    /** POSTs the key and the machine hash, and keeps the token on success. */
    ActivationResult activate (const juce::String& key);

    /** The stored JWT, or an empty string if there is none. */
    juce::String token() const;

    //==========================================================================
    /** Reads and de-obfuscates the record. False if there is none, it is
        corrupt, or it belongs to another machine.
    */
    bool load();

    /** Forgets the licence and deletes the file. */
    void deactivate();

    /** Current licence state. Never blocks and never touches the network. */
    LicenseState state() const;

    /** UTC milliseconds of the last successful activation, or 0. */
    juce::int64 lastValidatedMs() const;

    /** Whole days since the last successful activation. Clamped at zero, so a
        clock nudged backwards never reports a negative age. What the GUI
        banner needs in order to say how many days are left.
    */
    int daysSinceLastValidation() const;

    //==========================================================================
    /** Plan Task 29: warn after 7 days offline. */
    static constexpr int kGraceWarningDays = 7;

    /** Plan Task 29: stop processing after 10 days offline. */
    static constexpr int kGraceExpiryDays = 10;

    /** How far the clock may move backwards before it is called tampering.

        Not zero, deliberately. An NTP correction or a fix to a machine whose
        CMOS battery died moves the clock back by seconds or hours; treating
        that as tampering locks out a paying customer over our own strictness.
        A day is generous for a genuine correction and far below the seven-day
        grace, so it cannot be chained to extend the window.
    */
    static constexpr int kClockBackwardsToleranceHours = 24;

private:
    bool save();
    /** Validates JWT shape and expiry. Returns Success when usable. */
    ActivationResult checkToken (const juce::String& jwt) const;

    ActivationTransport& transport_;
    MachineIdSource&     machineId_;
    LicenseClock&        clock_;
    juce::File           storageFile_;

    juce::String key_, token_;
    juce::int64  lastValidatedMs_ = 0;

    static constexpr int kGroupLength = 4;
    static constexpr int kNumGroups   = 4;
};
