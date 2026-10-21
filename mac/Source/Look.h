#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Buttons, text boxes and scroll bars styled like the web version.
// Button styles are picked with the "style" property: "primary", "mode" or "transport".
class WebLook : public juce::LookAndFeel_V4
{
public:
    WebLook();

    static void setStyle (juce::Button& b, const char* style) { b.getProperties().set ("style", style); }

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override;
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                        int thumbStart, int thumbSize, bool over, bool down) override;
    int getDefaultScrollbarWidth() override { return 10; }

    // Sliders: "fader" (vertical console fader with a metal cap), "hfader" (horizontal,
    // smaller cap) or the default thin blue range control the web uses for gain and pan.
    static void setStyle (juce::Slider& s, const char* style) { s.getProperties().set ("style", style); }
    int getSliderThumbRadius (juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float min, float max,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h,
                      bool ticked, bool enabled, bool over, bool down) override;
};
