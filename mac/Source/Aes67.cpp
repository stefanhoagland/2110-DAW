#include "Aes67.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace juce;

namespace aes67
{
static const String dot = String (CharPointer_UTF8 (" \xc2\xb7 "));

String StreamInfo::format() const
{
    const bool wholeMs = std::abs (ptimeMs - std::round (ptimeMs)) < 1.0e-9;
    const auto packet = ptimeMs >= 1.0 ? String (ptimeMs, wholeMs ? 0 : 2) + " ms"
                                       : String (ptimeMs * 1000.0, 0) + String (CharPointer_UTF8 (" \xc2\xb5s"));
    const auto khz = std::fmod (rate, 1000.0) < 1.0e-9 ? String ((int) (rate / 1000.0)) : String (rate / 1000.0, 1);
    return String (channels) + " ch" + dot + "L" + String (bits) + dot + khz + " kHz" + dot + packet;
}

//==============================================================================
bool parseSdp (const String& text, StreamInfo& s, String& why)
{
    s = {};
    s.sdp = text;
    String rtpmap, sessionConn, mediaConn;
    bool inMedia = false, haveAudio = false;
    for (auto line : StringArray::fromLines (text))
    {
        line = line.trim();
        if (line.length() < 2 || line[1] != '=') continue;
        const auto v = line.substring (2).trim();
        switch (line[0])
        {
            case 'o':
            {
                auto f = StringArray::fromTokens (v, " ", "");
                if (f.size() >= 6) { s.sessionId = f[1]; s.origin = f[5]; }
                break;
            }
            case 's': if (! inMedia) s.name = v; break;
            case 'c':
            {
                auto f = StringArray::fromTokens (v, " ", "");
                if (f.size() >= 3) (inMedia ? mediaConn : sessionConn) = f[2].upToFirstOccurrenceOf ("/", false, false);
                break;
            }
            case 'm':
            {
                if (haveAudio) { inMedia = false; break; } // only the first audio stream
                auto f = StringArray::fromTokens (v, " ", "");
                inMedia = f[0] == "audio";
                if (inMedia && f.size() >= 4)
                {
                    haveAudio = true;
                    s.port = f[1].getIntValue();
                    s.payloadType = f[3].getIntValue();
                }
                break;
            }
            case 'a':
                if (! inMedia && haveAudio) break;
                if (v.startsWith ("rtpmap:"))
                {
                    const auto pt = v.fromFirstOccurrenceOf (":", false, false).upToFirstOccurrenceOf (" ", false, false).getIntValue();
                    if (pt == s.payloadType || ! haveAudio) rtpmap = v.fromFirstOccurrenceOf (" ", false, false).trim();
                }
                else if (v.startsWith ("ptime:"))
                    s.ptimeMs = v.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
                else if (v.startsWith ("source-filter:"))
                {
                    auto f = StringArray::fromTokens (v.fromFirstOccurrenceOf (":", false, false), " ", "");
                    f.removeEmptyStrings();
                    if (f.size() >= 5) s.sourceFilter = f[f.size() - 1];
                }
                break;
            default: break;
        }
    }
    s.address = mediaConn.isNotEmpty() ? mediaConn : sessionConn;
    if (! haveAudio) { why = "No audio stream in the SDP."; return false; }
    if (s.address.isEmpty()) { why = "The SDP has no address (c= line)."; return false; }
    auto r = StringArray::fromTokens (rtpmap, "/", "");
    const auto enc = r[0].toUpperCase();
    if (enc != "L24" && enc != "L16")
    {
        why = enc.isEmpty() ? "The SDP doesn't say the audio format." : enc + " isn't supported; AES67 uses L24 or L16.";
        return false;
    }
    s.bits = enc == "L24" ? 24 : 16;
    s.rate = r[1].getDoubleValue();
    s.channels = r.size() > 2 ? r[2].getIntValue() : 1;
    if (s.rate < 8000.0 || s.channels < 1 || s.channels > 64) { why = "Unusual rate or channel count in the SDP."; return false; }
    if (s.ptimeMs <= 0.0) s.ptimeMs = 1.0;
    if (s.name.isEmpty() || s.name == "-") s.name = s.address;
    return true;
}

bool parseSap (const void* data, int size, String& sdp, bool& deleted)
{
    auto* p = static_cast<const uint8*> (data);
    if (size < 8 || (p[0] >> 5) != 1) return false;
    if (p[0] & 0x03) return false; // encrypted or compressed
    deleted = (p[0] & 0x04) != 0;
    int at = 4 + ((p[0] & 0x10) ? 16 : 4) + p[1] * 4;
    if (at >= size) return false;
    String rest (CharPointer_UTF8 ((const char*) p + at), CharPointer_UTF8 ((const char*) p + size));
    if (! rest.startsWith ("v=0"))
    {
        // An optional payload type ("application/sdp") ending in a zero byte.
        int z = at;
        while (z < size && p[z] != 0) ++z;
        if (String (CharPointer_UTF8 ((const char*) p + at), CharPointer_UTF8 ((const char*) p + z)) != "application/sdp") return false;
        at = z + 1;
        rest = String (CharPointer_UTF8 ((const char*) p + at), CharPointer_UTF8 ((const char*) p + size));
    }
    sdp = rest;
    return sdp.startsWith ("v=0");
}

std::vector<Interface> localInterfaces()
{
    std::vector<Interface> list, loop;
    ifaddrs* all = nullptr;
    if (getifaddrs (&all) != 0) return list;
    for (auto* a = all; a != nullptr; a = a->ifa_next)
    {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET || ! (a->ifa_flags & IFF_UP)) continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop (AF_INET, &reinterpret_cast<sockaddr_in*> (a->ifa_addr)->sin_addr, ip, sizeof (ip));
        ((a->ifa_flags & IFF_LOOPBACK) ? loop : list).push_back ({ a->ifa_name, ip });
    }
    freeifaddrs (all);
    list.insert (list.end(), loop.begin(), loop.end());
    return list;
}

// A UDP socket on `port`, joined to `group` on the interface with address `ifaceIp`.
static int openSocket (const String& group, int port, const String& ifaceIp, String& error)
{
    in_addr g {}, ifa {};
    if (inet_pton (AF_INET, group.toRawUTF8(), &g) != 1) { error = "Bad address " + group; return -1; }
    if (ifaceIp.isEmpty() || inet_pton (AF_INET, ifaceIp.toRawUTF8(), &ifa) != 1) ifa.s_addr = htonl (INADDR_ANY);
    const int fd = socket (AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { error = "Can't create a network socket."; return -1; }
    int one = 1, buf = 4 << 20;
    setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
#ifdef SO_REUSEPORT
    setsockopt (fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof (one));
#endif
    setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof (buf));
    const bool multicast = IN_MULTICAST (ntohl (g.s_addr));
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons ((uint16) port);
    // Binding to the group means this socket only gets that group's packets, even when
    // several streams share port 5004.
    a.sin_addr.s_addr = multicast ? g.s_addr : htonl (INADDR_ANY);
    if (bind (fd, reinterpret_cast<sockaddr*> (&a), sizeof (a)) != 0)
    {
        close (fd);
        error = "Port " + String (port) + " is busy.";
        return -1;
    }
    if (multicast)
    {
        ip_mreq m {};
        m.imr_multiaddr = g;
        m.imr_interface = ifa;
        if (setsockopt (fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof (m)) != 0)
        {
            close (fd);
            error = "Can't join " + group + " on that network port.";
            return -1;
        }
    }
    return fd;
}

//==============================================================================
class Receiver : public Thread
{
public:
    Receiver (const StreamInfo& i, const String& ifaceIp) : Thread ("AES67 " + i.name), info (i), iface (ifaceIp)
    {
        const int frames = (int) (info.rate * 0.5); // half a second of buffer
        fifo.setTotalSize (frames);
        ring.resize ((size_t) (frames * info.channels));
        tmp.setSize (info.channels, 16384);
        interp.resize ((size_t) info.channels);
        packet.resize ((size_t) (9000 / 2 + 64)); // a jumbo packet's worth of samples
        discard.resize (8192);
    }

    ~Receiver() override
    {
        signalThreadShouldExit();
        stopThread (2000);
        if (fd >= 0) close (fd);
    }

    String open()
    {
        String error;
        fd = openSocket (info.address, info.port, iface, error);
        if (fd < 0) return error;
        startThread (Thread::Priority::highest);
        return {};
    }

    void run() override
    {
        uint8 buf[9000];
        in_addr filter {};
        const bool useFilter = info.sourceFilter.isNotEmpty() && inet_pton (AF_INET, info.sourceFilter.toRawUTF8(), &filter) == 1;
        while (! threadShouldExit())
        {
            pollfd p { fd, POLLIN, 0 };
            if (poll (&p, 1, 200) <= 0) continue;
            sockaddr_in from {};
            socklen_t len = sizeof (from);
            const auto n = recvfrom (fd, buf, sizeof (buf), 0, reinterpret_cast<sockaddr*> (&from), &len);
            if (n <= 0) continue;
            if (useFilter && from.sin_addr.s_addr != filter.s_addr) continue;
            handlePacket (buf, (int) n);
        }
    }

    // Audio thread. Writes `numDest` channels of `n` samples at the device rate.
    void read (float* const* dest, int numDest, int n, double deviceRate, double latencyMs)
    {
        const int ch = jmin (numDest, info.channels);
        auto silence = [&] { for (int c = 0; c < ch; ++c) if (dest[c] != nullptr) FloatVectorOperations::clear (dest[c], n); };
        if (deviceRate <= 0.0 || n > (int) discard.size()) { silence(); return; }

        const double base = info.rate / deviceRate; // stream samples per device sample
        const double target = latencyMs * 0.001 * info.rate + n * base;
        const int avail = fifo.getNumReady();
        if (buffering)
        {
            if (avail < target) { silence(); return; }
            buffering = false;
            fill = avail;
            for (auto& i : interp) i.reset();
        }
        // Too far behind (e.g. after a hiccup): jump to the target rather than lag.
        if (avail > target + 0.1 * info.rate)
        {
            fifo.finishedRead (avail - (int) target);
            fill = target;
        }
        fill += 0.02 * (fifo.getNumReady() - fill);
        // Follow the sender's clock: read a touch faster when the buffer grows, slower
        // when it shrinks. 0.2% at most, far beyond real clock differences.
        const double ratio = base * (1.0 + jlimit (-0.002, 0.002, (fill - target) / (info.rate * 0.5)));
        const int need = (int) std::ceil (n * ratio) + 2;
        if (need > tmp.getNumSamples()) { silence(); return; }
        if (fifo.getNumReady() < need)
        {
            ++dropouts;
            buffering = true;
            silence();
            return;
        }
        int s1, z1, s2, z2;
        fifo.prepareToRead (need, s1, z1, s2, z2);
        for (int c = 0; c < info.channels; ++c)
        {
            auto* t = tmp.getWritePointer (c);
            for (int k = 0; k < z1; ++k) t[k] = ring[(size_t) ((s1 + k) * info.channels + c)];
            for (int k = 0; k < z2; ++k) t[z1 + k] = ring[(size_t) ((s2 + k) * info.channels + c)];
        }
        int used = 0;
        for (int c = 0; c < info.channels; ++c)
        {
            float* out = c < ch && dest[c] != nullptr ? dest[c] : discard.data();
            used = interp[(size_t) c].process (ratio, tmp.getReadPointer (c), out, n);
        }
        fifo.finishedRead (jmin (used, need));
    }

    StreamInfo info;
    String iface;
    std::atomic<int64> packets { 0 }, lost { 0 };
    std::atomic<int> dropouts { 0 };
    std::atomic<uint32> lastPacketMs { 0 };
    int firstChannel = 0;

private:
    void handlePacket (const uint8* b, int size)
    {
        if (size < 12 || (b[0] >> 6) != 2) return;
        if ((b[1] & 0x7f) != info.payloadType) return;
        int at = 12 + 4 * (b[0] & 15);
        if ((b[0] & 0x10) && at + 4 <= size) at += 4 + 4 * ((b[at + 2] << 8) | b[at + 3]);
        int end = size;
        if ((b[0] & 0x20) && size > 0) end -= b[size - 1];
        const int bytes = info.bits / 8, frameBytes = bytes * info.channels;
        if (end <= at) return;
        const int frames = (end - at) / frameBytes;
        if (frames <= 0) return;

        const uint16 seq = (uint16) ((b[2] << 8) | b[3]);
        if (haveSeq)
        {
            const uint16 gap = (uint16) (seq - lastSeq);
            if (gap == 0 || gap > 32768) return; // duplicate or late
            if (gap > 1)
            {
                lost += gap - 1;
                if (gap - 1 <= 20) push (nullptr, (gap - 1) * frames); // keep time with silence
            }
        }
        haveSeq = true;
        lastSeq = seq;

        const int count = frames * info.channels;
        if ((int) packet.size() < count) return;
        auto* p = b + at;
        for (int k = 0; k < count; ++k, p += bytes)
        {
            const int32 v = bytes == 3 ? (int32) (((uint32) p[0] << 24) | ((uint32) p[1] << 16) | ((uint32) p[2] << 8))
                                       : (int32) (((uint32) p[0] << 24) | ((uint32) p[1] << 16));
            packet[(size_t) k] = (float) v * (1.0f / 2147483648.0f);
        }
        push (packet.data(), frames);
        ++packets;
        lastPacketMs = Time::getMillisecondCounter();
    }

    void push (const float* interleaved, int frames)
    {
        int s1, z1, s2, z2;
        fifo.prepareToWrite (frames, s1, z1, s2, z2);
        if (z1 + z2 < frames) { fifo.finishedWrite (0); return; } // reader stalled; drop
        const int ch = info.channels;
        auto copy = [&] (int start, int count, int offset)
        {
            if (interleaved != nullptr)
                std::copy (interleaved + offset * ch, interleaved + (offset + count) * ch, ring.begin() + start * ch);
            else
                std::fill (ring.begin() + start * ch, ring.begin() + (start + count) * ch, 0.0f);
        };
        copy (s1, z1, 0);
        copy (s2, z2, z1);
        fifo.finishedWrite (z1 + z2);
    }

    int fd = -1;
    AbstractFifo fifo { 1 };
    std::vector<float> ring, packet, discard;
    AudioBuffer<float> tmp;
    std::vector<LagrangeInterpolator> interp;
    bool buffering = true, haveSeq = false;
    uint16 lastSeq = 0;
    double fill = 0.0;
};

//==============================================================================
struct SapThread : Thread
{
    explicit SapThread (NetworkAudio& n) : Thread ("AES67 discovery"), owner (n) {}
    void run() override { loop(); }
    std::function<void()> loop;
    NetworkAudio& owner;
};

NetworkAudio::NetworkAudio()
{
    auto ifs = localInterfaces();
    iface = ifs.empty() ? String() : ifs.front().ip;
    startSap();
}

NetworkAudio::~NetworkAudio()
{
    cancelPendingUpdate();
    if (sapThread != nullptr) sapThread->stopThread (2000);
    if (sapSocket >= 0) close (sapSocket);
    {
        const SpinLock::ScopedLockType lock (liveLock);
        live.clear();
    }
    receivers.clear();
}

void NetworkAudio::startSap()
{
    if (sapThread != nullptr) sapThread->stopThread (2000);
    sapThread.reset();
    if (sapSocket >= 0) { close (sapSocket); sapSocket = -1; }
    String error;
    sapSocket = openSocket ("239.255.255.255", 9875, iface, error); // SAP for AES67
    if (sapSocket < 0) return;
    auto t = std::make_unique<SapThread> (*this);
    t->loop = [this] { sapLoop(); };
    sapThread = std::move (t);
    sapThread->startThread();
}

void NetworkAudio::sapLoop()
{
    char buf[4096];
    uint32 lastSweep = Time::getMillisecondCounter();
    while (! sapThread->threadShouldExit())
    {
        pollfd p { sapSocket.load(), POLLIN, 0 };
        if (poll (&p, 1, 250) > 0)
        {
            const auto n = recv (sapSocket, buf, sizeof (buf), 0);
            String sdp, why;
            bool deleted = false;
            StreamInfo info;
            if (n > 0 && parseSap (buf, (int) n, sdp, deleted) && parseSdp (sdp, info, why))
            {
                bool changed = false;
                {
                    const ScopedLock lock (sapLock);
                    auto it = std::find_if (seen.begin(), seen.end(), [&] (auto& s) { return s.info.key() == info.key(); });
                    if (deleted)
                    {
                        if (it != seen.end()) { seen.erase (it); changed = true; }
                    }
                    else if (it == seen.end()) { seen.push_back ({ info, Time::getMillisecondCounter() }); changed = true; }
                    else { changed = it->info.sdp != info.sdp; it->info = info; it->lastMs = Time::getMillisecondCounter(); }
                }
                if (changed) triggerAsyncUpdate();
            }
        }
        // Forget streams that stopped announcing (SAP repeats every 30 s or so).
        const auto now = Time::getMillisecondCounter();
        if (now - lastSweep > 5000)
        {
            lastSweep = now;
            const ScopedLock lock (sapLock);
            const auto before = seen.size();
            seen.erase (std::remove_if (seen.begin(), seen.end(), [now] (auto& s) { return now - s.lastMs > 300000; }), seen.end());
            if (seen.size() != before) triggerAsyncUpdate();
        }
    }
}

void NetworkAudio::setInterface (const String& ip)
{
    if (ip == iface) return;
    iface = ip;
    {
        const ScopedLock lock (sapLock);
        seen.clear();
    }
    startSap();
    // Rejoin every stream on the new port.
    std::vector<StreamInfo> infos;
    for (auto& r : receivers) infos.push_back (r->info);
    for (auto& i : infos) unsubscribe (i.key());
    for (auto& i : infos) subscribe (i);
    triggerAsyncUpdate();
}

void NetworkAudio::setLatencyMs (double ms) { latencyMs = jlimit (1.0, 100.0, ms); }

std::vector<StreamInfo> NetworkAudio::discovered() const
{
    const ScopedLock lock (sapLock);
    auto copy = seen;
    std::sort (copy.begin(), copy.end(), [] (auto& a, auto& b) { return a.info.name < b.info.name; });
    std::vector<StreamInfo> out;
    for (auto& s : copy) out.push_back (s.info);
    return out;
}

bool NetworkAudio::isSubscribed (const String& key) const
{
    for (auto& r : receivers) if (r->info.key() == key) return true;
    return false;
}

String NetworkAudio::subscribe (const StreamInfo& info)
{
    if (isSubscribed (info.key())) return {};
    if (channelCount.load() + info.channels > maxNetChannels)
        return "That would be more than " + String (maxNetChannels) + " network channels.";
    auto rx = std::make_shared<Receiver> (info, iface);
    auto err = rx->open();
    if (err.isNotEmpty()) return err;
    receivers.push_back (rx);
    rebuildChannels();
    return {};
}

void NetworkAudio::unsubscribe (const String& key)
{
    receivers.erase (std::remove_if (receivers.begin(), receivers.end(), [&] (auto& r) { return r->info.key() == key; }), receivers.end());
    rebuildChannels();
}

void NetworkAudio::rebuildChannels()
{
    int ch = 0;
    names.clear();
    for (auto& r : receivers)
    {
        r->firstChannel = ch;
        for (int c = 0; c < r->info.channels; ++c) names.add (r->info.name + " " + String (c + 1));
        ch += r->info.channels;
    }
    List next = receivers;
    {
        const SpinLock::ScopedLockType lock (liveLock);
        live.swap (next);
        channelCount = ch;
    }
    // `next` (the old list) is released here, so a removed receiver stops off the audio thread.
    triggerAsyncUpdate();
}

String NetworkAudio::channelName (int ch) const { return names[ch]; }

std::vector<NetworkAudio::Status> NetworkAudio::status() const
{
    std::vector<Status> out;
    const auto now = Time::getMillisecondCounter();
    for (auto& r : receivers)
    {
        Status s;
        s.info = r->info;
        s.firstChannel = r->firstChannel;
        s.packets = r->packets.load();
        s.lost = r->lost.load();
        s.dropouts = r->dropouts.load();
        s.receiving = s.packets > 0 && now - r->lastPacketMs.load() < 1000;
        out.push_back (s);
    }
    return out;
}

void NetworkAudio::pull (float* const* dest, int numDest, int numSamples, double deviceRate)
{
    const SpinLock::ScopedTryLockType lock (liveLock);
    if (! lock.isLocked())
    {
        for (int c = 0; c < numDest; ++c) if (dest[c] != nullptr) FloatVectorOperations::clear (dest[c], numSamples);
        return; // the stream list is changing this instant
    }
    const double latency = latencyMs.load (std::memory_order_relaxed);
    for (auto& r : live)
        if (r->firstChannel < numDest)
            r->read (dest + r->firstChannel, numDest - r->firstChannel, numSamples, deviceRate, latency);
}

String NetworkAudio::saveState() const
{
    Array<var> streams;
    for (auto& r : receivers) streams.add (r->info.sdp);
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty ("interface", iface);
    o->setProperty ("latencyMs", latencyMs.load());
    o->setProperty ("streams", streams);
    return JSON::toString (var (o.get()), true);
}

void NetworkAudio::loadState (const String& json)
{
    auto v = JSON::parse (json);
    if (! v.isObject()) return;
    setLatencyMs ((double) v.getProperty ("latencyMs", 5.0));
    const String saved = v.getProperty ("interface", String());
    for (auto& i : localInterfaces())
        if (i.ip == saved) { setInterface (saved); break; }
    if (auto* list = v.getProperty ("streams", var()).getArray())
        for (auto& s : *list)
        {
            StreamInfo info;
            String why;
            if (parseSdp (s.toString(), info, why)) subscribe (info);
        }
}

void NetworkAudio::handleAsyncUpdate()
{
    if (onChange) onChange();
}
}
