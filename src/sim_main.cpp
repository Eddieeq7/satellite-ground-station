// gs_sim — simulated satellite. Emits telemetry frames for several APIDs over UDP.
// Optional application-level impairment (--loss/--dup/--reorder) for testing on
// hosts without tc netem; on Linux prefer scripts/netem.sh for real link impairment.
#include "gs/protocol.hpp"
#include "gs/station.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

struct Args {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    uint64_t count = 100'000;
    uint64_t rate = 0;  // packets/s, 0 = as fast as possible
    int apids = 3;
    size_t payload = 64;
    double loss = 0, dup = 0, reorder = 0;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--host") a.host = v;
        else if (k == "--port") a.port = static_cast<uint16_t>(std::atoi(v));
        else if (k == "--count") a.count = std::strtoull(v, nullptr, 10);
        else if (k == "--rate") a.rate = std::strtoull(v, nullptr, 10);
        else if (k == "--apids") a.apids = std::atoi(v);
        else if (k == "--payload") a.payload = std::strtoull(v, nullptr, 10);
        else if (k == "--loss") a.loss = std::atof(v);
        else if (k == "--dup") a.dup = std::atof(v);
        else if (k == "--reorder") a.reorder = std::atof(v);
        else { std::fprintf(stderr, "unknown flag %s\n", k.c_str()); std::exit(2); }
    }
    return a;
}

// Fake but plausible housekeeping values so payloads aren't all zeros.
void fill_payload(uint8_t apid, uint32_t seq, std::byte* out, size_t n) {
    const double t = seq * 0.01;
    float vals[4] = {
        static_cast<float>(apid == 1 ? 28.0 + 0.4 * std::sin(t) : 0),        // bus voltage
        static_cast<float>(apid == 2 ? 0.05 * std::cos(t * 3) : 0),          // body rate
        static_cast<float>(apid == 3 ? 21.0 + 6.0 * std::sin(t / 7) : 0),    // panel temp
        static_cast<float>(seq % 1000),
    };
    std::memset(out, 0, n);
    std::memcpy(out, vals, std::min(n, sizeof(vals)));
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(a.port);
    ::inet_pton(AF_INET, a.host.c_str(), &dst.sin_addr);

    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> u(0, 1);
    std::vector<uint32_t> seq(256, 0);
    std::byte payload[gs::kMaxPayload];
    std::byte buf[gs::kMaxFrame];
    std::byte held[gs::kMaxFrame];
    size_t held_len = 0;
    int held_countdown = 0;  // release the held frame after this many sends

    auto send = [&](const std::byte* p, size_t n) {
        ::sendto(fd, p, n, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    };

    const auto start = std::chrono::steady_clock::now();
    uint64_t sent = 0, dropped = 0, duped = 0, reordered = 0;
    for (uint64_t i = 0; i < a.count; ++i) {
        if (a.rate) {
            const auto due = start + std::chrono::nanoseconds(i * 1'000'000'000ull / a.rate);
            while (std::chrono::steady_clock::now() < due) {}
        }
        const uint8_t apid = static_cast<uint8_t>(1 + i % a.apids);
        gs::FrameHeader h{apid, seq[apid]++, gs::now_ns(), 0};
        fill_payload(apid, h.seq, payload, a.payload);
        const size_t n = gs::encode(h, {payload, a.payload}, buf);

        if (u(rng) < a.loss) { ++dropped; continue; }
        if (held_len == 0 && u(rng) < a.reorder) {
            // Hold this frame until the next frame of the same APID has gone out
            // (APIDs are interleaved round-robin, so that's `apids` sends later).
            std::memcpy(held, buf, n);
            held_len = n;
            held_countdown = a.apids;
            ++reordered;
            continue;
        }
        send(buf, n);
        ++sent;
        if (u(rng) < a.dup) { send(buf, n); ++duped; }
        if (held_len && --held_countdown == 0) { send(held, held_len); ++sent; held_len = 0; }
    }
    if (held_len) { send(held, held_len); ++sent; }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("{\"sent\":%llu,\"dropped\":%llu,\"duplicated\":%llu,\"reordered\":%llu,\"seconds\":%.3f,\"pps\":%.0f}\n",
                (unsigned long long)sent, (unsigned long long)dropped, (unsigned long long)duped,
                (unsigned long long)reordered, secs, sent / secs);
    ::close(fd);
}
