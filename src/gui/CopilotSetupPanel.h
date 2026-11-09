// CopilotSetupPanel -- lane L2b (spec 2026-11-09-copilot-panel-design.md section 7,
// decision record Q12).
//
// A PASSIVE overlay that covers the left floor column while the owner sets up
// the "ask the AI" feature: provider, model, base URL, API key, and the consent
// to send the question + session log to the chosen provider. It is a child
// component, never a window or a dialog. It owns no key store, no settings
// file and no thread: MainComponent hands it values (setValues) and key status
// (setKeyStatus) and acts on its callbacks.
//
// KEY HYGIENE. The key editor is cleared the instant LUU KHOA is pressed, whether
// or not storing succeeds. The key text is handed to onStoreKey once and never
// kept: it is not in a label, a tooltip, a log, an assert, or a public getter.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace gui
{

struct CopilotSetupValues
{
    juce::String provider = "anthropic"; // "anthropic" | "openai_compatible"
    juce::String model;
    juce::String baseUrl;
    bool consent = false;
};

class CopilotSetupPanel : public juce::Component
{
public:
    CopilotSetupPanel();
    ~CopilotSetupPanel() override;

    /** Programmatic: no callback fires. Pass-through, nothing is prefilled. */
    void setValues (const CopilotSetupValues&);

    /** What the fields hold right now (never the key). */
    [[nodiscard]] CopilotSetupValues currentValues() const;

    /** `message` empty -> "key saved" / "no key" from `hasKey`; otherwise it is
        shown as a problem in the warn colour. */
    void setKeyStatus (bool hasKey, const juce::String& message = {});

    /** False off Windows: the key row is disabled with an explanation. */
    void setKeyStoreSupported (bool supported);

    /** Height the overlay needs at `width` so nothing is clipped. */
    [[nodiscard]] int preferredHeightForWidth (int width) const;

    // Fired only with VALID values (isValidCopilotModel / isValidCopilotBaseUrl),
    // on Return, on losing focus, on a provider switch and on the consent toggle.
    std::function<void (CopilotSetupValues)> onValuesChanged;
    std::function<void (juce::String provider, juce::String key)> onStoreKey;
    std::function<void (juce::String provider)> onDeleteKey;
    std::function<void (juce::String provider)> onProviderChanged;
    std::function<void()> onClose;

    static constexpr const char* kDefaultAnthropicModel = "claude-opus-5-5";

    // TEST ACCESSORS
    [[nodiscard]] juce::ComboBox& getProviderBoxForTest() { return providerBox_; }
    [[nodiscard]] juce::TextEditor& getModelEditorForTest() { return modelEditor_; }
    [[nodiscard]] juce::TextEditor& getUrlEditorForTest() { return urlEditor_; }
    [[nodiscard]] juce::TextEditor& getKeyEditorForTest() { return keyEditor_; }
    [[nodiscard]] juce::TextButton& getStoreKeyButtonForTest() { return storeKeyButton_; }
    [[nodiscard]] juce::TextButton& getDeleteKeyButtonForTest() { return deleteKeyButton_; }
    [[nodiscard]] juce::TextButton& getCloseButtonForTest() { return closeButton_; }
    [[nodiscard]] juce::ToggleButton& getConsentToggleForTest();
    [[nodiscard]] juce::String getKeyStatusTextForTest() const { return keyStatusText_; }
    [[nodiscard]] juce::String getModelReasonForTest() const { return modelReason_; }
    [[nodiscard]] juce::String getUrlReasonForTest() const { return urlReason_; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ConsentToggle;
    struct Plan;

    [[nodiscard]] juce::String providerName() const;
    void applyProviderEnablement();
    void refreshReasons();
    void emitIfValid();
    Plan computePlan (int width) const;

    juce::ComboBox providerBox_;
    juce::TextEditor modelEditor_, urlEditor_, keyEditor_;
    juce::TextButton storeKeyButton_, deleteKeyButton_, closeButton_;
    std::unique_ptr<ConsentToggle> consentToggle_;

    juce::String keyStatusText_, modelReason_, urlReason_;
    bool keyStatusIsProblem_ = false;
    bool keyStoreSupported_ = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CopilotSetupPanel)
};

} // namespace gui
