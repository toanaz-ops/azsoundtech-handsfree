#include "app/CopilotAnswer.h"

namespace
{
juce::String capped (const juce::String& s, int maxChars)
{
    return s.length() > maxChars ? s.substring (0, maxChars) : s;
}

int readTokenCount (const juce::var& v)
{
    if (! (v.isInt() || v.isInt64()))
        return -1;
    const auto n = (juce::int64) v;
    return (n >= 0 && n <= 2147483647) ? (int) n : -1;
}
}

CopilotAnswer parseCopilotAnswer (const juce::String& stdoutText)
{
    CopilotAnswer out;

    juce::var parsed;
    if (! juce::JSON::parse (stdoutText, parsed).wasOk())
        return out;

    const auto* object = parsed.getDynamicObject();
    if (object == nullptr)
        return out;

    const auto blocked = object->getProperty ("blocked");
    if (blocked.isBool() && (bool) blocked)
    {
        const auto message = object->getProperty ("message");
        if (! message.isString())
            return out;
        out.kind = CopilotAnswer::Kind::blocked;
        out.text = capped (message.toString(), kMaxCopilotMessageChars);
        return out;
    }

    if (object->hasProperty ("error"))
    {
        const auto error = object->getProperty ("error");
        if (! error.isString())
            return out;
        out.kind = CopilotAnswer::Kind::error;
        out.text = capped (error.toString(), kMaxCopilotMessageChars);
        return out;
    }

    const auto answer = object->getProperty ("answer");
    if (! answer.isString() || answer.toString().isEmpty())
        return out;

    out.kind = CopilotAnswer::Kind::answer;
    out.text = capped (answer.toString(), kMaxCopilotAnswerChars);

    const auto provider = object->getProperty ("provider");
    const auto model = object->getProperty ("model");
    if (provider.isString()) out.provider = provider.toString();
    if (model.isString())    out.model = model.toString();

    if (const auto* usage = object->getProperty ("usage").getDynamicObject())
    {
        out.inputTokens  = readTokenCount (usage->getProperty ("input_tokens"));
        out.outputTokens = readTokenCount (usage->getProperty ("output_tokens"));
    }

    const auto findings = object->getProperty ("findings");
    if (findings.isArray())
        if (auto rows = parseCopilotFindings (juce::JSON::toString (findings, true)))
            out.findings = std::move (*rows);

    return out;
}
