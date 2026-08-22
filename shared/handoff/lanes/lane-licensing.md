# Lane brief — Licensing (Tasks 27, 28, 29)

**Base:** `main` @ `194a090`. **Branch:** `feat/lane-licensing`.
Read `README.md` beside this file first — setup, ground rules, worktree traps.

This is the most independent lane in the project: **new files only**, no DSP, no
GUI, no audio device. It can run start to finish without touching anything
another lane owns.

## ⚠ Two things in the plan are broken. Verified, not suspected.

### 1. `wmic` does not exist on this machine

Plan Task 28 specifies the machine ID as *"SHA256 of WMIC output"*.

```
command -v wmic   →  NOT FOUND
```

Microsoft deprecated WMIC and it is absent from current Windows 11 builds
(this machine is 10.0.26200). A licence system keyed to a command that is not
installed would activate on the developer's machine and fail on every customer's
— or the reverse — and the failure would look like a licence-server problem.

**Use `juce::SystemStats::getUniqueDeviceID()`.** It exists in the vendored JUCE
(`modules/juce_core/system/juce_SystemStats.h:179`) and is the maintained
replacement. Note that `getDeviceIdentifiers()` sits right above it at line 164
and is **marked deprecated in the header itself** — *"The identifiers produced by
this function are not reliable"*. Do not use that one.

Wrap it behind your own `MachineId` seam anyway, so the hashing is testable
without depending on what the host machine returns.

### 2. The activation endpoint almost certainly does not exist

Plan Task 28 posts to `https://license.azsoundtech.com/activate`. Nothing in this
repo suggests that server has been built.

**Do not let that block you, and do not hard-code `juce::URL` into
`LicenseManager`.** Put the transport behind an interface:

```cpp
struct ActivationTransport {
    virtual ~ActivationTransport() = default;
    // Returns the raw response body, or an empty optional on transport failure.
    virtual std::optional<juce::String> post (const juce::String& url,
                                              const juce::String& jsonBody) = 0;
};
```

Production implements it with `juce::URL::createInputStream`. Tests implement it
with a fake that returns canned responses — including **failures**, which are the
cases that matter: timeout, 500, malformed JSON, a JWT that has expired.

This is better design regardless of whether the server exists. A licence check
that can only be tested against a live server is a licence check that never gets
tested against a rejection.

## Task 27 — Key format and storage

Format `AZHF-XXXX-XXXX-XXXX-XXXX`. Storage
`%APPDATA%/AZSoundtech/HandsFree/license.key`.

Plan says *"encrypted with simple XOR + machine hash"*. **Say plainly in your
report what that does and does not achieve.** XOR against a machine hash stops
casual copying of a key file between machines. It is not encryption against
anyone who opens the binary, and it will not survive a determined attacker. That
is very likely an acceptable trade for this product — but it should be a stated
decision, not an implied claim of security.

Do **not** invent something stronger on your own initiative; that is the owner's
call. Implement what the plan says and flag the limitation.

Validate the format strictly: length, the `AZHF-` prefix, the group structure,
the permitted character set. Reject and report; never half-accept.

## Task 28 — Online activation

Body per the plan:

```json
{ "key": "AZHF-1234-...", "machineId": "<hash>" }
```

Response is a JWT to store locally. Use `juce::JSON` for parsing — it is already
in `juce_core` and needs no new dependency.

**Do not send the raw machine ID.** Send the hash. The plan's own wording says
`SHA256 of ...`; keep it that way, and say so in the report, because a machine
identifier is personal data and this request leaves the user's computer.

Cover with the fake transport: success, network failure, HTTP error, malformed
body, a JWT whose signature or expiry does not check out.

## Task 29 — Offline grace period

```
now - last_validated  >  7 days   →  warning banner
now - last_validated  > 10 days   →  disable processing, show "Reactivate"
```

**Two things this lane must NOT do:**

1. **Do not add a "kill switch" to `AudioEngine`.** That file is owned by the
   DSP spine and is the busiest in the repo. Expose licence *state* from
   `LicenseManager` and let the application layer decide. A lane that reaches
   into `AudioEngine` to enforce a licence collides with the bridge work and
   puts a business rule inside the real-time path.
2. **Do not use `juce::Time::getCurrentTime()` directly in the expiry logic.**
   Inject the clock, exactly as the bridge design does (§4) and for the same
   reason: no test can wait ten days. A `ClockSource`-style seam makes "grace
   expires at day 10" a real test instead of a comment.

Also handle a clock that moves **backwards**. A user setting their system clock
back is the obvious way to defeat a date check, and a naive `now - stored`
returns a negative duration that silently reads as "plenty of time left". Decide
what to do, implement it, and say what you decided.

## You own

```
src/app/LicenseManager.h/cpp     (new)
src/app/ActivationTransport.h    (new, or wherever the seam lives)
tests/test_licensemanager.cpp    (new)
CMakeLists.txt                   (one line: your new sources)
```

**Do not touch** `src/dsp/*`, `src/app/AudioEngine.*`, `src/app/MainComponent.*`,
`src/gui/*`, or `installer/*`. The GUI lane is live in `MainComponent` right now.

Wiring the licence state into the UI is **not** this lane. Expose the state and
say in your report what the GUI will need to call.

## Done means

- `cmake --build build --config Release` exits 0
- `ctest -C Release` — 75/75 still pass, plus yours
- Activation is tested against a **fake** transport, including its failure modes
- Grace-period expiry is tested with an **injected** clock, forwards and backwards
- Your report states the XOR-storage limitation in plain words
