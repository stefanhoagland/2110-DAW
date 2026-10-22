#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "PatchList.h"

// An XY crosspoint grid: sources down the left, destinations across the top,
// a dot where they're connected. Click a cell to connect or disconnect;
// drag to do a run of cells. The row and column under the mouse are highlighted.
class PatchGrid : public juce::Component
{
public:
    std::function<int()> numRows, numCols;
    std::function<juce::String (int)> rowName, colName;
    std::function<bool (int row, int col)> isOn;
    std::function<void (int row, int col, bool on)> setOn;
    juce::String corner; // e.g. "Inputs ↓  Tracks →"

    void refresh();      // call when sizes or names change
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    static constexpr int cell = 22, rowHeader = 170, colHeader = 110;

private:
    juce::Point<int> cellAt (juce::Point<int>) const;
    juce::Point<int> scroll() const;
    juce::Point<int> hover { -1, -1 };
    bool dragValue = true;
    int dragRow = -1, dragCol = -1;
};

// Viewport that repaints the grid while scrolling, so its headers stay in view.
class PatchViewport : public juce::Viewport
{
public:
    void visibleAreaChanged (const juce::Rectangle<int>&) override
    {
        if (auto* c = getViewedComponent()) c->repaint();
    }
};

// The patch page: input patch (device inputs to tracks) and output patch
// (live mix and track direct outs to device outputs). Shared by every tab.
class PatchView : public juce::Component
{
public:
    PatchView (AudioEngine&, std::function<juce::String (int)> trackName);

    void refresh();
    std::function<void()> onChange;
    juce::Array<int> takeTouched() { auto t = list.touched; list.touched.clear(); return t; } // tracks patched in the list

    // How the input patch is shown: 0 = list (Dante style), 1 = XY grid.
    int getInputView() const { return inputView; }
    void setInputView (int);
    std::function<void()> onViewChange;

    // Settings text for the two patches, and back.
    static juce::String saveInputs (const AudioEngine&);
    static juce::String saveOutputs (const AudioEngine&);
    static void loadInputs (AudioEngine&, const juce::String&);
    static void loadOutputs (AudioEngine&, const juce::String&);

    // A network input as "net:<stream key>#<channel>" (empty for interface inputs), and
    // back to its current input number (-1 if that stream isn't received). Network inputs
    // move when streams are added or removed, so the patch follows them by these names.
    static juce::String networkId (const AudioEngine&, int input);
    static int networkInput (const AudioEngine&, const juce::String& id);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void showPage (int);
    juce::String inputName (int) const;
    juce::String outputName (int) const;

    AudioEngine& engine;
    std::function<juce::String (int)> trackName;
    juce::TextButton inputsTab { "Inputs to tracks" }, outputsTab { "Outputs" },
                     oneToOne { "Reset to 1:1" }, clearAll { "Clear all" },
                     listButton { "List" }, gridButton { "Grid" };
    juce::Label help;
    PatchGrid inGrid, outGrid;
    PatchViewport inView, outView;
    PatchListView list;
    juce::Rectangle<int> pill, viewPill;
    int page = 0, inputView = 0;
};
