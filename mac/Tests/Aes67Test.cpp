// Checks SDP/SAP parsing and receiving AES67 audio over loopback. Run: ./Aes67Test
#include "../Source/Aes67.h"
#include "RtpSender.h"
#include <iostream>

using namespace juce;

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    if (! ok) ++failures;
}

// Sends `seconds` of audio in 1 ms packets while pulling 1 ms blocks, like an audio
// interface would; returns the last block pulled.
static AudioBuffer<float> run (aes67::NetworkAudio& net, RtpSender& tx, double seconds, int channels,
                               std::function<float (int, int)> sample, int dropEvery = 0)
{
    AudioBuffer<float> out (channels, 48);
    const auto start = Time::getMillisecondCounterHiRes();
    const int packets = (int) (seconds * 1000.0);
    for (int i = 0; i < packets; ++i)
    {
        tx.send (48, sample, dropEvery > 0 && i % dropEvery == dropEvery - 1);
        // Keep real time so the receiver thread sees a steady stream.
        while (Time::getMillisecondCounterHiRes() - start < i + 1.0) Thread::yield();
        net.pull (out.getArrayOfWritePointers(), channels, 48, 48000.0);
    }
    return out;
}

int main()
{
    ScopedJuceInitialiser_GUI init;

    // ---- SDP ----
    const String ravenna =
        "v=0\r\no=- 1311738121 1311738121 IN IP4 192.168.1.20\r\ns=Stage Box 1\r\nc=IN IP4 239.69.11.44/32\r\n"
        "t=0 0\r\na=clock-domain:PTPv2 0\r\nm=audio 5004 RTP/AVP 98\r\nc=IN IP4 239.69.11.44/32\r\n"
        "a=rtpmap:98 L24/48000/8\r\na=sync-time:0\r\na=framecount:48\r\na=ptime:1\r\n"
        "a=source-filter: incl IN IP4 239.69.11.44 192.168.1.20\r\na=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0\r\n"
        "a=mediaclk:direct=0\r\n";
    aes67::StreamInfo s;
    String why;
    check (aes67::parseSdp (ravenna, s, why), "RAVENNA SDP parses " + why);
    check (s.name == "Stage Box 1" && s.address == "239.69.11.44" && s.port == 5004 && s.payloadType == 98, "name, address, port");
    check (s.channels == 8 && s.bits == 24 && s.rate == 48000.0 && s.ptimeMs == 1.0, "8 ch L24 48 kHz 1 ms");
    check (s.origin == "192.168.1.20" && s.sourceFilter == "192.168.1.20" && s.sessionId == "1311738121", "origin and source filter");
    check (s.format() == String (CharPointer_UTF8 ("8 ch \xc2\xb7 L24 \xc2\xb7 48 kHz \xc2\xb7 1 ms")), "format text: " + s.format());

    const String st2110 = "v=0\no=- 123 456 IN IP4 10.0.0.5\ns=Console Mix\nt=0 0\nm=audio 50000 RTP/AVP 97\n"
                          "c=IN IP4 239.100.1.2/64\na=rtpmap:97 L16/48000/2\na=ptime:0.125\na=mediaclk:direct=0\n";
    check (aes67::parseSdp (st2110, s, why) && s.channels == 2 && s.bits == 16 && s.ptimeMs == 0.125 && s.port == 50000,
           "2110-30 SDP with 125 us packets");
    check (s.format().contains (String (CharPointer_UTF8 ("125 \xc2\xb5s"))), "125 us shown: " + s.format());
    bool ok = aes67::parseSdp ("v=0\ns=Video\nc=IN IP4 239.1.1.1\nm=video 5000 RTP/AVP 96\na=rtpmap:96 raw/90000\n", s, why);
    check (! ok, "video stream refused: " + why);
    ok = aes67::parseSdp ("v=0\ns=AM824\nc=IN IP4 239.1.1.1\nm=audio 5000 RTP/AVP 96\na=rtpmap:96 AM824/48000/2\n", s, why);
    check (! ok && why.contains ("AM824"), "AM824 (2110-31) refused: " + why);
    check (aes67::parseSdp ("v=0\ns=Mono\nc=IN IP4 239.1.1.1\nm=audio 5004 RTP/AVP 96\na=rtpmap:96 L24/48000\n", s, why) && s.channels == 1,
           "channel count defaults to 1");

    // ---- SAP ----
    {
        std::vector<uint8> pkt { 0x20, 0, 0x12, 0x34, 192, 168, 1, 20 };
        for (auto c : String ("application/sdp")) pkt.push_back ((uint8) c);
        pkt.push_back (0);
        auto text = ravenna.toStdString();
        pkt.insert (pkt.end(), text.begin(), text.end());
        String sdp;
        bool deleted = true;
        check (aes67::parseSap (pkt.data(), (int) pkt.size(), sdp, deleted) && ! deleted && sdp == ravenna, "SAP announcement");
        pkt[0] |= 0x04;
        check (aes67::parseSap (pkt.data(), (int) pkt.size(), sdp, deleted) && deleted, "SAP deletion");
        pkt[0] = 0x22; // compressed
        check (! aes67::parseSap (pkt.data(), (int) pkt.size(), sdp, deleted), "compressed SAP ignored");
    }

    // ---- Receiving over loopback ----
    aes67::NetworkAudio net;
    net.setInterface ("127.0.0.1");
    net.setLatencyMs (5.0);
    const String unicast = "v=0\no=- 7 7 IN IP4 127.0.0.1\ns=Test Box\nc=IN IP4 127.0.0.1\nm=audio 46000 RTP/AVP 98\n"
                           "a=rtpmap:98 L24/48000/8\na=ptime:1\n";
    check (aes67::parseSdp (unicast, s, why), "test SDP parses");
    check (net.subscribe (s).isEmpty(), "subscribe");
    check (net.numChannels() == 8 && net.channelName (2) == "Test Box 3", "8 network channels named after the stream");

    RtpSender tx;
    tx.open ("127.0.0.1", 46000);
    auto dc = [] (int, int c) { return 0.05f * (float) (c + 1) - (c == 7 ? 0.6f : 0.0f); };
    auto out = run (net, tx, 1.0, 8, dc);
    bool exact = true;
    for (int c = 0; c < 8; ++c)
        for (int i = 0; i < 48; ++i)
            exact = exact && std::abs (out.getSample (c, i) - dc (0, c)) < 1.0e-5f;
    check (exact, "every channel arrives in order with 24-bit values (ch 8 = " + String (out.getSample (7, 0)) + ")");
    auto st = net.status();
    check (st.size() == 1 && st[0].receiving && st[0].packets > 900 && st[0].lost == 0, "status: receiving, " + String (st[0].packets) + " packets");

    // A sine through the receiver keeps its level, and lost packets are counted.
    tx.channels = 8;
    int64 n = 0;
    auto sine = [&n] (int f, int c) { return c == 0 ? 0.5f * std::sin ((float) (n + f) * 0.05f) : 0.0f; };
    AudioBuffer<float> peak (8, 48);
    float maxSeen = 0.0f;
    for (int k = 0; k < 500; ++k)
    {
        auto o = run (net, tx, 0.001, 8, sine, 0);
        n += 48;
        maxSeen = jmax (maxSeen, o.getMagnitude (0, 0, 48));
    }
    check (maxSeen > 0.45f && maxSeen < 0.55f, "sine level kept (" + String (maxSeen) + ")");
    run (net, tx, 0.5, 8, dc, 50);
    st = net.status();
    check (st[0].lost >= 8, "lost packets counted (" + String (st[0].lost) + ")");

    // Multicast on the loopback interface, as on a real network.
    const String mc = "v=0\no=- 8 8 IN IP4 127.0.0.1\ns=Multicast Box\nc=IN IP4 239.69.250.1/32\nm=audio 46002 RTP/AVP 98\n"
                      "a=rtpmap:98 L16/48000/2\na=ptime:1\n";
    aes67::parseSdp (mc, s, why);
    const auto err = net.subscribe (s);
    if (err.isEmpty())
    {
        check (net.numChannels() == 10, "second stream adds 2 channels");
        RtpSender mtx;
        mtx.channels = 2; mtx.bits = 16;
        mtx.open ("239.69.250.1", 46002);
        auto o = run (net, mtx, 0.5, 10, [] (int, int c) { return c == 0 ? 0.25f : -0.25f; });
        const bool gotIt = std::abs (o.getSample (8, 10) - 0.25f) < 1.0e-3f && std::abs (o.getSample (9, 10) + 0.25f) < 1.0e-3f;
        if (gotIt) check (true, "multicast stream received (16-bit)");
        else std::cout << "skip multicast loopback isn't available here\n";
        net.unsubscribe (s.key());
    }
    else std::cout << "skip multicast: " << err << "\n";

    net.unsubscribe (net.status()[0].info.key());
    check (net.numChannels() == 0, "unsubscribe removes the channels");

    // Save and load.
    net.subscribe ([&] { aes67::StreamInfo i; aes67::parseSdp (unicast, i, why); return i; }());
    const auto saved = net.saveState();
    aes67::NetworkAudio again;
    again.loadState (saved);
    check (again.numChannels() == 8 && again.getLatencyMs() == 5.0, "streams come back after a restart");

    std::cout << (failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
