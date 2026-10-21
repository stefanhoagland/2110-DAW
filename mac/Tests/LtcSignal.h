#pragma once
// Makes a test LTC signal (SMPTE 12M linear timecode), so the decoder can be checked
// without a real timecode generator.
#include <vector>
#include <cmath>
#include <random>

struct LtcSignal
{
    int nominal = 25; bool drop = false; double fps = 25.0;

    // Audio for `seconds`, starting at h:m:s:f, at `sampleRate`.
    std::vector<float> make (int h, int m, int s, int f, double seconds, double sampleRate) const
    {
        std::vector<float> out ((size_t) (seconds * sampleRate), 0.0f);
        const double bitLen = sampleRate / (fps * 80.0);
        double t = 0.0; // start of the current bit, in samples
        float level = 0.3f;
        size_t n = 0;
        auto fillTo = [&] (double until)
        {
            for (; n < out.size() && (double) n < until; ++n) out[n] = level;
        };
        while (n < out.size())
        {
            int bits[80] = {};
            auto put = [&] (int at, int value, int count) { for (int i = 0; i < count; ++i) bits[at + i] = (value >> i) & 1; };
            put (0, f % 10, 4);  put (8, f / 10, 2);  bits[10] = drop ? 1 : 0;
            put (16, s % 10, 4); put (24, s / 10, 3);
            put (32, m % 10, 4); put (40, m / 10, 3);
            put (48, h % 10, 4); put (56, h / 10, 2);
            const int sync[16] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
            for (int i = 0; i < 16; ++i) bits[64 + i] = sync[i];
            for (int b = 0; b < 80; ++b)
            {
                level = -level; // every bit starts with a transition
                if (bits[b]) { fillTo (t + bitLen / 2); level = -level; }
                t += bitLen;
                fillTo (t);
            }
            // Next label (drop-frame skips :00 and :01 each minute except every tenth).
            if (++f >= nominal) { f = 0; if (++s >= 60) { s = 0; if (++m >= 60) { m = 0; h = (h + 1) % 24; } } }
            if (drop && s == 0 && f < 2 && m % 10 != 0) f = 2;
        }
        // Soften the edges and add a little noise, like a real cable.
        std::mt19937 rng (7);
        std::normal_distribution<float> noise (0.0f, 0.005f);
        float y = 0.0f;
        for (auto& v : out) { y += 0.5f * (v - y); v = y + noise (rng); }
        return out;
    }
};
