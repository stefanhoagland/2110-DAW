#include "Patch.h"
#include "Look.h"
#include "Theme.h"

using namespace juce;

void PatchGrid::refresh()
{
    const int rows = numRows ? numRows() : 0, cols = numCols ? numCols() : 0;
    setSize (rowHeader + cols * cell + 1, colHeader + rows * cell + 1);
    repaint();
}

Point<int> PatchGrid::scroll() const
{
    if (auto* vp = findParentComponentOfClass<Viewport>())
        return vp->getViewPosition();
    return {};
}

Point<int> PatchGrid::cellAt (Point<int> p) const
{
    const auto s = scroll();
    // Clicks on the sticky headers aren't cells.
    if (p.x < s.x + rowHeader || p.y < s.y + colHeader) return { -1, -1 };
    const int c = (p.x - rowHeader) / cell, r = (p.y - colHeader) / cell;
    if (r < 0 || c < 0 || r >= numRows() || c >= numCols()) return { -1, -1 };
    return { c, r };
}

void PatchGrid::mouseMove (const MouseEvent& e)
{
    auto h = cellAt (e.getPosition());
    if (h != hover) { hover = h; repaint(); }
}

void PatchGrid::mouseExit (const MouseEvent&)
{
    hover = { -1, -1 };
    repaint();
}

void PatchGrid::mouseDown (const MouseEvent& e)
{
    auto c = cellAt (e.getPosition());
    if (c.x < 0) return;
    dragValue = ! isOn (c.y, c.x);
    dragRow = c.y;
    dragCol = c.x;
    setOn (c.y, c.x, dragValue);
    repaint();
}

void PatchGrid::mouseDrag (const MouseEvent& e)
{
    auto c = cellAt (e.getPosition());
    hover = c;
    if (c.x >= 0 && (c.y != dragRow || c.x != dragCol))
    {
        dragRow = c.y;
        dragCol = c.x;
        if (isOn (c.y, c.x) != dragValue) setOn (c.y, c.x, dragValue);
    }
    repaint();
}

void PatchGrid::paint (Graphics& g)
{
    const int rows = numRows(), cols = numCols();
    const auto s = scroll();
    const auto clip = g.getClipBounds();
    g.fillAll (theme::background);

    const int c0 = jmax (0, (clip.getX() - rowHeader) / cell), c1 = jmin (cols, (clip.getRight() - rowHeader) / cell + 1);
    const int r0 = jmax (0, (clip.getY() - colHeader) / cell), r1 = jmin (rows, (clip.getBottom() - colHeader) / cell + 1);

    // Cells: groups of 8 shaded alternately, crosshair on the hovered row and column.
    for (int r = r0; r < r1; ++r)
        for (int c = c0; c < c1; ++c)
        {
            Rectangle<int> b (rowHeader + c * cell, colHeader + r * cell, cell, cell);
            const bool shade = ((r / 8) + (c / 8)) % 2 == 0;
            Colour fill = shade ? theme::laneA : theme::laneB;
            if (r == hover.y || c == hover.x) fill = fill.brighter (0.12f);
            g.setColour (fill);
            g.fillRect (b);
            if (isOn (r, c))
            {
                g.setColour (theme::accent);
                g.fillEllipse (b.toFloat().reduced (5.0f));
            }
        }
    g.setColour (theme::line);
    for (int r = r0; r <= r1; ++r)
        g.fillRect (rowHeader + c0 * cell, colHeader + r * cell, (c1 - c0) * cell, r % 8 == 0 ? 2 : 1);
    for (int c = c0; c <= c1; ++c)
        g.fillRect (rowHeader + c * cell, colHeader + r0 * cell, c % 8 == 0 ? 2 : 1, (r1 - r0) * cell);

    // Row names, pinned to the left edge of the view.
    g.setFont (FontOptions (12.0f));
    g.setColour (theme::panel);
    g.fillRect (s.x, colHeader, rowHeader, rows * cell);
    for (int r = r0; r < r1; ++r)
    {
        Rectangle<int> b (s.x, colHeader + r * cell, rowHeader, cell);
        if (r == hover.y) { g.setColour (theme::panel2); g.fillRect (b); }
        g.setColour (r == hover.y ? theme::text : theme::textMuted);
        g.drawText (rowName (r), b.reduced (10, 0), Justification::centredLeft, true);
    }

    // Column names, written upwards, pinned to the top of the view.
    g.setColour (theme::panel);
    g.fillRect (rowHeader, s.y, cols * cell, colHeader);
    for (int c = c0; c < c1; ++c)
    {
        const int x = rowHeader + c * cell;
        if (c == hover.x) { g.setColour (theme::panel2); g.fillRect (x, s.y, cell, colHeader); }
        g.setColour (c == hover.x ? theme::text : theme::textMuted);
        Graphics::ScopedSaveState save (g);
        g.addTransform (AffineTransform::rotation (-MathConstants<float>::halfPi, (float) x, (float) (s.y + colHeader)));
        g.drawText (colName (c), x + 8, s.y + colHeader, colHeader - 12, cell, Justification::centredLeft, true);
    }
    g.setColour (theme::line);
    g.fillRect (s.x + rowHeader - 1, s.y, 1, getHeight());
    g.fillRect (s.x, s.y + colHeader - 1, getWidth(), 1);

    // Corner
    g.setColour (theme::panel);
    g.fillRect (s.x, s.y, rowHeader - 1, colHeader - 1);
    g.setColour (theme::textMuted);
    g.setFont (FontOptions (12.0f));
    g.drawFittedText (corner, Rectangle<int> (s.x, s.y, rowHeader, colHeader).reduced (10), Justification::bottomLeft, 3);
}

//==============================================================================
PatchView::PatchView (AudioEngine& e, std::function<String (int)> names)
    : engine (e), trackName (std::move (names)), list (e, trackName)
{
    addChildComponent (list);
    list.onChange = [this] { inGrid.refresh(); if (onChange) onChange(); };
    for (auto* b : { &listButton, &gridButton })
    {
        WebLook::setStyle (*b, "mode");
        addAndMakeVisible (*b);
    }
    listButton.setTooltip ("Tracks and available channels side by side, like Dante Controller");
    gridButton.setTooltip ("XY crosspoint grid");
    listButton.onClick = [this] { setInputView (0); if (onViewChange) onViewChange(); };
    gridButton.onClick = [this] { setInputView (1); if (onViewChange) onViewChange(); };

    for (auto* t : { &inputsTab, &outputsTab })
    {
        WebLook::setStyle (*t, "mode");
        addAndMakeVisible (*t);
    }
    inputsTab.onClick = [this] { showPage (0); };
    outputsTab.onClick = [this] { showPage (1); };
    oneToOne.setTooltip ("Input 1 to track 1, input 2 to track 2 and so on; live mix to outputs 1 and 2");
    oneToOne.onClick = [this]
    {
        engine.resetPatches();
        refresh();
        if (onChange) onChange();
    };
    clearAll.onClick = [this]
    {
        if (page == 0) for (int t = 0; t < AudioEngine::maxChannels; ++t) engine.setTrackSource (t, -1);
        else for (int s = 0; s < AudioEngine::numSources; ++s) for (int o = 0; o < AudioEngine::maxChannels; ++o) engine.setPatch (s, o, false);
        refresh();
        if (onChange) onChange();
    };
    for (auto* b : { &oneToOne, &clearAll }) addAndMakeVisible (*b);
    help.setFont (FontOptions (13.0f));
    help.setColour (Label::textColourId, theme::textMuted);
    addAndMakeVisible (help);

    // Inputs: one source per track (picking a new one moves the dot).
    inGrid.numRows = [this] { return engine.numInputs(); };
    inGrid.numCols = [] { return AudioEngine::numTracks; };
    inGrid.rowName = [this] (int r) { return inputName (r); };
    inGrid.colName = [this] (int c) { return String (c + 1) + "  " + trackName (c); };
    inGrid.isOn = [this] (int r, int c) { return engine.getTrackSource (c) == r; };
    inGrid.setOn = [this] (int r, int c, bool on)
    {
        if (on) engine.setTrackSource (c, r);
        else if (engine.getTrackSource (c) == r) engine.setTrackSource (c, -1);
        if (onChange) onChange();
    };
    inGrid.corner = String (CharPointer_UTF8 ("Interface inputs \xe2\x86\x93\nTracks \xe2\x86\x92"));

    // Outputs: any source to any output; several sources on one output are mixed.
    // Rows: mains 1-8 (left, right), auxes 1-12, then each track's direct signal.
    constexpr int mainRows = 2 * AudioEngine::numMains, busRows = mainRows + AudioEngine::numAux;
    auto source = [] (int r)
    {
        if (r < mainRows) return AudioEngine::mainSource (r / 2, r % 2);
        if (r < busRows) return AudioEngine::firstAux + (r - mainRows);
        return AudioEngine::firstDirect + (r - busRows);
    };
    outGrid.numRows = [] { return busRows + AudioEngine::numTracks; };
    outGrid.numCols = [this] { return engine.numOutputs(); };
    outGrid.rowName = [this] (int r)
    {
        if (r < mainRows)
        {
            const auto label = engine.busLabel (true, r / 2);
            return "Main " + String (r / 2 + 1) + (r % 2 == 0 ? " L" : " R") + (label.isNotEmpty() ? "  " + label : String())
                   + (r < 2 ? "  (master)" : "");
        }
        if (r < busRows) return engine.busFullName (false, r - mainRows);
        const int t = r - busRows;
        return "Track " + String (t + 1) + " direct  " + trackName (t);
    };
    outGrid.colName = [this] (int c) { return outputName (c); };
    outGrid.isOn = [this, source] (int r, int c) { return engine.getPatch (source (r), c); };
    outGrid.setOn = [this, source] (int r, int c, bool on) { engine.setPatch (source (r), c, on); if (onChange) onChange(); };
    outGrid.corner = String (CharPointer_UTF8 ("Sources \xe2\x86\x93\nInterface outputs \xe2\x86\x92"));

    for (auto* v : { &inView, &outView })
    {
        v->setScrollBarsShown (true, true);
        addChildComponent (*v);
    }
    inView.setViewedComponent (&inGrid, false);
    outView.setViewedComponent (&outGrid, false);
    showPage (0);
}

String PatchView::inputName (int i) const
{
    if (i >= engine.numDeviceInputs())
        return "AES67  " + engine.inputName (i);
    auto* dev = engine.devices().getCurrentAudioDevice();
    auto names = dev != nullptr ? dev->getInputChannelNames() : StringArray();
    const auto n = names[i];
    return "In " + String (i + 1) + (n.isNotEmpty() && n != String (i + 1) ? "  " + n : String());
}

String PatchView::outputName (int i) const
{
    auto* dev = engine.devices().getCurrentAudioDevice();
    auto names = dev != nullptr ? dev->getOutputChannelNames() : StringArray();
    const auto n = names[i];
    return "Out " + String (i + 1) + (n.isNotEmpty() && n != String (i + 1) ? "  " + n : String());
}

void PatchView::setInputView (int v)
{
    inputView = v == 1 ? 1 : 0;
    showPage (page);
}

void PatchView::showPage (int p)
{
    page = p;
    inputsTab.setToggleState (p == 0, dontSendNotification);
    outputsTab.setToggleState (p == 1, dontSendNotification);
    listButton.setToggleState (inputView == 0, dontSendNotification);
    gridButton.setToggleState (inputView == 1, dontSendNotification);
    listButton.setVisible (p == 0);
    gridButton.setVisible (p == 0);
    list.setVisible (p == 0 && inputView == 0);
    inView.setVisible (p == 0 && inputView == 1);
    outView.setVisible (p == 1);
    help.setText (p == 0 ? (inputView == 0 ? String() : "Pick which interface input feeds each track. Studio records and Live mixes the tracks.")
                         : "Send the mains, the auxes or any track's input straight to interface outputs.",
                  dontSendNotification);
    refresh();
}

void PatchView::refresh()
{
    if (page == 0 && inputView == 0) list.refresh();
    inGrid.refresh();
    outGrid.refresh();
    resized();
}

void PatchView::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (46).reduced (16, 6).withTrimmedBottom (1);
    auto width = [] (TextButton& b) { return GlyphArrangement::getStringWidthInt (FontOptions (13.0f, Font::bold), b.getButtonText()) + 30; };
    const int pw = width (inputsTab) + width (outputsTab) + 4 + 6;
    pill = bar.removeFromLeft (pw).withSizeKeepingCentre (pw, 34);
    auto p = pill.reduced (3);
    inputsTab.setBounds (p.removeFromLeft (width (inputsTab)));
    p.removeFromLeft (4);
    outputsTab.setBounds (p);
    bar.removeFromLeft (12);
    if (page == 0)
    {
        const int vw = width (listButton) + width (gridButton) + 4 + 6;
        viewPill = bar.removeFromLeft (vw).withSizeKeepingCentre (vw, 34);
        auto v = viewPill.reduced (3);
        listButton.setBounds (v.removeFromLeft (width (listButton)));
        v.removeFromLeft (4);
        gridButton.setBounds (v);
        bar.removeFromLeft (12);
    }
    else viewPill = {};
    clearAll.setBounds (bar.removeFromRight (width (clearAll)).reduced (0, 2));
    bar.removeFromRight (10);
    oneToOne.setBounds (bar.removeFromRight (width (oneToOne)).reduced (0, 2));
    bar.removeFromRight (10);
    help.setBounds (bar);
    const bool empty = engine.numInputs() == 0 && engine.numOutputs() == 0;
    inView.setBounds (empty ? Rectangle<int>() : r);
    list.setBounds (empty ? Rectangle<int>() : r);
    outView.setBounds (empty ? Rectangle<int>() : r);
    repaint();
}

void PatchView::paint (Graphics& g)
{
    if (engine.numInputs() == 0 && engine.numOutputs() == 0)
    {
        g.setColour (theme::textMuted);
        g.setFont (FontOptions (14.0f));
        g.drawText ("No audio interface is open. Pick one in Audio settings.", getLocalBounds().withTrimmedTop (46),
                    Justification::centred);
    }
    g.setColour (theme::background);
    g.fillRoundedRectangle (pill.toFloat(), 8.0f);
    if (! viewPill.isEmpty()) g.fillRoundedRectangle (viewPill.toFloat(), 8.0f);
    g.setColour (theme::line);
    g.fillRect (0, 45, getWidth(), 1);
}

//==============================================================================
String PatchView::saveInputs (const AudioEngine& e)
{
    StringArray list;
    for (int t = 0; t < AudioEngine::maxChannels; ++t) list.add (String (e.getTrackSource (t)));
    while (list.size() > 0 && list[list.size() - 1] == String (list.size() - 1)) list.remove (list.size() - 1); // trailing 1:1
    return list.joinIntoString (",");
}

String PatchView::saveOutputs (const AudioEngine& e)
{
    StringArray list;
    for (int s = 0; s < AudioEngine::numSources; ++s)
        for (int o = 0; o < AudioEngine::maxChannels; ++o)
            if (e.getPatch (s, o)) list.add (String (s) + ":" + String (o));
    return list.isEmpty() ? String ("none") : list.joinIntoString (",");
}

void PatchView::loadInputs (AudioEngine& e, const String& text)
{
    if (text.isEmpty()) return;
    auto list = StringArray::fromTokens (text, ",", {});
    for (int t = 0; t < list.size() && t < AudioEngine::maxChannels; ++t)
        e.setTrackSource (t, list[t].getIntValue());
}

void PatchView::loadOutputs (AudioEngine& e, const String& text)
{
    if (text.isEmpty()) return; // never saved: keep the default
    for (int s = 0; s < AudioEngine::numSources; ++s)
        for (int o = 0; o < AudioEngine::maxChannels; ++o)
            e.setPatch (s, o, false);
    if (text == "none") return;
    for (auto& item : StringArray::fromTokens (text, ",", {}))
        e.setPatch (item.upToFirstOccurrenceOf (":", false, false).getIntValue(),
                    item.fromFirstOccurrenceOf (":", false, false).getIntValue(), true);
}

//==============================================================================
String PatchView::networkId (const AudioEngine& e, int input)
{
    const int ch = input - e.numDeviceInputs();
    if (ch < 0 || input >= e.numInputs()) return {};
    for (auto& s : e.network().status())
        if (ch >= s.firstChannel && ch < s.firstChannel + s.info.channels)
            return "net:" + s.info.key() + "#" + String (ch - s.firstChannel);
    return {};
}

int PatchView::networkInput (const AudioEngine& e, const String& id)
{
    if (! id.startsWith ("net:")) return -1;
    const auto key = id.fromFirstOccurrenceOf ("net:", false, false).upToLastOccurrenceOf ("#", false, false);
    const int ch = id.fromLastOccurrenceOf ("#", false, false).getIntValue();
    for (auto& s : e.network().status())
        if (s.info.key() == key && ch < s.info.channels) return e.numDeviceInputs() + s.firstChannel + ch;
    return -1;
}
