#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"

// The AES67 window: which network port to use, the streams on the network (found by
// SAP) with a tick to receive each, their status, and a box to paste an SDP.
class NetworkView : public juce::Component, private juce::Timer
{
public:
    explicit NetworkView (AudioEngine&);
    void refresh(); // streams changed
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row : juce::Component
    {
        juce::ToggleButton receive;
        juce::Label name, detail, state;
        juce::String key;
        aes67::StreamInfo info;
        void resized() override;
    };
    void timerCallback() override;
    void updateStates();
    void showError (const juce::String&);
    void addFromText (const juce::String& sdp);

    AudioEngine& engine;
    juce::Label portLabel, latencyLabel, streamsLabel, hint, error, sdpLabel;
    juce::ComboBox port, latency;
    juce::OwnedArray<Row> rows;
    juce::Component list;
    juce::Viewport viewport;
    juce::TextEditor sdpText;
    juce::TextButton addButton { "Add stream" }, loadButton { "Load SDP file..." }, reportButton { "Copy report" };
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<aes67::Interface> interfaces;
};
