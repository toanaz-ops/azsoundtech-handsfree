#include "gui/CopilotPanel.h"

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
CopilotPanel::CopilotPanel()
    : content_ (std::make_unique<ListContent>())
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

    setOpaque (true);
    refreshStatus();
}

CopilotPanel::~CopilotPanel()
{
    viewport_.setViewedComponent (nullptr, false);
}

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
    repaint();
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

void CopilotPanel::resized()
{
    auto area = getLocalBounds();
    auto caption = area.removeFromTop (kCaptionHeight);
    area.removeFromTop (kStatusHeight);
    askRowBounds_ = area.removeFromBottom (kAskHeight);

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

    // The ask button keeps one width for both of its legends so it does not jump.
    auto row = askRowBounds_.withTrimmedLeft ((int) kLeftPad).withTrimmedRight ((int) kRightPad);
    const int askW = juce::jmax (buttonWidth (utf8 (kAskButton)), buttonWidth (utf8 (kCancelButton)));
    askButton_.setBounds (row.removeFromRight (askW).withSizeKeepingCentre (askW, kButtonHeight));
    row.removeFromRight (kButtonGap);
    askEditor_.setBounds (row.withSizeKeepingCentre (row.getWidth(), kButtonHeight));

    viewport_.setBounds (area);
    content_->layoutForWidth (viewport_.getWidth() - viewport_.getScrollBarThickness());
}

void CopilotPanel::paint (juce::Graphics& g)
{
    using namespace az::theme;

    g.fillAll (panel);

    auto area = getLocalBounds();
    auto caption = area.removeFromTop (kCaptionHeight);
    drawCaption (g, "COPILOT", caption.withTrimmedLeft ((int) kLeftPad), dim);
    drawEngravedDivider (g, caption.withTrimmedBottom (spacing));

    const auto status = area.removeFromTop (kStatusHeight);
    const bool problem = state_ == State::sidecarMissing || state_ == State::timedOut
                         || state_ == State::failed || state_ == State::blocked
                         || state_ == State::askFailed || state_ == State::needsSetup;
    g.setColour (problem ? warn : dim);
    g.setFont (baseFont (13.0f));
    g.drawText (statusText_, status.withTrimmedLeft ((int) kLeftPad).withTrimmedRight ((int) kRightPad),
                juce::Justification::centredLeft, true);

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
