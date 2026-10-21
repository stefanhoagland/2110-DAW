// Checks timecode maths, LTC decoding of a generated signal and MIDI timecode. Run: ./TimecodeTest
#include "../Source/Timecode.h"
#include "LtcSignal.h"
#include <iostream>

using namespace juce;

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    if (! ok) ++failures;
}

int main()
{
    const tc::Rate r25 { 25, false, 25.0 }, df { 30, true, 30000.0 / 1001.0 }, r30 { 30, false, 30.0 };

    check (tc::labelToFrames (1, 0, 0, 0, r25) == 90000, "01:00:00:00 at 25 fps is frame 90000");
    check (tc::toLabel (90000 + 24, r25) == "01:00:00:24", "frame back to a label");
    check (tc::labelToFrames (0, 1, 0, 2, df) == 1800, "drop-frame: 00:01:00;02 is frame 1800");
    check (tc::labelToFrames (0, 10, 0, 0, df) == 17982, "drop-frame: ten minutes is 17982 frames");
    check (tc::toLabel (1800, df) == "00:01:00;02" && tc::toLabel (1799, df) == "00:00:59;29", "drop-frame labels skip :00 and :01");
    check (tc::toLabel (17982, df) == "00:10:00;00", "drop-frame keeps :00 on the tenth minute");
    bool roundTrip = true;
    for (int64 n = 0; n < 200000; n += 37)
    {
        auto l = tc::toLabel ((double) n, df);
        auto p = StringArray::fromTokens (l.replaceCharacter (';', ':'), ":", "");
        roundTrip = roundTrip && tc::labelToFrames (p[0].getIntValue(), p[1].getIntValue(), p[2].getIntValue(), p[3].getIntValue(), df) == n;
    }
    check (roundTrip, "drop-frame label round trip");
    check (tc::toSamples ((double) tc::labelToFrames (1, 0, 0, 0, r25), r25, 48000.0) == 172800000, "BWF samples since midnight");
    check (tc::toSamples ((double) tc::labelToFrames (1, 0, 0, 0, r30), r30, 48000.0) == 172800000, "same at 30 fps");

    // LTC: generate a few seconds at each rate, decode in small blocks.
    struct Case { const char* name; LtcSignal sig; const char* expectName; };
    const Case cases[] = {
        { "24 fps",     { 24, false, 24.0 },             "24 fps" },
        { "23.976 fps", { 24, false, 24000.0 / 1001.0 }, "23.976 fps" },
        { "25 fps",     { 25, false, 25.0 },             "25 fps" },
        { "29.97 DF",   { 30, true, 30000.0 / 1001.0 },  "29.97 DF" },
        { "29.97 ND",   { 30, false, 30000.0 / 1001.0 }, "29.97 fps" },
        { "30 fps",     { 30, false, 30.0 },             "30 fps" },
    };
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (auto& c : cases)
        {
            auto audio = c.sig.make (10, 59, 58, 0, 4.0, sr);
            tc::LtcDecoder d;
            d.reset (sr);
            int frames = 0;
            double worst = 0.0;
            const tc::Rate rate { c.sig.nominal, c.sig.drop, c.sig.fps };
            const double start = (double) tc::labelToFrames (10, 59, 58, 0, rate);
            for (size_t i = 0; i < audio.size(); i += 256)
            {
                const int n = (int) std::min ((size_t) 256, audio.size() - i);
                if (d.process (audio.data() + i, n, (int64) i))
                {
                    ++frames;
                    if (d.rateKnown)
                    {
                        // The next frame starts where this one ended.
                        const double expected = start + (double) d.lastFrameEnd / sr * c.sig.fps;
                        worst = jmax (worst, std::abs ((double) (d.lastFrame + 1) - expected));
                    }
                }
            }
            const String what = String (c.name) + " at " + String (sr / 1000.0) + " kHz";
            check (d.rateKnown && d.rate.name() == c.expectName, what + ": rate detected as " + d.rate.name());
            check (frames > (int) (3.5 * c.sig.nominal / 2), what + ": frames decoded across the hour change");
            check (worst < 0.05, what + ": timing within " + String (worst, 3) + " frames");
            check (tc::toLabel ((double) d.lastFrame, d.rate).startsWith ("11:00:01"), what + ": last label " + tc::toLabel ((double) d.lastFrame, d.rate));
        }

    // Silence and noise produce nothing.
    {
        std::vector<float> quiet (48000, 0.0f);
        for (size_t i = 0; i < quiet.size(); ++i) quiet[i] = 0.2f * std::sin ((float) i * 0.03f);
        tc::LtcDecoder d;
        d.reset (48000.0);
        check (! d.process (quiet.data(), (int) quiet.size(), 0) && d.lastFrame < 0, "a sine wave isn't timecode");
    }

    // MTC: quarter frames for 01:02:03:04 at 25 fps.
    {
        tc::MtcParser p;
        const int h = 1, m = 2, s = 3, f = 4, code = 1;
        const int pieces[8] = { f & 15, f >> 4, s & 15, s >> 4, m & 15, m >> 4, h & 15, (h >> 4) | (code << 1) };
        bool done = false;
        p.quarterFrame (5, 3); // a stray piece mid-sequence is ignored
        for (int i = 0; i < 8; ++i) done = p.quarterFrame (i, pieces[i]);
        check (done && p.rate.nominal == 25 && p.frames == tc::labelToFrames (1, 2, 3, 6, r25), "MTC quarter frames (+2 frames)");
        check (p.fullFrame (23, 59, 59, 29, 2) && p.rate.drop && tc::toLabel ((double) p.frames, p.rate) == "23:59:59;29", "MTC full frame, 29.97 DF");
    }

    std::cout << (failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
