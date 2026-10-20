#include "TrackArea.h"
#include "Names.h"
#include "Theme.h"

using namespace juce;

void TrackArea::layout (int width)
{
    setSize (width, theme::rulerHeight + rows.size() * theme::rowHeight);
    for (int i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, theme::rulerHeight + i * theme::rowHeight, theme::headerWidth - 1, theme::rowHeight);
}

void TrackArea::setTake (int t, double s, bool rec)
{
    take = t;
    seconds = s;
    recording = rec;
    repaint (theme::headerWidth, 0, getWidth() - theme::headerWidth, getHeight());
}

void TrackArea::zoom (double factor)
{
    pxPerSec = jlimit (2.0, 400.0, pxPerSec * factor);
    repaint();
}

// Ruler step that keeps labels about 80 px apart (same steps as the web version).
static double niceInterval (double pps)
{
    for (double s : { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0 })
        if (s * pps >= 80.0) return s;
    return 7200.0;
}

static String formatTime (double sec, bool ms)
{
    const int m = (int) (sec / 60.0);
    const double s = sec - m * 60.0;
    return String (m) + ":" + (ms ? String (s, 2).paddedLeft ('0', 5) : String ((int) s).paddedLeft ('0', 2));
}

void TrackArea::paint (Graphics& g)
{
    const int hw = theme::headerWidth;
    const int rh = theme::rowHeight, ruler = theme::rulerHeight;

    // Header column with the "Tracks" ruler spacer.
    g.setColour (theme::panel);
    g.fillRect (0, 0, hw - 1, getHeight());
    g.setColour (theme::line);
    g.fillRect (hw - 1, 0, 1, getHeight());
    g.fillRect (0, ruler - 1, hw - 1, 1);
    g.setColour (theme::textMuted);
    g.setFont (FontOptions (13.0f));
    g.drawText ("Tracks", 10, 0, 200, ruler - 1, Justification::centredLeft);

    // Timeline: keep the record head in view by scrolling left as the take grows.
    const int x0 = hw, w = getWidth() - hw;
    if (w <= 0) return;
    const double t0 = jmax (0.0, seconds - (w - 40) / pxPerSec);

    g.saveState();
    g.reduceClipRegion (x0, 0, w, getHeight());
    g.setColour (theme::background);
    g.fillRect (x0, 0, w, getHeight());

    g.setFont (FontOptions (11.0f));
    for (int i = 0; i < rows.size(); ++i)
    {
        const int y = ruler + i * rh;
        g.setColour (i % 2 ? theme::laneB : theme::laneA);
        g.fillRect (x0, y, w, rh);
        g.setColour (theme::line);
        g.fillRect (x0, y + rh - 1, w, 1);

        auto* row = rows[i];
        if (! row->hasTake() || seconds <= 0.0) continue;
        const float cx0 = (float) x0 + (float) jmax (0.0, -t0 * pxPerSec);
        const float cx1 = (float) x0 + (float) ((seconds - t0) * pxPerSec);
        g.setColour (recording ? theme::clipRecording : theme::clipDone);
        g.fillRect (cx0, (float) y + 3.0f, cx1 - cx0, (float) rh - 7.0f);

        // Waveform from the meter peaks collected while recording.
        const auto& wave = row->getWave();
        const float mid = (float) y + rh / 2.0f - 1.0f, amp = rh / 2.0f - 6.0f;
        g.setColour (recording ? theme::waveRecording : theme::waveDone);
        for (int x = (int) cx0; x < (int) cx1; ++x)
        {
            const double ta = t0 + (x - x0) / pxPerSec, tb = t0 + (x + 1 - x0) / pxPerSec;
            const auto a = (size_t) (ta * TrackRow::wavesPerSecond);
            const auto b = jmax (a + 1, (size_t) (tb * TrackRow::wavesPerSecond));
            float m = 0.0f;
            for (auto k = a; k < b && k < wave.size(); ++k) m = jmax (m, wave[k]);
            const float ph = jmax (0.5f, jmin (1.0f, m) * amp);
            g.fillRect ((float) x, mid - ph, 1.0f, ph * 2.0f);
        }
        g.setColour (theme::text.withAlpha (0.75f));
        g.drawText (names::takeFileName (take, row->channel(), row->getName()),
                    (int) cx0 + 6, y + 4, w, 14, Justification::centredLeft);
    }

    // Ruler
    g.setColour (theme::panel);
    g.fillRect (x0, 0, w, ruler);
    g.setColour (theme::line);
    g.fillRect (x0, ruler - 1, w, 1);
    const double step = niceInterval (pxPerSec);
    g.setColour (theme::textMuted);
    for (double s = std::floor (t0 / step) * step; s < t0 + w / pxPerSec; s += step)
    {
        const float x = (float) x0 + (float) ((s - t0) * pxPerSec);
        g.fillRect (x, (float) ruler - 10.0f, 1.0f, 9.0f);
        g.drawText (formatTime (s, step < 1.0), (int) x + 3, 4, 80, 14, Justification::centredLeft);
    }

    // Record/play head
    const float px = (float) x0 + (float) ((seconds - t0) * pxPerSec);
    g.setColour (recording ? theme::record : theme::text);
    g.fillRect (px, 0.0f, 1.5f, (float) getHeight());
    g.restoreState();
}
