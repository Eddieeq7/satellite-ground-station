#include "gs/crc32.hpp"
#include "gs/protocol.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string_view>
#include <vector>

using namespace gs;

namespace {
std::span<const std::byte> bytes(std::string_view s) { return std::as_bytes(std::span(s.data(), s.size())); }

std::vector<std::byte> make_frame(FrameHeader h, std::vector<std::byte> payload) {
    std::vector<std::byte> out(kMaxFrame);
    out.resize(encode(h, payload, out));
    return out;
}
}  // namespace

// ── CRC-32 ──────────────────────────────────────────────────────────────────

TEST(Crc32, StandardCheckValue) { EXPECT_EQ(crc32(bytes("123456789")), 0xCBF43926u); }
TEST(Crc32, Empty) { EXPECT_EQ(crc32({}), 0u); }
TEST(Crc32, KnownString) { EXPECT_EQ(crc32(bytes("The quick brown fox jumps over the lazy dog")), 0x414FA339u); }
TEST(Crc32, IncrementalMatchesOneShot) {
    auto all = bytes("hello, ground station");
    EXPECT_EQ(crc32(all.subspan(7), crc32(all.first(7))), crc32(all));
}
TEST(Crc32, SingleBitFlipChangesValue) {
    std::vector<std::byte> v(64, std::byte{0xAB});
    const auto a = crc32(v);
    v[31] ^= std::byte{0x01};
    EXPECT_NE(crc32(v), a);
}
constexpr std::byte kA[] = {std::byte{'a'}};
static_assert(crc32(kA) == 0xE8B7BE43u, "crc32 is evaluated at compile time");

// ── Encode / decode round trip ──────────────────────────────────────────────

TEST(Protocol, RoundTripEmptyPayload) {
    auto f = make_frame({2, 7, 123, 0}, {});
    ASSERT_EQ(f.size(), kHeaderSize + kCrcSize);
    auto p = decode(f);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->header.apid, 2);
    EXPECT_EQ(p->header.seq, 7u);
    EXPECT_EQ(p->header.tx_ns, 123u);
    EXPECT_TRUE(p->payload.empty());
}

TEST(Protocol, RoundTripWithPayload) {
    std::vector<std::byte> pl(100);
    for (size_t i = 0; i < pl.size(); ++i) pl[i] = std::byte(i);
    auto f = make_frame({3, 0xDEADBEEF, 0x0102030405060708ull, 0}, pl);
    auto p = decode(f);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->header.seq, 0xDEADBEEFu);
    EXPECT_EQ(p->header.tx_ns, 0x0102030405060708ull);
    ASSERT_EQ(p->payload.size(), pl.size());
    EXPECT_EQ(std::memcmp(p->payload.data(), pl.data(), pl.size()), 0);
}

TEST(Protocol, RoundTripMaxPayload) {
    std::vector<std::byte> pl(kMaxPayload, std::byte{0x5A});
    auto f = make_frame({1, 1, 1, 0}, pl);
    EXPECT_EQ(f.size(), kMaxFrame);
    EXPECT_TRUE(decode(f));
}

TEST(Protocol, EncodeRejectsOversizedPayload) {
    std::vector<std::byte> pl(kMaxPayload + 1);
    std::vector<std::byte> out(kMaxFrame + 16);
    EXPECT_EQ(encode({}, pl, out), 0u);
}

TEST(Protocol, EncodeRejectsSmallBuffer) {
    std::vector<std::byte> out(kHeaderSize + kCrcSize - 1);
    EXPECT_EQ(encode({}, {}, out), 0u);
}

TEST(Protocol, WireIsLittleEndian) {
    auto f = make_frame({9, 0x11223344, 0, 0}, {});
    EXPECT_EQ(f[0], std::byte{0x47});
    EXPECT_EQ(f[1], std::byte{0x5A});
    EXPECT_EQ(f[3], std::byte{9});
    EXPECT_EQ(f[4], std::byte{0x44});
    EXPECT_EQ(f[7], std::byte{0x11});
}

// ── Decode error paths ──────────────────────────────────────────────────────

TEST(Protocol, TooShort) {
    std::vector<std::byte> f(kHeaderSize + kCrcSize - 1);
    EXPECT_EQ(decode(f).error(), ParseError::TooShort);
}

TEST(Protocol, BadMagic) {
    auto f = make_frame({1, 1, 1, 0}, {});
    f[0] = std::byte{0};
    EXPECT_EQ(decode(f).error(), ParseError::BadMagic);
}

TEST(Protocol, BadVersion) {
    auto f = make_frame({1, 1, 1, 0}, {});
    f[2] = std::byte{2};
    EXPECT_EQ(decode(f).error(), ParseError::BadVersion);
}

TEST(Protocol, TruncatedFrameIsLengthMismatch) {
    auto f = make_frame({1, 1, 1, 0}, std::vector<std::byte>(10));
    f.pop_back();
    EXPECT_EQ(decode(f).error(), ParseError::LengthMismatch);
}

TEST(Protocol, TrailingGarbageIsLengthMismatch) {
    auto f = make_frame({1, 1, 1, 0}, std::vector<std::byte>(10));
    f.push_back(std::byte{0});
    EXPECT_EQ(decode(f).error(), ParseError::LengthMismatch);
}

TEST(Protocol, DeclaredPayloadTooLarge) {
    auto f = make_frame({1, 1, 1, 0}, {});
    f[16] = std::byte{0xFF};
    f[17] = std::byte{0xFF};
    EXPECT_EQ(decode(f).error(), ParseError::PayloadTooLarge);
}

TEST(Protocol, CorruptPayloadFailsCrc) {
    auto f = make_frame({1, 1, 1, 0}, std::vector<std::byte>(32, std::byte{1}));
    f[kHeaderSize + 5] ^= std::byte{0x80};
    EXPECT_EQ(decode(f).error(), ParseError::BadCrc);
}

TEST(Protocol, CorruptHeaderFieldFailsCrc) {
    auto f = make_frame({1, 1, 1, 0}, {});
    f[5] ^= std::byte{0x01};  // seq byte
    EXPECT_EQ(decode(f).error(), ParseError::BadCrc);
}

TEST(Protocol, CorruptCrcFieldFailsCrc) {
    auto f = make_frame({1, 1, 1, 0}, {});
    f.back() ^= std::byte{0xFF};
    EXPECT_EQ(decode(f).error(), ParseError::BadCrc);
}

TEST(Protocol, EveryErrorHasName) {
    for (auto e : {ParseError::TooShort, ParseError::BadMagic, ParseError::BadVersion, ParseError::LengthMismatch,
                   ParseError::PayloadTooLarge, ParseError::BadCrc})
        EXPECT_STRNE(to_string(e), "unknown");
}

// Fuzz-ish: every single-bit flip anywhere in a frame must be rejected.
TEST(Protocol, AllSingleBitFlipsRejected) {
    auto good = make_frame({4, 99, 12345, 0}, std::vector<std::byte>(24, std::byte{0x3C}));
    for (size_t i = 0; i < good.size(); ++i) {
        for (int b = 0; b < 8; ++b) {
            auto f = good;
            f[i] ^= std::byte(1 << b);
            EXPECT_FALSE(decode(f)) << "flip at byte " << i << " bit " << b;
        }
    }
}
