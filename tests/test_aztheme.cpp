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
    expectArgb (az::theme::background, 0xff0b0f14u, "background");
    expectArgb (az::theme::panel,      0xff161e27u, "panel");
    expectArgb (az::theme::border,     0xff232c36u, "border");
    expectArgb (az::theme::text,       0xffc8d6e5u, "text");
    expectArgb (az::theme::dim,        0xff546e7au, "dim");
    expectArgb (az::theme::accent,     0xff4fc3f7u, "accent");
    expectArgb (az::theme::warn,       0xffff9800u, "warn");
    expectArgb (az::theme::ok,         0xff81c784u, "ok");
    expectArgb (az::theme::danger,     0xffff8a65u, "danger");
    expectArgb (az::theme::marker,     0xffffb74du, "marker");
}

TEST (AzTheme, MetricTokensMatchTheSpecifiedValues)
{
    EXPECT_EQ (az::theme::spacing,          4);
    EXPECT_EQ (az::theme::gap,              8);
    EXPECT_EQ (az::theme::touchTarget,      44);
    EXPECT_EQ (az::theme::railWidth,        96);
    EXPECT_EQ (az::theme::buttonCellWidth,  96);
    EXPECT_EQ (az::theme::buttonCellHeight, 64);
    EXPECT_FLOAT_EQ (az::theme::cornerRadius, 4.0f);
    EXPECT_FLOAT_EQ (az::theme::baseFontSize, 13.0f);
}

TEST (AzTheme, MonoFontIsConsolasAtTheBaseSizeByDefault)
{
    // Numbers must not shift width while counting down -- hence a monospaced
    // face, Consolas first with fallbacks for machines that lack it.
    const auto font = az::theme::monoFont();

    EXPECT_EQ (font.getTypefaceName(), juce::String ("Consolas"));
    EXPECT_FLOAT_EQ (font.getHeight(), az::theme::baseFontSize);
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
