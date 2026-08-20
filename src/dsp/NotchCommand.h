#pragma once

#include <cstdint>

enum class NotchCommandType : uint8_t
{
    Set,
    Clear
};

struct NotchCommand
{
    NotchCommandType type;
    uint8_t channel;    // 0 = Left, 1 = Right
    uint8_t index;      // 0-15 (notch slot in chain)
    float frequency;    // Hz, only valid for Set
    float Q;            // Quality factor, only valid for Set
    float depthDB;      // Depth in dB, only valid for Set
};