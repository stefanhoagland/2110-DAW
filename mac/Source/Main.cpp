#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "Theme.h"

class DawApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String&) override
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Soundbending Live";
        o.filenameSuffix = ".settings";
        o.folderName = "Soundbending Live";
        o.osxLibrarySubFolder = "Application Support";
        // Bring settings over from before the app was renamed (it was "2110 DAW").
        auto newFile = o.getDefaultFile();
        auto old = o;
        old.applicationName = old.folderName = "2110 DAW";
        if (! newFile.existsAsFile() && old.getDefaultFile().existsAsFile())
        {
            newFile.getParentDirectory().createDirectory();
            old.getDefaultFile().copyFileTo (newFile);
        }
        properties.setStorageParameters (o);

        auto& lf = juce::LookAndFeel::getDefaultLookAndFeel();
        if (auto* v4 = dynamic_cast<juce::LookAndFeel_V4*> (&lf))
            v4->setColourScheme (juce::LookAndFeel_V4::getMidnightColourScheme());
        lf.setColour (juce::ResizableWindow::backgroundColourId, theme::background);

        window = std::make_unique<MainWindow> (getApplicationName(), *properties.getUserSettings());
    }

    void shutdown() override
    {
        window = nullptr;
        properties.closeFiles();
    }

    void systemRequestedQuit() override
    {
        if (window == nullptr || window->content->canQuit())
            quit();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, juce::PropertiesFile& settings)
            : DocumentWindow (name, theme::background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            content = new MainComponent (settings);
            setContentOwned (content, true);
            setResizable (true, true);
            setResizeLimits (760, 420, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        MainComponent* content = nullptr;
    };

    juce::ApplicationProperties properties;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (DawApplication)
