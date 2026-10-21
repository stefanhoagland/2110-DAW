#include "Look.h"
#include "Theme.h"

using namespace juce;

static String styleOf (const Component& b) { return b.getProperties()["style"].toString(); }

WebLook::WebLook()
{
    setColourScheme (getMidnightColourScheme());
    setColour (ResizableWindow::backgroundColourId, theme::background);
    setColour (TextButton::buttonColourId, theme::panel2);
    setColour (TextButton::textColourOffId, theme::text);
    setColour (TextButton::textColourOnId, Colours::white);
    setColour (TextEditor::backgroundColourId, theme::background);
    setColour (TextEditor::textColourId, theme::text);
    setColour (TextEditor::highlightColourId, theme::accent.withAlpha (0.35f));
    setColour (TextEditor::outlineColourId, theme::line);
    setColour (TextEditor::focusedOutlineColourId, theme::accent);
    setColour (CaretComponent::caretColourId, theme::text);
    setColour (Label::textColourId, theme::text);
    setColour (TooltipWindow::backgroundColourId, theme::panel2);
    setColour (TooltipWindow::textColourId, theme::text);
    setColour (TooltipWindow::outlineColourId, theme::line);
    setColour (ScrollBar::thumbColourId, theme::line);
    setColour (PopupMenu::backgroundColourId, theme::panel2);
    setColour (PopupMenu::textColourId, theme::text);
    setColour (PopupMenu::headerTextColourId, theme::textMuted);
    setColour (PopupMenu::highlightedBackgroundColourId, theme::accent);
    setColour (PopupMenu::highlightedTextColourId, theme::accentText);
    setColour (AlertWindow::backgroundColourId, theme::panel);
    setColour (AlertWindow::textColourId, theme::text);
    setColour (ListBox::backgroundColourId, theme::background);
    setColour (ListBox::textColourId, theme::text);
    setColour (TableHeaderComponent::backgroundColourId, theme::panel2);
    setColour (TableHeaderComponent::textColourId, theme::text);
}

void WebLook::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool over, bool down)
{
    const auto style = styleOf (b);
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    const float alpha = b.isEnabled() ? 1.0f : 0.45f;

    if (style == "mode")
    {
        // Tabs inside the dark pill: transparent, or filled with the accent when active.
        if (on)
        {
            g.setColour (theme::accent);
            g.fillRoundedRectangle (r, 6.0f);
        }
        else if (over && b.isEnabled())
        {
            g.setColour (theme::panel2);
            g.fillRoundedRectangle (r, 6.0f);
        }
        return;
    }

    Colour fill = theme::panel2, border = theme::line;
    if (style == "primary") fill = border = theme::accent;
    if (on)
    {
        fill = b.findColour (TextButton::buttonOnColourId);
        border = fill;
    }
    if (down && b.isEnabled()) fill = fill.brighter (0.08f);
    if (over && b.isEnabled() && ! on && style != "primary") border = theme::accent;

    g.setColour (fill.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (border.withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
}

Font WebLook::getTextButtonFont (TextButton& b, int)
{
    const auto style = styleOf (b);
    if (style == "transport") return FontOptions (15.0f);
    if (style == "small") return FontOptions (11.0f, Font::bold);
    if (style == "small-plain") return FontOptions (11.0f);
    if (style == "primary" || style == "bold" || (style == "mode" && b.getToggleState())) return FontOptions (13.0f, Font::bold);
    return FontOptions (13.0f);
}

void WebLook::drawButtonText (Graphics& g, TextButton& b, bool, bool)
{
    const auto style = styleOf (b);
    Colour c = theme::text;
    if (style == "primary" || (style == "mode" && b.getToggleState())) c = theme::accentText;
    else if (style == "mode") c = theme::text.withAlpha (0.85f);
    if (b.getToggleState() && style != "mode") c = b.findColour (TextButton::textColourOnId);
    else if (b.getProperties().contains ("textColour")) c = Colour ((uint32) (int64) b.getProperties()["textColour"]);
    if (! b.isEnabled()) c = c.withMultipliedAlpha (style == "mode" ? 0.5f : 0.45f);

    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (c);
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 0), Justification::centred, 1);
}

void WebLook::fillTextEditorBackground (Graphics& g, int w, int h, TextEditor& e)
{
    g.setColour (e.findColour (TextEditor::backgroundColourId));
    g.fillRoundedRectangle (Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f), 4.0f);
}

void WebLook::drawTextEditorOutline (Graphics& g, int w, int h, TextEditor& e)
{
    g.setColour (e.hasKeyboardFocus (true) && ! e.isReadOnly() ? theme::accent : theme::line);
    g.drawRoundedRectangle (Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f), 4.0f, 1.0f);
}

void WebLook::drawScrollbar (Graphics& g, ScrollBar&, int x, int y, int w, int h, bool vertical,
                             int thumbStart, int thumbSize, bool over, bool down)
{
    Rectangle<float> thumb = vertical ? Rectangle<float> ((float) x + 2, (float) thumbStart, (float) w - 4, (float) thumbSize)
                                      : Rectangle<float> ((float) thumbStart, (float) y + 2, (float) thumbSize, (float) h - 4);
    g.setColour (over || down ? theme::textMuted : theme::line);
    g.fillRoundedRectangle (thumb, 3.0f);
}

int WebLook::getSliderThumbRadius (Slider& s)
{
    const auto style = styleOf (s);
    if (style == "fader") return 23;  // 46 px cap, like the web .fader thumb
    if (style == "hfader") return 7;  // 14 px cap
    return 6;
}

// The brushed-metal cap from the web CSS (.fader / .hfader thumbs).
static void drawCap (Graphics& g, Rectangle<float> cap, bool vertical, bool focused)
{
    g.setColour (Colours::black.withAlpha (0.6f));
    g.fillRoundedRectangle (cap.translated (0.0f, vertical ? 3.0f : 2.0f), 4.0f);

    ColourGradient metal (Colour (0xff8d939e), 0, 0, Colour (0xff7b818c), 0, 0, false);
    if (vertical) { metal.point1 = cap.getTopLeft(); metal.point2 = cap.getBottomLeft(); }
    else          { metal.point1 = cap.getTopLeft(); metal.point2 = cap.getTopRight(); }
    metal.addColour (0.18, Colour (0xffd4d8df));
    metal.addColour (0.5, Colour (0xff6f7580));
    metal.addColour (0.82, Colour (0xffc3c8d0));
    g.setGradientFill (metal);
    g.fillRoundedRectangle (cap, vertical ? 4.0f : 3.0f);

    if (vertical)
    {
        g.setColour (Colours::black.withAlpha (0.18f));
        for (float y = cap.getY() + 3.0f; y < cap.getBottom() - 1.0f; y += 4.0f)
            g.fillRect (cap.getX() + 1.0f, y, cap.getWidth() - 2.0f, 1.0f);
        g.setColour (Colour (0xffe9ecf2));
        g.fillRect (cap.getX(), cap.getCentreY() - 1.0f, cap.getWidth(), 2.0f);
    }
    else
    {
        g.setColour (Colour (0xffe9ecf2));
        g.fillRect (cap.getCentreX() - 1.0f, cap.getY(), 2.0f, cap.getHeight());
    }
    g.setColour (focused ? theme::accent : Colour (0xff0b0c0f));
    g.drawRoundedRectangle (cap.reduced (0.5f), vertical ? 4.0f : 3.0f, 1.0f);
}

void WebLook::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                Slider::SliderStyle, Slider& s)
{
    const auto style = styleOf (s);
    const auto b = Rectangle<int> (x, y, w, h).toFloat();
    if (style == "fader")
    {
        auto slot = Rectangle<float> (b.getCentreX() - 3.0f, 0.0f, 6.0f, (float) s.getHeight());
        ColourGradient grad (Colour (0xff050608), slot.getX(), 0, Colour (0xff050608), slot.getRight(), 0, false);
        grad.addColour (0.5, Colour (0xff1a1d23));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (slot, 3.0f);
        g.setColour (Colour (0xff2a2e37));
        g.drawRoundedRectangle (slot.expanded (0.5f), 3.0f, 1.0f);
        drawCap (g, Rectangle<float> (30.0f, 46.0f).withCentre ({ b.getCentreX(), pos }), true, s.hasKeyboardFocus (false));
        return;
    }
    if (style == "hfader")
    {
        auto slot = Rectangle<float> (0.0f, b.getCentreY() - 2.5f, (float) s.getWidth(), 5.0f);
        ColourGradient grad (Colour (0xff050608), 0, slot.getY(), Colour (0xff050608), 0, slot.getBottom(), false);
        grad.addColour (0.5, Colour (0xff1a1d23));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (slot, 2.5f);
        g.setColour (Colour (0xff2a2e37));
        g.drawRoundedRectangle (slot.expanded (0.5f), 2.5f, 1.0f);
        drawCap (g, Rectangle<float> (14.0f, 20.0f).withCentre ({ pos, b.getCentreY() }), false, s.hasKeyboardFocus (false));
        return;
    }
    // Thin range control: grey track, blue fill up to a round blue thumb.
    const float cy = b.getCentreY();
    const float alpha = s.isEnabled() ? 1.0f : 0.45f;
    g.setColour (theme::line.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (0.0f, cy - 2.0f, (float) s.getWidth(), 4.0f, 2.0f);
    g.setColour (Colour (0xff9cc8ff).withMultipliedAlpha (alpha));
    if (style == "pan")
    {
        // Pan fills out from the centre towards the side it leans to, with a centre mark.
        const float mid = b.getCentreX(); // where 0 (centre) sits
        g.fillRoundedRectangle (jmin (mid, pos), cy - 2.0f, std::abs (pos - mid), 4.0f, 2.0f);
        g.setColour (theme::textMuted.withMultipliedAlpha (alpha));
        g.fillRect (mid - 0.5f, cy - 5.0f, 1.0f, 10.0f);
    }
    else
        g.fillRoundedRectangle (0.0f, cy - 2.0f, pos, 4.0f, 2.0f);
    g.setColour (theme::accent.withMultipliedAlpha (alpha));
    g.fillEllipse (Rectangle<float> (12.0f, 12.0f).withCentre ({ pos, cy }));
}

void WebLook::drawTickBox (Graphics& g, Component&, float x, float y, float w, float h,
                           bool ticked, bool enabled, bool, bool)
{
    auto box = Rectangle<float> (x, y, w, h).withSizeKeepingCentre (14.0f, 14.0f);
    const float alpha = enabled ? 1.0f : 0.45f;
    g.setColour ((ticked ? theme::accent : theme::panel2).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (box, 3.0f);
    g.setColour ((ticked ? theme::accent : theme::line).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (box, 3.0f, 1.0f);
    if (ticked)
    {
        Path tick;
        tick.startNewSubPath (box.getX() + 3.5f, box.getCentreY());
        tick.lineTo (box.getX() + 6.0f, box.getBottom() - 4.0f);
        tick.lineTo (box.getRight() - 3.0f, box.getY() + 3.5f);
        g.setColour (theme::accentText);
        g.strokePath (tick, PathStrokeType (2.0f));
    }
}
