#include "app/AppSettings.h"

namespace
{
    bool readChannel (const juce::var& v, int& out)
    {
        if (! (v.isInt() || v.isInt64() || v.isDouble()))
            return false;
        const double d = v;
        if (! std::isfinite (d) || d < 0.0 || d != std::floor (d) || d > 1024.0)
            return false;
        out = (int) d;
        return true;
    }

    bool readChannelPair (const juce::var& v, int (&out)[kMaxSlotLanes])
    {
        const auto* arr = v.getArray();
        if (arr == nullptr || arr->size() != kMaxSlotLanes)
            return false;
        int tmp[kMaxSlotLanes] {};
        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
            if (! readChannel ((*arr)[lane], tmp[lane]))
                return false;
        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
            out[lane] = tmp[lane];
        return true;
    }

    // Whole entry or nothing: a slot whose width parsed but whose outputs did
    // not is not half-applied.
    SlotConfig readSlot (const juce::var& v)
    {
        SlotConfig parsed;
        auto* obj = v.getDynamicObject();
        if (obj == nullptr)
            return {};

        const juce::var enabled = obj->getProperty ("enabled");
        const juce::var width   = obj->getProperty ("width");
        if (! enabled.isBool() || ! (width.isInt() || width.isInt64()))
            return {};

        parsed.enabled = (bool) enabled;
        parsed.width   = (int) width;
        if (parsed.width != 1 && parsed.width != 2)
            return {};

        if (! readChannelPair (obj->getProperty ("inputChannels"),  parsed.inputChannels)
         || ! readChannelPair (obj->getProperty ("outputChannels"), parsed.outputChannels))
            return {};

        return parsed;
    }

    juce::var channelPair (const int (&ch)[kMaxSlotLanes])
    {
        juce::Array<juce::var> a;
        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
            a.add (ch[lane]);
        return a;
    }
}

juce::String uiModeName (UiMode mode)
{
    return mode == UiMode::Geek ? "geek" : "dumb";
}

AppSettings appSettingsFromHost (const az::host::HostSettings& host)
{
    AppSettings s;
    s.device = host.device;

    if (host.product["uiMode"].toString() == "geek")
        s.uiMode = UiMode::Geek;

    if (const auto* slots = host.product["slots"].getArray())
    {
        s.hasRouting = true;
        for (int i = 0; i < kMaxSlots; ++i)
            s.slots[(std::size_t) i] = i < slots->size() ? readSlot ((*slots)[i]) : SlotConfig {};
    }

    return s;
}

az::host::HostSettings appSettingsToHost (const AppSettings& settings)
{
    az::host::HostSettings host;
    host.device = settings.device;

    auto product = std::make_unique<juce::DynamicObject>();
    product->setProperty ("uiMode", uiModeName (settings.uiMode));

    if (settings.hasRouting)
    {
        juce::Array<juce::var> slots;
        for (const auto& c : settings.slots)
        {
            auto entry = std::make_unique<juce::DynamicObject>();
            entry->setProperty ("enabled",        c.enabled);
            entry->setProperty ("width",          c.width);
            entry->setProperty ("inputChannels",  channelPair (c.inputChannels));
            entry->setProperty ("outputChannels", channelPair (c.outputChannels));
            slots.add (juce::var (entry.release()));
        }
        product->setProperty ("slots", slots);
    }

    host.product = juce::var (product.release());
    return host;
}

juce::File defaultAppSettingsFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("AZSoundtech")
        .getChildFile ("HandsFree")
        .getChildFile ("settings.json");
}
