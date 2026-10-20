#include "TrackRow.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include "Theme.h"

using namespace juce;

static float dbOf (float g) { return Decibels::gainToDecibels (g, -100.0f); }
static constexpr float meterFloor = -60.0f;
static float posOf (float db) { return jlimit (0.0f, 1.0f, (db - meterFloor) / -meterFloor); }

void LevelMeter::setLevel (float peak)
{
    // Fast attack, ~20 dB/s fall, like a console meter.
    const float db = dbOf (peak);
    const float fallen = dbOf (level) - 0.7f;
    level = Decibels::decibelsToGain (jmax (db, fallen), -100.0f);
    if (peak >= 1.0f) clipped = true;
    if (peak >= hold) { hold = peak; holdFrames = 45; }
    else if (--holdFrames <= 0) hold = jmax (0.0f, hold * 0.9f);
    repaint();
}

void LevelMeter::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto readout = r.removeFromRight (44.0f);
    auto clip = r.removeFromRight (8.0f).reduced (1.0f, 2.0f);
    auto bar = r.reduced (0.0f, 2.0f);

    g.setColour (theme::meterBack);
    g.fillRoundedRectangle (bar, 2.0f);

    const float w = bar.getWidth() * posOf (dbOf (level));
    ColourGradient grad (theme::meterGreen, bar.getX(), 0.0f, theme::meterRed, bar.getRight(), 0.0f, false);
    grad.addColour (posOf (-12.0f), theme::meterGreen);
    grad.addColour (posOf (-6.0f), theme::meterYellow);
    g.setGradientFill (grad);
    g.fillRect (bar.withWidth (w));

    if (hold > 0.0f)
    {
        g.setColour (Colours::white.withAlpha (0.8f));
        g.fillRect (bar.getX() + bar.getWidth() * posOf (dbOf (hold)) - 1.0f, bar.getY(), 2.0f, bar.getHeight());
    }

    // dB ticks
    g.setColour (Colours::black.withAlpha (0.45f));
    for (float db : { -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, -3.0f })
        g.fillRect (bar.getX() + bar.getWidth() * posOf (db), bar.getY(), 1.0f, bar.getHeight());

    g.setColour (clipped ? theme::meterRed : theme::meterBack);
    g.fillRoundedRectangle (clip, 1.5f);

    g.setColour (hold >= 1.0f ? theme::meterRed : theme::textMuted);
    g.setFont (FontOptions (11.0f));
    const float hdb = dbOf (hold);
    g.drawText (hold <= 0.0f || hdb < -99.0f ? String ("-inf") : String (hdb, 1), readout, Justification::centredRight);
}

bool NameEditor::keyPressed (const KeyPress& key)
{
    if (key.getKeyCode() == KeyPress::tabKey || key.getKeyCode() == KeyPress::returnKey)
    {
        if (onMove) onMove (key.getModifiers().isShiftDown() ? -1 : 1);
        return true;
    }
    return TextEditor::keyPressed (key);
}

TrackRow::TrackRow (int channelIndex) : index (channelIndex)
{
    number.setText (String (index + 1), dontSendNotification);
    number.setJustificationType (Justification::centredRight);
    number.setColour (Label::textColourId, theme::textMuted);
    addAndMakeVisible (number);

    name.setSelectAllWhenFocused (true);
    name.setTabKeyUsedAsCharacter (false);
    name.setJustification (Justification::centredLeft);
    name.setIndents (6, 0);
    name.onTextChange = [this] { if (onChange) onChange(); };
    name.onMove = [this] (int d) { if (onMoveFocus) onMoveFocus (d); };
    addAndMakeVisible (name);

    arm.setClickingTogglesState (true);
    arm.setTooltip ("Record arm");
    arm.setColour (TextButton::buttonOnColourId, theme::record);
    arm.setColour (TextButton::textColourOnId, Colours::white);
    arm.onClick = [this] { if (onChange) onChange(); };
    addAndMakeVisible (arm);

    addAndMakeVisible (meter);
}

void TrackRow::resized()
{
    auto r = getLocalBounds().reduced (6, 4);
    number.setBounds (r.removeFromLeft (30));
    r.removeFromLeft (6);
    name.setBounds (r.removeFromLeft (190));
    r.removeFromLeft (6);
    arm.setBounds (r.removeFromLeft (30));
    r.removeFromLeft (10);
    meter.setBounds (r);
}

void TrackRow::paint (Graphics& g)
{
    g.setColour (theme::line);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}
