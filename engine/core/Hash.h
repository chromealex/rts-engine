// Incremental 64-bit FNV-1a used for simulation checksums (desync detection, replay verification).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>
#include "Fixed.h"

namespace rts {

class Hasher {
public:
    void bytes(const void* data, size_t n) {
        auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { h_ ^= p[i]; h_ *= 0x100000001b3ull; }
    }
    // Feed integers byte-by-byte in little-endian order so the hash is endian-independent.
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) { uint8_t b = uint8_t(v >> (i * 8)); bytes(&b, 1); } }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) { uint8_t b = uint8_t(v >> (i * 8)); bytes(&b, 1); } }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void fx(Fixed f) { i64(f.raw); }
    void v3(const Vec3& v) { fx(v.x); fx(v.y); fx(v.z); }
    void str(std::string_view s) { u32(uint32_t(s.size())); bytes(s.data(), s.size()); }
    uint64_t value() const { return h_; }

private:
    uint64_t h_ = 0xcbf29ce484222325ull;
};

} // namespace rts
