---
name: juce-ui-styles-store
description: JUCE UI theming patterns from José Proença's "Journey into audio programming #10" — centralised StylesStore, LookAndFeel_V4 colour schemes, and why to avoid scattering hex literals across paint() methods. Use when styling JUCE components, designing a theme layer, customising LookAndFeel drawing methods, or deciding where colours/sizes/fonts should live.
allowed-tools: Read, Grep, Glob
source: https://medium.com/@akaztp/journey-into-audio-programming-10-customize-the-ui-940c3df6a800
---

# JUCE UI Customization — the StylesStore Pattern

Distilled from José Proença's article (Nov 2024, series "Journey into audio
programming #10"). Attribution and link above; code snippets below are
adapted, not copied verbatim.

## The problem

Styling a JUCE app touches three places:

1. **Layout** — editor classes and grouping components need margins/gutters.
2. **JUCE widgets** — buttons, combos, sliders take colours via `colourId`s.
3. **Custom components** — your own `paint()` needs colours no `colourId` covers.

Without one home for style values you get hex literals scattered through
`paint()` methods — impossible to retheme, easy to drift.

## Why extending only `LookAndFeel_V4` is not enough

`LookAndFeel_V4::ColourScheme` is a fixed enum (`windowBackground`,
`widgetBackground`, ...) — you cannot add entries for domain-specific colours
like "waveform signal" or "notch marker". `initialiseColours()` is not
virtual. Component `colourId`s are plain ints and can collide. So the
article keeps `LookAndFeel_V4` for widget chrome but adds a separate store
for ALL values.

## The pattern

One store, typed enums, fixed-size arrays — fast lookup, no string keys:

```cpp
class StylesStore
{
public:
    enum class ColorIds  { WindowBackground = 0, WidgetBackground,
                           Outline, DefaultText, HighlightedFill,
                           WaveformSignal, WaveformZero, numIds };
    enum class NumberIds { LayoutMargin = 0, LayoutGutter,
                           CornerRadius, KeyboardHeight, numIds };
    enum class TextualIds{ FontName = 0, numIds };

    juce::Colour getColor   (ColorIds id)   const;
    float        getNumber  (NumberIds id)  const;
    juce::String getTextual (TextualIds id) const;
    void fillStore (/* maps of values */, juce::LookAndFeel_V4& laf);
private:
    juce::Colour colors [static_cast<int> (ColorIds::numIds)] {};
    float        numbers[static_cast<int> (NumberIds::numIds)] {};
    juce::String textuals[static_cast<int> (TextualIds::numIds)] {};
};
```

Key decisions and their rationale:

- **Enums over dynamic string keys**: integer compare is faster, and paint()
  code hardcodes names anyway — dynamic keys buy nothing at runtime.
- **Type-split getters**: every call site knows the type it wants; one
  variant union would force casts everywhere.
- **Values live OUTSIDE the store** (in a `Stylesheet` — static maps or a
  file loader later): the store is storage only, so switching themes or
  loading styles from disk never touches call sites.
- **`fillStore()` also writes the LookAndFeel colour scheme**, mapping the
  first N ColorIds onto `LookAndFeel_V4::ColourScheme::UIColour` positions so
  stock widgets pick up the theme without knowing about the store.

## Setup order matters

```cpp
// member declaration order: LaF must outlive children that query it
juce::LookAndFeel_V4 lookAndFeel;
StylesStore          stylesStore;

// constructor:
stylesStore.fillStore (Stylesheet::styleColours,
                       Stylesheet::styleNumbers, lookAndFeel);
setLookAndFeel (&lookAndFeel);
```

Custom components receive `const StylesStore&` and query it inside `paint()`;
layout code replaces magic numbers with `getNumber (NumberIds::LayoutMargin)`.

## How this maps to an existing theme layer

If a project already has a theme namespace (constants + a LookAndFeel
subclass), it already implements the *spirit* of this pattern. The article's
additions worth checking for:

1. Are layout metrics AND fonts AND colours all behind one namespace/class?
2. Is any `paint()` still holding a literal the theme should own?
3. Can the value SOURCE be swapped (compile-time constants → file) without
   touching consumers?
4. Does the LookAndFeel get its scheme FROM the same source the custom
   paint() uses (single origin), or are they two copies?

## When NOT to reach for this

A one-window tool with < 10 colours does not need a StylesStore class — a
namespace of constexpr colours plus a LookAndFeel subclass is simpler and
just as safe (that IS the minimal form of the pattern). Introduce the class
when themes must switch at runtime or load from files.
