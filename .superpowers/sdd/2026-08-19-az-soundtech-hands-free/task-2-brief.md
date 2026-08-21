# Task 2: JUCE & ASIO SDK Integration

## Requirements

Add JUCE framework and ASIO SDK to the project as external dependencies.

## Files to Create/Modify

- `.gitmodules` (create - for JUCE submodule)
- `external/` directory
- `CMakeLists.txt` (modify - add ASIO SDK path configuration)
- `README.md` (modify - add ASIO SDK setup instructions)

## Exact Specifications

**JUCE:**
- Add as git submodule at `external/JUCE`
- Repository: https://github.com/juce-framework/JUCE.git
- Branch: master
- Must be recursive submodule init

**ASIO SDK:**
- Cannot be committed due to Steinberg license restrictions
- Must be downloaded manually by developers
- Expected location: `external/asiosdk`
- Must contain `common/asio.h` at minimum
- Version: ASIO SDK 2.3+

## CMakeLists.txt Modifications

Add after the `add_subdirectory(external/JUCE)` line:

```cmake
# ASIO SDK path
set(JUCE_ASIO_SDK_PATH "${CMAKE_SOURCE_DIR}/external/asiosdk" CACHE PATH "Path to ASIO SDK")

if(EXISTS "${JUCE_ASIO_SDK_PATH}/common/asio.h")
    message(STATUS "ASIO SDK found at ${JUCE_ASIO_SDK_PATH}")
    set(JUCE_ASIO ON CACHE BOOL "Enable ASIO support" FORCE)
else()
    message(WARNING "ASIO SDK not found. Download from Steinberg and place in external/asiosdk/")
endif()
```

## README.md Additions

Add new section after "Requirements" heading:

```markdown
## ASIO SDK Setup

Due to licensing, ASIO SDK must be downloaded manually:

1. Download ASIO SDK 2.3+ from Steinberg: https://www.steinberg.net/asiosdk
2. Extract to `external/asiosdk/` (should contain `common/asio.h`)
3. Re-run CMake configuration
```

Also update the "Build" section step 1 to emphasize recursive clone:

```markdown
1. Clone with submodules:
   ```
   git clone --recursive <repo-url>
   ```
```

## .gitignore Addition

Add to prevent accidentally committing ASIO SDK:

```
# ASIO SDK (license restriction)
external/asiosdk/
```

## Test Strategy

1. Verify JUCE submodule added: `git submodule status` should show `external/JUCE`
2. Verify ASIO warning: Run CMake without ASIO SDK present, should show warning message
3. (Manual verification only - ASIO SDK download is external to this task)

## Success Criteria

1. JUCE added as git submodule at external/JUCE
2. CMakeLists.txt checks for ASIO SDK and warns if missing
3. README documents ASIO SDK setup clearly
4. .gitignore prevents ASIO SDK from being committed
5. CMake configuration succeeds (with ASIO warning if SDK not present)
6. Commit with message: "feat: integrate JUCE and ASIO SDK"

## Interfaces Produced

- JUCE framework available for compilation (via git submodule)
- ASIO SDK path configured (developer must supply SDK manually)
- Build system ready for Task 3 (application code)
