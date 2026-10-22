#include "Console.h"
#include "Look.h"
#include "Theme.h"

using namespace juce;

static float dbOf (float g) { return Decibels::gainToDecibels (g, -100.0f); }
static float meterPos (float gain) { return jlimit (0.0f, 1.0f, (dbOf (gain) + 60.0f) / 60.0f); }
static String fmtDb (float db)
{
    if (db <= -60.0f) return String (CharPointer_UTF8 ("-\xe2\x88\x9e"));
    if (std::abs (db) < 0.05f) return "0.0";
    return (db > 0 ? "+" : "") + String (db, 1);
}
static String fmtPan (float p)
{
    if (std::abs (p) < 0.005f) return "C";
    return (p < 0 ? "L" : "R") + String (roundToInt (std::abs (p) * 100.0f));
}

// Console-style meter ballistics: fast attack, ~20 dB/s fall, 1.5 s peak hold.
static void meterStep (float peak, float& level, float& hold, int& holdFrames, bool& clipped)
{
    level = Decibels::decibelsToGain (jmax (dbOf (peak), dbOf (level) - 0.7f), -100.0f);
    if (peak >= 1.0f) clipped = true;
    if (peak >= hold) { hold = peak; holdFrames = 45; }
    else if (--holdFrames <= 0) hold = jmax (0.0f, hold * 0.9f);
}

static void meterGradient (Graphics& g, Rectangle<float> r, bool vertical)
{
    auto p = [] (float db) { return (db + 60.0f) / 60.0f; };
    ColourGradient grad = vertical
        ? ColourGradient (theme::meterGreen, 0, r.getBottom(), theme::meterRed, 0, r.getY(), false)
        : ColourGradient (theme::meterGreen, r.getX(), 0, theme::meterRed, r.getRight(), 0, false);
    grad.addColour (p (-12.0f), theme::meterGreen);
    grad.addColour (p (-6.0f), theme::meterYellow);
    g.setGradientFill (grad);
}

//==============================================================================
void VMeter::setLevel (float peak)
{
    meterStep (peak, level, hold, holdFrames, clipped);
    repaint();
}

void VMeter::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    Path shape;
    shape.addRoundedRectangle (r, 2.0f);
    g.reduceClipRegion (shape);
    g.setColour (theme::meterBack);
    g.fillRect (r);
    meterGradient (g, r, true);
    const float h = r.getHeight() * meterPos (level);
    g.fillRect (r.withTop (r.getBottom() - h));
    g.setColour (theme::meterTick);
    for (float db : { -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, -3.0f })
        g.fillRect (r.getX(), r.getBottom() - std::round (r.getHeight() * (db + 60.0f) / 60.0f), r.getWidth(), 1.0f);
    const float hy = r.getHeight() * meterPos (hold);
    if (hy > 1.0f)
    {
        g.setColour (theme::text);
        g.fillRect (r.getX(), r.getBottom() - hy, r.getWidth(), 2.0f);
    }
    if (clipped)
    {
        g.setColour (theme::record);
        g.fillRect (r.withHeight (5.0f));
    }
}

//==============================================================================
static void smallLabel (Label& l, const String& text, Justification j, Colour c)
{
    l.setText (text, dontSendNotification);
    l.setFont (FontOptions (10.0f));
    l.setJustificationType (j);
    l.setColour (Label::textColourId, c);
    l.setBorderSize ({});
    l.setInterceptsMouseClicks (false, false);
}

void InsertItem::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (bypass ? theme::background : theme::panel2);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (isMouseOver() ? theme::accent : theme::line);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    g.setColour (bypass ? theme::textMuted : theme::text);
    g.setFont (FontOptions (11.0f));
    g.drawFittedText (name, getLocalBounds().reduced (5, 0), Justification::centredLeft, 1, 0.8f);
    if (bypass)
    {
        g.setColour (theme::textMuted.withAlpha (0.6f));
        g.drawLine (r.getX() + 4.0f, r.getCentreY(), r.getRight() - 4.0f, r.getCentreY());
    }
}

void InsertItem::mouseUp (const MouseEvent& e)
{
    if (! getLocalBounds().contains (e.getPosition())) return;
    if (e.mods.isPopupMenu()) host.showInsertMenu (track, index, *this);
    else if (e.mods.isAltDown()) host.toggleBypass (track, index);
    else host.openEditor (track, index);
}

ChannelStrip::ChannelStrip (AudioEngine& e, PluginHost& h, int ch) : engine (e), host (h), index (ch)
{
    smallLabel (number, String (ch + 1), Justification::bottomLeft, theme::textMuted);
    number.setFont (FontOptions (11.0f));
    smallLabel (name, {}, Justification::bottomLeft, theme::text);
    name.setFont (FontOptions (13.0f, Font::bold));
    name.setMinimumHorizontalScale (0.7f);
    smallLabel (gainLabel, "Gain", Justification::centredLeft, theme::textMuted);
    smallLabel (gainValue, "0.0", Justification::centredRight, theme::text);
    smallLabel (panLabel, "Pan", Justification::centredLeft, theme::textMuted);
    smallLabel (panValue, "C", Justification::centredRight, theme::text);
    smallLabel (dbValue, "0.0", Justification::centred, theme::text);
    dbValue.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
    for (auto* l : { &number, &name, &gainLabel, &gainValue, &panLabel, &panValue, &dbValue })
        addAndMakeVisible (*l);

    gain.setRange (-20.0, 20.0, 0.5);
    gain.setDoubleClickReturnValue (true, 0.0);
    gain.setTooltip ("Input gain (trim). Double-click to reset.");
    pan.setRange (-1.0, 1.0, 0.01);
    WebLook::setStyle (pan, "pan");
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setTooltip ("Pan. Double-click to centre.");
    fader.setSliderStyle (Slider::LinearVertical);
    WebLook::setStyle (fader, "fader");
    fader.setRange (-60.0, 10.0, 0.1);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setTooltip ("Fader. Double-click for 0 dB.");
    fader.setMouseCursor (MouseCursor::UpDownResizeCursor);
    fader.setVelocityBasedMode (false);
    for (auto* s : { &gain, &pan, &fader })
    {
        if (s != &fader) s->setSliderStyle (Slider::LinearHorizontal);
        s->setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        s->onValueChange = [this] { push(); };
        addAndMakeVisible (*s);
    }

    for (auto* b : { &mute, &solo })
    {
        WebLook::setStyle (*b, "small");
        b->setClickingTogglesState (true);
        b->onClick = [this] { push(); };
        addAndMakeVisible (*b);
    }
    mute.setTooltip ("Mute");
    solo.setTooltip ("Solo");
    mute.setColour (TextButton::buttonOnColourId, theme::mute);
    mute.setColour (TextButton::textColourOnId, Colour (0xff111111));
    solo.setColour (TextButton::buttonOnColourId, theme::solo);
    solo.setColour (TextButton::textColourOnId, Colour (0xff111111));

    smallLabel (mainsLabel, "Mains", Justification::centredLeft, theme::textMuted);
    addAndMakeVisible (mainsLabel);
    for (int m = 0; m < AudioEngine::numMains; ++m)
    {
        auto* b = mains.add (new TextButton (String (m + 1)));
        WebLook::setStyle (*b, "small");
        b->setClickingTogglesState (true);
        b->setColour (TextButton::buttonOnColourId, theme::accent);
        b->setColour (TextButton::textColourOnId, theme::accentText);
        b->onClick = [this, m]
        {
            engine.strip (index).toMain[(size_t) m] = mains[m]->getToggleState();
            if (onChange) onChange();
        };
        addAndMakeVisible (b);
    }
    refreshBusNames();
    WebLook::setStyle (pre, "small");
    pre.setClickingTogglesState (true);
    pre.setColour (TextButton::buttonOnColourId, theme::solo);
    pre.setColour (TextButton::textColourOnId, Colour (0xff111111));
    pre.setTooltip ("Pre fader: the send ignores the channel fader (usual for monitor mixes). Off: it follows the fader.");
    pre.onClick = [this]
    {
        if (sendsAux >= 0) engine.strip (index).sendPre[(size_t) sendsAux] = pre.getToggleState();
        if (onChange) onChange();
    };
    addChildComponent (pre);

    meter.setTooltip ("Level before the fader (after gain and plugins), so it shows even when the fader is down or muted. Click to reset the peak.");
    addAndMakeVisible (meter);

    WebLook::setStyle (addPlugin, "small-plain");
    addPlugin.setTooltip ("Add a VST3 or Audio Unit plugin to this channel");
    addPlugin.onClick = [this] { host.showAddMenu (index, addPlugin); };
    addAndMakeVisible (addPlugin);
    refreshFromEngine();
    refreshInserts();
}

void ChannelStrip::refreshInserts()
{
    insertItems.clear();
    auto list = engine.getInserts (index);
    for (int i = 0; i < (int) list.size(); ++i)
        addAndMakeVisible (insertItems.add (new InsertItem (host, index, i, list[(size_t) i]->description.name,
                                                            list[(size_t) i]->bypass.load())));
    addPlugin.setVisible ((int) list.size() < AudioEngine::maxInserts);
    resized();
}

void ChannelStrip::refreshFromEngine()
{
    auto& st = engine.strip (index);
    gain.setValue (st.trimDb.load(), dontSendNotification);
    pan.setValue (st.pan.load(), dontSendNotification);
    fader.setValue (sendsAux >= 0 ? st.sendDb[(size_t) sendsAux].load() : st.faderDb.load(), dontSendNotification);
    mute.setToggleState (st.mute.load(), dontSendNotification);
    solo.setToggleState (st.solo.load(), dontSendNotification);
    for (int m = 0; m < mains.size(); ++m) mains[m]->setToggleState (st.toMain[(size_t) m].load(), dontSendNotification);
    if (sendsAux >= 0) pre.setToggleState (st.sendPre[(size_t) sendsAux].load(), dontSendNotification);
    gainValue.setText (fmtDb ((float) gain.getValue()), dontSendNotification);
    panValue.setText (fmtPan ((float) pan.getValue()), dontSendNotification);
    dbValue.setText (fmtDb ((float) fader.getValue()), dontSendNotification);
}

void ChannelStrip::setSendsOnFader (int aux)
{
    if (aux == sendsAux) return;
    sendsAux = aux;
    const bool sends = aux >= 0;
    pre.setVisible (sends);
    for (auto* c : { (Component*) &pan, (Component*) &panLabel, (Component*) &panValue })
        c->setVisible (! sends);
    refreshBusNames();
    refreshFromEngine();
    repaint();
}

void ChannelStrip::refreshBusNames()
{
    fader.setTooltip (sendsAux >= 0 ? "Send to " + engine.busFullName (false, sendsAux) + ". Double-click for 0 dB."
                                    : String ("Fader. Double-click for 0 dB."));
    for (int m = 0; m < mains.size(); ++m)
        mains[m]->setTooltip ("Send this channel to " + engine.busFullName (true, m));
}

void ChannelStrip::push()
{
    auto& st = engine.strip (index);
    st.trimDb = (float) gain.getValue();
    st.pan = (float) pan.getValue();
    if (sendsAux >= 0) st.sendDb[(size_t) sendsAux] = (float) fader.getValue();
    else st.faderDb = (float) fader.getValue();
    st.mute = mute.getToggleState();
    st.solo = solo.getToggleState();
    gainValue.setText (fmtDb ((float) gain.getValue()), dontSendNotification);
    panValue.setText (fmtPan ((float) pan.getValue()), dontSendNotification);
    dbValue.setText (fmtDb ((float) fader.getValue()), dontSendNotification);
    if (onChange) onChange();
}

void ChannelStrip::tick()
{
    meter.setLevel (engine.takeStripPeak (index));
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto head = r.removeFromTop (17);
    number.setBounds (head.removeFromLeft (GlyphArrangement::getStringWidthInt (FontOptions (11.0f), number.getText()) + 4));
    name.setBounds (head);
    r.removeFromTop (6);

    auto knob = [&r] (Label& left, Label& right, Slider& s)
    {
        auto top = r.removeFromTop (12);
        left.setBounds (top.removeFromLeft (top.getWidth() / 2));
        right.setBounds (top);
        s.setBounds (r.removeFromTop (18));
        r.removeFromTop (6);
    };
    knob (gainLabel, gainValue, gain);

    // Inserts: one row per plugin, then "+ Plugin" (the web keeps 128 px for this).
    insertArea = r.removeFromTop (128);
    {
        auto a = insertArea;
        for (auto* item : insertItems)
        {
            item->setBounds (a.removeFromTop (20));
            a.removeFromTop (3);
        }
        addPlugin.setBounds (a.removeFromTop (22));
    }
    r.removeFromTop (6);
    pre.setBounds (r.getX(), r.getY() + 8, r.getWidth(), 22);
    knob (panLabel, panValue, pan);
    mainsLabel.setBounds (r.removeFromTop (12));
    for (int row = 0; row < 2; ++row)
    {
        auto line = r.removeFromTop (18);
        const int w = (line.getWidth() - 3 * 3) / 4;
        for (int i = 0; i < 4; ++i)
        {
            mains[row * 4 + i]->setBounds (line.removeFromLeft (w));
            line.removeFromLeft (3);
        }
        r.removeFromTop (3);
    }
    r.removeFromTop (4);

    auto buttons = r.removeFromBottom (22);
    mute.setBounds (buttons.removeFromLeft ((buttons.getWidth() - 4) / 2));
    buttons.removeFromLeft (4);
    solo.setBounds (buttons);
    r.removeFromBottom (6);
    dbValue.setBounds (r.removeFromBottom (14));
    r.removeFromBottom (6);

    // Fader area: meter, dB scale, fader (10 + 2 + 22 + 2 + 32 px, centred).
    auto area = r.withSizeKeepingCentre (68, r.getHeight());
    meter.setBounds (area.removeFromLeft (10));
    area.removeFromLeft (2);
    scaleArea = area.removeFromLeft (22);
    area.removeFromLeft (2);
    fader.setBounds (area.removeFromLeft (32));
    repaint();
}

void ChannelStrip::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (sendsAux >= 0 ? theme::solo : theme::line);
    g.drawRoundedRectangle (r, 6.0f, sendsAux >= 0 ? 1.5f : 1.0f);

    // dB marks beside the fader, lined up with the cap's centre line.
    const float top = (float) fader.getY() + 23.0f, travel = (float) fader.getHeight() - 46.0f;
    g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 9.0f, Font::plain));
    for (int db : { 10, 5, 0, -5, -10, -20, -30, -40, -60 })
    {
        const float y = std::round (top + (1.0f - (db + 60.0f) / 70.0f) * travel) + 0.5f;
        g.setColour (db == 0 ? theme::text : theme::textMuted);
        g.drawText (db > 0 ? "+" + String (db) : String (db),
                    Rectangle<float> ((float) scaleArea.getX() - 8.0f, y - 6.0f, (float) scaleArea.getWidth() + 2.0f, 12.0f),
                    Justification::centredRight);
        g.fillRect ((float) scaleArea.getRight() - 4.0f, y, 4.0f, 1.0f);
    }
}

//==============================================================================
static void paintFaderScale (Graphics& g, const Slider& fader, Rectangle<int> scaleArea)
{
    // dB marks beside the fader, lined up with the cap's centre line.
    const float top = (float) fader.getY() + 23.0f, travel = (float) fader.getHeight() - 46.0f;
    g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 9.0f, Font::plain));
    for (int db : { 10, 5, 0, -5, -10, -20, -30, -40, -60 })
    {
        const float y = std::round (top + (1.0f - (db + 60.0f) / 70.0f) * travel) + 0.5f;
        g.setColour (db == 0 ? theme::text : theme::textMuted);
        g.drawText (db > 0 ? "+" + String (db) : String (db),
                    Rectangle<float> ((float) scaleArea.getX() - 8.0f, y - 6.0f, (float) scaleArea.getWidth() + 2.0f, 12.0f),
                    Justification::centredRight);
        g.fillRect ((float) scaleArea.getRight() - 4.0f, y, 4.0f, 1.0f);
    }
}

BusStrip::BusStrip (AudioEngine& e, bool main, int i) : engine (e), isMain (main), index (i)
{
    smallLabel (name, {}, Justification::centredLeft, theme::text);
    name.setFont (FontOptions (13.0f, Font::bold));
    name.setInterceptsMouseClicks (true, false);
    name.setEditable (false, true, false);
    name.onEditorShow = [this] { if (auto* ed = name.getCurrentTextEditor()) ed->setText (engine.busLabel (isMain, index), false); };
    name.onTextChange = [this]
    {
        engine.setBusLabel (isMain, index, name.getText());
        refreshName();
        if (onRenamed) onRenamed();
    };
    refreshName();
    smallLabel (dbValue, "0.0", Justification::centred, theme::text);
    dbValue.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
    addAndMakeVisible (name);
    addAndMakeVisible (dbValue);

    fader.setSliderStyle (Slider::LinearVertical);
    WebLook::setStyle (fader, "fader");
    fader.setRange (-60.0, 10.0, 0.1);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    fader.setMouseCursor (MouseCursor::UpDownResizeCursor);
    fader.setVelocityBasedMode (false);
    fader.setTooltip (isMain && i == 0 ? String ("Main 1 is the master (the same fader as in the top bar). Double-click for 0 dB.")
                                       : String ("Bus master. Double-click for 0 dB."));
    fader.onValueChange = [this]
    {
        bus().faderDb = (float) fader.getValue();
        dbValue.setText (fmtDb ((float) fader.getValue()), dontSendNotification);
        if (onChange) onChange();
    };
    addAndMakeVisible (fader);

    WebLook::setStyle (mute, "small");
    mute.setClickingTogglesState (true);
    mute.setColour (TextButton::buttonOnColourId, theme::mute);
    mute.setColour (TextButton::textColourOnId, Colour (0xff111111));
    mute.setTooltip ("Mute this bus");
    mute.onClick = [this] { bus().mute = mute.getToggleState(); if (onChange) onChange(); };
    addAndMakeVisible (mute);

    meterL.setTooltip ("Bus level after its fader. Click to reset the peak.");
    meterR.setTooltip ("Bus level after its fader. Click to reset the peak.");
    addAndMakeVisible (meterL);
    if (isMain) addAndMakeVisible (meterR);
    refreshFromEngine();
}

void BusStrip::refreshName()
{
    name.setText (engine.busName (isMain, index), dontSendNotification);
    name.setTooltip (engine.busFullName (isMain, index) + ". Double-click to label it.");
}

void BusStrip::refreshFromEngine()
{
    refreshName();
    fader.setValue (bus().faderDb.load(), dontSendNotification);
    mute.setToggleState (bus().mute.load(), dontSendNotification);
    dbValue.setText (fmtDb ((float) fader.getValue()), dontSendNotification);
}

void BusStrip::tick()
{
    if (isMain)
    {
        meterL.setLevel (engine.takeMainPeak (index, 0));
        meterR.setLevel (engine.takeMainPeak (index, 1));
    }
    else meterL.setLevel (engine.takeAuxPeak (index));
}

void BusStrip::resized()
{
    auto r = getLocalBounds().reduced (6);
    name.setBounds (r.removeFromTop (17));
    r.removeFromTop (8);
    mute.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (6);
    dbValue.setBounds (r.removeFromBottom (14));
    r.removeFromBottom (6);
    auto area = r.withSizeKeepingCentre (isMain ? 78 : 68, r.getHeight());
    meterL.setBounds (area.removeFromLeft (isMain ? 7 : 10));
    if (isMain) { area.removeFromLeft (1); meterR.setBounds (area.removeFromLeft (7)); }
    area.removeFromLeft (2);
    scaleArea = area.removeFromLeft (22);
    area.removeFromLeft (2);
    fader.setBounds (area.removeFromLeft (32));
}

void BusStrip::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (isMain ? theme::accent.withAlpha (0.6f) : theme::line);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
    paintFaderScale (g, fader, scaleArea);
}

//==============================================================================
BusNamesView::BusNamesView (AudioEngine& e) : engine (e)
{
    for (auto* l : { &auxTitle, &mainTitle })
    {
        l->setFont (FontOptions (13.0f, Font::bold));
        l->setColour (Label::textColourId, theme::text);
        addAndMakeVisible (l);
    }
    auxTitle.setText ("Auxes", dontSendNotification);
    mainTitle.setText ("Mains", dontSendNotification);
    help.setText ("Type a name for any bus. It shows on the bus faders, the Faders menu and in Patch. Leave it empty for the plain number.",
                  dontSendNotification);
    help.setFont (FontOptions (13.0f));
    help.setColour (Label::textColourId, theme::textMuted);
    addAndMakeVisible (help);

    for (int i = 0; i < AudioEngine::numAux + AudioEngine::numMains; ++i)
    {
        const bool main = i >= AudioEngine::numAux;
        const int index = main ? i - AudioEngine::numAux : i;
        auto* n = numbers.add (new Label());
        n->setText ((main ? "Main " : "Aux ") + String (index + 1) + (main && index == 0 ? "  (master)" : ""), dontSendNotification);
        n->setFont (FontOptions (13.0f));
        n->setColour (Label::textColourId, theme::textMuted);
        addAndMakeVisible (n);
        auto* b = boxes.add (new TextEditor());
        b->setFont (FontOptions (14.0f));
        b->setTextToShowWhenEmpty ("No name", theme::textMuted.withAlpha (0.6f));
        b->setIndents (8, 6);
        b->setJustification (Justification::centredLeft);
        b->onTextChange = [this, b, main, index]
        {
            engine.setBusLabel (main, index, b->getText());
            if (onRenamed) onRenamed();
        };
        b->onReturnKey = [b] { b->giveAwayKeyboardFocus(); };
        b->onEscapeKey = [b] { b->giveAwayKeyboardFocus(); };
        addAndMakeVisible (b);
    }
    refresh();
}

void BusNamesView::refresh()
{
    for (int i = 0; i < boxes.size(); ++i)
    {
        const bool main = i >= AudioEngine::numAux;
        const auto label = engine.busLabel (main, main ? i - AudioEngine::numAux : i);
        if (boxes[i]->getText() != label) boxes[i]->setText (label, false);
    }
}

void BusNamesView::resized()
{
    auto r = getLocalBounds().reduced (24, 16);
    help.setBounds (r.removeFromTop (22));
    r.removeFromTop (12);
    const int colW = jmin (360, (r.getWidth() - 40) / 2), rowH = 34, numW = 110;
    auto layout = [&] (Rectangle<int> col, Label& title, int from, int count)
    {
        title.setBounds (col.removeFromTop (24));
        col.removeFromTop (6);
        for (int i = from; i < from + count; ++i)
        {
            auto row = col.removeFromTop (rowH).withTrimmedBottom (4);
            numbers[i]->setBounds (row.removeFromLeft (numW));
            boxes[i]->setBounds (row);
        }
    };
    layout (r.removeFromLeft (colW), auxTitle, 0, AudioEngine::numAux);
    r.removeFromLeft (40);
    layout (r.removeFromLeft (colW), mainTitle, AudioEngine::numAux, AudioEngine::numMains);
}

void BusNamesView::paint (Graphics& g) { g.fillAll (theme::background); }

//==============================================================================
ConsoleView::ConsoleView (AudioEngine& e, PluginHost& h) : engine (e), host (h), busNames (e)
{
    pluginsButton.setTooltip ("Find installed VST3 and Audio Unit plugins");
    pluginsButton.onClick = [this] { host.showScanner(); };
    addAndMakeVisible (pluginsButton);
    goLive.setTooltip ("Send the live mix to the output");
    goLive.setColour (TextButton::buttonOnColourId, theme::record);
    goLive.setColour (TextButton::textColourOnId, Colours::white);
    goLive.onClick = [this]
    {
        const bool on = ! engine.isLive();
        engine.setLive (on);
        refresh();
        if (onGoLive) onGoLive (on);
    };
    WebLook::setStyle (goLive, "bold");
    // The mix always reaches the outputs, so there's no Go live button or status to show.

    limiter.setToggleState (engine.limiterEnabled(), dontSendNotification);
    limiter.setColour (ToggleButton::textColourId, theme::text);
    limiter.setTooltip ("Keeps the output from clipping (ceiling -1 dB)");
    limiter.onClick = [this] { engine.setLimiter (limiter.getToggleState()); if (onChange) onChange(); };
    addAndMakeVisible (limiter);

    for (auto* l : { &status, &output, &latency, &hint })
    {
        l->setFont (FontOptions (13.0f));
        l->setColour (Label::textColourId, theme::textMuted);
        addAndMakeVisible (*l);
    }
    status.setVisible (false);
    hint.setText ("Double-click a control to reset it", dontSendNotification);
    hint.setJustificationType (Justification::centredRight);

    viewport.setViewedComponent (&stripHolder, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);

    // Buses: 12 auxes then 8 mains, shown instead of the channels.
    for (int a = 0; a < AudioEngine::numAux; ++a) buses.add (new BusStrip (engine, false, a));
    for (int m = 0; m < AudioEngine::numMains; ++m) buses.add (new BusStrip (engine, true, m));
    for (auto* b : buses)
    {
        b->onChange = [this] { if (onChange) onChange(); };
        b->onRenamed = [this] { busNames.refresh(); refreshBusNames(); };
        busHolder.addAndMakeVisible (b);
    }
    busViewport.setViewedComponent (&busHolder, false);
    busViewport.setScrollBarsShown (false, true);
    addChildComponent (busViewport);

    busNames.onRenamed = [this] { refreshBusNames(); };
    addChildComponent (busNames);

    for (auto* t : { &channelsTab, &busesTab, &namesTab })
    {
        WebLook::setStyle (*t, "mode");
        addAndMakeVisible (*t);
    }
    channelsTab.setToggleState (true, dontSendNotification);
    channelsTab.onClick = [this] { showView (0); };
    busesTab.onClick = [this] { showView (1); };
    namesTab.onClick = [this] { showView (2); };
    namesTab.setTooltip ("Label the auxes and mains");
    busesTab.setTooltip ("The 12 aux and 8 main bus masters. Patch them to outputs in Patch > Outputs.");

    faderLabel.setText ("Faders:", dontSendNotification);
    faderLabel.setFont (FontOptions (13.0f));
    faderLabel.setColour (Label::textColourId, theme::textMuted);
    addAndMakeVisible (faderLabel);
    faderMode.addItem ("Main mix", 1);
    for (int a = 0; a < AudioEngine::numAux; ++a) faderMode.addItem ("Aux " + String (a + 1) + " sends", a + 2);
    faderMode.setSelectedId (1, dontSendNotification);
    refreshBusNames();
    faderMode.setTooltip ("Choose an aux to set every channel's send to it with the channel faders (like sends on fader on a console).");
    faderMode.onChange = [this] { for (auto* st : strips) st->setSendsOnFader (faderMode.getSelectedId() - 2); };
    addAndMakeVisible (faderMode);
    refresh();
}

void ConsoleView::rebuild (int n, const StringArray& names)
{
    if (n != strips.size())
    {
        strips.clear();
        for (int i = 0; i < n; ++i)
        {
            auto* s = strips.add (new ChannelStrip (engine, host, i));
            s->onChange = [this] { if (onChange) onChange(); };
            s->setSendsOnFader (faderMode.getSelectedId() - 2);
            stripHolder.addAndMakeVisible (s);
        }
        resized();
    }
    setNames (names);
}

void ConsoleView::setNames (const StringArray& names)
{
    for (auto* s : strips)
    {
        s->setName (names[s->channel()]);
        s->setHasInput (isPositiveAndBelow (engine.getTrackSource (s->channel()), engine.numInputs()));
    }
}

void ConsoleView::refresh()
{
    const bool live = engine.isLive();
    goLive.setButtonText (live ? "Stop live" : "Go live");
    goLive.setToggleState (live, dontSendNotification);
    status.setText (live ? "Live mix is on" : "Live mix is off", dontSendNotification);
    status.setColour (Label::textColourId, live ? theme::record : theme::textMuted);

    // Where the mix goes, from the output patch.
    auto* dev = engine.devices().getCurrentAudioDevice();
    StringArray used;
    for (int o = 0; o < engine.numOutputs(); ++o)
        if (engine.getPatch (AudioEngine::mixLeft, o) || engine.getPatch (AudioEngine::mixRight, o))
        {
            auto n = dev != nullptr ? dev->getOutputChannelNames()[o] : String();
            used.add (n.isNotEmpty() ? n : "Out " + String (o + 1));
        }
    String out = used.isEmpty() ? String ("Output: none (patch the live mix in Patch)")
                                : "Output: " + engine.devices().getAudioDeviceSetup().outputDeviceName + " (" + used.joinIntoString (" + ") + ")";
    output.setText (out, dontSendNotification);
    const double ms = engine.outputLatencyMs();
    latency.setText (ms > 0 ? String (CharPointer_UTF8 ("Latency \xe2\x89\x88 ")) + String (roundToInt (ms)) + " ms" : String(),
                     dontSendNotification);
    resized();
}

void ConsoleView::refreshInserts (int track)
{
    for (auto* s : strips)
        if (s->channel() == track) s->refreshInserts();
}

void ConsoleView::tick()
{
    if (view == 1) for (auto* b : buses) b->tick();
    else if (view == 0) for (auto* s : strips) s->tick();
}

void ConsoleView::showView (int v)
{
    view = v;
    showBuses = v == 1;
    channelsTab.setToggleState (v == 0, dontSendNotification);
    busesTab.setToggleState (v == 1, dontSendNotification);
    namesTab.setToggleState (v == 2, dontSendNotification);
    resized();
}

void ConsoleView::refreshBusNames()
{
    for (auto* b : buses) b->refreshName();
    for (int a = 0; a < AudioEngine::numAux; ++a)
        faderMode.changeItemText (a + 2, engine.busFullName (false, a) + " sends");
    if (faderMode.getSelectedId() > 1) faderMode.setText (faderMode.getItemText (faderMode.getSelectedItemIndex()), dontSendNotification);
    for (auto* s : strips) s->refreshBusNames();
    if (onChange) onChange();
}

void ConsoleView::refreshBuses()
{
    for (auto* b : buses) b->refreshFromEngine();
}

void ConsoleView::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (46).reduced (16, 8).withTrimmedBottom (1);
    auto width = [] (const String& t, bool bold = false)
    { return GlyphArrangement::getStringWidthInt (FontOptions (13.0f, bold ? Font::bold : Font::plain), t); };

    const int tabW = width ("Channels", true) + 24, busW = width ("Buses", true) + 24, namesW = width ("Bus names", true) + 24;
    const int pillW = tabW + busW + namesW + 14;
    viewPill = bar.removeFromLeft (pillW).withSizeKeepingCentre (pillW, 30);
    {
        auto p = viewPill.reduced (3);
        channelsTab.setBounds (p.removeFromLeft (tabW));
        p.removeFromLeft (4);
        busesTab.setBounds (p.removeFromLeft (busW));
        p.removeFromLeft (4);
        namesTab.setBounds (p.removeFromLeft (namesW));
    }
    bar.removeFromLeft (14);
    faderLabel.setBounds (bar.removeFromLeft (width ("Faders:") + 6));
    faderMode.setBounds (bar.removeFromLeft (150).reduced (0, 1));
    faderMode.setVisible (view == 0);
    faderLabel.setVisible (view == 0);
    bar.removeFromLeft (14);

    output.setBounds (bar.removeFromLeft (jmin (bar.getWidth() / 2, width (output.getText()) + 10)));
    bar.removeFromLeft (12);
    limiter.setBounds (bar.removeFromLeft (width ("Master limiter") + 30));
    bar.removeFromLeft (12);
    latency.setBounds (bar.removeFromLeft (width (latency.getText()) + 10));
    bar.removeFromLeft (12);
    pluginsButton.setBounds (bar.removeFromRight (width ("Plugins...") + 22));
    bar.removeFromRight (12);
    hint.setBounds (bar);

    viewport.setVisible (view == 0);
    busViewport.setVisible (view == 1);
    busNames.setVisible (view == 2);
    busNames.setBounds (r);
    busViewport.setBounds (r);
    {
        const int bh = jmax (300, busViewport.getMaximumVisibleHeight());
        const int gap = 4, pad = 16, top = 10, groupGap = 18;
        busHolder.setSize (pad * 2 + buses.size() * (ChannelStrip::width + gap) + groupGap, bh);
        int x = pad;
        for (int i = 0; i < buses.size(); ++i)
        {
            if (i == AudioEngine::numAux) x += groupGap; // a gap between the auxes and the mains
            buses[i]->setBounds (x, top, ChannelStrip::width, bh - top - 10);
            x += ChannelStrip::width + gap;
        }
    }
    viewport.setBounds (r);
    const int h = jmax (300, viewport.getMaximumVisibleHeight());
    const int gap = 4, pad = 16, top = 10;
    stripHolder.setSize (pad * 2 + strips.size() * (ChannelStrip::width + gap), h);
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (pad + i * (ChannelStrip::width + gap), top, ChannelStrip::width, h - top - 10);
}

void ConsoleView::paint (Graphics& g)
{
    g.setColour (theme::line);
    g.fillRect (0, 45, getWidth(), 1);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (viewPill.toFloat(), 8.0f);
}

String ConsoleView::saveState() const
{
    Array<var> list;
    for (auto* s : strips)
    {
        auto& st = engine.strip (s->channel());
        auto* o = new DynamicObject();
        o->setProperty ("trim", st.trimDb.load());
        o->setProperty ("fader", st.faderDb.load());
        o->setProperty ("pan", st.pan.load());
        o->setProperty ("mute", st.mute.load());
        o->setProperty ("solo", st.solo.load());
        Array<var> sends, pre, mains;
        for (int a = 0; a < AudioEngine::numAux; ++a) { sends.add (st.sendDb[(size_t) a].load()); pre.add (st.sendPre[(size_t) a].load()); }
        for (int m = 0; m < AudioEngine::numMains; ++m) mains.add (st.toMain[(size_t) m].load());
        o->setProperty ("sends", sends);
        o->setProperty ("pre", pre);
        o->setProperty ("mains", mains);
        list.add (var (o));
    }
    Array<var> auxList, mainList;
    for (int a = 0; a < AudioEngine::numAux; ++a)
    {
        auto* o = new DynamicObject();
        o->setProperty ("fader", engine.aux (a).faderDb.load());
        o->setProperty ("mute", engine.aux (a).mute.load());
        auxList.add (var (o));
    }
    for (int m = 0; m < AudioEngine::numMains; ++m)
    {
        auto* o = new DynamicObject();
        o->setProperty ("fader", engine.mainBus (m).faderDb.load());
        o->setProperty ("mute", engine.mainBus (m).mute.load());
        mainList.add (var (o));
    }
    auto* root = new DynamicObject();
    root->setProperty ("strips", list);
    root->setProperty ("limiter", engine.limiterEnabled());
    root->setProperty ("master", engine.getMasterDb());
    root->setProperty ("aux", auxList);
    root->setProperty ("mainBuses", mainList);
    Array<var> auxNames, mainNames;
    for (int a = 0; a < AudioEngine::numAux; ++a) auxNames.add (engine.busLabel (false, a));
    for (int m = 0; m < AudioEngine::numMains; ++m) mainNames.add (engine.busLabel (true, m));
    root->setProperty ("auxNames", auxNames);
    root->setProperty ("mainNames", mainNames);
    return JSON::toString (var (root), true);
}

void ConsoleView::loadState (const String& json)
{
    auto v = JSON::parse (json);
    if (! v.isObject()) return;
    if (auto* list = v["strips"].getArray())
        for (int i = 0; i < list->size() && i < AudioEngine::maxChannels; ++i)
        {
            auto& o = list->getReference (i);
            auto& st = engine.strip (i);
            st.trimDb = jlimit (-20.0f, 20.0f, (float) o.getProperty ("trim", 0.0));
            st.faderDb = jlimit (-60.0f, 10.0f, (float) o.getProperty ("fader", 0.0));
            st.pan = jlimit (-1.0f, 1.0f, (float) o.getProperty ("pan", 0.0));
            st.mute = (bool) o.getProperty ("mute", false);
            st.solo = (bool) o.getProperty ("solo", false);
            auto* sends = o["sends"].getArray();
            auto* pre = o["pre"].getArray();
            auto* mains = o["mains"].getArray();
            for (int a = 0; a < AudioEngine::numAux; ++a)
            {
                st.sendDb[(size_t) a] = sends != nullptr && a < sends->size() ? jlimit (-60.0f, 10.0f, (float) (*sends)[a]) : -60.0f;
                st.sendPre[(size_t) a] = pre != nullptr && a < pre->size() ? (bool) (*pre)[a] : true;
            }
            for (int m = 0; m < AudioEngine::numMains; ++m)
                st.toMain[(size_t) m] = mains != nullptr && m < mains->size() ? (bool) (*mains)[m] : m == 0;
        }
    if (auto* list = v["aux"].getArray())
        for (int a = 0; a < list->size() && a < AudioEngine::numAux; ++a)
        {
            engine.aux (a).faderDb = jlimit (-60.0f, 10.0f, (float) (*list)[a].getProperty ("fader", 0.0));
            engine.aux (a).mute = (bool) (*list)[a].getProperty ("mute", false);
        }
    if (auto* list = v["mainBuses"].getArray())
        for (int m = 1; m < list->size() && m < AudioEngine::numMains; ++m) // main 1's fader is "master"
        {
            engine.mainBus (m).faderDb = jlimit (-60.0f, 10.0f, (float) (*list)[m].getProperty ("fader", 0.0));
            engine.mainBus (m).mute = (bool) (*list)[m].getProperty ("mute", false);
        }
    if (auto* list = v["mainBuses"].getArray(); list != nullptr && list->size() > 0)
        engine.mainBus (0).mute = (bool) (*list)[0].getProperty ("mute", false);
    if (v.hasProperty ("limiter")) engine.setLimiter ((bool) v["limiter"]);
    if (v.hasProperty ("master")) engine.setMasterDb (jlimit (-60.0f, 10.0f, (float) v["master"]));
    limiter.setToggleState (engine.limiterEnabled(), dontSendNotification);
    auto* auxNames = v["auxNames"].getArray();
    auto* mainNames = v["mainNames"].getArray();
    for (int a = 0; a < AudioEngine::numAux; ++a)
        engine.setBusLabel (false, a, auxNames != nullptr && a < auxNames->size() ? (*auxNames)[a].toString() : String());
    for (int m = 0; m < AudioEngine::numMains; ++m)
        engine.setBusLabel (true, m, mainNames != nullptr && m < mainNames->size() ? (*mainNames)[m].toString() : String());
    busNames.refresh();
    for (auto* s : strips) s->refreshFromEngine();
    refreshBuses();
    refreshBusNames();
}

//==============================================================================
MasterStrip::MasterStrip (AudioEngine& e) : engine (e)
{
    fader.setSliderStyle (Slider::LinearHorizontal);
    WebLook::setStyle (fader, "hfader");
    fader.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    fader.setRange (-60.0, 10.0, 0.1);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setTooltip ("Master volume of the live mix. Double-click for 0 dB.");
    fader.setMouseCursor (MouseCursor::LeftRightResizeCursor);
    fader.onValueChange = [this] { engine.setMasterDb ((float) fader.getValue()); if (onChange) onChange(); };
    addAndMakeVisible (fader);
    refreshFromEngine();
}

void MasterStrip::refreshFromEngine()
{
    fader.setValue (engine.getMasterDb(), dontSendNotification);
}

void MasterStrip::tick()
{
    for (int s = 0; s < 2; ++s)
        meterStep (engine.takeMasterPeak (s), level[s], hold[s], holdFrames[s], clip[s]);
    repaint();
}

void MasterStrip::resized()
{
    auto r = getLocalBounds().reduced (10, 8);
    auto top = r.removeFromTop (18);
    top.removeFromLeft (56);
    fader.setBounds (top);
}

void MasterStrip::paint (Graphics& g)
{
    auto box = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::clockBack);
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (theme::line);
    g.drawRoundedRectangle (box, 8.0f, 1.0f);

    auto r = getLocalBounds().reduced (10, 8);
    g.setColour (theme::textMuted);
    g.setFont (FontOptions (11.0f, Font::bold));
    g.drawText ("MASTER", r.removeFromTop (18).removeFromLeft (56), Justification::centredLeft);
    r.removeFromTop (4);

    // Two bars (L, R) with the dB scale between them and peak readouts on the right.
    auto readout = r.removeFromRight (40);
    auto bars = r.reduced (0, 0).withTrimmedRight (8);
    auto barL = bars.removeFromTop (8).toFloat();
    auto scale = bars.removeFromTop (bars.getHeight() - 8).toFloat();
    auto barR = bars.toFloat();
    for (int s = 0; s < 2; ++s)
    {
        auto b = s == 0 ? barL : barR;
        g.setColour (theme::meterBack);
        g.fillRect (b);
        meterGradient (g, b, false);
        g.fillRect (b.withWidth (b.getWidth() * meterPos (level[s])));
        g.setColour (theme::meterTick);
        for (float db : { -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, -3.0f })
            g.fillRect (b.getX() + std::round (b.getWidth() * (db + 60.0f) / 60.0f), b.getY(), 1.0f, b.getHeight());
        const float hx = b.getWidth() * meterPos (hold[s]);
        if (hx > 1.0f) { g.setColour (theme::text); g.fillRect (b.getX() + hx - 1.0f, b.getY(), 2.0f, b.getHeight()); }
        if (clip[s]) { g.setColour (theme::record); g.fillRect (b.getRight() - 5.0f, b.getY(), 5.0f, b.getHeight()); }

        const float hdb = dbOf (hold[s]);
        g.setColour (clip[s] ? theme::record : theme::textMuted);
        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
        g.drawText (hold[s] <= 0.0f || hdb < -99.0f ? String (CharPointer_UTF8 ("-\xe2\x88\x9e")) : String (hdb, 1),
                    readout.toFloat().withY (b.getY() - 3.0f).withHeight (b.getHeight() + 6.0f), Justification::centredRight);
    }
    g.setColour (theme::textMuted);
    g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 9.0f, Font::plain));
    for (int db : { -48, -36, -24, -18, -12, -6, 0 })
    {
        const float x = scale.getX() + scale.getWidth() * (db + 60.0f) / 60.0f;
        g.drawText (String (db), Rectangle<float> (x - 15.0f, scale.getY(), 30.0f, scale.getHeight()), Justification::centred);
    }
}
