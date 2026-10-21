#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"

// Hosts third-party VST3 and Audio Unit plugins on the live console's strips:
// the list of installed plugins (with a scanner), loading plugins into a track's
// insert chain, their editor windows, and saving every insert with its settings.
class PluginHost : private juce::ChangeListener
{
public:
    PluginHost (AudioEngine&, juce::PropertiesFile&);
    ~PluginHost() override;

    // "+ Plugin" menu for a track: installed plugins by maker, then "Scan for plugins...".
    void showAddMenu (int track, juce::Component& target);
    // Right-click menu for an insert: open, bypass, move, remove.
    void showInsertMenu (int track, int index, juce::Component& target);
    void openEditor (int track, int index);
    void toggleBypass (int track, int index);
    void showScanner();
    bool hasPlugins() const { return known.getNumTypes() > 0; }

    juce::String saveInserts() const; // every track's inserts with their settings (JSON)
    void loadInserts (const juce::String& json);
    void save();                      // writes saveInserts() to the settings

    std::function<void (int track)> onInsertsChanged;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void add (int track, const juce::PluginDescription&, const juce::MemoryBlock& state, bool bypass, bool saveAfter);
    void remove (int track, int index);
    void closeEditorFor (juce::AudioProcessor*);
    void changed (int track);

    class EditorWindow;
    AudioEngine& engine;
    juce::PropertiesFile& settings;
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    juce::OwnedArray<EditorWindow> editors;
    std::unique_ptr<juce::DocumentWindow> scanner;
    int pending = 0; // plugins still loading
    std::function<void()> loadNext; // next plugin of a saved session
};
