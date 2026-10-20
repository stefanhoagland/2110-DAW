#include "Look.h"
#include "Theme.h"

using namespace juce;

static String styleOf (const Button& b) { return b.getProperties()["style"].toString(); }

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
    if (style == "primary" || (style == "mode" && b.getToggleState())) return FontOptions (13.0f, Font::bold);
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
