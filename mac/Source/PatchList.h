#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"

// Dante Controller-style input patching: the tracks down the left with what feeds
// each one, and the available channels on the right (the interface's inputs and
// every AES67 stream, received or only announced), with a filter box. Drag channels
// (or a whole stream) onto a track; several go onto that track and the ones after it.
// Double-click a channel to patch it to the selected track and move down one.
class PatchListView : public juce::Component, public juce::DragAndDropContainer
{
public:
    PatchListView (AudioEngine&, std::function<juce::String (int)> trackName);
    ~PatchListView() override;

    void refresh();
    std::function<void()> onChange;
    juce::Array<int> touched; // tracks the last patch or unpatch set (read in onChange)

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

    // Patches `sources` (see sourceIds) onto tracks from `firstTrack`. Announced streams
    // that aren't received yet are started. Returns an error, or empty.
    juce::String patch (const juce::StringArray& sources, int firstTrack);
    juce::String dragDescription() const; // the channels selected on the right
    void patchToSelectedTrack (const juce::StringArray& sources);

    // A patch source as text: "in:3" (input index) or "net:<stream key>#<channel>".
    juce::String sourceLabel (int input) const; // "In 3  Mic" or "3@Stage Box A"

private:
    class Tracks;
    class SourceItem;
    void rebuildTree();
    void unpatchSelected();

    AudioEngine& engine;
    std::function<juce::String (int)> trackName;
    juce::Label tracksTitle, sourcesTitle, hint, message;
    std::unique_ptr<Tracks> tracks;
    juce::TreeView tree;
    std::unique_ptr<juce::TreeViewItem> root;
    juce::TextEditor filter;
    juce::TextButton unpatch { "Unpatch" };
    juce::String treeShape;
};
