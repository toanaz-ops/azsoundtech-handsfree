#include "MainComponent.h"

MainComponent::MainComponent()
{
    setSize(800, 600);
}

MainComponent::~MainComponent()
{
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    g.setColour(juce::Colours::white);
    g.setFont(20.0f);
    g.drawText("AZ Soundtech Hands-free", getLocalBounds(),
               juce::Justification::centred, true);
}

void MainComponent::resized()
{
    // Future components will be laid out here
}
