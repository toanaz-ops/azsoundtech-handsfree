#pragma once

#include <juce_graphics/juce_graphics.h>

// First-open window sizing. Pure function so it is testable without a screen.
//
// The default size (1520x980) is larger than a 1366x768 laptop panel, which
// opened the window with its bottom edge and the transport off-screen. Clamp
// the wanted size to the usable display area, but never below the resize
// limits: the minimum layout is the floor, the display is only a ceiling.
// If the display is smaller than the minimum, the minimum wins and the window
// is still positioned so its top-left stays visible (see fitWindowBounds).
inline juce::Point<int> clampSizeToArea (juce::Point<int> wanted,
                                         juce::Point<int> minimum,
                                         juce::Rectangle<int> userArea)
{
    return { juce::jmax (minimum.x, juce::jmin (wanted.x, userArea.getWidth())),
             juce::jmax (minimum.y, juce::jmin (wanted.y, userArea.getHeight())) };
}

// Clamped size, centred in userArea; if larger than the area (minimum wins),
// aligned to the area's top-left so the title bar stays reachable.
inline juce::Rectangle<int> fitWindowBounds (juce::Point<int> wanted,
                                             juce::Point<int> minimum,
                                             juce::Rectangle<int> userArea)
{
    const auto s = clampSizeToArea (wanted, minimum, userArea);
    const int x = s.x >= userArea.getWidth()  ? userArea.getX()
                                              : userArea.getX() + (userArea.getWidth()  - s.x) / 2;
    const int y = s.y >= userArea.getHeight() ? userArea.getY()
                                              : userArea.getY() + (userArea.getHeight() - s.y) / 2;
    return { x, y, s.x, s.y };
}
