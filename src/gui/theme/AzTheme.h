// SODIUM RACK -- the one source of truth for this app's visual language.
// Every colour, metric and font the GUI draws with comes from here; a hex
// literal anywhere else in src/gui is a test failure (tests/test_aztheme.cpp).
//
// The direction, and why it is this and not something else
// ========================================================
// This is a front-of-house tool. It is read for about one second at a time,
// from six feet away, in a dark room, by someone whose hands are busy. So the
// vernacular is not "dark dashboard" -- it is the touring rack: powder-coated
// panels, silkscreened DIN legends, engraved grooves between sections, and
// illuminated latching switches big enough to hit without looking.
//
// THE ONE IDEA: a notch is born hot and cools as it holds. The instant the
// detector places one it is sodium amber; over the next ~22 seconds it fades
// to ice blue. The analyser therefore carries the history of the show at a
// glance -- amber stems mean the room is fighting right now, a field of quiet
// blue means it was solved and is holding. `ice` is used for NOTHING else, so
// it never degrades into decoration. notchColour() below is that ramp, and it
// is the only place the ramp exists.
//
// The palette values are written as ARGB integers (0xAARRGGBB), not as string
// literals, so the grep-test's "#RRGGBB" scan stays clean even in this file.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace az::theme
{

//==============================================================================
// Palette.
//
// juce::Colour's constructor is not constexpr in this JUCE version, so the
// constexpr tokens are the raw ARGB integers; the juce::Colour objects below
// are the readable aliases every consumer uses.
//
// The neutrals are warm-neutral graphite, NOT blue-black. That is a deliberate
// move off the default: blue-black plus one bright accent is the look every
// dark audio tool already has, and next to real gear it reads cold and screeny.

inline constexpr juce::uint32 backgroundArgb = 0xff0a0b0du; // window / canvas
inline constexpr juce::uint32 panelArgb      = 0xff131519u; // rack panel face
inline constexpr juce::uint32 raiseArgb      = 0xff1b1e24u; // raised / hover cell
inline constexpr juce::uint32 wellArgb       = 0xff0c0e11u; // recessed field (combos)
inline constexpr juce::uint32 borderArgb     = 0xff2b2f37u; // engraved hairline
inline constexpr juce::uint32 shadeArgb      = 0xff060709u; // the DARK half of a groove
inline constexpr juce::uint32 textArgb       = 0xffe8eaedu; // silkscreen white
inline constexpr juce::uint32 dimArgb        = 0xff868d98u; // secondary silkscreen
inline constexpr juce::uint32 fadedArgb      = 0xff5c636eu; // tertiary: units, row numbers
inline constexpr juce::uint32 accentArgb     = 0xffff9f1cu; // sodium vapour -- THE accent
inline constexpr juce::uint32 warnArgb       = 0xffffc24du; // caution
inline constexpr juce::uint32 okArgb         = 0xff6ee7a0u; // LED SIG green: PROTECTING only
inline constexpr juce::uint32 dangerArgb     = 0xffff5a4eu; // destructive / clipping
inline constexpr juce::uint32 traceArgb      = 0xffffb552u; // the signal itself: lit sodium
inline constexpr juce::uint32 gridArgb       = 0xff1d2128u; // analyser grid -- READ, not felt
inline constexpr juce::uint32 peakArgb       = 0xffdde6f0u; // peak-hold ceiling trace
inline constexpr juce::uint32 markerArgb     = 0xffff9f1cu; // a notch the instant it fires
inline constexpr juce::uint32 coolingArgb    = 0xffc9d1d9u; // the ramp's midpoint: pale steel
inline constexpr juce::uint32 settledArgb    = 0xff5fc9ffu; // a notch that has held: ICE

inline const juce::Colour background { backgroundArgb };
inline const juce::Colour panel      { panelArgb };
inline const juce::Colour raise      { raiseArgb };
inline const juce::Colour well       { wellArgb };
inline const juce::Colour border     { borderArgb };
inline const juce::Colour shade      { shadeArgb };
inline const juce::Colour text       { textArgb };
inline const juce::Colour dim        { dimArgb };
inline const juce::Colour faded      { fadedArgb };
inline const juce::Colour accent     { accentArgb };
inline const juce::Colour warn       { warnArgb };
inline const juce::Colour ok         { okArgb };
inline const juce::Colour danger     { dangerArgb };
inline const juce::Colour trace      { traceArgb };
inline const juce::Colour grid       { gridArgb };
inline const juce::Colour peak       { peakArgb };
inline const juce::Colour marker     { markerArgb };
inline const juce::Colour cooling    { coolingArgb };
inline const juce::Colour settled    { settledArgb };

// The light half of an engraved groove. A groove is one dark line with one
// faint light line under it -- the bevel that reads as "milled into the panel"
// rather than "a box drawn on top of it".
inline const juce::Colour sheen = juce::Colours::white.withAlpha (0.045f);

//==============================================================================
// Peak hold is the SAME signal, held -- so it is deliberately not given a
// saturated hue of its own. A cool near-white reads as a ceiling line above an
// amber trace without claiming to be a semantic state the way sodium, LED
// green, red and ice all do.

//==============================================================================
// THE RAMP. A notch's age in milliseconds -> its colour: sodium at 0, ice from
// kNotchCoolMs on, through pale steel in between. Used by the spectrum markers,
// the notch table dots and anything else that shows a notch, so all of them
// always agree.
//
// Why THREE stops and not two: a straight RGB interpolation from an orange to
// a cyan passes through their component-wise midpoint, which is an olive
// khaki. Rendered on the graphite ground it reads as a fault rather than as a
// notch halfway through cooling. Routing the ramp through a pale steel
// desaturates on the way across instead, so the midpoint reads as "cooling".

inline constexpr double kNotchCoolMs = 22000.0;

[[nodiscard]] juce::Colour notchColour (double ageMs);

// How hot a notch still is, 1.0 at birth to 0.0 once settled. Callers use it
// for glow strength and stem opacity so the fade is one curve, not three.
[[nodiscard]] float notchHeat (double ageMs);

//==============================================================================
// Metrics. Integers because layout arithmetic is int.

inline constexpr int   spacing          = 4;   // spacing grid unit
inline constexpr int   gap              = 8;   // gap between cells (2 x spacing)
inline constexpr int   touchTarget      = 44;  // minimum tappable size in px

// The transport switches. Sized like the latching switches they imitate:
// 168 x 62 is ~2.7x the area of a 44 px square target, which is what "hit it
// without looking, in the dark, while doing something else" actually costs.
inline constexpr int   buttonCellWidth  = 168;
inline constexpr int   buttonCellHeight = 62;
inline constexpr int   clearCellWidth   = 148;   // fits CLEAR ALL untruncated
inline constexpr int   railWidth        = 96;  // vertical-orientation cell width

inline constexpr int   mastheadHeight   = 38;
inline constexpr int   transportHeight  = 86;  // switch height + breathing room
inline constexpr int   lampHeight       = 3;   // the lit bar on an active switch
inline constexpr int   fieldHeight      = 26;  // combo / recessed field
inline constexpr int   captionHeight    = 30;  // section caption band

// THE LEGEND GUTTER. Every row of the rig column -- device, stream, response,
// notch, trigger, and the routing table's first control column -- starts its
// fields at this x. One alignment line down the whole stack is the difference
// between "a designed column" and "three panels that happen to be stacked",
// and it only works if every panel in the column reads the figure from here.
inline constexpr int   gutterWidth      = 74;

// The secondary legend that sits INLINE before a row's second field, where
// the value alone would be ambiguous ("8 samples" needs BUFFER; a device name
// does not need DEVICE).
inline constexpr int   inlineLegendWidth = 58;

// Both rows of a two-field grid split at the same fraction, so the four
// columns line up vertically. Rows that split differently are exactly what
// made the pre-rebuild device panel look ragged.
inline constexpr float fieldSplit       = 0.42f;

// Two radii, not one. A field is a machined slot and stays tight at 3; a
// switch is a moulded cap and reads softer at 4. Using one figure for both
// makes the switches look like oversized text boxes.
// Tracking, per element, transcribed from the design study (docs/spec-ui-mockup.md
// section 1). One global figure was wrong: a section caption is set wider than
// a column heading, and a switch legend tighter than either.
inline constexpr float trackingCaption  = 0.20f;  // section captions, brand
inline constexpr float trackingColumn   = 0.16f;  // column headings, small legends
inline constexpr float trackingSwitch   = 0.15f;  // transport switch legends

// Type sizes, same source. Named rather than typed at each call site so the
// scale can be checked against the spec table in one place.
// The study's figures were measured against a browser rendering Saira
// Condensed with its own hinting. Set at 11/10.5 in the app the same face came
// out noticeably smaller, so both are up one step. The RATIO between them --
// caption above column heading -- is what the study actually fixes.
// Raised again after a second look at the running app. Saira Condensed is a
// CONDENSED face: at the study's browser figures it renders both smaller and
// lighter than the study did, and the small legends came out reading as a grey
// haze rather than as words. The RATIOS are what the study fixes; the absolute
// sizes have to suit the face actually being drawn.
inline constexpr float captionFontSize  = 14.5f;   // section caption
inline constexpr float columnFontSize   = 13.0f;   // column heading

// A 168 x 62 switch carried a 16 px legend, which left it looking like a large
// button with small writing on it. The legend is the whole point of the cell.
inline constexpr float switchFontSize   = 21.0f;   // transport switch legend
inline constexpr float hintFontSize     = 11.0f;   // switch second line (mono)
inline constexpr float brandFontSize    = 16.0f;   // HANDS-FREE
inline constexpr float segmentFontSize  = 12.0f;   // toolbar segment (mono, sentence case)
inline constexpr float tableFontSize    = 12.5f;   // notch table cell (mono)
inline constexpr float readoutFontSize  = 12.0f;   // masthead rig line (mono)
inline constexpr float countdownFontSize = 26.0f;  // soundcheck number (mono medium)
inline constexpr float chipFontSize     = 12.5f;   // ghost chip (mono, sentence case)
inline constexpr float dangerFontSize   = 16.0f;   // CLEAR ALL

inline constexpr float cornerRadius     = 3.0f;   // fields, wells, chips
inline constexpr float switchRadius     = 4.0f;   // transport switches
inline constexpr float baseFontSize     = 14.0f;
inline constexpr float legendFontSize   = 12.0f;

//==============================================================================
// Fonts.
//
// legendFont() is the panel silkscreen: Bahnschrift IS DIN 1451, the typeface
// actually printed onto touring equipment, and it ships with every Windows
// this app targets (10 1709+). Set uppercase and tracked out by the callers.
//
// monoFont() is what every NUMBER (Hz, dB, countdown) is drawn with: digits
// must not shift width while they count. Cascadia Mono first for its modern
// figures, Consolas behind it for Windows 10, then Courier New.
//
// baseFont() is prose -- dialog copy, empty states.

[[nodiscard]] juce::Font baseFont   (float height = baseFontSize);
[[nodiscard]] juce::Font monoFont   (float height = baseFontSize, bool medium = false);
[[nodiscard]] juce::Font legendFont (float height = legendFontSize,
                                     bool  bold     = true,
                                     float tracking = trackingColumn);

//==============================================================================
// Shared drawing primitives. These exist so "an engraved divider" or "a
// recessed well" is drawn identically everywhere instead of being re-invented
// per component -- and so the hex literals stay in this folder.

// The bevel under a band: a dark line with a faint light line beneath it.
void drawEngravedDivider (juce::Graphics& g, juce::Rectangle<int> band);

// A recessed field: what a combo box, a value readout or a text well sits in.
void drawWell (juce::Graphics& g, juce::Rectangle<float> bounds, bool focused);

// The component property a caller sets to give a switch its second line:
//     button.getProperties().set (az::theme::hintProperty, "sweep the room");
// Drawn under the legend, quiet and small. A switch legend says WHAT the mode
// is called; the hint says what it does, which is the part a soundman who has
// not read a manual needs.
inline const juce::Identifier hintProperty { "azHint" };

// Width of `text` set in `font`. juce::Font lost getStringWidth in this JUCE
// version; GlyphArrangement is the replacement and this is the one wrapper.
[[nodiscard]] float stringWidth (const juce::Font& font, const juce::String& text);

// Shortens `text` to fit `maxWidth` by removing characters from the MIDDLE,
// never the end.
//
// A channel is called "Analogue 1", and the part that identifies it is the
// LAST character. Trailing truncation -- which is what every default does --
// throws away the only part that matters and leaves two ports looking
// identical. "Ana... 1" is readable; "Analogue" twice is not.
[[nodiscard]] juce::String elideMiddle (const juce::Font& font,
                                        const juce::String& text,
                                        float maxWidth);

// A section caption, in tracked uppercase silkscreen.
void drawCaption (juce::Graphics& g, const juce::String& caption,
                  juce::Rectangle<int> bounds, juce::Colour colour);

// Halos are NOT here on purpose. A glow is a real gaussian (melatonin_blur),
// and melatonin caches per shadow OBJECT -- so the object has to be a member
// of the component that draws it. A theme-level helper would construct one per
// call and throw the cache away every frame. See SpectrumView and StatusBadge.

//==============================================================================
// The LookAndFeel that installs the palette application-wide.

class AzLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AzLookAndFeel();

    // Buttons are switches: a raised cell, and when latched ON a lit bar
    // across the top edge plus a halo -- the state reads from across the room
    // rather than only from the label.
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // The routing table's per-slot enables. Drawn as a hardware toggle -- a
    // track with a travelling knob -- because a JUCE tick box is both tiny and
    // ambiguous at arm's length.
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawLabel (juce::Graphics&, juce::Label&) override;
    juce::Font getLabelFont (juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    juce::Font getPopupMenuFont() override;

    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override;
};

} // namespace az::theme
