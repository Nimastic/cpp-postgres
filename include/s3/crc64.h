#pragma once

#include <cstdint>
#include <cstddef>
#include <array>

namespace s3 {

// Standard CRC64-NVME / ISO polynomial
class CRC64 {
public:
    static constexpr uint64_t POLYNOMIAL = 0x9A6C9329AC4BC9B5ULL;

    static consteval std::array<uint64_t, 256> generate_table() {
        std::array<uint64_t, 256> tbl{};
        for (uint64_t i = 0; i < 256; ++i) {
            uint64_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ POLYNOMIAL;
                } else {
                    crc >>= 1;
                }
            }
            tbl[i] = crc;
        }
        return tbl;
    }

    static inline uint64_t calculate(const void* data, size_t len, uint64_t init = 0) {
        static constexpr auto table = generate_table();
        const auto* bytes = static_cast<const uint8_t*>(data);
        uint64_t crc = ~init;
        for (size_t i = 0; i < len; ++i) {
            uint8_t idx = static_cast<uint8_t>(crc ^ bytes[i]);
            crc = (crc >> 8) ^ table[idx];
        }
        return ~crc;
    }
};

inline uint64_t crc64_nvme(const void* data, size_t len, uint64_t init = 0) {
    return CRC64::calculate(data, len, init);
}

} // namespace s3
