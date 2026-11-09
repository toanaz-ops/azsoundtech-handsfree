// Lane L2b step 2b: the "ask the AI" row and answer block of gui::CopilotPanel,
// gui::CopilotSetupPanel, applyCopilotAskResult, and their wiring in
// MainComponent.
//
// Same conventions as test_copilotpanel.cpp: the headless suite pumps no message
// loop (buttons are driven with onClick(), never triggerClick(); layouts call
// resized() by hand), no real sidecar ever launches (fake backend + hand-drained
// poster), and nothing may touch the real %APPDATA%: the key store points at a
// temp directory and settings are enabled only with a temp file.

#include <gtest/gtest.h>

#include "app/CopilotCredentialStore.h"
#include "app/MainComponent.h"
#include "gui/CopilotPanel.h"
#include "gui/CopilotSetupPanel.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <set>

namespace
{
using State = gui::CopilotPanel::State;
using S = CopilotRunResult::Status;

// Vietnamese sentences as escaped UTF-8 bytes (no /utf-8 on MSVC).
const char* const kAsking = "\xc4\x90" "ang h\xe1\xbb\x8fi AI\xe2\x80\xa6 t\xe1\xbb\x91i \xc4\x91" "a 2 ph\xc3\xbat.";
const char* const kAnswered = "AI \xc4\x91\xc3\xa3 tr\xe1\xba\xa3 l\xe1\xbb\x9di.";
const char* const kBlockedStatus = "C\xc3\xa2u tr\xe1\xba\xa3 l\xe1\xbb\x9di b\xe1\xbb\x8b ch\xe1\xba\xb7n.";
const char* const kAskFailed = "Kh\xc3\xb4ng h\xe1\xbb\x8fi \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c.";
const char* const kNeedsSetup = "Ch\xc6\xb0" "a c\xc3\xa0i AI. B\xe1\xba\xa5m C\xc3\x80I AI.";
const char* const kCancelled = "\xc4\x90\xc3\xa3 hu\xe1\xbb\xb7 c\xc3\xa2u h\xe1\xbb\x8fi.";
const char* const kBlockedBody = "C\xc3\xa2u tr\xe1\xba\xa3 l\xe1\xbb\x9di c\xe1\xbb\xa7" "a m\xc3\xb4 h\xc3\xacnh \xc4\x91\xc3\xa3 b\xe1\xbb\x8b ch\xe1\xba\xb7n v\xc3\xac n\xc3\xb3 khuy\xc3\xaan m\xe1\xbb\x99t vi\xe1\xbb\x87" "c c\xc3\xb3 th\xe1\xbb\x83 g\xc3\xa2y h\xc3\xba. H\xc3\xa3y h\xe1\xbb\x8fi l\xe1\xba\xa1i, ho\xe1\xba\xb7" "c \xc4\x91\xe1\xbb\x95i m\xc3\xb4 h\xc3\xacnh.";
const char* const kInvalidReply = "Ti\xe1\xba\xbfn tr\xc3\xacnh ph\xe1\xbb\xa5 tr\xe1\xba\xa3 v\xe1\xbb\x81 d\xe1\xbb\xaf li\xe1\xbb\x87u kh\xc3\xb4ng \xc4\x91\xe1\xbb\x8d" "c \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c.";
const char* const kBadSetup = "C\xc3\xa0i \xc4\x91\xe1\xba\xb7t AI ch\xc6\xb0" "a h\xe1\xbb\xa3p l\xe1\xbb\x87. B\xe1\xba\xa5m C\xc3\x80I AI.";
const char* const kAskButton = "H\xe1\xbb\x8eI";
const char* const kCancelButton = "HU\xe1\xbb\xb6";
const char* const kSetupButton = "C\xc3\x80I AI";
const char* const kConsent = "T\xc3\xb4i \xc4\x91\xe1\xbb\x93ng \xc3\xbd: c\xc3\xa2u h\xe1\xbb\x8fi v\xc3\xa0 d\xe1\xbb\xaf li\xe1\xbb\x87u log bu\xe1\xbb\x95i di\xe1\xbb\x85n (kh\xc3\xb4ng c\xc3\xb3 \xc3\xa2m thanh) s\xe1\xba\xbd g\xe1\xbb\xadi t\xe1\xbb\x9bi nh\xc3\xa0 cung c\xe1\xba\xa5p t\xc3\xb4i ch\xe1\xbb\x8dn.";
const char* const kKeySaved = "\xc4\x90\xc3\xa3 l\xc6\xb0u kho\xc3\xa1 cho nh\xc3\xa0 cung c\xe1\xba\xa5p n\xc3\xa0y.";
const char* const kKeyMissing = "Ch\xc6\xb0" "a c\xc3\xb3 kho\xc3\xa1.";
const char* const kKeyUnsupported = "C\xe1\xba\xa5t kho\xc3\xa1 ch\xe1\xbb\x89 c\xc3\xb3 tr\xc3\xaan Windows.";
const char* const kFooterDot = "claude-opus-5-5 \xc2\xb7 2140 in / 318 out";
const char* const kErrReason = "Sai kho\xc3\xa1 API ho\xe1\xba\xb7" "c h\xe1\xba\xbft h\xe1\xba\xa1n m\xe1\xbb\xa9" "c.";

juce::String vi (const char* s) { return juce::String::fromUTF8 (s); }

struct PostQueue
{
    std::mutex m;
    std::deque<std::function<void()>> q;

    void post (std::function<void()> fn)
    {
        std::lock_guard<std::mutex> l (m);
        q.push_back (std::move (fn));
    }

    bool waitAndDrain (int timeoutMs = 5000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::deque<std::function<void()>> local;
            {
                std::lock_guard<std::mutex> l (m);
                local.swap (q);
            }
            if (! local.empty())
            {
                for (auto& f : local)
                    f();
                return true;
            }
            juce::Thread::sleep (2);
        }
        return false;
    }
};

CopilotRunResult result (S s, int code, const char* out = "")
{
    CopilotRunResult r;
    r.status = s;
    r.exitCode = code;
    r.stdoutText = juce::String::fromUTF8 (out);
    return r;
}

class FakeBackend : public CopilotBackend
{
public:
    explicit FakeBackend (CopilotRunResult r) : result_ (std::move (r)) {}

    CopilotRunResult run (const CopilotRequest& req, CopilotCancelToken&) override
    {
        std::lock_guard<std::mutex> l (m_);
        lastRequest_ = req;
        ++calls_;
        return result_;
    }

    int calls() { std::lock_guard<std::mutex> l (m_); return calls_; }
    CopilotRequest lastRequest() { std::lock_guard<std::mutex> l (m_); return lastRequest_; }

private:
    std::mutex m_;
    CopilotRunResult result_;
    CopilotRequest lastRequest_;
    int calls_ = 0;
};

// Blocks until cancelled (bounded), then reports whether it saw the cancel.
class WaitingBackend : public CopilotBackend
{
public:
    CopilotRunResult run (const CopilotRequest&, CopilotCancelToken& cancel) override
    {
        entered = true;
        cancel.waitFor (10000);
        sawCancel = cancel.isCancelled();
        return result (S::failed, 3, R"({"error":"x"})");
    }

    std::atomic<bool> entered { false };
    std::atomic<bool> sawCancel { false };
};

// Ignores the cancel token until released: models a run that is still
// unwinding right after HUY.
class StubbornBackend : public CopilotBackend
{
public:
    CopilotRunResult run (const CopilotRequest&, CopilotCancelToken&) override
    {
        ++calls;
        entered = true;
        for (int i = 0; i < 2000 && ! release.load(); ++i)
            juce::Thread::sleep (5);
        return result (S::failed, 3, R"({"error":"x"})");
    }

    std::atomic<int> calls { 0 };
    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
};

const char* const kBusyText = "\xc4\x90" "ang b\xe1\xba\xadn, th\xe1\xbb\xad l\xe1\xba\xa1i sau m\xe1\xbb\x99t gi\xc3\xa2y.";

juce::File freshDir (const char* tag)
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                 .getChildFile ("handsfree-ask-" + juce::String (tag) + "-"
                                + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    d.createDirectory();
    return d;
}

const char* kAnswerJson =
    R"({"answer":"Two notches share 1008 Hz.","provider":"anthropic","model":"claude-opus-5-5",)"
    R"("usage":{"input_tokens":2140,"output_tokens":318},"findings":[],"blocked":false})";

CopilotAnswer makeAnswer (const juce::String& text, const juce::String& model = "claude-opus-5-5",
                          int in = 2140, int out = 318)
{
    CopilotAnswer a;
    a.kind = CopilotAnswer::Kind::answer;
    a.text = text;
    a.model = model;
    a.inputTokens = in;
    a.outputTokens = out;
    return a;
}

void layout (MainComponent& app, int w, int h)
{
    app.setSize (w, h);
    app.resized();
}
} // namespace

//==============================================================================
// CopilotPanel: the new states
TEST (CopilotAskPanel, EveryNewStateShowsItsSentenceAndTheRightControlState)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;

    struct Case { State s; const char* text; };
    const Case cases[] = {
        { State::asking, kAsking }, { State::answer, kAnswered }, { State::blocked, kBlockedStatus },
        { State::askFailed, kAskFailed }, { State::needsSetup, kNeedsSetup }, { State::cancelled, kCancelled }
    };

    std::set<juce::String> seen;
    for (const auto& c : cases)
    {
        panel.setState (c.s);
        EXPECT_EQ (panel.state(), c.s);
        EXPECT_EQ (panel.getStatusTextForTest(), vi (c.text));
        EXPECT_FALSE (panel.getStatusTextForTest().containsChar ((juce::juce_wchar) 0x00c3));   // mojibake guard
        seen.insert (panel.getStatusTextForTest());

        const bool asking = c.s == State::asking;
        EXPECT_EQ (panel.getAskButtonForTest().getButtonText(), vi (asking ? kCancelButton : kAskButton));
        EXPECT_EQ (panel.getAskEditorForTest().isEnabled(), ! asking);
        EXPECT_EQ (panel.getAnalyseButtonForTest().isEnabled(), ! asking);
        EXPECT_TRUE (panel.getAskButtonForTest().isEnabled());
    }
    EXPECT_EQ (seen.size(), (size_t) 6);

    // While the findings run, asking is off.
    panel.setState (State::running);
    EXPECT_FALSE (panel.getAskEditorForTest().isEnabled());
    EXPECT_FALSE (panel.getAskButtonForTest().isEnabled());

    EXPECT_EQ (panel.getSetupButtonForTest().getButtonText(), vi (kSetupButton));
}

TEST (CopilotAskPanel, AnswerBlockIsMeasuredGrowsWithTextAndShowsTheFooter)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    panel.setSize (360, 240);
    panel.resized();

    panel.setAnswer (makeAnswer ("Short."));
    EXPECT_EQ (panel.state(), State::answer);
    EXPECT_EQ (panel.getBlockTextForTest(), "Short.");
    EXPECT_EQ (panel.getFooterTextForTest(), vi (kFooterDot));
    EXPECT_EQ (panel.getRowCountForTest(), 0);
    const int shortH = panel.getContentHeightForTest();
    EXPECT_GT (shortH, 0);

    juce::String longText;
    for (int i = 0; i < 40; ++i)
        longText << "Sentence number " << i << " is long enough to wrap onto more lines. ";
    panel.setAnswer (makeAnswer (longText));
    const int longH = panel.getContentHeightForTest();
    EXPECT_GT (longH, shortH + 100);
    EXPECT_GT (longH, panel.getListBoundsForTest().getHeight());   // scrolls in the same viewport

    panel.setSize (240, 240);
    panel.resized();
    EXPECT_GT (panel.getContentHeightForTest(), longH);   // narrower -> more lines, never clipped

    // Footer variants.
    panel.setAnswer (makeAnswer ("x", "m1", -1, -1));
    EXPECT_EQ (panel.getFooterTextForTest(), "m1");
    panel.setAnswer (makeAnswer ("x", "", 5, -1));
    EXPECT_EQ (panel.getFooterTextForTest(), "5 in / ? out");

    // Leaving the answer drops the block.
    panel.setState (State::idle);
    EXPECT_TRUE (panel.getBlockTextForTest().isEmpty());
    EXPECT_EQ (panel.getContentHeightForTest(), 0);
}

TEST (CopilotAskPanel, SetAnswerMapsEveryKind)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    panel.setSize (360, 240);
    panel.resized();

    CopilotAnswer blocked;
    blocked.kind = CopilotAnswer::Kind::blocked;
    blocked.text = "provider text that must never be shown";
    panel.setAnswer (blocked);
    EXPECT_EQ (panel.state(), State::blocked);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kBlockedBody));   // the fixed sentence, not the provider's

    CopilotAnswer error;
    error.kind = CopilotAnswer::Kind::error;
    error.text = vi (kErrReason);
    panel.setAnswer (error);
    EXPECT_EQ (panel.state(), State::askFailed);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kErrReason));

    CopilotAnswer invalid;   // default kind
    panel.setAnswer (invalid);
    EXPECT_EQ (panel.state(), State::askFailed);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kInvalidReply));

    panel.setState (State::blocked);   // the bare state shows the same fixed sentence
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kBlockedBody));
}

TEST (CopilotAskPanel, AskRowSubmitsEmptyIsIgnoredAndCancelWhileAsking)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    std::vector<juce::String> asked;
    int cancels = 0;
    panel.onAsk = [&] (juce::String q) { asked.push_back (std::move (q)); };
    panel.onCancelAsk = [&] { ++cancels; };

    auto& editor = panel.getAskEditorForTest();
    auto& button = panel.getAskButtonForTest();

    button.onClick();                      // empty
    editor.setText ("   \t ", false);
    button.onClick();                      // whitespace only
    editor.onReturnKey();
    EXPECT_TRUE (asked.empty());

    editor.setText ("  why does 1008 Hz ring?  ", false);
    button.onClick();
    ASSERT_EQ (asked.size(), (size_t) 1);
    EXPECT_EQ (asked[0], "why does 1008 Hz ring?");   // trimmed

    editor.onReturnKey();                  // Return key submits too
    EXPECT_EQ (asked.size(), (size_t) 2);

    panel.setState (State::asking);
    EXPECT_FALSE (editor.isEnabled());
    editor.onReturnKey();                  // disabled editor: ignored
    EXPECT_EQ (asked.size(), (size_t) 2);
    button.onClick();                      // HUY
    EXPECT_EQ (cancels, 1);
    EXPECT_EQ (asked.size(), (size_t) 2);

    panel.setState (State::running);       // findings in flight: the ask button does nothing
    button.onClick();
    EXPECT_EQ (asked.size(), (size_t) 2);
}

TEST (CopilotAskPanel, SetupButtonCallsOnOpenSetupAndLayoutKeepsRowsApart)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    int opened = 0;
    panel.onOpenSetup = [&] { ++opened; };
    panel.getSetupButtonForTest().onClick();
    EXPECT_EQ (opened, 1);

    EXPECT_EQ (gui::CopilotPanel::kMinHeight, gui::CopilotPanel::kCaptionHeight + gui::CopilotPanel::kStatusHeight
                                                  + gui::CopilotPanel::kMinListHeight + gui::CopilotPanel::kAskHeight);

    panel.setSize (360, gui::CopilotPanel::kMinHeight);
    panel.resized();
    const auto list = panel.getListBoundsForTest();
    const auto ask = panel.getAskRowBoundsForTest();
    EXPECT_GE (list.getHeight(), gui::CopilotPanel::kMinListHeight);
    EXPECT_GE (ask.getY(), list.getBottom());
    EXPECT_TRUE (panel.getLocalBounds().contains (ask));

    EXPECT_FALSE (panel.getSetupButtonForTest().getBounds().intersects (panel.getAnalyseButtonForTest().getBounds()));
    EXPECT_TRUE (panel.getLocalBounds().contains (panel.getSetupButtonForTest().getBounds()));
    EXPECT_TRUE (ask.contains (panel.getAskEditorForTest().getBounds()));
    EXPECT_TRUE (ask.contains (panel.getAskButtonForTest().getBounds()));
    EXPECT_FALSE (panel.getAskEditorForTest().getBounds().intersects (panel.getAskButtonForTest().getBounds()));
}

//==============================================================================
TEST (CopilotAskResult, MapsEveryOutcome)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;

    gui::applyCopilotAskResult (panel, result (S::ok, 0, kAnswerJson));
    EXPECT_EQ (panel.state(), State::answer);
    EXPECT_EQ (panel.getBlockTextForTest(), "Two notches share 1008 Hz.");
    EXPECT_EQ (panel.getFooterTextForTest(), vi (kFooterDot));

    gui::applyCopilotAskResult (panel, result (S::failed, 3, R"({"error":"bad key"})"));
    EXPECT_EQ (panel.state(), State::askFailed);
    EXPECT_EQ (panel.getBlockTextForTest(), "bad key");

    gui::applyCopilotAskResult (panel, result (S::failed, 4, R"({"blocked":true,"message":"m"})"));
    EXPECT_EQ (panel.state(), State::blocked);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kBlockedBody));

    gui::applyCopilotAskResult (panel, result (S::failed, 5));
    EXPECT_EQ (panel.state(), State::askFailed);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kBadSetup));

    gui::applyCopilotAskResult (panel, result (S::failed, 2));
    EXPECT_EQ (panel.state(), State::noLog);

    gui::applyCopilotAskResult (panel, result (S::sidecarMissing, -1));
    EXPECT_EQ (panel.state(), State::sidecarMissing);

    gui::applyCopilotAskResult (panel, result (S::timedOut, 0));
    EXPECT_EQ (panel.state(), State::timedOut);

    gui::applyCopilotAskResult (panel, result (S::ok, 0, "not json at all"));
    EXPECT_EQ (panel.state(), State::askFailed);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kInvalidReply));

    gui::applyCopilotAskResult (panel, result (S::failed, 1, ""));   // no output at all
    EXPECT_EQ (panel.state(), State::askFailed);
}

TEST (CopilotAskResult, ExitCodeDecidesBeforeStdout)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    const char* const answerJson = R"({"answer":"x","blocked":false,"provider":"p","model":"m","findings":[]})";

    // Exit 4 is ALWAYS the blocked state with the fixed body, never an answer.
    gui::applyCopilotAskResult (panel, result (S::failed, 4, answerJson));
    EXPECT_EQ (panel.state(), State::blocked);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kBlockedBody));
    EXPECT_NE (panel.getBlockTextForTest(), "x");
    gui::applyCopilotAskResult (panel, result (S::failed, 4, ""));
    EXPECT_EQ (panel.state(), State::blocked);
    gui::applyCopilotAskResult (panel, result (S::failed, 4, "garbage"));
    EXPECT_EQ (panel.state(), State::blocked);

    // Exit 3 with an answer object: generic failure, the answer is not shown.
    gui::applyCopilotAskResult (panel, result (S::failed, 3, answerJson));
    EXPECT_EQ (panel.state(), State::askFailed);
    EXPECT_NE (panel.getBlockTextForTest(), "x");
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kAskFailed));
    gui::applyCopilotAskResult (panel, result (S::failed, 3, "not json"));
    EXPECT_EQ (panel.state(), State::askFailed);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kAskFailed));
    gui::applyCopilotAskResult (panel, result (S::failed, 3, R"({"error":"bad key"})"));
    EXPECT_EQ (panel.getBlockTextForTest(), "bad key");

    // Exit 0: only an answer is an answer; a blocked object is blocked; the rest is invalid data.
    gui::applyCopilotAskResult (panel, result (S::ok, 0, R"({"blocked":true,"message":"m"})"));
    EXPECT_EQ (panel.state(), State::blocked);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kBlockedBody));
    gui::applyCopilotAskResult (panel, result (S::ok, 0, R"({"error":"e"})"));
    EXPECT_EQ (panel.state(), State::askFailed);
    EXPECT_EQ (panel.getBlockTextForTest(), vi (kInvalidReply));
    gui::applyCopilotAskResult (panel, result (S::ok, 0, answerJson));
    EXPECT_EQ (panel.state(), State::answer);
    EXPECT_EQ (panel.getBlockTextForTest(), "x");
}

TEST (CopilotAskPanel, BusyStateShowsItsOneLine)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    panel.setState (State::busy);
    EXPECT_EQ (panel.getStatusTextForTest(), vi (kBusyText));
    EXPECT_TRUE (panel.getAskButtonForTest().isEnabled());
    EXPECT_TRUE (panel.getAnalyseButtonForTest().isEnabled());
}

//==============================================================================
// CopilotSetupPanel
TEST (CopilotSetup, ValuesRoundTripWithoutFiringCallbacks)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotSetupPanel p;
    int fired = 0;
    p.onValuesChanged = [&] (gui::CopilotSetupValues) { ++fired; };
    p.onProviderChanged = [&] (juce::String) { ++fired; };

    gui::CopilotSetupValues v;
    v.provider = "openai_compatible";
    v.model = "llama3";
    v.baseUrl = "http://localhost:11434/v1";
    v.consent = true;
    p.setValues (v);

    const auto got = p.currentValues();
    EXPECT_EQ (got.provider, v.provider);
    EXPECT_EQ (got.model, v.model);
    EXPECT_EQ (got.baseUrl, v.baseUrl);
    EXPECT_TRUE (got.consent);
    EXPECT_TRUE (p.getUrlEditorForTest().isEnabled());
    EXPECT_EQ (fired, 0);

    v.provider = "anthropic";
    v.model = "";           // setValues is literal: nothing is prefilled
    v.consent = false;
    p.setValues (v);
    EXPECT_FALSE (p.getUrlEditorForTest().isEnabled());   // the address is for openai_compatible only
    EXPECT_TRUE (p.currentValues().model.isEmpty());
    EXPECT_FALSE (p.currentValues().consent);
    EXPECT_EQ (fired, 0);
}

TEST (CopilotSetup, DisabledAddressFieldExplainsItself)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotSetupPanel p;
    gui::CopilotSetupValues v;
    v.provider = "anthropic";
    p.setValues (v);
    EXPECT_FALSE (p.getUrlEditorForTest().isEnabled());
    EXPECT_EQ (p.getUrlEditorForTest().getTextToShowWhenEmpty(),
               vi ("Ch\xe1\xbb\x89 d\xc3\xb9ng cho T\xc6\xb0\xc6\xa1ng th\xc3\xad" "ch OpenAI"));

    v.provider = "openai_compatible";
    p.setValues (v);
    EXPECT_TRUE (p.getUrlEditorForTest().isEnabled());
    EXPECT_TRUE (p.getUrlEditorForTest().getTextToShowWhenEmpty().isEmpty());
}

TEST (CopilotSetup, InvalidModelOrAddressIsExplainedAndNotEmitted)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotSetupPanel p;
    std::vector<gui::CopilotSetupValues> emitted;
    p.onValuesChanged = [&] (gui::CopilotSetupValues v) { emitted.push_back (std::move (v)); };

    gui::CopilotSetupValues v;
    v.provider = "openai_compatible";
    v.model = "bad model!";                 // space and ! are not allowed
    v.baseUrl = "ftp://example.com";
    p.setValues (v);
    EXPECT_TRUE (p.getModelReasonForTest().isNotEmpty());
    EXPECT_TRUE (p.getUrlReasonForTest().isNotEmpty());

    p.getModelEditorForTest().onReturnKey();
    EXPECT_TRUE (emitted.empty());

    p.getModelEditorForTest().setText ("llama3", false);
    p.getModelEditorForTest().onReturnKey();
    EXPECT_TRUE (emitted.empty());          // the address is still wrong
    EXPECT_TRUE (p.getModelReasonForTest().isEmpty());
    EXPECT_TRUE (p.getUrlReasonForTest().isNotEmpty());

    p.getUrlEditorForTest().setText ("http://localhost:11434/v1", false);
    p.getUrlEditorForTest().onFocusLost();
    ASSERT_EQ (emitted.size(), (size_t) 1);
    EXPECT_EQ (emitted[0].provider, "openai_compatible");
    EXPECT_EQ (emitted[0].model, "llama3");
    EXPECT_EQ (emitted[0].baseUrl, "http://localhost:11434/v1");
    EXPECT_TRUE (p.getUrlReasonForTest().isEmpty());

    // For Claude the address is irrelevant: a wrong one neither blocks nor is saved.
    p.getUrlEditorForTest().setText ("nonsense", false);
    p.getProviderBoxForTest().setSelectedId (1, juce::sendNotificationSync);
    ASSERT_EQ (emitted.size(), (size_t) 2);
    EXPECT_EQ (emitted[1].provider, "anthropic");
    EXPECT_TRUE (emitted[1].baseUrl.isEmpty());
}

TEST (CopilotSetup, SwitchingToClaudeFillsTheDefaultModelOnlyWhenNeeded)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotSetupPanel p;
    std::vector<juce::String> providers;
    p.onProviderChanged = [&] (juce::String s) { providers.push_back (std::move (s)); };

    gui::CopilotSetupValues v;
    v.provider = "openai_compatible";
    v.model = "";
    p.setValues (v);

    p.getProviderBoxForTest().setSelectedId (1, juce::sendNotificationSync);   // -> anthropic
    EXPECT_EQ (p.currentValues().model, "claude-opus-5-5");
    EXPECT_FALSE (p.getUrlEditorForTest().isEnabled());

    p.getProviderBoxForTest().setSelectedId (2, juce::sendNotificationSync);   // -> openai_compatible
    EXPECT_EQ (p.currentValues().model, "claude-opus-5-5");                // nothing prefilled, nothing erased
    EXPECT_TRUE (p.getUrlEditorForTest().isEnabled());

    p.getModelEditorForTest().setText ("gpt-4o-mini", false);
    p.getProviderBoxForTest().setSelectedId (1, juce::sendNotificationSync);   // a valid model is kept
    EXPECT_EQ (p.currentValues().model, "gpt-4o-mini");

    p.getModelEditorForTest().setText ("not valid", false);
    p.getProviderBoxForTest().setSelectedId (2, juce::sendNotificationSync);
    p.getProviderBoxForTest().setSelectedId (1, juce::sendNotificationSync);   // invalid -> default again
    EXPECT_EQ (p.currentValues().model, "claude-opus-5-5");

    ASSERT_GE (providers.size(), (size_t) 5);
    EXPECT_EQ (providers[0], "anthropic");
    EXPECT_EQ (providers[1], "openai_compatible");
}

TEST (CopilotSetup, KeyEditorIsEmptyAfterSaveWhateverHappensNext)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotSetupPanel p;
    std::vector<std::pair<juce::String, juce::String>> stored;
    p.onStoreKey = [&] (juce::String provider, juce::String key) { stored.emplace_back (provider, key); };

    auto& editor = p.getKeyEditorForTest();
    EXPECT_NE (editor.getPasswordCharacter(), (juce::juce_wchar) 0);   // a password field

    editor.setText ("sk-test-NOT-A-REAL-KEY", false);
    p.getStoreKeyButtonForTest().onClick();
    EXPECT_TRUE (editor.getText().isEmpty());
    ASSERT_EQ (stored.size(), (size_t) 1);
    EXPECT_EQ (stored[0].first, "anthropic");
    EXPECT_EQ (stored[0].second, "sk-test-NOT-A-REAL-KEY");
    EXPECT_FALSE (p.getKeyStatusTextForTest().contains ("sk-test"));

    // No handler at all: the editor is still cleared.
    p.onStoreKey = nullptr;
    editor.setText ("sk-test-second", false);
    p.getStoreKeyButtonForTest().onClick();
    EXPECT_TRUE (editor.getText().isEmpty());

    // An empty editor stores nothing.
    p.onStoreKey = [&] (juce::String, juce::String) { FAIL() << "empty key must not be stored"; };
    p.getStoreKeyButtonForTest().onClick();
}

TEST (CopilotSetup, DeleteKeyStatusLinesAndUnsupportedPlatform)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotSetupPanel p;

    juce::String deletedFor;
    p.onDeleteKey = [&] (juce::String provider) { deletedFor = provider; };
    p.getProviderBoxForTest().setSelectedId (2, juce::dontSendNotification);
    p.getDeleteKeyButtonForTest().onClick();
    EXPECT_EQ (deletedFor, "openai_compatible");

    p.setKeyStatus (true);
    EXPECT_EQ (p.getKeyStatusTextForTest(), vi (kKeySaved));
    p.setKeyStatus (false);
    EXPECT_EQ (p.getKeyStatusTextForTest(), vi (kKeyMissing));
    p.setKeyStatus (false, "custom problem");
    EXPECT_EQ (p.getKeyStatusTextForTest(), "custom problem");

    p.setKeyStoreSupported (false);
    EXPECT_FALSE (p.getKeyEditorForTest().isEnabled());
    EXPECT_FALSE (p.getStoreKeyButtonForTest().isEnabled());
    EXPECT_FALSE (p.getDeleteKeyButtonForTest().isEnabled());
    EXPECT_EQ (p.getKeyStatusTextForTest(), vi (kKeyUnsupported));

    p.setKeyStoreSupported (true);
    EXPECT_TRUE (p.getKeyEditorForTest().isEnabled());
}

TEST (CopilotSetup, ConsentToggleCarriesTheExactSentenceAndIsEmitted)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotSetupPanel p;
    std::vector<gui::CopilotSetupValues> emitted;
    p.onValuesChanged = [&] (gui::CopilotSetupValues v) { emitted.push_back (std::move (v)); };

    gui::CopilotSetupValues v;
    v.model = "claude-opus-5-5";
    p.setValues (v);

    auto& toggle = p.getConsentToggleForTest();
    EXPECT_EQ (toggle.getButtonText(), vi (kConsent));
    EXPECT_FALSE (toggle.getToggleState());

    toggle.setToggleState (true, juce::dontSendNotification);
    toggle.onClick();
    ASSERT_EQ (emitted.size(), (size_t) 1);
    EXPECT_TRUE (emitted[0].consent);

    toggle.setToggleState (false, juce::dontSendNotification);
    toggle.onClick();
    ASSERT_EQ (emitted.size(), (size_t) 2);
    EXPECT_FALSE (emitted[1].consent);

    p.getCloseButtonForTest().onClick();   // no handler: harmless
    bool closed = false;
    p.onClose = [&] { closed = true; };
    p.getCloseButtonForTest().onClick();
    EXPECT_TRUE (closed);
}

TEST (CopilotSetup, PreferredHeightIsPositiveAndNeverShrinksWhenNarrower)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotSetupPanel p;
    const int wide = p.preferredHeightForWidth (620);
    const int narrow = p.preferredHeightForWidth (360);
    EXPECT_GT (wide, 200);
    EXPECT_GE (narrow, wide);

    p.setSize (420, p.preferredHeightForWidth (420));
    p.resized();
    for (juce::Component* c : { (juce::Component*) &p.getProviderBoxForTest(), (juce::Component*) &p.getModelEditorForTest(),
                                (juce::Component*) &p.getUrlEditorForTest(), (juce::Component*) &p.getKeyEditorForTest(),
                                (juce::Component*) &p.getStoreKeyButtonForTest(), (juce::Component*) &p.getDeleteKeyButtonForTest(),
                                (juce::Component*) &p.getCloseButtonForTest(), (juce::Component*) &p.getConsentToggleForTest() })
        EXPECT_TRUE (p.getLocalBounds().contains (c->getBounds()));
}

//==============================================================================
// MainComponent wiring
namespace
{
gui::CopilotSetupValues openAiValues (bool consent = true)
{
    gui::CopilotSetupValues v;
    v.provider = "openai_compatible";
    v.model = "llama3";
    v.baseUrl = "http://localhost:11434/v1";
    v.consent = consent;
    return v;
}
} // namespace

TEST (CopilotAskWiring, NoLogAndNoConsentAndNoKeyAllStopBeforeAnyProcess)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto logDir = freshDir ("log");
    const auto credDir = freshDir ("cred");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        PostQueue queue;
        auto backend = std::make_unique<FakeBackend> (result (S::ok, 0, kAnswerJson));
        auto* fake = backend.get();
        app.setCopilotBackendForTest (std::move (backend), [&] (std::function<void()> fn) { queue.post (std::move (fn)); });
        auto& panel = app.getCopilotPanelForTest();

        panel.onAsk ("q");
        EXPECT_EQ (panel.state(), State::noLog);   // no session log yet

        ASSERT_TRUE (app.startSessionLog (logDir));

        panel.onAsk ("q");
        EXPECT_EQ (panel.state(), State::needsSetup);   // no consent (default settings)

        // Consent given, Claude, but no stored key.
        gui::CopilotSetupValues v;
        v.provider = "anthropic";
        v.model = "claude-opus-5-5";
        v.consent = true;
        app.getCopilotSetupPanelForTest().onValuesChanged (v);
        panel.setState (State::idle);
        panel.onAsk ("q");
        EXPECT_EQ (panel.state(), State::needsSetup);

        // OpenAI-compatible with a bad address.
        auto bad = openAiValues();
        bad.baseUrl = "";
        app.getCopilotSetupPanelForTest().onValuesChanged (bad);   // wiring stores what the panel emits
        panel.setState (State::idle);
        panel.onAsk ("q");
        EXPECT_EQ (panel.state(), State::needsSetup);

        EXPECT_EQ (fake->calls(), 0);
        app.stopSessionLog();
    }
    logDir.deleteRecursively();
    credDir.deleteRecursively();
}

TEST (CopilotAskWiring, ClaudeWithAStoredKeyReachesTheBackend)
{
    if (! CopilotCredentialStore::isSupported())
        GTEST_SKIP() << "the key store exists only on Windows";

    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto logDir = freshDir ("log2");
    const auto credDir = freshDir ("cred2");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        PostQueue queue;
        auto backend = std::make_unique<FakeBackend> (result (S::ok, 0, kAnswerJson));
        auto* fake = backend.get();
        app.setCopilotBackendForTest (std::move (backend), [&] (std::function<void()> fn) { queue.post (std::move (fn)); });
        ASSERT_TRUE (app.startSessionLog (logDir));

        // Store through the overlay's own callback (this is the wiring under test).
        app.getCopilotSetupPanelForTest().onStoreKey ("anthropic", "sk-test-NOT-A-REAL-KEY");
        EXPECT_EQ (app.getCopilotSetupPanelForTest().getKeyStatusTextForTest(), vi (kKeySaved));
        EXPECT_TRUE (app.copilotCredentialStoreForTest().hasKey ("anthropic"));

        gui::CopilotSetupValues v;
        v.model = "claude-opus-5-5";
        v.consent = true;
        app.getCopilotSetupPanelForTest().onValuesChanged (v);

        auto& panel = app.getCopilotPanelForTest();
        panel.onAsk ("q");
        EXPECT_EQ (panel.state(), State::asking);
        ASSERT_TRUE (queue.waitAndDrain());
        EXPECT_EQ (panel.state(), State::answer);
        EXPECT_EQ (fake->lastRequest().provider, "anthropic");
        EXPECT_TRUE (fake->lastRequest().baseUrl.isEmpty());

        app.getCopilotSetupPanelForTest().onDeleteKey ("anthropic");
        EXPECT_EQ (app.getCopilotSetupPanelForTest().getKeyStatusTextForTest(), vi (kKeyMissing));
        app.stopSessionLog();
    }
    logDir.deleteRecursively();
    credDir.deleteRecursively();
}

TEST (CopilotAskWiring, AFullAskBuildsTheRequestAndShowsTheAnswer)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto logDir = freshDir ("log3");
    const auto credDir = freshDir ("cred3");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        PostQueue queue;
        auto backend = std::make_unique<FakeBackend> (result (S::ok, 0, kAnswerJson));
        auto* fake = backend.get();
        app.setCopilotBackendForTest (std::move (backend), [&] (std::function<void()> fn) { queue.post (std::move (fn)); });
        ASSERT_TRUE (app.startSessionLog (logDir));
        app.getCopilotSetupPanelForTest().onValuesChanged (openAiValues());

        auto& panel = app.getCopilotPanelForTest();
        panel.getAskEditorForTest().setText ("why 1008 Hz?", false);
        panel.getAskButtonForTest().onClick();
        EXPECT_EQ (panel.state(), State::asking);
        EXPECT_EQ (panel.getAskButtonForTest().getButtonText(), vi (kCancelButton));

        ASSERT_TRUE (queue.waitAndDrain());
        EXPECT_EQ (panel.state(), State::answer);
        EXPECT_EQ (panel.getBlockTextForTest(), "Two notches share 1008 Hz.");
        EXPECT_EQ (panel.getFooterTextForTest(), vi (kFooterDot));
        EXPECT_EQ (panel.getAskButtonForTest().getButtonText(), vi (kAskButton));

        EXPECT_EQ (fake->calls(), 1);
        const auto req = fake->lastRequest();
        EXPECT_EQ (req.question, "why 1008 Hz?");
        EXPECT_EQ (req.provider, "openai_compatible");
        EXPECT_EQ (req.model, "llama3");
        EXPECT_EQ (req.baseUrl, "http://localhost:11434/v1");
        EXPECT_EQ (req.timeoutMs, 120000);
        EXPECT_EQ (req.sessionLog, app.currentSessionLogFile());

        // The runner is free again: findings still work on the same runner.
        app.stopSessionLog();
    }
    logDir.deleteRecursively();
    credDir.deleteRecursively();
}

TEST (CopilotAskWiring, ABlockedAnswerShowsTheFixedSentence)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto logDir = freshDir ("log4");
    const auto credDir = freshDir ("cred4");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        PostQueue queue;
        app.setCopilotBackendForTest (std::make_unique<FakeBackend> (result (S::failed, 4, R"({"blocked":true,"message":"m"})")),
                                      [&] (std::function<void()> fn) { queue.post (std::move (fn)); });
        ASSERT_TRUE (app.startSessionLog (logDir));
        app.getCopilotSetupPanelForTest().onValuesChanged (openAiValues());

        auto& panel = app.getCopilotPanelForTest();
        panel.onAsk ("q");
        ASSERT_TRUE (queue.waitAndDrain());
        EXPECT_EQ (panel.state(), State::blocked);
        EXPECT_EQ (panel.getBlockTextForTest(), vi (kBlockedBody));
        app.stopSessionLog();
    }
    logDir.deleteRecursively();
    credDir.deleteRecursively();
}

TEST (CopilotAskWiring, CancelEndsTheRunAndTheStaleResultIsDropped)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto logDir = freshDir ("log5");
    const auto credDir = freshDir ("cred5");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        PostQueue queue;
        auto backend = std::make_unique<WaitingBackend>();
        auto* waiting = backend.get();
        app.setCopilotBackendForTest (std::move (backend), [&] (std::function<void()> fn) { queue.post (std::move (fn)); });
        ASSERT_TRUE (app.startSessionLog (logDir));
        app.getCopilotSetupPanelForTest().onValuesChanged (openAiValues());

        auto& panel = app.getCopilotPanelForTest();
        panel.onAsk ("q");
        ASSERT_EQ (panel.state(), State::asking);

        // While the run is in flight both entry points are ignored.
        for (int i = 0; i < 400 && ! waiting->entered.load(); ++i)
            juce::Thread::sleep (5);
        ASSERT_TRUE (waiting->entered.load());
        panel.onAsk ("again");
        panel.onAnalyse();
        EXPECT_EQ (panel.state(), State::asking);

        panel.getAskButtonForTest().onClick();   // HUY
        EXPECT_EQ (panel.state(), State::cancelled);
        EXPECT_EQ (panel.getAskButtonForTest().getButtonText(), vi (kAskButton));

        // The backend saw the cancel and returned; its result must not repaint the panel.
        for (int i = 0; i < 400 && ! waiting->sawCancel.load(); ++i)
            juce::Thread::sleep (5);
        EXPECT_TRUE (waiting->sawCancel.load());
        queue.waitAndDrain (300);
        EXPECT_EQ (panel.state(), State::cancelled);
        app.stopSessionLog();
    }
    logDir.deleteRecursively();
    credDir.deleteRecursively();
}

TEST (CopilotAskWiring, ClickWhileACancelledRunUnwindsShowsBusy)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto logDir = freshDir ("log6");
    const auto credDir = freshDir ("cred6");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        PostQueue queue;
        auto backend = std::make_unique<StubbornBackend>();
        auto* stubborn = backend.get();
        app.setCopilotBackendForTest (std::move (backend), [&] (std::function<void()> fn) { queue.post (std::move (fn)); });
        ASSERT_TRUE (app.startSessionLog (logDir));
        app.getCopilotSetupPanelForTest().onValuesChanged (openAiValues());

        auto& panel = app.getCopilotPanelForTest();
        panel.onAsk ("q");
        ASSERT_EQ (panel.state(), State::asking);
        for (int i = 0; i < 400 && ! stubborn->entered.load(); ++i)
            juce::Thread::sleep (5);
        ASSERT_TRUE (stubborn->entered.load());

        panel.getAskButtonForTest().onClick();   // HUY: the backend ignores it for now
        ASSERT_EQ (panel.state(), State::cancelled);

        panel.onAsk ("again");
        EXPECT_EQ (panel.state(), State::busy);
        EXPECT_EQ (panel.getStatusTextForTest(), vi (kBusyText));
        panel.onAnalyse();
        EXPECT_EQ (panel.state(), State::busy);
        EXPECT_EQ (stubborn->calls.load(), 1);

        stubborn->release = true;
        queue.waitAndDrain (500);
        app.stopSessionLog();
    }
    logDir.deleteRecursively();
    credDir.deleteRecursively();
}

TEST (CopilotAskWiring, OverlayCoversTheLeftColumnOnlyAndFollowsTheRules)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto credDir = freshDir ("cred6");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        layout (app, 1440, 920);
        auto& overlay = app.getCopilotSetupPanelForTest();
        EXPECT_FALSE (overlay.isVisible());

        app.getCopilotPanelForTest().getSetupButtonForTest().onClick();
        EXPECT_TRUE (app.copilotSetupOpenForTest());
        ASSERT_TRUE (overlay.isVisible());

        const auto b = app.copilotSetupBoundsForTest();
        const auto col = app.leftColumnBoundsForTest();
        EXPECT_FALSE (col.isEmpty());
        EXPECT_EQ (b, col);   // exactly the union of notch list + copilot panel
        EXPECT_TRUE (col.contains (app.notchListBoundsForTest()));
        EXPECT_TRUE (col.contains (app.copilotPanelBoundsForTest()));
        EXPECT_TRUE (app.getLocalBounds().contains (b));
        EXPECT_FALSE (b.intersects (app.railBoundsForTest()));   // BYPASS stays reachable
        EXPECT_FALSE (b.intersects (app.spectrumBoundsForTest()));
        EXPECT_FALSE (b.intersects (app.slotTableBoundsForTest()));
        EXPECT_GE (b.getHeight(), overlay.preferredHeightForWidth (b.getWidth()));   // every row fits

        // The setup values reflect the settings.
        EXPECT_EQ (overlay.currentValues().provider, "anthropic");
        EXPECT_FALSE (overlay.currentValues().consent);

        // ESC-less close: the button.
        overlay.getCloseButtonForTest().onClick();
        EXPECT_FALSE (overlay.isVisible());
        EXPECT_FALSE (app.copilotSetupOpenForTest());

        // Too short a column: hidden even though open; comes back when there is room.
        app.getCopilotPanelForTest().getSetupButtonForTest().onClick();
        ASSERT_TRUE (overlay.isVisible());
        layout (app, 1440, 640);
        EXPECT_FALSE (overlay.isVisible());
        layout (app, 1440, 920);
        EXPECT_TRUE (overlay.isVisible());

        // Leaving Geek closes it, and it cannot open in Dumb.
        app.setUiMode (UiMode::Dumb);
        EXPECT_FALSE (overlay.isVisible());
        EXPECT_FALSE (app.copilotSetupOpenForTest());
        app.getCopilotPanelForTest().getSetupButtonForTest().onClick();
        EXPECT_FALSE (overlay.isVisible());
    }
    credDir.deleteRecursively();
}

TEST (CopilotAskWiring, SettingsAreSavedThroughTheUiModePathAndNeverContainAKey)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshDir ("settings");
    const auto credDir = freshDir ("cred7");
    const auto settingsFile = dir.getChildFile ("settings.json");
    const juce::String secret = "sk-test-SECRET-1234567890";
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        app.enableSettings (settingsFile);
        app.setUiMode (UiMode::Geek);

        auto& overlay = app.getCopilotSetupPanelForTest();
        overlay.onValuesChanged (openAiValues());
        overlay.onStoreKey ("openai_compatible", secret);   // the wiring path; may fail off Windows

        EXPECT_EQ (app.appSettingsForTest().copilotProvider, "openai_compatible");
        EXPECT_EQ (app.appSettingsForTest().copilotModel, "llama3");
        EXPECT_EQ (app.appSettingsForTest().copilotBaseUrl, "http://localhost:11434/v1");
        EXPECT_TRUE (app.appSettingsForTest().copilotConsent);

        // Nothing the user typed into the key field can ever be in the settings object.
        overlay.getKeyEditorForTest().setText (secret, false);
        overlay.getStoreKeyButtonForTest().onClick();
        EXPECT_TRUE (overlay.getKeyEditorForTest().getText().isEmpty());
    }

    ASSERT_TRUE (settingsFile.existsAsFile());
    const auto text = settingsFile.loadFileAsString();
    EXPECT_TRUE (text.contains ("openai_compatible"));
    EXPECT_TRUE (text.contains ("llama3"));
    EXPECT_FALSE (text.contains (secret));
    EXPECT_FALSE (text.contains ("sk-test"));

    // A fresh component reads them back.
    {
        MainComponent again;
        again.setCopilotCredentialDirectoryForTest (credDir);
        again.enableSettings (settingsFile);
        EXPECT_EQ (again.appSettingsForTest().copilotProvider, "openai_compatible");
        EXPECT_EQ (again.appSettingsForTest().copilotModel, "llama3");
        EXPECT_TRUE (again.appSettingsForTest().copilotConsent);
    }

    // The credentials file (Windows only) holds ciphertext, never the plain key.
    const auto credFile = credDir.getChildFile ("copilot-credentials.json");
    if (credFile.existsAsFile())
        EXPECT_FALSE (credFile.loadFileAsString().contains (secret));

    dir.deleteRecursively();
    credDir.deleteRecursively();
}

//==============================================================================
// Security-review fixes: caution line, consent withdrawal, key bound to an address.
namespace
{
const char* const kCaution = "AI c\xc3\xb3 th\xe1\xbb\x83 sai. \xc4\x90\xe1\xbb\xabng t\xc4\x83ng gain hay t\xe1\xba\xaft ch\xe1\xbb\x91ng h\xc3\xba theo l\xe1\xbb\x9di n\xc3\xa0y.";
const char* const kKeyOtherOrigin = "Kho\xc3\xa1 \xc4\x91\xc3\xa3 l\xc6\xb0u cho \xc4\x91\xe1\xbb\x8b" "a ch\xe1\xbb\x89 kh\xc3\xa1" "c. Nh\xe1\xba\xadp l\xe1\xba\xa1i kho\xc3\xa1.";
const char* const kKeyUrlInvalid = "\xc4\x90\xe1\xbb\x8b" "a ch\xe1\xbb\x89 ch\xc6\xb0" "a h\xe1\xbb\xa3p l\xe1\xbb\x87, ch\xc6\xb0" "a l\xc6\xb0u kho\xc3\xa1.";
} // namespace

TEST (CopilotAskPanel, CautionLineIsUnderEveryAnswerAndNowhereElse)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    panel.setSize (360, 300);
    panel.resized();

    EXPECT_EQ (gui::CopilotPanel::cautionTextForTest(), vi (kCaution));
    EXPECT_FALSE (panel.isCautionVisibleForTest());

    panel.setAnswer (makeAnswer ("Short."));
    ASSERT_TRUE (panel.isCautionVisibleForTest());
    auto c = panel.getCautionBoundsForTest();
    EXPECT_TRUE (panel.getLocalBounds().contains (c));
    EXPECT_GE (c.getY(), panel.getListBoundsForTest().getBottom());      // outside the scrolled list
    EXPECT_LE (c.getBottom(), panel.getAskRowBoundsForTest().getY());     // above the ask row

    // A long answer scrolls inside the list; the caution does not move out of the panel.
    juce::String longText;
    for (int i = 0; i < 60; ++i)
        longText << "Sentence number " << i << " is long enough to wrap onto more lines. ";
    panel.setAnswer (makeAnswer (longText));
    ASSERT_TRUE (panel.isCautionVisibleForTest());
    c = panel.getCautionBoundsForTest();
    EXPECT_TRUE (panel.getLocalBounds().contains (c));
    EXPECT_LE (c.getBottom(), panel.getAskRowBoundsForTest().getY());
    EXPECT_GE (c.getY(), panel.getListBoundsForTest().getBottom());
    EXPECT_GT (panel.getContentHeightForTest(), panel.getListBoundsForTest().getHeight());

    // Narrow panel: the line wraps, still inside.
    panel.setSize (240, 300);
    panel.resized();
    c = panel.getCautionBoundsForTest();
    EXPECT_TRUE (panel.getLocalBounds().contains (c));
    EXPECT_LE (c.getBottom(), panel.getAskRowBoundsForTest().getY());

    // Not under the fixed blocked message, an error, rule findings or any bare state.
    CopilotAnswer blocked;
    blocked.kind = CopilotAnswer::Kind::blocked;
    panel.setAnswer (blocked);
    EXPECT_FALSE (panel.isCautionVisibleForTest());

    panel.setAnswer (makeAnswer ("x"));
    EXPECT_TRUE (panel.isCautionVisibleForTest());
    CopilotAnswer error;
    error.kind = CopilotAnswer::Kind::error;
    error.text = "boom";
    panel.setAnswer (error);
    EXPECT_FALSE (panel.isCautionVisibleForTest());

    panel.setAnswer (makeAnswer ("x"));
    CopilotFinding f;
    f.severity = "warn";
    f.title = "t";
    f.advice = "a";
    panel.setFindings ({ f });
    EXPECT_FALSE (panel.isCautionVisibleForTest());

    panel.setAnswer (makeAnswer ("x"));
    panel.setState (State::idle);
    EXPECT_FALSE (panel.isCautionVisibleForTest());
}

TEST (CopilotAskWiring, WithdrawingConsentIsSavedEvenWhenAnotherFieldIsInvalid)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshDir ("consent");
    const auto credDir = freshDir ("cred8");
    const auto settingsFile = dir.getChildFile ("settings.json");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        app.enableSettings (settingsFile);
        app.setUiMode (UiMode::Geek);

        auto& overlay = app.getCopilotSetupPanelForTest();
        overlay.onValuesChanged (openAiValues());
        overlay.setValues (openAiValues());
        ASSERT_TRUE (app.appSettingsForTest().copilotConsent);

        // The address becomes invalid, then consent is switched OFF.
        overlay.getUrlEditorForTest().setText ("nonsense", false);
        overlay.getConsentToggleForTest().setToggleState (false, juce::dontSendNotification);
        overlay.getConsentToggleForTest().onClick();

        EXPECT_FALSE (app.appSettingsForTest().copilotConsent);
        EXPECT_EQ (app.appSettingsForTest().copilotBaseUrl, "http://localhost:11434/v1");   // invalid value not saved

        // Turning it back ON with an invalid field is still refused (existing validation).
        overlay.getConsentToggleForTest().setToggleState (true, juce::dontSendNotification);
        overlay.getConsentToggleForTest().onClick();
        EXPECT_FALSE (app.appSettingsForTest().copilotConsent);
    }
    {
        MainComponent again;
        again.setCopilotCredentialDirectoryForTest (credDir);
        again.enableSettings (settingsFile);
        EXPECT_FALSE (again.appSettingsForTest().copilotConsent);
    }
    dir.deleteRecursively();
    credDir.deleteRecursively();
}

TEST (CopilotAskWiring, SavingAnOpenAiKeyIsRefusedForAnInvalidAddress)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto credDir = freshDir ("cred9");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        auto& overlay = app.getCopilotSetupPanelForTest();

        auto v = openAiValues();
        v.baseUrl = "https://user:pass@host/v1";   // userinfo is not a valid address
        overlay.setValues (v);
        overlay.onStoreKey ("openai_compatible", "test-key-not-real");
        EXPECT_EQ (overlay.getKeyStatusTextForTest(), vi (kKeyUrlInvalid));
        EXPECT_FALSE (credDir.getChildFile ("copilot-credentials.json").existsAsFile());

        v.baseUrl = "";
        overlay.setValues (v);
        overlay.onStoreKey ("openai_compatible", "test-key-not-real");
        EXPECT_EQ (overlay.getKeyStatusTextForTest(), vi (kKeyUrlInvalid));
        EXPECT_FALSE (credDir.getChildFile ("copilot-credentials.json").existsAsFile());
    }
    credDir.deleteRecursively();
}

TEST (CopilotAskWiring, KeyStatusFollowsTheAddressAndAnOldFormatEntryIsNeverUsed)
{
    if (! CopilotCredentialStore::isSupported())
        GTEST_SKIP() << "the key store exists only on Windows";

    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto credDir = freshDir ("cred10");
    {
        MainComponent app;
        app.setCopilotCredentialDirectoryForTest (credDir);
        auto& overlay = app.getCopilotSetupPanelForTest();
        overlay.setValues (openAiValues());

        overlay.onStoreKey ("openai_compatible", "test-key-not-real");
        EXPECT_EQ (overlay.getKeyStatusTextForTest(), vi (kKeySaved));
        EXPECT_TRUE (app.copilotCredentialStoreForTest().hasKey ("openai_compatible", "http://localhost:11434/v1"));

        // Editing the address refreshes the status at once.
        overlay.getUrlEditorForTest().setText ("https://api.example.com/v1", false);
        overlay.getUrlEditorForTest().onTextChange();
        EXPECT_EQ (overlay.getKeyStatusTextForTest(), vi (kKeyOtherOrigin));

        overlay.getUrlEditorForTest().setText ("http://LOCALHOST:11434/other", false);
        overlay.getUrlEditorForTest().onTextChange();
        EXPECT_EQ (overlay.getKeyStatusTextForTest(), vi (kKeySaved));   // same origin

        // A file written by release 1.5.0 (blob, no origin).
        const auto file = credDir.getChildFile ("copilot-credentials.json");
        juce::var parsed;
        ASSERT_TRUE (juce::JSON::parse (file.loadFileAsString(), parsed).wasOk());
        parsed["profiles"]["openai_compatible"].getDynamicObject()->removeProperty ("origin");
        ASSERT_TRUE (file.replaceWithText (juce::JSON::toString (parsed, false), false, false, "\n"));

        overlay.getUrlEditorForTest().onTextChange();
        EXPECT_EQ (overlay.getKeyStatusTextForTest(), vi (kKeyOtherOrigin));
        EXPECT_FALSE (app.copilotCredentialStoreForTest().hasKey ("openai_compatible", "http://localhost:11434/v1"));

        // XOA KHOA still removes it.
        overlay.onDeleteKey ("openai_compatible");
        EXPECT_EQ (overlay.getKeyStatusTextForTest(), vi (kKeyMissing));
    }
    credDir.deleteRecursively();
}
