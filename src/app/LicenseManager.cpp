#include "app/LicenseManager.h"

bool LicenseManager::isAllowedKeyCharacter (juce::juce_wchar c)
{
    // Upper-case letters and digits only. Lower case is rejected rather than
    // folded: a stored licence file is keyed by the exact string, so accepting
    // two spellings of one key means two files.
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool LicenseManager::isValidKeyFormat (const juce::String& key)
{
    // "AZHF" + 4 groups of 4, each preceded by '-'.
    const int expectedLength = 4 + kNumGroups * (1 + kGroupLength);

    if (key.length() != expectedLength)
        return false;

    if (! key.startsWith ("AZHF"))
        return false;

    for (int group = 0; group < kNumGroups; ++group)
    {
        const int base = 4 + group * (1 + kGroupLength);

        if (key[base] != '-')
            return false;

        for (int i = 1; i <= kGroupLength; ++i)
            if (! isAllowedKeyCharacter (key[base + i]))
                return false;
    }

    return true;
}

//==============================================================================
namespace
{
/** Decodes one base64url segment of a JWT.

    JWT uses base64url (RFC 4648 section 5): '-' and '_' replace '+' and '/',
    and the '=' padding is stripped. juce::Base64 implements standard base64
    only, so both differences have to be undone before handing it over.
    Skipping the padding step makes decoding fail for exactly those payloads
    whose length is not a multiple of three -- an intermittent bug, not an
    obvious one.
*/
juce::String decodeBase64Url (juce::String segment)
{
    segment = segment.replaceCharacter ('-', '+').replaceCharacter ('_', '/');

    while (segment.length() % 4 != 0)
        segment += "=";

    juce::MemoryOutputStream decoded;

    if (! juce::Base64::convertFromBase64 (decoded, segment))
        return {};

    return decoded.toString();
}

/** XORs `bytes` against the repeating machine hash.

    What this achieves, stated plainly: it stops license.key being read or
    edited with a text editor, and it stops the file working after being copied
    to another machine. That is all it achieves. The hash is derived on the
    machine the binary runs on, so anyone willing to open the binary -- or to
    call getUniqueDeviceID() themselves -- can reproduce it. This is
    obfuscation, not encryption, and the plan asks for exactly that.
*/
void xorInPlace (juce::MemoryBlock& bytes, const juce::String& machineHash)
{
    const auto* mask = machineHash.toRawUTF8();
    const auto maskLength = machineHash.getNumBytesAsUTF8();

    if (maskLength == 0)
        return;

    auto* data = static_cast<char*> (bytes.getData());

    for (size_t i = 0; i < bytes.getSize(); ++i)
        data[i] = static_cast<char> (data[i] ^ mask[i % maskLength]);
}
} // namespace

//==============================================================================
std::optional<juce::String> UrlActivationTransport::post (const juce::String& url,
                                                          const juce::String& jsonBody)
{
    int statusCode = 0;

    const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                            .withExtraHeaders ("Content-Type: application/json")
                            .withConnectionTimeoutMs (connectionTimeoutMs)
                            .withStatusCode (&statusCode);

    if (auto stream = juce::URL (url).withPOSTData (jsonBody).createInputStream (options))
    {
        const auto body = stream->readEntireStreamAsString();

        if ((statusCode >= 200 && statusCode < 300) || body.isNotEmpty())
            return body;
    }

    return std::nullopt;
}

//==============================================================================
LicenseManager::LicenseManager (ActivationTransport& transport,
                                MachineIdSource& machineId,
                                LicenseClock& clock,
                                juce::File storageFile)
    : transport_ (transport),
      machineId_ (machineId),
      clock_ (clock),
      storageFile_ (std::move (storageFile))
{
}

juce::String LicenseManager::activationUrl()
{
    return "https://license.azsoundtech.com/activate";
}

juce::File LicenseManager::defaultStorageFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("AZSoundtech")
              .getChildFile ("HandsFree")
              .getChildFile ("license.key");
}

juce::String LicenseManager::machineIdHash() const
{
    const auto raw = machineId_.rawId();

    return juce::SHA256 (raw.toRawUTF8(), raw.getNumBytesAsUTF8()).toHexString();
}

juce::String LicenseManager::token() const
{
    return token_;
}

ActivationResult LicenseManager::checkToken (const juce::String& jwt) const
{
    juce::StringArray segments;
    segments.addTokens (jwt, ".", "");

    if (segments.size() != 3
        || segments[0].isEmpty() || segments[1].isEmpty() || segments[2].isEmpty())
        return ActivationResult::MalformedToken;

    const auto payload = juce::JSON::parse (decodeBase64Url (segments[1]));

    if (! payload.isObject())
        return ActivationResult::MalformedToken;

    // No `exp` claim means no expiry. That is not an oversight: spec section 1
    // sells this product "theo perpetual license", so a token without an
    // expiry is the normal case and must not be treated as malformed.
    const auto exp = payload.getProperty ("exp", {});

    if (! exp.isVoid())
        if (static_cast<juce::int64> (exp) * 1000 <= clock_.nowUtcMs())
            return ActivationResult::ExpiredToken;

    return ActivationResult::Success;
}

ActivationResult LicenseManager::activate (const juce::String& key)
{
    // Checked before the POST, so a typo costs nothing and tells the user
    // immediately rather than after a network round trip.
    if (! isValidKeyFormat (key))
        return ActivationResult::InvalidKeyFormat;

    juce::DynamicObject::Ptr body (new juce::DynamicObject());
    body->setProperty ("key", key);
    body->setProperty ("machineId", machineIdHash());

    const auto response = transport_.post (activationUrl(),
                                           juce::JSON::toString (juce::var (body.get())));

    if (! response.has_value())
        return ActivationResult::TransportFailure;

    const auto parsed = juce::JSON::parse (*response);

    if (! parsed.isObject())
        return ActivationResult::MalformedResponse;

    if (! parsed.getProperty ("error", {}).isVoid())
        return ActivationResult::ServerRejected;

    const auto jwt = parsed.getProperty ("token", {}).toString();

    if (jwt.isEmpty())
        return ActivationResult::MalformedResponse;

    if (const auto tokenState = checkToken (jwt); tokenState != ActivationResult::Success)
        return tokenState;

    key_             = key;
    token_           = jwt;
    lastValidatedMs_ = clock_.nowUtcMs();

    save();

    return ActivationResult::Success;
}

//==============================================================================
namespace
{
constexpr juce::int64 kMsPerDay = 24LL * 60LL * 60LL * 1000LL;
constexpr juce::int64 kClockBackwardsToleranceMs =
    LicenseManager::kClockBackwardsToleranceHours * 60LL * 60LL * 1000LL;
} // namespace

juce::int64 LicenseManager::lastValidatedMs() const
{
    return lastValidatedMs_;
}

bool LicenseManager::save()
{
    juce::DynamicObject::Ptr record (new juce::DynamicObject());
    record->setProperty ("key", key_);
    record->setProperty ("token", token_);
    record->setProperty ("lastValidatedMs", lastValidatedMs_);

    const auto json = juce::JSON::toString (juce::var (record.get()));

    juce::MemoryBlock bytes (json.toRawUTF8(), json.getNumBytesAsUTF8());
    xorInPlace (bytes, machineIdHash());

    if (! storageFile_.getParentDirectory().createDirectory())
        return false;

    return storageFile_.replaceWithData (bytes.getData(), bytes.getSize());
}

bool LicenseManager::load()
{
    juce::MemoryBlock bytes;

    if (! storageFile_.loadFileAsData (bytes) || bytes.getSize() == 0)
        return false;

    xorInPlace (bytes, machineIdHash());

    const juce::String json (static_cast<const char*> (bytes.getData()), bytes.getSize());
    const auto record = juce::JSON::parse (json);

    if (! record.isObject())
        return false;

    const auto storedKey = record.getProperty ("key", {}).toString();

    // The format check doubles as the integrity check. De-obfuscating with the
    // wrong machine hash yields bytes that neither parse as JSON nor carry a
    // well-formed key, so a file copied from another machine fails here rather
    // than loading a garbled licence.
    if (! isValidKeyFormat (storedKey))
        return false;

    key_             = storedKey;
    token_           = record.getProperty ("token", {}).toString();
    lastValidatedMs_ = static_cast<juce::int64> (record.getProperty ("lastValidatedMs", 0));

    return true;
}

void LicenseManager::deactivate()
{
    key_ = {};
    token_ = {};
    lastValidatedMs_ = 0;
    storageFile_.deleteFile();
}

LicenseState LicenseManager::state() const
{
    if (token_.isEmpty())
        return LicenseState::NotActivated;

    const auto elapsedMs = clock_.nowUtcMs() - lastValidatedMs_;

    // Checked BEFORE any division. A clock wound backwards makes elapsedMs
    // negative, and a negative duration compares as comfortably inside every
    // threshold below -- which is exactly how a naive date check is defeated.
    if (elapsedMs < -kClockBackwardsToleranceMs)
        return LicenseState::ClockTampered;

    const auto days = static_cast<double> (juce::jmax (juce::int64(), elapsedMs))
                        / static_cast<double> (kMsPerDay);

    // Plan Task 29 uses strict "greater than" for both thresholds, so day
    // seven exactly still runs clean and day ten exactly still only warns.
    if (days > kGraceExpiryDays)
        return LicenseState::Expired;

    if (days > kGraceWarningDays)
        return LicenseState::GraceWarning;

    return LicenseState::Active;
}

int LicenseManager::daysSinceLastValidation() const
{
    const auto elapsedMs = clock_.nowUtcMs() - lastValidatedMs_;

    if (elapsedMs <= 0)
        return 0;

    return static_cast<int> (elapsedMs / kMsPerDay);
}
