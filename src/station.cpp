#include "gs/station.hpp"

#include "gs/poller.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>

namespace gs {

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// ── LatencyHistogram ────────────────────────────────────────────────────────

void LatencyHistogram::record_ns(uint64_t ns) {
    const uint64_t us = ns / 1000;
    ++buckets_[us < kBuckets ? us : kBuckets];
    ++count_;
    if (ns > max_ns_) max_ns_ = ns;
}

void LatencyHistogram::merge(const LatencyHistogram& o) {
    for (size_t i = 0; i <= kBuckets; ++i) buckets_[i] += o.buckets_[i];
    count_ += o.count_;
    if (o.max_ns_ > max_ns_) max_ns_ = o.max_ns_;
}

double LatencyHistogram::percentile_us(double p) const {
    if (count_ == 0) return 0;
    const uint64_t target = static_cast<uint64_t>(p / 100.0 * static_cast<double>(count_ - 1)) + 1;
    uint64_t acc = 0;
    for (size_t i = 0; i <= kBuckets; ++i) {
        acc += buckets_[i];
        if (acc >= target) return static_cast<double>(i + 1);  // upper edge of the 1 µs bucket
    }
    return max_us();
}

// ── Worker ──────────────────────────────────────────────────────────────────

struct Station::Worker {
    explicit Worker(size_t cap) : ring(cap) {}
    SpscRing<RawFrame> ring;
    std::thread thread;
    // Owned by the worker thread; read by report() only after join.
    std::map<uint8_t, SequenceTracker> trackers;
    LatencyHistogram latency;
    uint64_t decode_errors = 0;
    uint64_t crc_errors = 0;

    void run(const std::atomic<bool>& producer_done) {
        for (;;) {
            auto f = ring.try_pop();
            if (!f) {
                if (producer_done.load(std::memory_order_acquire) && ring.size_approx() == 0) return;
                std::this_thread::yield();
                continue;
            }
            auto parsed = decode({f->bytes.data(), f->len});
            if (!parsed) {
                ++decode_errors;
                if (parsed.error() == ParseError::BadCrc) ++crc_errors;
                continue;
            }
            trackers[parsed->header.apid].observe(parsed->header.seq);
            const uint64_t done = now_ns();
            if (done > parsed->header.tx_ns) latency.record_ns(done - parsed->header.tx_ns);
        }
    }
};

// ── Station ─────────────────────────────────────────────────────────────────

Station::Station(StationConfig cfg) : cfg_(cfg) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "socket");
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &cfg_.rcvbuf_bytes, sizeof(cfg_.rcvbuf_bytes));
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg_.port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::system_error(errno, std::generic_category(), "bind");
    socklen_t len = sizeof(addr);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    bound_port_ = ntohs(addr.sin_port);

    for (size_t i = 0; i < cfg_.workers; ++i) workers_.push_back(std::make_unique<Worker>(cfg_.ring_capacity));
}

Station::~Station() {
    stop();
    if (fd_ >= 0) ::close(fd_);
}

void Station::start() {
    running_.store(true, std::memory_order_release);
    for (auto& w : workers_) w->thread = std::thread([wp = w.get(), this] { wp->run(rx_done_); });
    rx_thread_ = std::thread([this] { receive_loop(); });
}

void Station::stop() {
    if (!running_.exchange(false)) return;
    if (rx_thread_.joinable()) rx_thread_.join();
    // Workers drain their rings only after the receiver has stopped producing.
    rx_done_.store(true, std::memory_order_release);
    for (auto& w : workers_)
        if (w->thread.joinable()) w->thread.join();
}

void Station::dispatch(RawFrame& frame) {
    datagrams_.fetch_add(1, std::memory_order_relaxed);
    // Byte 3 is the APID; short/garbage frames go to worker 0, which rejects them.
    const size_t shard = frame.len > 3 ? static_cast<uint8_t>(frame.bytes[3]) % workers_.size() : 0;
    if (!workers_[shard]->ring.try_push(std::move(frame))) ++ring_full_drops_;
}

void Station::receive_loop() {
    Poller poller;
    poller.add_readable(fd_);
#if defined(__linux__)
    // recvmmsg pulls up to kBatch datagrams per syscall.
    constexpr unsigned kBatch = 64;
    auto batch = std::make_unique<RawFrame[]>(kBatch);
    mmsghdr msgs[kBatch]{};
    iovec iovs[kBatch];
    for (unsigned i = 0; i < kBatch; ++i) {
        iovs[i] = {batch[i].bytes.data(), batch[i].bytes.size()};
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
#else
    RawFrame frame;
#endif
    while (running_.load(std::memory_order_acquire)) {
        if (poller.wait(std::chrono::milliseconds(50)) == 0) continue;
        // Edge-triggered: drain until EAGAIN or we'd miss the next edge.
        for (;;) {
#if defined(__linux__)
            const int n = ::recvmmsg(fd_, msgs, kBatch, MSG_DONTWAIT, nullptr);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                throw std::system_error(errno, std::generic_category(), "recvmmsg");
            }
            const uint64_t t = now_ns();
            for (int i = 0; i < n; ++i) {
                batch[i].rx_ns = t;
                batch[i].len = static_cast<uint16_t>(msgs[i].msg_len);
                dispatch(batch[i]);
            }
#else
            const ssize_t n = ::recvfrom(fd_, frame.bytes.data(), frame.bytes.size(), 0, nullptr, nullptr);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                throw std::system_error(errno, std::generic_category(), "recvfrom");
            }
            frame.rx_ns = now_ns();
            frame.len = static_cast<uint16_t>(n);
            dispatch(frame);
#endif
        }
    }
}

StationReport Station::report() const {
    StationReport r;
    r.datagrams = datagrams_.load();
    r.ring_full_drops = ring_full_drops_;
    for (const auto& w : workers_) {
        r.decode_errors += w->decode_errors;
        r.crc_errors += w->crc_errors;
        r.latency.merge(w->latency);
        for (const auto& [apid, t] : w->trackers) {
            const auto& s = t.stats();
            r.seq.received += s.received;
            r.seq.lost += s.lost;
            r.seq.duplicates += s.duplicates;
            r.seq.reordered += s.reordered;
            r.seq.too_old += s.too_old;
        }
    }
    return r;
}

}  // namespace gs
