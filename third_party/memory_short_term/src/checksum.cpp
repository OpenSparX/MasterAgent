// checksum.cpp — CRC32/SHA256 helpers for journal integrity
#include <cstdint>
#include <string>
#include <array>

namespace sparx::memory {

class Checksum {
public:
    static uint32_t crc32(const void* data, size_t len) {
        auto* bytes = static_cast<const uint8_t*>(data);
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; ++i) {
            crc ^= bytes[i];
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
            }
        }
        return ~crc;
    }

    static uint32_t crc32(const std::string& s) {
        return crc32(s.data(), s.size());
    }

    static bool verify(const std::string& data, uint32_t expected) {
        return crc32(data) == expected;
    }
};

} // namespace sparx::memory
