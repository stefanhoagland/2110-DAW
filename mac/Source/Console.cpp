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
    fader.setValue (st.faderDb.load(), dontSendNotification);
    mute.setToggleState (st.mute.load(), dontSendNotification);
    solo.setToggleState (st.solo.load(), dontSendNotification);
    gainValue.setText (fmtDb ((float) gain.getValue()), dontSendNotification);
    panValue.setText (fmtPan ((float) pan.getValue()), dontSendNotification);
    dbValue.setText (fmtDb ((float) fader.getValue()), dontSendNotification);
}

void ChannelStrip::push()
{
    auto& st = engine.strip (index);
    st.trimDb = (float) gain.getValue();
    st.pan = (float) pan.getValue();
    st.faderDb = (float) fader.getValue();
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
    knob (panLabel, panValue, pan);

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
    g.setColour (theme::line);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

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
ConsoleView::ConsoleView (AudioEngine& e, PluginHost& h) : engine (e), host (h)
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
        s->setHasInput (s->channel() < engine.numInputs() && engine.getTrackSource (s->channel()) >= 0);
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
    for (auto* s : strips) s->tick();
}

void ConsoleView::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (46).reduced (16, 8).withTrimmedBottom (1);
    auto width = [] (const String& t, bool bold = false)
    { return GlyphArrangement::getStringWidthInt (FontOptions (13.0f, bold ? Font::bold : Font::plain), t); };

    output.setBounds (bar.removeFromLeft (jmin (bar.getWidth() / 2, width (output.getText()) + 10)));
    bar.removeFromLeft (12);
    limiter.setBounds (bar.removeFromLeft (width ("Master limiter") + 30));
    bar.removeFromLeft (12);
    latency.setBounds (bar.removeFromLeft (width (latency.getText()) + 10));
    bar.removeFromLeft (12);
    pluginsButton.setBounds (bar.removeFromRight (width ("Plugins...") + 22));
    bar.removeFromRight (12);
    hint.setBounds (bar);

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
        list.add (var (o));
    }
    auto* root = new DynamicObject();
    root->setProperty ("strips", list);
    root->setProperty ("limiter", engine.limiterEnabled());
    root->setProperty ("master", engine.getMasterDb());
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
        }
    if (v.hasProperty ("limiter")) engine.setLimiter ((bool) v["limiter"]);
    if (v.hasProperty ("master")) engine.setMasterDb (jlimit (-60.0f, 10.0f, (float) v["master"]));
    limiter.setToggleState (engine.limiterEnabled(), dontSendNotification);
    for (auto* s : strips) s->refreshFromEngine();
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
