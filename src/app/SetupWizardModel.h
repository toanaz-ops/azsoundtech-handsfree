// Lane U (spec 2026-10-18 section 3.5): the Dumb-mode setup wizard's state
// machine, with no component in it -- testable headless.
//
// Three steps: device, mic, output. The choices offered are built from the
// channel counts the audio CALLBACK actually receives (AudioEngine::
// getNumInput/OutputChannels), never from what a device advertises: a channel
// the engine does not open is a route that would carry silence (review I-7).

#pragma once

#include "app/SlotConfig.h"

#include <array>
#include <vector>

class SetupWizardModel
{
public:
    enum class Step { Device, Mic, Output };

    struct Choice
    {
        int width = 1;                         // 1 = one channel, 2 = a pair
        int channels[kMaxSlotLanes] = { 0, 0 };
    };

    // Every mono channel, then every adjacent pair starting on an even
    // channel (1+2, 3+4, ...) -- the way interfaces label stereo pairs.
    static std::vector<Choice> choicesFor (int numChannels);

    // More than one routing slot on: finishing would replace a multi-path
    // setup with one path, which the operator must agree to (Q6).
    static bool needsOverwriteConfirm (const std::array<SlotConfig, kMaxSlots>& current);

    void start (int numIn, int numOut);

    // Called again whenever the device changes in step 1; a choice that no
    // longer exists is dropped.
    void setChannelCounts (int numIn, int numOut);

    [[nodiscard]] Step getStep() const { return step_; }

    [[nodiscard]] std::vector<Choice> micChoices() const;
    // Same width as the chosen mic: a stereo mic pair goes to a stereo output
    // pair. Empty until a mic is chosen.
    [[nodiscard]] std::vector<Choice> outputChoices() const;

    void chooseMic (int index);
    void chooseOutput (int index);
    [[nodiscard]] int getMicIndex() const    { return mic_; }
    [[nodiscard]] int getOutputIndex() const { return out_; }

    [[nodiscard]] bool canAdvance() const;
    void next();
    void back();

    // Valid only when getStep() == Output and canAdvance().
    [[nodiscard]] SlotConfig result() const;

private:
    Step step_ = Step::Device;
    int numIn_ = 0, numOut_ = 0;
    int mic_ = -1, out_ = -1;
};
