#pragma once

// Lane C1b: the candidate-classifier model that ships INSIDE the binary.
// It is the C1a fixture weights under the id "untrained-fixture-v1": random,
// never trained, so its score means nothing about feedback or music. It exists
// to prove the inference path runs on a tester's machine. Spec:
// docs/superpowers/specs/2026-11-09-candidate-classifier-design.md section 5.

#include "app/CandidateClassifier.h"

#include <memory>

// Message thread, off the detector threads (it parses JSON and allocates).
// Returns nullptr and fills `error` when the embedded model does not load; the
// caller then runs with no classifier at all.
std::unique_ptr<CandidateClassifier> makeEmbeddedCandidateClassifier (juce::String& error);

// The raw embedded JSON text (for tests that want to alter the weights).
juce::String embeddedCandidateClassifierJson();
