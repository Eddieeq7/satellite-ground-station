// End-to-end loopback benchmark: an in-process sender thread paced at a target
// rate → UDP → Station. Reports achieved throughput and send→processed latency.
//
//   gs_bench [target_pps] [seconds] [workers]
//
// On Linux the sender batches with sendmmsg(2); elsewhere it sends one datagram per call.
//
// Latency is measured with CLOCK_MONOTONIC on the same host, so it includes the
// kernel UDP path, epoll wakeup, ring hand-off and decode — not just user code.
#include "gs/protocol.hpp"
#include "gs/station.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <algorithm>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
    const uint64_t target_pps = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200'000;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
    gs::StationConfig cfg;
    cfg.port = 0;  // ephemeral
    cfg.workers = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 2;
    cfg.ring_capacity = 1 << 14;

    gs::Station st(cfg);
    st.start();

    std::atomic<uint64_t> sent{0};
    std::thread tx([&] {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(st.bound_port());
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        std::byte payload[64]{};
        uint32_t seq[4]{};
        const uint64_t total = target_pps * seconds;
        const uint64_t t0 = gs::now_ns();
#if defined(__linux__)
        // sendmmsg: pace per batch of 32, one syscall per batch.
        constexpr unsigned kBatch = 32;
        static std::byte bufs[kBatch][gs::kMaxFrame];
        mmsghdr msgs[kBatch]{};
        iovec iovs[kBatch];
        for (uint64_t i = 0; i < total; i += kBatch) {
            const uint64_t due = t0 + i * 1'000'000'000ull / target_pps;
            while (gs::now_ns() < due) {}
            const unsigned n = static_cast<unsigned>(std::min<uint64_t>(kBatch, total - i));
            for (unsigned j = 0; j < n; ++j) {
                const uint8_t apid = static_cast<uint8_t>(1 + (i + j) % 3);
                gs::FrameHeader h{apid, seq[apid]++, gs::now_ns(), 0};
                iovs[j] = {bufs[j], gs::encode(h, payload, bufs[j])};
                msgs[j].msg_hdr.msg_name = &dst;
                msgs[j].msg_hdr.msg_namelen = sizeof(dst);
                msgs[j].msg_hdr.msg_iov = &iovs[j];
                msgs[j].msg_hdr.msg_iovlen = 1;
            }
            unsigned off = 0;
            while (off < n) {
                const int r = ::sendmmsg(fd, msgs + off, n - off, 0);
                if (r > 0) off += static_cast<unsigned>(r);
            }
            sent.fetch_add(n, std::memory_order_relaxed);
        }
#else
        std::byte buf[gs::kMaxFrame];
        for (uint64_t i = 0; i < total; ++i) {
            const uint64_t due = t0 + i * 1'000'000'000ull / target_pps;
            while (gs::now_ns() < due) {}
            const uint8_t apid = static_cast<uint8_t>(1 + i % 3);
            gs::FrameHeader h{apid, seq[apid]++, gs::now_ns(), 0};
            const size_t n = gs::encode(h, payload, buf);
            ::sendto(fd, buf, n, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            sent.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        ::close(fd);
    });

    const auto t0 = std::chrono::steady_clock::now();
    tx.join();
    const double tx_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the pipeline drain
    st.stop();

    const auto r = st.report();
    const double processed = static_cast<double>(r.latency.count());
    std::printf(
        "{\"target_pps\":%llu,\"workers\":%zu,\"sent\":%llu,\"processed\":%.0f,\"achieved_pps\":%.0f,"
        "\"kernel_or_ring_loss_pct\":%.3f,\"ring_full_drops\":%llu,"
        "\"latency_us\":{\"p50\":%.0f,\"p99\":%.0f,\"p999\":%.0f,\"max\":%.1f}}\n",
        (unsigned long long)target_pps, cfg.workers, (unsigned long long)sent.load(), processed,
        processed / tx_secs, 100.0 * (1.0 - processed / static_cast<double>(sent.load())),
        (unsigned long long)r.ring_full_drops, r.latency.percentile_us(50), r.latency.percentile_us(99),
        r.latency.percentile_us(99.9), r.latency.max_us());
}
