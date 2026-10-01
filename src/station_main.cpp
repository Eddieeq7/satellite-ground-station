// gs_station — runs the ground station for a fixed duration and prints a JSON report.
#include "gs/station.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    gs::StationConfig cfg;
    int seconds = 10;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        if (k == "--port") cfg.port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
        else if (k == "--workers") cfg.workers = std::strtoull(argv[i + 1], nullptr, 10);
        else if (k == "--seconds") seconds = std::atoi(argv[i + 1]);
        else { std::fprintf(stderr, "unknown flag %s\n", k.c_str()); return 2; }
    }

    gs::Station st(cfg);
    st.start();
    std::fprintf(stderr, "listening on udp/%u with %zu workers for %ds\n", st.bound_port(), cfg.workers, seconds);
    for (int s = 0; s < seconds; ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::fprintf(stderr, "  t=%ds datagrams=%llu\n", s + 1, (unsigned long long)st.datagrams_live());
    }
    st.stop();

    const auto r = st.report();
    std::printf(
        "{\"datagrams\":%llu,\"received\":%llu,\"lost\":%llu,\"duplicates\":%llu,\"reordered\":%llu,"
        "\"crc_errors\":%llu,\"decode_errors\":%llu,\"ring_full_drops\":%llu,"
        "\"latency_us\":{\"p50\":%.0f,\"p99\":%.0f,\"p999\":%.0f,\"max\":%.1f}}\n",
        (unsigned long long)r.datagrams, (unsigned long long)r.seq.received, (unsigned long long)r.seq.lost,
        (unsigned long long)r.seq.duplicates, (unsigned long long)r.seq.reordered, (unsigned long long)r.crc_errors,
        (unsigned long long)r.decode_errors, (unsigned long long)r.ring_full_drops, r.latency.percentile_us(50),
        r.latency.percentile_us(99), r.latency.percentile_us(99.9), r.latency.max_us());
}
