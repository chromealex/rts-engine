// Deterministic integer noise for the terrain generator (docs/game-format.md "Map generation").
// Every value is Q16 fixed point in plain integers (65536 = 1.0): no float anywhere, so the same seed gives
// bit-identical terrain on every CPU / compiler / WASM, which lockstep match-time generation requires.
#pragma once
#include <cstdint>

namespace rts::gen {

constexpr int32_t kOne = 65536;

uint32_t hash32(uint32_t a);                                   // avalanche mix
uint64_t mix64(uint64_t a);
uint32_t hashCoords(int32_t x, int32_t y, int32_t z, uint32_t seed);

// Single octave, input coordinates Q16, output roughly [-65536, 65536] (clamped).
int32_t valueNoise2(int64_t x, int64_t y, uint32_t seed);
int32_t gradNoise2(int64_t x, int64_t y, uint32_t seed);
int32_t gradNoise3(int64_t x, int64_t y, int64_t z, uint32_t seed);

struct Fbm {
    int octaves = 5;
    int32_t gain = 32768;     // amplitude factor per octave (Q16)
    int32_t ridged = 0;       // 0..65536: mix of ridged noise (1 - |n|)^2 into every octave
    int32_t warp = 0;         // domain warp strength in input units (Q16 of coordinates, i.e. lattice cells)
    bool value = false;       // value noise instead of gradient noise (blockier)
};
// Fractal sums, normalized back to about [-65536, 65536].
int32_t fbm2(int64_t x, int64_t y, uint32_t seed, const Fbm& p);
int32_t fbm3(int64_t x, int64_t y, int64_t z, uint32_t seed, const Fbm& p);

// Q16 helpers
inline int64_t qmul(int64_t a, int64_t b) { return (a * b) >> 16; }
int32_t smoothstepQ(int32_t e0, int32_t e1, int32_t x);       // 0..65536
int64_t isqrt(int64_t v);                                      // floor(sqrt(v))

} // namespace rts::gen
