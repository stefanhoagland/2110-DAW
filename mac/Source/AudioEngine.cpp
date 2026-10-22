#include "AudioEngine.h"
#include "Names.h"

using namespace juce;

static const String sep = String (CharPointer_UTF8 ("  \xc2\xb7  "));

AudioEngine::AudioEngine()
{
    resetPatches();
    writerThread.startThread (Thread::Priority::high);
    deviceManager.addAudioCallback (this);
}

AudioEngine::~AudioEngine()
{
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

    const int block = jmax (dev->getCurrentBufferSizeSamples(), 4096);
    netBuf.setSize (aes67::NetworkAudio::maxNetChannels, block, false, true, false);
    work.setSize (1, block, false, true, false);
    fx.setSize (8, block, false, true, false);
    midi.ensureSize (2048);
    lastTrim.fill (1.0f);
    for (int t = 0; t < maxChannels; ++t)
    {
        const SpinLock::ScopedLockType lock (insertLocks[(size_t) t]);
        for (auto& ins : inserts[(size_t) t]) prepare (*ins);
    }
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
    const int tracks = devIns + netCh;
    for (int t = 0; t < tracks; ++t)
    {
        const int src = trackSource[(size_t) t].load (std::memory_order_relaxed);
        if (src >= devIns && src < tracks)
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
    auto* w = work.getWritePointer (0);
    for (int t = 0; t < tracks; ++t)
    {
        const float* in = trackIn[(size_t) t];
        if (in == nullptr) continue;
        const auto ch = (size_t) t;
        auto& st = strips[ch];
        const bool audible = ! st.mute.load (std::memory_order_relaxed) && (! anySolo || st.solo.load (std::memory_order_relaxed));

        // Trim, then the plugin inserts.
        const float trim = Decibels::decibelsToGain (st.trimDb.load (std::memory_order_relaxed));
        FloatVectorOperations::copy (w, in, numSamples);
        work.applyGainRamp (0, 0, numSamples, lastTrim[ch], trim);
        lastTrim[ch] = trim;
        runInserts (t, w, numSamples);

        // Mute, solo and the fader set what goes to the mix; the meter shows the level
        // before them, so you can see a channel is live even when it's pulled down.
        const float fader = faderGain (st.faderDb.load (std::memory_order_relaxed));
        const float gain = audible ? fader : 0.0f;
        // Constant-power pan, the same law as the web's StereoPannerNode for a mono source.
        const float x = (jlimit (-1.0f, 1.0f, st.pan.load (std::memory_order_relaxed)) + 1.0f) * 0.5f;
        const float gl = gain * std::cos (x * MathConstants<float>::halfPi);
        const float gr = gain * std::sin (x * MathConstants<float>::halfPi);

        // Pre-fader meter (after gain and plugins), like the web strip meter.
        storeMax (stripPeaks[ch], work.getMagnitude (0, 0, numSamples));

        if (gl > 0.0f || gr > 0.0f || lastL[ch] > 0.0f || lastR[ch] > 0.0f)
        {
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
    else
    {
        limiterEnv = 0.0f; // start fresh when it's switched back on
    }
    storeMax (masterPeaks[0], bus.getMagnitude (0, 0, numSamples));
    storeMax (masterPeaks[1], bus.getMagnitude (1, 0, numSamples));

    // Output patch. The mix goes out unless muted (only when the app closes); direct track signals always do.
    const bool live = liveOn.load (std::memory_order_relaxed);
    const int outs = jmin (outputCount.load (std::memory_order_relaxed), maxChannels);
    for (int o = 0; o < outs; ++o)
    {
        const int idx = outChannelToActive[(size_t) o];
        if (! isPositiveAndBelow (idx, numOuts) || outputs[idx] == nullptr) continue;
        auto* dest = outputs[idx];
        if (live)
            for (int side = 0; side < 2; ++side)
                if (patch[(size_t) (side * maxChannels + o)].load (std::memory_order_relaxed))
                    FloatVectorOperations::add (dest, bus.getReadPointer (side), numSamples);
        for (int t = 0; t < tracks; ++t)
            if (trackIn[(size_t) t] != nullptr
                && patch[(size_t) ((firstDirect + t) * maxChannels + o)].load (std::memory_order_relaxed))
                FloatVectorOperations::add (dest, trackIn[(size_t) t], numSamples);
    }
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
    if (isPositiveAndBelow (source, numSources) && isPositiveAndBelow (output, maxChannels))
        patch[(size_t) (source * maxChannels + output)] = on;
}

bool AudioEngine::getPatch (int source, int output) const
{
    return isPositiveAndBelow (source, numSources) && isPositiveAndBelow (output, maxChannels)
        && patch[(size_t) (source * maxChannels + output)].load();
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
