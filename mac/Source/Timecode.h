#pragma once
#include <juce_core/juce_core.h>
#include <cstdint>

// SMPTE timecode: frame maths, an LTC decoder and an MTC (MIDI timecode) parser.
// Times are kept as a frame count since midnight, which handles drop-frame labels.
namespace tc
{
    struct Rate
    {
        int nominal = 25;    // frames per labelled second: 24, 25 or 30
        bool drop = false;   // 29.97 drop-frame labels
        double fps = 25.0;   // real frames per second (23.976, 29.97 for pulled-down rates)

        juce::String name() const;  // "25 fps", "29.97 DF", ...
        juce::String shortName() const; // "25", "29.97DF", ... for file stamps
    };

    // Snaps a measured frame rate to the nearest standard one.
    Rate snap (double measuredFps, bool drop);
    // MTC rate code 0..3 (24, 25, 29.97 DF, 30).
    Rate fromMtcCode (int code);

    juce::int64 labelToFrames (int h, int m, int s, int f, const Rate&);
    // "01:02:03:04" (";" before the frames for drop-frame). Fractions are dropped.
    juce::String toLabel (double frames, const Rate&);
    // Samples since midnight for BWF files.
    juce::int64 toSamples (double frames, const Rate&, double sampleRate);

    //==============================================================================
    // Reads LTC (SMPTE 12M linear timecode) from an audio signal. Biphase-mark coded:
    // every bit starts with a transition, and a 1 has another one halfway. Real-time
    // safe: no allocation in process(). Forward playback only.
    class LtcDecoder
    {
    public:
        void reset (double sampleRate);

        // Feeds a block. `firstSample` is the running sample position of samples[0].
        // Returns true if at least one frame finished in this block.
        bool process (const float* samples, int numSamples, juce::int64 firstSample);

        // The frame that most recently finished: its frame count (as labelled), the sample
        // just after it ended (where the next frame starts), and the rate.
        juce::int64 lastFrame = -1, lastFrameEnd = 0;
        Rate rate;
        bool rateKnown = false;  // set once about a second of steady frames has been measured

    private:
        void edge (juce::int64 at);
        void bit (int b, juce::int64 at);
        void frameDone (juce::int64 at);

        double sr = 48000.0;
        float env = 0.0f, decay = 0.9999f;
        int state = 0; // -1 low, +1 high, 0 not started
        juce::int64 lastEdge = -1;
        double period = 0.0; // length of one bit, in samples
        bool half = false;   // first half of a 1 seen
        std::uint64_t lo = 0; std::uint16_t hi = 0; // last 80 bits; bit 79 is newest
        // Rate measuring: a run of consecutive frames.
        juce::int64 runStartFrame = -1, runStartSample = 0, runLength = 0;
        int maxFrameNumber = 0;
        bool dropFlag = false;
    };

    //==============================================================================
    // Assembles MIDI timecode from quarter-frame and full-frame messages.
    class MtcParser
    {
    public:
        // Returns true when a new time is complete. `frames` is the time at the moment the
        // message arrived (quarter-frame times are 2 frames old when the last piece lands).
        bool quarterFrame (int piece, int value);
        bool fullFrame (int h, int m, int s, int f, int rateCode);
        juce::int64 frames = 0;
        Rate rate;

    private:
        int pieces[8] {};
        int next = 0; // piece expected next; quarter frames must arrive in order
    };
}
