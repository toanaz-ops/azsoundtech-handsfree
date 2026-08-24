// Shared headless-GUI-test helpers -- extracted from copies that used to be
// duplicated across test_spectrumview.cpp, test_notchlistpanel.cpp and
// test_gui_wiring.cpp. Pure refactor; behaviour is unchanged.
//
// Same pattern everywhere these are used: ScopedJuceInitialiser_GUI brings up
// the MessageManager in the test itself; components are built headless, never
// added to a desktop; no message loop is pumped (JUCE_MODAL_LOOPS_PERMITTED
// off).

#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace gui_test
{

// Paints a component into an off-screen image without a desktop peer.
inline void paintHeadless (juce::Component& component, const int width, const int height)
{
    juce::Image image (juce::Image::ARGB, width, height, true);
    juce::Graphics g (image);
    component.setSize (width, height);
    component.paint (g);   // must simply not crash
}

// Persistence redirected into a scratch directory under %TEMP% via an ABSOLUTE
// folderName (File::getChildFile returns an absolute path as-is, so
// PropertiesFile::Options::getDefaultFile resolves there instead of
// %APPDATA%). A real user's saved layout is never read or clobbered, and the
// scratch directory is removed with the store -- no restore step to forget.
class TempLayoutStore
{
public:
    TempLayoutStore()
        : directory (juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("HandsFreeGuiTests_"
                                        + juce::String (juce::Random::getSystemRandom().nextInt())))
    {
        directory.createDirectory();
    }

    ~TempLayoutStore() { directory.deleteRecursively(); }

    TempLayoutStore (const TempLayoutStore&) = delete;
    TempLayoutStore& operator= (const TempLayoutStore&) = delete;

    juce::PropertiesFile::Options options() const
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "AZ Soundtech Hands-free";   // same file NAME as production
        o.filenameSuffix  = "xml";
        o.folderName      = directory.getFullPathName(); // absolute: wins over app-data dir
        return o;
    }

private:
    juce::File directory;
};

} // namespace gui_test
