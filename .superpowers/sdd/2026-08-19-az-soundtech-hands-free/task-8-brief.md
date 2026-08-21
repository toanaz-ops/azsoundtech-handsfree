# Task 8: AudioEngine Class - ASIO I/O

## Requirements

Implement the AudioEngine class that wraps JUCE AudioDeviceManager for ASIO I/O and integrates the DSP chain.

## Files to Create

- `src/app/AudioEngine.h`
- `src/app/AudioEngine.cpp`

## Exact Specifications

**Purpose:** Bridge between JUCE audio I/O and the DSP core (NotchChain). Manages:
- ASIO device opening/closing via AudioDeviceManager
- Audio callback (real-time thread) - reads input, processes through notch chain, writes output
- Tap signal to ring buffer for detector thread
- Mode management (Bypass/Auto/Soundcheck)
- Latency reporting

**Class interface:**

```cpp
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    enum class Mode
    {
        Bypass,
        Auto,
        Soundcheck
    };
    
    AudioEngine();
    ~AudioEngine();
    
    // Device management
    void start();
    void stop();
    bool isRunning() const;
    
    // ASIO device control
    void setAudioDeviceType(const juce::String& typeName);
    void setAudioDevice(const juce::String& deviceName);
    juce::String getCurrentDeviceName() const;
    juce::String getCurrentSampleRate() const;
    int getCurrentBufferSize() const;
    double getCurrentLatency() const;
    
    // Mode control
    void setMode(Mode mode);
    Mode getMode() const;
    
    // AudioIODeviceCallback interface
    void audioDeviceIOCallback(const float** inputChannelData,
                                int numInputChannels,
                                float** outputChannelData,
                                int numOutputChannels,
                                int numSamples) override;
    
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;

private:
    juce::AudioDeviceManager deviceManager_;
    std::array<NotchChain, 2> notchChains_;  // L=0, R=1
    Mode currentMode_ = Mode::Bypass;
    double currentSampleRate_ = 48000.0;
    int currentBufferSize_ = 64;
    bool isRunning_ = false;
};
```

**Architecture:**
- Inherits `juce::AudioIODeviceCallback` for the audio thread
- Uses `juce::AudioDeviceManager` to manage device lifecycle
- Holds two `NotchChain` instances (one per channel, 16 notches each)
- Real-time audio callback processes samples through notch chains

**Threading model:**
- `start()`/`stop()` - called from UI thread
- `audioDeviceIOCallback()` - called from audio thread (real-time, no allocation!)
- All DSP work must be lock-free

**Implementation notes:**
1. Constructor: Initialize deviceManager, create 2 NotchChain instances (one L, one R)
2. Destructor: Ensure stop() is called
3. start(): Open default ASIO device, add this as callback
4. stop(): Remove callback, close device
5. audioDeviceIOCallback(): Read input samples → process through notchChains_[ch] → write output
6. audioDeviceAboutToStart(): Reset notch chains, capture sample rate/buffer size
7. audioDeviceError(): Set isRunning_=false, log error

**Critical requirements:**
- NO allocation in audioDeviceIOCallback
- NO locking in audioDeviceIOCallback
- Sample-by-sample processing through NotchChain (which is already no-allocation)
- Bypass mode: skip notch processing entirely (input → output)

## Files to Modify

### CMakeLists.txt

Add to `target_sources(HandsFree PRIVATE`:
```cmake
    src/app/AudioEngine.cpp
    src/app/AudioEngine.h
```

## Test Strategy

For this task, no automated unit tests (audio I/O requires hardware). Manual verification:
1. App launches with no errors
2. Window opens
3. Connect ASIO device, verify audio passes through in Bypass mode
4. Set a notch manually (via test code), verify attenuation

**Code verification:**
- Build succeeds (with MSVC toolchain)
- No compilation errors or warnings

## Critical Quality Requirements

1. **No allocation in audioDeviceIOCallback** - real-time audio thread
2. **No locks in audioDeviceIOCallback** - audio thread must never block
3. **JUCE API usage correct** - follow JUCE patterns
4. **Header guards** - #pragma once
5. **JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR** macro

## Success Criteria

1. `src/app/AudioEngine.h` and `.cpp` created with exact interface
2. AudioEngine inherits juce::AudioIODeviceCallback
3. Implements all methods from brief
4. CMakeLists.txt updated
5. Builds successfully (MSVC)
6. Commit with message: "feat: add AudioEngine class with JUCE AudioIODeviceCallback integration"

## Interfaces Produced

- `class AudioEngine` ready for Task 9 (passthrough with tap) and Task 10+ (detector integration)

## Report Contract

Status: DONE | DONE_WITH_CONCERNS | NEEDS_CONTEXT | BLOCKED
- Files created (line counts)
- Build verification
- Code review summary
- Commit hash
- Concerns

**IMPORTANT NOTE:** This is the first task being dispatched to the `implement` subagent (DeepSeek V4 Flash Free) per the new config. The orchestrator is Plan agent (Opus 5).
