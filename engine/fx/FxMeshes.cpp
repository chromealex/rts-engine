// Built-in meshes for mesh particles and the "mesh" shape: Unity's primitives in engine model space
// (Y up, right-handed, same sizes as Unity: cube 1, sphere d 1, capsule / cylinder d 1 h 2, plane 10, quad 1).
// Low polygon on purpose: every mesh particle is expanded on the CPU.
#include <cmath>
#include "FxInternal.h"

namespace rts::fx::detail {

namespace {

void add(FxMesh& m, float3 p, float3 n, float2 uv) { m.pos.push_back(p); m.nrm.push_back(n); m.uv.push_back(uv); }
void quadFace(FxMesh& m, float3 c, float3 u, float3 v, float3 n) {   // centre, half axes (u x v = n)
    const uint32_t b = uint32_t(m.pos.size());
    add(m, c - u - v, n, {0, 1}); add(m, c + u - v, n, {1, 1}); add(m, c + u + v, n, {1, 0}); add(m, c - u + v, n, {0, 0});
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.idx.push_back(b + i);
}

// Surface of revolution around Y: profile points (radius, y, normal xz-scale, normal y), segments around.
void lathe(FxMesh& m, const std::vector<float4>& profile, int segs) {
    const uint32_t base = uint32_t(m.pos.size());
    const int rows = int(profile.size());
    for (int r = 0; r < rows; ++r)
        for (int s = 0; s <= segs; ++s) {
            const float a = float(s) / float(segs) * 2 * kPi, ca = std::cos(a), sa = std::sin(a);
            const float4 pr = profile[size_t(r)];
            add(m, {pr.x * ca, pr.y, pr.x * sa}, render::normalize(float3{pr.z * ca, pr.w, pr.z * sa}), {float(s) / float(segs), float(r) / float(rows - 1)});
        }
    for (int r = 0; r + 1 < rows; ++r)
        for (int s = 0; s < segs; ++s) {
            const uint32_t i0 = base + uint32_t(r * (segs + 1) + s), i1 = i0 + 1, i2 = i0 + uint32_t(segs + 1), i3 = i2 + 1;
            for (uint32_t i : {i0, i2, i1, i1, i2, i3}) m.idx.push_back(i);
        }
}

void disc(FxMesh& m, float y, float radius, float ny, int segs) {
    const uint32_t c = uint32_t(m.pos.size());
    add(m, {0, y, 0}, {0, ny, 0}, {0.5f, 0.5f});
    for (int s = 0; s <= segs; ++s) {
        const float a = float(s) / float(segs) * 2 * kPi;
        add(m, {radius * std::cos(a), y, radius * std::sin(a)}, {0, ny, 0}, {0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)});
    }
    for (int s = 0; s < segs; ++s) {
        const uint32_t a = c + 1 + uint32_t(s), b = a + 1;
        if (ny > 0) { m.idx.push_back(c); m.idx.push_back(b); m.idx.push_back(a); }
        else { m.idx.push_back(c); m.idx.push_back(a); m.idx.push_back(b); }
    }
}

} // namespace

bool builtinMesh(const std::string& name, FxMesh& m) {
    m = {};
    if (name == "cube") {
        const float h = 0.5f;
        quadFace(m, {h, 0, 0}, {0, 0, -h}, {0, h, 0}, {1, 0, 0});
        quadFace(m, {-h, 0, 0}, {0, 0, h}, {0, h, 0}, {-1, 0, 0});
        quadFace(m, {0, h, 0}, {h, 0, 0}, {0, 0, -h}, {0, 1, 0});
        quadFace(m, {0, -h, 0}, {h, 0, 0}, {0, 0, h}, {0, -1, 0});
        quadFace(m, {0, 0, h}, {h, 0, 0}, {0, h, 0}, {0, 0, 1});
        quadFace(m, {0, 0, -h}, {-h, 0, 0}, {0, h, 0}, {0, 0, -1});
        return true;
    }
    if (name == "quad") { quadFace(m, {0, 0, 0}, {0.5f, 0, 0}, {0, 0.5f, 0}, {0, 0, 1}); return true; }
    if (name == "plane") { quadFace(m, {0, 0, 0}, {5, 0, 0}, {0, 0, -5}, {0, 1, 0}); return true; }
    if (name == "sphere") {
        std::vector<float4> prof;
        const int rings = 8;
        for (int r = 0; r <= rings; ++r) {
            const float a = -kPi * 0.5f + kPi * float(r) / float(rings);
            prof.push_back({0.5f * std::cos(a), 0.5f * std::sin(a), std::cos(a), std::sin(a)});
        }
        lathe(m, prof, 12);
        return true;
    }
    if (name == "capsule") {
        std::vector<float4> prof;
        const int half = 4;
        for (int r = 0; r <= half; ++r) {   // bottom hemisphere
            const float a = -kPi * 0.5f + kPi * 0.5f * float(r) / float(half);
            prof.push_back({0.5f * std::cos(a), -0.5f + 0.5f * std::sin(a), std::cos(a), std::sin(a)});
        }
        for (int r = 0; r <= half; ++r) {   // top hemisphere
            const float a = kPi * 0.5f * float(r) / float(half);
            prof.push_back({0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a), std::cos(a), std::sin(a)});
        }
        lathe(m, prof, 12);
        return true;
    }
    if (name == "cylinder") {
        lathe(m, {{0.5f, -1, 1, 0}, {0.5f, 1, 1, 0}}, 16);
        disc(m, 1, 0.5f, 1, 16);
        disc(m, -1, 0.5f, -1, 16);
        return true;
    }
    return false;
}

} // namespace rts::fx::detail
