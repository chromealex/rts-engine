// SHA-256 (FIPS 180-4). Used to verify downloaded pack chunks: unlike FNV it resists tampering,
// which matters because chunks may come from any mirror or peer.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rts::pack {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t n);
    std::array<uint8_t, 32> finish();

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

std::array<uint8_t, 32> sha256(const void* data, size_t n);
std::string toHex(const uint8_t* data, size_t n);

} // namespace rts::pack
