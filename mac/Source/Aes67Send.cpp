#include "Aes67Send.h"
#include "Aes67.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <thread>

using namespace juce;

namespace aes67
{
static const String dot = String (CharPointer_UTF8 (" \xc2\xb7 "));
static constexpr int64 second = 1000000000LL;
static constexpr int rtpRate = 48000, payloadType = 97;

String SendConfig::format() const
{
    const auto packet = ptimeUs >= 1000 ? String (ptimeUs / 1000) + " ms" : String (ptimeUs) + String (CharPointer_UTF8 (" \xc2\xb5s"));
    return String (channels) + " ch" + dot + "L24" + dot + "48 kHz" + dot + packet;
}

static bool isMulticast (const String& address)
{
    in_addr a {};
    return inet_pton (AF_INET, address.toRawUTF8(), &a) == 1 && IN_MULTICAST (ntohl (a.s_addr));
}

String makeSdp (const SendConfig& c, const String& ifaceIp, uint32 version, const String& gm, int domain)
{
    const String ptime = c.ptimeUs >= 1000 ? String (c.ptimeUs / 1000) : String (c.ptimeUs / 1000.0, 3);
    const bool multicast = isMulticast (c.address);
    const String conn = "c=IN IP4 " + c.address + (multicast ? "/32" : "");
    StringArray l;
    l.add ("v=0");
    l.add ("o=- " + c.id() + " " + String ((int64) version) + " IN IP4 " + ifaceIp);
    l.add ("s=" + (c.name.isNotEmpty() ? c.name : String ("Soundbending Live")));
    l.add (conn);
    l.add ("t=0 0");
    l.add ("a=clock-domain:PTPv2 " + String (domain));
    l.add ("m=audio " + String (c.port) + " RTP/AVP " + String (payloadType));
    l.add (conn);
    l.add ("a=rtpmap:" + String (payloadType) + " L24/48000/" + String (c.channels));
    l.add ("a=ptime:" + ptime);
    l.add ("a=maxptime:" + ptime);
    l.add (gm.isNotEmpty() ? "a=ts-refclk:ptp=IEEE1588-2008:" + gm + ":" + String (domain)
                           : String ("a=ts-refclk:ptp=IEEE1588-2008:traceable"));
    l.add ("a=mediaclk:direct=0");
    if (multicast) l.add ("a=source-filter: incl IN IP4 " + c.address + " " + ifaceIp);
    l.add ("a=recvonly");
    return l.joinIntoString ("\r\n") + "\r\n";
}

MemoryBlock makeSap (const String& sdp, const String& ifaceIp, uint16 hash, bool deletion)
{
    MemoryBlock m;
    uint8 head[8] { (uint8) (deletion ? 0x24 : 0x20), 0, (uint8) (hash >> 8), (uint8) hash, 0, 0, 0, 0 };
    in_addr a {};
    inet_pton (AF_INET, ifaceIp.toRawUTF8(), &a);
    std::memcpy (head + 4, &a.s_addr, 4);
    m.append (head, 8);
    const char type[] = "application/sdp";
    m.append (type, sizeof (type)); // with its zero byte
    m.append (sdp.toRawUTF8(), sdp.getNumBytesAsUTF8());
    return m;
}

// A UDP socket for sending from the interface with address `ifaceIp`.
static int openSendSocket (const String& ifaceIp, String& error)
{
    const int fd = socket (AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { error = "Can't create a network socket."; return -1; }
    in_addr ifa {};
    if (ifaceIp.isEmpty() || inet_pton (AF_INET, ifaceIp.toRawUTF8(), &ifa) != 1) ifa.s_addr = htonl (INADDR_ANY);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr = ifa;
    if (bind (fd, reinterpret_cast<sockaddr*> (&a), sizeof (a)) != 0)
    {
        close (fd);
        error = "Can't send from " + ifaceIp + ". Is that network port still connected?";
        return -1;
    }
    setsockopt (fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof (ifa));
    unsigned char ttl = 32, loop = 1;
    setsockopt (fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof (ttl));
    setsockopt (fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof (loop));
    int tos = 34 << 2; // DSCP AF41, the AES67 default for audio
    setsockopt (fd, IPPROTO_IP, IP_TOS, &tos, sizeof (tos));
    int buf = 1 << 20;
    setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof (buf));
    return fd;
}

static int64 samplesAt (int64 ns)
{
    return (ns / second) * rtpRate + ((ns % second) * rtpRate) / second;
}

//==============================================================================
class Sender : public Thread
{
public:
    Sender (const SendConfig& c, const String& ifaceIp, PtpClock& clock)
        : Thread ("AES67 send " + c.name), config (c), iface (ifaceIp), ptp (clock)
    {
        const int frames = 48000; // a second of buffer at up to 48 kHz device rate
        fifo.setTotalSize (frames);
        ring.resize ((size_t) (frames * config.channels));
        tmp.setSize (config.channels, 2048);
        out.setSize (config.channels, config.framesPerPacket());
        interp.resize ((size_t) config.channels);
        ssrc = (uint32) Random::getSystemRandom().nextInt();
        seq = (uint16) Random::getSystemRandom().nextInt (65536);
    }

    ~Sender() override
    {
        signalThreadShouldExit();
        stopThread (2000);
        if (fd >= 0) close (fd);
    }

    String open()
    {
        String error;
        fd = openSendSocket (iface, error);
        if (fd < 0) return error;
        dest.sin_family = AF_INET;
        dest.sin_port = htons ((uint16) config.port);
        if (inet_pton (AF_INET, config.address.toRawUTF8(), &dest.sin_addr) != 1) return "Bad address " + config.address;
        startThread (Thread::Priority::highest);
        return {};
    }

    // Audio thread: this stream's channels for one block.
    void write (const float* const* src, int numSrc, int n, double deviceRate)
    {
        rate.store (deviceRate, std::memory_order_relaxed);
        if (n > maxBlock.load (std::memory_order_relaxed)) maxBlock.store (n, std::memory_order_relaxed);
        lastWriteMs.store (Time::getMillisecondCounter(), std::memory_order_relaxed);
        int s1, z1, s2, z2;
        fifo.prepareToWrite (n, s1, z1, s2, z2);
        if (z1 + z2 < n) { fifo.finishedWrite (0); return; } // the sender isn't reading; drop
        const int ch = config.channels;
        auto copy = [&] (int start, int count, int offset)
        {
            for (int c = 0; c < ch; ++c)
            {
                const float* s = c < numSrc ? src[c] : nullptr;
                for (int k = 0; k < count; ++k)
                    ring[(size_t) ((start + k) * ch + c)] = s != nullptr ? s[offset + k] : 0.0f;
            }
        };
        copy (s1, z1, 0);
        copy (s2, z2, z1);
        fifo.finishedWrite (z1 + z2);
    }

    void run() override
    {
        const int frames = config.framesPerPacket(), ch = config.channels;
        std::vector<uint8> pkt ((size_t) (12 + frames * ch * 3));
        int64 nextTs = -1;
        while (! threadShouldExit())
        {
            int64 ns = 0;
            ptp.now (ns);
            const int64 now = samplesAt (ns);
            // Start, or start over when PTP time jumped (lock found, master changed).
            if (nextTs < 0 || now - nextTs > rtpRate / 20 || nextTs - now > rtpRate / 20)
            {
                if (nextTs >= 0) ++resyncs;
                nextTs = (now / frames) * frames - frames;
            }
            int burst = 0;
            while (now >= nextTs + frames && burst++ < 64)
            {
                produce (frames);
                buildPacket (pkt.data(), (uint32) nextTs, frames);
                sendto (fd, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*> (&dest), sizeof (dest));
                ++packets;
                lastSendMs = Time::getMillisecondCounter();
                nextTs += frames;
            }
            // Sleep until the next packet is due.
            const int64 dueNs = ((nextTs + frames) / rtpRate) * second + ((nextTs + frames) % rtpRate) * second / rtpRate;
            const int64 waitUs = jlimit ((int64) 50, (int64) 1000, (dueNs - ns) / 1000);
            std::this_thread::sleep_for (std::chrono::microseconds (waitUs));
        }
    }

    SendConfig config;
    String iface, sdp;
    uint32 sdpVersion = 0;
    uint32 lastAnnounceMs = 0;
    std::atomic<int64> packets { 0 };
    std::atomic<int> underruns { 0 }, resyncs { 0 };
    std::atomic<uint32> lastSendMs { 0 }, lastWriteMs { 0 };
    std::atomic<bool> flowing { false };

private:
    // The next `frames` samples at 48 kHz from the app's audio, resampled from the
    // device's clock to the PTP clock by keeping the buffer at a steady fill.
    void produce (int frames)
    {
        const int ch = config.channels;
        out.clear();
        const double r = rate.load (std::memory_order_relaxed);
        const bool fresh = Time::getMillisecondCounter() - lastWriteMs.load (std::memory_order_relaxed) < 200;
        if (r <= 0.0 || ! fresh) { flowing = false; return; }
        const double target = maxBlock.load (std::memory_order_relaxed) + 0.002 * r + frames * r / rtpRate;
        int ready = fifo.getNumReady();
        if (! flowing)
        {
            if (ready < target) return; // silence while it fills
            flowing = true;
            fill = ready;
            for (auto& i : interp) i.reset();
        }
        if (ready > target + 0.1 * r) // far too much (after a stall): catch up
        {
            fifo.finishedRead (ready - (int) target);
            ready = fifo.getNumReady();
            fill = ready;
        }
        fill += jmin (1.0, frames / (0.2 * rtpRate)) * (ready - fill);
        const double ratio = r / rtpRate * (1.0 + jlimit (-0.002, 0.002, (fill - target) / (r * 0.5)));
        const int need = (int) std::ceil (frames * ratio) + 2;
        if (need > tmp.getNumSamples()) return;
        if (ready < need) { ++underruns; flowing = false; return; }
        int s1, z1, s2, z2;
        fifo.prepareToRead (need, s1, z1, s2, z2);
        for (int c = 0; c < ch; ++c)
        {
            auto* t = tmp.getWritePointer (c);
            for (int k = 0; k < z1; ++k) t[k] = ring[(size_t) ((s1 + k) * ch + c)];
            for (int k = 0; k < z2; ++k) t[z1 + k] = ring[(size_t) ((s2 + k) * ch + c)];
        }
        int used = 0;
        for (int c = 0; c < ch; ++c)
            used = interp[(size_t) c].process (ratio, tmp.getReadPointer (c), out.getWritePointer (c), frames);
        fifo.finishedRead (jmin (used, need));
    }

    void buildPacket (uint8* p, uint32 ts, int frames)
    {
        p[0] = 0x80;
        p[1] = payloadType;
        p[2] = (uint8) (seq >> 8); p[3] = (uint8) seq;
        ++seq;
        p[4] = (uint8) (ts >> 24); p[5] = (uint8) (ts >> 16); p[6] = (uint8) (ts >> 8); p[7] = (uint8) ts;
        p[8] = (uint8) (ssrc >> 24); p[9] = (uint8) (ssrc >> 16); p[10] = (uint8) (ssrc >> 8); p[11] = (uint8) ssrc;
        auto* d = p + 12;
        const int ch = config.channels;
        for (int k = 0; k < frames; ++k)
            for (int c = 0; c < ch; ++c)
            {
                const float x = jlimit (-1.0f, 1.0f, out.getSample (c, k));
                const int32 v = (int32) std::lrint (x * 8388607.0f);
                *d++ = (uint8) (v >> 16); *d++ = (uint8) (v >> 8); *d++ = (uint8) v;
            }
    }

    PtpClock& ptp;
    int fd = -1;
    sockaddr_in dest {};
    uint32 ssrc = 0;
    uint16 seq = 0;
    AbstractFifo fifo { 1 };
    std::vector<float> ring;
    AudioBuffer<float> tmp, out;
    std::vector<LagrangeInterpolator> interp;
    std::atomic<double> rate { 0.0 };
    std::atomic<int> maxBlock { 0 };
    double fill = 0.0;
};

//==============================================================================
NetworkSend::NetworkSend()
{
    auto ifs = localInterfaces();
    iface = ifs.empty() ? String() : ifs.front().ip;
    String error;
    sapFd = openSendSocket (iface, error);
    ptp.start (iface, ptpDomain);
    startTimer (1000);
}

NetworkSend::~NetworkSend()
{
    stopTimer();
    cancelPendingUpdate();
    for (auto& s : senders) announce (*s, true);
    {
        const SpinLock::ScopedLockType lock (liveLock);
        live.clear();
    }
    senders.clear();
    if (sapFd >= 0) close (sapFd);
}

void NetworkSend::setInterface (const String& ip)
{
    if (ip == iface) return;
    for (auto& s : senders) announce (*s, true);
    iface = ip;
    if (sapFd >= 0) close (sapFd);
    String error;
    sapFd = openSendSocket (iface, error);
    ptp.start (iface, ptpDomain);
    auto configs = streams();
    for (auto& c : configs) removeStream (c.id());
    for (auto& c : configs) addStream (c);
    triggerAsyncUpdate();
}

void NetworkSend::setPtpDomain (int d)
{
    d = jlimit (0, 127, d);
    if (d == ptpDomain) return;
    ptpDomain = d;
    ptp.start (iface, ptpDomain);
    timerCallback(); // re-announce with the new domain
}

String NetworkSend::suggestAddress (const StringArray& alsoTaken) const
{
    StringArray taken (alsoTaken);
    for (auto& s : senders) taken.add (s->config.address);
    const int third = jmax (1, iface.fromLastOccurrenceOf (".", false, false).getIntValue());
    for (int n = 1; n < 255; ++n)
    {
        const String a = "239.69." + String (third) + "." + String (n);
        if (! taken.contains (a)) return a;
    }
    return "239.69.0.1";
}

int NetworkSend::findFreeChannels (int count) const
{
    for (int first = 0; first + count <= maxChannels; ++first)
    {
        bool clash = false;
        for (auto& s : senders)
            if (first < s->config.firstChannel + s->config.channels && s->config.firstChannel < first + count) { clash = true; break; }
        if (! clash) return first;
    }
    return -1;
}

String NetworkSend::addStream (SendConfig& c)
{
    c.name = c.name.trim();
    c.address = c.address.trim();
    if (c.name.isEmpty()) return "Give the stream a name.";
    in_addr a {};
    if (inet_pton (AF_INET, c.address.toRawUTF8(), &a) != 1) return "\"" + c.address + "\" isn't an IPv4 address like 239.69.1.1.";
    if (c.ptimeUs != 1000 && c.ptimeUs != 250 && c.ptimeUs != 125) c.ptimeUs = 1000;
    const int most = SendConfig::maxChannelsFor (c.ptimeUs);
    if (c.channels < 1 || c.channels > most)
        return String (c.channels) + " channels don't fit in a " + String (c.ptimeUs) + String (CharPointer_UTF8 (" \xc2\xb5s"))
               + " packet. Use up to " + String (most) + ", or a shorter packet time.";
    if (c.port < 1024 || c.port > 65535) return "Use a port from 1024 to 65535 (5004 is usual).";
    for (auto& s : senders)
        if (s->config.address == c.address && s->config.port == c.port) return "Another stream already sends to " + c.address + ".";

    bool keep = c.firstChannel >= 0 && c.firstChannel + c.channels <= maxChannels;
    for (auto& s : senders)
        if (keep && c.firstChannel < s->config.firstChannel + s->config.channels && s->config.firstChannel < c.firstChannel + c.channels)
            keep = false;
    if (! keep) c.firstChannel = findFreeChannels (c.channels);
    if (c.firstChannel < 0) return "That's more than " + String (maxChannels) + " sending channels.";
    while (c.sessionId == 0 || std::any_of (senders.begin(), senders.end(), [&] (auto& s) { return s->config.sessionId == c.sessionId; }))
        c.sessionId = (uint32) Random::getSystemRandom().nextInt (0x7fffffff);

    auto s = std::make_shared<Sender> (c, iface, ptp);
    auto err = s->open();
    if (err.isNotEmpty()) return err;
    senders.push_back (s);
    std::sort (senders.begin(), senders.end(), [] (auto& x, auto& y) { return x->config.firstChannel < y->config.firstChannel; });
    rebuildLive();
    const auto st = ptp.status();
    s->sdp = makeSdp (s->config, iface, ++s->sdpVersion, st.locked ? st.grandmaster : String(), ptpDomain);
    announce (*s, false);
    triggerAsyncUpdate();
    return {};
}

void NetworkSend::removeStream (const String& id)
{
    for (auto& s : senders)
        if (s->config.id() == id) announce (*s, true);
    senders.erase (std::remove_if (senders.begin(), senders.end(), [&] (auto& s) { return s->config.id() == id; }), senders.end());
    rebuildLive();
    triggerAsyncUpdate();
}

void NetworkSend::rebuildLive()
{
    List next = senders;
    int end = 0;
    for (auto& s : senders) end = jmax (end, s->config.firstChannel + s->config.channels);
    {
        const SpinLock::ScopedLockType lock (liveLock);
        live.swap (next);
        channelEnd = end;
    }
    // The old list is released here, off the audio thread.
}

std::vector<SendConfig> NetworkSend::streams() const
{
    std::vector<SendConfig> out;
    for (auto& s : senders) out.push_back (s->config);
    return out;
}

std::vector<NetworkSend::Status> NetworkSend::status() const
{
    std::vector<Status> out;
    const auto now = Time::getMillisecondCounter();
    for (auto& s : senders)
    {
        Status st;
        st.config = s->config;
        st.sdp = s->sdp;
        st.packets = s->packets.load();
        st.sending = st.packets > 0 && now - s->lastSendMs.load() < 1000;
        st.audio = s->flowing.load();
        st.underruns = s->underruns.load();
        st.resyncs = s->resyncs.load();
        out.push_back (st);
    }
    return out;
}

String NetworkSend::sdpFor (const String& id) const
{
    for (auto& s : senders) if (s->config.id() == id) return s->sdp;
    return {};
}

std::vector<int> NetworkSend::usedChannels() const
{
    std::vector<int> out;
    for (auto& s : senders)
        for (int c = 0; c < s->config.channels; ++c) out.push_back (s->config.firstChannel + c);
    return out;
}

String NetworkSend::channelName (int ch) const
{
    for (auto& s : senders)
        if (ch >= s->config.firstChannel && ch < s->config.firstChannel + s->config.channels)
            return s->config.name + " " + String (ch - s->config.firstChannel + 1);
    return {};
}

void NetworkSend::push (const float* const* src, int numSrc, int numSamples, double deviceRate)
{
    const SpinLock::ScopedTryLockType lock (liveLock);
    if (! lock.isLocked()) return; // the stream list is changing this instant
    for (auto& s : live)
    {
        const int first = s->config.firstChannel;
        s->write (src + jmin (first, numSrc), jmax (0, numSrc - first), numSamples, deviceRate);
    }
}

void NetworkSend::announce (Sender& s, bool deletion)
{
    if (sapFd < 0 || s.sdp.isEmpty()) return;
    const auto packet = makeSap (s.sdp, iface, (uint16) (s.config.sessionId ^ (s.config.sessionId >> 16)), deletion);
    sockaddr_in to {};
    to.sin_family = AF_INET;
    to.sin_port = htons (9875);
    inet_pton (AF_INET, "239.255.255.255", &to.sin_addr);
    sendto (sapFd, packet.getData(), packet.getSize(), 0, reinterpret_cast<sockaddr*> (&to), sizeof (to));
    s.lastAnnounceMs = Time::getMillisecondCounter();
}

void NetworkSend::timerCallback()
{
    // Re-announce every 30 s, and at once when the SDP changes (a new grandmaster).
    const auto st = ptp.status();
    const auto gm = st.locked ? st.grandmaster : String();
    const auto now = Time::getMillisecondCounter();
    for (auto& s : senders)
    {
        auto sdp = makeSdp (s->config, iface, s->sdpVersion, gm, ptpDomain);
        if (sdp != s->sdp)
        {
            s->sdp = makeSdp (s->config, iface, ++s->sdpVersion, gm, ptpDomain);
            announce (*s, false);
        }
        else if (now - s->lastAnnounceMs > 30000) announce (*s, false);
    }
}

String NetworkSend::saveState() const
{
    Array<var> list;
    for (auto& s : senders)
    {
        auto* o = new DynamicObject();
        o->setProperty ("name", s->config.name);
        o->setProperty ("address", s->config.address);
        o->setProperty ("port", s->config.port);
        o->setProperty ("channels", s->config.channels);
        o->setProperty ("ptimeUs", s->config.ptimeUs);
        o->setProperty ("sessionId", (int64) s->config.sessionId);
        o->setProperty ("firstChannel", s->config.firstChannel);
        list.add (var (o));
    }
    auto* root = new DynamicObject();
    root->setProperty ("interface", iface);
    root->setProperty ("ptpDomain", ptpDomain);
    root->setProperty ("streams", list);
    return JSON::toString (var (root), true);
}

void NetworkSend::loadState (const String& json)
{
    auto v = JSON::parse (json);
    if (! v.isObject()) return;
    for (auto& c : streams()) removeStream (c.id());
    const String saved = v.getProperty ("interface", String());
    for (auto& i : localInterfaces())
        if (i.ip == saved) { setInterface (saved); break; }
    setPtpDomain ((int) v.getProperty ("ptpDomain", 0));
    if (auto* list = v["streams"].getArray())
        for (auto& o : *list)
        {
            SendConfig c;
            c.name = o.getProperty ("name", String());
            c.address = o.getProperty ("address", String());
            c.port = o.getProperty ("port", 5004);
            c.channels = o.getProperty ("channels", 2);
            c.ptimeUs = o.getProperty ("ptimeUs", 1000);
            c.sessionId = (uint32) (int64) o.getProperty ("sessionId", 0);
            c.firstChannel = o.getProperty ("firstChannel", -1);
            addStream (c);
        }
    triggerAsyncUpdate();
}

void NetworkSend::handleAsyncUpdate()
{
    if (onChange) onChange();
}
}
