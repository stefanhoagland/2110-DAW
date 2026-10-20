#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Horizontal level meter (like the web track meter): bar with dB ticks, peak hold,
// clip light, and the held peak in dB on the right. Click to reset.
class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setLevel (float peak); // call ~30 times a second
    void mouseDown (const juce::MouseEvent&) override { clipped = false; hold = 0.0f; repaint(); }
    void paint (juce::Graphics&) override;

    static constexpr int barWidth = 128, readoutWidth = 36;

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

// One track header, laid out like the web version: number, name and record arm,
// with the input meter underneath. It also keeps the waveform of the current take.
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

    // Waveform of the current or last take: one peak per 1/wavesPerSecond s.
    static constexpr int wavesPerSecond = 30;
    void clearWave() { wave.clear(); inTake = false; }
    void startWave() { wave.clear(); inTake = true; }
    void addWave (double seconds, float peak);
    const std::vector<float>& getWave() const { return wave; }
    bool hasTake() const { return inTake; }

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
    std::vector<float> wave;
    bool inTake = false;
};
