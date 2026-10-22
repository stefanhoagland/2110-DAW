#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "Plugins.h"

// Vertical meter for a console strip: green/yellow/red bar, peak hold, clip light.
class VMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setLevel (float peak);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { clipped = false; hold = 0.0f; repaint(); }
    bool isClipped() const { return clipped; }

private:
    float level = 0.0f, hold = 0.0f;
    int holdFrames = 0;
    bool clipped = false;
};

// One plugin in a strip's insert list: click to open it, right-click for more.
class InsertItem : public juce::Component, public juce::SettableTooltipClient
{
public:
    InsertItem (PluginHost& h, int t, int i, const juce::String& n, bool bypassed)
        : host (h), track (t), index (i), name (n), bypass (bypassed) { setTooltip (n + " (click to open, right-click for more)"); }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    PluginHost& host;
    int track, index;
    juce::String name;
    bool bypass;
};

// One channel strip of the live console, like the web version's:
// gain, plugin inserts, pan, fader with dB scale and meter, mute and solo.
class ChannelStrip : public juce::Component
{
public:
    ChannelStrip (AudioEngine& engine, PluginHost& host, int channel);
    void refreshInserts();

    void setName (const juce::String& n) { name.setText (n, juce::dontSendNotification); }
    void setHasInput (bool has) { hasInput = has; setAlpha (has ? 1.0f : 0.5f); }
    void refreshFromEngine();  // after loading saved settings
    void tick();               // meters, ~30 times a second
    int channel() const { return index; }
    // -1: the fader is the channel fader. 0-11: the fader sets this channel's send to that aux.
    void setSendsOnFader (int aux);

    std::function<void()> onChange;

    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr int width = 92;

private:
    void push();

    AudioEngine& engine;
    PluginHost& host;
    int index;
    bool hasInput = true;
    juce::OwnedArray<InsertItem> insertItems;
    juce::TextButton addPlugin { "+ Plugin" };
    juce::Rectangle<int> insertArea;
    juce::Label number, name, gainLabel, gainValue, panLabel, panValue, dbValue;
    juce::Slider gain, pan, fader;
    juce::TextButton mute { "M" }, solo { "S" }, pre { "Pre fader" };
    juce::OwnedArray<juce::TextButton> mains; // assign to main 1-8
    juce::Label mainsLabel;
    int sendsAux = -1;
    VMeter meter;
    juce::Rectangle<int> scaleArea;
};

// A bus master strip in the Buses view: an aux (mono) or a main (stereo).
class BusStrip : public juce::Component
{
public:
    BusStrip (AudioEngine& engine, bool isMain, int index);
    void refreshFromEngine();
    void tick();
    std::function<void()> onChange;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    AudioEngine::Bus& bus() const { return isMain ? engine.mainBus (index) : engine.aux (index); }
    AudioEngine& engine;
    bool isMain;
    int index;
    juce::Label name, dbValue;
    juce::Slider fader;
    juce::TextButton mute { "M" };
    VMeter meterL, meterR;
    juce::Rectangle<int> scaleArea;
};

// The Live tab: the bar with Go live, output, limiter and latency, then the strips.
class ConsoleView : public juce::Component
{
public:
    ConsoleView (AudioEngine& engine, PluginHost& host);
    void refreshInserts (int track);

    void rebuild (int numChannels, const juce::StringArray& names);
    void setNames (const juce::StringArray& names);
    void refresh();  // output name, latency and button states
    void refreshBuses(); // after the master fader moved
    void tick();

    // Settings as JSON text (strips, limiter, master) and back.
    juce::String saveState() const;
    void loadState (const juce::String& json);

    std::function<void()> onChange;
    std::function<void (bool)> onGoLive;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    AudioEngine& engine;
    PluginHost& host;
    juce::TextButton goLive { "Go live" }, pluginsButton { "Plugins..." };
    juce::ToggleButton limiter { "Master limiter" };
    juce::Label status, output, latency, hint, faderLabel;
    juce::ComboBox faderMode; // what the channel faders control: main mix or an aux's sends
    juce::TextButton channelsTab { "Channels" }, busesTab { "Buses" };
    juce::Rectangle<int> viewPill;
    bool showBuses = false;
    juce::OwnedArray<ChannelStrip> strips;
    juce::OwnedArray<BusStrip> buses;
    juce::Component stripHolder, busHolder;
    juce::Viewport viewport, busViewport;
};

// The master section in the top bar: master fader and stereo output meter.
class MasterStrip : public juce::Component
{
public:
    explicit MasterStrip (AudioEngine& engine);
    void tick();
    void refreshFromEngine();
    std::function<void()> onChange;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    AudioEngine& engine;
    juce::Slider fader;
    float level[2] {}, hold[2] {};
    int holdFrames[2] {};
    bool clip[2] {};
};
