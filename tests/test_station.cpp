// Integration tests: real UDP over loopback into a running Station.
#include "gs/protocol.hpp"
#include "gs/station.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace gs;

namespace {
class Sender {
public:
    explicit Sender(uint16_t port) : fd_(::socket(AF_INET, SOCK_DGRAM, 0)) {
        dst_.sin_family = AF_INET;
        dst_.sin_port = htons(port);
        dst_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    ~Sender() { ::close(fd_); }
    void frame(uint8_t apid, uint32_t seq) {
        std::byte buf[kMaxFrame];
        std::byte pl[16]{};
        size_t n = encode({apid, seq, now_ns(), 0}, pl, buf);
        raw(buf, n);
    }
    void raw(const std::byte* p, size_t n) {
        ::sendto(fd_, p, n, 0, reinterpret_cast<sockaddr*>(&dst_), sizeof(dst_));
        std::this_thread::sleep_for(std::chrono::microseconds(50));  // keep loopback ordering deterministic
    }

private:
    int fd_;
    sockaddr_in dst_{};
};

StationReport run(const std::function<void(Sender&)>& script, size_t workers = 2) {
    Station st({.port = 0, .workers = workers, .ring_capacity = 1024});
    st.start();
    {
        Sender s(st.bound_port());
        script(s);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    st.stop();
    return st.report();
}
}  // namespace

TEST(Station, CleanStream) {
    auto r = run([](Sender& s) {
        for (uint32_t i = 0; i < 300; ++i) s.frame(static_cast<uint8_t>(1 + i % 3), i / 3);
    });
    EXPECT_EQ(r.datagrams, 300u);
    EXPECT_EQ(r.seq.received, 300u);
    EXPECT_EQ(r.seq.lost, 0u);
    EXPECT_EQ(r.seq.duplicates, 0u);
    EXPECT_EQ(r.latency.count(), 300u);
}

TEST(Station, DetectsLossDuplicateReorderPerApid) {
    auto r = run([](Sender& s) {
        for (uint32_t q : {0u, 1u, 3u, 4u, 4u, 6u, 5u, 7u}) s.frame(1, q);  // lost 2, dup 4, swap 5/6
        for (uint32_t q : {0u, 1u, 2u}) s.frame(2, q);                       // clean, independent seq space
    });
    EXPECT_EQ(r.seq.received, 10u);
    EXPECT_EQ(r.seq.lost, 1u);
    EXPECT_EQ(r.seq.duplicates, 1u);
    EXPECT_EQ(r.seq.reordered, 1u);
}

TEST(Station, RejectsCorruptedFrames) {
    auto r = run([](Sender& s) {
        std::byte buf[kMaxFrame];
        std::byte pl[8]{};
        size_t n = encode({1, 0, now_ns(), 0}, pl, buf);
        buf[kHeaderSize] ^= std::byte{0xFF};
        s.raw(buf, n);
        std::byte junk[3]{};
        s.raw(junk, sizeof junk);
        s.frame(1, 1);
    });
    EXPECT_EQ(r.datagrams, 3u);
    EXPECT_EQ(r.decode_errors, 2u);
    EXPECT_EQ(r.crc_errors, 1u);
    EXPECT_EQ(r.seq.received, 1u);
}

TEST(Station, SingleWorkerHandlesAllApids) {
    auto r = run([](Sender& s) {
        for (uint32_t i = 0; i < 50; ++i) s.frame(static_cast<uint8_t>(i % 5), i / 5);
    }, 1);
    EXPECT_EQ(r.seq.received, 50u);
    EXPECT_EQ(r.seq.lost, 0u);
}

TEST(Station, StopIsIdempotent) {
    Station st({.port = 0, .workers = 1});
    st.start();
    st.stop();
    st.stop();
    SUCCEED();
}

TEST(LatencyHistogram, Percentiles) {
    LatencyHistogram h;
    for (int i = 1; i <= 100; ++i) h.record_ns(static_cast<uint64_t>(i) * 1000);
    EXPECT_EQ(h.count(), 100u);
    EXPECT_NEAR(h.percentile_us(50), 51, 1);
    EXPECT_NEAR(h.percentile_us(99), 100, 1);
    EXPECT_NEAR(h.max_us(), 100, 0.01);
}

TEST(LatencyHistogram, OverflowBucket) {
    LatencyHistogram h;
    h.record_ns(50'000'000);  // 50 ms
    EXPECT_EQ(h.count(), 1u);
    EXPECT_GT(h.percentile_us(99), 9'000);
}
