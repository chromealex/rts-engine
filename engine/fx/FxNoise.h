// Procedural helpers of the particle system (internal to engine/fx): value noise for the "noise"
// module and the built-in texture atlas (soft | spark | ring | smoke), generated at start-up.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../render/Math3D.h"

namespace rts::fx::detail {

using render::float3;
constexpr int kAtlasCell = 64, kAtlasCells = 4;

inline float hash2(int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
inline float valueNoise(float x, float y) {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - float(xi), fy = y - float(yi);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi), b = hash2(xi + 1, yi), c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// 3D value noise in -1..1 per axis ("noise" module): coherent, cheap, no tables.
inline float hash3(int x, int y, int z) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(z) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
inline float valueNoise3(float x, float y, float z) {
    int xi = int(std::floor(x)), yi = int(std::floor(y)), zi = int(std::floor(z));
    float fx = x - float(xi), fy = y - float(yi), fz = z - float(zi);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy); fz = fz * fz * (3 - 2 * fz);
    auto layer = [&](int zz) {
        float a = hash3(xi, yi, zz), b = hash3(xi + 1, yi, zz), c = hash3(xi, yi + 1, zz), d = hash3(xi + 1, yi + 1, zz);
        float ab = a + (b - a) * fx, cd = c + (d - c) * fx;
        return ab + (cd - ab) * fy;
    };
    float l0 = layer(zi), l1 = layer(zi + 1);
    return (l0 + (l1 - l0) * fz) * 2 - 1;
}
inline float3 noise3(float3 p) {
    return {valueNoise3(p.x, p.y, p.z), valueNoise3(p.x + 31.4f, p.y + 17.2f, p.z + 5.9f), valueNoise3(p.x + 11.7f, p.y + 43.1f, p.z + 23.3f)};
}

// 256x64 RGBA: soft | spark | ring | smoke. White RGB, shape in alpha (tinted by the particle colour).
inline std::vector<uint8_t> makeBuiltinAtlas() {
    const int W = kAtlasCell * kAtlasCells, H = kAtlasCell;
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    for (int cell = 0; cell < kAtlasCells; ++cell)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < kAtlasCell; ++x) {
                float u = (float(x) + 0.5f) / kAtlasCell * 2 - 1, v = (float(y) + 0.5f) / kAtlasCell * 2 - 1;
                float r = std::sqrt(u * u + v * v);
                float a = 0, l = 1;
                if (cell == 0) { float t = std::clamp(1 - r, 0.0f, 1.0f); a = t * t * (3 - 2 * t); }
                else if (cell == 1) a = std::exp(-r * r * 14) + 0.3f * std::exp(-r * r * 4);
                else if (cell == 2) { float d = (r - 0.72f) / 0.13f; a = std::exp(-d * d); }
                else {
                    float n = 0, amp = 0.5f, f = 3;
                    for (int o = 0; o < 4; ++o, amp *= 0.5f, f *= 2.1f) n += amp * valueNoise(u * f + 11.3f, v * f + 7.1f);
                    float t = std::clamp(1 - r, 0.0f, 1.0f);
                    a = t * t * (3 - 2 * t) * std::clamp(0.35f + n * 1.1f, 0.0f, 1.0f);
                    l = 0.78f + 0.22f * n;
                }
                if (r >= 0.98f) a = 0;   // empty border: no bleeding into the neighbour cell
                uint8_t* p = &px[(size_t(y) * W + size_t(cell * kAtlasCell + x)) * 4];
                p[0] = p[1] = p[2] = uint8_t(std::clamp(l, 0.0f, 1.0f) * 255 + 0.5f);
                p[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255 + 0.5f);
            }
    return px;
}

} // namespace rts::fx::detail
