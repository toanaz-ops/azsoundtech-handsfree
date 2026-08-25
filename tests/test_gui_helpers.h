// Shared headless-GUI-test helpers -- extracted from copies that used to be
// duplicated across test_spectrumview.cpp, test_notchlistpanel.cpp and
// test_gui_wiring.cpp. Pure refactor; behaviour is unchanged.
//
// Same pattern everywhere these are used: ScopedJuceInitialiser_GUI brings up
// the MessageManager in the test itself; components are built headless, never
// added to a desktop; no message loop is pumped (JUCE_MODAL_LOOPS_PERMITTED
// off).

#pragma once

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

} // namespace gui_test
