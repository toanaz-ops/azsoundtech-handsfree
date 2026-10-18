// The host shell's memory between launches: which audio device was open, and
// an opaque section each product fills with its own state.
//
// Shared library (lane U, spec 2026-10-18 section 3.1). Depends on juce_core
// ONLY and never includes anything from a product's src/ -- the `product` var
// is how a product stores its own types (routing, UI mode) without this
// library knowing them.

#pragma once

#include <juce_core/juce_core.h>

namespace az::host
{

struct DeviceSetup
{
    juce::String type;          ///< Driver type name, e.g. "ASIO". Empty = never saved.
    juce::String name;          ///< Device name within that type. Empty = driver default.
    double       sampleRate = 0.0;  ///< 0 = not saved, keep the device's own.
    int          bufferSize = 0;    ///< 0 = not saved, keep the device's own.

    bool operator== (const DeviceSetup& o) const
    {
        return type == o.type && name == o.name
            && sampleRate == o.sampleRate && bufferSize == o.bufferSize;
    }
};

struct HostSettings
{
    static constexpr int kSchema = 1;

    DeviceSetup device;
    juce::var   product;        ///< The product's own section. Never read here.
};

struct LoadResult
{
    HostSettings settings;      ///< Defaults whenever the file could not be used.
    juce::String error;         ///< Empty = fine, including "no file yet".

    // False when the file was written by a NEWER build (unknown schema). It is
    // left exactly as found and must not be saved over: downgrading an alpha
    // build must not destroy the newer build's settings.
    bool mayOverwrite = true;
};

// Never throws. A missing file is the first-run case: defaults, no error. A
// malformed file returns defaults with `error` set and is RENAMED to
// `<name>.bad` -- kept for diagnosis, moved out of the way of the next save.
// An unknown schema returns defaults, `error` set, the file untouched and
// `mayOverwrite` false.
LoadResult loadHostSettings (const juce::File& file);

// Writes a temporary sibling and moves it over `file`, so a crash mid-write
// leaves the previous settings rather than half a file. Creates the parent
// directory. Returns false when nothing was written.
bool saveHostSettings (const juce::File& file, const HostSettings& settings);

} // namespace az::host
