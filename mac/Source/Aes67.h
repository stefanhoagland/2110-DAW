#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <memory>
#include <vector>

// AES67 / SMPTE ST 2110-30 network audio input, built into the app (no driver needed).
// Streams are found automatically from SAP announcements or added from an SDP file.
// Each subscribed stream's channels become extra inputs after the audio interface's.
// The receiver follows each stream's clock by gently resampling against the interface,
// so PTP isn't needed to receive (streams aren't sample-aligned with each other).
namespace aes67
{
    struct StreamInfo
    {
        juce::String name, sessionId, origin; // origin: the sender's IP from o=
        juce::String address;                 // multicast (or unicast) destination
        juce::String sourceFilter;            // a=source-filter sender address, if given
        int port = 5004, payloadType = 96, channels = 0, bits = 24;
        double rate = 48000.0, ptimeMs = 1.0;
        juce::String sdp;                     // the original text

        juce::String key() const { return origin + "/" + sessionId + "/" + address + ":" + juce::String (port); }
        juce::String format() const;          // "8 ch · L24 · 48 kHz · 1 ms"
    };

    // Reads an SDP description. Returns false (with a reason) if it isn't a stream we can
    // receive: we take uncompressed L16/L24 audio over RTP.
    bool parseSdp (const juce::String& text, StreamInfo& out, juce::String& why);

    // Reads a SAP packet. Sets `deleted` for a goodbye. False if it isn't SAP with SDP.
    bool parseSap (const void* data, int size, juce::String& sdp, bool& deleted);

    struct Interface { juce::String name, ip; };
    std::vector<Interface> localInterfaces(); // IPv4 interfaces, loopback last

    class Receiver;

    //==============================================================================
    class NetworkAudio : private juce::AsyncUpdater
    {
    public:
        static constexpr int maxNetChannels = 128;

        NetworkAudio();
        ~NetworkAudio() override;

        // The network port (by its IPv4 address) used to join streams and hear SAP.
        void setInterface (const juce::String& ip);
        juce::String getInterface() const { return iface; }
        void setLatencyMs (double ms);
        double getLatencyMs() const { return latencyMs.load(); }

        std::vector<StreamInfo> discovered() const; // from SAP, newest first
        // Starts receiving a stream. Returns an error, or empty on success.
        juce::String subscribe (const StreamInfo&);
        void unsubscribe (const juce::String& key);
        bool isSubscribed (const juce::String& key) const;

        struct Status
        {
            StreamInfo info;
            int firstChannel = 0;           // its first channel among the network inputs
            bool receiving = false;         // packets in the last second
            juce::int64 packets = 0, lost = 0;
            int dropouts = 0;               // times the buffer ran dry
            double bufferMs = 0.0;          // buffer in use (raised automatically after dropouts)
            double maxGapMs = 0.0;          // longest wait between packets lately
            double lowestMs = 0.0;          // least audio left in the buffer lately
            double driftPpm = 0.0;          // sender's clock against the audio interface
        };
        std::vector<Status> status() const;

        int numChannels() const { return channelCount.load(); }
        juce::String channelName (int ch) const;

        // Audio thread: fills `dest[0..numChannels)` with numSamples at the device rate.
        void pull (float* const* dest, int numDest, int numSamples, double deviceRate);

        // A plain-text summary of every stream's health, for troubleshooting.
        juce::String report() const;
        void resetStats(); // starts the "lately" figures over

        juce::String saveState() const;
        void loadState (const juce::String& json);

        std::function<void()> onChange; // message thread: streams or channel count changed

    private:
        void handleAsyncUpdate() override;
        void startSap();
        void sapLoop();
        void rebuildChannels();

        juce::String iface;
        std::atomic<double> latencyMs { 5.0 };

        using List = std::vector<std::shared_ptr<Receiver>>;
        List receivers;                          // message thread copy
        List live;                               // audio thread reads under liveLock
        juce::SpinLock liveLock;
        std::atomic<int> channelCount { 0 };
        juce::StringArray names;                 // per network channel, message thread

        mutable juce::CriticalSection sapLock;
        struct Seen { StreamInfo info; juce::uint32 lastMs; };
        std::vector<Seen> seen;
        std::unique_ptr<juce::Thread> sapThread;
        std::atomic<int> sapSocket { -1 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudio)
    };
}
