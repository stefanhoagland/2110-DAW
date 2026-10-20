#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Colours and sizes copied from the web version (css/style.css), so both apps look the same.
namespace theme
{
    const juce::Colour background { 0xff15171c }; // --bg
    const juce::Colour panel { 0xff1d2027 };      // --panel
    const juce::Colour panel2 { 0xff252932 };     // --panel-2 (buttons)
    const juce::Colour line { 0xff333845 };       // --line
    const juce::Colour text { 0xffe6e8ee };       // --text
    const juce::Colour textMuted { 0xff8b92a3 };  // --muted
    const juce::Colour accent { 0xff4fa3ff };     // --accent
    const juce::Colour accentText { 0xff0b1220 }; // text on accent buttons
    const juce::Colour record { 0xffff4757 };     // --rec
    const juce::Colour ok { 0xff2ed573 };         // --ok
    const juce::Colour clockBack { 0xff0c0d10 };
    const juce::Colour meterBack { 0xff16181d };
    const juce::Colour meterTick { 0xd90c0d10 };
    const juce::Colour meterGreen { 0xff2ed573 };
    const juce::Colour meterYellow { 0xfff5c542 };
    const juce::Colour meterRed { 0xffff4757 };
    const juce::Colour laneA { 0xff1b1e24 };
    const juce::Colour laneB { 0xff181a20 };
    const juce::Colour clipRecording { 0x2eff4757 };
    const juce::Colour clipDone { 0x294fa3ff };
    const juce::Colour waveRecording { 0xffff8a95 };
    const juce::Colour waveDone { 0xff7fbcff };

    constexpr int rowHeight = 64;    // --row-h
    constexpr int rulerHeight = 28;  // --ruler-h
    constexpr int headerWidth = 330; // --header-w
}
