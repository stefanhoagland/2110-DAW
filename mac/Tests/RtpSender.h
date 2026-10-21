#pragma once
// Sends test AES67 audio (RTP, L24 or L16, big-endian) to an address, so the receiver
// can be checked without real network gear.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <vector>

struct RtpSender
{
    int fd = -1;
    sockaddr_in to {};
    uint16_t seq = 1000;
    uint32_t ts = 12345;
    int channels = 8, bits = 24, payloadType = 98;

    bool open (const char* address, int port)
    {
        fd = socket (AF_INET, SOCK_DGRAM, 0);
        to.sin_family = AF_INET;
        to.sin_port = htons ((uint16_t) port);
        inet_pton (AF_INET, address, &to.sin_addr);
        in_addr lo {};
        inet_pton (AF_INET, "127.0.0.1", &lo);
        setsockopt (fd, IPPROTO_IP, IP_MULTICAST_IF, &lo, sizeof (lo));
        unsigned char loop = 1;
        setsockopt (fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof (loop));
        return fd >= 0;
    }
    ~RtpSender() { if (fd >= 0) close (fd); }

    // One packet of `frames` frames; sample(frame, channel) gives the value.
    template <typename F> void send (int frames, F sample, bool skip = false)
    {
        std::vector<uint8_t> p (12 + (size_t) (frames * channels * bits / 8));
        p[0] = 0x80; p[1] = (uint8_t) payloadType;
        p[2] = (uint8_t) (seq >> 8); p[3] = (uint8_t) seq;
        for (int i = 0; i < 4; ++i) p[(size_t) (4 + i)] = (uint8_t) (ts >> (24 - 8 * i));
        size_t at = 12;
        for (int f = 0; f < frames; ++f)
            for (int c = 0; c < channels; ++c)
            {
                const int32_t v = (int32_t) (sample (f, c) * 2147483647.0);
                p[at++] = (uint8_t) (v >> 24); p[at++] = (uint8_t) (v >> 16);
                if (bits == 24) p[at++] = (uint8_t) (v >> 8);
            }
        if (! skip) sendto (fd, p.data(), p.size(), 0, reinterpret_cast<sockaddr*> (&to), sizeof (to));
        ++seq;
        ts += (uint32_t) frames;
    }
};
