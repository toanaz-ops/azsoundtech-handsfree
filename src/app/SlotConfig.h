// SlotConfig: một luồng xử lý độc lập trong mô hình multi-slot (spec
// docs/superpowers/specs/2026-08-24-multi-slot-routing-design.md §3).
// Thuần dữ liệu std-only để test standalone, không phụ thuộc JUCE.
#pragma once

#include <algorithm>

constexpr int kMaxSlots     = 8;
constexpr int kMaxSlotLanes = 2;   // stereo = 2 làn, mono = 1

struct SlotConfig
{
    bool enabled = false;
    int  width   = 2;                    // 1 = mono, 2 = stereo
    int  inputChannels[kMaxSlotLanes]  = { 0, 1 };
    int  outputChannels[kMaxSlotLanes] = { 0, 1 };
};

inline bool slotConfigIsValid (const SlotConfig& c,
                               int numInputChannels,
                               int numOutputChannels)
{
    if (numInputChannels <= 0 || numOutputChannels <= 0) return false;
    if (c.width != 1 && c.width != 2) return false;

    for (int lane = 0; lane < c.width; ++lane)
    {
        if (c.inputChannels[lane]  < 0 || c.inputChannels[lane]  >= numInputChannels)
            return false;
        if (c.outputChannels[lane] < 0 || c.outputChannels[lane] >= numOutputChannels)
            return false;
    }
    return true;   // các làn >= width bị bỏ qua, không cần hợp lệ
}

inline SlotConfig slotClampedTo (const SlotConfig& c,
                                 int numInputChannels,
                                 int numOutputChannels)
{
    SlotConfig r = c;
    if (numInputChannels  <= 0) numInputChannels  = 1;
    if (numOutputChannels <= 0) numOutputChannels = 1;
    for (int lane = 0; lane < kMaxSlotLanes; ++lane)
    {
        r.inputChannels[lane]  = std::clamp (r.inputChannels[lane],
                                             0, numInputChannels  - 1);
        r.outputChannels[lane] = std::clamp (r.outputChannels[lane],
                                             0, numOutputChannels - 1);
    }
    return r;
}
