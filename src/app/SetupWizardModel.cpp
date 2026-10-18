#include "app/SetupWizardModel.h"

std::vector<SetupWizardModel::Choice> SetupWizardModel::choicesFor (const int numChannels)
{
    std::vector<Choice> out;

    for (int ch = 0; ch < numChannels; ++ch)
        out.push_back ({ 1, { ch, ch } });

    for (int ch = 0; ch + 1 < numChannels; ch += 2)
        out.push_back ({ 2, { ch, ch + 1 } });

    return out;
}

bool SetupWizardModel::needsOverwriteConfirm (const std::array<SlotConfig, kMaxSlots>& current)
{
    int enabled = 0;
    for (const auto& c : current)
        enabled += c.enabled ? 1 : 0;
    return enabled > 1;
}

void SetupWizardModel::start (const int numIn, const int numOut)
{
    step_ = Step::Device;
    mic_  = -1;
    out_  = -1;
    setChannelCounts (numIn, numOut);
}

void SetupWizardModel::setChannelCounts (const int numIn, const int numOut)
{
    numIn_  = numIn  > 0 ? numIn  : 0;
    numOut_ = numOut > 0 ? numOut : 0;

    if (mic_ >= (int) micChoices().size())
        mic_ = -1;
    if (mic_ < 0 || out_ >= (int) outputChoices().size())
        out_ = -1;
}

std::vector<SetupWizardModel::Choice> SetupWizardModel::micChoices() const
{
    return choicesFor (numIn_);
}

std::vector<SetupWizardModel::Choice> SetupWizardModel::outputChoices() const
{
    const auto mics = micChoices();
    if (mic_ < 0 || mic_ >= (int) mics.size())
        return {};

    const int width = mics[(std::size_t) mic_].width;

    std::vector<Choice> out;
    for (const auto& c : choicesFor (numOut_))
        if (c.width == width)
            out.push_back (c);
    return out;
}

void SetupWizardModel::chooseMic (const int index)
{
    if (index < 0 || index >= (int) micChoices().size())
        return;

    // A different mic width invalidates the output picked for the old one.
    const int oldWidth = mic_ >= 0 ? micChoices()[(std::size_t) mic_].width : 0;
    mic_ = index;
    if (micChoices()[(std::size_t) mic_].width != oldWidth)
        out_ = -1;
}

void SetupWizardModel::chooseOutput (const int index)
{
    if (index >= 0 && index < (int) outputChoices().size())
        out_ = index;
}

bool SetupWizardModel::canAdvance() const
{
    switch (step_)
    {
        case Step::Device: return numIn_ > 0 && numOut_ > 0;
        case Step::Mic:    return mic_ >= 0;
        case Step::Output: return out_ >= 0;
    }
    return false;
}

void SetupWizardModel::next()
{
    if (! canAdvance())
        return;

    if (step_ == Step::Device)   step_ = Step::Mic;
    else if (step_ == Step::Mic) step_ = Step::Output;
}

void SetupWizardModel::back()
{
    if (step_ == Step::Output)   step_ = Step::Mic;
    else if (step_ == Step::Mic) step_ = Step::Device;
}

SlotConfig SetupWizardModel::result() const
{
    SlotConfig c;
    const auto mics = micChoices();
    const auto outs = outputChoices();
    if (mic_ < 0 || out_ < 0)
        return c;   // disabled default: nothing is routed by accident

    const auto& m = mics[(std::size_t) mic_];
    const auto& o = outs[(std::size_t) out_];

    c.enabled = true;
    c.width   = m.width;
    for (int lane = 0; lane < kMaxSlotLanes; ++lane)
    {
        c.inputChannels[lane]  = m.channels[lane];
        c.outputChannels[lane] = o.channels[lane];
    }
    return c;
}
