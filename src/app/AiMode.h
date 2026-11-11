// Lane L3a (docs/superpowers/specs/2026-11-09-ai-drive-design.md sections 2, 3):
// the AI mode switch and the notch "author" tag.
//
// Both are ATTRIBUTION / GATING data only. Nothing in the detector, the
// scorer or the notch ladder reads either of them for a decision -- phase L3a
// changes the audio by exactly 0 dB at every output. Header-only, no JUCE.

#pragma once

#include <cstdint>
#include <string_view>

// What the operator asked the AI to do. Persisted as the REQUESTED mode; each
// actor (network, LLM) is then clamped by its own ceiling (effectiveAiMode).
enum class AiMode : std::uint8_t { Off, Shadow, Suggest, Act };

[[nodiscard]] constexpr const char* aiModeToString (AiMode m) noexcept
{
    switch (m)
    {
        case AiMode::Off:     return "off";
        case AiMode::Shadow:  return "shadow";
        case AiMode::Suggest: return "suggest";
        case AiMode::Act:     return "act";
    }
    return "shadow";   // unreachable for a valid enumerator; the safe default
}

// Unknown or empty text is Shadow (today's behaviour: score and log only).
[[nodiscard]] constexpr AiMode aiModeFromString (std::string_view s) noexcept
{
    if (s == "off")     return AiMode::Off;
    if (s == "suggest") return AiMode::Suggest;
    if (s == "act")     return AiMode::Act;
    return AiMode::Shadow;
}

// The lower of the two: an actor can never do more than the operator asked, nor
// more than its own ceiling allows.
[[nodiscard]] constexpr AiMode effectiveAiMode (AiMode requested, AiMode ceiling) noexcept
{
    return (std::uint8_t) requested < (std::uint8_t) ceiling ? requested : ceiling;
}

// Who is responsible for a notch's existence or last change. Attribution only;
// NotchCommand (src/dsp) deliberately does not carry it.
enum class Author : std::uint8_t { Detector, Human, Network, Llm };

[[nodiscard]] constexpr const char* authorName (Author a) noexcept
{
    switch (a)
    {
        case Author::Detector: return "detector";
        case Author::Human:    return "human";
        case Author::Network:  return "network";
        case Author::Llm:      return "llm";
    }
    return "detector";
}
