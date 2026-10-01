#pragma once
// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) with a compile-time table.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gs {

namespace detail {
constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
inline constexpr auto kCrcTable = make_crc_table();
}  // namespace detail

constexpr uint32_t crc32(std::span<const std::byte> data, uint32_t seed = 0) {
    uint32_t c = ~seed;
    for (std::byte b : data) c = detail::kCrcTable[(c ^ static_cast<uint8_t>(b)) & 0xFF] ^ (c >> 8);
    return ~c;
}

}  // namespace gs
