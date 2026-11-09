// Lane L2a: gui::CopilotPanel, its wiring in MainComponent, and the layout rule
// that shares the left floor column with the notch list.
//
// The headless suite pumps no message loop and an unshown Component never gets
// resized() from setSize() (memory/gui-console-lessons-2026-08-24.md), so every
// layout test here calls resized() by hand, and buttons are driven through
// onClick(), never triggerClick() (which is async).

#include <gtest/gtest.h>

#include "app/MainComponent.h"
#include "gui/CopilotPanel.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <set>

namespace
{
using State = gui::CopilotPanel::State;

CopilotFinding finding (const char* severity, const char* title, const char* advice)
{
    CopilotFinding f;
    f.code = "x";
    f.severity = severity;
    f.title = title;
    f.advice = advice;
    return f;
}

std::vector<CopilotFinding> threeFindings()
{
    return {
        finding ("alert", "1008 Hz hits the ceiling", "Short advice."),
        finding ("warn", "1/3 notches marked false",
                 "A much longer piece of advice that has to wrap onto several lines in a narrow column so that "
                 "the row height must grow with it and nothing may ever be clipped at the bottom of the row."),
        finding ("info", "1008 Hz rings again", "Check the mic position.")
    };
}

// A JSON array exactly as the sidecar prints it (ASCII; \u escapes for Vietnamese).
const char* kFixtureJson = R"([
  {"code":"ceiling","severity":"alert","title":"1008 Hz ch\u1ea1m tr\u1ea7n -18 dB","advice":"H\u1ea1 gain monitor.","evidence":{}},
  {"code":"false_rate","severity":"warn","title":"1/3 notch b\u1ecb \u0111\u00e1nh d\u1ea5u oan","advice":"Th\u1eed preset Safe.","evidence":{}},
  {"code":"recurrence","severity":"info","title":"1008 Hz h\u00fa l\u1ea1i 2 l\u1ea7n","advice":"Ki\u1ec3m tra h\u01b0\u1edbng mic.","evidence":{}}
])";

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

CopilotRunResult result (CopilotRunResult::Status s, int code, const char* out = "")
{
    CopilotRunResult r;
    r.status = s;
    r.exitCode = code;
    r.stdoutText = juce::String::fromUTF8 (out);
    return r;
}

juce::File freshDir (const char* tag)
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                 .getChildFile ("handsfree-copilot-" + juce::String (tag) + "-"
                                + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    d.createDirectory();
    return d;
}
} // namespace

//==============================================================================
TEST (CopilotPanel, EveryStateShowsItsOwnSentenceAndTheRightButtonState)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;

    const State states[] = { State::idle, State::running, State::clean, State::noLog,
                             State::sidecarMissing, State::timedOut, State::failed };

    std::set<juce::String> seen;
    for (const auto s : states)
    {
        panel.setState (s);
        const auto text = panel.getStatusTextForTest();
        EXPECT_TRUE (text.isNotEmpty());
        EXPECT_EQ (panel.state(), s);
        EXPECT_EQ (panel.getAnalyseButtonForTest().isEnabled(), s != State::running);

        // Mojibake guard: U+00C3 ("A tilde") is what a mis-decoded Vietnamese
        // lead byte turns into, and U+FFFD is a decode failure. Neither occurs
        // in a correct sentence.
        EXPECT_FALSE (text.containsChar ((juce::juce_wchar) 0x00c3));
        EXPECT_FALSE (text.containsChar ((juce::juce_wchar) 0xfffd));
        seen.insert (text);
    }
    EXPECT_EQ (seen.size(), (size_t) 7);   // one distinct sentence per state

    // The idle sentence names the button; checks the diacritics survive.
    panel.setState (State::idle);
    EXPECT_TRUE (panel.getStatusTextForTest().contains (juce::String::fromUTF8 ("PH\xc3\x82N T\xc3\x8d" "CH")));
    EXPECT_EQ (panel.getAnalyseButtonForTest().getButtonText(), juce::String::fromUTF8 ("PH\xc3\x82N T\xc3\x8d" "CH"));

    panel.setFindings (threeFindings());
    EXPECT_EQ (panel.state(), State::findings);
    EXPECT_TRUE (panel.getStatusTextForTest().startsWith ("3"));
    EXPECT_TRUE (panel.getAnalyseButtonForTest().isEnabled());
}

TEST (CopilotPanel, FindingsGiveRowsAndAnEmptyListMeansClean)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    panel.setSize (360, 220);
    panel.resized();

    panel.setFindings (threeFindings());
    EXPECT_EQ (panel.state(), State::findings);
    EXPECT_EQ (panel.getRowCountForTest(), 3);

    panel.setFindings ({});
    EXPECT_EQ (panel.state(), State::clean);
    EXPECT_EQ (panel.getRowCountForTest(), 0);

    panel.setFindings (threeFindings());
    panel.setState (State::running);   // leaving `findings` drops the old rows
    EXPECT_EQ (panel.getRowCountForTest(), 0);
}

TEST (CopilotPanel, ButtonPressCallsOnAnalyseExceptWhileRunning)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    int calls = 0;
    panel.onAnalyse = [&] { ++calls; };

    panel.getAnalyseButtonForTest().onClick();   // not triggerClick(): that is async
    EXPECT_EQ (calls, 1);

    panel.setState (State::running);
    panel.getAnalyseButtonForTest().onClick();
    EXPECT_EQ (calls, 1);
}

TEST (CopilotPanel, RowsAreMeasuredSoWrappedAdviceIsNeverClipped)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    panel.setSize (360, 240);
    panel.resized();
    panel.setFindings (threeFindings());

    const int shortRow = panel.getRowHeightForTest (0);
    const int longRow  = panel.getRowHeightForTest (1);
    EXPECT_GT (longRow, shortRow + 20);   // the long advice wrapped to extra lines

    int sum = 0;
    for (int i = 0; i < 3; ++i)
        sum += panel.getRowHeightForTest (i);
    EXPECT_EQ (panel.getContentHeightForTest(), sum);

    // Narrower column -> the same text needs MORE height, not clipping.
    panel.setSize (240, 240);
    panel.resized();
    EXPECT_GT (panel.getRowHeightForTest (1), longRow);
}

//==============================================================================
// The mapping from a sidecar run to a state.
TEST (CopilotPanel, ApplyResultMapsEveryOutcome)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    using S = CopilotRunResult::Status;

    gui::applyCopilotResult (panel, result (S::sidecarMissing, -1));
    EXPECT_EQ (panel.state(), State::sidecarMissing);
    gui::applyCopilotResult (panel, result (S::timedOut, 0));
    EXPECT_EQ (panel.state(), State::timedOut);
    gui::applyCopilotResult (panel, result (S::failed, 2));
    EXPECT_EQ (panel.state(), State::noLog);
    gui::applyCopilotResult (panel, result (S::failed, 1));
    EXPECT_EQ (panel.state(), State::failed);
    gui::applyCopilotResult (panel, result (S::ok, 0, "not json"));
    EXPECT_EQ (panel.state(), State::failed);
    gui::applyCopilotResult (panel, result (S::ok, 0, "[]"));
    EXPECT_EQ (panel.state(), State::clean);
    gui::applyCopilotResult (panel, result (S::ok, 0, kFixtureJson));
    EXPECT_EQ (panel.state(), State::findings);
    EXPECT_EQ (panel.getRowCountForTest(), 3);
}

//==============================================================================
// MainComponent wiring. No real sidecar is ever launched: a fake backend and a
// hand-drained poster replace the runner, and the session log lives in a temp
// directory (never the user's real log folder).
TEST (CopilotWiring, NoSessionLogShowsNoLogAndLaunchesNothing)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    PostQueue queue;
    auto backend = std::make_unique<FakeBackend> (result (CopilotRunResult::Status::ok, 0, "[]"));
    auto* fake = backend.get();
    app.setCopilotBackendForTest (std::move (backend), [&] (std::function<void()> fn) { queue.post (std::move (fn)); });

    EXPECT_EQ (app.getCopilotPanelForTest().state(), State::idle);   // nothing launched at construction
    app.getCopilotPanelForTest().getAnalyseButtonForTest().onClick();

    EXPECT_EQ (app.getCopilotPanelForTest().state(), State::noLog);
    EXPECT_EQ (fake->calls(), 0);
}

TEST (CopilotWiring, BackendFindingsOutputBecomesFindings)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshDir ("ok");
    {
        MainComponent app;
        PostQueue queue;
        auto backend = std::make_unique<FakeBackend> (result (CopilotRunResult::Status::ok, 0, kFixtureJson));
        auto* fake = backend.get();
        app.setCopilotBackendForTest (std::move (backend), [&] (std::function<void()> fn) { queue.post (std::move (fn)); });

        ASSERT_TRUE (app.startSessionLog (dir));
        auto& panel = app.getCopilotPanelForTest();
        panel.getAnalyseButtonForTest().onClick();
        EXPECT_EQ (panel.state(), State::running);
        EXPECT_FALSE (panel.getAnalyseButtonForTest().isEnabled());

        ASSERT_TRUE (queue.waitAndDrain());
        EXPECT_EQ (panel.state(), State::findings);
        EXPECT_EQ (panel.getRowCountForTest(), 3);
        EXPECT_EQ (fake->lastRequest().sessionLog, app.currentSessionLogFile());
        EXPECT_EQ (fake->lastRequest().timeoutMs, 30000);

        app.stopSessionLog();
    }
    dir.deleteRecursively();
}

TEST (CopilotWiring, ExitCodeTwoBecomesNoLogAndMissingSidecarIsReported)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = freshDir ("code2");
    {
        MainComponent app;
        PostQueue queue;
        app.setCopilotBackendForTest (std::make_unique<FakeBackend> (result (CopilotRunResult::Status::failed, 2)),
                                      [&] (std::function<void()> fn) { queue.post (std::move (fn)); });
        ASSERT_TRUE (app.startSessionLog (dir));
        auto& panel = app.getCopilotPanelForTest();

        panel.getAnalyseButtonForTest().onClick();
        ASSERT_TRUE (queue.waitAndDrain());
        EXPECT_EQ (panel.state(), State::noLog);

        app.setCopilotBackendForTest (std::make_unique<FakeBackend> (result (CopilotRunResult::Status::sidecarMissing, -1)),
                                      [&] (std::function<void()> fn) { queue.post (std::move (fn)); });
        panel.getAnalyseButtonForTest().onClick();
        ASSERT_TRUE (queue.waitAndDrain());
        EXPECT_EQ (panel.state(), State::sidecarMissing);

        app.stopSessionLog();
    }
    dir.deleteRecursively();
}

//==============================================================================
// Layout: the copilot panel shares the LEFT floor column with the notch list.
namespace
{
constexpr int kMinNotch = gui::NotchListPanel::kCaptionHeight + gui::NotchListPanel::kHeaderHeight
                        + 6 * gui::NotchListPanel::kRowHeight;

void layout (MainComponent& app, int w, int h)
{
    app.setSize (w, h);
    app.resized();   // headless: no peer, so drive the layout pass directly
}
}

TEST (CopilotLayout, BothPanelsFitAtTheReferenceSizeAndDoNotOverlap)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    layout (app, 1440, 920);

    auto& copilot = app.getCopilotPanelForTest();
    const auto c = app.copilotPanelBoundsForTest();
    const auto n = app.notchListBoundsForTest();

    EXPECT_TRUE (copilot.isVisible());
    EXPECT_FALSE (c.isEmpty());
    EXPECT_FALSE (c.intersects (n));
    EXPECT_LT (n.getBottom(), c.getY());              // notch list above, with a gap
    EXPECT_EQ (n.getX(), c.getX());
    EXPECT_EQ (n.getWidth(), c.getWidth());
    EXPECT_GE (n.getHeight(), kMinNotch);              // six rows still fit
    EXPECT_GE (c.getHeight(), gui::CopilotPanel::kMinHeight);
    EXPECT_FALSE (c.intersects (app.slotTableBoundsForTest()));
    EXPECT_FALSE (c.intersects (app.spectrumBoundsForTest()));
    EXPECT_TRUE (app.getLocalBounds().contains (c));
}

TEST (CopilotLayout, ShortWindowHidesCopilotAndGivesTheNotchListTheWholeColumn)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;

    layout (app, 1440, 920);
    EXPECT_TRUE (app.getCopilotPanelForTest().isVisible());

    layout (app, 1440, 640);
    EXPECT_FALSE (app.getCopilotPanelForTest().isVisible());
    const auto shortN = app.notchListBoundsForTest();
    // Hidden: the notch list spans the column the copilot panel would share, so
    // its bounds are what they were before this lane existed.
    EXPECT_GT (shortN.getHeight(), 0);
    EXPECT_LT (shortN.getHeight(), kMinNotch + 8 + gui::CopilotPanel::kMinHeight);
}
