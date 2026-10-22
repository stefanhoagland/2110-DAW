#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "TrackArea.h"
#include "Look.h"
#include "Console.h"
#include "Patch.h"
#include "NetworkView.h"
#include "Usage.h"

// The main window, laid out like the web version's Studio view: top bar with modes and
// transport, a setup bar (device, save folder), a toolbar, then the tracks and timeline.
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
    enum Mode { studioMode, liveMode, patchMode };
    void setMode (Mode);
    void savePatch();
    void rememberPatch();       // note which network channel feeds each track
    void followNetworkPatch();  // re-point those tracks after streams or the device change
    juce::StringArray patchIds; // per track: PatchView::networkId of its source, or empty
    void namesFollowPatch();    // tracks just patched or unpatched take their source's name
    void noteSources();
    bool hasSource (int track) const;
    int lastInputs = -1;
    std::vector<int> lastSources;
    // Show files: the tracks, patch, live mix, plugins and AES67 streams in one file.
    void showShowMenu();
    void saveShow (bool askWhere);
    void openShow();
    juce::String showToJson();
    void applyShow (const juce::var&);
    void refreshShowButton();
    juce::File showFile;
    void showTimecodeMenu();
    void showNetwork();
    void networkChanged();
    juce::String autoName (int track) const;
    bool looksAutomatic (const juce::String& saved, int track) const;
    void applyAutoNames();
    void applyTimecode (const juce::String& setting, bool save);
    void updateClock (bool rec, double secs);
    void updateUsage();
    void saveLive();
    juce::StringArray trackNames() const;
    int nextTakeNumber() const;

    juce::PropertiesFile& settings;
    AudioEngine engine;
    juce::File folder;
    int take = 0;
    double lastTakeSeconds = 0.0;
    int ticks = 0;

    WebLook look;
    std::unique_ptr<juce::Drawable> logo;
    juce::Label deviceInfo, folderInfo, clock, takeInfo, notice, tcInfo, usageInfo;
    AppUsage usage;
    juce::String tcSetting { "off" };
    juce::TextButton liveTab { "Live" }, studioTab { "Studio" }, soundcheckTab { "Soundcheck" }, patchTab { "Patch" };
    juce::TextButton rtzButton { juce::CharPointer_UTF8 ("\xe2\x8f\xae") }, stopButton { juce::CharPointer_UTF8 ("\xe2\x96\xa0") },
                     playButton { juce::CharPointer_UTF8 ("\xe2\x96\xb6") }, recordButton { juce::CharPointer_UTF8 ("\xe2\x97\x8f") };
    juce::TextButton showButton { "Show" }, audioButton { "Audio settings" }, timecodeButton { "Timecode: Off" }, networkButton { "AES67 network" }, folderButton { "Choose save folder" },
                     armAll { "Arm all" }, disarmAll { "Disarm all" },
                     importButton { "Import names" }, exportButton { "Export names" },
                     zoomOut { juce::CharPointer_UTF8 ("\xe2\x88\x92") }, zoomIn { "+" };
    juce::Label zoomLabel;
    juce::Rectangle<int> topBar, setupBar, toolBar, noticeBar, modesPill, clockBox;
    juce::OwnedArray<TrackRow> rows;
    TrackArea trackArea { rows };
    juce::Viewport viewport;
    PluginHost plugins { engine, settings };
    ConsoleView console { engine, plugins };
    MasterStrip master { engine };
    PatchView patchView { engine, [this] (int t) { return t < rows.size() ? rows[t]->getName() : juce::String(); } };
    Mode mode = studioMode;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this };
    juce::Component::SafePointer<juce::DialogWindow> networkWindow;
    juce::StringArray lastAuto; // each track's automatic name when last set

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
