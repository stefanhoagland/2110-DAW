// Records through the real AudioEngine with a pretend interface and checks
// the WAV files it writes. Run: ./EngineTest
#include "../Source/AudioEngine.h"
#include <iostream>

using namespace juce;

// A stand-in audio interface: 64 inputs at 48 kHz, driven by hand.
struct FakeDevice : AudioIODevice
{
    FakeDevice() : AudioIODevice ("Fake 64", "test") { active.setRange (0, 64, true); }
    StringArray getOutputChannelNames() override { return {}; }
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
    BigInteger getActiveOutputChannels() const override { return {}; }
    BigInteger getActiveInputChannels() const override { return active; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
    BigInteger active;
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

    dir.deleteRecursively();
    std::cout << (failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
