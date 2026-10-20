#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "TrackRow.h"

// The recorder window: device and folder controls, transport, and one row per input.
class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    explicit MainComponent (juce::PropertiesFile& settings);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool canQuit(); // asks before quitting during a take

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void rebuildTracks();
    void saveTracks();
    void saveDevice();
    void showAudioSettings();
    void chooseFolder();
    void importNames();
    void exportNames();
    void toggleRecord();
    void startRecording();
    void stopRecording();
    void setStatus (const juce::String&, bool error = false);
    void refreshLabels();
    int nextTakeNumber() const;

    juce::PropertiesFile& settings;
    AudioEngine engine;
    juce::File folder;
    int take = 0;

    juce::Label title, deviceInfo, folderInfo, clock, status;
    juce::TextButton audioButton { "Audio settings" }, folderButton { "Save folder" },
                     armAll { "Arm all" }, disarmAll { "Disarm all" },
                     importButton { "Import names" }, exportButton { "Export names" },
                     recordButton { "Record" };
    juce::OwnedArray<TrackRow> rows;
    juce::Component rowHolder;
    juce::Viewport viewport;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
