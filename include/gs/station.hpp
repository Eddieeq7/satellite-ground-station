#pragma once
// The ground station pipeline:
//
//   UDP socket ──► [receive thread: epoll + non-blocking recvfrom drain]
//                        │  shard by APID (apid % workers)
//                        ▼
//                 SpscRing<RawFrame> × N      (one ring per worker ⇒ true SPSC)
//                        │
//                        ▼
//                 [worker threads: decode + CRC check + SequenceTracker + latency]
//
// Sharding by APID means every subsystem's sequence space is owned by exactly one
// worker, so sequence tracking needs no locks.
#include "gs/protocol.hpp"
#include "gs/sequence_tracker.hpp"
#include "gs/spsc_ring.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace gs {

struct RawFrame {
    uint64_t rx_ns = 0;
    uint16_t len = 0;
    std::array<std::byte, kMaxFrame> bytes{};
};

// Latency histogram: 1 µs buckets up to 10 ms, plus overflow.
class LatencyHistogram {
public:
    static constexpr size_t kBuckets = 10'000;
    void record_ns(uint64_t ns);
    void merge(const LatencyHistogram& o);
    uint64_t count() const { return count_; }
    double percentile_us(double p) const;
    double max_us() const { return max_ns_ / 1000.0; }

private:
    std::array<uint64_t, kBuckets + 1> buckets_{};
    uint64_t count_ = 0;
    uint64_t max_ns_ = 0;
};

struct StationConfig {
    uint16_t port = 9000;
    size_t workers = 2;
    size_t ring_capacity = 4096;
    int rcvbuf_bytes = 8 << 20;
};

struct StationReport {
    uint64_t datagrams = 0;
    uint64_t ring_full_drops = 0;   // backpressure: ring full when receiver tried to push
    uint64_t decode_errors = 0;     // bad magic / length / CRC
    uint64_t crc_errors = 0;
    SeqStats seq;                   // summed across APIDs
    LatencyHistogram latency;
};

uint64_t now_ns();

class Station {
public:
    explicit Station(StationConfig cfg);
    ~Station();
    void start();
    void stop();             // joins all threads
    uint16_t bound_port() const { return bound_port_; }
    StationReport report() const;  // call after stop()

    // Live counters (relaxed, for progress output).
    uint64_t datagrams_live() const { return datagrams_.load(std::memory_order_relaxed); }

private:
    struct Worker;
    void receive_loop();
    void dispatch(RawFrame& frame);  // receive thread only

    StationConfig cfg_;
    int fd_ = -1;
    uint16_t bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> rx_done_{false};
    std::atomic<uint64_t> datagrams_{0};
    uint64_t ring_full_drops_ = 0;  // receive thread only
    std::vector<std::unique_ptr<Worker>> workers_;
    std::thread rx_thread_;
};

}  // namespace gs
