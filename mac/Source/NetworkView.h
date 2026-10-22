#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"

// The AES67 windows: which network port to use, then one of two pages.
// Receivers: the streams on the network (found by SAP) with a tick to receive each, their
// status, and a box to paste an SDP.
// Transmitters: the PTP clock, the streams this Mac sends, and a form to create one.
class NetworkView : public juce::Component, private juce::Timer
{
public:
    // page 0: receivers, 1: transmitters
    NetworkView (AudioEngine&, int page);
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
    juce::String tracksFedBy (int firstInput, int channels) const;
    void showError (const juce::String&);
    void addFromText (const juce::String& sdp);
    void showTab (int t);
    void refreshSend();
    void updateSendStates();
    void createStream();
    void layoutSend (juce::Rectangle<int>);

    struct SendRow : juce::Component
    {
        juce::Label name, detail, state;
        juce::TextButton copy { "Copy SDP" }, remove { "Remove" };
        juce::String id;
        void resized() override;
    };
    AudioEngine& engine;
    juce::Label portLabel, latencyLabel, streamsLabel, hint, error, sdpLabel;
    juce::ComboBox port, latency;
    juce::OwnedArray<Row> rows;
    juce::Component list;
    juce::Viewport viewport;
    juce::TextEditor sdpText, search;
    juce::TextButton addButton { "Add stream" }, loadButton { "Load SDP file..." }, reportButton { "Copy report" };
    std::unique_ptr<juce::FileChooser> chooser;

    int tab = 0;
    juce::Label ptpState, domainLabel, sendLabel, sendHint, newLabel, nameLabel, chLabel, ptimeLabel, addrLabel, feedLabel;
    juce::ComboBox domain, channels, ptime, feed;
    juce::TextEditor nameEdit, addrEdit;
    juce::TextButton createButton { "Create stream" };
    juce::OwnedArray<SendRow> sendRows;
    juce::Component sendList;
    juce::Viewport sendViewport;
    std::vector<int> feedSources; // sources in order: mains L/R, auxes, track directs
    std::vector<aes67::Interface> interfaces;
};
