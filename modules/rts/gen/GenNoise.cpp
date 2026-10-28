#include "GenNoise.h"

namespace rts::gen {

uint32_t hash32(uint32_t a) {
    a ^= a >> 16; a *= 0x7feb352du; a ^= a >> 15; a *= 0x846ca68bu; a ^= a >> 16;
    return a;
}

uint64_t mix64(uint64_t z) {
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

uint32_t hashCoords(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = hash32(seed ^ 0x9e3779b9u);
    h = hash32(h ^ (uint32_t(x) * 0x27d4eb2du));
    h = hash32(h ^ (uint32_t(y) * 0x165667b1u));
    h = hash32(h ^ (uint32_t(z) * 0xd3a2646cu));
    return h;
}

namespace {
inline int64_t fade(int64_t t) {                 // 6t^5 - 15t^4 + 10t^3, t in [0, 1] (Q16)
    const int64_t t3 = ((t * t) >> 16) * t >> 16;
    const int64_t inner = ((t * (6 * t - 15 * kOne)) >> 16) + 10 * kOne;
    return (t3 * inner) >> 16;
}
inline int64_t lerp(int64_t a, int64_t b, int64_t t) { return a + (((b - a) * t) >> 16); }
inline int32_t clampQ(int64_t v) { return int32_t(v < -kOne ? -kOne : v > kOne ? kOne : v); }
constexpr int64_t kInvSqrt2 = 46341;              // 1/sqrt(2) in Q16

inline int64_t grad2(uint32_t h, int64_t x, int64_t y) {
    switch (h & 7u) {
        case 0: return x;
        case 1: return -x;
        case 2: return y;
        case 3: return -y;
        case 4: return ((x + y) * kInvSqrt2) >> 16;
        case 5: return ((-x + y) * kInvSqrt2) >> 16;
        case 6: return ((x - y) * kInvSqrt2) >> 16;
        default: return ((-x - y) * kInvSqrt2) >> 16;
    }
}
inline int64_t grad3(uint32_t h, int64_t x, int64_t y, int64_t z) {
    int64_t r;
    switch (h % 12u) {
        case 0: r = x + y; break;   case 1: r = -x + y; break;  case 2: r = x - y; break;   case 3: r = -x - y; break;
        case 4: r = x + z; break;   case 5: r = -x + z; break;  case 6: r = x - z; break;   case 7: r = -x - z; break;
        case 8: r = y + z; break;   case 9: r = -y + z; break;  case 10: r = y - z; break;  default: r = -y - z; break;
    }
    return (r * kInvSqrt2) >> 16;
}
} // namespace

int32_t valueNoise2(int64_t x, int64_t y, uint32_t seed) {
    const int32_t ix = int32_t(x >> 16), iy = int32_t(y >> 16);
    const int64_t u = fade(x & 0xffff), v = fade(y & 0xffff);
    auto val = [&](int32_t a, int32_t b) { return int64_t(hashCoords(a, b, 0, seed) >> 15) - kOne; };
    return clampQ(lerp(lerp(val(ix, iy), val(ix + 1, iy), u), lerp(val(ix, iy + 1), val(ix + 1, iy + 1), u), v));
}

int32_t gradNoise2(int64_t x, int64_t y, uint32_t seed) {
    const int32_t ix = int32_t(x >> 16), iy = int32_t(y >> 16);
    const int64_t fx = x & 0xffff, fy = y & 0xffff;
    const int64_t u = fade(fx), v = fade(fy);
    const int64_t n00 = grad2(hashCoords(ix, iy, 0, seed), fx, fy);
    const int64_t n10 = grad2(hashCoords(ix + 1, iy, 0, seed), fx - kOne, fy);
    const int64_t n01 = grad2(hashCoords(ix, iy + 1, 0, seed), fx, fy - kOne);
    const int64_t n11 = grad2(hashCoords(ix + 1, iy + 1, 0, seed), fx - kOne, fy - kOne);
    const int64_t r = lerp(lerp(n00, n10, u), lerp(n01, n11, u), v);
    return clampQ((r * 92682) >> 16);              // gradient noise peaks near +-0.71: scale by sqrt(2)
}

int32_t gradNoise3(int64_t x, int64_t y, int64_t z, uint32_t seed) {
    const int32_t ix = int32_t(x >> 16), iy = int32_t(y >> 16), iz = int32_t(z >> 16);
    const int64_t fx = x & 0xffff, fy = y & 0xffff, fz = z & 0xffff;
    const int64_t u = fade(fx), v = fade(fy), w = fade(fz);
    int64_t c[2][2][2];
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                c[dz][dy][dx] = grad3(hashCoords(ix + dx, iy + dy, iz + dz, seed), fx - dx * kOne, fy - dy * kOne, fz - dz * kOne);
    const int64_t a = lerp(lerp(c[0][0][0], c[0][0][1], u), lerp(c[0][1][0], c[0][1][1], u), v);
    const int64_t b = lerp(lerp(c[1][0][0], c[1][0][1], u), lerp(c[1][1][0], c[1][1][1], u), v);
    return clampQ((lerp(a, b, w) * 80000) >> 16);
}

namespace {
inline int64_t ridge(int64_t n, int32_t mixQ) {
    if (mixQ <= 0) return n;
    int64_t r = kOne - (n < 0 ? -n : n);
    r = ((r * r) >> 16) * 2 - kOne;
    return n + (((r - n) * mixQ) >> 16);
}
} // namespace

int32_t fbm2(int64_t x, int64_t y, uint32_t seed, const Fbm& p) {
    if (p.warp > 0) {
        Fbm q; q.octaves = 3; q.value = p.value;
        const int64_t wx = fbm2(x + 340787, y + 85197, seed ^ 0x51ed27u, q);
        const int64_t wy = fbm2(x + 128451, y + 612399, seed ^ 0x2c1b3cu, q);
        x += (wx * p.warp) >> 16; y += (wy * p.warp) >> 16;
    }
    int64_t sum = 0, amp = kOne, norm = 0;
    for (int o = 0; o < p.octaves; ++o) {
        const uint32_t s = seed + uint32_t(o) * 0x9e3779b9u;
        const int64_t n = ridge(p.value ? valueNoise2(x, y, s) : gradNoise2(x, y, s), p.ridged);
        sum += (n * amp) >> 16; norm += amp;
        amp = (amp * p.gain) >> 16;
        x *= 2; y *= 2;
        if (amp <= 0) break;
    }
    return clampQ(norm > 0 ? sum * kOne / norm : 0);
}

int32_t fbm3(int64_t x, int64_t y, int64_t z, uint32_t seed, const Fbm& p) {
    if (p.warp > 0) {
        Fbm q; q.octaves = 3;
        const int64_t wx = fbm3(x + 340787, y + 85197, z + 22113, seed ^ 0x51ed27u, q);
        const int64_t wy = fbm3(x + 128451, y + 612399, z + 90001, seed ^ 0x2c1b3cu, q);
        const int64_t wz = fbm3(x + 777013, y + 31337, z + 450071, seed ^ 0x7a11u, q);
        x += (wx * p.warp) >> 16; y += (wy * p.warp) >> 16; z += (wz * p.warp) >> 16;
    }
    int64_t sum = 0, amp = kOne, norm = 0;
    for (int o = 0; o < p.octaves; ++o) {
        const uint32_t s = seed + uint32_t(o) * 0x9e3779b9u;
        const int64_t n = ridge(gradNoise3(x, y, z, s), p.ridged);
        sum += (n * amp) >> 16; norm += amp;
        amp = (amp * p.gain) >> 16;
        x *= 2; y *= 2; z *= 2;
        if (amp <= 0) break;
    }
    return clampQ(norm > 0 ? sum * kOne / norm : 0);
}

int32_t smoothstepQ(int32_t e0, int32_t e1, int32_t x) {
    if (e1 == e0) return x < e0 ? 0 : kOne;
    int64_t t = (int64_t(x) - e0) * kOne / (int64_t(e1) - e0);
    if (t < 0) t = 0;
    if (t > kOne) t = kOne;
    return int32_t((((t * t) >> 16) * (3 * kOne - 2 * t)) >> 16);
}

int64_t isqrt(int64_t v) {
    if (v <= 0) return 0;
    int64_t r = 0, bit = int64_t(1) << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

} // namespace rts::gen
