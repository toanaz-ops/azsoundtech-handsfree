#include "gui/CopilotSetupPanel.h"

#include "app/SidecarRunner.h"
#include "gui/theme/AzTheme.h"

#include <algorithm>
#include <cmath>

namespace gui
{

namespace
{
// Display strings are escaped UTF-8 bytes: this project compiles without
// /utf-8 (same rule as CopilotPanel.cpp and DumbView.cpp).
const char* const kCaption = "C\xc3\x80I \xc4\x90\xe1\xba\xb6T AI";
const char* const kClose = "\xc4\x90\xc3\x93NG";
const char* const kLabelProvider = "NH\xc3\x80 CUNG C\xe1\xba\xa4P";
const char* const kLabelModel = "MODEL";
const char* const kLabelUrl = "\xc4\x90\xe1\xbb\x8a" "A CH\xe1\xbb\x88";
const char* const kLabelKey = "KHO\xc3\x81";
const char* const kProviderAnthropic = "Claude (Anthropic)";
const char* const kProviderOpenAi = "T\xc6\xb0\xc6\xa1ng th\xc3\xad" "ch OpenAI";
const char* const kStoreKey = "L\xc6\xafU KHO\xc3\x81";
const char* const kDeleteKey = "XO\xc3\x81 KHO\xc3\x81";
const char* const kKeySaved = "\xc4\x90\xc3\xa3 l\xc6\xb0u kho\xc3\xa1 cho nh\xc3\xa0 cung c\xe1\xba\xa5p n\xc3\xa0y.";
const char* const kKeyMissing = "Ch\xc6\xb0" "a c\xc3\xb3 kho\xc3\xa1.";
const char* const kKeyUnsupported = "C\xe1\xba\xa5t kho\xc3\xa1 ch\xe1\xbb\x89 c\xc3\xb3 tr\xc3\xaan Windows.";
const char* const kUrlHint = "V\xc3\xad d\xe1\xbb\xa5 https://api.openai.com/v1 ho\xe1\xba\xb7" "c http://localhost:11434/v1";
// "Chi dung cho Tuong thich OpenAI"
const char* const kUrlDisabledHint = "Ch\xe1\xbb\x89 d\xc3\xb9ng cho T\xc6\xb0\xc6\xa1ng th\xc3\xad" "ch OpenAI";
const char* const kConsent ="T\xc3\xb4i \xc4\x91\xe1\xbb\x93ng \xc3\xbd: c\xc3\xa2u h\xe1\xbb\x8fi v\xc3\xa0 d\xe1\xbb\xaf li\xe1\xbb\x87u log bu\xe1\xbb\x95i di\xe1\xbb\x85n (kh\xc3\xb4ng c\xc3\xb3 \xc3\xa2m thanh) s\xe1\xba\xbd g\xe1\xbb\xadi t\xe1\xbb\x9bi nh\xc3\xa0 cung c\xe1\xba\xa5p t\xc3\xb4i ch\xe1\xbb\x8dn.";
const char* const kKeyHint = "Kho\xc3\xa1 \xc4\x91\xc6\xb0\xe1\xbb\xa3" "c m\xc3\xa3 ho\xc3\xa1 theo t\xc3\xa0i kho\xe1\xba\xa3n Windows n\xc3\xa0y, kh\xc3\xb4ng n\xe1\xba\xb1m trong settings hay log.";
const char* const kModelReason = "Model ch\xe1\xbb\x89 g\xe1\xbb\x93m ch\xe1\xbb\xaf, s\xe1\xbb\x91 v\xc3\xa0 . _ : / - (t\xe1\xbb\x91i \xc4\x91" "a 128 k\xc3\xbd t\xe1\xbb\xb1).";
const char* const kUrlReason = "\xc4\x90\xe1\xbb\x8b" "a ch\xe1\xbb\x89 ph\xe1\xba\xa3i b\xe1\xba\xaft \xc4\x91\xe1\xba\xa7u b\xe1\xba\xb1ng http:// ho\xe1\xba\xb7" "c https://, kh\xc3\xb4ng c\xc3\xb3 kho\xe1\xba\xa3ng tr\xe1\xba\xafng.";

juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }

constexpr int   kPad         = 12;
constexpr int   kRowH        = 26;
constexpr int   kButtonH     = 22;
constexpr int   kVGap        = 8;
constexpr int   kReasonH     = 16;
constexpr int   kStatusH     = 18;
constexpr int   kLabelGap    = 8;
constexpr float kHintSize    = 12.0f;
constexpr float kBodySize    = 13.0f;
constexpr float kTrackW      = 32.0f;
constexpr float kTrackH      = 16.0f;
constexpr float kTrackInset  = 2.0f;
constexpr int   kToggleTextGap = 8;
constexpr int   kComboAnthropic = 1;
constexpr int   kComboOpenAi    = 2;

// Wrapped text, measured with the real font (same technique as the finding rows).
int measureWrapped (const juce::String& text, const juce::Font& font, int width)
{
    juce::AttributedString as;
    as.setJustification (juce::Justification::topLeft);
    as.setWordWrap (juce::AttributedString::byWord);
    as.append (text, font, az::theme::dim);

    juce::TextLayout layout;
    layout.createLayout (as, (float) juce::jmax (1, width));
    return (int) std::ceil (layout.getHeight());
}

void drawWrapped (juce::Graphics& g, const juce::String& text, const juce::Font& font,
                  juce::Colour colour, juce::Rectangle<int> area)
{
    juce::AttributedString as;
    as.setJustification (juce::Justification::topLeft);
    as.setWordWrap (juce::AttributedString::byWord);
    as.append (text, font, colour);

    juce::TextLayout layout;
    layout.createLayout (as, (float) juce::jmax (1, area.getWidth()));
    layout.draw (g, area.toFloat());
}

void styleEditor (juce::TextEditor& e, const juce::Font& font)
{
    e.setMultiLine (false);
    e.setReturnKeyStartsNewLine (false);
    e.setFont (font);
    e.setColour (juce::TextEditor::backgroundColourId, az::theme::well);
    e.setColour (juce::TextEditor::outlineColourId, az::theme::border);
    e.setColour (juce::TextEditor::focusedOutlineColourId, az::theme::accent);
    e.setColour (juce::TextEditor::textColourId, az::theme::text);
    e.setColour (juce::TextEditor::highlightColourId, az::theme::accent.withAlpha (0.3f));
}

juce::Font legend()
{
    return az::theme::legendFont (az::theme::columnFontSize, true, az::theme::trackingColumn);
}
} // namespace

//==============================================================================
// A hardware-style toggle like the theme's, but its legend WRAPS: the consent
// sentence is three lines in a narrow column and the stock drawToggleButton
// draws one line and clips it.
class CopilotSetupPanel::ConsentToggle : public juce::ToggleButton
{
public:
    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        using namespace az::theme;

        const bool on = getToggleState();
        const auto track = juce::Rectangle<float> (kTrackW, kTrackH)
                               .withCentre ({ kTrackInset + kTrackW * 0.5f, kTrackH * 0.5f + 3.0f });

        g.setColour (on ? accent.withAlpha (0.28f) : well);
        g.fillRoundedRectangle (track, kTrackH * 0.5f);
        g.setColour (on ? accent.withAlpha (0.8f) : (highlighted ? border.brighter (0.3f) : border));
        g.drawRoundedRectangle (track.reduced (0.5f), kTrackH * 0.5f, 1.0f);

        const float knobD = kTrackH - 2.0f * kTrackInset;
        const float knobX = on ? track.getRight() - kTrackInset - knobD : track.getX() + kTrackInset;
        g.setColour (on ? accent : dim);
        g.fillEllipse (knobX, track.getY() + kTrackInset, knobD, knobD);

        const int tx = (int) (track.getRight() + kToggleTextGap);
        drawWrapped (g, getButtonText(), baseFont (kBodySize), az::theme::text.withAlpha (isEnabled() ? 1.0f : 0.4f),
                     { tx, 0, getWidth() - tx, getHeight() });
    }

    int heightForWidth (int width) const
    {
        const int tx = (int) (kTrackInset + kTrackW + kToggleTextGap);
        return juce::jmax (kRowH, measureWrapped (getButtonText(), az::theme::baseFont (kBodySize), width - tx) + 4);
    }
};

//==============================================================================
struct CopilotSetupPanel::Plan
{
    juce::Rectangle<int> caption, closeButton, providerRow, modelRow, modelReason, urlRow, urlReason,
                         urlHint, keyRow, storeKeyButton, deleteKeyButton, keyStatus, consent, keyHint;
    int fieldX = 0;
    int total = 0;
};

CopilotSetupPanel::Plan CopilotSetupPanel::computePlan (const int width) const
{
    Plan p;

    int gutter = 0;
    for (const char* label : { kLabelProvider, kLabelModel, kLabelUrl, kLabelKey })
        gutter = juce::jmax (gutter, juce::roundToInt (az::theme::stringWidth (legend(), utf8 (label))));

    p.fieldX = kPad + gutter + 14;
    const int fieldW = juce::jmax (1, width - p.fieldX - kPad);

    int y = 0;
    p.caption = { 0, y, width, az::theme::captionHeight };
    const int closeW = juce::roundToInt (az::theme::stringWidth (legend(), utf8 (kClose))) + 28;
    p.closeButton = juce::Rectangle<int> (width - kPad - closeW, y + (p.caption.getHeight() - kButtonH) / 2,
                                          closeW, kButtonH);
    y += p.caption.getHeight() + kVGap;

    p.providerRow = { p.fieldX, y, fieldW, kRowH };
    y += kRowH + kVGap;

    p.modelRow = { p.fieldX, y, fieldW, kRowH };
    y += kRowH;
    p.modelReason = { p.fieldX, y, fieldW, kReasonH };
    y += kReasonH + 4;

    p.urlRow = { p.fieldX, y, fieldW, kRowH };
    y += kRowH;
    p.urlReason = { p.fieldX, y, fieldW, kReasonH };
    y += kReasonH;
    const int hintH = measureWrapped (utf8 (kUrlHint), az::theme::baseFont (kHintSize), fieldW);
    p.urlHint = { p.fieldX, y, fieldW, hintH };
    y += hintH + kVGap;

    p.keyRow = { p.fieldX, y, fieldW, kRowH };
    y += kRowH + 6;

    const auto buttonW = [] (const char* t) { return juce::roundToInt (az::theme::stringWidth (legend(), utf8 (t))) + 28; };
    p.storeKeyButton = { p.fieldX, y, buttonW (kStoreKey), kButtonH };
    p.deleteKeyButton = { p.storeKeyButton.getRight() + 6, y, buttonW (kDeleteKey), kButtonH };
    y += kButtonH + 6;

    p.keyStatus = { p.fieldX, y, fieldW, kStatusH };
    y += kStatusH + kVGap + 4;

    const int consentW = width - 2 * kPad;
    const int consentH = consentToggle_ != nullptr ? consentToggle_->heightForWidth (consentW) : kRowH;
    p.consent = { kPad, y, consentW, consentH };
    y += consentH + kVGap;

    const int keyHintH = measureWrapped (utf8 (kKeyHint), az::theme::baseFont (kHintSize), consentW);
    p.keyHint = { kPad, y, consentW, keyHintH };
    y += keyHintH + kPad;

    p.total = y;
    return p;
}

//==============================================================================
CopilotSetupPanel::CopilotSetupPanel()
    : consentToggle_ (std::make_unique<ConsentToggle>())
{
    providerBox_.addItem (utf8 (kProviderAnthropic), kComboAnthropic);
    providerBox_.addItem (utf8 (kProviderOpenAi), kComboOpenAi);
    providerBox_.setSelectedId (kComboAnthropic, juce::dontSendNotification);
    providerBox_.setWantsKeyboardFocus (false);
    providerBox_.onChange = [this]
    {
        const auto provider = providerName();
        // Claude has one sensible default; another provider's model is the owner's.
        if (provider == "anthropic" && ! isValidCopilotModel (modelEditor_.getText().trim()))
            modelEditor_.setText (kDefaultAnthropicModel, false);

        applyProviderEnablement();
        refreshReasons();
        if (onProviderChanged != nullptr)
            onProviderChanged (provider);
        emitIfValid();
    };
    addAndMakeVisible (providerBox_);

    const auto mono = az::theme::monoFont (az::theme::segmentFontSize);
    for (auto* e : { &modelEditor_, &urlEditor_ })
    {
        styleEditor (*e, mono);
        e->onTextChange = [this] { refreshReasons(); };
        e->onReturnKey = [this] { emitIfValid(); };
        e->onFocusLost = [this] { emitIfValid(); };
        addAndMakeVisible (*e);
    }

    styleEditor (keyEditor_, mono);
    keyEditor_.setPasswordCharacter ((juce::juce_wchar) 0x2022);
    keyEditor_.setInputRestrictions (4096);
    addAndMakeVisible (keyEditor_);

    const auto styleButton = [this] (juce::TextButton& b, const char* label)
    {
        b.setButtonText (utf8 (label));
        b.setColour (juce::TextButton::textColourOffId, az::theme::accent);
        b.setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    };
    styleButton (storeKeyButton_, kStoreKey);
    styleButton (deleteKeyButton_, kDeleteKey);
    styleButton (closeButton_, kClose);

    storeKeyButton_.onClick = [this]
    {
        // Take the key and clear the editor FIRST: whatever the store does next,
        // the secret is no longer on screen or in the editor's undo history.
        juce::String key = keyEditor_.getText();
        keyEditor_.clear();
        if (key.trim().isEmpty() || onStoreKey == nullptr)
            return;
        onStoreKey (providerName(), std::move (key));
    };
    deleteKeyButton_.onClick = [this]
    {
        if (onDeleteKey != nullptr)
            onDeleteKey (providerName());
    };
    closeButton_.onClick = [this]
    {
        if (onClose != nullptr)
            onClose();
    };

    consentToggle_->setButtonText (utf8 (kConsent));
    consentToggle_->setWantsKeyboardFocus (false);
    consentToggle_->onClick = [this] { emitIfValid(); };
    addAndMakeVisible (*consentToggle_);

    setOpaque (true);
    setKeyStatus (false);
    applyProviderEnablement();
    refreshReasons();
}

CopilotSetupPanel::~CopilotSetupPanel() = default;

juce::ToggleButton& CopilotSetupPanel::getConsentToggleForTest() { return *consentToggle_; }

juce::String CopilotSetupPanel::providerName() const
{
    return providerBox_.getSelectedId() == kComboOpenAi ? "openai_compatible" : "anthropic";
}

void CopilotSetupPanel::applyProviderEnablement()
{
    const bool openai = providerName() == "openai_compatible";
    urlEditor_.setEnabled (openai);
    // A disabled field must not be a blank box: say what it is for.
    urlEditor_.setTextToShowWhenEmpty (openai ? juce::String() : utf8 (kUrlDisabledHint), az::theme::dim);
    repaint();
}

void CopilotSetupPanel::setValues (const CopilotSetupValues& v)
{
    providerBox_.setSelectedId (v.provider == "openai_compatible" ? kComboOpenAi : kComboAnthropic,
                                juce::dontSendNotification);
    modelEditor_.setText (v.model, false);
    urlEditor_.setText (v.baseUrl, false);
    consentToggle_->setToggleState (v.consent, juce::dontSendNotification);
    applyProviderEnablement();
    refreshReasons();
}

CopilotSetupValues CopilotSetupPanel::currentValues() const
{
    CopilotSetupValues v;
    v.provider = providerName();
    v.model = modelEditor_.getText().trim();
    v.baseUrl = urlEditor_.getText().trim();
    v.consent = consentToggle_->getToggleState();
    return v;
}

void CopilotSetupPanel::refreshReasons()
{
    const auto v = currentValues();

    modelReason_ = isValidCopilotModel (v.model) ? juce::String() : utf8 (kModelReason);

    // The address only matters (and is only editable) for an OpenAI-compatible
    // endpoint; an empty one there is just as invalid as a malformed one.
    urlReason_ = (v.provider == "openai_compatible" && ! isValidCopilotBaseUrl (v.baseUrl))
                     ? utf8 (kUrlReason)
                     : juce::String();
    repaint();
}

void CopilotSetupPanel::emitIfValid()
{
    refreshReasons();
    if (modelReason_.isNotEmpty() || urlReason_.isNotEmpty() || onValuesChanged == nullptr)
        return;

    auto v = currentValues();
    if (v.provider != "openai_compatible" && ! isValidCopilotBaseUrl (v.baseUrl))
        v.baseUrl = {};   // an unused, malformed address is not worth saving
    onValuesChanged (std::move (v));
}

void CopilotSetupPanel::setKeyStatus (const bool hasKey, const juce::String& message)
{
    keyStatusIsProblem_ = message.isNotEmpty();
    keyStatusText_ = message.isNotEmpty() ? message : utf8 (hasKey ? kKeySaved : kKeyMissing);
    repaint();
}

void CopilotSetupPanel::setKeyStoreSupported (const bool supported)
{
    keyStoreSupported_ = supported;
    keyEditor_.setEnabled (supported);
    storeKeyButton_.setEnabled (supported);
    deleteKeyButton_.setEnabled (supported);
    if (! supported)
        setKeyStatus (false, utf8 (kKeyUnsupported));
    repaint();
}

int CopilotSetupPanel::preferredHeightForWidth (const int width) const
{
    return computePlan (width).total;
}

void CopilotSetupPanel::resized()
{
    const auto p = computePlan (getWidth());
    providerBox_.setBounds (p.providerRow);
    modelEditor_.setBounds (p.modelRow);
    urlEditor_.setBounds (p.urlRow);
    keyEditor_.setBounds (p.keyRow);
    storeKeyButton_.setBounds (p.storeKeyButton);
    deleteKeyButton_.setBounds (p.deleteKeyButton);
    closeButton_.setBounds (p.closeButton);
    consentToggle_->setBounds (p.consent);
}

void CopilotSetupPanel::paint (juce::Graphics& g)
{
    using namespace az::theme;

    g.fillAll (panel);
    g.setColour (border);
    g.drawRect (getLocalBounds(), 1);

    const auto p = computePlan (getWidth());

    drawCaption (g, utf8 (kCaption), p.caption.withTrimmedLeft (kPad), dim);
    drawEngravedDivider (g, p.caption.withTrimmedBottom (spacing));

    g.setColour (dim);
    g.setFont (legend());
    const auto label = [&g, &p] (const juce::Rectangle<int>& row, const char* legendText)
    {
        g.drawText (utf8 (legendText), juce::Rectangle<int> (kPad, row.getY(), p.fieldX - kPad - kLabelGap, row.getHeight()),
                    juce::Justification::centredLeft, true);
    };
    label (p.providerRow, kLabelProvider);
    label (p.modelRow, kLabelModel);
    label (p.urlRow, kLabelUrl);
    label (p.keyRow, kLabelKey);

    g.setColour (warn);
    g.setFont (baseFont (kHintSize));
    g.drawText (modelReason_, p.modelReason, juce::Justification::centredLeft, true);
    g.drawText (urlReason_, p.urlReason, juce::Justification::centredLeft, true);

    drawWrapped (g, utf8 (kUrlHint), baseFont (kHintSize), faded, p.urlHint);

    g.setColour (keyStatusIsProblem_ ? warn : dim);
    g.setFont (baseFont (kHintSize + 1.0f));
    g.drawText (keyStatusText_, p.keyStatus, juce::Justification::centredLeft, true);

    drawWrapped (g, utf8 (kKeyHint), baseFont (kHintSize), faded, p.keyHint);
}

} // namespace gui
