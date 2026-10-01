#pragma once
// Wire format for simulated satellite telemetry frames.
//
//  offset size field
//  0      2    magic        0x5A47 ("GZ")
//  2      1    version      1
//  3      1    apid         application/subsystem id (e.g. 1=EPS, 2=ADCS, 3=THERMAL)
//  4      4    seq          monotonically increasing per sender
//  8      8    tx_ns        sender monotonic timestamp, nanoseconds
//  16     2    payload_len  bytes of payload that follow
//  18     N    payload
//  18+N   4    crc32        over bytes [0, 18+N)
//
// All multi-byte fields are little-endian and serialized byte-by-byte, so the
// format does not depend on host endianness or struct padding.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gs {

inline constexpr uint16_t kMagic = 0x5A47;
inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kHeaderSize = 18;
inline constexpr size_t kCrcSize = 4;
inline constexpr size_t kMaxPayload = 1024;
inline constexpr size_t kMaxFrame = kHeaderSize + kMaxPayload + kCrcSize;

struct FrameHeader {
    uint8_t apid = 0;
    uint32_t seq = 0;
    uint64_t tx_ns = 0;
    uint16_t payload_len = 0;
};

enum class ParseError { TooShort, BadMagic, BadVersion, LengthMismatch, PayloadTooLarge, BadCrc };

const char* to_string(ParseError e);

// Writes a full frame into `out`; returns bytes written (0 if `out` is too small
// or the payload exceeds kMaxPayload).
size_t encode(const FrameHeader& h, std::span<const std::byte> payload, std::span<std::byte> out);

// Validates framing and CRC. On success the payload span aliases `frame`.
struct Parsed {
    FrameHeader header;
    std::span<const std::byte> payload;
};

// Minimal C++20 stand-in for std::expected<Parsed, ParseError>.
class DecodeResult {
public:
    DecodeResult(Parsed p) : ok_(true), value_(p) {}
    DecodeResult(ParseError e) : ok_(false), err_(e) {}
    explicit operator bool() const { return ok_; }
    const Parsed* operator->() const { return &value_; }
    const Parsed& operator*() const { return value_; }
    ParseError error() const { return err_; }

private:
    bool ok_;
    Parsed value_{};
    ParseError err_{};
};

DecodeResult decode(std::span<const std::byte> frame);

}  // namespace gs
