#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Timecode.h"
#include "Aes67.h"

// Audio input, metering, multitrack recording and the live mix.
// Every input channel of the interface is a track. While recording, each armed
// track streams to its own 24-bit mono WAV file on a background thread, so the
// audio thread never touches the disk.
class AudioEngine : public juce::AudioIODeviceCallback,
                    private juce::MidiInputCallback
{
public:
    static constexpr int maxChannels = 256;

    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& devices() { return deviceManager; }

    // Opens the saved device setup (or the default device) with every input enabled.
    void initialise (const juce::XmlElement* savedState);

    // Tracks: the interface's inputs, then any AES67 network channels.
    int numInputs() const { return juce::jmin (maxChannels, inputCount.load() + net.numChannels()); }
    int numDeviceInputs() const { return inputCount.load(); }
    juce::String inputName (int input) const; // "In 3", or the network channel's name
    aes67::NetworkAudio& network() { return net; }
    double sampleRate() const { return currentRate.load(); }
    juce::String deviceDescription() const;

    // Highest level seen on a channel since the last call (0..1+), then resets.
    float takePeak (int channel);

    struct Track { int channel; juce::String name; };

    // Starts writing the given tracks into `folder`. Returns an error message,
    // or an empty string on success.
    juce::String startRecording (const juce::File& folder, int take, const std::vector<Track>& tracks);
    // Stops and finishes the files (flushes buffers and fixes WAV headers).
    void stopRecording();

    bool isRecording() const { return recording.load(); }
    double recordedSeconds() const;
    int droppedBlocks() const { return dropped.load(); }
    juce::StringArray lastTakeFiles() const { return takeFiles; }

    // ---- Live mix (the Live tab) ----
    // Each input: trim -> fader -> pan into a stereo bus -> master fader ->
    // optional limiter -> the first two outputs of the device. Same chain as the
    // web version's Live mode. Controls are atomics so the UI can set them any time.
    struct Strip
    {
        std::atomic<float> trimDb { 0.0f };   // -20..+20
        std::atomic<float> faderDb { 0.0f };  // -60..+10; at or below -60 is off
        std::atomic<float> pan { 0.0f };      // -1 (left)..+1 (right)
        std::atomic<bool> mute { false }, solo { false };
    };
    Strip& strip (int channel) { return strips[(size_t) juce::jlimit (0, maxChannels - 1, channel)]; }

    void setLive (bool on) { liveOn = on; }
    bool isLive() const { return liveOn.load(); }
    void setLimiter (bool on) { limiterOn = on; }
    bool limiterEnabled() const { return limiterOn.load(); }
    void setMasterDb (float db) { masterDb = db; }
    float getMasterDb() const { return masterDb.load(); }
    int numOutputs() const { return outputCount.load(); }
    double outputLatencyMs() const;

    // Post-fader level of a strip, and the master output (after the limiter),
    // since the last call; then resets.
    float takeStripPeak (int channel);
    float takeMasterPeak (int side);

    static float faderGain (float db) { return db <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (db); }

    // ---- Plugin inserts (VST3 / AU) ----
    // Each track runs trim -> its inserts in order (skipping bypassed ones) -> fader,
    // like the web version. Plugins run mono if they support it, otherwise in
    // stereo with the input on both sides and the two outputs averaged.
    struct Insert
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::PluginDescription description;
        std::atomic<bool> bypass { false };
        int channels = 1; // channels the plugin runs with
    };
    using InsertList = std::vector<std::shared_ptr<Insert>>;
    static constexpr int maxInserts = 8;

    // Takes the plugin, sets its channel layout and prepares it. False if the list is full.
    bool addInsert (int track, std::unique_ptr<juce::AudioPluginInstance>, const juce::PluginDescription&, bool bypass = false);
    void removeInsert (int track, int index);
    void moveInsert (int track, int from, int to);
    InsertList getInserts (int track) const; // message thread

    // ---- Patching ----
    // Input patch: which device input feeds each track (-1 = none). Default is 1:1.
    // Tracks are what Studio records and what the Live strips mix.
    void setTrackSource (int track, int deviceInput);
    int getTrackSource (int track) const;
    // Output patch: any source to any device output (several sources on one output add up).
    // Sources: 0 = live mix left, 1 = live mix right, 2 + t = track t's direct signal.
    static constexpr int mixLeft = 0, mixRight = 1, firstDirect = 2, numSources = 2 + maxChannels;
    void setPatch (int source, int output, bool on);
    bool getPatch (int source, int output) const;
    void resetPatches(); // 1:1 inputs, mix to outputs 1 and 2

    // ---- Timecode ----
    // An external timecode source: LTC on one interface input, or MIDI timecode (MTC).
    // While it's locked, takes are stamped with the timecode of their first sample (BWF
    // time reference), so other DAWs place the files at the right time.
    void setTimecodeOff();
    void setTimecodeLtc (int deviceInput);
    bool setTimecodeMtc (const juce::String& midiDeviceId); // false if the port can't open
    struct TimecodeNow
    {
        bool active = false;  // a source is chosen
        bool locked = false;  // timecode arrived in the last half second
        bool seen = false;    // any timecode since the source was chosen
        double frames = 0.0;  // now, freewheeling from the last frame when not locked
        tc::Rate rate;
    };
    TimecodeNow timecodeNow() const;
    // Timecode of the current (or last) take's first sample; seen is false if it had none.
    TimecodeNow takeTimecode() const;
    // Writes the time reference and description into a WAV file's bext chunk.
    static bool stampBext (const juce::File& wav, juce::int64 timeReference, const juce::String& description);

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    void selectLargestInput();
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void stampTake();
    void mixLive (int tracks, float* const* outputs, int numOuts, int numSamples);

    juce::AudioDeviceManager deviceManager;
    juce::TimeSliceThread writerThread { "Soundbending Live disk writer" };

    std::array<std::atomic<float>, maxChannels> peaks {};
    std::array<int, maxChannels> activeToChannel {};  // callback input index -> device channel
    std::array<int, maxChannels> channelToActive {};  // device input -> callback index, or -1
    std::array<int, maxChannels> outChannelToActive {}; // device output -> callback index, or -1
    std::array<std::atomic<int>, maxChannels> trackSource;
    std::array<std::atomic<bool>, (size_t) numSources * maxChannels> patch {};
    std::array<const float*, maxChannels> trackIn {}; // audio thread only
    std::array<float, maxChannels> lastTrim {};
    std::array<InsertList, maxChannels> inserts;           // swapped by the message thread
    mutable std::array<juce::SpinLock, maxChannels> insertLocks; // audio thread only try-locks
    juce::AudioBuffer<float> fx;
    juce::MidiBuffer midi;
    void prepare (Insert&) const;
    void runInserts (int track, float* samples, int numSamples);
    std::atomic<int> inputCount { 0 };
    aes67::NetworkAudio net;
    juce::AudioBuffer<float> netBuf; // network channels for this block
    std::atomic<double> currentRate { 0.0 };

    std::array<Strip, maxChannels> strips;
    std::array<std::atomic<float>, maxChannels> stripPeaks {};
    std::array<std::atomic<float>, 2> masterPeaks {};
    std::array<float, maxChannels> lastL {}, lastR {}; // gains used last block, for smooth changes
    std::atomic<bool> liveOn { false }, limiterOn { true };
    std::atomic<float> masterDb { 0.0f };
    std::atomic<int> outputCount { 0 };
    float lastMaster = 0.0f, limiterEnv = 0.0f, limiterRelease = 0.0f;
    juce::AudioBuffer<float> work, bus;

    juce::SpinLock writersLock; // held by the audio thread while writing; by start/stop to swap
    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> writers; // by device channel
    std::atomic<bool> recording { false };
    std::atomic<juce::int64> samplesRecorded { 0 };
    std::atomic<int> dropped { 0 };
    juce::StringArray takeFiles;

    // Timecode. The anchor ties a timecode frame count to a sample position; the audio
    // thread only try-locks tcLock, so it never waits.
    struct Anchor { double frames = 0.0; juce::int64 sample = 0; tc::Rate rate; bool valid = false; };
    mutable juce::SpinLock tcLock;
    Anchor anchor;
    juce::int64 clockSample = 0; double clockMs = 0.0; // callback start, for MIDI and the UI
    juce::int64 samplePos = 0;                          // audio thread
    std::atomic<int> tcMode { 0 }, ltcInput { -1 };     // 0 off, 1 LTC, 2 MTC
    std::atomic<bool> ltcReset { true };
    tc::LtcDecoder ltc;                                 // audio thread
    tc::MtcParser mtc;                                  // MIDI thread
    std::unique_ptr<juce::MidiInput> mtcInput;
    std::atomic<bool> takeTcValid { false };
    double takeTcFrames = 0.0, takeSampleRate = 0.0; tc::Rate takeTcRate;     // written before takeTcValid is set

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
