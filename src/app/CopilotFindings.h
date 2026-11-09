// CopilotFindings: parses the sidecar's `--json` output into rows for the panel.
//
// The sidecar (tools/copilot/copilot.py) prints one JSON array; each element has
// code, severity, title, advice, evidence. The GUI needs the first four.
// Spec: docs/superpowers/specs/2026-11-09-copilot-panel-design.md section 4.1.

#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

struct CopilotFinding
{
    juce::String code, severity, title, advice;
};

/** Upper bound on rows kept, so garbled or hostile output cannot flood the UI. */
constexpr int kMaxCopilotFindings = 64;

/** nullopt when the text is not a valid JSON array. An element that is not an
    object, or lacks code/title/advice, is skipped. A severity other than
    alert/warn/info (or a missing one) becomes "info". */
std::optional<std::vector<CopilotFinding>> parseCopilotFindings (const juce::String& utf8Json);
