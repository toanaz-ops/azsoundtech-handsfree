// The one source of truth for the "Console industrial" visual language
// (spec 2026-08-23 section 1). Every colour, metric and font the GUI draws
// with comes from here -- a hex literal anywhere else in src/gui is a test
// failure (tests/test_aztheme.cpp).
//
// The palette values below are written as ARGB integers (0xAARRGGBB), not as
// string literals, so the grep-test's "#RRGGBB" scan stays clean even here.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace az::theme
{

//==============================================================================
// Palette (spec section 1).
//
// juce::Colour's constructor is not constexpr in this JUCE version, so the
// constexpr tokens are the raw ARGB integers; the juce::Colour objects below
// are the readable aliases every consumer uses.

inline constexpr juce::uint32 backgroundArgb = 0xff0b0f14u; // window / canvas black
inline constexpr juce::uint32 panelArgb      = 0xff161e27u; // raised surfaces, buttons
inline constexpr juce::uint32 borderArgb     = 0xff232c36u; // hairline outlines
inline constexpr juce::uint32 textArgb       = 0xffc8d6e5u; // primary foreground
inline constexpr juce::uint32 dimArgb        = 0xff546e7au; // secondary foreground
inline constexpr juce::uint32 accentArgb     = 0xff4fc3f7u; // active / selected
inline constexpr juce::uint32 warnArgb       = 0xffff9800u; // warnings
inline constexpr juce::uint32 okArgb         = 0xff81c784u; // healthy states
inline constexpr juce::uint32 dangerArgb     = 0xffff8a65u; // destructive actions
inline constexpr juce::uint32 markerArgb     = 0xffffb74du; // spectrum notch markers

inline const juce::Colour background { backgroundArgb };
inline const juce::Colour panel      { panelArgb };
inline const juce::Colour border     { borderArgb };
inline const juce::Colour text       { textArgb };
inline const juce::Colour dim        { dimArgb };
inline const juce::Colour accent     { accentArgb };
inline const juce::Colour warn       { warnArgb };
inline const juce::Colour ok         { okArgb };
inline const juce::Colour danger     { dangerArgb };
inline const juce::Colour marker     { markerArgb };

//==============================================================================
// Metrics (spec section 1). Integers because layout arithmetic is int.

inline constexpr int   spacing          = 4;   // spacing grid unit
inline constexpr int   gap              = 8;   // gap between cells (2 x spacing)
inline constexpr int   touchTarget      = 44;  // minimum tappable size in px
inline constexpr int   railWidth        = 96;  // performance rail width
inline constexpr int   buttonCellWidth  = 96;
inline constexpr int   buttonCellHeight = 64;
inline constexpr float cornerRadius     = 4.0f;
inline constexpr float baseFontSize     = 13.0f;

//==============================================================================
// Fonts.
//
// baseFont() is the default UI face at the theme size. monoFont() is what
// every NUMBER (Hz, dB, countdown) is drawn with: digits must not shift width
// while they count, so a monospaced face is mandatory. Consolas ships with
// every Windows this app targets; the fallbacks cover non-Windows dev builds,
// and JUCE falls back to its default face if none of them exist.

[[nodiscard]] juce::Font baseFont();
[[nodiscard]] juce::Font monoFont (float height = baseFontSize);

//==============================================================================
// The LookAndFeel that installs the palette application-wide.

class AzLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AzLookAndFeel();

    // Points a component (and, through Component::getLookAndFeel(), its whole
    // child subtree) at this look and feel. After it returns,
    // dynamic_cast<AzLookAndFeel*>(&component->getLookAndFeel()) is non-null.
    void applyTo (juce::Component* component);

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    void drawLabel (juce::Graphics&, juce::Label&) override;

    juce::Font getLabelFont (juce::Label&) override;
};

//==============================================================================
// The entry point the brief names: AzTheme::applyTo(&component) installs the
// shared AzLookAndFeel on any component. MainComponent does not use this -- it
// owns its own AzLookAndFeel member so its lifetime is unambiguous.

struct AzTheme
{
    static void applyTo (juce::Component* component);
};

} // namespace az::theme
