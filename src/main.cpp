#include <JuceHeader.h>
#include "app/MainComponent.h"

class HandsFreeApplication : public juce::JUCEApplication
{
public:
    HandsFreeApplication() = default;

    const juce::String getApplicationName() override
    {
        return "AZ Soundtech Hands-free";
    }

    const juce::String getApplicationVersion() override
    {
        return "1.0.0";
    }

    bool moreThanOneInstanceAllowed() override
    {
        return false;
    }

    void initialise(const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name)
            : DocumentWindow(name,
                             juce::Desktop::getInstance().getDefaultLookAndFeel()
                                 .findColour(juce::ResizableWindow::backgroundColourId),
                             DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);

            auto* content = new MainComponent();
            setContentOwned(content, true);

            setResizable(true, true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);

            // Opening the audio device is an explicit call, not a MainComponent
            // constructor side effect -- that is what keeps the whole component
            // constructible in a test on a machine with no audio hardware.
            // Done AFTER setVisible so a device that refuses to open leaves a
            // visible window showing why, rather than nothing at all.
            content->startAudio();
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(HandsFreeApplication)
