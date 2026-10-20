#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

// Audio input, metering and multitrack recording.
// Every input channel of the interface is a track. While recording, each armed
// track streams to its own 24-bit mono WAV file on a background thread, so the
// audio thread never touches the disk.
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    static constexpr int maxChannels = 256;

    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& devices() { return deviceManager; }

    // Opens the saved device setup (or the default device) with every input enabled.
    void initialise (const juce::XmlElement* savedState);

    int numInputs() const { return inputCount.load(); }
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

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    void selectLargestInput();

    juce::AudioDeviceManager deviceManager;
    juce::TimeSliceThread writerThread { "Soundbending Live disk writer" };

    std::array<std::atomic<float>, maxChannels> peaks {};
    std::array<int, maxChannels> activeToChannel {}; // callback input index -> device channel
    std::atomic<int> inputCount { 0 };
    std::atomic<double> currentRate { 0.0 };

    juce::SpinLock writersLock; // held by the audio thread while writing; by start/stop to swap
    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> writers; // by device channel
    std::atomic<bool> recording { false };
    std::atomic<juce::int64> samplesRecorded { 0 };
    std::atomic<int> dropped { 0 };
    juce::StringArray takeFiles;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
