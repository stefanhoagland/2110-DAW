// Records through the real AudioEngine with a pretend interface and checks
// the WAV files it writes. Run: ./EngineTest
#include "../Source/AudioEngine.h"
#include "LtcSignal.h"
#include "RtpSender.h"
#include <iostream>

using namespace juce;

// A stand-in audio interface: 64 inputs at 48 kHz, driven by hand.
struct FakeDevice : AudioIODevice
{
    FakeDevice() : AudioIODevice ("Fake 64", "test") { active.setRange (0, 64, true); activeOut.setRange (0, 8, true); }
    StringArray getOutputChannelNames() override { StringArray s; for (int i = 0; i < 8; ++i) s.add ("Out " + String (i + 1)); return s; }
    StringArray getInputChannelNames() override { StringArray s; for (int i = 0; i < 64; ++i) s.add ("In " + String (i + 1)); return s; }
    Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    Array<int> getAvailableBufferSizes() override { return { 256 }; }
    int getDefaultBufferSize() override { return 256; }
    String open (const BigInteger&, const BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return 256; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 24; }
    BigInteger getActiveOutputChannels() const override { return activeOut; }
    BigInteger getActiveInputChannels() const override { return active; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
    BigInteger active, activeOut;
};

// A minimal "plugin" for the insert tests: multiplies by a gain, mono or stereo.
struct GainPlugin : AudioPluginInstance
{
    explicit GainPlugin (float g, bool monoOk = true)
        : AudioPluginInstance (BusesProperties().withInput ("In", AudioChannelSet::stereo()).withOutput ("Out", AudioChannelSet::stereo())),
          gain (g), allowMono (monoOk) {}
    void fillInPluginDescription (PluginDescription& d) const override { d.name = "Test Gain"; d.pluginFormatName = "Test"; }
    const String getName() const override { return "Test Gain"; }
    void prepareToPlay (double, int) override { prepared = true; }
    void releaseResources() override {}
    void processBlock (AudioBuffer<float>& b, MidiBuffer&) override { b.applyGain (gain); lastChannels = b.getNumChannels(); }
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
        return in == out && (in == AudioChannelSet::stereo() || (allowMono && in == AudioChannelSet::mono()));
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const String getProgramName (int) override { return {}; }
    void changeProgramName (int, const String&) override {}
    void getStateInformation (MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
    float gain;
    bool allowMono, prepared = false;
    int lastChannels = 0;
};

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    if (! ok) ++failures;
}

int main()
{
    ScopedJuceInitialiser_GUI init;
    AudioEngine engine;
    FakeDevice dev;
    engine.audioDeviceAboutToStart (&dev);
    check (engine.numInputs() == 64, "64 inputs seen");

    auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("2110-engine-test");
    dir.deleteRecursively();

    auto err = engine.startRecording (dir, 7, { { 0, "Kick" }, { 63, "Room R" } });
    check (err.isEmpty(), "recording starts " + err);

    const int block = 256, blocks = 48000 * 3 / block; // 3 seconds
    AudioBuffer<float> in (64, block);
    AudioIODeviceCallbackContext ctx;
    int64 pos = 0;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < block; ++i, ++pos)
        {
            in.setSample (0, i, 0.5f * std::sin ((float) pos * 0.05f));
            in.setSample (63, i, i == 0 ? 1.0f : 0.0f); // a click at every block start
        }
        engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 64, nullptr, 0, block, ctx);
        if (b % 32 == 0) Thread::sleep (2); // let the writer thread run, as real time would
    }
    check (std::abs (engine.recordedSeconds() - 3.0) < 0.01, "3 s counted");
    check (engine.takePeak (0) > 0.49f && engine.takePeak (5) == 0.0f, "meter peaks");
    engine.stopRecording();
    check (engine.droppedBlocks() == 0, "no dropped audio");

    WavAudioFormat wav;
    auto kick = dir.getChildFile ("T07_01_Kick.wav");
    auto room = dir.getChildFile ("T07_64_Room_R.wav");
    check (kick.existsAsFile() && room.existsAsFile(), "files named T07_01_Kick.wav and T07_64_Room_R.wav");
    check (dir.getNumberOfChildFiles (File::findFiles) == 2, "only armed tracks written");

    std::unique_ptr<AudioFormatReader> r (wav.createReaderFor (kick.createInputStream().release(), true));
    check (r != nullptr && r->numChannels == 1 && r->bitsPerSample == 24 && r->sampleRate == 48000.0, "24-bit mono 48 kHz");
    check (r != nullptr && r->lengthInSamples == (int64) blocks * block, "every sample written");
    if (r != nullptr)
    {
        AudioBuffer<float> back (1, 1000);
        r->read (&back, 0, 1000, 12345, true, false);
        float err24 = 0.0f;
        for (int i = 0; i < 1000; ++i)
            err24 = jmax (err24, std::abs (back.getSample (0, i) - 0.5f * std::sin ((float) (12345 + i) * 0.05f)));
        check (err24 < 1.0e-5f, "audio matches (max error " + String (err24) + ")");
    }

    // ---- Live mix ----
    AudioBuffer<float> out (8, block);
    auto run = [&] (int count)
    {
        for (int b = 0; b < count; ++b)
        {
            for (int i = 0; i < block; ++i, ++pos)
            {
                in.setSample (0, i, 0.5f * std::sin ((float) pos * 0.05f));
                in.setSample (63, i, 0.0f);
            }
            engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 64, out.getArrayOfWritePointers(), 8, block, ctx);
        }
        return std::pair<float, float> { out.getMagnitude (0, 0, block), out.getMagnitude (1, 0, block) };
    };
    for (int c = 1; c < 64; ++c) engine.strip (c).faderDb = -60.0f; // only channel 1 up
    auto [l0, r0] = run (4);
    check (l0 == 0.0f && r0 == 0.0f, "silent until Go live");
    check (engine.takeStripPeak (0) > 0.49f, "strip meter runs before going live");

    engine.setLive (true);
    auto [l1, r1] = run (4);
    check (std::abs (l1 - r1) < 0.01f && std::abs (l1 - 0.5f * 0.7071f) < 0.01f, "centre pan: -3 dB each side (" + String (l1) + ", " + String (r1) + ")");

    engine.strip (0).pan = -1.0f;
    auto [l2, r2] = run (4);
    check (std::abs (l2 - 0.5f) < 0.01f && r2 < 0.001f, "hard left pan");

    engine.strip (0).faderDb = -6.0f;
    auto [l3, r3] = run (4);
    check (std::abs (l3 - 0.5f * 0.5012f) < 0.01f, "fader at -6 dB");

    engine.strip (5).solo = true;
    auto [l4, r4] = run (4);
    check (l4 == 0.0f, "solo on another channel silences this one");
    engine.strip (5).solo = false;

    engine.strip (0).mute = true;
    engine.takeStripPeak (0);
    auto [l5, r5] = run (4);
    check (l5 == 0.0f, "mute");
    check (engine.takeStripPeak (0) > 0.2f, "a muted channel's meter still shows its level");
    engine.strip (0).mute = false;

    engine.strip (0).faderDb = 10.0f;
    engine.strip (0).trimDb = 20.0f;
    auto [l6, r6] = run (8);
    check (l6 <= 0.8915f && l6 > 0.85f, "limiter holds the output at -1 dB (" + String (l6) + ")");
    engine.setLimiter (false);
    auto [l7, r7] = run (4);
    check (l7 > 10.0f, "limiter off lets it through");

    engine.setLive (false);
    auto [l8, r8] = run (2);
    check (l8 == 0.0f, "silent after leaving live");

    // ---- Patching ----
    engine.setLimiter (true);
    engine.strip (0).trimDb = 0.0f;
    engine.strip (0).faderDb = 0.0f;
    engine.strip (0).pan = 0.0f;
    engine.strip (2).faderDb = 0.0f;
    engine.setTrackSource (0, -1);    // track 1 unpatched
    engine.setTrackSource (2, 0);     // input 1 now feeds track 3
    run (1);
    engine.takePeak (0); engine.takePeak (2); // forget peaks from before the change
    run (2);
    check (engine.takePeak (0) == 0.0f && engine.takePeak (2) > 0.49f, "input patch moves input 1 to track 3");
    engine.setLive (true);
    engine.setPatch (AudioEngine::mixLeft, 0, false);
    engine.setPatch (AudioEngine::mixLeft, 4, true);  // mix L to output 5
    engine.setPatch (AudioEngine::firstDirect + 2, 7, true); // track 3 direct to output 8
    run (4);
    check (out.getMagnitude (0, 0, block) == 0.0f && out.getMagnitude (4, 0, block) > 0.3f, "mix L patched to output 5 (" + String (out.getMagnitude (0, 0, block)) + ", " + String (out.getMagnitude (4, 0, block)) + ")");
    check (std::abs (out.getMagnitude (7, 0, block) - 0.5f) < 0.01f, "track direct out to output 8");
    engine.setLive (false);
    run (2);
    check (out.getMagnitude (4, 0, block) == 0.0f && out.getMagnitude (7, 0, block) > 0.49f, "direct out keeps going when not live");
    engine.resetPatches();
    run (2);
    check (engine.takePeak (0) > 0.49f && engine.getPatch (AudioEngine::mixLeft, 0) && ! engine.getPatch (AudioEngine::mixLeft, 4), "reset to 1:1");

    // ---- Plugin inserts ----
    engine.strip (2).faderDb = -60.0f;
    engine.setLimiter (false); // measure the plugins, not the limiter's recovery from the earlier +30 dB test
    engine.setLive (true);
    auto [b0, b0r] = run (4);
    auto half = std::make_unique<GainPlugin> (0.5f);
    auto* halfPtr = half.get();
    check (engine.addInsert (0, std::move (half), {}), "insert added");
    check (halfPtr->prepared && halfPtr->getTotalNumInputChannels() == 1, "insert prepared, running mono");
    auto [b1, b1r] = run (4);
    check (std::abs (b1 - 0.5f * b0) < 0.01f, "insert halves the level (" + String (b0) + " -> " + String (b1) + ")");
    engine.takeStripPeak (0);
    run (2);
    check (std::abs (engine.takeStripPeak (0) - 0.25f) < 0.01f, "strip meter is after the inserts");

    auto stereoOnly = std::make_unique<GainPlugin> (2.0f, false);
    auto* stereoPtr = stereoOnly.get();
    engine.addInsert (0, std::move (stereoOnly), {});
    auto [b2, b2r] = run (4);
    check (stereoPtr->lastChannels == 2 && std::abs (b2 - b0) < 0.01f, "stereo-only plugin runs in stereo, chain 0.5 x 2 = unity");
    engine.getInserts (0)[0]->bypass = true;
    auto [b3, b3r] = run (4);
    check (std::abs (b3 - 2.0f * b0) < 0.02f, "bypass skips a plugin");
    engine.moveInsert (0, 1, 0);
    check (engine.getInserts (0)[0]->plugin.get() == stereoPtr, "move changes the order");
    engine.removeInsert (0, 0);
    engine.removeInsert (0, 0);
    auto [b4, b4r] = run (4);
    check (engine.getInserts (0).empty() && std::abs (b4 - b0) < 0.01f, "removing plugins restores the dry level");

    // ---- Timecode ----
    {
        // Without timecode, takes are plain WAVs (no bext chunk).
        auto plain = std::unique_ptr<AudioFormatReader> (wav.createReaderFor (kick.createInputStream().release(), true));
        check (plain != nullptr && ! plain->metadataValues.containsKey (WavAudioFormat::bwavTimeReference), "no timecode: no BWF stamp");

        // 25 fps LTC from 10:00:00:00 on input 10.
        LtcSignal sig { 25, false, 25.0 };
        auto ltcAudio = sig.make (10, 0, 0, 0, 4.0, 48000.0);
        engine.setTimecodeLtc (9);
        size_t at = 0;
        auto feed = [&] (int count)
        {
            for (int b = 0; b < count && at + block <= ltcAudio.size(); ++b, at += block)
            {
                for (int i = 0; i < block; ++i) { in.setSample (9, i, ltcAudio[at + (size_t) i]); in.setSample (0, i, 0.1f); }
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 64, out.getArrayOfWritePointers(), 8, block, ctx);
            }
        };
        feed (48000 / block); // about a second to lock
        auto now = engine.timecodeNow();
        check (now.locked && now.rate.nominal == 25, "LTC locks at 25 fps");
        // Here `at` samples of LTC have gone by since 10:00:00:00.
        const double expectFrames = 900000.0 + (double) at / 48000.0 * 25.0;
        check (std::abs (now.frames - expectFrames) < 1.0, "clock reads " + tc::toLabel (now.frames, now.rate));

        const auto first = (int64) at;
        check (engine.startRecording (dir, 20, { { 0, "Kick" } }).isEmpty(), "record with timecode");
        feed (40);
        engine.stopRecording();
        check (engine.takeTimecode().seen, "take has a start timecode");
        std::unique_ptr<AudioFormatReader> tr (wav.createReaderFor (dir.getChildFile ("T20_01_Kick.wav").createInputStream().release(), true));
        const auto ref = tr != nullptr ? tr->metadataValues.getValue (WavAudioFormat::bwavTimeReference, "-1").getLargeIntValue() : -1;
        const int64 expectRef = (int64) 10 * 3600 * 48000 + first;
        check (std::abs (ref - expectRef) <= 2, "BWF time reference is the first sample (" + String (ref) + " vs " + String (expectRef) + ")");
        check (tr != nullptr && tr->metadataValues.getValue (WavAudioFormat::bwavDescription, {}) == "sTC=10:00:00:24 fps=25",
               "BWF description has the start timecode: " + (tr != nullptr ? tr->metadataValues.getValue (WavAudioFormat::bwavDescription, {}) : String()));
        check (tr != nullptr && tr->lengthInSamples == 40 * block, "stamped file still has all its audio");

        engine.setTimecodeOff();
        check (! engine.timecodeNow().active, "timecode off");
    }

    // ---- AES67 network inputs ----
    {
        auto& net = engine.network();
        net.setInterface ("127.0.0.1");
        aes67::StreamInfo info;
        String why;
        aes67::parseSdp ("v=0\no=- 9 9 IN IP4 127.0.0.1\ns=Net\nc=IN IP4 127.0.0.1\nm=audio 46010 RTP/AVP 98\n"
                         "a=rtpmap:98 L24/48000/8\na=ptime:1\n", info, why);
        check (net.subscribe (info).isEmpty(), "network stream subscribed");
        check (engine.numInputs() == 72 && engine.numDeviceInputs() == 64, "8 network channels become tracks 65-72");
        check (engine.inputName (66) == "Net 3", "network track named after the stream");
        RtpSender tx;
        tx.open ("127.0.0.1", 46010);
        AudioBuffer<float> small (64, 48), outs (8, 48);
        small.clear();
        const auto start = Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 400; ++i)
        {
            tx.send (48, [] (int, int c) { return c == 2 ? 0.3f : 0.0f; });
            while (Time::getMillisecondCounterHiRes() - start < i + 1.0) Thread::yield();
            engine.takePeak (66);
            engine.audioDeviceIOCallbackWithContext (small.getArrayOfReadPointers(), 64, outs.getArrayOfWritePointers(), 8, 48, ctx);
        }
        check (std::abs (engine.takePeak (66) - 0.3f) < 0.001f, "network channel 3 reaches track 67's meter");
        engine.setTrackSource (0, 66);
        engine.takePeak (0); // clear what earlier tests left
        engine.audioDeviceIOCallbackWithContext (small.getArrayOfReadPointers(), 64, outs.getArrayOfWritePointers(), 8, 48, ctx);
        { const float p0 = engine.takePeak (0); check (std::abs (p0 - 0.3f) < 0.001f, "a network channel can be patched to track 1 (" + String (p0) + ")"); }
        engine.setTrackSource (0, 0);
        net.unsubscribe (info.key());
        check (engine.numInputs() == 64, "unsubscribing removes the network tracks");
    }

    dir.deleteRecursively();
    std::cout << (failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
