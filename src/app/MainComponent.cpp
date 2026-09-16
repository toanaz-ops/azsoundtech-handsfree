#include "app/MainComponent.h"

#include "app/PresetFirstRun.h"
#include "app/PresetManager.h"
#include "gui/DeviceViewModel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace
{
// 5 Hz. The status line carries a latency figure and two channel counts; there
// is nothing here a soundman reads faster than that, and the poll costs three
// relaxed atomic loads plus a device query.
constexpr int kStatusRefreshMs = 200;

// Smallest spectrum worth looking at; resized() clamps the drawer against it.
constexpr int kMinSpectrumHeight = 120;

// Height of the notch list column: caption, column headers and ~3 visible
// notch rows.
constexpr int kNotchListHeight = 120;

// The largest share of the space under the transport the bottom floor may
// take, however much its contents want. See floorHeightFor().
constexpr float kMaxFloorShare = 0.55f;

// Breathing room added to the height the floor needs, so "it fits" is visibly
// true rather than true to the pixel. See heightThatFitsTheFloor().
constexpr int kFloorSlack = 24;

//==============================================================================
// Lane D (data loop): session-log event shaping. roundSig3 is what keeps a
// 1025-value spectrum array short in the JSONL file -- JSON::toString would
// otherwise print every double to full precision.
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
        case NotchController::ClearReason::SoundcheckReplace:  return "soundcheck_replace";
    }
    return "unknown";
}

// Lane G. A FREE function, in the same anonymous namespace as originName and
// reasonName above -- notchEventToVar calls it unqualified from this same
// translation unit, and a `MainComponent::` member would need a header
// declaration it deliberately does not have.
const char* retuneReasonName (NotchController::RetuneReason r)
{
    switch (r)
    {
        case NotchController::RetuneReason::Deepen:  return "deepen";
        case NotchController::RetuneReason::Release: return "release";
        case NotchController::RetuneReason::Reclamp: return "reclamp";
        case NotchController::RetuneReason::Ceiling: return "ceiling";
    }
    // Same fallthrough as originName and reasonName: an enumerator added
    // without a name here reads as "unknown" in the log rather than
    // masquerading as a deepening that never happened.
    return "unknown";
}

//==============================================================================
// LANE M Task 10 -- every operator-facing sentence, as EXPLICIT UTF-8 BYTES.
//
// This build passes no /utf-8 to MSVC and only one file in the tree carries a
// BOM, so a plain source literal is decoded with whatever the machine's active
// codepage happens to be -- the mojibake middle dot all over again
// (src/gui/DeviceViewModel.cpp:13, gui/SoundcheckPanel.h:44-48 are the
// precedents being followed). Each refusal gets its OWN sentence: "no device"
// and "the room is ringing" are different problems with different answers, and
// a single "could not measure" would send the operator looking in the wrong
// place (inv 19, F26).

// "Chua do duoc: thiet bi am thanh chua chay."
const char* kScEngineNotRunning =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: thi\xe1\xba\xbft b\xe1\xbb\x8b \xc3\xa2m thanh ch\xc6\xb0" "a ch\xe1\xba\xa1y.";
// "Chua do duoc: thiet bi khong co kenh vao hoac kenh ra."
const char* kScNoChannels =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: thi\xe1\xba\xbft b\xe1\xbb\x8b kh\xc3\xb4ng c\xc3\xb3 k\xc3\xaanh v\xc3\xa0o ho\xe1\xba\xb7" "c k\xc3\xaanh ra.";
// "Chua do duoc: chua co slot nao duoc bat voi do rong hop le."
const char* kScSlotDisabled =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: ch\xc6\xb0" "a c\xc3\xb3 slot n\xc3\xa0o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c b\xe1\xba\xadt v\xe1\xbb\x9bi \xc4\x91\xe1\xbb\x99 r\xe1\xbb\x99ng h\xe1\xbb\xa3p l\xe1\xbb\x87.";
// "Chua do duoc: dinh tuyen kenh sai. Kiem tra lai bang routing."
const char* kScInvalidChannelPair =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: \xc4\x91\xe1\xbb\x8bnh tuy\xe1\xba\xbfn k\xc3\xaanh sai. Ki\xe1\xbb\x83m tra l\xe1\xba\xa1i b\xe1\xba\xa3ng routing.";
// "Chua do duoc: phong dang co nguy co hu. Ha gain roi thu lai."
const char* kScRingRiskRising =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: ph\xc3\xb2ng \xc4\x91" "ang c\xc3\xb3 nguy c\xc6\xa1 h\xc3\xba. H\xe1\xba\xa1 gain r\xe1\xbb\x93i th\xe1\xbb\xad l\xe1\xba\xa1i.";
// "Chua do duoc: tham so chay khong hop le (tran notch hoac nguong nen chua dat)."
const char* kScInvalidParams =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: tham s\xe1\xbb\x91 ch\xe1\xba\xa1y kh\xc3\xb4ng h\xe1\xbb\xa3p l\xe1\xbb\x87 (tr\xe1\xba\xa7n notch ho\xe1\xba\xb7" "c ng\xc6\xb0\xe1\xbb\xa1ng n\xe1\xbb\x81n ch\xc6\xb0" "a \xc4\x91\xe1\xba\xb7t).";
// "Dang do roi."
const char* kScAlreadyRunning = "\xc4\x90" "ang \xc4\x91o r\xe1\xbb\x93i.";
// "Chua do duoc: lan do truoc con dang tat tieng dan. Doi mot nhip roi bam lai."
const char* kScRampOutPending =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: l\xe1\xba\xa7n \xc4\x91o tr\xc6\xb0\xe1\xbb\x9b" "c c\xc3\xb2n \xc4\x91" "ang t\xe1\xba\xaft ti\xe1\xba\xbfng d\xe1\xba\xa7n. \xc4\x90\xe1\xbb\xa3i m\xe1\xbb\x99t nh\xe1\xbb\x8bp r\xe1\xbb\x93i b\xe1\xba\xa5m l\xe1\xba\xa1i.";
// "Chua do duoc: ly do khong xac dinh."
const char* kScUnknownRefusal =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: l\xc3\xbd do kh\xc3\xb4ng x\xc3\xa1" "c \xc4\x91\xe1\xbb\x8bnh.";
// "Chua do duoc: thiet bi dang bao loi. " -- the device's own text follows.
const char* kScDeviceError =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: thi\xe1\xba\xbft b\xe1\xbb\x8b \xc4\x91" "ang b\xc3\xa1o l\xe1\xbb\x97i. ";
// "Chua do duoc: chua co hop thoai xac nhan."
const char* kScNoConfirmHook =
    "Ch\xc6\xb0" "a \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c: ch\xc6\xb0" "a c\xc3\xb3 h\xe1\xbb\x99p tho\xe1\xba\xa1i x\xc3\xa1" "c nh\xe1\xba\xadn.";
// "Dang do: khong nap preset duoc. Bam BO hoac doi do xong."
const char* kScPresetRefused =
    "\xc4\x90" "ang \xc4\x91o: kh\xc3\xb4ng n\xe1\xba\xa1p preset \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c. B\xe1\xba\xa5m B\xe1\xbb\x8e ho\xe1\xba\xb7" "c \xc4\x91\xe1\xbb\xa3i \xc4\x91o xong.";
// "Thiet bi vua khoi dong lai: phep do da dung."
const char* kScAbortedByRestart =
    "Thi\xe1\xba\xbft b\xe1\xbb\x8b v\xe1\xbb\xab" "a kh\xe1\xbb\x9fi \xc4\x91\xe1\xbb\x99ng l\xe1\xba\xa1i: ph\xc3\xa9p \xc4\x91o \xc4\x91\xc3\xa3 d\xe1\xbb\xabng.";
// "Da go " ... " notch cu va khong dat lai duoc cai nao. Phong dang kem hon
// truoc khi bam AP DUNG."  THE outcome that must never be silent.
const char* kScWorseOffHead = "\xc4\x90\xc3\xa3 g\xe1\xbb\xa1 ";
const char* kScWorseOffTail =
    " notch c\xc5\xa9 v\xc3\xa0 kh\xc3\xb4ng \xc4\x91\xe1\xba\xb7t l\xe1\xba\xa1i \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c c\xc3\xa1i n\xc3\xa0o. Ph\xc3\xb2ng \xc4\x91" "ang k\xc3\xa9m h\xc6\xa1n tr\xc6\xb0\xe1\xbb\x9b" "c khi b\xe1\xba\xa5m \xc3\x81P D\xe1\xbb\xa4NG.";

// The confirmation. "HA MASTER TRUOC" is the title; the body states the level
// HONESTLY (spec 4.3 / F13: no SPL figure, because the app cannot know one)
// and the real cost in seconds.
// "Bo qua xac nhan cu: bang dieu khien da doi tu luc hoi. Bam DO lai."
const char* kScStaleConfirmation =
    "B\xe1\xbb\x8f qua x\xc3\xa1" "c nh\xe1\xba\xadn c\xc5\xa9: b\xe1\xba\xa3ng \xc4\x91i\xe1\xbb\x81u khi\xe1\xbb\x83n \xc4\x91\xc3\xa3 \xc4\x91\xe1\xbb\x95i t\xe1\xbb\xab l\xc3\xba" "c h\xe1\xbb\x8fi. B\xe1\xba\xa5m \xc4\x90O l\xe1\xba\xa1i.";
// "Dang cho xac nhan. Tra loi hop thoai truoc."
const char* kScConfirmAlreadyOpen =
    "\xc4\x90" "ang ch\xe1\xbb\x9d x\xc3\xa1" "c nh\xe1\xba\xadn. Tr\xe1\xba\xa3 l\xe1\xbb\x9di h\xe1\xbb\x99p tho\xe1\xba\xa1i tr\xc6\xb0\xe1\xbb\x9b" "c.";

// ONE SENTENCE PER ABORT REASON. Every abort but the two a finger causes is
// decided on the lane M thread, which may not touch a component -- so this is
// the only place the operator is ever told WHY a run vanished, and "a mic that
// was too hot" and "a room that was already ringing" want different answers
// from them. DUNG and Esc get nothing: they already know.
// "Da dung do: tin hieu mic qua lon. Ha gain dau vao roi do lai."
const char* kScAbortMicHot =
    "\xc4\x90\xc3\xa3 d\xe1\xbb\xabng \xc4\x91o: t\xc3\xadn hi\xe1\xbb\x87u mic qu\xc3\xa1 l\xe1\xbb\x9bn. H\xe1\xba\xa1 gain \xc4\x91\xe1\xba\xa7u v\xc3\xa0o r\xe1\xbb\x93i \xc4\x91o l\xe1\xba\xa1i.";
// "Da dung do: phong da hu san truoc khi phat. Ha gain roi do lai."
const char* kScAbortRoomRinging =
    "\xc4\x90\xc3\xa3 d\xe1\xbb\xabng \xc4\x91o: ph\xc3\xb2ng \xc4\x91\xc3\xa3 h\xc3\xba s\xe1\xba\xb5n tr\xc6\xb0\xe1\xbb\x9b" "c khi ph\xc3\xa1t. H\xe1\xba\xa1 gain r\xe1\xbb\x93i \xc4\x91o l\xe1\xba\xa1i.";
// "Da dung do: thiet bi am thanh da dung."
const char* kScAbortEngineStopped =
    "\xc4\x90\xc3\xa3 d\xe1\xbb\xabng \xc4\x91o: thi\xe1\xba\xbft b\xe1\xbb\x8b \xc3\xa2m thanh \xc4\x91\xc3\xa3 d\xe1\xbb\xabng.";
// "Da dung do: thiet bi bao loi."
const char* kScAbortDeviceError =
    "\xc4\x90\xc3\xa3 d\xe1\xbb\xabng \xc4\x91o: thi\xe1\xba\xbft b\xe1\xbb\x8b b\xc3\xa1o l\xe1\xbb\x97i.";
// "Da dung do: thiet bi hoac dinh tuyen doi giua chung."
const char* kScAbortDeviceChanged =
    "\xc4\x90\xc3\xa3 d\xe1\xbb\xabng \xc4\x91o: thi\xe1\xba\xbft b\xe1\xbb\x8b ho\xe1\xba\xb7" "c \xc4\x91\xe1\xbb\x8bnh tuy\xe1\xba\xbfn \xc4\x91\xe1\xbb\x95i gi\xe1\xbb\xaf" "a ch\xe1\xbb\xabng.";
// "Da dung do: mat du lieu mic. Tang buffer roi do lai."
const char* kScAbortCaptureDrop =
    "\xc4\x90\xc3\xa3 d\xe1\xbb\xabng \xc4\x91o: m\xe1\xba\xa5t d\xe1\xbb\xaf li\xe1\xbb\x87u mic. T\xc4\x83ng buffer r\xe1\xbb\x93i \xc4\x91o l\xe1\xba\xa1i.";
// "Da dung do: khong do duoc nen nhieu. Kiem tra kenh mic."
const char* kScAbortNoiseFloor =
    "\xc4\x90\xc3\xa3 d\xe1\xbb\xabng \xc4\x91o: kh\xc3\xb4ng \xc4\x91o \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c n\xe1\xbb\x81n nhi\xe1\xbb\x85u. Ki\xe1\xbb\x83m tra k\xc3\xaanh mic.";

const char* kScConfirmTitle  = "H\xe1\xba\xa0 MASTER TR\xc6\xaf\xe1\xbb\x9a" "C";
const char* kScConfirmOk     = "\xc4\x90O";
const char* kScConfirmCancel = "H\xe1\xbb\xa6Y";
// "Sweep phat o " N " dB duoi toan thang, tai vi tri master hien tai cua ban.
//  Master mo het thi muc do van rat to: HA MASTER TRUOC."
const char* kScConfirmHead  = "Sweep ph\xc3\xa1t \xe1\xbb\x9f ";
const char* kScConfirmHead2 =
    " dB d\xc6\xb0\xe1\xbb\x9bi to\xc3\xa0n thang, t\xe1\xba\xa1i v\xe1\xbb\x8b tr\xc3\xad master hi\xe1\xbb\x87n t\xe1\xba\xa1i c\xe1\xbb\xa7" "a b\xe1\xba\xa1n. Master m\xe1\xbb\x9f h\xe1\xba\xbft th\xc3\xac m\xe1\xbb\xa9" "c \xc4\x91\xc3\xb3 v\xe1\xba\xabn r\xe1\xba\xa5t to: H\xe1\xba\xa0 MASTER TR\xc6\xaf\xe1\xbb\x9a" "C.\x0a\x0a";
// "Moi luot do lam kenh ngo ra do im hoan toan " X " giay. " N " luot do tren "
// M " kenh ngo ra, tong khoang " Y " giay."
//
// TWO COUNTS, because they differ and the difference is the operator's time
// (review M-1). A PASS is one (slot, lane) measurement; two slots feeding one
// output are two passes on ONE channel. The seconds follow the PASSES -- that
// is what the machine actually spends -- while the channel count is what the
// operator recognises on their patch. Printing the pass count as "N kenh ngo
// ra" overstated how much of their rig goes quiet.
const char* kScConfirmSilence  = "M\xe1\xbb\x97i l\xc6\xb0\xe1\xbb\xa3t \xc4\x91o l\xc3\xa0m k\xc3\xaanh ng\xc3\xb5 ra \xc4\x91\xc3\xb3 im ho\xc3\xa0n to\xc3\xa0n ";
const char* kScConfirmSilence2 = " gi\xc3\xa2y. ";
const char* kScConfirmPasses   = " l\xc6\xb0\xe1\xbb\xa3t \xc4\x91o tr\xc3\xaan ";
const char* kScConfirmTotal    = " k\xc3\xaanh ng\xc3\xb5 ra, t\xe1\xbb\x95ng kho\xe1\xba\xa3ng ";
const char* kScConfirmTotal2   = " gi\xc3\xa2y.";

// "LOI: phep do khong dung duoc khi thiet bi khoi dong lai. Kiem tra lai thiet
//  bi truoc khi do tiep."  DISTINCT from the normal abort sentence (review M-4):
// abortAndJoin() returning false means the machine did NOT reach Idle, which is
// a different and much worse situation than "your measurement was cancelled".
const char* kScRestartJoinFailed =
    "L\xe1\xbb\x96I: ph\xc3\xa9p \xc4\x91o kh\xc3\xb4ng d\xe1\xbb\xabng \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c khi thi\xe1\xba\xbft b\xe1\xbb\x8b kh\xe1\xbb\x9fi \xc4\x91\xe1\xbb\x99ng l\xe1\xba\xa1i. Ki\xe1\xbb\x83m tra l\xe1\xba\xa1i thi\xe1\xba\xbft b\xe1\xbb\x8b tr\xc6\xb0\xe1\xbb\x9b" "c khi \xc4\x91o ti\xe1\xba\xbfp.";

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

MainComponent::MainComponent()
    : notchControllers_ ([this]
      {
          // Heap allocation per slot -- see the member comment in the header
          // for why these cannot live inside this object's stack frame.
          decltype (notchControllers_) controllers;
          for (int i = 0; i < kMaxSlots; ++i)
              controllers[(std::size_t) i] =
                  std::make_unique<NotchController> (engine_.getTapBuffer (i, 0),
                                                     &engine_.getTapBuffer (i, 1),
                                                     engine_.getCommandQueue (i),
                                                     systemClock_, i);
          return controllers;
      }())
    , spectrumView_ (*notchControllers_[0])
    , modeRail_ (gui::ModeRail::Orientation::Horizontal)
    , notchListPanel_ (*notchControllers_[0])
    , deviceDrawer_ (devicePanel_)
{
    // The Sodium Rack theme, applied once here and inherited by every child
    // through the Component::getLookAndFeel() chain.
    setLookAndFeel (&azLookAndFeel_);

    // Lane D (data loop): every controller's Set/Clear events feed the
    // session log. Wired here, with every detector thread still stopped, so
    // setEventSink()'s precondition holds; see the destructor for the
    // matching teardown order.
    for (auto& controller : notchControllers_)
        controller->setEventSink ([this] (const NotchController::NotchEvent& e)
        {
            // I-2: notchEventToVar() rounds and boxes up to three 1025-value
            // spectra. 26 tests and the snapshot tool construct MainComponent
            // with the logger never started, and log() would drop the var on
            // arrival anyway -- so do not build it. isActive() is a relaxed
            // atomic load; a stop() racing this line only costs the event the
            // logger was about to drop regardless.
            if (sessionLogger_.isActive())
                sessionLogger_.log (notchEventToVar (e));
        });

    //==========================================================================
    // LANE M (Task 10). THE THREE CALLBACKS ARE ASSIGNED HERE, ONCE, AND NEVER
    // AGAIN (SoundcheckController.h, I-10/N-2). They are bare public
    // std::functions with no lock, invoked from the lane M thread AND from
    // whatever thread calls stop() / abortAndJoin() / the destructor -- so
    // assigning one while it could be invoked is a data race. This is the only
    // point in the program's life where no such thread can exist: the
    // controller has never been started.
    soundcheck_.setDetectionActiveOnAllSlots = [this] (bool on)
    {
        // ONE RELAXED ATOMIC STORE PER SLOT AND NOTHING ELSE
        // (NotchController::setDetectionActive). That is the entire reason this
        // is safe from the lane M thread, and the entire reason it lives HERE
        // rather than inside SoundcheckController, which holds no
        // NotchController pointer at all (inv 17).
        //
        // It restores detection UNCONDITIONALLY, which is not the same thing as
        // restoring the MODE's gating -- Bypass means no detection, and a
        // disabled slot has no chain to protect. endSoundcheckSession() runs
        // applyModeGating over every slot on the message thread afterwards,
        // which is where that difference is settled.
        for (auto& c : notchControllers_)
            c->setDetectionActive (on);
    };
    soundcheck_.logEvent = [this] (const juce::var& v) { sessionLogger_.log (v); };
    soundcheck_.onStateChanged = [this]
    {
        // LANE M THREAD (and the caller thread of stop()/abortAndJoin()/~dtor).
        // A relaxed store is ALL it may do: every component touch happens later
        // in syncSoundcheckUi(), on the message thread, off the status timer.
        soundcheckDirty_.store (true, std::memory_order_release);
    };

    // Seed the shipped presets exe-adjacent -> user dir, never overwriting.
    // Source: <exe dir>/presets (the installer puts them there, P1). Running
    // from the repo, or from a test/snapshot exe with no presets/ beside it,
    // the source dir is simply absent -- seedDefaultPresets reports that in
    // SeedResult::errors and the app carries on. Message thread, runs once,
    // touches no audio state.
    {
        const auto exeDir = juce::File::getSpecialLocation (
            juce::File::currentExecutableFile).getParentDirectory();
        const auto seeded = presetfirstrun::seedDefaultPresets (
            exeDir.getChildFile ("presets"),
            PresetManager::getPresetDirectory());
        // Result deliberately ignored: the only failure modes are a missing
        // source dir (the normal repo/test/snapshot case) or a copy error, and
        // neither should block startup -- the app runs fine without seeded
        // presets, and logging the missing-source case would just be noise.
        juce::ignoreUnused (seeded);
    }

    // The window sizes itself from this (DocumentWindow::setContentOwned), so
    // an unsized content component opens at the resize LIMIT instead.
    //
    // Taken as the LARGER of the nominal default and the height the floor
    // actually needs: the routing table's height depends on how many rows are
    // showing, so a fixed number is a guess that goes stale the moment that
    // changes. parentHierarchyChanged() re-checks once a real window exists.
    setSize (kDefaultWidth, juce::jmax (kDefaultHeight, heightThatFitsTheFloor()));

    // Per-slot tuning (brief 2026-08-24): every slot starts on Global.
    slotUsesGlobalTuning_.fill (true);

    addAndMakeVisible (spectrumView_);

    // RING RISK (docs/spec-ring-risk.md section 4, lane R ruling A-R6). The
    // number comes from the DETECTOR of the slot the console is MONITORING --
    // not slot 0, and never from anything the GUI computes for itself: a
    // second, separately-derived peakiness on screen would disagree with the
    // one the filters follow, and the operator could not tell which.
    //
    // The band this returns is RAW. SpectrumView::timerCallback runs it
    // through the 750 ms anti-flicker hold before anything is painted, so
    // holding here as well would hold twice.
    //
    // A fresh copySnapshot per tick, as the spec asks: ~8 KB at 30 fps, off
    // one uncontended mutex, on the message thread. Reusing the view's own
    // snapshot_ would save that and couple the readout to the refresh order
    // of the plot -- not a trade worth making before anything has measured a
    // problem.
    spectrumView_.ringRiskProvider = [this]
    {
        NotchController::SnapshotBuffer snapshot {};
        notchControllers_[(std::size_t) displayedSlot_]->copySnapshot (snapshot);
        return gui::SpectrumView::riskForScore (snapshot);
    };

    addAndMakeVisible (modeRail_);
    // LANE M Task 9. Added as a CHILD COMPONENT, not made visible: the panel
    // starts in Mode::Hidden and shows itself when the owner sets a mode.
    // Added AFTER spectrumView_ so it paints over the analyser rather than
    // under it. Nothing here connects it to a SoundcheckController -- that is
    // Task 10; this console only owns, lays out and renders it.
    addChildComponent (soundcheckPanel_);

    // The strip takes a band off the analyser rather than covering it, so a
    // mode change changes the layout and the console has to be told.
    soundcheckPanel_.onModeChanged = [this] { resized(); };
    addAndMakeVisible (deviceDrawer_);
    addAndMakeVisible (slotScroller_);
    // The notch list is a FIXED bottom strip -- always visible.
    addAndMakeVisible (notchListPanel_);
    // statusBar_ / modeBar_ stay alive but hidden: see MainComponent.h.

    // Lane D: a FALSE verdict is written to the log BEFORE the clear it
    // causes -- the clear's own notch_clear event carries reason
    // verdict_false, but the verdict itself (which lane/index/hz a human
    // rejected) only exists here.
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

    modeBar_.onModeRequested = [this] (AudioEngine::Mode mode) { requestMode (mode); };

    // New console wiring. The rail requests modes through the same
    // requestMode() path the old bar used -- one route to the engine.
    modeRail_.onSoundcheck = [this] { requestMode (AudioEngine::Mode::Soundcheck); };
    modeRail_.onAuto       = [this] { requestMode (AudioEngine::Mode::Auto); };
    modeRail_.onBypass     = [this] { requestMode (AudioEngine::Mode::Bypass); };

    // R-3 already guards this behind ModeRail's confirmation hook. Only the
    // slots the engine has enabled hold live notches.
    modeRail_.onClearAllConfirmed = [this]
    {
        for (int i = 0; i < kMaxSlots; ++i)
            if (engine_.getSlotConfig (i).enabled)
                notchControllers_[(std::size_t) i]->clearAll();
    };
    // LANE M (Q16). A SEPARATE button from SOUNDCHECK, because the two things
    // are separate: SOUNDCHECK waits 15 s for the room to howl on its own, DO
    // plays a swept signal into the PA for up to ~72 s.
    modeRail_.onMeasure = [this] { beginSoundcheck(); };

    // DUNG -- and Esc, which SoundcheckPanel routes through this same callback
    // (SoundcheckPanel::keyPressed). Best effort by construction: the SOUND
    // stops on this thread inside requestStop(); the state machine catches up
    // at its next poll, which is what re-arms detection.
    soundcheckPanel_.onStop = [this]
    {
        soundcheck_.requestStop (SoundcheckController::AbortReason::UserStop);
    };
    soundcheckPanel_.onApply   = [this] { applySoundcheckProposals(); };
    soundcheckPanel_.onDismiss = [this]
    {
        // BO from Results, and the only control an Applied report carries.
        // dismissRequested() is a no-op outside Results, so one lambda serves
        // both without asking which mode the strip happens to be in.
        soundcheck_.dismissRequested();
        endSoundcheckSession (gui::SoundcheckPanel::Mode::Hidden);
    };

    // The confirmation, defaulting to a native ASYNC box for the same reason
    // ModeRail::confirmHook does (R-3): JUCE_MODAL_LOOPS_PERMITTED is off, so
    // nothing here may block. Injectable, so a headless test answers it
    // without a dialog (memory/gui-console-lessons-2026-08-24.md).
    soundcheckConfirmHook = [] (const juce::String& text,
                                std::function<void (bool)> onAnswer)
    {
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel (
            juce::MessageBoxIconType::WarningIcon,
            juce::String::fromUTF8 (kScConfirmTitle),
            text,
            juce::String::fromUTF8 (kScConfirmOk),
            juce::String::fromUTF8 (kScConfirmCancel),
            nullptr);

        juce::NativeMessageBox::showAsync (options,
            [onAnswer = std::move (onAnswer)] (int result) { onAnswer (result == 1); });
    };

    modeRail_.getSoundcheckRemainingMs = [this]
    {
        double remaining = 0.0;
        for (auto& controller : notchControllers_)
            remaining = juce::jmax (remaining, controller->getSoundcheckRemainingMs());
        return remaining;
    };

    // PRESET row (Task P3). The drawer only reports the request; the chooser and
    // the load/save live here. The choosers are injectable so a headless test
    // can hand the inner callback a known file with no native dialog.
    presetLoadChooser = [] (std::function<void (const juce::File&)> onPicked)
    {
        // Kept alive across the async call by the shared_ptr captured in the
        // completion lambda (memory gui-console-lessons-2026-08-24).
        auto chooser = std::make_shared<juce::FileChooser> (
            "Load preset", PresetManager::getPresetDirectory(), "*.json");

        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [chooser, onPicked] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file != juce::File{})   // a cancel returns an invalid file
                    onPicked (file);
            });
    };

    presetSaveChooser = [] (std::function<void (const juce::File&)> onPicked)
    {
        auto chooser = std::make_shared<juce::FileChooser> (
            "Save preset", PresetManager::getPresetDirectory(), "*.json");

        chooser->launchAsync (
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [chooser, onPicked] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file != juce::File{})
                    onPicked (file);
            });
    };

    // The button requests route through the injectable choosers to load/save.
    // The outer lambda is invoked synchronously by the drawer's onClick, so a
    // plain `this` capture there is fine. But the INNER callback (onPicked) is
    // stored inside the chooser and fires later, after launchAsync returns --
    // possibly after MainComponent has been destroyed if the user closes the
    // window while the native picker is open. That inner callback crosses the
    // async boundary back into MainComponent, so it captures a
    // Component::SafePointer and no-ops if the component is already gone. The
    // shared_ptr<FileChooser> above keeps the chooser itself alive for the
    // duration of the async call; the SafePointer guards the completion.
    deviceDrawer_.onLoadRequested = [this]
    {
        if (presetLoadChooser)
        {
            const juce::Component::SafePointer<MainComponent> safe (this);
            presetLoadChooser ([safe] (const juce::File& f)
            {
                if (safe != nullptr)
                    safe->loadPreset (f);
            });
        }
    };
    deviceDrawer_.onSaveRequested = [this]
    {
        if (presetSaveChooser)
        {
            const juce::Component::SafePointer<MainComponent> safe (this);
            presetSaveChooser ([safe] (const juce::File& f)
            {
                if (safe != nullptr)
                    safe->savePreset (f);
            });
        }
    };

    // The protection badge is MASTHEAD furniture, not drawer furniture. It
    // answers the question the whole window exists to answer, so it belongs in
    // the one band that is always visible and never scrolls.
    addAndMakeVisible (statusBadge_);

    // The masthead's slot selector. It reflects and reports; what a selection
    // MEANS is owned here, in setDisplayedSlot.
    slotTabs_.setSlotCount (slotPanel_.getVisibleRowCount());
    slotTabs_.onSlotSelected = [this] (int slot) { setDisplayedSlot (slot); };

    // Hosted BY the notch panel, so it sits directly above the table it
    // changes. It re-points the analyser too -- that is this class's job to
    // know, not the panel's.
    notchListPanel_.setSlotTabs (&slotTabs_);

    // Seed the display at slot 0 through the SAME route a click takes, so the
    // notch table's caption names its slot from the first frame instead of
    // only after the user has picked something.
    setDisplayedSlot (0);

    // Bridge design §6.5: a device change in the panel is always an engine
    // RESTART, and the rings are cleared on the way -- so the detector thread
    // must be joined first and relaunched after. These hooks live on the ONE
    // DevicePanel instance; re-parenting it into the drawer changed nothing.
    devicePanel_.onBeforeRestart = [this]
    {
        // LANE M FIRST (F15). audioDeviceAboutToStart() clears EVERY ring, and
        // that is only legal with "no producer and no consumer running"
        // (LockFreeRingBuffer.h) -- micCapture_ joined that block in Task 5, and
        // the lane M thread is its consumer. abortAndJoin() requests the stop,
        // JOINS, and only then stands the run down, so the caller is the one
        // thread left inside the machine (C-2).
        //
        // It is also the moment that makes the injected callbacks safe to touch
        // again (I-10) -- not that this hook touches them.
        // C-1 (round 2). EVERY CONFIRMATION ASKED BEFORE THIS POINT IS NOW
        // STALE, and the state checks alone cannot see that: with the machine
        // Idle and the box open, this hook tears the console down and puts the
        // lock back to None, so a later OK would find "Idle, unlocked, fine"
        // and arm with targets captured against the PREVIOUS device's routing.
        ++soundcheckConsoleGeneration_;

        // WAS THERE A MEASUREMENT TO LOSE? Mode::Applied is NOT one (review
        // M-2): the run is over, its proposals are placed, and the report on
        // screen is a record of work already done. Saying "your measurement was
        // stopped" there is a lie about something that finished successfully.
        const bool wasMeasuring =
            soundcheck_.getState() != SoundcheckController::State::Idle
            || soundcheckPanel_.getMode() == gui::SoundcheckPanel::Mode::Running;

        const bool idle = soundcheck_.abortAndJoin();

        if (! idle)
        {
            // The machine did NOT reach Idle after a request-join-stand-down.
            // That is not "your measurement was cancelled", it is a state this
            // code believes impossible -- the poll thread is joined and the
            // caller is the only thread left (C-2). A DIFFERENT sentence,
            // because the operator's next move is different: check the device
            // before measuring again (review M-4).
            jassertfalse;
            showMessage (juce::String::fromUTF8 (kScRestartJoinFailed));
        }
        else if (wasMeasuring)
        {
            // A restart the operator did not ask for CAN happen (the spec says
            // so), so it aborts rather than being locked out -- but a
            // measurement that vanished has to SAY it vanished, or the operator
            // waits for results that will never come.
            showMessage (juce::String::fromUTF8 (kScAbortedByRestart));
        }

        // Unlock, hand detection back to the mode, put the strip away. A run
        // torn down by a device change has no results worth showing.
        endSoundcheckSession (gui::SoundcheckPanel::Mode::Hidden);

        // §6.5 for EVERY slot: all detector threads must be joined before a
        // device restart can clear the rings.
        for (auto& controller : notchControllers_)
            controller->stop (1000);
    };
    devicePanel_.onAfterRestart  = [this]
    {
        // setWidth() is only legal while the thread is stopped, so the width
        // from the (freshly restarted) engine config is applied here, then
        // the poll loop relaunches.
        for (int i = 0; i < kMaxSlots; ++i)
        {
            auto& controller = *notchControllers_[(std::size_t) i];
            controller.setWidth (engine_.getSlotConfig (i).width);
            controller.start();
        }

        // *** abortAndJoin() LEFT THE POLL THREAD STOPPED *** and nothing
        // restarts it there (SoundcheckController.h:219-220). Without this line
        // the next DO would arm a run that never polls: the first target would
        // sit in NoiseFloor for ever with the taps suspended and detection off.
        soundcheck_.start();
    };

    // A setting the hardware refused. Held rather than flashed: the user needs
    // to still be reading it a few seconds later.
    devicePanel_.onMessage = [this] (const juce::String& message)
    {
        panelMessage_ = message;
        refreshStatus();
    };

    // The routing table never touches the engine itself: a mapping change is
    // the SAME §6.5 shape as a device change, and it goes through the ONE
    // restart cycle (changeSlotConfig, shared with nothing else) rather than
    // re-typing the thread-join/restart bodies here.
    slotScroller_.setViewedComponent (&slotPanel_, false);
    // The Add button grows the table through this callback: MainComponent::
    // resized() is what sizes slotPanel_ from getPreferredHeight(), and the
    // Viewport parent alone never would.
    slotPanel_.onPreferredHeightChanged = [this]
    {
        // Revealing a routing row also makes that slot selectable: a slot with
        // no row in the table is a slot the user has no way to configure, and
        // monitoring one would show a spectrum they cannot act on.
        slotTabs_.setSlotCount (slotPanel_.getVisibleRowCount());

        if (displayedSlot_ >= slotPanel_.getVisibleRowCount())
            setDisplayedSlot (0);

        // The "+ Add slot" button revealed a row; the window takes the height
        // to show it rather than pushing the button under a scrollbar. Called
        // BEFORE resized() so the layout runs once, at the final size.
        growWindowToFitFloor();

        resized();
    };
    slotPanel_.onSlotConfigChanged = [this] (int slotIndex, const SlotConfig& config)
    {
        changeSlotConfig (slotIndex, config);
        slotPanel_.refresh();
    };

    // Per-slot LINK/INDEP (Task 7): the panel only ever asks for/reports the
    // policy; setSlotLinked/isSlotLinked own what it means at runtime.
    slotPanel_.onSlotLinkChanged = [this] (int slotIndex, bool linked)
    {
        setSlotLinked (slotIndex, linked);
    };
    slotPanel_.slotLinkedProvider = [this] (int slotIndex)
    {
        return isSlotLinked (slotIndex);
    };

    // Per-slot tuning (brief 2026-08-24): the panel reports a COMPLETE
    // SlotTuning; here is where it means something. Global just flips the
    // flag -- the strip keeps driving the controller. Custom applies the five
    // values to THAT slot's controller through the existing clamping setters.
    slotPanel_.onSlotTuningChanged = [this] (int slotIndex,
                                             const gui::SlotPanel::SlotTuning& t)
    {
        if (slotIndex < 0 || slotIndex >= kMaxSlots)
            return;

        auto& controller = *notchControllers_[(std::size_t) slotIndex];
        slotUsesGlobalTuning_[(std::size_t) slotIndex] = t.usesGlobal;

        if (! t.usesGlobal)
        {
            controller.setRiseReferenceMs (t.riseMs);
            controller.setPersistenceBlocks (t.persist);
            controller.setNotchDefaults (t.q, t.depthDb);
            controller.setPeakinessThreshold ((float) t.thr);
        }

        auto v = SessionLogger::makeEvent ("tuning");
        auto* o = v.getDynamicObject();
        o->setProperty ("slot", slotIndex);
        o->setProperty ("uses_global", t.usesGlobal);
        o->setProperty ("rise_ms", t.riseMs);
        o->setProperty ("persist", t.persist);
        o->setProperty ("q", t.q);
        o->setProperty ("depth_db", t.depthDb);
        o->setProperty ("thr", t.thr);
        sessionLogger_.log (v);
    };

    // The editor seeds itself from the slot's controller plus its mode flag.
    slotPanel_.slotTuningProvider = [this] (int slotIndex) -> gui::SlotPanel::SlotTuning
    {
        if (slotIndex < 0 || slotIndex >= kMaxSlots)
            return {};

        const auto& c = *notchControllers_[(std::size_t) slotIndex];

        gui::SlotPanel::SlotTuning t;
        t.usesGlobal = slotUsesGlobalTuning_[(std::size_t) slotIndex];
        t.riseMs  = c.getRiseReferenceMs();
        t.persist = c.getPersistenceBlocks();
        t.depthDb = c.getNotchDepthDb();
        t.q       = c.getNotchQ();
        t.thr     = (double) c.getPeakinessThreshold();
        return t;
    };

    // Detection tuning (brief 2026-08-24): the panel never touches a
    // controller -- a Params change loops over the controllers HERE, exactly
    // like modeRail_'s CLEAR ALL loop. Per-slot tuning (brief 2026-08-24):
    // a slot switched to Custom is SKIPPED -- it keeps its own values until
    // its Tune combo goes back to G. Slot 0 is also the read-back source for
    // the strip: every Global controller carries the same values because every
    // change fans out to all of them.
    addAndMakeVisible (tuningPanel_);
    tuningPanel_.onTuningChanged = [this] (const gui::TuningPanel::Params& p)
    {
        for (int i = 0; i < kMaxSlots; ++i)
        {
            if (! slotUsesGlobalTuning_[(std::size_t) i])
                continue;

            auto& controller = *notchControllers_[(std::size_t) i];
            controller.setRiseReferenceMs ((double) p.riseReferenceMs);
            controller.setPersistenceBlocks (p.persistenceBlocks);
            controller.setNotchDefaults ((double) p.q, (double) p.depthDb);
            controller.setPeakinessThreshold (p.peakinessThreshold);
        }

        auto v = SessionLogger::makeEvent ("tuning");
        auto* o = v.getDynamicObject();
        o->setProperty ("slot", -1);
        // M-8: TuningPanel::Params holds int/float where SlotPanel::SlotTuning
        // holds double. Cast so BOTH tuning events carry the same JSON types
        // for the same field -- logstats reads one column, not two.
        o->setProperty ("rise_ms", (double) p.riseReferenceMs);
        o->setProperty ("persist", p.persistenceBlocks);
        o->setProperty ("q", (double) p.q);
        o->setProperty ("depth_db", (double) p.depthDb);
        o->setProperty ("thr", (double) p.peakinessThreshold);
        sessionLogger_.log (v);
    };
    tuningPanel_.paramsProvider = [this]
    {
        const auto& c = *notchControllers_[0];
        return gui::TuningPanel::Params { (int) c.getRiseReferenceMs(),
                                          c.getPersistenceBlocks(),
                                          (int) c.getNotchDepthDb(),
                                          (int) c.getNotchQ(),
                                          c.getPeakinessThreshold() };
    };

    // Enumeration is safe with no device open -- every AudioEngine query used
    // here guards getCurrentAudioDevice() being null. The rate and buffer
    // combos will come up empty and are refilled by startAudio().
    devicePanel_.refresh();
    modeBar_.setDisplayedMode (engine_.getMode());
    refreshStatus();

    startTimer (kStatusRefreshMs);

    setSize (800, 600);
}

MainComponent::~MainComponent()
{
    stopTimer();

    // LANE M FIRST, and explicitly rather than by relying on member order.
    // stop() stands the RUN down, not just the thread: it joins, then restores
    // detection, lifts the tap suspension and logs the abort ON THIS THREAD. It
    // needs engine_, sessionLogger_ and every NotchController alive to do that,
    // and all three are still alive here. (soundcheck_ is also the LAST-declared
    // member, so its own destructor would run before any of them die anyway --
    // this line just makes the ordering a statement instead of an inference.)
    soundcheck_.stop (2000);
    // Detach the look and feel while every child is still alive -- a
    // Component must not outlive the LookAndFeel it points at.
    setLookAndFeel (nullptr);
    // §6.5: every detector thread must be dead before the engine tears down.
    // Lane D (amendment A-3): each stop() flushes its last events into the
    // logger, which is still alive -- sessionLogger_ is declared before
    // systemClock_ and the controllers, so it outlives every one of these
    // joins.
    for (auto& controller : notchControllers_)
        controller->stop (1000);
    sessionLogger_.stop();   // session_end, then the file closes
    for (auto& controller : notchControllers_)
        controller->setEventSink (nullptr);   // threads are joined: precondition holds
    engine_.stop();
}

AudioEngine& MainComponent::getAudioEngine()
{
    return engine_;
}

NotchController* MainComponent::getNotchControllerForTest (int slot)
{
    if (slot < 0 || slot >= kMaxSlots)
        return nullptr;

    return notchControllers_[(std::size_t) slot].get();
}

void MainComponent::setSlotLinked (int slotIndex, bool linked)
{
    if (slotIndex < 0 || slotIndex >= kMaxSlots)
        return;

    slotLinked_[(std::size_t) slotIndex] = linked;
    notchControllers_[(std::size_t) slotIndex]->setLinked (linked);
}

bool MainComponent::isSlotLinked (int slotIndex) const
{
    return slotIndex >= 0 && slotIndex < kMaxSlots && slotLinked_[(std::size_t) slotIndex];
}

//==============================================================================
// Lane D (data loop): the session log.

juce::var MainComponent::notchEventToVar (const NotchController::NotchEvent& e)
{
    using Ev = NotchController::NotchEvent;
    // B-3: `ev` is the key tools/logstats.py dispatches on, and the ternary
    // this replaced would have written every Retune as a notch_clear -- which
    // closes the notch's record at its first 300 ms deepening and makes every
    // deepened notch look like a 300 ms false positive.
    const char* evName = e.kind == Ev::Kind::Set    ? "notch_set"
                       : e.kind == Ev::Kind::Retune ? "notch_retune"
                                                    : "notch_clear";
    auto v = SessionLogger::makeEvent (evName);
    auto* o = v.getDynamicObject();
    o->setProperty ("slot", e.slot);
    o->setProperty ("lane", e.lane);
    o->setProperty ("index", e.index);
    o->setProperty ("hz", (double) e.hz);
    o->setProperty ("q", (double) e.q);
    o->setProperty ("depth_db", (double) e.depthDb);
    o->setProperty ("origin", originName (e.origin));

    if (e.kind == Ev::Kind::Retune)
    {
        o->setProperty ("reason", retuneReasonName (e.retuneReason));
        o->setProperty ("from_db", (double) e.fromDepthDb);
        o->setProperty ("age_ms", e.ageMs);
        return v;   // no score, no ctx: a retune is not a placement decision
    }

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
        c->setProperty ("now", spectrumVar (e.ctx->now));
        // M-3: refAgeMs describes the reference FRAME. With no reference
        // frame it is 0.0, which reads as "compared against something 0 ms
        // old" -- so it travels inside the same guard as "ref".
        if (e.ctx->hasRef)
        {
            c->setProperty ("ref", spectrumVar (e.ctx->ref));
            c->setProperty ("ref_age_ms", e.ctx->refAgeMs);
        }
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
        for (int l = 0; l < cfg.width; ++l)
        {
            in.add (cfg.inputChannels[l]);
            out.add (cfg.outputChannels[l]);
        }
        s->setProperty ("in", in);
        s->setProperty ("out", out);
        s->setProperty ("linked", slotLinked_[(std::size_t) i]);
        slots.add (juce::var (s));
    }
    o->setProperty ("slots", slots);

    return v;
}

void MainComponent::setAppVersion (const juce::String& version)
{
    appVersion_ = version;
}

void MainComponent::showMessage (const juce::String& message)
{
    panelMessage_ = message;
    refreshStatus();
}

bool MainComponent::startSessionLog (const juce::File& directory)
{
    if (! sessionLogger_.start (directory, sessionHeader()))
        return false;

    // T6: the log must record the mode the session STARTED in. requestMode()
    // is the only other producer of a `mode` event, so a session nobody ever
    // switches would otherwise carry none at all and a reader could not tell
    // Auto from Bypass.
    auto v = SessionLogger::makeEvent ("mode");
    v.getDynamicObject()->setProperty ("mode", modeName (engine_.getMode()));
    sessionLogger_.log (v);
    return true;
}

void MainComponent::stopSessionLog()
{
    sessionLogger_.stop();
}

juce::File MainComponent::sessionLogFileForTest() const
{
    return sessionLogger_.currentFile();
}

int MainComponent::lastLoadSkippedNotchesForTest() const
{
    return lastLoadSkipped_;
}

void MainComponent::startAudio()
{
    // Task 16: prefer ASIO, but do NOT require it. The plan said "filter to
    // show ASIO devices only"; the ASIO SDK is excluded from this repo for
    // licensing reasons, so on CI and on this dev machine the ASIO type is
    // never registered and an ASIO-only list is empty. Asking the engine for a
    // type that IS registered also matters: AudioEngine's own default is the
    // literal string "ASIO", and JUCE silently keeps whatever type is current
    // when the requested one does not exist.
    engine_.setAudioDeviceType (gui::chooseDefaultDeviceType (engine_.getAvailableDeviceTypeNames()));
    engine_.start();

    // The device is open: the detectors can start pumping. start() is a no-op
    // if the thread already runs. Width first, for the same reason as the
    // after-restart hook: setWidth() needs the thread stopped.
    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& controller = *notchControllers_[(std::size_t) i];
        controller.setWidth (engine_.getSlotConfig (i).width);
        controller.start();
    }

    // The lane M poll thread follows the DEVICE, exactly as the detectors do:
    // arm() refuses outright while the engine is not running, so there is
    // nothing for it to poll before this point. start() is idempotent.
    soundcheck_.start();

    // Only now do getAvailableSampleRates() and getAvailableBufferSizes()
    // return anything.
    devicePanel_.refresh();
    // Same for the routing table's channel names: empty until the device is
    // open, which is why refresh() has to run again here.
    slotPanel_.refresh();
    refreshStatus();
}

void MainComponent::requestMode (AudioEngine::Mode mode)
{
    // Soundcheck sets the mode and nothing else. The specified countdown is NOT
    // implemented, deliberately: owner decision D-06 freezes the detector's
    // timers while the tap is dead, so a GUI-side wall-clock juce::Timer would
    // disagree with a frozen detector and show a countdown that does not match
    // what the app is doing. The remaining time has to come from the detector's
    // getSoundcheckSecondsRemaining() (bridge design section 4), which does not
    // exist yet.
    engine_.setMode (mode);
    modeBar_.setDisplayedMode (engine_.getMode());

    {
        auto v = SessionLogger::makeEvent ("mode");
        v.getDynamicObject()->setProperty ("mode", modeName (engine_.getMode()));
        sessionLogger_.log (v);
    }

    // KD-9 detection gating lives HERE because this is the one object that owns
    // both the mode controls and the controllers: Bypass must never place a
    // notch, Soundcheck detects for its 15 s live-time window (KD-7 exempts its
    // notches from auto-release), Auto detects continuously. Applied to every
    // slot the engine has enabled -- a disabled slot has no live chain to
    // protect and its controller must stay silent.
    for (int i = 0; i < kMaxSlots; ++i)
        applyModeGating (i);
}

void MainComponent::applyModeGating (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= kMaxSlots)
        return;

    auto& controller = *notchControllers_[(std::size_t) slotIndex];

    // A DISABLED SLOT IS GATED OFF, not skipped (lane M round 2, I-2). It used
    // to return here, which was harmless while nothing ever armed detection
    // behind this function's back -- and then lane M's restore did exactly
    // that, with one unconditional store per slot (inv 17 allows it nothing
    // else). A disabled slot has no live chain to protect, and a detector
    // scoring one is a notch waiting to be placed in a chain nobody is
    // listening to.
    if (! engine_.getSlotConfig (slotIndex).enabled)
    {
        controller.setDetectionActive (false);
        return;
    }

    switch (engine_.getMode())
    {
        case AudioEngine::Mode::Bypass:
            controller.setDetectionActive (false);
            break;
        case AudioEngine::Mode::Auto:
            controller.setDetectionActive (true);
            break;
        case AudioEngine::Mode::Soundcheck:
            controller.startSoundcheck();
            break;
    }
}

void MainComponent::changeSlotConfig (int slotIndex, const SlotConfig& config)
{
    jassert (devicePanel_.onBeforeRestart != nullptr);

    if (devicePanel_.onBeforeRestart != nullptr)
        devicePanel_.onBeforeRestart();

    engine_.setSlotConfig (slotIndex, config);

    if (devicePanel_.onAfterRestart != nullptr)
        devicePanel_.onAfterRestart();

    // The restart cycle above restores widths and threads but NOT the
    // detection gate. A slot enabled or re-mapped while Auto/Soundcheck is
    // already running would otherwise stay DEAF until the next mode request
    // -- protection shown on screen that does not exist in the chains. Re-arm
    // it to whatever the current mode says, right now. Soundcheck windows are
    // measured in each controller's OWN live time (D-06), so a slot armed
    // mid-show gets its own full 15 s live-time window; there is no shared
    // clock to inherit a partial one from.
    applyModeGating (slotIndex);
}

//==============================================================================
// LANE M Task 10 -- the DO flow. MESSAGE THREAD, every line of it.

std::vector<SoundcheckController::Target> MainComponent::buildSoundcheckTargets() const
{
    std::vector<SoundcheckController::Target> targets;
    targets.reserve ((std::size_t) kMaxSlots * (std::size_t) kMaxSlotLanes);

    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        const auto cfg = engine_.getSlotConfig (slot);

        if (! cfg.enabled)
            continue;

        // NOT deduplicated by output channel. Two slots feeding one output are
        // two DIFFERENT loops -- different microphones round the same speaker --
        // and each one's notches belong to its own slot. The spec's worst case
        // (8 stereo slots, 16 outputs, ~72 s) is exactly this count.
        const int lanes = juce::jlimit (0, kMaxSlotLanes, cfg.width);

        for (int lane = 0; lane < lanes; ++lane)
        {
            SoundcheckController::Target t;
            t.slot       = slot;
            t.lane       = lane;
            t.inChannel  = cfg.inputChannels[lane];
            t.outChannel = cfg.outputChannels[lane];
            targets.push_back (t);
        }
    }

    return targets;
}

SoundcheckController::RunParams MainComponent::buildSoundcheckRunParams() const
{
    // EVERYTHING A RUN NEEDS, READ HERE, ONCE, AND HANDED OVER FROZEN. A
    // threshold that moved mid-run would score channel 1 and channel 9 of one
    // measurement on two different rulers, and nobody reading the log could
    // tell (SoundcheckController.h, "RunParams IS FROZEN FOR THE RUN").
    const auto& tuning = *notchControllers_[0];

    SoundcheckController::RunParams params;
    // A PEAKINESS RATIO read live from the detector, so lane M's gate cannot
    // drift away from the detector's own (N1). Never a 0..1 score.
    params.noiseFloorGate    = tuning.getPeakinessThreshold();
    params.peak              = SoundcheckController::kSoundcheckMaxPeak;
    params.sampleRate        = engine_.getCurrentSampleRateHz();
    params.numInputChannels  = engine_.getNumInputChannels();
    params.numOutputChannels = engine_.getNumOutputChannels();
    // THE RUNNING PRESET'S CEILING -- the shallowest cut this run may propose.
    // Without it SoundcheckCandidates::Input::ceilingDb keeps its NaN "unset"
    // default and the run produces marks with no proposals at all, which reads
    // downstream as an excellent room (Task 3 I-3). Note that arm() cannot save
    // us from forgetting this line: a defaulted 0.0 is FINITE and passes its
    // isfinite() check, which is why a test asserts the value itself.
    params.ceilingDb         = tuning.getNotchDepthDb();
    params.notchQ            = tuning.getNotchQ();
    return params;
}

int MainComponent::distinctOutputCount (const std::vector<SoundcheckController::Target>& targets)
{
    // At most kMaxSlots * kMaxSlotLanes = 16 targets, so the quadratic scan is
    // cheaper than the allocation it replaces -- and the comment now matches the
    // code, which the first version did not: it claimed "no allocation" over a
    // std::vector.
    int count = 0;

    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        bool seenEarlier = false;

        for (std::size_t j = 0; j < i; ++j)
            seenEarlier = seenEarlier || targets[j].outChannel == targets[i].outChannel;

        if (! seenEarlier)
            ++count;
    }

    return count;
}

juce::String MainComponent::soundcheckConfirmText (const int passCount, const int distinctOutputs)
{
    // Every number DERIVED, never re-typed: the dialog and the machine's own
    // deadlines cannot disagree (SoundcheckController.h kPerTargetMs).
    const double peakDbfs =
        20.0 * std::log10 (juce::jmax ((double) SoundcheckController::kSoundcheckMaxPeak, 1.0e-9));
    const double perPassSec = SoundcheckController::kPerTargetMs / 1000.0;
    const int    passes     = juce::jmax (0, passCount);
    const int    outputs    = juce::jlimit (0, passes, distinctOutputs);
    // THE PASSES, not the channels: a channel two slots feed goes quiet twice,
    // and the machine spends the time twice (review M-1).
    const double totalSec   = perPassSec * (double) passes;

    return juce::String::fromUTF8 (kScConfirmHead)
         + juce::String (juce::roundToInt (-peakDbfs))
         + juce::String::fromUTF8 (kScConfirmHead2)
         + juce::String::fromUTF8 (kScConfirmSilence)
         + juce::String (perPassSec, 1)
         + juce::String::fromUTF8 (kScConfirmSilence2)
         + juce::String (passes)
         + juce::String::fromUTF8 (kScConfirmPasses)
         + juce::String (outputs)
         + juce::String::fromUTF8 (kScConfirmTotal)
         + juce::String (totalSec, 1)
         + juce::String::fromUTF8 (kScConfirmTotal2);
}

juce::String MainComponent::soundcheckRefusalMessage (const SoundcheckController::Refusal r)
{
    switch (r)
    {
        case SoundcheckController::Refusal::None:               break;
        case SoundcheckController::Refusal::EngineNotRunning:   return juce::String::fromUTF8 (kScEngineNotRunning);
        case SoundcheckController::Refusal::NoChannels:         return juce::String::fromUTF8 (kScNoChannels);
        case SoundcheckController::Refusal::SlotDisabled:       return juce::String::fromUTF8 (kScSlotDisabled);
        case SoundcheckController::Refusal::InvalidChannelPair: return juce::String::fromUTF8 (kScInvalidChannelPair);
        case SoundcheckController::Refusal::RingRiskRising:     return juce::String::fromUTF8 (kScRingRiskRising);
        case SoundcheckController::Refusal::InvalidParams:      return juce::String::fromUTF8 (kScInvalidParams);
        case SoundcheckController::Refusal::AlreadyRunning:     return juce::String::fromUTF8 (kScAlreadyRunning);
        case SoundcheckController::Refusal::RampOutPending:     return juce::String::fromUTF8 (kScRampOutPending);
    }

    // Same fallthrough discipline as originName/reasonName above: an enumerator
    // added without a sentence here says so, rather than silently reusing
    // somebody else's reason.
    return juce::String::fromUTF8 (kScUnknownRefusal);
}

void MainComponent::beginSoundcheck()
{
    // ONE DIALOG AT A TIME (review I-2). Two stacked confirmations are two
    // arms: the second OK arrives against targets the first already consumed,
    // and arm() would either refuse (AlreadyRunning) with the operator
    // believing they started something, or -- if the first run had already
    // finished -- start a SECOND sweep nobody asked for. ModeRail::
    // handleClearAllClicked carries exactly this guard for exactly this reason.
    if (soundcheckConfirmPending_)
    {
        // A dead button that also says nothing is a button the operator presses
        // twice and then distrusts. DO is disabled here too (updateMeasureEnabled
        // shares this exact condition), so this only fires when something drove
        // the callback directly -- but it costs one line to be honest about it.
        showMessage (juce::String::fromUTF8 (kScConfirmAlreadyOpen));
        return;
    }

    if (soundcheck_.getState() != SoundcheckController::State::Idle)
    {
        showMessage (juce::String::fromUTF8 (kScAlreadyRunning));
        return;
    }

    // A DEVICE ERROR LATCHES until a successful start() (AudioEngine), so an
    // arm here would abort within a poll or two with nothing said. Surface it
    // BEFORE the dialog: the operator is being asked to drop their master for a
    // measurement that cannot happen.
    const auto deviceError = engine_.getLastDeviceError();
    if (deviceError.isNotEmpty())
    {
        showMessage (juce::String::fromUTF8 (kScDeviceError) + deviceError);
        return;
    }

    const auto targets = buildSoundcheckTargets();

    // The RING RISK the dialog is answered against comes from the detector of
    // the slot the console is MONITORING -- the same source the on-screen chip
    // uses, so the two can never disagree (lane R, A-R6).
    NotchController::SnapshotBuffer risk {};
    notchControllers_[(std::size_t) displayedSlot_]->copySnapshot (risk);

    const auto refusal = soundcheck_.preflight (targets, risk);
    if (refusal != SoundcheckController::Refusal::None)
    {
        showMessage (soundcheckRefusalMessage (refusal));
        return;
    }

    if (soundcheckConfirmHook == nullptr)
    {
        // NO HOOK MEANS NO RUN. The alternative -- arming an unconfirmed sweep
        // because the dialog was missing -- is the one failure mode this
        // confirmation exists to prevent.
        showMessage (juce::String::fromUTF8 (kScNoConfirmHook));
        return;
    }

    askForSoundcheckConfirmation (targets);
}

void MainComponent::askForSoundcheckConfirmation (
    const std::vector<SoundcheckController::Target>& targets)
{
    if (soundcheckConfirmPending_ || soundcheckConfirmHook == nullptr)
        return;

    // DO GOES DEAD FOR THE LIFE OF ITS OWN DIALOG (review I-2), and comes back
    // on every path that does not end in an armed run: a HUY, a stale answer
    // after the console moved on, or an arm() that refused. On the path that
    // DOES arm, the Measuring lock takes ownership of the button before this
    // one would have restored it.
    soundcheckConfirmPending_ = true;
    updateMeasureEnabled();

    // The answer arrives after this returns, possibly after the window has been
    // closed, so it crosses back through a SafePointer -- the same pattern the
    // preset choosers use.
    const juce::Component::SafePointer<MainComponent> safe (this);
    // THE CONSOLE THIS QUESTION WAS ASKED ABOUT (C-1, round 2).
    const auto generation = soundcheckConsoleGeneration_;

    soundcheckConfirmHook (soundcheckConfirmText ((int) targets.size(),
                                                  distinctOutputCount (targets)),
        [safe, targets, generation] (bool confirmed)
        {
            if (safe == nullptr)
                return;

            safe->soundcheckConfirmPending_ = false;

            if (! confirmed)
            {
                safe->updateMeasureEnabled();
                return;
            }

            // A STALE OK -- and the GENERATION is what catches it, not the
            // state. The console can be torn down and put back to Idle and
            // unlocked while the box is open (a device restart does exactly
            // that), and a state check would then see nothing wrong and arm
            // with `targets` captured against the previous device's routing.
            // The state checks stay as a second line: another run may have
            // started from somewhere else entirely.
            if (generation != safe->soundcheckConsoleGeneration_
                || safe->soundcheck_.getState() != SoundcheckController::State::Idle
                || safe->soundcheckLock_ != SoundcheckLock::None)
            {
                safe->updateMeasureEnabled();
                // SAY SO. An OK that does nothing, silently, teaches the
                // operator that the button is unreliable.
                safe->showMessage (juce::String::fromUTF8 (kScStaleConfirmation));
                return;
            }

            safe->armSoundcheck (targets);
        });
}

void MainComponent::armSoundcheck (const std::vector<SoundcheckController::Target>& targets)
{
    // S-1: a FRESH risk snapshot. preflight ran before the dialog and a room can
    // start ringing while the operator reads it; this is the read arm() logs.
    NotchController::SnapshotBuffer risk {};
    notchControllers_[(std::size_t) displayedSlot_]->copySnapshot (risk);

    // The poll thread must exist BEFORE the machine leaves Idle: arm() enters
    // the first target itself and every phase after that is a poll. start() is
    // idempotent (juce::Thread::startThread returns false while running), so
    // this costs nothing on the normal path where startAudio() already ran --
    // and covers the path where a device was opened without it.
    soundcheck_.start();

    const auto refusal = soundcheck_.arm (targets, buildSoundcheckRunParams(), risk);

    if (refusal != SoundcheckController::Refusal::None)
    {
        // inv 19: not one sample was emitted and the state is still Idle. The
        // console is therefore NOT locked -- locking before the arm, as the
        // brief sketched, would leave the operator shut out of their own mode
        // buttons because of a refusal.
        showMessage (soundcheckRefusalMessage (refusal));
        updateMeasureEnabled();
        return;
    }

    setSoundcheckLock (SoundcheckLock::Measuring);

    // Show the strip now rather than up to one status tick later: the operator
    // pressed a button and the PA is about to go quiet.
    soundcheckDirty_.store (true, std::memory_order_release);
    syncSoundcheckUi();
}

void MainComponent::setSoundcheckLock (const SoundcheckLock lock)
{
    soundcheckLock_ = lock;

    const bool measuring = lock == SoundcheckLock::Measuring;
    // Measuring OR Pending: a run in flight, or proposals on screen waiting for
    // an answer. Both are states in which the chain must not move under the
    // operator.
    const bool held      = lock != SoundcheckLock::None;

    // SOUNDCHECK / AUTO / BYPASS / CLEAR ALL come BACK the moment the sweep
    // stops (spec 4.3, review I-3). BYPASS is how a soundman saves a show; a
    // results strip is not a reason to take it away from them.
    modeRail_.setModeControlsEnabled (! measuring);

    // DO does NOT come back with them. A second run while proposals are pending
    // would throw them away without asking, and arm() would refuse it anyway --
    // and an open confirmation holds it down too, whatever the lock says.
    updateMeasureEnabled();

    // PRESET LOAD, PRESET SAVE and every device control that would restart the
    // engine live in the drawer -- devicePanel_ is re-parented INTO it, and
    // Component::isEnabled() walks the parent chain
    // (juce_Component.cpp:3127-3131), so one call covers the whole column.
    // PRESET LOAD is the reason this stays dead through Results: adoptPreset
    // writes at the FILE's indices without checking n.active and would erase
    // every proposal the operator has not answered yet.
    deviceDrawer_.setEnabled (! held);

    // enable / width / routing / LINK-INDEP on every row, by the same
    // parent-chain mechanism. These are the controls that change the chain the
    // run is measuring -- and, through Results, the chain the proposals were
    // computed against (Task 7's stale-linked ruling).
    slotPanel_.setEnabled (! held);

    // The DETECTION strip. RunParams are FROZEN AT ARM, so a DEPTH move here
    // does not reach the run -- but it DOES reach applySoundcheckResults, which
    // clamps every proposal against the controller's live ceiling. Left live,
    // the operator could pull the ceiling two rungs shallower between reading
    // "5 hot spots" and pressing AP DUNG, and get cuts that are not the ones
    // the strip described (review I-1).
    tuningPanel_.setEnabled (! held);

    // THE NOTCH TABLE (review C-1, Critical). Its FALSE verdict button is one
    // click from clearNotch(VerdictFalse) -- a partial CLEAR ALL under another
    // name -- and mid-sweep it removes a notch from the very chain being
    // measured, which silently invalidates the run. The slot selector it hosts
    // freezes with it; that is display-only and the acceptable half of the
    // trade.
    notchListPanel_.setEnabled (! held);
}

void MainComponent::updateMeasureEnabled()
{
    // ONE PREDICATE (C-1, round 2). The button and beginSoundcheck's own guard
    // are now the same condition, so they cannot disagree: before this, a
    // device restart during a confirmation unlocked the console and lit DO back
    // up with the box still on screen -- a button that looked pressable and did
    // nothing, over a dialog whose OK was about to be dropped.
    modeRail_.setMeasureEnabled (soundcheckLock_ == SoundcheckLock::None
                                 && ! soundcheckConfirmPending_);
}

juce::String MainComponent::soundcheckAbortMessage (const SoundcheckController::AbortReason r)
{
    using Reason = SoundcheckController::AbortReason;

    switch (r)
    {
        // THE OPERATOR DID THIS. They pressed DUNG, or Esc; saying "the
        // measurement was stopped" back at them is noise on a status strip that
        // has to stay worth reading.
        case Reason::UserStop:
        case Reason::Esc:                  return {};

        case Reason::MicHot:               return juce::String::fromUTF8 (kScAbortMicHot);
        case Reason::RoomRinging:          return juce::String::fromUTF8 (kScAbortRoomRinging);
        case Reason::EngineStopped:        return juce::String::fromUTF8 (kScAbortEngineStopped);
        case Reason::DeviceError:          return juce::String::fromUTF8 (kScAbortDeviceError);
        case Reason::DeviceChanged:        return juce::String::fromUTF8 (kScAbortDeviceChanged);
        case Reason::CaptureDrop:          return juce::String::fromUTF8 (kScAbortCaptureDrop);
        case Reason::NoiseFloorUnmeasured: return juce::String::fromUTF8 (kScAbortNoiseFloor);
    }

    // An enumerator added without a sentence here falls back to the generic
    // one rather than silently borrowing somebody else's reason -- the same
    // discipline originName/reasonName follow.
    return juce::String::fromUTF8 (kScAbortedByRestart);
}

void MainComponent::announceSoundcheckAbort()
{
    if (! soundcheck_.hasLastAbortReason())
        return;

    const auto message = soundcheckAbortMessage (soundcheck_.getLastAbortReason());

    if (message.isNotEmpty())
        showMessage (message);
}

void MainComponent::applyModeGatingToAllSlots()
{
    for (int i = 0; i < kMaxSlots; ++i)
        applyModeGating (i);
}

gui::SoundcheckPanel::Model MainComponent::soundcheckResultsModel() const
{
    gui::SoundcheckPanel::Model model;

    for (const auto& r : soundcheck_.copyResults())
    {
        // THREE DIFFERENT SENTENCES, kept apart on purpose (F26, Task 3 I-3):
        // "the patch is wrong", "I could not measure this one", and "I had
        // nothing to propose from" are different problems, and none of them is
        // "the room is clean".
        if (r.routingInvalid)                     { ++model.routingInvalid; continue; }
        if (! r.measured)                         { ++model.unmeasured;     continue; }
        if (r.ceilingMissing || r.ladderMissing)  { ++model.cannotPropose;  continue; }

        model.hotSpots      += r.candidateCount;
        model.saturatedBins += r.saturatedBins;
    }

    return model;
}

void MainComponent::refreshSoundcheckOverlay()
{
    // The analyser draws ONE lane of ONE slot, so the overlay is that result and
    // no other. Fed lane M's OWN arrays, never copySnapshot() -- the detector's
    // publish is frozen for the whole run (SpectrumView.h:207-223).
    const int lane = spectrumView_.getDisplayLane();

    for (const auto& r : soundcheck_.copyResultsForSlot (displayedSlot_))
    {
        if (r.lane != lane || ! r.measured || r.routingInvalid)
            continue;

        spectrumView_.setSoundcheckOverlay (r.marginDb.data(), r.marked.data(),
                                            r.trusted.data(),
                                            (int) r.marginDb.size(),
                                            engine_.getCurrentSampleRateHz());
        return;
    }

    // Nothing measured for this lane: an overlay from the OTHER lane would be a
    // curve labelled with the wrong channel.
    spectrumView_.clearSoundcheckOverlay();
}

void MainComponent::applySoundcheckProposals()
{
    // MESSAGE THREAD. This is the ONLY place a preventive notch is written.
    if (soundcheck_.getState() != SoundcheckController::State::Results)
        return;

    SoundcheckApplyStats total;

    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        const auto forSlot = soundcheck_.copyResultsForSlot (slot);

        if (forSlot.empty())
            continue;

        // ONE LEDGER PER SLOT, alive for this component's lifetime: only what a
        // previous apply PLACED is ever replaced, so the notches the operator
        // locked in by hand with the SOUNDCHECK mode switch -- also stamped
        // Origin::Soundcheck -- survive (C-2/Q7).
        const auto stats = applySoundcheckResults (*notchControllers_[(std::size_t) slot],
                                                   slot, forSlot,
                                                   soundcheckLedgers_[(std::size_t) slot]);

        total.placed           += stats.placed;
        total.refused          += stats.refused;
        total.clearedPrevious  += stats.clearedPrevious;
        total.skippedLive      += stats.skippedLive;
        total.skippedOtherSlot += stats.skippedOtherSlot;
        total.skippedBadLane   += stats.skippedBadLane;

        // The FIFTH lane M event, in the shape Task 8 owns -- never a second
        // one inlined here. One per slot applied, so a reader can see WHICH
        // chain the counts belong to; `slot` is the only field added.
        auto ev = makeSoundcheckApplyEvent (stats);
        if (auto* o = ev.getDynamicObject())
            o->setProperty ("slot", slot);
        sessionLogger_.log (ev);
    }

    soundcheck_.applyRequested();

    // THE REPORT. `clearedPrevious > 0 && placed == 0` is a REAL outcome, not a
    // bug and not a success: the previous proposals went and there was no room
    // to put the new ones back, so the operator is LESS protected than before
    // they pressed the button. Mode::Applied exists precisely so that cannot
    // ship silently (SoundcheckPanel.h:72-78).
    auto model = soundcheckResultsModel();
    model.placed          = total.placed;
    model.clearedPrevious = total.clearedPrevious;
    soundcheckPanel_.setResults (model);

    endSoundcheckSession (gui::SoundcheckPanel::Mode::Applied);

    if (model.worseOff())
        showMessage (juce::String::fromUTF8 (kScWorseOffHead)
                     + juce::String (model.clearedPrevious)
                     + juce::String::fromUTF8 (kScWorseOffTail));
}

void MainComponent::endSoundcheckSession (const gui::SoundcheckPanel::Mode panelMode)
{
    // Anything asked before this teardown is answering about a console that no
    // longer exists (C-1). One counter, bumped at every teardown, is what a
    // confirmation's answer is checked against.
    ++soundcheckConsoleGeneration_;

    // An APPLIED report is still a pending thing: its proposals are placed, but
    // the strip is on screen and the ledger, the routing and the ceiling they
    // were computed against must not move under the operator until they dismiss
    // it. Everything else is a full release (review I-3).
    setSoundcheckLock (panelMode == gui::SoundcheckPanel::Mode::Applied
                           ? SoundcheckLock::Pending
                           : SoundcheckLock::None);

    // DETECTION GOES BACK UNDER THE MODE'S RULES, not lane M's. The controller
    // restores it with one relaxed store per slot -- that is all its thread may
    // do (inv 17) -- which would leave BYPASS detecting and would arm a slot the
    // engine has disabled. applyModeGating is this console's own answer to
    // "what should this slot be doing", and it runs here, on the message
    // thread, exactly once per session end.
    //
    // THE WINDOW, NOW THAT BOTH EDGES CLOSE IT (M-3, rewritten in round 2).
    //
    // The controller restores detection on its own thread with one
    // unconditional store per slot -- inv 17 allows it nothing else -- so
    // between that store and a message-thread correction, BYPASS detects and a
    // disabled slot is armed. There are exactly two machine-driven paths, and
    // BOTH are corrected within one status tick (kStatusRefreshMs = 200 ms):
    //
    //   finishRun() -> Results : syncSoundcheckUi's Results edge gates (I-2)
    //   beginAbort() -> Idle   : syncSoundcheckUi's Idle edge lands here
    //
    // On every finger-driven path -- AP DUNG, BO, a device restart -- the gap is
    // ZERO, because those call this synchronously. The previous version of this
    // comment claimed the bound while the Results window, all 20 s of it, was
    // still running ungated; that hole is what I-2 closed.
    //
    // Even at 200 ms nothing can be placed: placement needs persistenceBlocks
    // consecutive confirmations at ~10.7 ms a hop AND a score over threshold,
    // and a bypassed chain is not ringing.
    applyModeGatingToAllSlots();

    if (panelMode == gui::SoundcheckPanel::Mode::Hidden)
        spectrumView_.clearSoundcheckOverlay();

    soundcheckPanel_.setMode (panelMode);

    // Swallow the edge this very call just created, so the next status tick
    // does not run the teardown a second time (and re-start a 15 s passive
    // soundcheck window with it).
    lastSoundcheckState_ = soundcheck_.getState();
    soundcheckDirty_.store (false, std::memory_order_relaxed);
}

double MainComponent::soundcheckCountdownMs() const
{
    return soundcheck_.getRemainingMsInRun();
}

void MainComponent::syncSoundcheckUi()
{
    using State = SoundcheckController::State;
    using Mode  = gui::SoundcheckPanel::Mode;

    const auto state   = soundcheck_.getState();
    const bool dirty   = soundcheckDirty_.exchange (false, std::memory_order_acq_rel);
    const bool changed = dirty || state != lastSoundcheckState_;
    lastSoundcheckState_ = state;

    // A run in flight. Preflight/Confirm/Arm are GUI-owned and this machine
    // never enters them (SoundcheckController.h:124-129), so "not Idle and not
    // Results" is exactly "measuring".
    if (state != State::Idle && state != State::Results)
    {
        if (soundcheckPanel_.getMode() != Mode::Running)
            soundcheckPanel_.setMode (Mode::Running);

        // THE COUNTDOWN IS LANE M'S OWN (F12). getRemainingMsInRun() runs off
        // the controller's injected ClockSource. NotchController::
        // getSoundcheckRemainingMs() -- the PASSIVE 15 s window -- is measured
        // in liveMs_, which is frozen while the taps are suspended, so it would
        // show a number that stands still or reads 0 for the whole run.
        soundcheckPanel_.setProgress (soundcheck_.getCurrentTargetIndex(),
                                      soundcheck_.getTargetCount(),
                                      soundcheckCountdownMs());
        return;
    }

    if (state == State::Results)
    {
        // Rebuilt on an EDGE, not per tick: copyResults() copies three
        // 1025-entry arrays per output, which is ~12 KB a result.
        if (changed || soundcheckPanel_.getMode() != Mode::Results)
        {
            // THE SOUND HAS STOPPED, so the mode rail comes back -- but DO, the
            // preset row, the routing table, the DETECTION strip and the notch
            // verdicts stay dead while proposals are on screen (review I-3).
            setSoundcheckLock (SoundcheckLock::Pending);

            // I-2 (round 2). DETECTION GOES BACK UNDER THE MODE'S RULES HERE
            // TOO, not only when the strip is dismissed. finishRun() restored it
            // with one unconditional store per slot -- that is all the lane M
            // thread may do (inv 17) -- so BYPASS would detect, and a slot the
            // engine has disabled would be armed, for the whole 20 s Results
            // window. This runs on the message thread, so it may call a
            // NotchController.
            applyModeGatingToAllSlots();

            soundcheckPanel_.setResults (soundcheckResultsModel());
            soundcheckPanel_.setMode (Mode::Results);
            refreshSoundcheckOverlay();
        }
        return;
    }

    // Idle. A run that ended anywhere other than AP DUNG lands here: an abort,
    // a BO, or the 20 s Results timeout. Applied is left alone -- its report is
    // the operator's to dismiss, and it was already stood down by the apply.
    if (changed && soundcheckPanel_.getMode() != Mode::Applied)
    {
        // WHY IT ENDED, BEFORE THE STRIP GOES AWAY. beginAbort() decided this
        // on the lane M thread and could not say a word about it from there;
        // this is the only place the operator is ever told that the mic was too
        // hot, or that the room was already ringing. Announced BEFORE the
        // teardown, because endSoundcheckSession() is also what a device
        // restart calls -- and that path has its own sentence.
        announceSoundcheckAbort();
        endSoundcheckSession (Mode::Hidden);
    }
}

bool MainComponent::loadPreset (const juce::File& file)
{
    // F10. PRESET LOAD IS REFUSED WHILE LANE M HOLDS THE CONSOLE, Results
    // included. adoptPreset() writes at the FILE's indices and overwrites
    // without checking n.active (NotchController.cpp setNotchImpl), so a load
    // here would silently erase every proposal the operator has not answered
    // yet -- and would do it to a chain a measurement is still describing.
    //
    // The refusal is VISIBLE: a false return with nothing on screen is
    // indistinguishable from a corrupt file.
    if (soundcheckLock_ != SoundcheckLock::None)
    {
        showMessage (juce::String::fromUTF8 (kScPresetRefused));
        return false;
    }

    // Channel counts come from the OPEN device. With no device yet the engine
    // reports zero, but the loader needs real numbers to clamp each slot's
    // channel mapping against -- stereo is what a default interface implies,
    // and matches what startAudio() will find on the common rig.
    const auto channelCount = [] (int reported)
    {
        return reported > 0 ? reported : 2;
    };

    const auto result = PresetManager::loadFromFile (
        file,
        channelCount (engine_.getNumInputChannels()),
        channelCount (engine_.getNumOutputChannels()));

    if (! result.ok)
        return false;

    if (result.skippedNotchCount > 0)
    {
        // A skipped notch is a WARNING, never a refused file (PresetManager.h).
        // The GUI toast is future work; until then the log carries it.
        juce::Logger::writeToLog (
            "preset \"" + file.getFileName() + "\": skipped "
            + juce::String (result.skippedNotchCount)
            + " notch(es) whose routing slot is out of range");
    }

    // Routing configs land FIRST, so the widths adopted below are read back
    // from the engine state this very load established.
    for (const auto& entry : result.preset.slots)
        engine_.setSlotConfig (entry.index, entry.config);

    // Detectors run exactly while audio does (startAudio / the after-restart
    // hook), so that is also the only time they need stopping for setWidth()
    // and adoptPreset(), whose precondition is a STOPPED detector thread.
    // Stop ALL of them once, mutate, restart once -- the §6.5 shape.
    const bool detectorsRunning = engine_.isRunning();

    if (detectorsRunning)
        for (auto& controller : notchControllers_)
            controller->stop (1000);

    // Width resync for EVERY slot whose config landed from the file -- not
    // just notch-bearing ones. A slot declared mono with zero notches today
    // must not detect on two lanes tomorrow. (The channel-aware loader also
    // auto-adds every referenced slot to this list, so every adopt below is
    // covered too. An out-of-range index was already ignored by
    // setSlotConfig() above and is ignored here for the same reason.)
    for (const auto& entry : result.preset.slots)
    {
        if (entry.index < 0 || entry.index >= kMaxSlots)
            continue;

        setSlotLinked (entry.index, entry.linked);

        notchControllers_[(std::size_t) entry.index]->setWidth (
            engine_.getSlotConfig (entry.index).width);
    }

    // Lane D / lane S loose end (A-9): lastLoadSkipped_ starts from
    // PresetManager's own out-of-range-slot count and adds every notch
    // adoptPreset() itself skips (today: a lane-1 notch on a mono slot) --
    // the two are different rejections and neither subsumes the other.
    lastLoadSkipped_ = result.skippedNotchCount;
    int adoptedTotal = 0;

    for (int s = 0; s < kMaxSlots; ++s)
    {
        std::vector<PresetNotch> notchesForSlot;

        for (const auto& notch : result.preset.notches)
            if (notch.slot == s)
                notchesForSlot.push_back (notch);

        if (! notchesForSlot.empty())
        {
            int skipped = 0;
            adoptedTotal += notchControllers_[(std::size_t) s]->adoptPreset (notchesForSlot, &skipped);
            if (skipped > 0)
            {
                lastLoadSkipped_ += skipped;
                juce::Logger::writeToLog ("preset \"" + file.getFileName() + "\": slot "
                    + juce::String (s + 1) + " is mono, skipped " + juce::String (skipped)
                    + " lane-R notch(es)");
            }
        }
    }

    if (detectorsRunning)
        for (auto& controller : notchControllers_)
            controller->start();

    // Q11, the READ side: the file's CEILING lands on the live controllers.
    //
    // savePreset writes notchDefaults from slot 0; without this nothing ever
    // read it back, so reopening a show tuned at -18 dB left the ceiling
    // wherever the tuning strip happened to be standing and capped every
    // detector notch placed after the load at that value instead.
    //
    // ONLY when the file actually carried the block. `hasNotchDefaults` is
    // false for a v1 preset and for anything written before the ceiling was
    // saved at all, and PresetNotchDefaults' -12 dB fallback must not be
    // mistaken for an operator's choice -- applying it would walk a rig tuned
    // at -18 back two rungs every time an old file was opened.
    //
    // Routing mirrors the TuningPanel handler above (the Global strip): every
    // slot on Global tuning follows, a slot switched to Custom keeps its own
    // values until its Tune combo goes back to G. The preset format carries
    // ONE global pair, so there is nothing per-slot to restore.
    //
    // EXPECTED LEVEL CHANGE -- this is NOT 0 dB, and the round-1 note that
    // said it was has been corrected here.
    //
    // A Detector notch carries no ceiling of its own (ModelNotch::ceilingDb is
    // NaN, so ceilingDbFor() reads the LIVE value this line writes). The
    // detection pass re-tunes any such notch standing DEEPER than the new
    // ceiling up to it on the very next tick -- NotchController.cpp, the
    // `n.depthDB < ceiling` branch -- as one ramped step of 10 ms. Loading a
    // shallower ceiling over a live -24 dB notch under Music.json's -10 dB is
    // therefore +14 dB at a bin that was ringing. Loading a DEEPER ceiling is
    // 0 dB now and only allows deeper rungs later. Preset, Manual and
    // Soundcheck notches carry their own ceiling (Q8) and are untouched either
    // way, as are the notches this load just adopted at their file depth.
    //
    // The pull itself is by design (Q1/Q8) and is not changed here; what is
    // added is that the load now SAYS it moved the ceiling, in the
    // `preset_load` event below.
    bool   ceilingApplied = false;
    double appliedQ       = 0.0;
    double appliedDepthDb = 0.0;

    if (result.preset.hasNotchDefaults)
    {
        for (int i = 0; i < kMaxSlots; ++i)
        {
            if (! slotUsesGlobalTuning_[(std::size_t) i])
                continue;

            // setNotchDefaults clamps to Q 8..50 / -24..-6 dB, so a file
            // carrying a legal-but-extreme pair cannot push the controller
            // outside the range the panels can reach.
            auto& controller = *notchControllers_[(std::size_t) i];
            controller.setNotchDefaults (result.preset.notchDefaults.Q,
                                         result.preset.notchDefaults.depthDB);

            // Read BACK, so the log carries the ceiling that is actually
            // standing rather than the number the file asked for. They differ
            // whenever the clamp above bit. Also: with every slot on Custom
            // this loop never runs, and the load really did apply nothing.
            if (! ceilingApplied)
            {
                ceilingApplied = true;
                appliedQ       = controller.getNotchQ();
                appliedDepthDb = controller.getNotchDepthDb();
            }
        }

        // The strip re-reads slot 0 through paramsProvider, so the Q and depth
        // combos show what the file just installed rather than the value the
        // operator left them on. A ceiling need not sit on a combo rung (Q13);
        // TuningPanel::refresh shows such a value as text.
        tuningPanel_.refresh();
    }

    // M-4: the mono-skip count had nowhere to go but a Logger line and a test
    // accessor. It belongs in the session log next to the notches the load
    // DID install -- a preset that silently loses half its notches on a mono
    // rig is exactly the kind of thing a show log has to be able to explain.
    // File NAME only: the full path can carry the operator's own name.
    {
        auto v = SessionLogger::makeEvent ("preset_load");
        auto* o = v.getDynamicObject();
        o->setProperty ("file", file.getFileName());
        o->setProperty ("adopted", adoptedTotal);
        o->setProperty ("skipped", lastLoadSkipped_);

        // The ceiling this load moved, if it moved one (fix round 2).
        //
        // A load that LOWERS the ceiling is NOT level-neutral: the controller
        // re-tunes every live detector notch deeper than the new ceiling up to
        // it on the next tick, which is a real level rise at a bin that was
        // ringing (a -24 dB notch under Music.json's -10 dB ceiling comes up
        // 14 dB, ramped over 10 ms). That has to be readable afterwards, so
        // the log says whether a ceiling was applied and which one.
        //
        // `ceiling_applied` is always written -- a reader must be able to tell
        // "this load moved nothing" from "the field is new" -- but q /
        // depth_db only when there is a ceiling to report. The names and the
        // double type match the `tuning` event's, so logstats reads one column
        // for both.
        o->setProperty ("ceiling_applied", ceilingApplied);

        if (ceilingApplied)
        {
            o->setProperty ("q",        appliedQ);
            o->setProperty ("depth_db", appliedDepthDb);
        }

        sessionLogger_.log (v);
    }

    slotPanel_.refresh();   // the routing table shows what the file just changed (lane S loose end)

    return true;
}

bool MainComponent::savePreset (const juce::File& file)
{
    Preset preset;
    preset.device     = engine_.getCurrentDeviceName();
    preset.bufferSize = engine_.getCurrentBufferSize();

    // The rate the notches were PUBLISHED at (SnapshotBuffer::sampleRate), taken
    // as the first non-zero one seen -- all slots share the device rate. Never
    // 0: saveToFile validates each notch's freq against this rate's Nyquist, and
    // a notch detected at rate R is below R/2 by construction. See PresetManager
    // decision [N]. If no slot ever published (idle app, no block pumped) this
    // stays 0 and saveToFile refuses the empty preset -- the honest outcome,
    // surfaced through the returned bool rather than by inventing a rate.
    double presetRate = 0.0;

    for (int s = 0; s < kMaxSlots; ++s)
    {
        NotchController::SnapshotBuffer snap {};
        notchControllers_[(std::size_t) s]->copySnapshot (snap);

        if (presetRate <= 0.0 && snap.sampleRate > 0.0)
            presetRate = snap.sampleRate;

        // ONE PresetNotch per SNAPSHOT notch -- (slot, lane, index), no dedup.
        //
        // This loop used to collapse the two lanes of a slot into one entry,
        // keeping the lowest channel and never writing "lane". That was
        // correct while lane P owned this file alone: detection was mono and
        // adoptPreset mirrored every notch onto both lanes, so the two lanes
        // held identical parameters and one of them described both.
        //
        // Spec S (docs/superpowers/specs/2026-09-05-stereo-aware-detection-
        // design.md) ended that. Every lane of a stereo slot now detects and
        // places its own notches, INDEP is the default, and LINK is per slot.
        // Under those rules a dedup silently DROPS every lane-1 notch: the
        // soundman saves a tuned rig and reopens half of it. So each snapshot
        // notch is written on its own, carrying sn.channel as `lane`.
        //
        // Uniqueness is per (slot, lane, index) in PresetManager::validate --
        // decision [S] -- so two notches sharing an index on different lanes
        // are a legal file, which is exactly what INDEP produces.
        for (std::uint32_t n = 0; n < snap.notchCount; ++n)
        {
            const auto& sn = snap.notches[n];

            PresetNotch pn;
            pn.index   = sn.index;
            pn.freq    = sn.frequency;
            pn.Q       = sn.Q;
            // Q11: the depth the room NEEDED, not the rung the release ladder
            // happens to be resting on when SAVE was pressed. A preset saved
            // during a quiet stretch would otherwise reload two rungs too
            // shallow and let the same howl come back.
            pn.depthDB = sn.deepestDb;
            pn.slot    = s;
            pn.lane    = sn.channel;
            preset.notches.push_back (pn);
        }
    }

    // The "slots" section: routing AND the per-slot LINK flag, for every slot
    // the engine reports enabled. Without it a saved preset restores notches
    // onto default stereo routing and with LINK cleared -- i.e. it loses the
    // half of the tuning that is not a notch. loadPreset() applies each
    // entry's `linked` before setWidth(), so the reloaded slot detects in the
    // mode it was saved in.
    for (int s = 0; s < kMaxSlots; ++s)
    {
        const SlotConfig config = engine_.getSlotConfig (s);

        if (! config.enabled)
            continue;

        PresetSlot entry;
        entry.index  = s;
        entry.config = config;
        entry.linked = isSlotLinked (s);
        preset.slots.push_back (entry);
    }

    preset.sampleRate = presetRate;

    // Q11: the CEILING is written here and read back by loadPreset(), which
    // applies it to every slot on Global tuning. Both halves are needed: a
    // preset whose ceiling is only WRITTEN reloads onto whatever the tuning
    // strip was left on, and one that is neither written nor read falls back
    // to PresetNotchDefaults' -12 dB (PresetManager.h), capping every detector
    // notch two rungs shallower than the show was tuned at.
    //
    // Slot 0 is the source because notchDefaults is a single global pair in
    // the format and every Global slot carries the same values. A slot on
    // Custom tuning is not represented in the file at all -- see
    // docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md 4.8 / Q11
    // if per-slot ceilings ever arrive.
    preset.notchDefaults.Q       = notchControllers_[0]->getNotchQ();
    preset.notchDefaults.depthDB = notchControllers_[0]->getNotchDepthDb();

    juce::StringArray errors;
    const bool ok = PresetManager::saveToFile (preset, file, errors);

    if (! ok)
        juce::Logger::writeToLog (
            "savePreset \"" + file.getFileName() + "\" refused: " + errors.joinIntoString ("; "));

    return ok;
}

void MainComponent::setDisplayedSlot (const int slotIndex)
{
    if (! juce::isPositiveAndBelow (slotIndex, slotPanel_.getVisibleRowCount()))
        return;

    displayedSlot_ = slotIndex;

    auto& controller = *notchControllers_[(std::size_t) slotIndex];

    // BOTH panels, together. Each drops everything it derived from the slot it
    // was on -- see their setController comments for why an age ledger cannot
    // cross a slot boundary.
    spectrumView_.setController (controller);
    notchListPanel_.setController (controller);
    notchListPanel_.setDisplayedSlot (slotIndex);

    slotTabs_.setSelected (slotIndex);

    // The overlay belongs to the slot AND lane on display, so re-point it with
    // them. Skipped while the strip is away: with no run to draw, clearing is
    // the caller's job (endSoundcheckSession) and doing it here as well would
    // wipe an overlay a later Results state is about to want.
    if (soundcheckPanel_.getMode() != gui::SoundcheckPanel::Mode::Hidden)
        refreshSoundcheckOverlay();
}

void MainComponent::refreshStatus()
{
    gui::DeviceStatus status;
    status.running           = engine_.isRunning();
    status.sampleRateHz      = engine_.getCurrentSampleRateHz();
    status.numInputChannels  = engine_.getNumInputChannels();
    status.numOutputChannels = engine_.getNumOutputChannels();
    status.latencySeconds    = engine_.getCurrentLatency();

    // A device error outranks a refused setting: the refusal describes a device
    // that is no longer running.
    juce::String message = gui::formatDeviceBanner (engine_.getLastDeviceError());

    if (message.isEmpty())
        message = panelMessage_;

    statusBar_.setStatus (status, message);

    // The masthead readout. A device error replaces the rig line entirely --
    // when the interface is gone, its sample rate is not the news.
    rigIsHealthy_ = status.running && message.isEmpty();

    // Device presence, stated separately from protection state.
    deviceIsUp_ = status.running;
    deviceLine_ = engine_.getLastDeviceError().isNotEmpty() ? "DEVICE ERROR"
                : status.running                           ? "DEVICE OK"
                                                           : "NO DEVICE";

    const double cpu = engine_.getCpuUsage();
    cpuIsHot_ = cpu >= kCpuWarnFraction;
    cpuLine_  = status.running
                    ? "CPU " + juce::String (juce::roundToInt (cpu * 100.0)) + "%"
                    : juce::String ("CPU --");

    const auto next = message.isNotEmpty() ? message
                                           : gui::formatStatusLine (status);

    if (next != rigLine_)
    {
        rigLine_ = next;
        repaint (getLocalBounds().removeFromTop (az::theme::mastheadHeight));
    }
}

void MainComponent::timerCallback()
{
    refreshStatus();

    // LANE M's only route onto the message thread. The controller's
    // onStateChanged sets a flag from its own thread; everything that touches a
    // component happens here.
    syncSoundcheckUi();

    // The badge answers "is it protecting?" in one glance (spec sections 2
    // and 5): IDLE with no device running, BYPASSED when the mode says so,
    // PROTECTING only while audio actually flows through active detection.
    if (! engine_.isRunning())
        statusBadge_.setState (gui::ProtectionState::Idle);
    else if (engine_.getMode() == AudioEngine::Mode::Bypass)
        statusBadge_.setState (gui::ProtectionState::Bypassed);
    else
        statusBadge_.setState (gui::ProtectionState::Protecting);

    // The engine's mode can change from somewhere other than these buttons --
    // it already can via setMode(), and the detector will do it when Soundcheck
    // becomes real. setDisplayedMode() reflects without requesting, so this
    // cannot fight the user.
    modeBar_.setDisplayedMode (engine_.getMode());

    // The RAIL is the one the user actually looks at, and its lit lamp is the
    // primary "what mode am I in" signal in this design -- so it has to track
    // the engine too, not just the last click it received.
    switch (engine_.getMode())
    {
        case AudioEngine::Mode::Soundcheck:
            modeRail_.setDisplayedMode (gui::ModeRail::Mode::Soundcheck);
            break;
        case AudioEngine::Mode::Auto:
            modeRail_.setDisplayedMode (gui::ModeRail::Mode::Auto);
            break;
        case AudioEngine::Mode::Bypass:
            modeRail_.setDisplayedMode (gui::ModeRail::Mode::Bypass);
            break;
    }
}

//==============================================================================
// The window's own painting: the masthead, the raised transport and floor
// bands, and the engraved grooves that separate them. Every child paints its
// own inside.

void MainComponent::paint (juce::Graphics& g)
{
    using namespace az::theme;

    g.fillAll (background);

    auto area = getLocalBounds();

    //--------------------------------------------------------------------
    // Masthead: mark, rig readout, protection badge (a child, positioned in
    // resized()).
    auto masthead = area.removeFromTop (mastheadHeight);
    g.setColour (panel);
    g.fillRect (masthead);
    drawEngravedDivider (g, masthead);

    auto mark = masthead.reduced (kEdgePad, 0);

    // The sodium bar IS the brand mark. It is also the only place on screen
    // the accent appears without meaning "a notch just fired".
    g.setColour (accent);
    g.fillRect (mark.removeFromLeft (3).withSizeKeepingCentre (3, 18));
    mark.removeFromLeft (10);

    g.setColour (text);
    g.setFont (legendFont (brandFontSize, true, trackingCaption));
    g.drawText ("HANDS-FREE", mark.removeFromLeft (kNameWidth),
                juce::Justification::centredLeft, false);
    mark.removeFromLeft (gap + spacing);

    g.setColour (dim);
    g.setFont (legendFont (columnFontSize, false, trackingCaption));
    g.drawText ("AZ SOUNDTECH", mark.removeFromLeft (kCompanyWidth),
                juce::Justification::centredLeft, false);

    // Right of the masthead, reading outward from the protection badge:
    //
    //     ... rig line ...   CPU 12%   [• DEVICE OK]   [• PROTECTING]
    //
    // The badge itself is a child and was positioned in resized(); everything
    // else here is painted, because none of it is interactive.
    auto right = masthead.reduced (kEdgePad, 0);
    right.removeFromRight (kBadgeWidth + gap);

    const auto deviceChip = right.removeFromRight (kDeviceChipWidth)
                                 .withSizeKeepingCentre (kDeviceChipWidth, kBadgeHeight);
    right.removeFromRight (gap);

    const auto deviceColour = deviceLine_ == "DEVICE ERROR" ? danger
                            : deviceIsUp_                   ? ok
                                                            : dim;

    g.setColour (well);
    g.fillRoundedRectangle (deviceChip.toFloat(), cornerRadius);
    g.setColour (border);
    g.drawRoundedRectangle (deviceChip.toFloat().reduced (0.5f), cornerRadius, 1.0f);

    constexpr float dotSize = 8.0f;
    g.setColour (deviceColour);
    g.fillEllipse ((float) deviceChip.getX() + 10.0f,
                   (float) deviceChip.getCentreY() - dotSize * 0.5f,
                   dotSize, dotSize);

    g.setFont (legendFont (columnFontSize, true, trackingColumn));
    g.drawText (deviceLine_, deviceChip.withTrimmedLeft (24),
                juce::Justification::centredLeft, false);

    // CPU: a number, so mono, and no chrome around it -- it is a reading, not
    // a state.
    const auto cpuArea = right.removeFromRight (kCpuChipWidth);
    right.removeFromRight (gap);

    g.setColour (cpuIsHot_ ? warn : dim);
    g.setFont (monoFont (readoutFontSize));
    g.drawText (cpuLine_, cpuArea, juce::Justification::centredRight, false);

    // The rig readout fills whatever is left between the mark and those.
    auto rigArea = right.withTrimmedLeft (kMarkWidth);
    if (rigArea.getWidth() > 0)
    {
        g.setColour (rigIsHealthy_ ? dim : warn);
        g.setFont (monoFont (readoutFontSize));
        g.drawText (rigLine_, rigArea, juce::Justification::centredRight, true);
    }

    //--------------------------------------------------------------------
    // Transport: a raised band, so the switches sit ON something instead of
    // floating on the canvas.
    auto transport = area.removeFromTop (transportHeight);
    g.setColour (panel);
    g.fillRect (transport);
    drawEngravedDivider (g, transport);

    //--------------------------------------------------------------------
    // Floor: the same raised band at the window's bottom, with a vertical
    // groove milled between its two columns.
    const auto floor = floorBoundsForPaint();
    if (floor.getHeight() > 2 * gap)
    {
        g.setColour (panel);
        g.fillRect (floor);

        // The groove ABOVE the floor: drawEngravedDivider draws on a band's
        // bottom edge, so it is handed a zero-height band sitting one pixel up.
        drawEngravedDivider (g, floor.withHeight (0).translated (0, -1));

        const int grooveX = floor.getX() + kEdgePad
                          + juce::roundToInt ((float) (floor.getWidth() - 2 * kEdgePad)
                                              * kNotchColumnFraction)
                          + gap;

        g.setColour (shade);
        g.fillRect (grooveX, floor.getY() + gap, 1, floor.getHeight() - 2 * gap);
        g.setColour (sheen);
        g.fillRect (grooveX + 1, floor.getY() + gap, 1, floor.getHeight() - 2 * gap);
    }
}

//==============================================================================
// Layout.
//
// The shape, and why: masthead / transport / ANALYSER / floor. The analyser is
// the only thing on screen that changes thirty times a second, so it takes
// every pixel the fixed bands do not need. The pre-rebuild layout gave it a
// middling slice between seven equal-weight full-width strips, which is most
// of why nothing on screen read as more important than anything else.

int MainComponent::naturalFloorHeight() const
{
    using namespace az::theme;

    // What the rig column WANTS: device drawer, detection strip, routing table.
    const int rigColumn = deviceDrawer_.getPreferredHeight()
                        + gap + gui::TuningPanel::kPanelHeight
                        + gap + slotPanel_.getPreferredHeight();

    return juce::jmax (kNotchListHeight, rigColumn) + 2 * gap;
}

int MainComponent::heightThatFitsTheFloor() const
{
    using namespace az::theme;

    const int natural = naturalFloorHeight();
    const int chrome  = mastheadHeight + transportHeight;

    // BOTH ceilings in floorHeightFor have to clear, or the floor is trimmed
    // anyway and the extra height goes to the analyser instead of to the row
    // the user just asked for.
    const int forAnalyser = chrome + kMinSpectrumHeight + gap + natural;
    const int forShare    = chrome + juce::roundToInt ((float) natural / kMaxFloorShare);

    // Plus slack. Without it the floor fits EXACTLY, which means one pixel of
    // rounding anywhere puts the Add row under the window edge -- and "fits
    // exactly" is indistinguishable from "clipped" to the person looking at it.
    return juce::jmax (kMinimumHeight, juce::jmax (forAnalyser, forShare)) + kFloorSlack;
}

void MainComponent::parentHierarchyChanged()
{
    // The app used to open at a height where the routing table's "+ Add slot"
    // row landed exactly on the window's bottom edge, so the first thing a
    // user had to do was drag the window taller. Sizing here, once a real
    // window exists, is what makes the opening size honest.
    growWindowToFitFloor();
}

void MainComponent::growWindowToFitFloor()
{
    // Headless (and in every test): a component with no desktop peer is its
    // own top level, and there is no window to grow.
    auto* window = getTopLevelComponent();
    if (window == nullptr || window == this)
        return;

    const int wanted = heightThatFitsTheFloor();
    if (wanted <= getHeight())
        return;

    // Never past the display the window is on. A window taller than the screen
    // puts the row the user just revealed under the taskbar, which is the
    // problem this is here to solve.
    const auto* display = juce::Desktop::getInstance().getDisplays()
                              .getDisplayForRect (window->getScreenBounds());

    const int grown = window->getHeight() + (wanted - getHeight());
    const int ceiling = display != nullptr ? display->userArea.getHeight() : grown;

    window->setSize (window->getWidth(), juce::jmin (ceiling, grown));
}

int MainComponent::floorHeightFor (const int available) const
{
    using namespace az::theme;

    const int wanted = naturalFloorHeight();

    // TWO ceilings, and the floor gets the lower of them.
    //
    // The first is the analyser's hard minimum, reserved before the floor gets
    // anything: a short window shrinks the floor -- whose routing table
    // scrolls -- rather than the analyser, which has nowhere to go.
    //
    // The second is a SHARE cap, and it is the one that matters in practice.
    // The rig column's natural height is fixed (device + detection + two slot
    // rows), so without a cap a modest window hands the floor more pixels than
    // the analyser -- which inverts the whole point of the layout. The
    // analyser is the only thing on screen that moves, and it keeps the
    // majority of the space at every window size.
    const int analyserFloor = juce::jmax (0, available - kMinSpectrumHeight - gap);
    const int shareCeiling  = juce::roundToInt ((float) available * kMaxFloorShare);

    return juce::jlimit (0, juce::jmin (analyserFloor, shareCeiling), wanted);
}

juce::Rectangle<int> MainComponent::floorBoundsForPaint() const
{
    using namespace az::theme;

    auto area = getLocalBounds();
    area.removeFromTop (mastheadHeight);
    area.removeFromTop (transportHeight);

    return area.removeFromBottom (floorHeightFor (area.getHeight()));
}

void MainComponent::resized()
{
    using namespace az::theme;

    auto area = getLocalBounds();

    //--------------------------------------------------------------------
    // 1. Masthead -- painted; the badge is its only child.
    auto masthead = area.removeFromTop (mastheadHeight).reduced (kEdgePad, 0);
    statusBadge_.setBounds (masthead.removeFromRight (kBadgeWidth)
                                    .withSizeKeepingCentre (kBadgeWidth, kBadgeHeight));

    // The selector's WIDTH is set here and its position by its host panel:
    // the chip count follows the routing table's visible rows and changes
    // while the app runs, so the width is asked for rather than assumed.
    slotTabs_.setSize (slotTabs_.getPreferredWidth(), kBadgeHeight);

    //--------------------------------------------------------------------
    // 2. Transport. The rail owns its internal grid; all this owes it is a
    //    band of the right height with the window margin applied.
    auto transport = area.removeFromTop (transportHeight);
    modeRail_.setBounds (transport.reduced (kEdgePad, gap + spacing));

    //--------------------------------------------------------------------
    // 3. The floor, carved off the bottom BEFORE the analyser is measured.
    notchListPanel_.setVisible (true);

    auto floor = area.removeFromBottom (floorHeightFor (area.getHeight()));

    //--------------------------------------------------------------------
    // 4. The analyser takes everything that is left.
    auto analyser = area.reduced (kEdgePad, 0).withTrimmedBottom (gap);

    //--------------------------------------------------------------------
    // 4b. Lane M's strip TAKES A BAND off the bottom of the analyser; it does
    //     not lie on top of it.
    //
    // Round 1 of this task did lay it over the plot, and the render showed why
    // that is wrong: the strip buried the whole frequency axis AND the
    // overlay's own marked-bin rake, which lives along the plot floor. Both
    // are exactly what an operator reads a results screen for. No test noticed
    // -- the panel was inside its own bounds and every assertion passed.
    //
    // Bounds are set whether or not the strip is visible, so a panel shown
    // between two layout passes is never drawn at stale coordinates (the bug
    // that left the routing table an empty black rect on 2026-08-24).
    //
    // THE STRIP WINS, and the analyser goes under kMinSpectrumHeight if that is
    // what it costs. Round 1 had it the other way round -- below
    // kMinSpectrumHeight the strip went back on top of the plot -- and that is
    // the wrong trade twice over: it buries the frequency axis and the
    // marked-bin rake again, AND the strip it saves room for is the thing
    // carrying "1 kenh sai dinh tuyen", a sentence the operator has to read to
    // know the run told them nothing. A short analyser is a nuisance; a fault
    // sentence nobody sees is a room that stays wrong.
    //
    // The height ASKED FOR, not kPanelHeight: the summary's line count is
    // data-dependent, and SoundcheckPanel::paint does not truncate.
    {
        const int wanted = soundcheckPanel_.preferredHeight();
        const int stripH = juce::jmin (wanted, juce::jmax (0, analyser.getHeight()));

        if (soundcheckPanel_.isVisible())
            soundcheckPanel_.setBounds (analyser.removeFromBottom (stripH));
        else
            soundcheckPanel_.setBounds (analyser.withTop (analyser.getBottom() - stripH));
    }

    spectrumView_.setBounds (analyser);

    //--------------------------------------------------------------------
    // 5. Floor columns: the notch table reads left -- it is the answer -- and
    //    the rig sits right, because it is the setup you touch once.
    auto inner = floor.reduced (kEdgePad, gap);

    const int notchWidth = juce::roundToInt ((float) inner.getWidth() * kNotchColumnFraction);
    notchListPanel_.setBounds (inner.removeFromLeft (notchWidth));
    notchListPanel_.resized();   // re-place the hosted selector at the new width
    inner.removeFromLeft (2 * gap + 2);   // the painted groove lives in here

    const int drawerHeight = juce::jmin (deviceDrawer_.getPreferredHeight(),
                                         inner.getHeight());
    deviceDrawer_.setBounds (inner.removeFromTop (drawerHeight));
    inner.removeFromTop (gap);

    if (inner.getHeight() > gui::TuningPanel::kPanelHeight)
    {
        tuningPanel_.setVisible (true);
        tuningPanel_.setBounds (inner.removeFromTop (gui::TuningPanel::kPanelHeight));
        inner.removeFromTop (gap);
    }
    else
    {
        tuningPanel_.setVisible (false);
    }

    // Visibility tracks the layout decision: a shrinking window that drops the
    // table must not leave the scroller sitting at stale bounds.
    slotScroller_.setVisible (inner.getHeight() > 0);

    if (inner.getHeight() > 0)
    {
        slotScroller_.setBounds (inner);

        // A Viewport never sizes its content by itself: without this the table
        // renders as an empty black rect (the 2026-08-24 bug). Full preferred
        // height for the CURRENT visible row count; the viewport adds a
        // scrollbar only when the column is shorter than the table.
        // At LEAST the table's own preferred width: below that the Viewport
        // scrolls sideways rather than clipping the TUNE column off the edge.
        slotPanel_.setSize (juce::jmax (slotScroller_.getMaximumVisibleWidth(),
                                        slotPanel_.getPreferredWidth()),
                            slotPanel_.getPreferredHeight());
    }
}
