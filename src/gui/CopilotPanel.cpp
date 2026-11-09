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

    void setFindings (const std::vector<CopilotFinding>& findings)
    {
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
    }
    return {};
}

void CopilotPanel::refreshStatus()
{
    statusText_ = statusTextFor (state_, findingCount_);
    analyseButton_.setEnabled (state_ != State::running);
    repaint();
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
    refreshStatus();
}

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

    const auto font = az::theme::legendFont (az::theme::columnFontSize, true, az::theme::trackingColumn);
    const int buttonW = juce::roundToInt (az::theme::stringWidth (font, analyseButton_.getButtonText())) + 28;

    caption.removeFromRight (juce::roundToInt (kRightPad));
    analyseButton_.setBounds (caption.removeFromRight (buttonW)
                                     .withSizeKeepingCentre (buttonW, kButtonHeight));

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
                         || state_ == State::failed;
    g.setColour (problem ? warn : dim);
    g.setFont (baseFont (13.0f));
    g.drawText (statusText_, status.withTrimmedLeft ((int) kLeftPad).withTrimmedRight ((int) kRightPad),
                juce::Justification::centredLeft, true);
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

} // namespace gui
