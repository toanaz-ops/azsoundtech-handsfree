// CopilotAnswer: parses the sidecar's `--ask --json` stdout (one JSON object).
//
// Contract: docs/superpowers/specs/2026-11-09-copilot-panel-design.md, "L2b
// sidecar contract". Exit codes 0, 3 and 4 each carry one object on stdout.

#pragma once

#include "app/CopilotFindings.h"

#include <juce_core/juce_core.h>

#include <vector>

struct CopilotAnswer
{
    enum class Kind { answer, blocked, error, invalid };

    Kind kind = Kind::invalid;
    juce::String text; // answer text, blocked message or error reason
    juce::String provider, model;
    int inputTokens = -1, outputTokens = -1; // -1 = not reported
    std::vector<CopilotFinding> findings;
};

constexpr int kMaxCopilotAnswerChars = 16000;
constexpr int kMaxCopilotMessageChars = 500;

/** {"blocked": true, "message": M} -> blocked; {"error": E} -> error; an object
    with a non-empty string "answer" -> answer; anything else (not an object,
    empty or non-string answer) -> invalid. Text is capped, findings use the same
    per-item rules as parseCopilotFindings (a bad array yields an empty list),
    token counts are kept only when non-negative integers. */
CopilotAnswer parseCopilotAnswer (const juce::String& stdoutText);
