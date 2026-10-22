#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>

// A PTPv2 (IEEE 1588-2008) follower for AES67 sending. It listens to the network's
// grandmaster (picked by the best master rules from its Announce messages), measures
// the path delay with Delay_Req, and fits the network time against the Mac's own clock.
// It never acts as a master. Timestamps are taken in software, which is close enough
// for AES67 (receivers hold back a millisecond or more).
namespace aes67
{
    class PtpClock
    {
    public:
        PtpClock();
        ~PtpClock();

        // Starts following PTP on the network port with this IPv4 address.
        void start (const juce::String& ifaceIp, int domain);
        void stop();
        int getDomain() const { return domain.load(); }

        // PTP time (TAI) in nanoseconds. Returns false when not locked; the time is then
        // the Mac's own clock (as TAI), which keeps steady so sending can carry on.
        bool now (juce::int64& ns) const;

        struct Status
        {
            bool running = false;        // the PTP ports are open
            bool locked = false;         // following a grandmaster
            bool masterSeen = false;     // a grandmaster is announcing
            juce::String grandmaster;    // "00-1D-C1-FF-FE-12-34-56"
            juce::String masterAddress;  // IP the Sync messages come from
            int domain = 0;
            double jitterUs = 0.0;       // spread of the recent offsets around the fit
            double delayUs = 0.0;        // network path delay
            bool delayMeasured = false;
            double rateErrorPpm = 0.0;   // the Mac's clock against the grandmaster
            juce::int64 syncs = 0;
            juce::String error;          // why it couldn't start
        };
        Status status() const;

        // Steady local clock in nanoseconds (what the fit maps from).
        static juce::int64 localNs();

        // For tests: feed one measurement pair without the network.
        void addSyncForTest (juce::int64 local, juce::int64 master) { addSync (local, master); }

        // Parses a PTP message's header fields used here; for tests too.
        struct Header
        {
            int type = -1, version = 0, domain = 0, length = 0;
            bool twoStep = false;
            juce::int64 correctionNs = 0;
            juce::uint8 source[10] {}; // clock identity + port number
            int sequence = 0;
        };
        static bool parseHeader (const juce::uint8* b, int size, Header& h);
        static juce::int64 readTimestamp (const juce::uint8* p); // 10 bytes: 48-bit seconds, 32-bit ns
        static juce::String formatIdentity (const juce::uint8* id8);

    private:
        struct Impl;
        void run();
        void handle (const juce::uint8* b, int size, juce::int64 rxLocal, bool event, juce::uint32 fromAddr);
        void addSync (juce::int64 local, juce::int64 master);
        void sendDelayReq();

        std::unique_ptr<Impl> impl;
        std::atomic<int> domain { 0 };
        juce::int64 freeRunOffset = 0; // local -> TAI when not locked
    };
}
