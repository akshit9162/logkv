#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace logkv {

// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — the same variant used by
// zlib and gzip. Table is built at compile time so there is no init order or
// thread-safety question at startup.
namespace detail {

consteval std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        t[i] = c;
    }
    return t;
}

inline constexpr auto kCrcTable = make_crc_table();

}  // namespace detail

[[nodiscard]] inline std::uint32_t crc32(const void* data, std::size_t len,
                                         std::uint32_t seed = 0) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t c = seed ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = detail::kCrcTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

}  // namespace logkv
