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
    // Two outputs feed the live mix (pick them in Audio settings).
    auto err = deviceManager.initialise (maxChannels, 2, savedState, true);
    if (err.isNotEmpty())
        deviceManager.initialiseWithDefaultDevices (maxChannels, 2);

    // First launch: the system default input is usually the built-in mic, so
    // pick the connected interface with the most inputs instead.
    if (savedState == nullptr)
        selectLargestInput();

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

void AudioEngine::selectLargestInput()
{
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (type == nullptr)
        return;
    type->scanForDevices();
    String best;
    int bestCount = 0;
    for (auto& name : type->getDeviceNames (true))
    {
        std::unique_ptr<AudioIODevice> probe (type->createDevice ({}, name));
        const int n = probe != nullptr ? probe->getInputChannelNames().size() : 0;
        if (n > bestCount) { best = name; bestCount = n; }
    }
    auto setup = deviceManager.getAudioDeviceSetup();
    if (best.isEmpty() || best == setup.inputDeviceName)
        return;
    setup.inputDeviceName = best;
    // Interfaces usually have outputs too; use the same one so the clocks match.
    if (type->getDeviceNames (false).contains (best))
        setup.outputDeviceName = best;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;
    deviceManager.setAudioDeviceSetup (setup, true);
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
    outputCount = dev->getActiveOutputChannels().countNumberOfSetBits();
    currentRate = dev->getCurrentSampleRate();

    const int block = jmax (dev->getCurrentBufferSizeSamples(), 4096);
    work.setSize (1, block, false, true, false);
    bus.setSize (2, block, false, true, false);
    lastL.fill (0.0f);
    lastR.fill (0.0f);
    lastMaster = 0.0f;
    limiterEnv = 0.0f;
    // About 100 ms release, like the web version's limiter.
    limiterRelease = (float) std::exp (-1.0 / (0.1 * jmax (1.0, dev->getCurrentSampleRate())));
}

double AudioEngine::outputLatencyMs() const
{
    auto* dev = deviceManager.getCurrentAudioDevice();
    if (dev == nullptr || dev->getCurrentSampleRate() <= 0.0) return 0.0;
    const int samples = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples()
                      + 2 * dev->getCurrentBufferSizeSamples();
    return 1000.0 * samples / dev->getCurrentSampleRate();
}

float AudioEngine::takeStripPeak (int channel)
{
    if (! isPositiveAndBelow (channel, maxChannels)) return 0.0f;
    return stripPeaks[(size_t) channel].exchange (0.0f);
}

float AudioEngine::takeMasterPeak (int side)
{
    return masterPeaks[(size_t) jlimit (0, 1, side)].exchange (0.0f);
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

    mixLive (inputs, numIns, outputs, numOuts, numSamples);
}

static void storeMax (std::atomic<float>& slot, float p)
{
    if (p > slot.load (std::memory_order_relaxed))
        slot.store (p, std::memory_order_relaxed);
}

void AudioEngine::mixLive (const float* const* inputs, int numIns, float* const* outputs, int numOuts, int numSamples)
{
    for (int i = 0; i < numOuts; ++i)
        if (outputs[i] != nullptr)
            FloatVectorOperations::clear (outputs[i], numSamples);
    if (numSamples > work.getNumSamples())
        return; // a bigger block than the device promised; skip rather than allocate here

    bool anySolo = false;
    for (int i = 0; i < numIns; ++i)
        anySolo = anySolo || strips[(size_t) activeToChannel[(size_t) i]].solo.load (std::memory_order_relaxed);

    bus.clear (0, numSamples);
    auto* w = work.getWritePointer (0);
    for (int i = 0; i < numIns; ++i)
    {
        if (inputs[i] == nullptr) continue;
        const auto ch = (size_t) activeToChannel[(size_t) i];
        auto& st = strips[ch];
        const bool audible = ! st.mute.load (std::memory_order_relaxed) && (! anySolo || st.solo.load (std::memory_order_relaxed));
        const float gain = audible ? Decibels::decibelsToGain (st.trimDb.load (std::memory_order_relaxed))
                                       * faderGain (st.faderDb.load (std::memory_order_relaxed))
                                   : 0.0f;
        // Constant-power pan, the same law as the web's StereoPannerNode for a mono source.
        const float x = (jlimit (-1.0f, 1.0f, st.pan.load (std::memory_order_relaxed)) + 1.0f) * 0.5f;
        const float gl = gain * std::cos (x * MathConstants<float>::halfPi);
        const float gr = gain * std::sin (x * MathConstants<float>::halfPi);

        // Post-fader meter (before pan), like the web strip meter.
        const float post = gain * FloatVectorOperations::findMaximum (inputs[i], numSamples) ;
        const float postNeg = -gain * FloatVectorOperations::findMinimum (inputs[i], numSamples);
        storeMax (stripPeaks[ch], jmax (post, postNeg));

        if (gl > 0.0f || gr > 0.0f || lastL[ch] > 0.0f || lastR[ch] > 0.0f)
        {
            FloatVectorOperations::copy (w, inputs[i], numSamples);
            bus.addFromWithRamp (0, 0, w, numSamples, lastL[ch], gl);
            bus.addFromWithRamp (1, 0, w, numSamples, lastR[ch], gr);
        }
        lastL[ch] = gl;
        lastR[ch] = gr;
    }

    // Master fader, then the limiter (instant attack, stereo-linked, ceiling -1 dB).
    const float master = faderGain (masterDb.load (std::memory_order_relaxed));
    bus.applyGainRamp (0, numSamples, lastMaster, master);
    lastMaster = master;
    if (limiterOn.load (std::memory_order_relaxed))
    {
        constexpr float ceiling = 0.891f; // -1 dBFS
        auto* l = bus.getWritePointer (0);
        auto* r = bus.getWritePointer (1);
        for (int n = 0; n < numSamples; ++n)
        {
            const float peak = jmax (std::abs (l[n]), std::abs (r[n]));
            limiterEnv = jmax (peak, limiterEnv * limiterRelease);
            if (limiterEnv > ceiling)
            {
                const float g = ceiling / limiterEnv;
                l[n] *= g;
                r[n] *= g;
            }
        }
    }
    storeMax (masterPeaks[0], bus.getMagnitude (0, 0, numSamples));
    storeMax (masterPeaks[1], bus.getMagnitude (1, 0, numSamples));

    if (! liveOn.load (std::memory_order_relaxed))
        return;
    int side = 0;
    for (int i = 0; i < numOuts && side < 2; ++i)
        if (outputs[i] != nullptr)
            FloatVectorOperations::copy (outputs[i], bus.getReadPointer (side++), numSamples);
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
