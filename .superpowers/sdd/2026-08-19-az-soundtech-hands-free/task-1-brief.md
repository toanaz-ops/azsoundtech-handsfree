# Task 1: CMake Project Setup & JUCE Integration

## Requirements

Create the foundational CMake build system for a JUCE-based standalone Windows audio application.

## Files to Create

- `CMakeLists.txt` (root)
- `README.md`
- `.gitignore`
- `.github/workflows/build.yml`

## Exact Specifications (from spec)

**Product Details:**
- Product Name: "AZ Soundtech Hands-free"
- Company: "AZ Soundtech"
- Bundle ID: com.azsoundtech.handsfree
- Version: 1.0.0

**Tech Stack Requirements:**
- JUCE 7.0+ (CMake support required)
- C++17 minimum
- Windows 10/11 64-bit only
- CMake 3.22+
- GoogleTest for unit tests
- ASIO SDK (Steinberg) - will be added in Task 2

## CMakeLists.txt Requirements

1. Set C++17 as minimum standard
2. Add JUCE as subdirectory (expects `external/JUCE`)
3. Create JUCE standalone GUI application target named "HandsFree"
4. Link JUCE modules:
   - juce::juce_audio_devices
   - juce::juce_audio_utils
   - juce::juce_dsp
   - juce::juce_gui_basics
   - juce::juce_recommended_config_flags
   - juce::juce_recommended_lto_flags
   - juce::juce_recommended_warning_flags
5. Set compile definitions:
   - JUCE_WEB_BROWSER=0
   - JUCE_USE_CURL=0
   - JUCE_APPLICATION_NAME_STRING
   - JUCE_APPLICATION_VERSION_STRING
6. Initial source: just `src/main.cpp`
7. Enable testing with `enable_testing()`
8. Add tests subdirectory

## README.md Requirements

Document:
- Project description (Windows standalone ASIO feedback eliminator)
- System requirements (Windows 10/11 64-bit, Visual Studio 2019+, CMake 3.22+, ASIO interface)
- Build instructions (clone with submodules, CMake configure, build)
- Test instructions (ctest)
- License note (Proprietary, requires activation)

## .gitignore Requirements

Ignore:
- Build artifacts: `build/`, `cmake-build-*/`, `out/`
- IDE files: `.vs/`, `.vscode/`, `*.suo`, `*.user`, `*.sln.docstates`
- Binaries: `*.exe`, `*.dll`, `*.lib`, `*.obj`
- Temp files: `*.log`, `*.tmp`, `~*`

## GitHub Actions CI Requirements

Create `.github/workflows/build.yml`:
- Trigger on push and pull_request
- Job: build-windows on windows-latest
- Steps:
  1. Checkout with recursive submodules
  2. Configure with CMake (Visual Studio 17 2022, x64)
  3. Build Release configuration
  4. Run ctest

## Test Strategy

At this stage: Verify CMake configuration generates successfully without errors.
Command: `cmake -B build -G "Visual Studio 17 2022" -A x64`
Expected: Configuration completes, build directory created.

## Interfaces Produced

- CMake build system that compiles JUCE standalone app
- CI pipeline that builds on every push
- Development environment ready for src/main.cpp (Task 3)

## Success Criteria

1. CMakeLists.txt exists and configures without errors
2. README.md documents build process
3. .gitignore covers all build artifacts
4. GitHub Actions workflow is valid YAML
5. All files committed with message: "feat: add CMake project setup with JUCE"
