#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include "Ptp.h"
#include <atomic>
#include <memory>
#include <vector>

// AES67 sending: streams the app creates, each carrying some of the network outputs
// (patched like any output in Patch > Outputs). Audio is L24 at 48 kHz, timed by the
// network's PTP clock (mediaclk:direct=0), so Dante (in AES67 mode) and other AES67 or
// ST 2110-30 gear can take it. Each stream is announced with SAP and has an SDP to copy.
namespace aes67
{
    class Sender;

    struct SendConfig
    {
        juce::String name;
        juce::String address;   // multicast group (or a unicast receiver)
        int port = 5004;
        int channels = 2;
        int ptimeUs = 1000;     // 1000, 250 or 125
        juce::uint32 sessionId = 0;
        int firstChannel = 0;   // among the network outputs, set by addStream

        static int maxChannelsFor (int ptimeUs) { return ptimeUs >= 1000 ? 8 : ptimeUs >= 250 ? 32 : 64; }
        int framesPerPacket() const { return 48 * ptimeUs / 1000; }
        juce::String id() const { return juce::String ((juce::int64) sessionId); }
        juce::String format() const; // "8 ch · L24 · 48 kHz · 1 ms"
    };

    // The SDP text for a stream sent from `ifaceIp`, referenced to this PTP grandmaster
    // (empty: not locked).
    juce::String makeSdp (const SendConfig&, const juce::String& ifaceIp, juce::uint32 version,
                          const juce::String& grandmaster, int domain);
    // A SAP packet carrying the SDP (or saying goodbye).
    juce::MemoryBlock makeSap (const juce::String& sdp, const juce::String& ifaceIp, juce::uint16 hash, bool deletion);

    class NetworkSend : private juce::Timer, private juce::AsyncUpdater
    {
    public:
        static constexpr int maxChannels = 128; // network outputs

        NetworkSend();
        ~NetworkSend() override;

        void setInterface (const juce::String& ip);
        juce::String getInterface() const { return iface; }
        void setPtpDomain (int domain);
        int getPtpDomain() const { return ptpDomain; }
        PtpClock::Status ptpStatus() const { return ptp.status(); }
        PtpClock& clock() { return ptp; }

        // A free multicast address for a new stream, like 239.69.10.1.
        juce::String suggestAddress (const juce::StringArray& alsoTaken = {}) const;
        // Starts a stream. Fills in the session id and first channel. Returns an error, or empty.
        juce::String addStream (SendConfig&);
        void removeStream (const juce::String& id);
        std::vector<SendConfig> streams() const;

        struct Status
        {
            SendConfig config;
            juce::String sdp;
            bool sending = false;        // packets going out
            bool audio = false;          // the app's audio is feeding it
            juce::int64 packets = 0;
            int underruns = 0;           // times audio ran out (silence was sent)
            int resyncs = 0;             // times the PTP time jumped and the stream restarted
            juce::String error;
        };
        std::vector<Status> status() const;
        juce::String sdpFor (const juce::String& id) const;

        // Network outputs in use, in order, and their names ("Stage feed 2").
        std::vector<int> usedChannels() const;
        int usedChannelCount() const { return channelEnd.load (std::memory_order_relaxed); } // highest in use + 1 (audio thread)
        juce::String channelName (int ch) const;

        // Audio thread: `src[c]` is network output c (nullptr: silence) for this block.
        void push (const float* const* src, int numSrc, int numSamples, double deviceRate);

        juce::String saveState() const;
        void loadState (const juce::String& json);

        std::function<void()> onChange; // message thread: streams changed

    private:
        void timerCallback() override;    // SAP announcements
        void handleAsyncUpdate() override;
        void announce (Sender&, bool deletion);
        void rebuildLive();
        int findFreeChannels (int count) const;

        juce::String iface;
        int ptpDomain = 0;
        PtpClock ptp;
        int sapFd = -1;
        juce::uint32 sapVersion = 1;

        using List = std::vector<std::shared_ptr<Sender>>;
        List senders;               // message thread
        List live;                  // audio thread, under liveLock
        juce::SpinLock liveLock;
        std::atomic<int> channelEnd { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkSend)
    };
}
