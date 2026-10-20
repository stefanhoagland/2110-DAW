#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Horizontal level meter with a dB scale, peak hold and clip light.
class LevelMeter : public juce::Component
{
public:
    void setLevel (float peak); // call ~30 times a second
    void mouseDown (const juce::MouseEvent&) override { clipped = false; hold = 0.0f; repaint(); }
    void paint (juce::Graphics&) override;

private:
    float level = 0.0f, hold = 0.0f;
    int holdFrames = 0;
    bool clipped = false;
};

// Name editor that moves to the next/previous track with Tab, Shift+Tab and Enter.
class NameEditor : public juce::TextEditor
{
public:
    std::function<void (int direction)> onMove;
    bool keyPressed (const juce::KeyPress&) override;
};

// One track: number, name, record arm and input meter.
class TrackRow : public juce::Component
{
public:
    explicit TrackRow (int channelIndex);

    int channel() const { return index; }
    juce::String getName() const { return name.getText(); }
    void setName (const juce::String& n) { name.setText (n, false); }
    bool isArmed() const { return arm.getToggleState(); }
    void setArmed (bool a) { arm.setToggleState (a, juce::dontSendNotification); }
    void setLevel (float peak) { meter.setLevel (peak); }
    void focusName() { name.grabKeyboardFocus(); name.selectAll(); }
    void setLocked (bool locked) { arm.setEnabled (! locked); }

    std::function<void()> onChange;
    std::function<void (int direction)> onMoveFocus;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    int index;
    juce::Label number;
    NameEditor name;
    juce::TextButton arm { juce::CharPointer_UTF8 ("\xe2\x97\x8f") };
    LevelMeter meter;
};
