# Task 3: Minimal JUCE Application Stub

## Requirements

Create a minimal working JUCE application that launches a window with placeholder UI.

## Files to Create

- `src/main.cpp` - JUCE application entry point
- `src/app/MainComponent.h` - Main UI component header
- `src/app/MainComponent.cpp` - Main UI component implementation

## Modifications

- `CMakeLists.txt` - Update target_sources and include directories

## Exact Specifications (from spec)

**Application Details:**
- Application name: "AZ Soundtech Hands-free"
- Version: "1.0.0"
- Window size: 800x600 pixels
- Single instance only (moreThanOneInstanceAllowed = false)

**Architecture:**
- JUCEApplication subclass with MainWindow inner class
- MainWindow inherits DocumentWindow
- MainComponent is the content component
- Native title bar enabled
- Resizable window
- Centered on screen

## main.cpp Requirements

Create `HandsFreeApplication` class:
- Inherit from `juce::JUCEApplication`
- Implement required virtuals:
  - `getApplicationName()` → "AZ Soundtech Hands-free"
  - `getApplicationVersion()` → "1.0.0"
  - `moreThanOneInstanceAllowed()` → false
  - `initialise()` - create MainWindow
  - `shutdown()` - destroy MainWindow
  - `systemRequestedQuit()` - call quit()

Create `MainWindow` inner class:
- Inherit from `juce::DocumentWindow`
- Constructor: set title, use default look and feel background color, show all buttons
- Enable native title bar
- Set MainComponent as content (owned)
- Make resizable
- Center window
- Make visible

Use `START_JUCE_APPLICATION(HandsFreeApplication)` macro.

## MainComponent Requirements

Header (`MainComponent.h`):
- Inherit from `juce::Component`
- Public constructor/destructor
- Override `paint()` and `resized()`
- Use `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR` macro

Implementation (`MainComponent.cpp`):
- Constructor: set size to 800x600
- `paint()`: fill background with default color, draw centered white text "AZ Soundtech Hands-free" at 20pt font
- `resized()`: empty for now (future components will layout here)

## CMakeLists.txt Updates

Update `target_sources(HandsFree PRIVATE` section:
```cmake
target_sources(HandsFree PRIVATE
    src/main.cpp
    src/app/MainComponent.cpp
    src/app/MainComponent.h
)

target_include_directories(HandsFree PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
```

## Test Strategy

**Manual verification (GUI app):**
1. Build: `cmake --build build --config Release`
2. Run: `build\Release\HandsFree.exe`
3. Verify: Window opens with "AZ Soundtech Hands-free" text, 800x600, centered
4. Verify: Window title shows "AZ Soundtech Hands-free"
5. Verify: Window can be resized
6. Verify: Window closes cleanly (no crash)
7. Verify: Cannot launch second instance (single instance constraint)

Expected: All verifications pass.

## Success Criteria

1. Application compiles without errors
2. Window opens and displays correctly
3. Application name and version match spec exactly
4. Window is resizable and centered
5. Single instance constraint works
6. Application closes cleanly
7. Commit with message: "feat: add minimal JUCE application stub"

## Interfaces Produced

- `class MainComponent` - base UI component for future Task 7+ GUI work
- Working JUCE application ready for AudioEngine integration (Task 8)
- Build verification that JUCE 9.0.1 + CMake setup works correctly
