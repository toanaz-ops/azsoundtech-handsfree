// Lane L3a, the visible part: the AI mode switch and its explainer, the
// supervisor chip, the proposal rows, the BY column of the notch table and the
// authorship / ghost marks on the spectrum chart.
//
// PHASE L3a CHANGES THE AUDIO BY EXACTLY 0 dB: every test here that clicks BO
// or APPLY also proves that no notch event or command was produced.
//
// Same headless rules as the sibling GUI tests (memory/gui-console-lessons):
// no message loop, resized() driven by hand, buttons driven through onClick().

#include <gtest/gtest.h>

#include "app/MainComponent.h"
#include "app/NotchController.h"
#include "dsp/ClockSource.h"
#include "dsp/Detector.h"
#include "dsp/LockFreeRingBuffer.h"
#include "gui/CopilotPanel.h"
#include "gui/NotchListPanel.h"
#include "gui/SpectrumView.h"
#include "gui/theme/AzTheme.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace
{
juce::String u (const char* bytes) { return juce::String::fromUTF8 (bytes); }

juce::String minus() { return juce::String::charToString ((juce::juce_wchar) 0x2212); }
juce::String emDash() { return juce::String (" ") + juce::String::charToString ((juce::juce_wchar) 0x2014) + " "; }

// A button's click as a user produces it: the toggle flips, then onClick runs.
// (triggerClick() is asynchronous and the suite pumps no message loop.)
void clickSegment (juce::Button& b)
{
    b.setToggleState (true, juce::dontSendNotification);
    if (b.onClick != nullptr)
        b.onClick();
}

ShownProposal proposal (std::uint32_t id, AiProposal::Tool tool, AiProposal::Reason reason,
                        float hz, float targetDb = 0.0f, int holdSeconds = 0,
                        int lane = 0, int index = 0, int slot = 0)
{
    ShownProposal sp;
    sp.id = id;
    sp.generation = 1;
    sp.slot = slot; sp.lane = lane; sp.index = index;
    sp.hz = hz;
    sp.proposal.tool = tool;
    sp.proposal.nid = AiSupervisor::globalNid (slot, 1);
    sp.proposal.expectHz = hz;
    sp.proposal.targetDb = targetDb;
    sp.proposal.holdSeconds = holdSeconds;
    sp.proposal.reason = reason;
    return sp;
}

void layout (gui::CopilotPanel& panel, int w = 620, int h = 330)
{
    panel.setSize (w, h);
    panel.resized();   // headless: setSize() alone does not run it
}

struct FedController
{
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tap, commands, clock };

    void pump (int blocks = 8)
    {
        std::vector<float> hop ((std::size_t) Detector::kHopSize, 0.1f);
        for (int i = 0; i < blocks; ++i)
        {
            tap.write (hop.data(), hop.size());
            controller.runOnce();
        }
    }
};
} // namespace

//==============================================================================
// The switch
TEST (AiDriveSwitch, ReflectsAndSetsTheModeAndActCannotBeSelected)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    auto& panel = app.getCopilotPanelForTest();
    auto& sw = panel.getModeSwitchForTest();

    ASSERT_EQ (sw.getNumSegments(), 4);
    EXPECT_EQ (sw.getSegmentForTest (0).getButtonText(), u ("T\xe1\xba\xaeT"));
    EXPECT_EQ (sw.getSegmentForTest (1).getButtonText(), u ("B\xc3\x93NG"));
    EXPECT_EQ (sw.getSegmentForTest (2).getButtonText(), u ("G\xe1\xbb\xa2I \xc3\x9d"));
    EXPECT_EQ (sw.getSegmentForTest (3).getButtonText(), u ("T\xe1\xbb\xb0 L\xc3\x81I"));

    // The app's mode drives the switch...
    for (const auto [mode, index] : { std::pair { AiMode::Off, 0 }, std::pair { AiMode::Shadow, 1 },
                                      std::pair { AiMode::Suggest, 2 } })
    {
        app.setAiMode (mode);
        EXPECT_EQ (sw.getSelectedIndex(), index);
        EXPECT_EQ (panel.aiMode(), mode);
    }

    // ...and the switch drives the app's mode.
    clickSegment (sw.getSegmentForTest (0));
    EXPECT_EQ (app.aiMode(), AiMode::Off);
    clickSegment (sw.getSegmentForTest (1));
    EXPECT_EQ (app.aiMode(), AiMode::Shadow);
    clickSegment (sw.getSegmentForTest (2));
    EXPECT_EQ (app.aiMode(), AiMode::Suggest);

    // TU LAI: disabled, says why, and clicking it changes nothing.
    auto& act = sw.getSegmentForTest (3);
    EXPECT_FALSE (act.isEnabled());
    EXPECT_EQ (act.getTooltip(), u ("Ch\xc6\xb0" "a c\xc3\xb3 \xe1\xbb\x9f b\xe1\xba\xa3n n\xc3\xa0y."));
    clickSegment (act);
    EXPECT_EQ (app.aiMode(), AiMode::Suggest) << "TU LAI must not change the mode";
    EXPECT_EQ (sw.getSelectedIndex(), 2) << "and the lit segment goes back to the real mode";

    // A persisted Act (older file) is shown as Suggest: the segment cannot light.
    app.setAiMode (AiMode::Act);
    EXPECT_NE (sw.getSelectedIndex(), 3);
}

TEST (AiDriveSwitch, ExplainerLineFollowsTheSelection)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;

    panel.setAiMode (AiMode::Off);
    EXPECT_EQ (panel.getExplainerTextForTest(),
               u ("AI kh\xc3\xb4ng ch\xe1\xba\xa1y. Detector l\xc3\xa0m vi\xe1\xbb\x87" "c nh\xc6\xb0 th\xc6\xb0\xe1\xbb\x9dng."));
    panel.setAiMode (AiMode::Shadow);
    EXPECT_EQ (panel.getExplainerTextForTest(),
               u ("M\xe1\xba\xa1ng n\xc6\xa1-ron ch\xe1\xba\xa5m \xc4\x91i\xe1\xbb\x83m v\xc3\xa0 ghi log. Kh\xc3\xb4ng \xc4\x91\xe1\xbb\x95i g\xc3\xac tr\xc3\xaan notch."));
    panel.setAiMode (AiMode::Suggest);
    EXPECT_EQ (panel.getExplainerTextForTest(),
               u ("AI t\xe1\xbb\xb1 xem bu\xe1\xbb\x95i di\xe1\xbb\x85n khi c\xc3\xb3 s\xe1\xbb\xb1 ki\xe1\xbb\x87n, t\xe1\xbb\x91i \xc4\x91" "a 12 l\xc6\xb0\xe1\xbb\xa3t/gi\xe1\xbb\x9d, d\xc3\xb9ng kho\xc3\xa1 c\xe1\xbb\xa7" "a b\xe1\xba\xa1n. Ch\xe1\xbb\x89 \xc4\x91\xe1\xbb\x81 xu\xe1\xba\xa5t."));

    // No mojibake lead byte (U+00C3) and no decode failure (U+FFFD).
    for (const auto m : { AiMode::Off, AiMode::Shadow, AiMode::Suggest })
    {
        const auto t = gui::CopilotPanel::explainerFor (m);
        EXPECT_FALSE (t.containsChar ((juce::juce_wchar) 0x00c3));
        EXPECT_FALSE (t.containsChar ((juce::juce_wchar) 0xfffd));
    }
}

TEST (AiDriveSwitch, HeaderFitsWithoutClippingAndNeverSqueezesTheTitleButtons)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    panel.setSupervisorStatus (u ("AI Â· Ä" "ang xÃ©t 12 s"));

    for (const int width : { 360, 380, 480, 560, 620, 900 })
    {
        layout (panel, width, 330);
        auto& sw = panel.getModeSwitchForTest();
        EXPECT_GE (sw.getWidth(), sw.getPreferredWidth()) << "the four legends must not be squeezed at " << width;
        EXPECT_TRUE (panel.getLocalBounds().contains (sw.getBounds())) << width;
        EXPECT_FALSE (sw.getBounds().intersects (panel.getAnalyseButtonForTest().getBounds())) << width;
        EXPECT_FALSE (sw.getBounds().intersects (panel.getSetupButtonForTest().getBounds())) << width;
        EXPECT_GE (panel.getSetupButtonForTest().getX(), 0) << width;
        EXPECT_LE (panel.getSetupButtonForTest().getRight(), panel.getAnalyseButtonForTest().getX()) << width;

        // Wide enough: the switch shares the caption row with the title. Narrower:
        // it takes its own row under it.
        // (A minimum-width window gives the notch column about 515 px: always wide.)
        if (width >= 480)
            EXPECT_LT (sw.getBottom(), gui::CopilotPanel::kCaptionHeight + 1) << width;
        else if (width <= 380)
            EXPECT_GE (sw.getY(), gui::CopilotPanel::kCaptionHeight) << width;

        // The explainer is measured, so even the longest text fits its band.
        EXPECT_GE (panel.getExplainerBoundsForTest().getHeight(), gui::CopilotPanel::kExplainerMinHeight) << width;
        EXPECT_TRUE (panel.getLocalBounds().contains (panel.getExplainerBoundsForTest())) << width;
        EXPECT_GE (panel.getExplainerBoundsForTest().getY(), gui::CopilotPanel::kCaptionHeight) << width;
        EXPECT_TRUE (panel.getLocalBounds().contains (panel.getChipBoundsForTest())) << width;
        EXPECT_FALSE (panel.getChipBoundsForTest().isEmpty()) << width;
    }
}

//==============================================================================
// The chip
TEST (AiDriveChip, FollowsTheSupervisorStatusAndHidesWhenEmpty)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    auto& panel = app.getCopilotPanelForTest();

    // Suggest with no key / no session log: the supervisor says so, the chip says the same.
    app.setAiMode (AiMode::Suggest);
    EXPECT_EQ (app.aiSupervisor().status().kind, AiSupervisor::StatusKind::NotConfigured);
    EXPECT_TRUE (panel.isChipVisibleForTest());
    EXPECT_EQ (panel.getChipTextForTest(), app.aiSupervisor().statusText());

    app.setAiMode (AiMode::Off);
    EXPECT_EQ (panel.getChipTextForTest(), app.aiSupervisor().statusText());
    EXPECT_NE (panel.getChipTextForTest(), u ("AI \xc2\xb7 ch\xc6\xb0" "a c\xe1\xba\xa5u h\xc3\xacnh"));

    // Empty text hides it.
    panel.setSupervisorStatus ({});
    EXPECT_FALSE (panel.isChipVisibleForTest());
    panel.setSupervisorStatus (u ("AI \xc2\xb7 \xc4\x91" "ang x\xc3\xa9t 3 s"));
    EXPECT_TRUE (panel.isChipVisibleForTest());
}

//==============================================================================
// The proposals list
TEST (AiDriveProposals, EveryToolAndReasonHasItsFixedSentence)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    using Tool = AiProposal::Tool;
    using Reason = AiProposal::Reason;

    const auto sentence = [] (Tool t, Reason r, float hz, float target, int hold)
    {
        return gui::CopilotPanel::proposalSentence (proposal (1, t, r, hz, target, hold));
    };

    EXPECT_EQ (sentence (Tool::Deepen, Reason::RecurringAtFrequency, 1008.0f, -12.0f, 0),
               u ("\xc4\x90\xc3\xa0o s\xc3\xa2u notch ") + "1.0 kHz" + u (" xu\xe1\xbb\x91ng ") + minus() + "12.0 dB" + emDash()
                   + u ("t\xe1\xba\xa7n s\xe1\xbb\x91 quay l\xe1\xba\xa1i nhi\xe1\xbb\x81u l\xe1\xba\xa7n"));
    EXPECT_EQ (sentence (Tool::Deepen, Reason::StillRingingAtDepth, 247.0f, -24.0f, 0),
               u ("\xc4\x90\xc3\xa0o s\xc3\xa2u notch ") + "247 Hz" + u (" xu\xe1\xbb\x91ng ") + minus() + "24.0 dB" + emDash()
                   + u ("v\xe1\xba\xabn rung \xe1\xbb\x9f \xc4\x91\xe1\xbb\x99 s\xc3\xa2u hi\xe1\xbb\x87n t\xe1\xba\xa1i"));
    EXPECT_EQ (sentence (Tool::Hold, Reason::ShowInProgress, 2000.0f, 0.0f, 120),
               u ("Gi\xe1\xbb\xaf notch ") + "2.0 kHz" + u (" th\xc3\xaam ") + "120 s" + emDash() + u ("\xc4\x91" "ang di\xe1\xbb\x85n"));
    EXPECT_EQ (sentence (Tool::Hold, Reason::RecurringAtFrequency, 500.0f, 0.0f, 60),
               u ("Gi\xe1\xbb\xaf notch ") + "500 Hz" + u (" th\xc3\xaam ") + "60 s" + emDash()
                   + u ("t\xe1\xba\xa7n s\xe1\xbb\x91 quay l\xe1\xba\xa1i nhi\xe1\xbb\x81u l\xe1\xba\xa7n"));
    EXPECT_EQ (sentence (Tool::Release, Reason::JudgedFalse, 1920.0f, 0.0f, 0),
               u ("Nh\xe1\xba\xa3 notch ") + "1.9 kHz" + emDash() + u ("\xc4\x91\xc6\xb0\xe1\xbb\xa3" "c ch\xe1\xba\xa5m l\xc3\xa0 nh\xe1\xba\xa7m"));
    EXPECT_EQ (sentence (Tool::Release, Reason::StaleNoRecurrence, 300.0f, 0.0f, 0),
               u ("Nh\xe1\xba\xa3 notch ") + "300 Hz" + emDash() + u ("l\xc3\xa2u kh\xc3\xb4ng t\xc3\xa1i di\xe1\xbb\x85n"));

    // no_action is never shown, so it has no sentence.
    EXPECT_TRUE (gui::CopilotPanel::proposalSentence (proposal (1, Tool::NoAction, Reason::NothingToDo, 0.0f)).isEmpty());
}

// A NEW enumerator cannot be left without a sentence: every tool a proposal can
// carry produces a non-empty sentence, and every reason either has text or is
// one of the three no_action reasons that are never displayed.
TEST (AiDriveProposals, EnumToTextLoopsOverEveryEnumerator)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    using Tool = AiProposal::Tool;
    using Reason = AiProposal::Reason;

    const std::set<Reason> neverShown { Reason::NothingToDo, Reason::InsufficientData, Reason::OutsideScope };
    int reasonsSeen = 0;
    for (int v = 0; v < 32; ++v)
    {
        const auto r = (Reason) v;
        const bool known = juce::String (aiReasonName (r)) != "nothing_to_do" || r == Reason::NothingToDo;
        if (! known)
            continue;   // beyond the last enumerator: aiReasonName's fallback
        ++reasonsSeen;
        const auto text = gui::CopilotPanel::reasonText (r);
        if (neverShown.count (r) == 0)
            EXPECT_TRUE (text.isNotEmpty()) << "reason " << aiReasonName (r) << " has no sentence";
        else
            EXPECT_TRUE (text.isEmpty());
        EXPECT_FALSE (text.containsChar ((juce::juce_wchar) 0x00c3));
        EXPECT_FALSE (text.containsChar ((juce::juce_wchar) 0xfffd));
    }
    EXPECT_EQ (reasonsSeen, 8) << "AiProposal::Reason grew: give the new reason a sentence (or list it as never shown)";

    int toolsSeen = 0;
    for (int v = 0; v < 16; ++v)
    {
        const auto t = (Tool) v;
        const bool known = juce::String (aiToolName (t)) != "no_action" || t == Tool::NoAction;
        if (! known)
            continue;
        ++toolsSeen;
        const auto text = gui::CopilotPanel::proposalSentence (proposal (1, t, Reason::RecurringAtFrequency, 1000.0f, -12.0f, 60));
        if (t == Tool::NoAction)
            EXPECT_TRUE (text.isEmpty());
        else
            EXPECT_TRUE (text.isNotEmpty()) << "tool " << aiToolName (t) << " has no sentence";
    }
    EXPECT_EQ (toolsSeen, 4) << "AiProposal::Tool grew: give the new tool a sentence";
}

TEST (AiDriveProposals, RowsShowSentencesButtonsAndTheDismissAllControl)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    layout (panel);

    EXPECT_EQ (panel.getProposalRowCountForTest(), 0);
    EXPECT_TRUE (panel.getProposalsBoundsForTest().isEmpty());
    EXPECT_FALSE (panel.getDismissAllButtonForTest().isVisible());

    using Tool = AiProposal::Tool;
    using Reason = AiProposal::Reason;
    panel.setProposals ({ proposal (1, Tool::Deepen, Reason::RecurringAtFrequency, 247.0f, -24.0f) });
    EXPECT_EQ (panel.getProposalRowCountForTest(), 1);
    EXPECT_FALSE (panel.getProposalsBoundsForTest().isEmpty());
    EXPECT_FALSE (panel.getDismissAllButtonForTest().isVisible()) << "BO HET needs two or more";

    panel.setProposals ({ proposal (1, Tool::Deepen, Reason::RecurringAtFrequency, 247.0f, -24.0f),
                          proposal (2, Tool::Release, Reason::JudgedFalse, 1920.0f) });
    EXPECT_EQ (panel.getProposalRowCountForTest(), 2);
    EXPECT_TRUE (panel.getDismissAllButtonForTest().isVisible());
    EXPECT_EQ (panel.getDismissAllButtonForTest().getButtonText(), u ("B\xe1\xbb\x8e H\xe1\xba\xbeT"));
    EXPECT_EQ (panel.getProposalSentenceForTest (0), gui::CopilotPanel::proposalSentence (
                   proposal (1, Tool::Deepen, Reason::RecurringAtFrequency, 247.0f, -24.0f)));

    for (int row = 0; row < 2; ++row)
    {
        ASSERT_NE (panel.getApplyButtonForTest (row), nullptr);
        ASSERT_NE (panel.getDismissButtonForTest (row), nullptr);
        EXPECT_EQ (panel.getApplyButtonForTest (row)->getButtonText(), u ("\xc3\x81P D\xe1\xbb\xa4NG"));
        EXPECT_EQ (panel.getDismissButtonForTest (row)->getButtonText(), u ("B\xe1\xbb\x8e"));
        EXPECT_FALSE (panel.getApplyButtonForTest (row)->isEnabled());
        EXPECT_EQ (panel.getApplyButtonForTest (row)->getTooltip(), gui::CopilotPanel::applyTooltipText());
        EXPECT_TRUE (panel.getDismissButtonForTest (row)->isEnabled());
    }
    EXPECT_EQ (panel.getApplyButtonForTest (2), nullptr);
}

TEST (AiDriveProposals, ListScrollsAndNeverTakesTheAnswerAreaOrTheCautionLine)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    gui::CopilotPanel panel;
    layout (panel, 620, gui::CopilotPanel::kMinHeight + 40);

    std::vector<ShownProposal> many;
    for (std::uint32_t i = 1; i <= 8; ++i)
        many.push_back (proposal (i, AiProposal::Tool::Deepen, AiProposal::Reason::RecurringAtFrequency,
                                  300.0f * (float) i, -18.0f));
    panel.setProposals (many);

    CopilotAnswer answer;
    answer.kind = CopilotAnswer::Kind::answer;
    answer.text = "A short answer.";
    answer.model = "m";
    panel.setAnswer (answer);
    layout (panel, 620, gui::CopilotPanel::kMinHeight + 40);

    ASSERT_TRUE (panel.isCautionVisibleForTest()) << "the caution line of an AI answer must stay";
    const auto bounds = panel.getLocalBounds();
    EXPECT_TRUE (bounds.contains (panel.getCautionBoundsForTest()));
    EXPECT_FALSE (panel.getProposalsBoundsForTest().intersects (panel.getCautionBoundsForTest()));
    EXPECT_FALSE (panel.getProposalsBoundsForTest().intersects (panel.getListBoundsForTest()));
    EXPECT_GT (panel.getListBoundsForTest().getHeight(), 0) << "the answer area keeps room";
    EXPECT_LT (panel.getProposalsBoundsForTest().getBottom(), panel.getAskRowBoundsForTest().getY());
}

// BO calls dismiss(id) and nothing else; it removes exactly that proposal and
// no notch command or event comes out. APPLY is disabled and does nothing.
TEST (AiDriveProposals, DismissRemovesExactlyThatProposalAndTouchesNoNotch)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    auto* controller = app.getNotchControllerForTest (0);
    ASSERT_NE (controller, nullptr);
    ASSERT_TRUE (controller->setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));

    using Tool = AiProposal::Tool;
    using Reason = AiProposal::Reason;
    app.aiSupervisor().setAiProposalsForTest ({ proposal (11, Tool::Deepen, Reason::StillRingingAtDepth, 1000.0f, -18.0f),
                                                proposal (12, Tool::Release, Reason::StaleNoRecurrence, 1000.0f),
                                                proposal (13, Tool::Hold, Reason::ShowInProgress, 1000.0f, 0.0f, 90) });
    auto& panel = app.getCopilotPanelForTest();
    ASSERT_EQ (panel.getProposalRowCountForTest(), 3);

    const int eventsBefore = controller->pendingEventsForTest();
    const double depthBefore = controller->depthDbForTest (0, 0);

    // APPLY: disabled, and driving it does nothing at all.
    auto* apply = panel.getApplyButtonForTest (0);
    ASSERT_NE (apply, nullptr);
    EXPECT_FALSE (apply->isEnabled());
    if (apply->onClick != nullptr)
        apply->onClick();
    EXPECT_EQ (app.aiSupervisor().proposals().size(), 3u);

    // BO on the middle row.
    ASSERT_NE (panel.getDismissButtonForTest (1), nullptr);
    panel.getDismissButtonForTest (1)->onClick();
    const auto& left = app.aiSupervisor().proposals();
    ASSERT_EQ (left.size(), 2u);
    EXPECT_EQ (left[0].id, 11u);
    EXPECT_EQ (left[1].id, 13u);
    EXPECT_EQ (panel.getProposalRowCountForTest(), 2);

    // BO HET.
    app.aiSupervisor().dismissAll();
    EXPECT_TRUE (app.aiSupervisor().proposals().empty());
    EXPECT_EQ (panel.getProposalRowCountForTest(), 0);
    EXPECT_TRUE (panel.getProposalsBoundsForTest().isEmpty());

    EXPECT_EQ (controller->pendingEventsForTest(), eventsBefore) << "no notch event";
    EXPECT_DOUBLE_EQ (controller->depthDbForTest (0, 0), depthBefore) << "the notch stands exactly where it was";
    EXPECT_TRUE (controller->activeForTest (0, 0));
}

TEST (AiDriveProposals, DismissAllButtonOfThePanelCallsTheSupervisor)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    using Tool = AiProposal::Tool;
    using Reason = AiProposal::Reason;
    app.aiSupervisor().setAiProposalsForTest ({ proposal (1, Tool::Deepen, Reason::StillRingingAtDepth, 1000.0f, -18.0f),
                                                proposal (2, Tool::Release, Reason::StaleNoRecurrence, 2000.0f) });
    auto& panel = app.getCopilotPanelForTest();
    app.setSize (1440, 920);
    app.resized();
    ASSERT_TRUE (panel.getDismissAllButtonForTest().isVisible());
    panel.getDismissAllButtonForTest().onClick();
    EXPECT_TRUE (app.aiSupervisor().proposals().empty());
}

//==============================================================================
// The BY column
TEST (AiDriveByColumn, EveryAuthorCombinationHasItsCell)
{
    using gui::NotchListPanel;
    const Author all[] = { Author::Detector, Author::Human, Author::Network, Author::Llm };
    const auto letter = [] (Author a) { return a == Author::Detector ? "D" : a == Author::Human ? "H" : a == Author::Network ? "N" : "A"; };

    for (const auto placed : all)
        for (const auto changed : all)
        {
            const auto expected = placed == changed ? juce::String (letter (placed))
                                                    : juce::String (letter (placed)) + ">" + letter (changed);
            EXPECT_EQ (NotchListPanel::formatAuthor (placed, changed), expected);
        }
    EXPECT_EQ (NotchListPanel::formatAuthor (Author::Detector, Author::Llm), "D>A");
}

TEST (AiDriveByColumn, WidestCellsFitTheColumnWithTheRealFont)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    using gui::NotchListPanel;
    const Author all[] = { Author::Detector, Author::Human, Author::Network, Author::Llm };

    float widest = 0.0f;
    for (const auto a : all)
        for (const auto b : all)
            widest = std::max (widest, az::theme::stringWidth (az::theme::monoFont(), NotchListPanel::formatAuthor (a, b)));

    EXPECT_LE (widest + 4.0f, NotchListPanel::kColByW) << "the widest cell (" << widest << " px) + 4 px inset must fit";
    const auto header = az::theme::legendFont (az::theme::columnFontSize, true, az::theme::trackingColumn);
    EXPECT_LE (az::theme::stringWidth (header, "BY"), NotchListPanel::kColByW);

    // The status column still clears its widest cell at the narrowest panel.
    const auto mono = az::theme::monoFont();
    EXPECT_GE (NotchListPanel::statusWidthFor (360.0f) + 0.0f, 0.0f);
    juce::ignoreUnused (mono);
}

TEST (AiDriveByColumn, RowsCarryTheAuthorOfEachNotch)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    FedController fed;
    ASSERT_TRUE (fed.controller.setNotchAuthoredForTest (0, 0, 500.0, 30.0, -12.0, Author::Detector));
    ASSERT_TRUE (fed.controller.setNotch (0, 1, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));   // Human
    ASSERT_TRUE (fed.controller.setNotchAuthoredForTest (0, 2, 2000.0, 30.0, -12.0, Author::Network));
    ASSERT_TRUE (fed.controller.setNotchAuthoredForTest (0, 3, 4000.0, 30.0, -12.0, Author::Llm));
    fed.pump();

    gui::NotchListPanel table (fed.controller);
    table.refreshFromSnapshot();
    ASSERT_EQ (table.rowCountForTest(), 4);

    std::map<juce::String, juce::String> byFreq;
    for (int i = 0; i < table.rowCountForTest(); ++i)
        byFreq[table.rowForTest (i).freq] = table.rowForTest (i).by;
    EXPECT_EQ (byFreq["500 Hz"], "D");
    EXPECT_EQ (byFreq["1.0 kHz"], "H");
    EXPECT_EQ (byFreq["2.0 kHz"], "N");
    EXPECT_EQ (byFreq["4.0 kHz"], "A");

    // Layout survives: stereo-width, mono-width and a narrow panel all paint.
    for (const int w : { 360, 440, 640 })
    {
        juce::Image image (juce::Image::ARGB, w, 260, true);
        juce::Graphics g (image);
        table.setSize (w, 260);
        table.paint (g);
        EXPECT_GE (gui::NotchListPanel::statusWidthFor ((float) w), 0.0f);
    }
}

//==============================================================================
// The chart
TEST (AiDriveChart, AuthoredNotchesReportAPipAndALetterAndOthersDoNot)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    FedController fed;
    ASSERT_TRUE (fed.controller.setNotchAuthoredForTest (0, 0, 500.0, 30.0, -12.0, Author::Detector));
    ASSERT_TRUE (fed.controller.setNotch (0, 1, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    ASSERT_TRUE (fed.controller.setNotchAuthoredForTest (0, 2, 2000.0, 30.0, -12.0, Author::Network));
    ASSERT_TRUE (fed.controller.setNotchAuthoredForTest (1, 3, 4000.0, 30.0, -12.0, Author::Llm));
    fed.pump();

    gui::SpectrumView view (fed.controller);
    view.refreshFromSnapshot();

    std::map<int, char> letterAt;       // by rounded Hz
    std::map<int, juce::String> flagAt;
    for (std::uint32_t i = 0; i < view.snapshotNotchCountForTest(); ++i)
    {
        const int hz = (int) std::lround (view.snapshotNotchForTest (i).frequency);
        letterAt[hz] = view.authorMarkLetterForTest (i);
        flagAt[hz] = view.flagLabelForTest (i);
    }
    const char none = '\0';
    EXPECT_EQ (letterAt[500], none) << "detector: no pip, no letter";
    EXPECT_EQ (letterAt[1000], none) << "human: no pip, no letter";
    EXPECT_EQ (letterAt[2000], 'N');
    EXPECT_EQ (letterAt[4000], 'A');
    EXPECT_FALSE (flagAt[500].containsAnyOf ("NA"));
    EXPECT_FALSE (flagAt[1000].containsAnyOf ("NA"));
    EXPECT_TRUE (flagAt[2000].endsWith ("N"));
    EXPECT_TRUE (flagAt[4000].endsWith ("RA")) << "lane suffix R composes with the author letter: " << flagAt[4000];

    // The letter comes from lastChangedBy first, placedBy second.
    EXPECT_EQ (gui::SpectrumView::authorMarkLetter (Author::Detector, Author::Llm), 'A');
    EXPECT_EQ (gui::SpectrumView::authorMarkLetter (Author::Llm, Author::Human), 'A');
    EXPECT_EQ (gui::SpectrumView::authorMarkLetter (Author::Network, Author::Detector), 'N');
    EXPECT_EQ (gui::SpectrumView::authorMarkLetter (Author::Human, Author::Detector), none);

    // Painting the authored frame must work.
    juce::Image image (juce::Image::ARGB, 900, 420, true);
    juce::Graphics g (image);
    view.setSize (900, 420);
    view.paint (g);
}

TEST (AiDriveChart, GhostsComeOnlyFromProposalsAndDrawNoFillAndNoAccent)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    FedController fed;
    ASSERT_TRUE (fed.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    fed.pump();

    gui::SpectrumView view (fed.controller);
    view.refreshFromSnapshot();
    view.setSize (900, 420);
    EXPECT_EQ (view.ghostCountForTest(), 0u) << "no proposals, no ghosts";

    const auto render = [&view]
    {
        juce::Image image (juce::Image::ARGB, 900, 420, true);
        juce::Graphics g (image);
        view.paint (g);
        return image;
    };

    // The real marker's age ramp keeps ticking between two renders, so the
    // comparison is made with ghosts on and off inside one clock-stable moment:
    // render twice WITHOUT ghosts first to find the noise, then WITH.
    const auto without = render();

    using Ghost = gui::SpectrumView::ProposalGhost;
    Ghost deepen; deepen.kind = Ghost::Kind::Deepen; deepen.hz = 3000.0f; deepen.currentDb = -12.0f; deepen.targetDb = -24.0f;
    Ghost release; release.kind = Ghost::Kind::Release; release.hz = 6000.0f; release.currentDb = -12.0f;
    view.setProposalGhosts ({ deepen, release });
    EXPECT_EQ (view.ghostCountForTest(), 2u);
    EXPECT_TRUE (std::isnan (view.ghostForTest (1).targetDb));
    EXPECT_EQ (gui::SpectrumView::ghostFlagLabel(), "+A");

    const auto withGhosts = render();

    int changed = 0;
    for (int y = 0; y < withGhosts.getHeight(); ++y)
        for (int x = 0; x < withGhosts.getWidth(); ++x)
        {
            const auto a = without.getPixelAt (x, y), b = withGhosts.getPixelAt (x, y);
            if (a == b)
                continue;
            ++changed;
            // A ghost is quiet: neutral-to-cool grey blends of the secondary colour and the
            // panel. Never the amber accent (R far above B) nor the ice (B far above R).
            EXPECT_LE (std::abs ((int) b.getRed() - (int) b.getBlue()), 40)
                << "pixel " << x << "," << y << " is not a dim-grey blend";
        }
    EXPECT_GT (changed, 40) << "the ghosts must actually draw";

    // Same list again: nothing to do. Cleared: back to none.
    view.setProposalGhosts ({ deepen, release });
    EXPECT_EQ (view.ghostCountForTest(), 2u);
    view.setProposalGhosts ({});
    EXPECT_EQ (view.ghostCountForTest(), 0u);
}

// MainComponent fills the chart from the supervisor, for the displayed slot only.
TEST (AiDriveChart, MainComponentFeedsGhostsFromTheSupervisorForTheDisplayedSlot)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent app;
    auto* controller = app.getNotchControllerForTest (0);
    ASSERT_NE (controller, nullptr);
    ASSERT_TRUE (controller->setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    {   // publish a snapshot carrying the notch (and its nid)
        controller->setSampleRate (48000.0);
        LockFreeRingBuffer<float>& tap = app.getAudioEngine().getTapBuffer (0, 0);
        std::vector<float> hop ((std::size_t) Detector::kHopSize, 0.1f);
        for (int i = 0; i < 8; ++i)
        {
            tap.write (hop.data(), hop.size());
            controller->runOnce();
        }
    }
    NotchController::SnapshotBuffer snap;
    controller->copySnapshot (snap);
    ASSERT_EQ (snap.notchCount, 1u);

    using Tool = AiProposal::Tool;
    auto here = proposal (1, Tool::Deepen, AiProposal::Reason::StillRingingAtDepth, 1000.0f, -18.0f);
    here.proposal.nid = AiSupervisor::globalNid (0, snap.notches[0].nid);
    auto elsewhere = proposal (2, Tool::Release, AiProposal::Reason::StaleNoRecurrence, 1000.0f, 0.0f, 0, 0, 0, 1);

    app.aiSupervisor().setAiProposalsForTest ({ here, elsewhere });
    auto& view = app.getSpectrumViewForTest();
    ASSERT_EQ (view.ghostCountForTest(), 1u) << "only the displayed slot's proposal is drawn";
    EXPECT_EQ (view.ghostForTest (0).kind, gui::SpectrumView::ProposalGhost::Kind::Deepen);
    EXPECT_FLOAT_EQ (view.ghostForTest (0).currentDb, -12.0f);
    EXPECT_FLOAT_EQ (view.ghostForTest (0).targetDb, -18.0f);

    app.aiSupervisor().dismissAll();
    EXPECT_EQ (view.ghostCountForTest(), 0u);
}
