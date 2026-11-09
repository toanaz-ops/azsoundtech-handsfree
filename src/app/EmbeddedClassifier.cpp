#include "app/EmbeddedClassifier.h"

#include <HandsFreeModelsData.h>

juce::String embeddedCandidateClassifierJson()
{
    return juce::String::fromUTF8 (HandsFreeModelsData::candidateclassifieruntrainedv1_json,
                                   HandsFreeModelsData::candidateclassifieruntrainedv1_jsonSize);
}

std::unique_ptr<CandidateClassifier> makeEmbeddedCandidateClassifier (juce::String& error)
{
    auto classifier = std::make_unique<CandidateClassifier>();
    if (! classifier->loadFromJson (embeddedCandidateClassifierJson(), error))
        return nullptr;
    return classifier;
}
