#include "gui/SoundcheckPanel.h"

#include "gui/theme/AzTheme.h"

#include <cmath>

namespace gui
{

namespace
{
using namespace az::theme;

// The stripe down the left edge that says "this band is over the analyser, not
// part of it". Three pixels: a hairline reads as a border, anything wider reads
// as a second panel.
constexpr int kAccentBarWidth = 3;

// The caption line above the countdown number, same figure ModeRail reserves
// for its own caption (ModeRail.cpp kCountdownCaptionHeight).
constexpr int kCaptionHeight = 16;

// The countdown's own band. The number is the one thing in this strip read
// from across a room.
constexpr int kNumberHeight = 40;
constexpr float kNumberFontSize = 34.0f;

// Line height of one summary sentence, and the two type sizes it carries.
constexpr int   kSummaryLineHeight = SoundcheckPanel::kSummaryLineHeightForTest;
// Breathing room above the first line, so the headline does not sit on the
// panel's top edge when the strip is at its minimum height.
constexpr int   kSummaryTopPad     = 4;
constexpr float kHeadlineFontSize  = 15.5f;
constexpr float kSentenceFontSize  = 13.0f;

// EVERY string below is EXPLICIT UTF-8 BYTES. See the header.
//
// Where a byte escape is followed by a character that is itself a hex digit,
// the literal is SPLIT -- C++ hex escapes are greedy, so "\xa3c" would be read
// as one escape with three hex digits rather than as a byte and the letter c.
// That is a silent, compiling, wrong-bytes bug, so the splits are deliberate.

juce::String measuringCaption()        // "DANG DO"  (crossed D)
{
    return juce::String::fromUTF8 ("\xc4\x90" "ANG \xc4\x90O");
}

juce::String channelWord()             // "kenh"
{
    return juce::String::fromUTF8 ("k\xc3\xaanh");
}

juce::String middleDot()               // U+00B7
{
    return juce::String::fromUTF8 ("\xc2\xb7");
}

juce::String foundPrefix()             // "tim thay "
{
    return juce::String::fromUTF8 ("t\xc3\xacm th\xe1\xba\xa5y ");
}

juce::String hotSpotWord()             // " diem de hu"
{
    return juce::String::fromUTF8 (" \xc4\x91i\xe1\xbb\x83m d\xe1\xbb\x85 h\xc3\xba");
}

juce::String nothingFound()            // "khong tim thay diem de hu nao"
{
    return juce::String::fromUTF8 ("kh\xc3\xb4ng t\xc3\xacm th\xe1\xba\xa5y "
                                   "\xc4\x91i\xe1\xbb\x83m d\xe1\xbb\x85 h\xc3\xba "
                                   "n\xc3\xa0o");
}

juce::String unmeasuredSentence()      // " kenh khong do duoc"
{
    return juce::String::fromUTF8 (" " "k\xc3\xaanh kh\xc3\xb4ng \xc4\x91o "
                                   "\xc4\x91\xc6\xb0\xe1\xbb\xa3" "c");
}

juce::String misroutedSentence()       // " kenh sai dinh tuyen"
{
    return juce::String::fromUTF8 (" k\xc3\xaanh sai \xc4\x91\xe1\xbb\x8bnh "
                                   "tuy\xe1\xba\xbfn");
}

juce::String saturatedSentence()
{
    // " diem con vuot sau khi cat sau nhat -- chinh gain, ha tran, hoac doi vi tri mic"
    return juce::String::fromUTF8 (" \xc4\x91i\xe1\xbb\x83m c\xc3\xb2n "
                                   "v\xc6\xb0\xe1\xbb\xa3t sau khi c\xe1\xba\xaft "
                                   "s\xc3\xa2u nh\xe1\xba\xa5t \xe2\x80\x94 "
                                   "ch\xe1\xbb\x89nh gain, h\xe1\xba\xa1 "
                                   "tr\xe1\xba\xa7n, ho\xe1\xba\xb7" "c "
                                   "\xc4\x91\xe1\xbb\x95i v\xe1\xbb\x8b "
                                   "tr\xc3\xad mic");
}

juce::String clearedPrefix()           // "da xoa "
{
    return juce::String::fromUTF8 ("\xc4\x91\xc3\xa3 xo\xc3\xa1 ");
}

juce::String worseOffSentence()
{
    // " notch cu nhung khong dat duoc notch moi -- phong KEM an toan hon truoc"
    return juce::String::fromUTF8 (" notch c\xc5\xa9 nh\xc6\xb0ng kh\xc3\xb4ng "
                                   "\xc4\x91\xe1\xba\xb7t \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c "
                                   "notch m\xe1\xbb\x9bi \xe2\x80\x94 ph\xc3\xb2ng "
                                   "K\xc3\x89M an to\xc3\xa0n h\xc6\xa1n "
                                   "tr\xc6\xb0\xe1\xbb\x9b" "c");
}

juce::String placedSentence()          // " notch moi"
{
    return juce::String::fromUTF8 (" notch m\xe1\xbb\x9bi");
}

juce::String appliedPrefix()           // "da dat "
{
    return juce::String::fromUTF8 ("\xc4\x91\xc3\xa3 \xc4\x91\xe1\xba\xb7t ");
}

juce::String cannotProposeSentence()
{
    // " kenh thieu tran cat -- khong de xuat duoc"
    return juce::String::fromUTF8 (" k\xc3\xaanh thi\xe1\xba\xbfu "
                                   "tr\xe1\xba\xa7n c\xe1\xba\xaft \xe2\x80\x94 "
                                   "kh\xc3\xb4ng \xc4\x91\xe1\xbb\x81 xu\xe1\xba\xa5t "
                                   "\xc4\x91\xc6\xb0\xe1\xbb\xa3" "c");
}

// The width a legend needs in the font the LookAndFeel will actually draw it
// in, plus the padding drawButtonText() reduces the face by on each side.
int cellWidthFor (const juce::String& legend, const juce::Font& font, const int minimum)
{
    return juce::jmax (minimum,
                       (int) std::ceil (stringWidth (font, legend.toUpperCase()))
                           + 2 * gap);
}
} // namespace

//==============================================================================

SoundcheckPanel::SoundcheckPanel()
{
    using namespace az::theme;

    // Opaque: this band sits ON TOP of the analyser, and a translucent strip
    // over a live trace is a strip nobody can read the text on.
    setOpaque (true);

    // DUNG carries the app's established destructive/stop treatment -- the same
    // outline-that-fills CLEAR ALL uses -- so "the control that stops this" has
    // one look everywhere in the console rather than a new one per panel.
    stopButton.getProperties().set ("azStyle", "danger");
    stopButton.onClick = [this] { if (onStop != nullptr) onStop(); };
    addChildComponent (stopButton);

    // AP DUNG is the PRIMARY action, said with the accent rather than with a
    // bigger slab: it is the sodium the whole console reserves for "this is the
    // live thing". BO keeps the default dim legend, so the quiet answer looks
    // quiet -- an operator who is not sure should find BO the easier press.
    applyButton.setColour (juce::TextButton::textColourOffId, accent);
    applyButton.onClick = [this] { if (onApply != nullptr) onApply(); };
    addChildComponent (applyButton);

    dismissButton.onClick = [this] { if (onDismiss != nullptr) onDismiss(); };
    addChildComponent (dismissButton);

    // The countdown is a NUMBER: mono, so the digits do not shift width while
    // they count, and big, because it is read at a glance from across a room.
    countdownLabel_.setFont (monoFont (kNumberFontSize, true));
    countdownLabel_.setColour (juce::Label::textColourId, accent);
    countdownLabel_.setJustificationType (juce::Justification::centredLeft);
    addChildComponent (countdownLabel_);

    // MEASURED with the font each button will really be drawn in, not
    // estimated. stopButton is danger-styled, so its legend comes back at
    // dangerFontSize; the other two are plain cells over 40 px tall and get the
    // switch legend. Getting this wrong does not fail a test -- it ships a
    // truncated button (memory/stereo-lane-lessons-2026-09-05.md).
    const auto switchLegend = legendFont (switchFontSize, true, trackingSwitch);
    const auto dangerLegend = legendFont (dangerFontSize, true, trackingSwitch);

    // DUNG is a SAFETY control, so its floor is generous on purpose: it is the
    // only way an operator stops a signal that is already going into the PA.
    stopWidth_    = cellWidthFor (stopButton.getButtonText(),    dangerLegend, 180);
    applyWidth_   = cellWidthFor (applyButton.getButtonText(),   switchLegend, 168);
    dismissWidth_ = cellWidthFor (dismissButton.getButtonText(), switchLegend, 110);

    setProgress (0, 0, 0.0);
    rebuildSummary();
    setMode (Mode::Hidden);
}

//==============================================================================

void SoundcheckPanel::setMode (const Mode mode)
{
    mode_ = mode;

    const bool running = mode == Mode::Running;
    const bool results = mode == Mode::Results;
    const bool applied = mode == Mode::Applied;

    setVisible (mode != Mode::Hidden);

    stopButton     .setVisible (running);
    countdownLabel_.setVisible (running);
    applyButton    .setVisible (results);
    // BO is the only control an Applied report carries: there is nothing left
    // to apply, and the report still has to be dismissable -- an outcome the
    // operator cannot clear is an outcome that covers the analyser until the
    // app is restarted.
    dismissButton  .setVisible (results || applied);

    // Esc can only arrive at a component that has focus, so the panel asks for
    // it exactly while the run it can abort is in flight -- and gives it back
    // afterwards, so the preset name field and the rest of the console are not
    // fighting a hidden strip for keys (F14).
    setWantsKeyboardFocus (running);
    if (running)
        grabKeyboardFocus();

    // The Applied headline depends on the mode, so the summary is rebuilt here
    // rather than only in setResults -- a Model set before the mode changed
    // would otherwise still be printing the Results headline.
    rebuildSummary();
    resized();
    repaint();

    // Last, and after this panel is already consistent: the owner's resized()
    // will set new bounds on it, and it must not be doing that against a
    // half-applied mode.
    if (onModeChanged != nullptr)
        onModeChanged();
}

void SoundcheckPanel::setProgress (const int channelIndex, const int channelCount,
                                   const double remainingMs)
{
    // 1-BASED for the human: "channel 0 of 4" is not a thing anybody counts.
    // Clamped rather than trusted, so a stale index from a run that has already
    // finished cannot print "channel 5/4".
    const int shown = channelCount > 0
                          ? juce::jlimit (1, channelCount, channelIndex + 1)
                          : 0;

    progressCaption_ = channelCount > 0
                           ? measuringCaption() + " " + middleDot() + " " + channelWord()
                                 + " " + juce::String (shown) + "/" + juce::String (channelCount)
                           : measuringCaption();

    // Whole seconds, rounded UP: a countdown that reads 0 while sound is still
    // coming out of the PA is a countdown that has lied.
    const int seconds = (int) std::ceil (juce::jmax (0.0, remainingMs) / 1000.0);
    countdownLabel_.setText (juce::String (seconds) + " s", juce::dontSendNotification);

    repaint();
}

void SoundcheckPanel::setResultsSummary (const int hotSpots, const int saturatedBins,
                                         const int unmeasured, const int routingInvalid)
{
    Model model;
    model.hotSpots       = hotSpots;
    model.saturatedBins  = saturatedBins;
    model.unmeasured     = unmeasured;
    model.routingInvalid = routingInvalid;
    setResults (model);
}

int SoundcheckPanel::preferredHeight() const
{
    using namespace az::theme;

    if (mode_ == Mode::Running)
        return kPanelHeight;

    // Every line it is holding, plus the panel's own padding. NEVER the other
    // way round -- a height chosen first and lines fitted into it afterwards is
    // how a fault sentence gets cut in half, and the two sentences most likely
    // to be last are the two that ask the operator to go and fix something.
    const int needed = 2 * spacing
                     + (int) summaryLineCount_ * kSummaryLineHeight
                     + kSummaryTopPad;

    return juce::jmax (kPanelHeight, needed);
}

void SoundcheckPanel::setResults (const Model& model)
{
    model_ = model;
    rebuildSummary();
    resized();      // the line count changed, so the block it occupies did too
    repaint();
}

//==============================================================================

void SoundcheckPanel::addSummaryLine (const juce::String& text, const juce::Colour colour)
{
    if (summaryLineCount_ >= kMaxSummaryLines)
        return;

    summaryLines_[summaryLineCount_].text   = text;
    summaryLines_[summaryLineCount_].colour = colour;
    ++summaryLineCount_;
}

void SoundcheckPanel::rebuildSummary()
{
    using namespace az::theme;

    summaryLineCount_ = 0;

    // ceilingMissing / ladderMissing mean pick() had nothing to propose FROM --
    // a fault in the run's inputs, not a verdict about the room. It is an
    // ERROR, and it is what replaces the headline when there is nothing else to
    // put there: "0 hot spots" on a run that judged nothing reads as an
    // excellent PA (Task 3 I-3).
    hasError_ = model_.cannotPropose > 0 || model_.routingInvalid > 0 || model_.worseOff();

    // THE line that must never be silent, and it goes FIRST: an apply that
    // removed notches and placed none left the room worse than it found it.
    if (model_.worseOff())
        addSummaryLine (clearedPrefix() + juce::String (model_.clearedPrevious)
                            + worseOffSentence(),
                        danger);
    else if (mode_ == Mode::Applied)
        addSummaryLine (appliedPrefix() + juce::String (model_.placed) + placedSentence(),
                        accent);

    if (model_.hotSpots > 0)
        addSummaryLine (foundPrefix() + juce::String (model_.hotSpots) + hotSpotWord(),
                        accent);
    else if (! hasError_ && model_.unmeasured == 0 && model_.saturatedBins == 0
             && mode_ != Mode::Applied)
        // "no hot spots" is only the same thing as "clean room" when nothing
        // ELSE went wrong. A saturated bin means the run found something a
        // filter cannot fix, which is the opposite of a clean room -- round 1
        // still printed the clean-room line beside it.
        addSummaryLine (nothingFound(), dim);

    if (model_.cannotPropose > 0)
        addSummaryLine (juce::String (model_.cannotPropose) + cannotProposeSentence(),
                        danger);

    // A bin still over after the DEEPEST rung on the ladder cannot be fixed by
    // another filter, so the sentence names what CAN fix it. A bare count would
    // leave the operator reaching for the one control that will not help.
    if (model_.saturatedBins > 0)
        addSummaryLine (juce::String (model_.saturatedBins) + saturatedSentence(),
                        warn);

    // Its own sentence, never folded into the headline: "could not measure" is
    // a VALID result (Q8) and asks the operator to look at the mic and the
    // gain, which is not what a hot-spot count asks for.
    if (model_.unmeasured > 0)
        addSummaryLine (juce::String (model_.unmeasured) + unmeasuredSentence(), warn);

    // And its own sentence again, separate from unmeasured (F26): a wrong patch
    // is fixed at the desk, not in the room.
    if (model_.routingInvalid > 0)
        addSummaryLine (juce::String (model_.routingInvalid) + misroutedSentence(), danger);
}

juce::String SoundcheckPanel::summaryTextForTest() const
{
    juce::StringArray lines;
    for (std::size_t i = 0; i < summaryLineCount_; ++i)
        lines.add (summaryLines_[i].text);
    return lines.joinIntoString ("\n");
}

//==============================================================================

bool SoundcheckPanel::keyPressed (const juce::KeyPress& key)
{
    // Esc aborts a RUN and nothing else. In Results the two answers are AP DUNG
    // and BO, and an Esc that silently picked one of them would be a guess
    // about which -- one of those two throws a set of proposals away.
    if (mode_ == Mode::Running && key == juce::KeyPress (juce::KeyPress::escapeKey))
    {
        if (onStop != nullptr)
            onStop();
        return true;
    }

    // Everything else is REFUSED, so the key still reaches whatever else in the
    // console wants it.
    return false;
}

//==============================================================================

void SoundcheckPanel::paint (juce::Graphics& g)
{
    using namespace az::theme;

    g.fillAll (panel);

    // The stripe: sodium while a run is live, dim once it is only a report.
    if (! accentBar_.isEmpty())
    {
        g.setColour (mode_ == Mode::Running ? accent : dim);
        g.fillRect (accentBar_);
    }

    g.setColour (border);
    g.drawRect (getLocalBounds(), 1);

    if (mode_ == Mode::Running)
    {
        if (! captionArea_.isEmpty())
            drawCaption (g, progressCaption_, captionArea_, dim);

        return;
    }

    if (summaryArea_.isEmpty())
        return;

    // The summary is PAINTED rather than pushed into a Label because its lines
    // carry DIFFERENT colours -- an error is not the same red-orange as a
    // warning, and one Label cannot be two colours. Geometry comes from
    // resized(); nothing here computes a layout.
    // NO BOUNDS CHECK, deliberately, and this is the point of preferredHeight():
    // a fault sentence is never dropped or half-drawn to make the strip fit.
    // The owner is told how much room the lines need and gives it; if the
    // console is so short that even that is impossible, the strip keeps its
    // height and the ANALYSER goes under its floor instead. Round 1 had a
    // `break` here, which silently swallowed whichever sentence was last --
    // and the last two are the ones asking the operator to fix a patch.
    auto row = summaryArea_.withHeight (kSummaryLineHeight);
    for (std::size_t i = 0; i < summaryLineCount_; ++i)
    {
        // The first line is THE answer and is set one step up from the
        // qualifications under it, for the same reason the countdown is bigger
        // than its caption: one is read across the room, the rest are read
        // once somebody has walked over.
        g.setFont (baseFont (i == 0 ? kHeadlineFontSize : kSentenceFontSize));
        g.setColour (summaryLines_[i].colour);
        g.drawText (summaryLines_[i].text, row, juce::Justification::centredLeft, false);
        row.translate (0, kSummaryLineHeight);
    }
}

void SoundcheckPanel::resized()
{
    using namespace az::theme;

    auto area = getLocalBounds().reduced (gap, spacing);

    if (area.getWidth() <= 0 || area.getHeight() <= 0)
    {
        captionArea_ = summaryArea_ = accentBar_ = {};
        countdownLabel_.setBounds ({});
        return;
    }

    accentBar_ = area.removeFromLeft (kAccentBarWidth);
    area.removeFromLeft (gap);

    const int buttonH = juce::jmin (area.getHeight(), buttonCellHeight);

    // The controls are carved off the RIGHT before anything else claims space,
    // so the text block can never run underneath them -- a results line half
    // covered by AP DUNG is a line nobody reads before pressing it.
    auto placeRight = [&area, buttonH] (juce::Component& c, int width)
    {
        const auto cell = area.removeFromRight (juce::jmin (width, area.getWidth()));
        c.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), buttonH));
        area.removeFromRight (gap);
    };

    if (mode_ == Mode::Running)
    {
        placeRight (stopButton, stopWidth_);

        captionArea_ = area.removeFromTop (kCaptionHeight);
        countdownLabel_.setBounds (area.removeFromTop (juce::jmin (area.getHeight(),
                                                                   kNumberHeight)));
        summaryArea_ = {};
        return;
    }

    placeRight (dismissButton, dismissWidth_);
    placeRight (applyButton,   applyWidth_);

    captionArea_ = {};
    countdownLabel_.setBounds ({});
    summaryArea_  = area.withTrimmedTop (kSummaryTopPad);
}

} // namespace gui
