#include "gs/protocol.hpp"

#include "gs/crc32.hpp"

#include <cstring>

namespace gs {

namespace {
template <typename T>
void put_le(std::byte* p, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}
template <typename T>
T get_le(const std::byte* p) {
    T v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<uint8_t>(p[i])) << (8 * i);
    return v;
}
}  // namespace

const char* to_string(ParseError e) {
    switch (e) {
        case ParseError::TooShort: return "too_short";
        case ParseError::BadMagic: return "bad_magic";
        case ParseError::BadVersion: return "bad_version";
        case ParseError::LengthMismatch: return "length_mismatch";
        case ParseError::PayloadTooLarge: return "payload_too_large";
        case ParseError::BadCrc: return "bad_crc";
    }
    return "unknown";
}

size_t encode(const FrameHeader& h, std::span<const std::byte> payload, std::span<std::byte> out) {
    if (payload.size() > kMaxPayload) return 0;
    const size_t total = kHeaderSize + payload.size() + kCrcSize;
    if (out.size() < total) return 0;
    std::byte* p = out.data();
    put_le<uint16_t>(p + 0, kMagic);
    p[2] = static_cast<std::byte>(kVersion);
    p[3] = static_cast<std::byte>(h.apid);
    put_le<uint32_t>(p + 4, h.seq);
    put_le<uint64_t>(p + 8, h.tx_ns);
    put_le<uint16_t>(p + 16, static_cast<uint16_t>(payload.size()));
    if (!payload.empty()) std::memcpy(p + kHeaderSize, payload.data(), payload.size());
    const uint32_t crc = crc32({p, kHeaderSize + payload.size()});
    put_le<uint32_t>(p + kHeaderSize + payload.size(), crc);
    return total;
}

DecodeResult decode(std::span<const std::byte> f) {
    if (f.size() < kHeaderSize + kCrcSize) return (ParseError::TooShort);
    const std::byte* p = f.data();
    if (get_le<uint16_t>(p) != kMagic) return (ParseError::BadMagic);
    if (static_cast<uint8_t>(p[2]) != kVersion) return (ParseError::BadVersion);
    Parsed out;
    out.header.apid = static_cast<uint8_t>(p[3]);
    out.header.seq = get_le<uint32_t>(p + 4);
    out.header.tx_ns = get_le<uint64_t>(p + 8);
    out.header.payload_len = get_le<uint16_t>(p + 16);
    if (out.header.payload_len > kMaxPayload) return (ParseError::PayloadTooLarge);
    if (f.size() != kHeaderSize + out.header.payload_len + kCrcSize)
        return (ParseError::LengthMismatch);
    const size_t body = kHeaderSize + out.header.payload_len;
    if (crc32(f.first(body)) != get_le<uint32_t>(p + body)) return (ParseError::BadCrc);
    out.payload = f.subspan(kHeaderSize, out.header.payload_len);
    return out;
}

}  // namespace gs
