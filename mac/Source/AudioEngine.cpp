#include "AudioEngine.h"
#include "Names.h"

using namespace juce;

static const String sep = String (CharPointer_UTF8 ("  \xc2\xb7  "));

class AudioEngine::IdleClock : public Thread
{
public:
    explicit IdleClock (AudioEngine& e) : Thread ("Soundbending Live idle clock"), engine (e) {}
    ~IdleClock() override { stopThread (2000); }

    static constexpr double rate = 48000.0;
    static constexpr int block = 480; // 10 ms

    void run() override
    {
        bool prepared = false;
        double next = Time::getMillisecondCounterHiRes();
        while (! threadShouldExit())
        {
            if (engine.deviceRunning.load())
            {
                prepared = false;
                wait (50);
                next = Time::getMillisecondCounterHiRes();
                continue;
            }
            engine.runIdleBlock (! prepared);
            prepared = true;
            next += 1000.0 * block / rate;
            const double now = Time::getMillisecondCounterHiRes();
            if (next < now - 100.0) next = now; // fell well behind (machine asleep): don't catch up
            if (next > now) wait (jmax (1, (int) (next - now)));
        }
    }

private:
    AudioEngine& engine;
};

AudioEngine::AudioEngine()
{
    resetPatches();
    writerThread.startThread (Thread::Priority::high);
    deviceManager.addAudioCallback (this);
    idleClock = std::make_unique<IdleClock> (*this);
    idleClock->startThread (Thread::Priority::high);
}

AudioEngine::~AudioEngine()
{
    idleClock.reset();
    mtcInput.reset();
    stopRecording();
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
    writerThread.stopThread (5000);
}

void AudioEngine::initialise (const XmlElement* savedState)
{
    // Two outputs feed the live mix (pick them in Audio settings).
    auto err = deviceManager.initialise (maxChannels, maxChannels, savedState, true);
    if (err.isNotEmpty())
        deviceManager.initialiseWithDefaultDevices (maxChannels, maxChannels);

    // First launch: the system default input is usually the built-in mic, so
    // pick the connected interface with the most inputs instead.
    if (savedState == nullptr)
        selectLargestInput();

    // Make sure every input of the device is on (a saved setup may have fewer).
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        const int n = dev->getInputChannelNames().size();
        const int outs = dev->getOutputChannelNames().size();
        // Every input and output is on, so the patch grid can reach all of them.
        if (setup.inputChannels.countNumberOfSetBits() < jmin (n, maxChannels)
            || setup.outputChannels.countNumberOfSetBits() < jmin (outs, maxChannels))
        {
            setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
            setup.inputChannels.clear();
            setup.inputChannels.setRange (0, jmin (n, maxChannels), true);
            setup.outputChannels.clear();
            setup.outputChannels.setRange (0, jmin (outs, maxChannels), true);
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

String AudioEngine::inputName (int i) const
{
    const int devIns = inputCount.load();
    if (i >= devIns) return net.channelName (i - devIns);
    auto* dev = deviceManager.getCurrentAudioDevice();
    const auto n = dev != nullptr ? dev->getInputChannelNames()[i] : String();
    return n.isNotEmpty() ? n : "In " + String (i + 1);
}

float AudioEngine::takePeak (int channel)
{
    if (! isPositiveAndBelow (channel, maxChannels))
        return 0.0f;
    return peaks[(size_t) channel].exchange (0.0f);
}

void AudioEngine::audioDeviceAboutToStart (AudioIODevice* dev)
{
    deviceRunning = true;
    const ScopedLock sl (blockLock); // let a block on the idle clock finish first
    auto active = dev->getActiveInputChannels();
    channelToActive.fill (-1);
    int k = 0;
    for (int ch = 0; ch < maxChannels && k < maxChannels; ++ch)
        if (active[ch])
        {
            channelToActive[(size_t) ch] = k;
            activeToChannel[(size_t) k++] = ch;
        }
    auto activeOut = dev->getActiveOutputChannels();
    outChannelToActive.fill (-1);
    k = 0;
    for (int ch = 0; ch < maxChannels; ++ch)
        if (activeOut[ch])
            outChannelToActive[(size_t) ch] = k++;
    inputCount = jmin (maxChannels, dev->getInputChannelNames().size());
    outputCount = jmin (maxChannels, dev->getOutputChannelNames().size());
    currentRate = dev->getCurrentSampleRate();
    ltcReset = true;

    prepareBuffers (dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples());
}

void AudioEngine::prepareBuffers (double rate, int deviceBlock)
{
    const int block = jmax (deviceBlock, 4096);
    netBuf.setSize (aes67::NetworkAudio::maxNetChannels, block, false, true, false);
    netOutBuf.setSize (aes67::NetworkSend::maxChannels, block, false, true, false);
    work.setSize (1, block, false, true, false);
    fx.setSize (8, block, false, true, false);
    midi.ensureSize (2048);
    lastTrim.fill (1.0f);
    for (int t = 0; t < maxChannels; ++t)
    {
        const SpinLock::ScopedLockType lock (insertLocks[(size_t) t]);
        for (auto& ins : inserts[(size_t) t]) prepare (*ins);
    }
    bus.setSize (2 * numMains, block, false, true, false);
    auxBus.setSize (numAux, block, false, true, false);
    for (auto& g : lastMainGain) g.fill (0.0f);
    for (auto& g : lastSendGain) g.fill (0.0f);
    lastMainMaster.fill (0.0f);
    lastAuxMaster.fill (0.0f);
    limiterEnv = 0.0f;
    // About 100 ms release, like the web version's limiter.
    limiterRelease = (float) std::exp (-1.0 / (0.1 * jmax (1.0, rate)));
}

void AudioEngine::runIdleBlock (bool prepareFirst)
{
    const ScopedLock sl (blockLock);
    if (deviceRunning.load())
        return;
    if (prepareFirst)
    {
        // No device inputs or outputs. The input count stays as it was, so network
        // channels keep their track numbers.
        channelToActive.fill (-1);
        outChannelToActive.fill (-1);
        currentRate = IdleClock::rate;
        ltcReset = true;
        prepareBuffers (IdleClock::rate, IdleClock::block);
    }
    audioDeviceIOCallbackWithContext (nullptr, 0, nullptr, 0, IdleClock::block, {});
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

float AudioEngine::takeMainPeak (int m, int side)
{
    return mainPeaks[(size_t) (jlimit (0, numMains - 1, m) * 2 + jlimit (0, 1, side))].exchange (0.0f);
}

void AudioEngine::setBusLabel (bool main, int index, const String& label)
{
    if (main) mainLabels[(size_t) jlimit (0, numMains - 1, index)] = label.trim();
    else auxLabels[(size_t) jlimit (0, numAux - 1, index)] = label.trim();
}

String AudioEngine::busLabel (bool main, int index) const
{
    return main ? mainLabels[(size_t) jlimit (0, numMains - 1, index)] : auxLabels[(size_t) jlimit (0, numAux - 1, index)];
}

String AudioEngine::busName (bool main, int index) const
{
    const auto label = busLabel (main, index);
    return label.isNotEmpty() ? label : (main ? "Main " : "Aux ") + String (index + 1);
}

String AudioEngine::busFullName (bool main, int index) const
{
    const auto label = busLabel (main, index);
    return (main ? "Main " : "Aux ") + String (index + 1) + (label.isNotEmpty() ? ": " + label : String());
}

float AudioEngine::takeAuxPeak (int a)
{
    return auxPeaks[(size_t) jlimit (0, numAux - 1, a)].exchange (0.0f);
}

AudioEngine::Strip::Strip()
{
    for (auto& d : sendDb) d = -60.0f;
    for (auto& p : sendPre) p = true;
    for (size_t m = 0; m < toMain.size(); ++m) toMain[m] = m == 0;
}

void AudioEngine::audioDeviceStopped()
{
    currentRate = 0.0;
    deviceRunning = false; // the idle clock takes over
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numIns,
                                                    float* const* outputs, int numOuts, int numSamples,
                                                    const AudioIODeviceCallbackContext&)
{
    // Timecode: note where this block sits in time, and read LTC if that's the source.
    const double rate = currentRate.load (std::memory_order_relaxed);
    Anchor tcNow;
    {
        const SpinLock::ScopedTryLockType lock (tcLock);
        if (lock.isLocked())
        {
            clockSample = samplePos;
            clockMs = Time::getMillisecondCounterHiRes();
            tcNow = anchor;
        }
    }
    if (tcMode.load (std::memory_order_relaxed) == 1)
    {
        if (ltcReset.exchange (false)) ltc.reset (rate);
        const int src = ltcInput.load (std::memory_order_relaxed);
        const int idx = isPositiveAndBelow (src, maxChannels) ? channelToActive[(size_t) src] : -1;
        if (isPositiveAndBelow (idx, numIns) && inputs[idx] != nullptr
            && ltc.process (inputs[idx], numSamples, samplePos))
        {
            const SpinLock::ScopedTryLockType lock (tcLock);
            if (lock.isLocked())
                anchor = { (double) ltc.lastFrame + 1.0, ltc.lastFrameEnd, ltc.rate, true };
        }
    }

    // AES67 network channels come after the interface's inputs.
    const int devIns = jmin (inputCount.load (std::memory_order_relaxed), maxChannels);
    int netCh = jmin (net.numChannels(), maxChannels - devIns, netBuf.getNumChannels());
    if (netCh > 0 && numSamples <= netBuf.getNumSamples())
        net.pull (netBuf.getArrayOfWritePointers(), netCh, numSamples, rate);
    else
        netCh = 0;

    // Patch: find each track's input.
    const int tracks = numTracks;
    for (int t = 0; t < tracks; ++t)
    {
        const int src = trackSource[(size_t) t].load (std::memory_order_relaxed);
        if (src >= devIns && src < devIns + netCh)
        {
            trackIn[(size_t) t] = netBuf.getReadPointer (src - devIns);
            continue;
        }
        const int idx = isPositiveAndBelow (src, maxChannels) ? channelToActive[(size_t) src] : -1;
        trackIn[(size_t) t] = isPositiveAndBelow (idx, numIns) ? inputs[idx] : nullptr;
    }

    for (int t = 0; t < tracks; ++t)
    {
        auto* in = trackIn[(size_t) t];
        if (in == nullptr) continue;
        auto range = FloatVectorOperations::findMinAndMax (in, numSamples);
        const float p = jmax (-range.getStart(), range.getEnd());
        auto& slot = peaks[(size_t) t];
        if (p > slot.load (std::memory_order_relaxed))
            slot.store (p, std::memory_order_relaxed);
    }

    if (recording.load())
    {
        const SpinLock::ScopedTryLockType lock (writersLock);
        if (lock.isLocked())
        {
            bool lost = false;
            for (int t = 0; t < tracks; ++t)
            {
                if ((size_t) t >= writers.size() || writers[(size_t) t] == nullptr) continue;
                const float* in = trackIn[(size_t) t];
                if (in == nullptr && numSamples <= work.getNumSamples())
                {
                    // Unpatched track: record silence so every file stays the same length.
                    auto* w = work.getWritePointer (0);
                    FloatVectorOperations::clear (w, jmin (numSamples, work.getNumSamples()));
                    in = w;
                }
                if (in == nullptr || ! writers[(size_t) t]->write (&in, numSamples))
                    lost = true;
            }
            if (lost) ++dropped;
            if (samplesRecorded.load (std::memory_order_relaxed) == 0 && tcNow.valid && rate > 0.0
                && std::abs ((double) (samplePos - tcNow.sample)) < 0.5 * rate)
            {
                // The take's first sample, in timecode.
                takeTcFrames = tcNow.frames + (double) (samplePos - tcNow.sample) / rate * tcNow.rate.fps;
                takeTcRate = tcNow.rate;
                takeTcValid.store (true, std::memory_order_release);
            }
            samplesRecorded += numSamples;
        }
    }

    mixLive (tracks, outputs, numOuts, numSamples);
    samplePos += numSamples;
}

static void storeMax (std::atomic<float>& slot, float p)
{
    if (p > slot.load (std::memory_order_relaxed))
        slot.store (p, std::memory_order_relaxed);
}

void AudioEngine::mixLive (int tracks, float* const* outputs, int numOuts, int numSamples)
{
    for (int i = 0; i < numOuts; ++i)
        if (outputs[i] != nullptr)
            FloatVectorOperations::clear (outputs[i], numSamples);
    if (numSamples > work.getNumSamples())
        return; // a bigger block than the device promised; skip rather than allocate here

    bool anySolo = false;
    for (int t = 0; t < tracks; ++t)
        anySolo = anySolo || strips[(size_t) t].solo.load (std::memory_order_relaxed);

    bus.clear (0, numSamples);
    auxBus.clear (0, numSamples);
    auto* w = work.getWritePointer (0);
    for (int t = 0; t < tracks; ++t)
    {
        const float* in = trackIn[(size_t) t];
        const auto ch = (size_t) t;
        if (in == nullptr)
        {
            // Nothing patched: let any ramps from last block finish at silence next time.
            lastMainGain[ch].fill (0.0f);
            lastSendGain[ch].fill (0.0f);
            continue;
        }
        auto& st = strips[ch];
        const bool muted = st.mute.load (std::memory_order_relaxed);
        const bool audible = ! muted && (! anySolo || st.solo.load (std::memory_order_relaxed));

        // Trim, then the plugin inserts.
        const float trim = Decibels::decibelsToGain (st.trimDb.load (std::memory_order_relaxed));
        FloatVectorOperations::copy (w, in, numSamples);
        work.applyGainRamp (0, 0, numSamples, lastTrim[ch], trim);
        lastTrim[ch] = trim;
        runInserts (t, w, numSamples);

        // Pre-fader meter (after gain and plugins), like the web strip meter.
        storeMax (stripPeaks[ch], work.getMagnitude (0, 0, numSamples));

        // Mute, solo and the fader set what goes to the mains.
        const float fader = faderGain (st.faderDb.load (std::memory_order_relaxed));
        const float gain = audible ? fader : 0.0f;
        // Constant-power pan, the same law as the web's StereoPannerNode for a mono source.
        const float x = (jlimit (-1.0f, 1.0f, st.pan.load (std::memory_order_relaxed)) + 1.0f) * 0.5f;
        const float gl = gain * std::cos (x * MathConstants<float>::halfPi);
        const float gr = gain * std::sin (x * MathConstants<float>::halfPi);
        auto& lastM = lastMainGain[ch];
        for (int m = 0; m < numMains; ++m)
        {
            const bool on = st.toMain[(size_t) m].load (std::memory_order_relaxed);
            for (int side = 0; side < 2; ++side)
            {
                const float target = on ? (side == 0 ? gl : gr) : 0.0f;
                auto& last = lastM[(size_t) (m * 2 + side)];
                if (target > 0.0f || last > 0.0f)
                    bus.addFromWithRamp (m * 2 + side, 0, w, numSamples, last, target);
                last = target;
            }
        }

        // Aux sends: mute cuts them, solo doesn't (so monitor mixes don't change when
        // someone solos at front of house). Pre-fader sends ignore the fader.
        auto& lastS = lastSendGain[ch];
        for (int a = 0; a < numAux; ++a)
        {
            const float send = muted ? 0.0f : faderGain (st.sendDb[(size_t) a].load (std::memory_order_relaxed))
                                                  * (st.sendPre[(size_t) a].load (std::memory_order_relaxed) ? 1.0f : fader);
            auto& last = lastS[(size_t) a];
            if (send > 0.0f || last > 0.0f)
                auxBus.addFromWithRamp (a, 0, w, numSamples, last, send);
            last = send;
        }
    }

    // Bus masters. Main 1 then goes through the limiter (instant attack, stereo-linked, ceiling -1 dB).
    for (int m = 0; m < numMains; ++m)
    {
        auto& b = mainBuses[(size_t) m];
        const float g = b.mute.load (std::memory_order_relaxed) ? 0.0f : faderGain (b.faderDb.load (std::memory_order_relaxed));
        for (int side = 0; side < 2; ++side)
            bus.applyGainRamp (m * 2 + side, 0, numSamples, lastMainMaster[(size_t) m], g);
        lastMainMaster[(size_t) m] = g;
    }
    for (int a = 0; a < numAux; ++a)
    {
        auto& b = auxBuses[(size_t) a];
        const float g = b.mute.load (std::memory_order_relaxed) ? 0.0f : faderGain (b.faderDb.load (std::memory_order_relaxed));
        auxBus.applyGainRamp (a, 0, numSamples, lastAuxMaster[(size_t) a], g);
        lastAuxMaster[(size_t) a] = g;
        storeMax (auxPeaks[(size_t) a], auxBus.getMagnitude (a, 0, numSamples));
    }
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
    else
    {
        limiterEnv = 0.0f; // start fresh when it's switched back on
    }
    for (int c = 0; c < 2 * numMains; ++c)
    {
        const float p = bus.getMagnitude (c, 0, numSamples);
        storeMax (mainPeaks[(size_t) c], p);
        if (c < 2) storeMax (masterPeaks[(size_t) c], p);
    }

    // Output patch. The mix goes out unless muted (only when the app closes); direct track signals always do.
    const bool live = liveOn.load (std::memory_order_relaxed);
    const int outs = jmin (outputCount.load (std::memory_order_relaxed), maxChannels);
    for (int o = 0; o < outs; ++o)
    {
        const int idx = outChannelToActive[(size_t) o];
        if (isPositiveAndBelow (idx, numOuts) && outputs[idx] != nullptr)
            mixOutput (outputs[idx], o, tracks, numSamples, live);
    }

    // AES67 sending channels.
    if (numSamples <= netOutBuf.getNumSamples())
    {
        const auto used = netSend.usedChannelCount();
        for (int c = 0; c < used; ++c)
        {
            auto* dest = netOutBuf.getWritePointer (c);
            FloatVectorOperations::clear (dest, numSamples);
            mixOutput (dest, firstNetOut + c, tracks, numSamples, live);
        }
        if (used > 0) netSend.push (netOutBuf.getArrayOfReadPointers(), used, numSamples, currentRate.load (std::memory_order_relaxed));
    }
}

void AudioEngine::mixOutput (float* dest, int o, int tracks, int numSamples, bool live)
{
    if (live)
    {
        for (int m = 0; m < numMains; ++m)
            for (int side = 0; side < 2; ++side)
                if (patch[(size_t) (mainSource (m, side) * numOutSlots + o)].load (std::memory_order_relaxed))
                    FloatVectorOperations::add (dest, bus.getReadPointer (m * 2 + side), numSamples);
        for (int a = 0; a < numAux; ++a)
            if (patch[(size_t) ((firstAux + a) * numOutSlots + o)].load (std::memory_order_relaxed))
                FloatVectorOperations::add (dest, auxBus.getReadPointer (a), numSamples);
    }
    for (int t = 0; t < tracks; ++t)
        if (trackIn[(size_t) t] != nullptr
            && patch[(size_t) ((firstDirect + t) * numOutSlots + o)].load (std::memory_order_relaxed))
            FloatVectorOperations::add (dest, trackIn[(size_t) t], numSamples);
}

void AudioEngine::setTrackSource (int track, int deviceInput)
{
    if (isPositiveAndBelow (track, maxChannels))
        trackSource[(size_t) track] = isPositiveAndBelow (deviceInput, maxChannels) ? deviceInput : -1;
}

int AudioEngine::getTrackSource (int track) const
{
    return isPositiveAndBelow (track, maxChannels) ? trackSource[(size_t) track].load() : -1;
}

void AudioEngine::setPatch (int source, int output, bool on)
{
    if (isPositiveAndBelow (source, numSources) && isPositiveAndBelow (output, numOutSlots))
        patch[(size_t) (source * numOutSlots + output)] = on;
}

bool AudioEngine::getPatch (int source, int output) const
{
    return isPositiveAndBelow (source, numSources) && isPositiveAndBelow (output, numOutSlots)
        && patch[(size_t) (source * numOutSlots + output)].load();
}

void AudioEngine::clearOutput (int output)
{
    for (int s = 0; s < numSources; ++s) setPatch (s, output, false);
}

void AudioEngine::resetPatches()
{
    for (int t = 0; t < maxChannels; ++t) trackSource[(size_t) t] = t;
    for (auto& p : patch) p = false;
    setPatch (mixLeft, 0, true);
    setPatch (mixRight, 1, true);
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

    // With timecode locked, files are Broadcast WAVs stamped with the start time. The value
    // here is an estimate; stopRecording() writes the exact time of the first sample.
    std::unordered_map<String, String> bext;
    const auto now = timecodeNow();
    if (now.locked)
    {
        const auto t = Time::getCurrentTime();
        bext[WavAudioFormat::bwavDescription] = "sTC=" + tc::toLabel (now.frames, now.rate) + " fps=" + now.rate.shortName();
        bext[WavAudioFormat::bwavOriginator] = "Soundbending Live";
        bext[WavAudioFormat::bwavOriginationDate] = t.formatted ("%Y-%m-%d");
        bext[WavAudioFormat::bwavOriginationTime] = t.formatted ("%H:%M:%S");
        bext[WavAudioFormat::bwavTimeReference] = String (tc::toSamples (now.frames, now.rate, rate));
    }

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

        auto opts = AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24).withMetadataValues (bext);
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
    takeTcValid = false;
    takeSampleRate = rate;
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
    const bool hadWriters = ! old.empty();
    old.clear(); // each ThreadedWriter flushes its buffer and closes its file here
    if (hadWriters && takeTcValid.load (std::memory_order_acquire))
        stampTake();
}

void AudioEngine::stampTake()
{
    const auto label = tc::toLabel (takeTcFrames, takeTcRate);
    const auto ref = tc::toSamples (takeTcFrames, takeTcRate, takeSampleRate);
    for (auto& path : takeFiles)
        stampBext (File (path), ref, "sTC=" + label + " fps=" + takeTcRate.shortName());
}

bool AudioEngine::stampBext (const File& wav, int64 timeReference, const String& description)
{
    // Find the bext chunk, then overwrite its Description (256 bytes) and TimeReference
    // (64-bit little-endian at byte 338).
    int64 bextData = -1;
    {
        FileInputStream in (wav);
        if (! in.openedOk() || in.readInt() != (int) ByteOrder::littleEndianInt ("RIFF")) return false;
        in.readInt();
        if (in.readInt() != (int) ByteOrder::littleEndianInt ("WAVE")) return false;
        while (! in.isExhausted())
        {
            const int id = in.readInt();
            const auto size = (int64) (uint32) in.readInt();
            if (id == (int) ByteOrder::littleEndianInt ("bext")) { bextData = in.getPosition(); break; }
            in.setPosition (in.getPosition() + size + (size & 1));
        }
    }
    if (bextData < 0) return false;
    FileOutputStream out (wav); // opens without truncating
    if (! out.openedOk()) return false;
    char text[256] = {};
    description.copyToUTF8 (text, sizeof (text) - 1);
    out.setPosition (bextData);
    out.write (text, sizeof (text));
    out.setPosition (bextData + 338);
    out.writeInt64 (timeReference); // little-endian
    out.flush();
    return ! out.getStatus().failed();
}

void AudioEngine::setTimecodeOff()
{
    tcMode = 0;
    mtcInput.reset();
    const SpinLock::ScopedLockType lock (tcLock);
    anchor.valid = false;
}

void AudioEngine::setTimecodeLtc (int deviceInput)
{
    mtcInput.reset();
    ltcInput = deviceInput;
    ltcReset = true;
    {
        const SpinLock::ScopedLockType lock (tcLock);
        anchor.valid = false;
    }
    tcMode = 1;
}

bool AudioEngine::setTimecodeMtc (const String& id)
{
    setTimecodeOff();
    mtcInput = MidiInput::openDevice (id, this);
    if (mtcInput == nullptr) return false;
    tcMode = 2;
    mtcInput->start();
    return true;
}

void AudioEngine::handleIncomingMidiMessage (MidiInput*, const MidiMessage& m)
{
    if (tcMode.load() != 2) return;
    bool done = false;
    if (m.isQuarterFrame())
        done = mtc.quarterFrame (m.getQuarterFrameSequenceNumber(), m.getQuarterFrameValue());
    else if (m.isFullFrame())
    {
        int h = 0, mi = 0, s = 0, f = 0;
        MidiMessage::SmpteTimecodeType type;
        m.getFullFrameParameters (h, mi, s, f, type);
        done = mtc.fullFrame (h, mi, s, f, (int) type);
    }
    if (! done) return;
    const double rate = currentRate.load();
    const SpinLock::ScopedLockType lock (tcLock);
    // MIDI timestamps are on the same high-resolution clock as clockMs.
    const auto at = clockSample + (int64) std::llround ((m.getTimeStamp() * 1000.0 - clockMs) * rate / 1000.0);
    anchor = { (double) mtc.frames, at, mtc.rate, true };
}

AudioEngine::TimecodeNow AudioEngine::timecodeNow() const
{
    TimecodeNow t;
    t.active = tcMode.load() != 0;
    Anchor a;
    int64 cs; double cm;
    {
        const SpinLock::ScopedLockType lock (tcLock);
        a = anchor; cs = clockSample; cm = clockMs;
    }
    const double rate = currentRate.load();
    if (! t.active || ! a.valid || rate <= 0.0) return t;
    const double now = (double) cs + (Time::getMillisecondCounterHiRes() - cm) * rate / 1000.0;
    const double dt = (now - (double) a.sample) / rate;
    t.seen = true;
    t.locked = dt > -0.5 && dt < 0.5;
    t.frames = a.frames + dt * a.rate.fps;
    t.rate = a.rate;
    return t;
}

AudioEngine::TimecodeNow AudioEngine::takeTimecode() const
{
    TimecodeNow t;
    if (takeTcValid.load (std::memory_order_acquire))
    {
        t.active = t.seen = t.locked = true;
        t.frames = takeTcFrames;
        t.rate = takeTcRate;
    }
    return t;
}

double AudioEngine::recordedSeconds() const
{
    const double rate = sampleRate();
    return rate > 0.0 ? (double) samplesRecorded.load() / rate : 0.0;
}

//==============================================================================
void AudioEngine::prepare (Insert& ins) const
{
    auto& p = *ins.plugin;
    const double rate = currentRate.load() > 0.0 ? currentRate.load() : 48000.0;
    const int block = jmax (work.getNumSamples(), 512);
    // Mono if the plugin can do it, otherwise stereo.
    ins.channels = 2;
    if (p.setBusesLayout ({ { AudioChannelSet::mono() }, { AudioChannelSet::mono() } }))
        ins.channels = 1;
    else
        p.setBusesLayout ({ { AudioChannelSet::stereo() }, { AudioChannelSet::stereo() } });
    p.setRateAndBufferSizeDetails (rate, block);
    p.setNonRealtime (false);
    p.prepareToPlay (rate, block);
}

void AudioEngine::runInserts (int track, float* samples, int numSamples)
{
    const SpinLock::ScopedTryLockType lock (insertLocks[(size_t) track]);
    if (! lock.isLocked()) return; // the chain is being changed this instant; pass the audio through dry
    for (auto& ins : inserts[(size_t) track])
    {
        if (ins->bypass.load (std::memory_order_relaxed)) continue;
        auto& p = *ins->plugin;
        const int chans = jmax (ins->channels, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
        if (chans > fx.getNumChannels() || numSamples > fx.getNumSamples()) continue;
        for (int c = 0; c < chans; ++c)
            FloatVectorOperations::copy (fx.getWritePointer (c), samples, numSamples);
        AudioBuffer<float> view (fx.getArrayOfWritePointers(), chans, numSamples);
        midi.clear();
        p.processBlock (view, midi);
        if (ins->channels == 1 || p.getTotalNumOutputChannels() < 2)
            FloatVectorOperations::copy (samples, fx.getReadPointer (0), numSamples);
        else
        {
            FloatVectorOperations::copyWithMultiply (samples, fx.getReadPointer (0), 0.5f, numSamples);
            FloatVectorOperations::addWithMultiply (samples, fx.getReadPointer (1), 0.5f, numSamples);
        }
    }
}

bool AudioEngine::addInsert (int track, std::unique_ptr<AudioPluginInstance> plugin, const PluginDescription& d, bool bypass)
{
    if (! isPositiveAndBelow (track, maxChannels) || plugin == nullptr) return false;
    auto next = getInserts (track);
    if ((int) next.size() >= maxInserts) return false;
    auto ins = std::make_shared<Insert>();
    ins->plugin = std::move (plugin);
    ins->description = d;
    ins->bypass = bypass;
    prepare (*ins);
    next.push_back (ins);
    {
        const SpinLock::ScopedLockType lock (insertLocks[(size_t) track]);
        inserts[(size_t) track].swap (next);
    }
    return true; // `next` (the old list) is released here, outside the lock
}

void AudioEngine::removeInsert (int track, int index)
{
    if (! isPositiveAndBelow (track, maxChannels)) return;
    auto next = getInserts (track);
    if (! isPositiveAndBelow (index, (int) next.size())) return;
    next.erase (next.begin() + index);
    {
        const SpinLock::ScopedLockType lock (insertLocks[(size_t) track]);
        inserts[(size_t) track].swap (next);
    }
    // The removed plugin is deleted when `next` goes out of scope, off the audio thread.
}

void AudioEngine::moveInsert (int track, int from, int to)
{
    if (! isPositiveAndBelow (track, maxChannels)) return;
    auto next = getInserts (track);
    if (! isPositiveAndBelow (from, (int) next.size()) || ! isPositiveAndBelow (to, (int) next.size())) return;
    auto item = next[(size_t) from];
    next.erase (next.begin() + from);
    next.insert (next.begin() + to, item);
    const SpinLock::ScopedLockType lock (insertLocks[(size_t) track]);
    inserts[(size_t) track].swap (next);
}

AudioEngine::InsertList AudioEngine::getInserts (int track) const
{
    if (! isPositiveAndBelow (track, maxChannels)) return {};
    const SpinLock::ScopedLockType lock (insertLocks[(size_t) track]);
    return inserts[(size_t) track];
}
