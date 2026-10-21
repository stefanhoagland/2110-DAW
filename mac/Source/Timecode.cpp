#include "Timecode.h"

using namespace juce;

namespace tc
{
String Rate::name() const
{
    if (drop) return "29.97 DF";
    if (nominal == 30 && fps < 29.99) return "29.97 fps";
    if (nominal == 24 && fps < 23.99) return "23.976 fps";
    return String (nominal) + " fps";
}

String Rate::shortName() const
{
    return name().replace (" fps", "").removeCharacters (" ");
}

Rate snap (double measured, bool drop)
{
    if (drop) return { 30, true, 30000.0 / 1001.0 };
    const Rate all[] = { { 24, false, 24000.0 / 1001.0 }, { 24, false, 24.0 }, { 25, false, 25.0 },
                         { 30, false, 30000.0 / 1001.0 }, { 30, false, 30.0 } };
    Rate best = all[2];
    for (auto& r : all)
        if (std::abs (r.fps - measured) < std::abs (best.fps - measured))
            best = r;
    return best;
}

Rate fromMtcCode (int code)
{
    switch (code & 3)
    {
        case 0:  return { 24, false, 24.0 };
        case 1:  return { 25, false, 25.0 };
        case 2:  return { 30, true, 30000.0 / 1001.0 };
        default: return { 30, false, 30.0 };
    }
}

int64 labelToFrames (int h, int m, int s, int f, const Rate& r)
{
    const int64 plain = ((int64) h * 3600 + m * 60 + s) * r.nominal + f;
    if (! r.drop) return plain;
    // Drop-frame skips labels :00 and :01 at the start of every minute except each tenth.
    const int64 minutes = (int64) h * 60 + m;
    return plain - 2 * (minutes - minutes / 10);
}

String toLabel (double frames, const Rate& r)
{
    const int64 perDay = labelToFrames (24, 0, 0, 0, r);
    int64 n = (int64) std::floor (frames);
    n = ((n % perDay) + perDay) % perDay;
    if (r.drop)
    {
        const int64 d = n / 17982, rem = n % 17982;
        n += 18 * d + (rem >= 2 ? 2 * ((rem - 2) / 1798) : 0);
    }
    const int f = (int) (n % r.nominal);
    const int64 secs = n / r.nominal;
    auto two = [] (int64 v) { return String (v).paddedLeft ('0', 2); };
    return two (secs / 3600) + ":" + two ((secs / 60) % 60) + ":" + two (secs % 60) + (r.drop ? ";" : ":") + two (f);
}

int64 toSamples (double frames, const Rate& r, double sampleRate)
{
    return (int64) std::llround (frames / r.fps * sampleRate);
}

//==============================================================================
void LtcDecoder::reset (double sampleRate)
{
    *this = LtcDecoder();
    sr = jmax (8000.0, sampleRate);
    decay = (float) std::exp (-1.0 / (0.2 * sr)); // level follower with ~200 ms memory
    period = sr / 2160.0;                           // between 23.976 and 30 fps (80 bits a frame)
}

bool LtcDecoder::process (const float* x, int n, int64 firstSample)
{
    const int64 before = lastFrameEnd;
    for (int i = 0; i < n; ++i)
    {
        const float v = x[i];
        env = jmax (std::abs (v), env * decay);
        const float th = jmax (0.002f, env * 0.2f); // hysteresis so noise doesn't make edges
        if (state <= 0 && v > th)
        {
            if (state != 0) edge (firstSample + i);
            state = 1;
        }
        else if (state >= 0 && v < -th)
        {
            if (state != 0) edge (firstSample + i);
            state = -1;
        }
    }
    return lastFrameEnd != before;
}

void LtcDecoder::edge (int64 at)
{
    if (lastEdge < 0) { lastEdge = at; return; }
    const double len = (double) (at - lastEdge);
    lastEdge = at;
    // Bits are 1/2400 to 1/1918 s long; allow some speed variation.
    const double minPeriod = sr / 2400.0 * 0.8, maxPeriod = sr / 1918.0 * 1.25;
    if (len < 0.25 * period || len > 1.5 * period)
    {
        half = false; // noise or a gap; start over on the next clean bit
        return;
    }
    if (len < 0.75 * period)
    {
        if (! half) { half = true; return; }
        half = false;
        bit (1, at);
    }
    else
    {
        half = false;
        period += 0.1 * (len - period);
        bit (0, at);
    }
    period = jlimit (minPeriod, maxPeriod, period);
}

void LtcDecoder::bit (int b, int64 at)
{
    lo = (lo >> 1) | ((std::uint64_t) (hi & 1) << 63);
    hi = (std::uint16_t) ((hi >> 1) | (b << 15));
    if (hi == 0xBFFC) // sync word 0011 1111 1111 1101, read in transmission order
        frameDone (at);
}

void LtcDecoder::frameDone (int64 at)
{
    auto digit = [this] (int shift, int bits) { return (int) ((lo >> shift) & ((1u << bits) - 1)); };
    const int fu = digit (0, 4), ft = digit (8, 2), su = digit (16, 4), st = digit (24, 3),
              mu = digit (32, 4), mt = digit (40, 3), hu = digit (48, 4), ht = digit (56, 2);
    if (fu > 9 || su > 9 || mu > 9 || hu > 9 || st > 5 || mt > 5)
        return;
    const int f = ft * 10 + fu, s = st * 10 + su, m = mt * 10 + mu, h = ht * 10 + hu;
    if (f > 29 || h > 23)
        return;
    dropFlag = digit (10, 1) == 1;

    // Measure the rate over a run of back-to-back frames.
    const double frameLen = 80.0 * period;
    if (runLength > 0 && std::abs ((double) (at - lastFrameEnd) - frameLen) < 0.15 * frameLen)
        ++runLength;
    else
    {
        runLength = 1; // this frame starts a new run
        runStartSample = at;
    }
    const double measured = runLength >= 12 ? (double) (runLength - 1) * sr / (double) (at - runStartSample)
                                            : sr / frameLen;
    rate = snap (measured, dropFlag);
    rateKnown = runLength >= 12;
    // A frame number the rate can't have means the guess was wrong (e.g. 24 when it's 25).
    if (f >= rate.nominal)
        rate = snap (f >= 25 ? 30.0 : 25.0, dropFlag);

    lastFrame = labelToFrames (h, m, s, f, rate);
    lastFrameEnd = at;
}

//==============================================================================
bool MtcParser::quarterFrame (int piece, int value)
{
    if (piece != next) { next = 0; return false; } // wait for a clean sequence from piece 0
    pieces[piece] = value & 15;
    next = piece + 1;
    if (piece < 7) return false;
    next = 0;
    const int f = pieces[0] | ((pieces[1] & 1) << 4);
    const int s = pieces[2] | ((pieces[3] & 3) << 4);
    const int m = pieces[4] | ((pieces[5] & 3) << 4);
    const int h = pieces[6] | ((pieces[7] & 1) << 4);
    rate = fromMtcCode ((pieces[7] >> 1) & 3);
    // The time in a full set of quarter frames was true when piece 0 was sent, two frames ago.
    frames = labelToFrames (h, m, s, f, rate) + 2;
    return true;
}

bool MtcParser::fullFrame (int h, int m, int s, int f, int rateCode)
{
    rate = fromMtcCode (rateCode);
    frames = labelToFrames (h, m, s, f, rate);
    next = 0;
    return true;
}
}
