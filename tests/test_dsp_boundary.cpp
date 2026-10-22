// The DSP core is the boundary for the future VST3 build (CLAUDE.md, "Phần
// dùng chung phải là thư viện"): nothing under src/dsp may reach a device, a
// window or the JUCE amalgamated header. True today and kept true by nothing
// but review -- review 2026-10-20 (M4d) grepped it by hand. This test reads
// src/dsp as text at run time, the same way the theme test reads src/gui.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST (DspBoundary, SrcDspIncludesNoDeviceGuiOrAmalgamatedJuceHeader)
{
    const fs::path dspRoot { HANDSFREE_DSP_SOURCE_DIR };
    ASSERT_TRUE (fs::is_directory (dspRoot)) << dspRoot.string();

    // juce_dsp, juce_core and juce_audio_basics are what a plugin build has
    // too; anything that needs a device, a window or the app header is not.
    static const std::regex forbidden {
        R"(#\s*include\s*[<"](juce_audio_devices|juce_audio_utils|juce_gui_basics|juce_gui_extra|juce_graphics|JuceHeader\.h))"
    };

    std::vector<std::string> offenders;
    int scanned = 0;
    for (const auto& entry : fs::recursive_directory_iterator (dspRoot))
    {
        if (! entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h")
            continue;

        std::ifstream file (entry.path(), std::ios::binary);
        ASSERT_TRUE (file.is_open()) << entry.path().string();
        std::ostringstream buffer;
        buffer << file.rdbuf();
        ++scanned;

        if (std::regex_search (buffer.str(), forbidden))
            offenders.push_back (entry.path().string());
    }

    EXPECT_GT (scanned, 10) << "the scan found almost nothing: wrong directory?";
    for (const auto& o : offenders)
        ADD_FAILURE() << o << " includes a device/GUI module or JuceHeader.h";
}
