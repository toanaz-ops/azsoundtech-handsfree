#include <JuceHeader.h>
#include "app/MainComponent.h"
#include "app/SessionLogger.h"

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
        return JUCE_APPLICATION_VERSION_STRING;   // the one place the number lives: CMakeLists project()
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
            content->setAppVersion(JUCE_APPLICATION_VERSION_STRING);
            setContentOwned(content, true);

            setResizable(true, true);
            // Minimum window: must fit the 96 px rail above a usable spectrum
            // with no overlap (spec 2026-08-23 section 3).
            // Lives here because setResizeLimits is a ResizableWindow API.
            setResizeLimits(MainComponent::kMinimumWidth,
                            MainComponent::kMinimumHeight,
                            16384, 16384);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);

            // Opening the audio device is an explicit call, not a MainComponent
            // constructor side effect -- that is what keeps the whole component
            // constructible in a test on a machine with no audio hardware.
            // Done AFTER setVisible so a device that refuses to open leaves a
            // visible window showing why, rather than nothing at all.
            content->startAudio();
            // Lane D: the session log opens AFTER the device, so its header
            // names the device actually in use. A failed start (no %APPDATA%)
            // is not fatal -- the app runs without a log.
            // I-3: a refused start is not fatal, but it must not be silent --
            // the operator would otherwise finish a show believing there is a
            // log to look at. ASCII in the literal on purpose: MSVC's default
            // execution charset mangles non-ASCII source literals.
            if (! content->startSessionLog(SessionLogger::defaultDirectory()))
            {
                const auto where = SessionLogger::defaultDirectory().getFullPathName();
                juce::Logger::writeToLog("session log: could not open " + where);
                content->showMessage("LOG: khong ghi duoc " + where);
            }
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
