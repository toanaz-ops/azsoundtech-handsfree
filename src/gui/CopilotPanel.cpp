#include "gui/CopilotPanel.h"

#include "gui/NotchListPanel.h"
#include "gui/theme/AzTheme.h"

#include <algorithm>
#include <cmath>

namespace gui
{

namespace
{
// Display strings are written as escaped UTF-8 bytes: this project compiles
// without /utf-8, so a raw literal would be re-read through the execution
// charset and come out as mojibake (same rule as DumbView.cpp).
const char* const kButton = "PH\xc3\x82N T\xc3\x8d" "CH";
const char* const kIdle = "B\xe1\xba\xa5m PH\xc3\x82N T\xc3\x8d" "CH \xc4\x91\xe1\xbb\x83 xem nh\xe1\xba\xadn x\xc3\xa9t v\xe1\xbb\x81 bu\xe1\xbb\x95i di\xe1\xbb\x85n n\xc3\xa0y.";
const char* const kRunning = "\xc4\x90" "ang ph\xc3\xa2n t\xc3\xad" "ch\xe2\x80\xa6";
const char* const kClean = "Kh\xc3\xb4ng th\xe1\xba\xa5y v\xe1\xba\xa5n \xc4\x91\xe1\xbb\x81 n\xc3\xa0o trong log.";
const char* const kNoLog = "Ch\xc6\xb0" "a c\xc3\xb3 log bu\xe1\xbb\x95i di\xe1\xbb\x85n.";
const char* const kMissing = "Ch\xc6\xb0" "a c\xc3\xa0i th\xc3\xa0nh ph\xe1\xba\xa7n copilot.";
const char* const kTimedOut = "Ph\xc3\xa2n t\xc3\xad" "ch qu\xc3\xa1 l\xc3\xa2u, \xc4\x91\xc3\xa3 d\xe1\xbb\xabng.";
const char* const kFailed = "Kh\xc3\xb4ng ph\xc3\xa2n t\xc3\xad" "ch \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c log.";
const char* const kFindingsSuffix = " nh\xe1\xba\xadn x\xc3\xa9t";
const char* const kSetupButton = "C\xc3\x80I AI";
const char* const kAskButton = "H\xe1\xbb\x8eI";
const char* const kCancelButton = "HU\xe1\xbb\xb6";
const char* const kPlaceholder = "H\xe1\xbb\x8fi v\xe1\xbb\x81 bu\xe1\xbb\x95i di\xe1\xbb\x85n n\xc3\xa0y\xe2\x80\xa6";
const char* const kAsking = "\xc4\x90" "ang h\xe1\xbb\x8fi AI\xe2\x80\xa6 t\xe1\xbb\x91i \xc4\x91" "a 2 ph\xc3\xbat.";
const char* const kAnswered = "AI \xc4\x91\xc3\xa3 tr\xe1\xba\xa3 l\xe1\xbb\x9di.";
const char* const kBlockedStatus = "C\xc3\xa2u tr\xe1\xba\xa3 l\xe1\xbb\x9di b\xe1\xbb\x8b ch\xe1\xba\xb7n.";
const char* const kAskFailed = "Kh\xc3\xb4ng h\xe1\xbb\x8fi \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c.";
const char* const kNeedsSetup = "Ch\xc6\xb0" "a c\xc3\xa0i AI. B\xe1\xba\xa5m C\xc3\x80I AI.";
const char* const kCancelled = "\xc4\x90\xc3\xa3 hu\xe1\xbb\xb7 c\xc3\xa2u h\xe1\xbb\x8fi.";
const char* const kBlockedBody = "C\xc3\xa2u tr\xe1\xba\xa3 l\xe1\xbb\x9di c\xe1\xbb\xa7" "a m\xc3\xb4 h\xc3\xacnh \xc4\x91\xc3\xa3 b\xe1\xbb\x8b ch\xe1\xba\xb7n v\xc3\xac n\xc3\xb3 khuy\xc3\xaan m\xe1\xbb\x99t vi\xe1\xbb\x87" "c c\xc3\xb3 th\xe1\xbb\x83 g\xc3\xa2y h\xc3\xba. H\xc3\xa3y h\xe1\xbb\x8fi l\xe1\xba\xa1i, ho\xe1\xba\xb7" "c \xc4\x91\xe1\xbb\x95i m\xc3\xb4 h\xc3\xacnh.";
const char* const kInvalidReply = "Ti\xe1\xba\xbfn tr\xc3\xacnh ph\xe1\xbb\xa5 tr\xe1\xba\xa3 v\xe1\xbb\x81 d\xe1\xbb\xaf li\xe1\xbb\x87u kh\xc3\xb4ng \xc4\x91\xe1\xbb\x8d" "c \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c.";
const char* const kBadSetup = "C\xc3\xa0i \xc4\x91\xe1\xba\xb7t AI ch\xc6\xb0" "a h\xe1\xbb\xa3p l\xe1\xbb\x87. B\xe1\xba\xa5m C\xc3\x80I AI.";
// "Dang ban, thu lai sau mot giay."
const char* const kBusy = "\xc4\x90" "ang b\xe1\xba\xad" "n, th\xe1\xbb\xad l\xe1\xba\xa1i sau m\xe1\xbb\x99t gi\xc3\xa2y.";
const char* const kDot ="\xc2\xb7";
// "AI co the sai. Dung tang gain hay tat chong hu theo loi nay."
const char* const kCaution = "AI c\xc3\xb3 th\xe1\xbb\x83 sai. \xc4\x90\xe1\xbb\xabng t\xc4\x83ng gain hay t\xe1\xba\xaft ch\xe1\xbb\x91ng h\xc3\xba theo l\xe1\xbb\x9di n\xc3\xa0y.";

// Lane L3a: the AI mode switch, the proposals list. Same escaped-UTF-8 rule.
const char* const kModeOff = "T\xe1\xba\xaeT";
const char* const kModeShadow = "B\xc3\x93NG";
const char* const kModeSuggest = "G\xe1\xbb\xa2I \xc3\x9d";
const char* const kModeAct = "T\xe1\xbb\xb0 L\xc3\x81I";
const char* const kActTooltip = "Ch\xc6\xb0" "a c\xc3\xb3 \xe1\xbb\x9f b\xe1\xba\xa3n n\xc3\xa0y.";
const char* const kExplainOff = "AI kh\xc3\xb4ng ch\xe1\xba\xa1y. Detector l\xc3\xa0m vi\xe1\xbb\x87" "c nh\xc6\xb0 th\xc6\xb0\xe1\xbb\x9dng.";
const char* const kExplainShadow = "M\xe1\xba\xa1ng n\xc6\xa1-ron ch\xe1\xba\xa5m \xc4\x91i\xe1\xbb\x83m v\xc3\xa0 ghi log. Kh\xc3\xb4ng \xc4\x91\xe1\xbb\x95i g\xc3\xac tr\xc3\xaan notch.";
const char* const kExplainSuggest = "AI t\xe1\xbb\xb1 xem bu\xe1\xbb\x95i di\xe1\xbb\x85n khi c\xc3\xb3 s\xe1\xbb\xb1 ki\xe1\xbb\x87n, t\xe1\xbb\x91i \xc4\x91" "a 12 l\xc6\xb0\xe1\xbb\xa3t/gi\xe1\xbb\x9d, d\xc3\xb9ng kho\xc3\xa1 c\xe1\xbb\xa7" "a b\xe1\xba\xa1n. Ch\xe1\xbb\x89 \xc4\x91\xe1\xbb\x81 xu\xe1\xba\xa5t.";
const char* const kProposalsCaption = "\xc4\x90\xe1\xbb\x80 XU\xe1\xba\xa4T";
const char* const kApply = "\xc3\x81P D\xe1\xbb\xa4NG";
const char* const kDismiss = "B\xe1\xbb\x8e";
const char* const kDismissAll = "B\xe1\xbb\x8e H\xe1\xba\xbeT";
const char* const kDeepen = "\xc4\x90\xc3\xa0o s\xc3\xa2u notch ";
const char* const kDeepenTo = " xu\xe1\xbb\x91ng ";
const char* const kHold = "Gi\xe1\xbb\xaf notch ";
const char* const kHoldMore = " th\xc3\xaam ";
const char* const kRelease = "Nh\xe1\xba\xa3 notch ";
const char* const kReasonRecurring = "t\xe1\xba\xa7n s\xe1\xbb\x91 quay l\xe1\xba\xa1i nhi\xe1\xbb\x81u l\xe1\xba\xa7n";
const char* const kReasonStill = "v\xe1\xba\xabn rung \xe1\xbb\x9f \xc4\x91\xe1\xbb\x99 s\xc3\xa2u hi\xe1\xbb\x87n t\xe1\xba\xa1i";
const char* const kReasonShow = "\xc4\x91" "ang di\xe1\xbb\x85n";
const char* const kReasonFalse = "\xc4\x91\xc6\xb0\xe1\xbb\xa3" "c ch\xe1\xba\xa5m l\xc3\xa0 nh\xe1\xba\xa7m";
const char* const kReasonStale = "l\xc3\xa2u kh\xc3\xb4ng t\xc3\xa1i di\xe1\xbb\x85n";

juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }

constexpr float kLeftPad      = 10.0f;
constexpr float kRightPad     = 10.0f;
constexpr float kDotSize      = 7.0f;
constexpr float kDotGap       = 8.0f;
constexpr float kTitleSize    = 14.0f;
constexpr float kAdviceSize   = 13.0f;
constexpr int   kRowPadTop    = 7;
constexpr int   kRowPadBottom = 8;
constexpr int   kTitleGap     = 3;
constexpr int   kButtonHeight = 22;
constexpr float kFooterSize   = 12.0f;
constexpr int   kBlockPad     = 8;
constexpr int   kFooterGap    = 6;
constexpr int   kButtonGap    = 6;
constexpr float kCautionSize  = 12.0f;
constexpr int   kCautionPad   = 5;

// Lane L3a.
constexpr int   kSwitchHeight       = 22;
constexpr int   kChipGap            = 12;
constexpr float kExplainerSize      = 12.0f;
constexpr int   kExplainerPad       = 2;
constexpr int   kProposalPadV       = 6;
constexpr int   kProposalButtonH    = 22;
constexpr int   kProposalButtonGap  = 4;
constexpr int   kProposalTextGap    = 10;
constexpr int   kMinProposalsRows   = 46;    // the proposals section always keeps one row's room
constexpr int   kMinMainListHeight  = 40;    // and the answer area never gets squeezed below this

juce::StringArray modeLabels()
{
    return juce::StringArray { utf8 (kModeOff), utf8 (kModeShadow), utf8 (kModeSuggest), utf8 (kModeAct) };
}

int segmentOf (AiMode mode)
{
    switch (mode)
    {
        case AiMode::Off:     return 0;
        case AiMode::Shadow:  return 1;
        case AiMode::Suggest: return 2;
        case AiMode::Act:     return 2;   // not selectable in L3a: shown as Suggest
    }
    return 1;
}

constexpr int kActSegment = 3;

// The sentence of a proposal as (text, isNumber) parts: numbers go in the mono
// face, words in the body face. Built ONLY from typed fields -- there is no
// model text anywhere in a proposal.
using SentencePart = std::pair<juce::String, bool>;

std::vector<SentencePart> sentenceParts (const ShownProposal& sp)
{
    using Tool = AiProposal::Tool;
    const auto dash = juce::String (" ") + juce::String::charToString ((juce::juce_wchar) 0x2014) + " ";
    const auto hz = NotchListPanel::formatFrequency (sp.hz);

    std::vector<SentencePart> parts;
    switch (sp.proposal.tool)
    {
        case Tool::Deepen:
            parts = { { utf8 (kDeepen), false }, { hz, true }, { utf8 (kDeepenTo), false },
                      { NotchListPanel::formatDepthDb (sp.proposal.targetDb), true } };
            break;
        case Tool::Hold:
            parts = { { utf8 (kHold), false }, { hz, true }, { utf8 (kHoldMore), false },
                      { juce::String (sp.proposal.holdSeconds) + " s", true } };
            break;
        case Tool::Release:
            parts = { { utf8 (kRelease), false }, { hz, true } };
            break;
        case Tool::NoAction:
            return {};   // never shown
    }
    parts.push_back ({ dash, false });
    parts.push_back ({ CopilotPanel::reasonText (sp.proposal.reason), false });
    return parts;
}

float textLeft() { return kLeftPad + kDotSize + kDotGap; }

juce::Colour dotColour (const juce::String& severity)
{
    if (severity == "alert") return az::theme::danger;
    if (severity == "warn")  return az::theme::warn;
    return az::theme::dim;
}
} // namespace

//==============================================================================
class CopilotPanel::ListContent : public juce::Component
{
public:
    struct Row
    {
        juce::Colour dot;
        juce::String title;
        juce::String advice;
        juce::TextLayout layout;
        int titleH = 0;
        int height = 0;
    };

    void setBlock (const juce::String& text, const juce::String& footer, juce::Colour colour)
    {
        rows_.clear();
        hasBlock_ = true;
        blockText_ = text;
        blockFooter_ = footer;
        blockColour_ = colour;
    }

    juce::String blockText() const { return hasBlock_ ? blockText_ : juce::String(); }
    juce::String blockFooter() const { return hasBlock_ ? blockFooter_ : juce::String(); }

    void setFindings (const std::vector<CopilotFinding>& findings)
    {
        hasBlock_ = false;
        rows_.clear();
        for (const auto& f : findings)
        {
            Row r;
            r.dot    = dotColour (f.severity);
            r.title  = f.title;
            r.advice = f.advice;
            rows_.push_back (std::move (r));
        }
    }

    // Measures every row with the real font at the width it will be drawn at.
    void layoutForWidth (int width)
    {
        width_ = juce::jmax (1, width);
        const float adviceW = juce::jmax (1.0f, (float) width_ - textLeft() - kRightPad);

        int total = 0;

        if (hasBlock_)
        {
            juce::AttributedString as;
            as.setJustification (juce::Justification::topLeft);
            as.setWordWrap (juce::AttributedString::byWord);
            as.append (blockText_, az::theme::baseFont (kAdviceSize), blockColour_);
            blockLayout_.createLayout (as, juce::jmax (1.0f, (float) width_ - kLeftPad - kRightPad));

            blockTextH_ = (int) std::ceil (blockLayout_.getHeight());
            blockFooterH_ = blockFooter_.isEmpty()
                                ? 0
                                : juce::roundToInt (std::ceil (az::theme::monoFont (kFooterSize).getHeight()));
            total = kBlockPad + blockTextH_
                    + (blockFooterH_ > 0 ? kFooterGap + blockFooterH_ : 0) + kBlockPad;
        }

        for (auto& r : rows_)
        {
            juce::AttributedString as;
            as.setJustification (juce::Justification::topLeft);
            as.setWordWrap (juce::AttributedString::byWord);
            as.append (r.advice, az::theme::baseFont (kAdviceSize), az::theme::dim);
            r.layout.createLayout (as, adviceW);

            r.titleH = juce::roundToInt (std::ceil (az::theme::baseFont (kTitleSize).getHeight()));
            const int adviceH = (int) std::ceil (r.layout.getHeight());
            r.height = kRowPadTop + r.titleH + (adviceH > 0 ? kTitleGap + adviceH : 0) + kRowPadBottom;
            total += r.height;
        }

        setSize (width_, total);
        repaint();
    }

    int rowCount() const { return (int) rows_.size(); }
    int rowHeight (int i) const { return i >= 0 && i < rowCount() ? rows_[(size_t) i].height : 0; }
    // Findings rows or a text block (answer / error) are showing: the proposals
    // section must leave them their minimum room. An idle or clean list is empty.
    bool hasContent() const { return hasBlock_ || ! rows_.empty(); }

    void paint (juce::Graphics& g) override
    {
        using namespace az::theme;

        g.fillAll (panel);

        if (hasBlock_)
        {
            const float w = (float) width_ - kLeftPad - kRightPad;
            blockLayout_.draw (g, juce::Rectangle<float> (kLeftPad, (float) kBlockPad, w, (float) blockTextH_));

            if (blockFooterH_ > 0)
            {
                g.setColour (faded);
                g.setFont (monoFont (kFooterSize));
                g.drawText (blockFooter_,
                            juce::Rectangle<float> (kLeftPad, (float) (kBlockPad + blockTextH_ + kFooterGap),
                                                    w, (float) blockFooterH_),
                            juce::Justification::centredLeft, true);
            }
            return;
        }

        const auto clip = g.getClipBounds();
        int y = 0;

        for (const auto& r : rows_)
        {
            const int bottom = y + r.height;
            if (bottom >= clip.getY() && y <= clip.getBottom())
            {
                const float titleCentre = (float) (y + kRowPadTop) + (float) r.titleH * 0.5f;
                g.setColour (r.dot);
                g.fillEllipse (juce::Rectangle<float> (kDotSize, kDotSize)
                                   .withCentre ({ kLeftPad + kDotSize * 0.5f, titleCentre }));

                const float tx = textLeft();
                const float tw = (float) width_ - tx - kRightPad;

                g.setColour (text);
                g.setFont (baseFont (kTitleSize));
                g.drawText (r.title, juce::Rectangle<float> (tx, (float) (y + kRowPadTop), tw, (float) r.titleH),
                            juce::Justification::centredLeft, true);

                r.layout.draw (g, juce::Rectangle<float> (tx, (float) (y + kRowPadTop + r.titleH + kTitleGap),
                                                          tw, (float) (r.height - kRowPadTop - r.titleH - kTitleGap)));
            }

            g.setColour (shade);
            g.fillRect (0, bottom - 1, width_, 1);
            y = bottom;
        }
    }

private:
    std::vector<Row> rows_;
    int width_ = 1;

    bool hasBlock_ = false;
    juce::String blockText_, blockFooter_;
    juce::Colour blockColour_;
    juce::TextLayout blockLayout_;
    int blockTextH_ = 0, blockFooterH_ = 0;
};

//==============================================================================
// Lane L3a: the proposal rows. Each row is one fixed sentence plus two REAL
// buttons (the verdict-button pattern of the notch table: ghost style, mono
// face). The rows are rebuilt only when the list actually changes, so a click
// never lands on a button that a timer tick has just replaced; a rebuilt
// row's buttons are parked in `retired_` until the next rebuild, because BO
// removes its own row from inside the button's onClick.
class CopilotPanel::ProposalsContent : public juce::Component
{
public:
    struct Row
    {
        std::uint32_t id = 0;
        juce::String sentence;
        std::vector<SentencePart> parts;
        juce::TextLayout layout;
        std::unique_ptr<juce::TextButton> apply, dismiss;
        int height = 0;
    };

    std::function<void (std::uint32_t)> onDismiss;

    static int applyWidth()   { return juce::roundToInt (az::theme::stringWidth (az::theme::monoFont (az::theme::chipFontSize), utf8 (kApply))) + 18; }
    static int dismissWidth() { return juce::roundToInt (az::theme::stringWidth (az::theme::monoFont (az::theme::chipFontSize), utf8 (kDismiss))) + 18; }

    // True when `proposals` would build exactly the rows already showing.
    bool sameAs (const std::vector<ShownProposal>& proposals) const
    {
        if (proposals.size() != rows_.size())
            return false;
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].id != proposals[i].id || rows_[i].sentence != CopilotPanel::proposalSentence (proposals[i]))
                return false;
        return true;
    }

    void setProposals (const std::vector<ShownProposal>& proposals)
    {
        retired_.clear();   // buttons retired by the PREVIOUS rebuild: nothing is inside their onClick any more
        for (auto& r : rows_)
        {
            for (auto* b : { r.apply.get(), r.dismiss.get() })
            {
                removeChildComponent (b);
                b->setVisible (false);
            }
            retired_.push_back (std::move (r.apply));
            retired_.push_back (std::move (r.dismiss));
        }
        rows_.clear();

        for (const auto& sp : proposals)
        {
            Row r;
            r.id = sp.id;
            r.parts = sentenceParts (sp);
            r.sentence = CopilotPanel::proposalSentence (sp);

            r.apply = std::make_unique<juce::TextButton>();   // no tooltip argument: JUCE 9 trap
            r.apply->setButtonText (utf8 (kApply));
            r.apply->setTooltip (CopilotPanel::applyTooltipText());
            r.apply->setEnabled (false);                      // L3a: the control exists, it does nothing
            r.dismiss = std::make_unique<juce::TextButton>();
            r.dismiss->setButtonText (utf8 (kDismiss));
            const auto id = sp.id;
            r.dismiss->onClick = [this, id] { if (onDismiss != nullptr) onDismiss (id); };

            for (auto* b : { r.apply.get(), r.dismiss.get() })
            {
                b->getProperties().set (juce::Identifier ("azStyle"), "ghost");
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (*b);
            }
            rows_.push_back (std::move (r));
        }
    }

    // Measures every sentence with the real fonts at the width it will be drawn at.
    void layoutForWidth (int width)
    {
        width_ = juce::jmax (1, width);
        const int buttonsW = applyWidth() + kProposalButtonGap + dismissWidth();
        const float textW = juce::jmax (1.0f, (float) width_ - kLeftPad - (float) buttonsW
                                                 - (float) kProposalTextGap - kRightPad);
        int y = 0;
        for (auto& r : rows_)
        {
            juce::AttributedString as;
            as.setJustification (juce::Justification::topLeft);
            as.setWordWrap (juce::AttributedString::byWord);
            for (const auto& [t, isNumber] : r.parts)
                as.append (t, isNumber ? az::theme::monoFont (kAdviceSize - 1.0f) : az::theme::baseFont (kAdviceSize),
                           az::theme::text);
            r.layout.createLayout (as, textW);

            const int textH = (int) std::ceil (r.layout.getHeight());
            r.height = juce::jmax (textH, kProposalButtonH) + 2 * kProposalPadV;

            const int by = y + (r.height - kProposalButtonH) / 2;
            const int bx = width_ - (int) kRightPad - buttonsW;
            r.apply->setBounds (bx, by, applyWidth(), kProposalButtonH);
            r.dismiss->setBounds (bx + applyWidth() + kProposalButtonGap, by, dismissWidth(), kProposalButtonH);
            y += r.height;
        }
        setSize (width_, y);
        repaint();
    }

    int rowCount() const { return (int) rows_.size(); }
    juce::String sentence (int i) const { return i >= 0 && i < rowCount() ? rows_[(size_t) i].sentence : juce::String(); }
    juce::TextButton* applyButton (int i)   { return i >= 0 && i < rowCount() ? rows_[(size_t) i].apply.get() : nullptr; }
    juce::TextButton* dismissButton (int i) { return i >= 0 && i < rowCount() ? rows_[(size_t) i].dismiss.get() : nullptr; }

    void paint (juce::Graphics& g) override
    {
        using namespace az::theme;
        g.fillAll (panel);

        const auto clip = g.getClipBounds();
        const int buttonsW = applyWidth() + kProposalButtonGap + dismissWidth();
        const float tw = juce::jmax (1.0f, (float) width_ - kLeftPad - (float) buttonsW
                                               - (float) kProposalTextGap - kRightPad);
        int y = 0;
        for (const auto& r : rows_)
        {
            const int bottom = y + r.height;
            if (bottom >= clip.getY() && y <= clip.getBottom())
            {
                // Centred against the 22 px buttons when the sentence is one line.
                const float textH = r.layout.getHeight();
                const float ty = (float) y + ((float) r.height - textH) * 0.5f;
                r.layout.draw (g, juce::Rectangle<float> (kLeftPad, ty, tw, textH));
            }
            g.setColour (shade);
            g.fillRect (0, bottom - 1, width_, 1);
            y = bottom;
        }
    }

private:
    std::vector<Row> rows_;
    std::vector<std::unique_ptr<juce::TextButton>> retired_;
    int width_ = 1;
};

//==============================================================================
CopilotPanel::CopilotPanel()
    : content_ (std::make_unique<ListContent>()),
      modeSwitch_ (modeLabels()),
      proposalsContent_ (std::make_unique<ProposalsContent>())
{
    analyseButton_.setButtonText (utf8 (kButton));
    analyseButton_.setColour (juce::TextButton::textColourOffId, az::theme::accent);
    analyseButton_.setWantsKeyboardFocus (false);
    analyseButton_.onClick = [this]
    {
        if (state_ == State::running)
            return;
        if (onAnalyse != nullptr)
            onAnalyse();
    };
    addAndMakeVisible (analyseButton_);

    setupButton_.setButtonText (utf8 (kSetupButton));
    setupButton_.setColour (juce::TextButton::textColourOffId, az::theme::accent);
    setupButton_.setWantsKeyboardFocus (false);
    setupButton_.onClick = [this]
    {
        if (onOpenSetup != nullptr)
            onOpenSetup();
    };
    addAndMakeVisible (setupButton_);

    askEditor_.setMultiLine (false);
    askEditor_.setReturnKeyStartsNewLine (false);
    askEditor_.setFont (az::theme::baseFont (13.0f));
    askEditor_.setInputRestrictions (2000);
    askEditor_.setTextToShowWhenEmpty (utf8 (kPlaceholder), az::theme::faded);
    askEditor_.setColour (juce::TextEditor::backgroundColourId, az::theme::well);
    askEditor_.setColour (juce::TextEditor::outlineColourId, az::theme::border);
    askEditor_.setColour (juce::TextEditor::focusedOutlineColourId, az::theme::accent);
    askEditor_.setColour (juce::TextEditor::textColourId, az::theme::text);
    askEditor_.setColour (juce::TextEditor::highlightColourId, az::theme::accent.withAlpha (0.3f));
    askEditor_.onReturnKey = [this] { if (askEditor_.isEnabled()) submitQuestion(); };
    addAndMakeVisible (askEditor_);

    askButton_.setButtonText (utf8 (kAskButton));
    askButton_.setColour (juce::TextButton::textColourOffId, az::theme::accent);
    askButton_.setWantsKeyboardFocus (false);
    askButton_.onClick = [this]
    {
        if (state_ == State::asking)
        {
            if (onCancelAsk != nullptr)
                onCancelAsk();
        }
        else if (state_ != State::running)
        {
            submitQuestion();
        }
    };
    addAndMakeVisible (askButton_);

    viewport_.setViewedComponent (content_.get(), false);
    viewport_.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport_);

    // Lane L3a. The Act segment is shown but disabled: dimmed by the theme, no
    // hover or press, and the tooltip says why. Its click path can still be
    // driven programmatically, so the handler refuses it too.
    modeSwitch_.getSegmentForTest (kActSegment).setEnabled (false);
    modeSwitch_.getSegmentForTest (kActSegment).setTooltip (applyTooltipText());
    modeSwitch_.onSelected = [this] (int index)
    {
        if (index == kActSegment || ! juce::isPositiveAndBelow (index, kActSegment))
        {
            modeSwitch_.setSelectedIndex (segmentOf (aiMode_));   // refused: the mode does not change
            return;
        }
        const AiMode mode = index == 0 ? AiMode::Off : index == 1 ? AiMode::Shadow : AiMode::Suggest;
        aiMode_ = mode;
        reflectAiMode();
        if (onAiModeSelected != nullptr)
            onAiModeSelected (mode);
    };
    addAndMakeVisible (modeSwitch_);

    dismissAllButton_.setButtonText (utf8 (kDismissAll));
    dismissAllButton_.getProperties().set (juce::Identifier ("azStyle"), "ghost");
    dismissAllButton_.setWantsKeyboardFocus (false);
    dismissAllButton_.onClick = [this] { if (onDismissAllProposals != nullptr) onDismissAllProposals(); };
    addChildComponent (dismissAllButton_);

    proposalsContent_->onDismiss = [this] (std::uint32_t id) { if (onDismissProposal != nullptr) onDismissProposal (id); };
    proposalsViewport_.setViewedComponent (proposalsContent_.get(), false);
    proposalsViewport_.setScrollBarsShown (true, false);
    addChildComponent (proposalsViewport_);

    setOpaque (true);
    reflectAiMode();
    refreshStatus();
}

CopilotPanel::~CopilotPanel()
{
    viewport_.setViewedComponent (nullptr, false);
    proposalsViewport_.setViewedComponent (nullptr, false);
}

//==============================================================================
// Lane L3a
juce::String CopilotPanel::applyTooltipText() { return utf8 (kActTooltip); }

juce::String CopilotPanel::modeLabel (const AiMode mode)
{
    switch (mode)
    {
        case AiMode::Off:     return utf8 (kModeOff);
        case AiMode::Shadow:  return utf8 (kModeShadow);
        case AiMode::Suggest: return utf8 (kModeSuggest);
        case AiMode::Act:     return utf8 (kModeAct);
    }
    return {};
}

juce::String CopilotPanel::explainerFor (const AiMode mode)
{
    switch (mode)
    {
        case AiMode::Off:     return utf8 (kExplainOff);
        case AiMode::Shadow:  return utf8 (kExplainShadow);
        case AiMode::Suggest: return utf8 (kExplainSuggest);
        case AiMode::Act:     return utf8 (kExplainSuggest);   // not reachable in L3a (shown as Suggest)
    }
    return {};
}

juce::String CopilotPanel::reasonText (const AiProposal::Reason reason)
{
    using R = AiProposal::Reason;
    switch (reason)
    {
        case R::RecurringAtFrequency: return utf8 (kReasonRecurring);
        case R::StillRingingAtDepth:  return utf8 (kReasonStill);
        case R::ShowInProgress:       return utf8 (kReasonShow);
        case R::JudgedFalse:          return utf8 (kReasonFalse);
        case R::StaleNoRecurrence:    return utf8 (kReasonStale);
        case R::NothingToDo:
        case R::InsufficientData:
        case R::OutsideScope:         return {};   // no_action reasons: never displayed
    }
    return {};
}

juce::String CopilotPanel::proposalSentence (const ShownProposal& proposal)
{
    juce::String out;
    for (const auto& part : sentenceParts (proposal))
        out += part.first;
    return out;
}

void CopilotPanel::reflectAiMode()
{
    modeSwitch_.setSelectedIndex (segmentOf (aiMode_));
    explainerText_ = explainerFor (aiMode_);
    repaint (explainerBounds_);
}

void CopilotPanel::setAiMode (const AiMode mode)
{
    aiMode_ = mode == AiMode::Act ? AiMode::Suggest : mode;
    reflectAiMode();
}

void CopilotPanel::setSupervisorStatus (const juce::String& text)
{
    if (text == chipText_)
        return;
    chipText_ = text;
    layoutChip();
}

void CopilotPanel::setProposals (const std::vector<ShownProposal>& proposals)
{
    if (proposalsContent_->sameAs (proposals))
        return;
    proposalsContent_->setProposals (proposals);
    layoutList();
}

int CopilotPanel::getProposalRowCountForTest() const { return proposalsContent_->rowCount(); }
juce::String CopilotPanel::getProposalSentenceForTest (const int row) const { return proposalsContent_->sentence (row); }
juce::TextButton* CopilotPanel::getApplyButtonForTest (const int row) { return proposalsContent_->applyButton (row); }
juce::TextButton* CopilotPanel::getDismissButtonForTest (const int row) { return proposalsContent_->dismissButton (row); }

juce::String CopilotPanel::statusTextFor (const State s, const int count)
{
    switch (s)
    {
        case State::idle:           return utf8 (kIdle);
        case State::running:        return utf8 (kRunning);
        case State::findings:       return juce::String (count) + utf8 (kFindingsSuffix);
        case State::clean:          return utf8 (kClean);
        case State::noLog:          return utf8 (kNoLog);
        case State::sidecarMissing: return utf8 (kMissing);
        case State::timedOut:       return utf8 (kTimedOut);
        case State::failed:         return utf8 (kFailed);
        case State::asking:         return utf8 (kAsking);
        case State::answer:         return utf8 (kAnswered);
        case State::blocked:        return utf8 (kBlockedStatus);
        case State::askFailed:      return utf8 (kAskFailed);
        case State::needsSetup:     return utf8 (kNeedsSetup);
        case State::cancelled:      return utf8 (kCancelled);
        case State::busy:           return utf8 (kBusy);
    }
    return {};
}

void CopilotPanel::refreshStatus()
{
    statusText_ = statusTextFor (state_, findingCount_);
    const bool busy = state_ == State::running || state_ == State::asking;
    analyseButton_.setEnabled (! busy);

    // While a question is in flight the one button cancels it; while the
    // findings run, asking is off (MainComponent runs one process at a time).
    askButton_.setButtonText (utf8 (state_ == State::asking ? kCancelButton : kAskButton));
    askButton_.setEnabled (state_ != State::running);
    askEditor_.setEnabled (! busy);
    layoutList();
}

void CopilotPanel::submitQuestion()
{
    const auto question = askEditor_.getText().trim();
    if (question.isEmpty() || onAsk == nullptr)
        return;
    onAsk (question);
}

void CopilotPanel::setBlock (const juce::String& text, const juce::String& footer, const juce::Colour colour)
{
    content_->setBlock (text, footer, colour);
    content_->layoutForWidth (viewport_.getWidth() - viewport_.getScrollBarThickness());
    viewport_.setViewPosition (0, 0);
}

void CopilotPanel::setState (const State newState)
{
    state_ = newState;
    if (newState != State::findings)
    {
        findingCount_ = 0;
        content_->setFindings ({});
        content_->layoutForWidth (viewport_.getWidth() - viewport_.getScrollBarThickness());
    }
    if (newState == State::blocked)
        setBlock (utf8 (kBlockedBody), {}, az::theme::warn);
    refreshStatus();
}

void CopilotPanel::setAskFailed (const juce::String& reason)
{
    findingCount_ = 0;
    state_ = State::askFailed;
    setBlock (reason, {}, az::theme::warn);
    refreshStatus();
}

void CopilotPanel::setAnswer (const CopilotAnswer& a)
{
    findingCount_ = 0;
    switch (a.kind)
    {
        case CopilotAnswer::Kind::answer:
        {
            state_ = State::answer;
            juce::String tokens;
            if (a.inputTokens >= 0 || a.outputTokens >= 0)
                tokens = (a.inputTokens >= 0 ? juce::String (a.inputTokens) : juce::String ("?")) + " in / "
                         + (a.outputTokens >= 0 ? juce::String (a.outputTokens) : juce::String ("?")) + " out";

            juce::String footer = a.model;
            if (tokens.isNotEmpty())
                footer = footer.isEmpty() ? tokens : footer + " " + utf8 (kDot) + " " + tokens;

            setBlock (a.text, footer, az::theme::text);
            break;
        }
        case CopilotAnswer::Kind::blocked:
            state_ = State::blocked;
            setBlock (utf8 (kBlockedBody), {}, az::theme::warn);
            break;
        case CopilotAnswer::Kind::error:
            state_ = State::askFailed;
            setBlock (a.text, {}, az::theme::warn);
            break;
        case CopilotAnswer::Kind::invalid:
            state_ = State::askFailed;
            setBlock (utf8 (kInvalidReply), {}, az::theme::warn);
            break;
    }
    refreshStatus();
}

juce::String CopilotPanel::getBlockTextForTest() const { return content_->blockText(); }
juce::String CopilotPanel::getFooterTextForTest() const { return content_->blockFooter(); }
juce::String CopilotPanel::cautionTextForTest() { return utf8 (kCaution); }

void CopilotPanel::setFindings (std::vector<CopilotFinding> findings)
{
    findingCount_ = (int) findings.size();
    state_ = findings.empty() ? State::clean : State::findings;
    content_->setFindings (findings);
    content_->layoutForWidth (viewport_.getWidth() - viewport_.getScrollBarThickness());
    viewport_.setViewPosition (0, 0);
    refreshStatus();
}

int CopilotPanel::getRowCountForTest() const { return content_->rowCount(); }
int CopilotPanel::getRowHeightForTest (const int row) const { return content_->rowHeight (row); }
int CopilotPanel::getContentHeightForTest() const { return content_->getHeight(); }

// The explainer is measured with the real font at the panel's width, over ALL
// the texts it can show, so switching mode never makes the layout jump.
static int explainerHeightFor (const int width)
{
    int h = 0;
    for (const auto mode : { AiMode::Off, AiMode::Shadow, AiMode::Suggest })
    {
        juce::AttributedString as;
        as.setWordWrap (juce::AttributedString::byWord);
        as.append (CopilotPanel::explainerFor (mode), az::theme::baseFont (kExplainerSize), az::theme::dim);
        juce::TextLayout layout;
        layout.createLayout (as, (float) juce::jmax (1, width - (int) (kLeftPad + kRightPad)));
        h = juce::jmax (h, (int) std::ceil (layout.getHeight()));
    }
    return juce::jmax (CopilotPanel::kExplainerMinHeight, h + 2 * kExplainerPad);
}

void CopilotPanel::resized()
{
    auto area = getLocalBounds();
    captionBounds_ = area.removeFromTop (kCaptionHeight);
    auto caption = captionBounds_;

    const auto font = az::theme::legendFont (az::theme::columnFontSize, true, az::theme::trackingColumn);
    const auto buttonWidth = [&font] (const juce::String& t)
    {
        return juce::roundToInt (az::theme::stringWidth (font, t)) + 28;
    };

    caption.removeFromRight (juce::roundToInt (kRightPad));
    const int analyseW = buttonWidth (analyseButton_.getButtonText());
    analyseButton_.setBounds (caption.removeFromRight (analyseW)
                                     .withSizeKeepingCentre (analyseW, kButtonHeight));
    caption.removeFromRight (kButtonGap);
    const int setupW = buttonWidth (setupButton_.getButtonText());
    setupButton_.setBounds (caption.removeFromRight (setupW)
                                   .withSizeKeepingCentre (setupW, kButtonHeight));

    // The mode switch sits in the caption row, between the title and the two
    // buttons, when all four fit. At a width where they do not it gets its own
    // row under the title, so the title and its buttons never lose room to it.
    const int titleW = juce::roundToInt (az::theme::stringWidth (
                           az::theme::legendFont (az::theme::captionFontSize, true, az::theme::trackingCaption),
                           "COPILOT")) + 1;
    const int switchW = modeSwitch_.getPreferredWidth();
    const int roomForSwitch = caption.getWidth() - (int) kLeftPad - titleW - 2 * kChipGap;
    if (roomForSwitch >= switchW)
    {
        const int x = caption.getX() + (int) kLeftPad + titleW + kChipGap;
        modeSwitch_.setBounds (juce::Rectangle<int> (x, caption.getY(), switchW, caption.getHeight())
                                   .withSizeKeepingCentre (switchW, kSwitchHeight));
    }
    else
    {
        auto modeRow = area.removeFromTop (kModeRowHeight).withTrimmedLeft ((int) kLeftPad)
                                                          .withTrimmedRight ((int) kRightPad);
        const int w = juce::jmin (switchW, modeRow.getWidth());
        modeSwitch_.setBounds (modeRow.removeFromLeft (w).withSizeKeepingCentre (w, kSwitchHeight));
    }

    explainerBounds_ = area.removeFromTop (explainerHeightFor (getWidth()));
    statusBounds_ = area.removeFromTop (kStatusHeight);
    askRowBounds_ = area.removeFromBottom (kAskHeight);

    // The ask button keeps one width for both of its legends so it does not jump.
    auto row = askRowBounds_.withTrimmedLeft ((int) kLeftPad).withTrimmedRight ((int) kRightPad);
    const int askW = juce::jmax (buttonWidth (utf8 (kAskButton)), buttonWidth (utf8 (kCancelButton)));
    askButton_.setBounds (row.removeFromRight (askW).withSizeKeepingCentre (askW, kButtonHeight));
    row.removeFromRight (kButtonGap);
    askEditor_.setBounds (row.withSizeKeepingCentre (row.getWidth(), kButtonHeight));

    layoutChip();
    listArea_ = area;
    layoutList();
}

// The supervisor chip sits at the right end of the status row, in the secondary
// colour; the status sentence keeps the room to its left (it ellipsises first).
void CopilotPanel::layoutChip()
{
    auto row = statusBounds_.withTrimmedLeft ((int) kLeftPad).withTrimmedRight ((int) kRightPad);
    chipBounds_ = {};
    if (chipText_.isNotEmpty() && ! row.isEmpty())
    {
        const int wanted = juce::roundToInt (az::theme::stringWidth (az::theme::baseFont (kExplainerSize), chipText_)) + 4;
        const int w = juce::jmin (wanted, row.getWidth() / 2);
        chipBounds_ = row.removeFromRight (w);
        row.removeFromRight (kChipGap);
    }
    statusTextBounds_ = row;
    repaint (statusBounds_);
}

// The caution strip is OUTSIDE the scrolled content, so a long answer can
// neither push it out of the panel nor overlap the model/token footer. The
// proposals section (lane L3a) is carved off the TOP of the same area with a
// cap: the answer area keeps its minimum, and the proposals scroll when short.
void CopilotPanel::layoutList()
{
    auto area = listArea_;
    cautionBounds_ = {};
    proposalsArea_ = {};
    proposalsHeadingBounds_ = {};

    const int proposalCount = proposalsContent_->rowCount();
    if (proposalCount > 0 && area.getHeight() > kProposalHeadingHeight)
    {
        const int scrollbar = proposalsViewport_.getScrollBarThickness();
        proposalsContent_->layoutForWidth (area.getWidth() - scrollbar);
        const int contentH = proposalsContent_->getHeight();

        const int avail = area.getHeight();
        const int cap = content_->hasContent()
                            ? juce::jmax (kProposalHeadingHeight + kMinProposalsRows, avail - kMinListHeight)
                            : avail;   // nothing below to protect: the list may use the whole area
        const int wanted = juce::jmin (kProposalHeadingHeight + contentH, cap,
                                       content_->hasContent() ? avail - kMinMainListHeight : avail);
        if (wanted > kProposalHeadingHeight)
        {
            proposalsArea_ = area.removeFromTop (wanted);
            proposalsHeadingBounds_ = proposalsArea_.removeFromTop (kProposalHeadingHeight);
            // Same bounds, now scrolling: ScrollBar-wide gutter handled by the viewport.
            proposalsViewport_.setBounds (proposalsArea_);
            proposalsContent_->layoutForWidth (proposalsViewport_.getWidth() - scrollbar);
        }
    }
    const bool showProposals = ! proposalsArea_.isEmpty();
    proposalsViewport_.setVisible (showProposals);
    dismissAllButton_.setVisible (showProposals && proposalCount >= 2);
    if (showProposals)
    {
        const int w = juce::roundToInt (az::theme::stringWidth (az::theme::monoFont (az::theme::chipFontSize),
                                                                 utf8 (kDismissAll))) + 18;
        dismissAllButton_.setBounds (proposalsHeadingBounds_.withTrimmedRight ((int) kRightPad)
                                         .removeFromRight (w).withSizeKeepingCentre (w, kButtonHeight));
    }

    if (state_ == State::answer && ! area.isEmpty())
    {
        juce::AttributedString as;
        as.setWordWrap (juce::AttributedString::byWord);
        as.append (utf8 (kCaution), az::theme::baseFont (kCautionSize), az::theme::dim);
        juce::TextLayout layout;
        layout.createLayout (as, (float) juce::jmax (1, area.getWidth() - (int) (kLeftPad + kRightPad)));
        const int h = (int) std::ceil (layout.getHeight()) + 2 * kCautionPad;
        cautionBounds_ = area.removeFromBottom (juce::jmin (h, area.getHeight()));
    }
    viewport_.setBounds (area);
    content_->layoutForWidth (viewport_.getWidth() - viewport_.getScrollBarThickness());
    repaint();
}

void CopilotPanel::paint (juce::Graphics& g)
{
    using namespace az::theme;

    g.fillAll (panel);

    drawCaption (g, "COPILOT", captionBounds_.withTrimmedLeft ((int) kLeftPad), dim);
    drawEngravedDivider (g, captionBounds_.withTrimmedBottom (spacing));

    // Lane L3a: the one-line explainer under the title, in the secondary colour:
    // quiet, not a control.
    {
        juce::AttributedString as;
        as.setJustification (juce::Justification::topLeft);
        as.setWordWrap (juce::AttributedString::byWord);
        as.append (explainerText_, baseFont (kExplainerSize), dim);
        juce::TextLayout layout;
        const auto r = explainerBounds_.withTrimmedLeft ((int) kLeftPad).withTrimmedRight ((int) kRightPad)
                                       .withTrimmedTop (kExplainerPad);
        layout.createLayout (as, (float) juce::jmax (1, r.getWidth()));
        layout.draw (g, r.toFloat());
    }

    const bool problem = state_ == State::sidecarMissing || state_ == State::timedOut
                         || state_ == State::failed || state_ == State::blocked
                         || state_ == State::askFailed || state_ == State::needsSetup;
    g.setColour (problem ? warn : dim);
    g.setFont (baseFont (13.0f));
    g.drawText (statusText_, statusTextBounds_, juce::Justification::centredLeft, true);

    // The supervisor chip: text straight from AiSupervisor::statusText().
    if (chipText_.isNotEmpty() && ! chipBounds_.isEmpty())
    {
        g.setColour (dim);
        g.setFont (baseFont (kExplainerSize));
        g.drawText (chipText_, chipBounds_, juce::Justification::centredRight, true);
    }

    if (! proposalsArea_.isEmpty())
    {
        // The heading is drawn directly: drawCaption() upper-cases with the
        // locale's tables, which is not safe for Vietnamese letters.
        g.setColour (dim);
        g.setFont (legendFont (captionFontSize, true, trackingCaption));
        g.drawText (utf8 (kProposalsCaption), proposalsHeadingBounds_.withTrimmedLeft ((int) kLeftPad),
                    juce::Justification::centredLeft, false);
        g.setColour (shade);
        g.fillRect (proposalsArea_.getX(), proposalsArea_.getBottom() - 1, proposalsArea_.getWidth(), 1);
    }

    if (! cautionBounds_.isEmpty())
    {
        g.setColour (shade);
        g.fillRect (cautionBounds_.withHeight (1));
        const auto r = cautionBounds_.reduced ((int) kLeftPad, kCautionPad);
        juce::AttributedString as;
        as.setJustification (juce::Justification::topLeft);
        as.setWordWrap (juce::AttributedString::byWord);
        as.append (utf8 (kCaution), baseFont (kCautionSize), dim);
        juce::TextLayout layout;
        layout.createLayout (as, (float) juce::jmax (1, r.getWidth()));
        layout.draw (g, r.toFloat());
    }

    if (! askRowBounds_.isEmpty())
        drawEngravedDivider (g, juce::Rectangle<int> (0, askRowBounds_.getY() - spacing, getWidth(), spacing));
}

void applyCopilotResult (CopilotPanel& panel, const CopilotRunResult& result)
{
    using S = CopilotRunResult::Status;
    using P = CopilotPanel::State;

    switch (result.status)
    {
        case S::sidecarMissing: panel.setState (P::sidecarMissing); return;
        case S::timedOut:       panel.setState (P::timedOut);       return;
        case S::failed:         panel.setState (result.exitCode == 2 ? P::noLog : P::failed); return;
        case S::ok:
            if (auto parsed = parseCopilotFindings (result.stdoutText))
                panel.setFindings (std::move (*parsed));
            else
                panel.setState (P::failed);
            return;
    }
    panel.setState (P::failed);
}

void applyCopilotAskResult (CopilotPanel& panel, const CopilotRunResult& result)
{
    using S = CopilotRunResult::Status;
    using P = CopilotPanel::State;

    if (result.status == S::sidecarMissing) { panel.setState (P::sidecarMissing); return; }
    if (result.status == S::timedOut)       { panel.setState (P::timedOut);       return; }

    if (result.exitCode == 5) { panel.setAskFailed (utf8 (kBadSetup)); return; }
    if (result.exitCode == 2) { panel.setState (P::noLog);             return; }

    // The exit code decides first: a failing run never shows an answer, whatever
    // its stdout says.
    if (result.exitCode == 4) { panel.setState (P::blocked); return; }

    const auto parsed = parseCopilotAnswer (result.stdoutText);
    using K = CopilotAnswer::Kind;
    if (result.exitCode == 0)
    {
        if (parsed.kind == K::answer || parsed.kind == K::blocked)
            panel.setAnswer (parsed);
        else
            panel.setAnswer (CopilotAnswer {}); // kind invalid -> "invalid data"
        return;
    }

    // Exit 3 and any other failure: the reported error text, else a generic sentence.
    if (parsed.kind == K::error && parsed.text.isNotEmpty())
        panel.setAskFailed (parsed.text);
    else
        panel.setAskFailed (utf8 (kAskFailed));
}

} // namespace gui
