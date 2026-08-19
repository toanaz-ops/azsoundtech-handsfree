# AZ Soundtech Hands-free

Windows standalone audio application for ASIO feedback elimination in live sound environments.

## System Requirements

- Windows 10/11 64-bit
- Visual Studio 2019 or later
- CMake 3.22 or later
- ASIO-compatible audio interface

## ASIO SDK Setup

Due to licensing, ASIO SDK must be downloaded manually:

1. Download ASIO SDK 2.3+ from Steinberg: https://www.steinberg.net/asiosdk
2. Extract to `external/asiosdk/` (should contain `common/asio.h`)
3. Re-run CMake configuration

## Build Instructions

1. Clone the repository with submodules:
```bash
git clone --recursive <repository-url>
cd PROJECT005-AZ-handsfree
```

2. Configure the project with CMake:
```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
```

3. Build the project:
```bash
cmake --build build --config Release
```

## Testing

Run tests using CTest:
```bash
cd build
ctest -C Release
```

## License

Proprietary software. Requires activation. Copyright (c) 2026 AZ Soundtech. All rights reserved.
