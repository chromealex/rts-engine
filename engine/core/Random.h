// Deterministic PRNG (PCG32). Never use std::rand / std::mt19937 distributions in the
// simulation: distribution implementations differ between standard libraries.
#pragma once
#include <cstdint>
#include "Fixed.h"

namespace rts {

class Random {
public:
    explicit Random(uint64_t seed = 0x853c49e6748fea9bull, uint64_t stream = 0xda3e39cb94b95bdbull) { reseed(seed, stream); }

    void reseed(uint64_t seed, uint64_t stream = 0xda3e39cb94b95bdbull) {
        state_ = 0; inc_ = (stream << 1u) | 1u;
        next(); state_ += seed; next();
    }

    uint32_t next() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ull + inc_;
        uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // Uniform in [0, bound) without modulo bias.
    uint32_t below(uint32_t bound) {
        if (bound == 0) return 0;
        uint32_t threshold = (0u - bound) % bound;
        for (;;) { uint32_t r = next(); if (r >= threshold) return r % bound; }
    }
    int32_t range(int32_t lo, int32_t hiInclusive) {
        return lo + static_cast<int32_t>(below(static_cast<uint32_t>(hiInclusive - lo + 1)));
    }
    // Uniform Fixed in [0, 1)
    Fixed unit() { return Fixed::fromRaw(static_cast<int64_t>(next() >> (32 - Fixed::kFracBits))); }

    uint64_t state() const { return state_; }
    uint64_t inc() const { return inc_; }

private:
    uint64_t state_ = 0, inc_ = 0;
};

} // namespace rts
