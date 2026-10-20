#include "AudioEngine.h"
#include "Names.h"

using namespace juce;

static const String sep = String (CharPointer_UTF8 ("  \xc2\xb7  "));

AudioEngine::AudioEngine()
{
    writerThread.startThread (Thread::Priority::high);
    deviceManager.addAudioCallback (this);
}

AudioEngine::~AudioEngine()
{
    stopRecording();
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    writerThread.stopThread (5000);
}

void AudioEngine::initialise (const XmlElement* savedState)
{
    // No outputs yet; the live console will add them.
    auto err = deviceManager.initialise (maxChannels, 2, savedState, true);
    if (err.isNotEmpty())
        deviceManager.initialiseWithDefaultDevices (maxChannels, 2);

    // Make sure every input of the device is on (a saved setup may have fewer).
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        const int n = dev->getInputChannelNames().size();
        if (setup.inputChannels.countNumberOfSetBits() < n)
        {
            setup.useDefaultInputChannels = false;
            setup.inputChannels.clear();
            setup.inputChannels.setRange (0, jmin (n, maxChannels), true);
            deviceManager.setAudioDeviceSetup (setup, true);
        }
    }
}

String AudioEngine::deviceDescription() const
{
    auto* dev = deviceManager.getCurrentAudioDevice();
    if (dev == nullptr)
        return "No audio device";
    const int buffer = dev->getCurrentBufferSizeSamples();
    const double rate = dev->getCurrentSampleRate();
    return dev->getName() + sep + String (numInputs()) + " inputs" + sep
         + String (rate / 1000.0, 1) + " kHz" + sep + String (buffer) + " samples ("
         + String (1000.0 * buffer / jmax (1.0, rate), 1) + " ms)";
}

float AudioEngine::takePeak (int channel)
{
    if (! isPositiveAndBelow (channel, maxChannels))
        return 0.0f;
    return peaks[(size_t) channel].exchange (0.0f);
}

void AudioEngine::audioDeviceAboutToStart (AudioIODevice* dev)
{
    auto active = dev->getActiveInputChannels();
    int k = 0;
    for (int ch = 0; ch < maxChannels && k < maxChannels; ++ch)
        if (active[ch])
            activeToChannel[(size_t) k++] = ch;
    inputCount = jmin (maxChannels, dev->getInputChannelNames().size());
    currentRate = dev->getCurrentSampleRate();
}

void AudioEngine::audioDeviceStopped()
{
    currentRate = 0.0;
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numIns,
                                                    float* const* outputs, int numOuts, int numSamples,
                                                    const AudioIODeviceCallbackContext&)
{
    for (int i = 0; i < numIns; ++i)
    {
        if (inputs[i] == nullptr) continue;
        auto range = FloatVectorOperations::findMinAndMax (inputs[i], numSamples);
        const float p = jmax (-range.getStart(), range.getEnd());
        auto& slot = peaks[(size_t) activeToChannel[(size_t) i]];
        if (p > slot.load (std::memory_order_relaxed))
            slot.store (p, std::memory_order_relaxed);
    }

    if (recording.load())
    {
        const SpinLock::ScopedTryLockType lock (writersLock);
        if (lock.isLocked())
        {
            bool lost = false;
            for (int i = 0; i < numIns; ++i)
            {
                const auto ch = (size_t) activeToChannel[(size_t) i];
                if (ch < writers.size() && writers[ch] != nullptr && inputs[i] != nullptr)
                    if (! writers[ch]->write (&inputs[i], numSamples))
                        lost = true;
            }
            if (lost) ++dropped;
            samplesRecorded += numSamples;
        }
    }

    for (int i = 0; i < numOuts; ++i)
        if (outputs[i] != nullptr)
            FloatVectorOperations::clear (outputs[i], numSamples);
}

String AudioEngine::startRecording (const File& folder, int take, const std::vector<Track>& tracks)
{
    stopRecording();
    const double rate = sampleRate();
    if (rate <= 0.0)
        return "The audio device isn't running. Check Audio settings.";
    if (tracks.empty())
        return "No tracks are armed.";
    if (! folder.createDirectory())
        return "Can't create the folder " + folder.getFullPathName();

    WavAudioFormat wav;
    std::vector<std::unique_ptr<AudioFormatWriter::ThreadedWriter>> next ((size_t) maxChannels);
    StringArray files;
    const int bufferSamples = (int) (rate * 4.0); // 4 s of slack per track for slow disks

    for (auto& t : tracks)
    {
        auto file = folder.getChildFile (names::takeFileName (take, t.channel, t.name));
        file.deleteFile();
        std::unique_ptr<OutputStream> stream = std::make_unique<FileOutputStream> (file);
        if (! static_cast<FileOutputStream*> (stream.get())->openedOk())
            return "Can't write " + file.getFullPathName();

        auto opts = AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24);
        auto writer = wav.createWriterFor (stream, opts);
        if (writer == nullptr)
            return "Can't create a WAV file at " + String (rate) + " Hz.";
        next[(size_t) t.channel] = std::make_unique<AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, bufferSamples);
        files.add (file.getFullPathName());
    }

    {
        const SpinLock::ScopedLockType lock (writersLock);
        writers = std::move (next);
        samplesRecorded = 0;
        dropped = 0;
        takeFiles = files;
    }
    recording = true;
    return {};
}

void AudioEngine::stopRecording()
{
    recording = false;
    std::vector<std::unique_ptr<AudioFormatWriter::ThreadedWriter>> old;
    {
        const SpinLock::ScopedLockType lock (writersLock);
        old.swap (writers);
    }
    old.clear(); // each ThreadedWriter flushes its buffer and closes its file here
}

double AudioEngine::recordedSeconds() const
{
    const double rate = sampleRate();
    return rate > 0.0 ? (double) samplesRecorded.load() / rate : 0.0;
}
