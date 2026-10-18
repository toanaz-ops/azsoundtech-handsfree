#include "az/host/HostSettings.h"

namespace az::host
{

namespace
{
    // A doubled sample rate or a negative buffer are not "close enough": the
    // device setter would be asked for a value no hardware offers. Anything
    // outside these ranges reads as "not saved".
    constexpr double kMinRate = 8000.0, kMaxRate = 768000.0;
    constexpr int    kMinBuffer = 16, kMaxBuffer = 8192;

    void quarantine (const juce::File& file)
    {
        const auto bad = file.getSiblingFile (file.getFileName() + ".bad");
        bad.deleteFile();
        file.moveFileTo (bad);
    }

    LoadResult malformed (const juce::File& file, const juce::String& why)
    {
        LoadResult r;
        r.error = why + ": " + file.getFullPathName();
        quarantine (file);
        return r;
    }
}

LoadResult loadHostSettings (const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    const juce::var root = juce::JSON::parse (file.loadFileAsString());

    auto* obj = root.getDynamicObject();
    if (obj == nullptr)
        return malformed (file, "settings file is not a JSON object");

    const juce::var schema = obj->getProperty ("schema");
    if (! (schema.isInt() || schema.isInt64()) || (int) schema < 1)
        return malformed (file, "settings file has no valid schema");

    if ((int) schema > HostSettings::kSchema)
    {
        LoadResult r;
        r.error        = "settings file is from a newer build (schema " + schema.toString()
                       + "), left untouched: " + file.getFullPathName();
        r.mayOverwrite = false;
        return r;
    }

    LoadResult r;
    auto& s = r.settings;

    if (auto* dev = obj->getProperty ("device").getDynamicObject())
    {
        s.device.type = dev->getProperty ("type").toString();
        s.device.name = dev->getProperty ("name").toString();

        const double rate = dev->getProperty ("sampleRate");
        if (std::isfinite (rate) && rate >= kMinRate && rate <= kMaxRate)
            s.device.sampleRate = rate;

        const int buffer = dev->getProperty ("bufferSize");
        if (buffer >= kMinBuffer && buffer <= kMaxBuffer)
            s.device.bufferSize = buffer;
    }

    s.product = obj->getProperty ("product");
    return r;
}

bool saveHostSettings (const juce::File& file, const HostSettings& settings)
{
    if (! file.getParentDirectory().createDirectory())
        return false;

    auto dev = std::make_unique<juce::DynamicObject>();
    dev->setProperty ("type",       settings.device.type);
    dev->setProperty ("name",       settings.device.name);
    dev->setProperty ("sampleRate", settings.device.sampleRate);
    dev->setProperty ("bufferSize", settings.device.bufferSize);

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("schema",  HostSettings::kSchema);
    root->setProperty ("device",  juce::var (dev.release()));
    root->setProperty ("product", settings.product);

    const juce::String text = juce::JSON::toString (juce::var (root.release()));

    // TemporaryFile writes beside the target and replaceFileIn() moves it over
    // in one step: the old file stays whole until the new one is.
    juce::TemporaryFile temp (file);
    if (! temp.getFile().replaceWithText (text))
        return false;

    return temp.overwriteTargetFileWithTemporary();
}

} // namespace az::host
