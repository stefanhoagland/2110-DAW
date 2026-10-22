#include "Ptp.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <map>
#include <vector>

using namespace juce;

namespace aes67
{
namespace
{
    constexpr int64 second = 1000000000LL;
    constexpr int taiMinusUtc = 37; // seconds, since 2017

    enum Type { sync = 0x0, delayReq = 0x1, followUp = 0x8, delayResp = 0x9, announce = 0xB };

    struct Master
    {
        uint8 priority1 = 255, clockClass = 255, accuracy = 255, priority2 = 255;
        uint16 variance = 0xffff, stepsRemoved = 0;
        uint8 gmIdentity[8] {};
        uint8 port[10] {};
        uint32 address = 0;
        int64 lastSeen = 0;

        // Lower is better, like the best master clock algorithm.
        bool betterThan (const Master& o) const
        {
            if (priority1 != o.priority1) return priority1 < o.priority1;
            if (clockClass != o.clockClass) return clockClass < o.clockClass;
            if (accuracy != o.accuracy) return accuracy < o.accuracy;
            if (variance != o.variance) return variance < o.variance;
            if (priority2 != o.priority2) return priority2 < o.priority2;
            if (int c = std::memcmp (gmIdentity, o.gmIdentity, 8)) return c < 0;
            if (stepsRemoved != o.stepsRemoved) return stepsRemoved < o.stepsRemoved;
            return std::memcmp (port, o.port, 10) < 0;
        }
    };

    String portKey (const uint8* p) { return String::toHexString (p, 10, 0); }

    int openPtpSocket (int port, const String& ifaceIp, String& error)
    {
        in_addr ifa {};
        if (ifaceIp.isEmpty() || inet_pton (AF_INET, ifaceIp.toRawUTF8(), &ifa) != 1) ifa.s_addr = htonl (INADDR_ANY);
        const int fd = socket (AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { error = "Can't create a network socket for PTP."; return -1; }
        int one = 1;
        setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
#ifdef SO_REUSEPORT
        setsockopt (fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof (one));
#endif
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons ((uint16) port);
        a.sin_addr.s_addr = htonl (INADDR_ANY);
        if (bind (fd, reinterpret_cast<sockaddr*> (&a), sizeof (a)) != 0)
        {
            close (fd);
            error = "PTP port " + String (port) + " is in use by another program.";
            return -1;
        }
        ip_mreq m {};
        inet_pton (AF_INET, "224.0.1.129", &m.imr_multiaddr);
        m.imr_interface = ifa;
        if (setsockopt (fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof (m)) != 0)
        {
            close (fd);
            error = "Can't join PTP on that network port.";
            return -1;
        }
        setsockopt (fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof (ifa));
        unsigned char ttl = 1, loop = 1;
        setsockopt (fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof (ttl));
        setsockopt (fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof (loop));
        return fd;
    }
}

struct PtpClock::Impl : Thread
{
    explicit Impl (PtpClock& o) : Thread ("PTP follower"), owner (o) {}
    void run() override { owner.run(); }

    PtpClock& owner;
    int eventFd = -1, generalFd = -1;
    String iface, error;
    uint8 clockId[8] {};

    // Network thread only.
    std::map<String, Master> masters;
    bool haveMaster = false;
    Master best;
    struct Pending { int seq = -1; int64 local = 0, correction = 0; } pending;
    struct Sample { int64 local, offset; };
    std::vector<Sample> samples;
    std::vector<int64> delays;
    int outliers = 0;
    uint16 delaySeq = 0;
    int64 delaySentLocal = 0, nextDelayReq = 0;
    int64 syncCount = 0;

    // The fit, read by now() from other threads under fitLock.
    struct Fit
    {
        bool valid = false;
        int count = 0;
        int64 localRef = 0, offsetRef = 0; // offset = master - local
        double a = 0.0, b = 0.0;            // offset - offsetRef = a + b * (local - localRef) seconds
        int64 delay = 0;
        bool delayMeasured = false;
        int64 lastSync = 0;
        double jitterNs = 0.0;
        int64 offsetAt (int64 local) const { return offsetRef + (int64) std::llround (a + b * (double) (local - localRef) / (double) second); }
    };
    mutable SpinLock fitLock;
    Fit fit;
    String gmText, masterAddr;
    bool masterSeen = false;
};

PtpClock::PtpClock()
{
    impl = std::make_unique<Impl> (*this);
    Random r;
    for (auto& b : impl->clockId) b = (uint8) r.nextInt (256);
    impl->clockId[3] = 0xff;
    impl->clockId[4] = 0xfe;
    const auto sys = std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::system_clock::now().time_since_epoch()).count();
    freeRunOffset = (int64) sys + taiMinusUtc * second - localNs();
}

PtpClock::~PtpClock() { stop(); }

int64 PtpClock::localNs()
{
    return (int64) std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

void PtpClock::start (const String& ifaceIp, int d)
{
    stop();
    domain = jlimit (0, 127, d);
    impl->iface = ifaceIp;
    impl->error = {};
    impl->eventFd = openPtpSocket (319, ifaceIp, impl->error);
    if (impl->eventFd >= 0) impl->generalFd = openPtpSocket (320, ifaceIp, impl->error);
    if (impl->generalFd < 0)
    {
        if (impl->eventFd >= 0) close (impl->eventFd);
        impl->eventFd = -1;
        return;
    }
    impl->startThread (Thread::Priority::high);
}

void PtpClock::stop()
{
    impl->signalThreadShouldExit();
    impl->stopThread (2000);
    if (impl->eventFd >= 0) close (impl->eventFd);
    if (impl->generalFd >= 0) close (impl->generalFd);
    impl->eventFd = impl->generalFd = -1;
    impl->masters.clear();
    impl->haveMaster = false;
    impl->samples.clear();
    impl->delays.clear();
    const SpinLock::ScopedLockType lock (impl->fitLock);
    impl->masterSeen = false;
    impl->gmText = impl->masterAddr = {};
    // Keep the last fit (holdover), so time doesn't jump while switching ports.
}

bool PtpClock::parseHeader (const uint8* b, int size, Header& h)
{
    if (size < 34) return false;
    h.type = b[0] & 0x0f;
    h.version = b[1] & 0x0f;
    h.length = (b[2] << 8) | b[3];
    h.domain = b[4];
    h.twoStep = (b[6] & 0x02) != 0;
    int64 c = 0;
    for (int i = 0; i < 8; ++i) c = (c << 8) | b[8 + i];
    h.correctionNs = c >> 16;
    std::memcpy (h.source, b + 20, 10);
    h.sequence = (b[30] << 8) | b[31];
    return h.version == 2 && h.length <= size;
}

int64 PtpClock::readTimestamp (const uint8* p)
{
    int64 s = 0;
    for (int i = 0; i < 6; ++i) s = (s << 8) | p[i];
    const int64 ns = ((int64) p[6] << 24) | ((int64) p[7] << 16) | ((int64) p[8] << 8) | (int64) p[9];
    return s * second + ns;
}

String PtpClock::formatIdentity (const uint8* id)
{
    StringArray parts;
    for (int i = 0; i < 8; ++i) parts.add (String::toHexString ((int) id[i]).paddedLeft ('0', 2).toUpperCase());
    return parts.joinIntoString ("-");
}

void PtpClock::run()
{
    auto& m = *impl;
    uint8 buf[1500];
    while (! m.threadShouldExit())
    {
        pollfd p[2] { { m.eventFd, POLLIN, 0 }, { m.generalFd, POLLIN, 0 } };
        if (poll (p, 2, 100) > 0)
            for (int i = 0; i < 2; ++i)
                if (p[i].revents & POLLIN)
                {
                    sockaddr_in from {};
                    socklen_t len = sizeof (from);
                    const auto n = recvfrom (p[i].fd, buf, sizeof (buf), 0, reinterpret_cast<sockaddr*> (&from), &len);
                    const int64 rx = localNs();
                    if (n > 0) handle (buf, (int) n, rx, i == 0, from.sin_addr.s_addr);
                }

        const int64 now = localNs();
        // Forget masters that stopped announcing.
        bool changed = false;
        for (auto it = m.masters.begin(); it != m.masters.end();)
            if (now - it->second.lastSeen > 8 * second) { it = m.masters.erase (it); changed = true; }
            else ++it;
        if (changed && m.haveMaster && m.masters.count (portKey (m.best.port)) == 0)
        {
            m.haveMaster = false;
            m.samples.clear();
            m.delays.clear();
            const SpinLock::ScopedLockType lock (m.fitLock);
            m.masterSeen = false;
            m.gmText = m.masterAddr = {};
        }
        if (m.haveMaster && now >= m.nextDelayReq)
        {
            sendDelayReq();
            m.nextDelayReq = now + second + Random::getSystemRandom().nextInt (200) * 1000000LL;
        }
    }
}

void PtpClock::handle (const uint8* b, int size, int64 rx, bool event, uint32 fromAddr)
{
    auto& m = *impl;
    Header h;
    if (! parseHeader (b, size, h) || h.domain != domain.load()) return;
    if (std::memcmp (h.source, m.clockId, 8) == 0) return; // our own Delay_Req looped back

    if (h.type == announce && size >= 64)
    {
        Master a;
        a.priority1 = b[47];
        a.clockClass = b[48];
        a.accuracy = b[49];
        a.variance = (uint16) ((b[50] << 8) | b[51]);
        a.priority2 = b[52];
        std::memcpy (a.gmIdentity, b + 53, 8);
        a.stepsRemoved = (uint16) ((b[61] << 8) | b[62]);
        std::memcpy (a.port, h.source, 10);
        a.address = fromAddr;
        a.lastSeen = rx;
        m.masters[portKey (h.source)] = a;

        const Master* top = nullptr;
        for (auto& [k, c] : m.masters) if (top == nullptr || c.betterThan (*top)) top = &c;
        if (top != nullptr && (! m.haveMaster || std::memcmp (top->port, m.best.port, 10) != 0
                               || std::memcmp (top->gmIdentity, m.best.gmIdentity, 8) != 0))
        {
            m.best = *top;
            m.haveMaster = true;
            m.samples.clear();
            m.delays.clear();
            m.pending = {};
            m.nextDelayReq = rx + 200000000LL;
            char ip[INET_ADDRSTRLEN] = {};
            in_addr ia {};
            ia.s_addr = top->address;
            inet_ntop (AF_INET, &ia, ip, sizeof (ip));
            const SpinLock::ScopedLockType lock (m.fitLock);
            m.gmText = formatIdentity (top->gmIdentity);
            m.masterAddr = ip;
            m.masterSeen = true;
            m.fit.delay = 0;
            m.fit.delayMeasured = false;
        }
        return;
    }

    if (! m.haveMaster || std::memcmp (h.source, m.best.port, 10) != 0) return;

    if (h.type == sync && event && size >= 44)
    {
        if (h.twoStep)
            m.pending = { h.sequence, rx, h.correctionNs };
        else
            addSync (rx, readTimestamp (b + 34) + h.correctionNs);
    }
    else if (h.type == followUp && size >= 44)
    {
        if (h.sequence == m.pending.seq)
            addSync (m.pending.local, readTimestamp (b + 34) + m.pending.correction + h.correctionNs);
        m.pending.seq = -1;
    }
    else if (h.type == delayResp && size >= 54)
    {
        if (std::memcmp (b + 44, m.clockId, 8) != 0 || h.sequence != m.delaySeq) return;
        const int64 t4 = readTimestamp (b + 34) - h.correctionNs;
        Impl::Fit f;
        {
            const SpinLock::ScopedLockType lock (m.fitLock);
            f = m.fit;
        }
        if (! f.valid) return;
        const int64 d = ((t4 - m.delaySentLocal) - f.offsetAt (m.delaySentLocal)) / 2;
        if (d < -1000000 || d > 50000000) return; // not believable
        m.delays.push_back (d);
        if (m.delays.size() > 9) m.delays.erase (m.delays.begin());
        auto sorted = m.delays;
        std::sort (sorted.begin(), sorted.end());
        const SpinLock::ScopedLockType lock (m.fitLock);
        m.fit.delay = jmax ((int64) 0, sorted[sorted.size() / 2]);
        m.fit.delayMeasured = true;
    }
}

void PtpClock::sendDelayReq()
{
    auto& m = *impl;
    uint8 p[44] {};
    p[0] = delayReq;
    p[1] = 2;
    p[3] = 44;
    p[4] = (uint8) domain.load();
    std::memcpy (p + 20, m.clockId, 8);
    p[29] = 1;
    ++m.delaySeq;
    p[30] = (uint8) (m.delaySeq >> 8);
    p[31] = (uint8) m.delaySeq;
    p[32] = 1;
    p[33] = 0x7f;
    sockaddr_in to {};
    to.sin_family = AF_INET;
    to.sin_port = htons (319);
    inet_pton (AF_INET, "224.0.1.129", &to.sin_addr);
    m.delaySentLocal = localNs();
    sendto (m.eventFd, p, sizeof (p), 0, reinterpret_cast<sockaddr*> (&to), sizeof (to));
}

void PtpClock::addSync (int64 local, int64 master)
{
    auto& m = *impl;
    const int64 offset = master - local;
    ++m.syncCount;
    Impl::Fit f;
    {
        const SpinLock::ScopedLockType lock (m.fitLock);
        f = m.fit;
    }
    // A sample far off the fit is a glitch, unless they keep coming (the master stepped).
    if (f.valid && m.samples.size() >= 4 && std::abs (offset - f.offsetAt (local)) > 1000000)
    {
        if (++m.outliers < 4) return;
        m.samples.clear();
    }
    m.outliers = 0;
    m.samples.push_back ({ local, offset });
    while (m.samples.size() > 64 || (m.samples.size() > 4 && local - m.samples.front().local > 16 * second))
        m.samples.erase (m.samples.begin());

    // Least squares: offset against local time.
    Impl::Fit n;
    n.localRef = local;
    n.offsetRef = m.samples.front().offset;
    const auto count = (double) m.samples.size();
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (auto& s : m.samples)
    {
        const double x = (double) (s.local - n.localRef) / (double) second, y = (double) (s.offset - n.offsetRef);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double den = count * sxx - sx * sx;
    n.b = m.samples.size() >= 3 && den > 1.0e-9 ? (count * sxy - sx * sy) / den : 0.0;
    n.b = jlimit (-500000.0, 500000.0, n.b); // 500 ppm at most
    n.a = (sy - n.b * sx) / count;
    double sq = 0;
    for (auto& s : m.samples)
    {
        const double x = (double) (s.local - n.localRef) / (double) second, y = (double) (s.offset - n.offsetRef);
        sq += (y - n.a - n.b * x) * (y - n.a - n.b * x);
    }
    n.jitterNs = std::sqrt (sq / count);
    n.count = (int) m.samples.size();
    n.valid = true;
    n.lastSync = local;
    const SpinLock::ScopedLockType lock (m.fitLock);
    n.delay = m.fit.delay;
    n.delayMeasured = m.fit.delayMeasured;
    m.fit = n;
}

bool PtpClock::now (int64& ns) const
{
    const int64 local = localNs();
    Impl::Fit f;
    {
        const SpinLock::ScopedLockType lock (impl->fitLock);
        f = impl->fit;
    }
    if (! f.valid) { ns = local + freeRunOffset; return false; }
    ns = local + f.offsetAt (local) + f.delay; // holdover on the last fit if syncs stop
    return f.count >= 4 && local - f.lastSync < 5 * second;
}

PtpClock::Status PtpClock::status() const
{
    Status s;
    s.running = impl->isThreadRunning();
    s.error = impl->error;
    s.domain = domain.load();
    s.syncs = impl->syncCount;
    const SpinLock::ScopedLockType lock (impl->fitLock);
    const auto& f = impl->fit;
    s.masterSeen = impl->masterSeen;
    s.grandmaster = impl->gmText;
    s.masterAddress = impl->masterAddr;
    s.locked = s.masterSeen && f.valid && f.count >= 4 && localNs() - f.lastSync < 5 * second;
    s.jitterUs = f.jitterNs / 1000.0;
    s.delayUs = (double) f.delay / 1000.0;
    s.delayMeasured = f.delayMeasured;
    s.rateErrorPpm = f.b / 1000.0;
    return s;
}
}
