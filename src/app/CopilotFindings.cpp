#include "app/CopilotFindings.h"

std::optional<std::vector<CopilotFinding>> parseCopilotFindings (const juce::String& utf8Json)
{
    juce::var parsed;
    if (! juce::JSON::parse (utf8Json, parsed).wasOk())
        return std::nullopt;

    const auto* array = parsed.getArray();
    if (array == nullptr)
        return std::nullopt;

    const juce::Identifier codeId ("code"), titleId ("title"), adviceId ("advice"),
                           severityId ("severity");

    std::vector<CopilotFinding> out;

    for (const auto& element : *array)
    {
        if (static_cast<int> (out.size()) >= kMaxCopilotFindings)
            break;

        const auto* object = element.getDynamicObject();
        if (object == nullptr)
            continue;

        if (! object->hasProperty (codeId) || ! object->hasProperty (titleId)
            || ! object->hasProperty (adviceId))
            continue;

        CopilotFinding f;
        f.code   = object->getProperty (codeId).toString();
        f.title  = object->getProperty (titleId).toString();
        f.advice = object->getProperty (adviceId).toString();

        const auto severity = object->getProperty (severityId).toString();
        f.severity = (severity == "alert" || severity == "warn" || severity == "info")
                         ? severity
                         : juce::String ("info");

        out.push_back (std::move (f));
    }

    return out;
}
