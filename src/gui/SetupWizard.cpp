#include "gui/SetupWizard.h"

#include "gui/theme/AzTheme.h"

#include <cmath>

namespace gui
{

namespace
{
    juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }

    const char* kTitle = "C\xc3\xa0i \xc4\x91\xe1\xba\xb7t";
    const char* kStep1 = "B\xc6\xb0\xe1\xbb\x9b" "c 1/3 \xe2\x80\x94 Sound card";
    const char* kStep2 = "B\xc6\xb0\xe1\xbb\x9b" "c 2/3 \xe2\x80\x94 Mic c\xe1\xba\xafm v\xc3\xa0o k\xc3\xaanh n\xc3\xa0o?";
    const char* kStep3 = "B\xc6\xb0\xe1\xbb\x9b" "c 3/3 \xe2\x80\x94 Ra loa \xe1\xbb\x9f k\xc3\xaanh n\xc3\xa0o?";
    const char* kHelp1 = "Ch\xe1\xbb\x8dn driver v\xc3\xa0 sound card \xc4\x91" "ang n\xe1\xbb\x91i v\xe1\xbb\x9bi mixer.";
    const char* kHelp1Wait = "\xc4\x90" "ang ch\xe1\xbb\x9d sound card g\xe1\xbb\xadi t\xc3\xadn hi\xe1\xbb\x87u\xe2\x80\xa6";
    const char* kHelp2 = "N\xc3\xb3i v\xc3\xa0o mic: thanh m\xe1\xbb\xa9" "c c\xe1\xbb\xa7" "a k\xc3\xaanh \xc4\x91\xc3\xbang s\xe1\xba\xbd nh\xe1\xba\xa3y.";
    const char* kHelp3 = "K\xc3\xaanh ra \xc4\x91\xc6\xb0" "a t\xc3\xadn hi\xe1\xbb\x87u \xc4\x91\xc3\xa3 ch\xe1\xbb\x91ng h\xc3\xba v\xe1\xbb\x81 mixer ho\xe1\xba\xb7" "c loa.";
    const char* kNext   = "TI\xe1\xba\xbeP";
    const char* kDone   = "XONG";
    const char* kBack   = "QUAY L\xe1\xba\xa0I";
    const char* kCancel = "HU\xe1\xbb\xb6";
    const char* kChannelUpper = "K\xc3\x8aNH";
    const char* kDriver = "Driver";
    const char* kDevice = "Sound card";
    const char* kLevel   = "M\xe1\xbb\xa9" "c v\xc3\xa0o";

    constexpr int   kMeterHz      = 30;
    constexpr float kMeterDecay   = 0.85f;   // per tick: ~0.5 s to fall 20 dB
    constexpr float kTitleSize    = 40.0f;
    constexpr float kStepSize     = 24.0f;
    constexpr float kHelpSize     = 17.0f;
    constexpr int   kChoiceHeight = 64;
    constexpr int   kChoiceWidth  = 200;
    constexpr int   kNavHeight    = 72;
    constexpr int   kNavWidth     = 200;
    constexpr int   kComboWidth   = 420;
}

SetupWizard::SetupWizard()
{
    setOpaque (true);

    addChildComponent (typeBox_);
    addChildComponent (deviceBox_);
    typeBox_.onChange = [this]
    {
        if (! filling_ && onDeviceChosen)
            onDeviceChosen (typeBox_.getText(), {});
    };
    deviceBox_.onChange = [this]
    {
        if (! filling_ && onDeviceChosen)
            onDeviceChosen (typeBox_.getText(), deviceBox_.getText());
    };

    for (auto* b : { &nextButton_, &backButton_, &cancelButton_ })
        addAndMakeVisible (*b);

    nextButton_.setButtonText (utf8 (kNext));
    backButton_.setButtonText (utf8 (kBack));
    cancelButton_.setButtonText (utf8 (kCancel));
    cancelButton_.getProperties().set ("azStyle", "ghost");

    nextButton_.onClick = [this]
    {
        if (! model_.canAdvance())
            return;

        if (model_.getStep() == SetupWizardModel::Step::Output)
        {
            // XONG. The caller may still ask the operator to confirm an
            // overwrite; closing is its call, after that answer.
            if (onFinish)
                onFinish (model_.result());
            return;
        }

        model_.next();
        showStep();
    };
    backButton_.onClick = [this] { model_.back(); showStep(); };
    cancelButton_.onClick = [this]
    {
        close();
        if (onCancel)
            onCancel();
    };

    setVisible (false);
}

SetupWizard::~SetupWizard()
{
    stopTimer();
}

void SetupWizard::open()
{
    if (isVisible())
        return;

    const auto counts = channelCountsProvider ? channelCountsProvider() : std::pair<int, int> {};
    model_.start (counts.first, counts.second);

    setVisible (true);
    toFront (false);
    refreshDevice();
    showStep();
    startTimerHz (kMeterHz);
}

void SetupWizard::close()
{
    stopTimer();
    setMeter (false);
    setVisible (false);
}

void SetupWizard::refreshDevice()
{
    filling_ = true;

    const auto fill = [] (juce::ComboBox& box, const juce::StringArray& items, const juce::String& current)
    {
        box.clear (juce::dontSendNotification);
        for (int i = 0; i < items.size(); ++i)
            box.addItem (items[i], i + 1);
        const int idx = items.indexOf (current);
        if (idx >= 0)
            box.setSelectedId (idx + 1, juce::dontSendNotification);
    };

    fill (typeBox_,   typeNamesProvider   ? typeNamesProvider()   : juce::StringArray {},
                      currentTypeProvider ? currentTypeProvider() : juce::String {});
    fill (deviceBox_, deviceNamesProvider ? deviceNamesProvider() : juce::StringArray {},
                      currentDeviceProvider ? currentDeviceProvider() : juce::String {});

    filling_ = false;

    const auto counts = channelCountsProvider ? channelCountsProvider() : std::pair<int, int> {};
    model_.setChannelCounts (counts.first, counts.second);
    meterLevels_.assign ((std::size_t) juce::jmax (0, counts.first), 0.0f);

    if (isVisible())
        showStep();
}

juce::String SetupWizard::labelFor (const SetupWizardModel::Choice& c)
{
    // Written UPPERCASE here: switch legends are uppercased by the look and
    // feel through towupper, which leaves "ê" alone outside a Vietnamese
    // locale; an already-uppercase "Ê" survives it.
    return c.width == 1
        ? utf8 (kChannelUpper) + " " + juce::String (c.channels[0] + 1)
        : utf8 (kChannelUpper) + " " + juce::String (c.channels[0] + 1) + " + "
              + juce::String (c.channels[1] + 1) + " STEREO";
}

void SetupWizard::rebuildChoices()
{
    for (auto& b : choiceButtons_)
        removeChildComponent (b.get());
    choiceButtons_.clear();

    const bool mic = model_.getStep() == SetupWizardModel::Step::Mic;
    if (! mic && model_.getStep() != SetupWizardModel::Step::Output)
        return;

    const auto choices = mic ? model_.micChoices() : model_.outputChoices();
    const int selected = mic ? model_.getMicIndex() : model_.getOutputIndex();

    for (int i = 0; i < (int) choices.size(); ++i)
    {
        // The default latching-switch style: a bordered face whether or not it
        // is picked, and the lamp on the picked one. (Segment style, tried
        // first, draws an unpicked choice as bare text -- the render showed
        // two of three choices reading as captions, not buttons.)
        auto b = std::make_unique<juce::TextButton> (labelFor (choices[(std::size_t) i]));
        b->setClickingTogglesState (false);
        b->setToggleState (i == selected, juce::dontSendNotification);
        b->setColour (juce::TextButton::buttonOnColourId, az::theme::accent);
        b->onClick = [this, i, mic]
        {
            if (mic) model_.chooseMic (i);
            else     model_.chooseOutput (i);
            showStep();
        };
        addAndMakeVisible (*b);
        choiceButtons_.push_back (std::move (b));
    }
}

void SetupWizard::showStep()
{
    const auto step = model_.getStep();
    typeBox_  .setVisible (step == SetupWizardModel::Step::Device);
    deviceBox_.setVisible (step == SetupWizardModel::Step::Device);

    setMeter (step == SetupWizardModel::Step::Mic);
    rebuildChoices();
    updateButtons();
    resized();
    repaint();
}

void SetupWizard::updateButtons()
{
    const bool last = model_.getStep() == SetupWizardModel::Step::Output;
    nextButton_.setButtonText (utf8 (last ? kDone : kNext));
    nextButton_.setEnabled (model_.canAdvance());
    backButton_.setVisible (model_.getStep() != SetupWizardModel::Step::Device);
}

void SetupWizard::setMeter (const bool on)
{
    if (on == meterOn_)
        return;
    meterOn_ = on;
    std::fill (meterLevels_.begin(), meterLevels_.end(), 0.0f);
    if (onMeterEnabled)
        onMeterEnabled (on);
}

void SetupWizard::timerCallback()
{
    // Step 1: a device that has just opened delivers its first callback a
    // moment later, and only then do the channel counts exist.
    if (model_.getStep() == SetupWizardModel::Step::Device)
    {
        const auto counts = channelCountsProvider ? channelCountsProvider() : std::pair<int, int> {};
        const bool could = model_.canAdvance();
        model_.setChannelCounts (counts.first, counts.second);
        if ((int) meterLevels_.size() != counts.first)
            meterLevels_.assign ((std::size_t) juce::jmax (0, counts.first), 0.0f);
        if (could != model_.canAdvance())
        {
            updateButtons();
            repaint (textArea_);
        }
        return;
    }

    if (! meterOn_ || ! inputPeakProvider)
        return;

    for (int ch = 0; ch < (int) meterLevels_.size(); ++ch)
    {
        const float peak = juce::jlimit (0.0f, 1.0f, inputPeakProvider (ch));
        auto& level = meterLevels_[(std::size_t) ch];
        level = juce::jmax (peak, level * kMeterDecay);
    }
    repaint (meterArea_);
}

void SetupWizard::paint (juce::Graphics& g)
{
    using namespace az::theme;
    g.fillAll (background);

    auto area = textArea_;
    g.setColour (text);
    g.setFont (legendFont (kTitleSize, true, 0.02f));
    g.drawText (utf8 (kTitle), area.removeFromTop (juce::roundToInt (kTitleSize * 1.3f)),
                juce::Justification::centredLeft, false);

    const auto step = model_.getStep();
    g.setColour (accent);
    g.setFont (legendFont (kStepSize, true, 0.02f));
    g.drawText (utf8 (step == SetupWizardModel::Step::Device ? kStep1
                    : step == SetupWizardModel::Step::Mic    ? kStep2
                                                             : kStep3),
                area.removeFromTop (juce::roundToInt (kStepSize * 1.6f)),
                juce::Justification::centredLeft, false);

    const bool waiting = step == SetupWizardModel::Step::Device && ! model_.canAdvance();
    g.setColour (waiting ? warn : dim);
    g.setFont (baseFont (kHelpSize));
    g.drawText (utf8 (step == SetupWizardModel::Step::Device ? (waiting ? kHelp1Wait : kHelp1)
                    : step == SetupWizardModel::Step::Mic    ? kHelp2
                                                             : kHelp3),
                area.removeFromTop (juce::roundToInt (kHelpSize * 1.8f)),
                juce::Justification::centredLeft, false);

    if (step == SetupWizardModel::Step::Device)
    {
        g.setColour (faded);
        g.setFont (legendFont (legendFontSize, true, trackingColumn));
        g.drawText (kDriver, typeBox_.getBounds().translated (0, -18).withHeight (16),
                    juce::Justification::bottomLeft, false);
        g.drawText (kDevice, deviceBox_.getBounds().translated (0, -18).withHeight (16),
                    juce::Justification::bottomLeft, false);
    }

    if (step != SetupWizardModel::Step::Mic || meterArea_.isEmpty())
        return;

    // One bar per input channel the callback delivers, labelled 1-based.
    auto meters = meterArea_;
    g.setColour (faded);
    g.setFont (legendFont (legendFontSize, true, trackingColumn));
    g.drawText (utf8 (kLevel), meters.removeFromTop (20), juce::Justification::centredLeft, false);

    const int rows = juce::jmax (1, (int) meterLevels_.size());
    const int rowH = juce::jmin (28, meters.getHeight() / rows);
    for (int ch = 0; ch < (int) meterLevels_.size(); ++ch)
    {
        auto row = meters.removeFromTop (rowH).reduced (0, 4);
        g.setColour (dim);
        g.setFont (monoFont (tableFontSize));
        g.drawText (juce::String (ch + 1), row.removeFromLeft (28), juce::Justification::centredLeft, false);

        auto bar = row.toFloat();
        drawWell (g, bar, false);
        const float level = meterLevels_[(std::size_t) ch];
        // dBFS -60..0 mapped onto the bar, so speech is clearly visible.
        const float db   = level > 0.001f ? 20.0f * std::log10 (level) : -60.0f;
        const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (level > 0.89f ? danger : ok);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * frac).reduced (2.0f), cornerRadius);
    }
}

void SetupWizard::resized()
{
    using namespace az::theme;

    auto area = getLocalBounds().reduced (6 * gap);

    auto nav = area.removeFromBottom (kNavHeight);
    nextButton_  .setBounds (nav.removeFromRight (kNavWidth));
    nav.removeFromRight (3 * gap);
    backButton_  .setBounds (nav.removeFromRight (kNavWidth));
    cancelButton_.setBounds (nav.removeFromLeft (kNavWidth / 2 + 20).withSizeKeepingCentre (kNavWidth / 2 + 20, touchTarget));

    textArea_ = area.removeFromTop (juce::roundToInt (kTitleSize * 1.3f + kStepSize * 1.6f + kHelpSize * 1.8f));
    area.removeFromTop (3 * gap);

    if (typeBox_.isVisible())
    {
        area.removeFromTop (20);   // caption, painted
        typeBox_  .setBounds (area.removeFromTop (touchTarget).removeFromLeft (kComboWidth));
        area.removeFromTop (2 * gap + 20);
        deviceBox_.setBounds (area.removeFromTop (touchTarget).removeFromLeft (kComboWidth));
    }

    // Choices flow left to right, wrapping.
    auto row = area.removeFromTop (kChoiceHeight);
    for (auto& b : choiceButtons_)
    {
        if (row.getWidth() < kChoiceWidth)
        {
            area.removeFromTop (2 * gap);
            row = area.removeFromTop (kChoiceHeight);
        }
        b->setBounds (row.removeFromLeft (kChoiceWidth));
        row.removeFromLeft (2 * gap);
    }

    area.removeFromTop (4 * gap);
    meterArea_ = area.removeFromTop (juce::jmin (area.getHeight(), 20 + 28 * juce::jmax (1, (int) meterLevels_.size())))
                     .removeFromLeft (kComboWidth + 200);
}

} // namespace gui
