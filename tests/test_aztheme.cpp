// Task 0 -- AzTheme + AzLookAndFeel (spec 2026-08-23 section 1).
//
// Two things are proven here:
//
// 1. The theme's tokens: the values match the approved palette exactly, and
//    the mono font exists for the Hz/dB/countdown draws that later tasks add.
//    (The former applyTo() contract tests were removed with the dead
//    AzTheme::applyTo path; MainComponent owns its LookAndFeel directly.)
//
// 2. The rule that keeps it true: NO hex colour literal anywhere in src/gui
//    outside src/gui/theme/. The test reads the sources as text at run time,
//    so a future component that hardcodes a colour fails CI rather than
//    quietly drifting away from the one source of truth.

#include <gtest/gtest.h>

#include "app/MainComponent.h"
#include "gui/theme/AzTheme.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

//==============================================================================
// The application window installs the theme's LookAndFeel exactly once, in
// its own constructor (decision D-3 of task 0).

TEST (AzTheme, MainComponentAppliesTheAzLookAndFeel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    MainComponent app;

    EXPECT_NE (dynamic_cast<az::theme::AzLookAndFeel*> (&app.getLookAndFeel()), nullptr);
}

//==============================================================================
// Token values -- the binding table is spec section 1; these hex integers are
// its transcription. A change here means the owner re-approved the palette.

namespace
{

void expectArgb (const juce::Colour& actual, unsigned int expectedArgb, const char* name)
{
    EXPECT_EQ ((unsigned int) actual.getARGB(), expectedArgb) << name;
}

} // namespace

TEST (AzTheme, PaletteTokensMatchTheSpecifiedValues)
{
    // SODIUM RACK, owner-approved 2026-08-25. The neutrals are warm graphite
    // rather than the previous blue-black, and the accent moved from Material
    // cyan to sodium amber -- see the rationale at the top of AzTheme.h.
    expectArgb (az::theme::background, 0xff0a0b0du, "background");
    expectArgb (az::theme::panel,      0xff131519u, "panel");
    expectArgb (az::theme::raise,      0xff1b1e24u, "raise");
    expectArgb (az::theme::well,       0xff0c0e11u, "well");
    expectArgb (az::theme::border,     0xff2b2f37u, "border");
    expectArgb (az::theme::shade,      0xff060709u, "shade");
    expectArgb (az::theme::text,       0xffe8eaedu, "text");
    expectArgb (az::theme::dim,        0xff868d98u, "dim");
    expectArgb (az::theme::faded,      0xff5c636eu, "faded");
    expectArgb (az::theme::accent,     0xffff9f1cu, "accent");
    expectArgb (az::theme::warn,       0xffffc24du, "warn");
    expectArgb (az::theme::ok,         0xff6ee7a0u, "ok");
    expectArgb (az::theme::danger,     0xffff5a4eu, "danger");
    expectArgb (az::theme::trace,      0xffffb552u, "trace");
    expectArgb (az::theme::grid,       0xff1d2128u, "grid");
    expectArgb (az::theme::peak,       0xffdde6f0u, "peak");
    expectArgb (az::theme::marker,     0xffff9f1cu, "marker");
    expectArgb (az::theme::cooling,    0xffc9d1d9u, "cooling");
    expectArgb (az::theme::settled,    0xff5fc9ffu, "settled");
}

TEST (AzTheme, MetricTokensMatchTheSpecifiedValues)
{
    EXPECT_EQ (az::theme::spacing,          4);
    EXPECT_EQ (az::theme::gap,              8);
    EXPECT_EQ (az::theme::touchTarget,      44);
    EXPECT_EQ (az::theme::railWidth,        96);

    // The transport switches. Grown from 96x64 with the rebuild: they are hit
    // in the dark, by someone doing something else with their other hand.
    EXPECT_EQ (az::theme::buttonCellWidth,  168);
    EXPECT_EQ (az::theme::buttonCellHeight, 62);
    EXPECT_EQ (az::theme::clearCellWidth,   148);

    EXPECT_EQ (az::theme::mastheadHeight,   38);
    EXPECT_EQ (az::theme::transportHeight,  86);
    EXPECT_EQ (az::theme::lampHeight,        3);
    EXPECT_EQ (az::theme::fieldHeight,      26);
    EXPECT_EQ (az::theme::captionHeight,    30);
    EXPECT_EQ (az::theme::gutterWidth,      74);
    EXPECT_EQ (az::theme::inlineLegendWidth, 58);
    EXPECT_FLOAT_EQ (az::theme::fieldSplit, 0.42f);

    EXPECT_FLOAT_EQ (az::theme::cornerRadius,   3.0f);
    EXPECT_FLOAT_EQ (az::theme::switchRadius,   4.0f);
    EXPECT_FLOAT_EQ (az::theme::baseFontSize,  14.0f);
    EXPECT_FLOAT_EQ (az::theme::legendFontSize, 12.0f);
}

TEST (AzTheme, EveryTransportSwitchClearsTheTouchTarget)
{
    // The reason the cells are sized at all: a switch smaller than the touch
    // target is one a soundman misses in the dark. Asserted as a RELATION so
    // a future resize cannot quietly drop under it.
    EXPECT_GE (az::theme::buttonCellWidth,  az::theme::touchTarget);
    EXPECT_GE (az::theme::buttonCellHeight, az::theme::touchTarget);
    EXPECT_GE (az::theme::clearCellWidth,   az::theme::touchTarget);
}

TEST (AzTheme, EveryFaceIsEmbeddedRatherThanResolvedFromTheMachine)
{
    // The design was approved in Saira Condensed / IBM Plex Sans / IBM Plex
    // Mono, and all three ship in assets/fonts/ as binary data. Naming them in
    // a fallback list instead would give the design on a machine that happens
    // to have them and something else everywhere else -- which is not a
    // design, it is a lottery.
    //
    // This asserts the TYPEFACE NAMES that come back, which is the only way to
    // tell an embedded face from a silent substitution: a missing font does
    // not fail, it falls back.
    EXPECT_EQ (az::theme::monoFont().getTypefaceName(),   juce::String ("IBM Plex Mono"));
    EXPECT_EQ (az::theme::baseFont().getTypefaceName(),   juce::String ("IBM Plex Sans"));
    EXPECT_EQ (az::theme::legendFont().getTypefaceName(), juce::String ("Saira Condensed"));
}

TEST (AzTheme, MonoFontHonoursTheRequestedSize)
{
    // Numbers must not shift width while counting down -- hence a monospaced
    // face -- and the countdown is drawn far larger than the base size.
    EXPECT_FLOAT_EQ (az::theme::monoFont().getHeight(), az::theme::baseFontSize);
    EXPECT_FLOAT_EQ (az::theme::monoFont (32.0f).getHeight(), 32.0f);
}

TEST (AzTheme, TheTypeScaleKeepsTheStudysOrdering)
{
    // The study fixes the RATIOS between these, not their absolute pixel
    // values (docs/spec-ui-mockup.md section 1). Asserting the order means a
    // future size tweak stays a tweak instead of flattening the hierarchy.
    using namespace az::theme;

    EXPECT_GT (switchFontSize,  brandFontSize);      // a switch shouts loudest
    EXPECT_GT (brandFontSize,   captionFontSize);
    EXPECT_GT (captionFontSize, columnFontSize);     // section above column
    EXPECT_GT (columnFontSize,  hintFontSize);
    EXPECT_GT (countdownFontSize, switchFontSize);   // the number beats them all

    // Tracking widens as the label gets more architectural.
    EXPECT_GT (trackingCaption, trackingColumn);
    EXPECT_GT (trackingColumn,  trackingSwitch);
}

//==============================================================================
// THE RAMP -- the one idea the rebuild is built around, and the only piece of
// the theme with behaviour rather than values. A notch is sodium the instant
// it fires and ice once it has held; the analyser's stems and the notch
// table's dots BOTH read it, so if this drifts the two stop agreeing about
// the same notch.

TEST (AzTheme, AFreshNotchIsSodiumAndFullyHot)
{
    EXPECT_FLOAT_EQ (az::theme::notchHeat (0.0), 1.0f);
    EXPECT_EQ (az::theme::notchColour (0.0).getARGB(), az::theme::marker.getARGB());
}

TEST (AzTheme, ASettledNotchIsIceAndFullyCool)
{
    EXPECT_FLOAT_EQ (az::theme::notchHeat (az::theme::kNotchCoolMs), 0.0f);
    EXPECT_EQ (az::theme::notchColour (az::theme::kNotchCoolMs).getARGB(),
               az::theme::settled.getARGB());

    // Past the cool point it STAYS ice rather than wrapping or overshooting.
    EXPECT_FLOAT_EQ (az::theme::notchHeat (10.0 * az::theme::kNotchCoolMs), 0.0f);
    EXPECT_EQ (az::theme::notchColour (10.0 * az::theme::kNotchCoolMs).getARGB(),
               az::theme::settled.getARGB());
}

TEST (AzTheme, TheRampMidpointIsPaleSteelRatherThanOlive)
{
    // The reason the ramp has three stops. A straight RGB lerp from sodium to
    // ice lands on an olive khaki halfway across; routing it through a pale
    // steel keeps the midpoint desaturated, so it reads as "cooling" instead
    // of as a rendering fault.
    const double halfway = az::theme::kNotchCoolMs * 0.5;
    EXPECT_FLOAT_EQ (az::theme::notchHeat (halfway), 0.5f);
    EXPECT_EQ (az::theme::notchColour (halfway).getARGB(), az::theme::cooling.getARGB());

    // Saturation dips in the middle and rises again at both ends -- that is
    // what "desaturates on the way across" means, stated as a measurement.
    const float midSaturation = az::theme::notchColour (halfway).getSaturation();
    EXPECT_LT (midSaturation, az::theme::marker.getSaturation());
    EXPECT_LT (midSaturation, az::theme::settled.getSaturation());
}

TEST (AzTheme, NotchHeatFallsMonotonicallyWithAge)
{
    // Monotonic, not merely "different at the ends": a notch that visibly
    // warmed back up as it aged would read as a NEW notch that never fired.
    float previous = az::theme::notchHeat (0.0);

    for (int step = 1; step <= 40; ++step)
    {
        const double age = az::theme::kNotchCoolMs * (double) step / 40.0;
        const float  heat = az::theme::notchHeat (age);

        EXPECT_LE (heat, previous) << "age " << age << " ms";
        EXPECT_GE (heat, 0.0f);
        EXPECT_LE (heat, 1.0f);
        previous = heat;
    }
}

TEST (AzTheme, NegativeAgesAreClampedRatherThanExtrapolated)
{
    // A clock that steps backwards (or a notch first seen in the same
    // millisecond it is drawn) must not produce a colour outside the ramp.
    EXPECT_FLOAT_EQ (az::theme::notchHeat (-5000.0), 1.0f);
    EXPECT_EQ (az::theme::notchColour (-5000.0).getARGB(), az::theme::marker.getARGB());
}

TEST (AzTheme, MonoFontAcceptsAnExplicitSize)
{
    // Later tasks draw countdown digits as large as fit their cell; the size
    // has to be caller-controlled.
    const auto big = az::theme::monoFont (32.0f);

    EXPECT_FLOAT_EQ (big.getHeight(), 32.0f);
}

TEST (AzTheme, BaseFontIsThirteenPoints)
{
    EXPECT_FLOAT_EQ (az::theme::baseFont().getHeight(), az::theme::baseFontSize);
}

//==============================================================================
// The grep-test: no "#RRGGBB" anywhere under src/gui except the theme folder.

TEST (AzTheme, NoHexColourLiteralsOutsideTheThemeFolder)
{
    namespace fs = std::filesystem;

    const fs::path guiRoot { HANDSFREE_GUI_SOURCE_DIR };
    ASSERT_TRUE (fs::exists (guiRoot)) << guiRoot.string();
    ASSERT_TRUE (fs::is_directory (guiRoot)) << guiRoot.string();

    static const std::regex hexColourLiteral { "#[0-9A-Fa-f]{6}" };
    std::vector<std::string> offenders;

    for (const auto& entry : fs::recursive_directory_iterator (guiRoot))
    {
        if (! entry.is_regular_file())
            continue;

        const auto extension = entry.path().extension().string();
        if (extension != ".cpp" && extension != ".h")
            continue;

        if (entry.path().parent_path() == guiRoot / "theme")
            continue;

        std::ifstream file (entry.path(), std::ios::binary);
        ASSERT_TRUE (file.is_open()) << entry.path().string();
        std::ostringstream buffer;
        buffer << file.rdbuf();

        if (std::regex_search (buffer.str(), hexColourLiteral))
            offenders.push_back (entry.path().string());
    }

    // An empty exception list by design: the theme folder is the ONLY place a
    // colour literal may live, and even there the tokens use integer form.
    EXPECT_TRUE (offenders.empty())
        << "hex colour literals found outside src/gui/theme/: "
        << [&offenders]
           {
               std::string joined;
               for (const auto& path : offenders)
                   joined += "\n  " + path;
               return joined;
           }();
}
