#include "TrackRow.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include "Look.h"
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
    auto bar = r.removeFromLeft ((float) barWidth).withSizeKeepingCentre ((float) barWidth, 12.0f);
    r.removeFromLeft (6.0f);
    auto readout = r.removeFromLeft ((float) readoutWidth);

    Path clipShape;
    clipShape.addRoundedRectangle (bar, 2.0f);
    g.saveState();
    g.reduceClipRegion (clipShape);

    g.setColour (theme::meterBack);
    g.fillRect (bar);

    ColourGradient grad (theme::meterGreen, bar.getX(), 0.0f, theme::meterRed, bar.getRight(), 0.0f, false);
    grad.addColour (posOf (-12.0f), theme::meterGreen);
    grad.addColour (posOf (-6.0f), theme::meterYellow);
    g.setGradientFill (grad);
    g.fillRect (bar.withWidth (bar.getWidth() * posOf (dbOf (level))));

    g.setColour (theme::meterTick);
    for (float db : { -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, -3.0f })
        g.fillRect (bar.getX() + std::round (bar.getWidth() * posOf (db)) - 1.0f, bar.getY(), 1.0f, bar.getHeight());

    const float hx = bar.getWidth() * posOf (dbOf (hold));
    if (hx > 1.0f)
    {
        g.setColour (theme::text);
        g.fillRect (bar.getX() + hx - 1.0f, bar.getY(), 2.0f, bar.getHeight());
    }
    if (clipped)
    {
        g.setColour (theme::record);
        g.fillRect (bar.getRight() - 5.0f, bar.getY(), 5.0f, bar.getHeight());
    }
    g.restoreState();

    const float hdb = dbOf (hold);
    g.setColour (clipped ? theme::record : theme::textMuted);
    g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, clipped ? Font::bold : Font::plain));
    g.drawText (hold <= 0.0f || hdb < -99.0f ? String (CharPointer_UTF8 ("-\xe2\x88\x9e")) : String (hdb, 1),
                readout, Justification::centredRight);
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
    number.setFont (FontOptions (13.0f));
    number.setBorderSize ({});
    addAndMakeVisible (number);

    name.setSelectAllWhenFocused (true);
    name.setTabKeyUsedAsCharacter (false);
    name.setJustification (Justification::centredLeft);
    name.setIndents (6, 0);
    name.setFont (FontOptions (13.0f));
    name.setInputRestrictions (24);
    name.onTextChange = [this] { if (onChange) onChange(); };
    name.onMove = [this] (int d) { if (onMoveFocus) onMoveFocus (d); };
    addAndMakeVisible (name);

    WebLook::setStyle (arm, "small");
    arm.setClickingTogglesState (true);
    arm.setTooltip ("Record arm");
    arm.setColour (TextButton::buttonOnColourId, theme::record);
    arm.setColour (TextButton::textColourOnId, Colours::white);
    arm.onClick = [this] { if (onChange) onChange(); };
    addAndMakeVisible (arm);

    meter.setTooltip ("Input level. Click to reset the peak.");
    addAndMakeVisible (meter);
}

void TrackRow::addWave (double seconds, float peak)
{
    const auto i = (size_t) jmax (0, (int) (seconds * wavesPerSecond));
    if (wave.size() <= i) wave.resize (i + 1, peak);
    wave[i] = jmax (wave[i], peak);
}

void TrackRow::resized()
{
    // Same box as the web .track-header: 6px/10px padding, two rows 6px apart.
    auto r = getLocalBounds().reduced (10, 6).withTrimmedBottom (1);
    const int rowH = (r.getHeight() - 6) / 2;
    auto row1 = r.removeFromTop (rowH);
    r.removeFromTop (6);
    auto row2 = r;

    number.setBounds (row1.removeFromLeft (20));
    row1.removeFromLeft (6);
    arm.setBounds (row1.removeFromRight (26).withSizeKeepingCentre (26, 22));
    row1.removeFromRight (6);
    name.setBounds (row1);

    meter.setBounds (row2.removeFromLeft (LevelMeter::barWidth + 6 + LevelMeter::readoutWidth));
}

void TrackRow::paint (Graphics& g)
{
    g.setColour (theme::line);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}
