// Procedural mesh assembly (built-in shapes, terrain). No GPU code except upload().
#include "Renderer3D.h"
#include <algorithm>
#include <cmath>

namespace rts::render {

namespace {
constexpr float kPi = 3.14159265358979f;
} // namespace

// ============================== MeshBuilder ==============================

uint32_t MeshBuilder::add(float3 p, float3 n) {
    float3 wp = xf_.transformPoint(p);
    float4 wn4 = xf_ * float4{n.x, n.y, n.z, 0};
    verts_.push_back({wp, normalize({wn4.x, wn4.y, wn4.z}), color_});
    return uint32_t(verts_.size() - 1);
}
// Emits a triangle wound counter-clockwise as seen from the side `n` points to.
void MeshBuilder::face(uint32_t a, uint32_t b, uint32_t c, float3 n) {
    float4 wn4 = xf_ * float4{n.x, n.y, n.z, 0};
    float3 wn{wn4.x, wn4.y, wn4.z};
    float3 p0 = verts_[a].pos, p1 = verts_[b].pos, p2 = verts_[c].pos;
    if (dot(cross(p1 - p0, p2 - p0), wn) >= 0) addTriangle(a, b, c); else addTriangle(a, c, b);
}

MeshBuilder& MeshBuilder::box(float3 c, float3 s) {
    const float3 h = s * 0.5f;
    const float3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto n : normals) {
        float3 u = std::fabs(n.y) > 0.5f ? float3{1, 0, 0} : float3{0, 1, 0};
        float3 v = cross(n, u);
        float3 fc = c + float3{n.x * h.x, n.y * h.y, n.z * h.z};
        float3 du{u.x * h.x, u.y * h.y, u.z * h.z}, dv{v.x * h.x, v.y * h.y, v.z * h.z};
        uint32_t a = add(fc - du - dv, n), b = add(fc + du - dv, n), cc = add(fc + du + dv, n), d = add(fc - du + dv, n);
        face(a, b, cc, n); face(a, cc, d, n);
    }
    return *this;
}

MeshBuilder& MeshBuilder::quad(float3 a, float3 b, float3 c, float3 d) {
    float3 n = normalize(cross(b - a, c - a));
    uint32_t i0 = add(a, n), i1 = add(b, n), i2 = add(c, n), i3 = add(d, n);
    face(i0, i1, i2, n); face(i0, i2, i3, n);
    return *this;
}

MeshBuilder& MeshBuilder::cylinder(float3 base, float r, float hgt, int seg, bool caps, float topR) {
    if (topR < 0) topR = r;
    const float slope = (r - topR) / std::max(hgt, 1e-4f);
    for (int i = 0; i < seg; ++i) {
        float a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
        float3 n0 = normalize({std::cos(a0), slope, std::sin(a0)}), n1 = normalize({std::cos(a1), slope, std::sin(a1)});
        uint32_t b0 = add(base + float3{std::cos(a0) * r, 0, std::sin(a0) * r}, n0), b1 = add(base + float3{std::cos(a1) * r, 0, std::sin(a1) * r}, n1);
        uint32_t t0 = add(base + float3{std::cos(a0) * topR, hgt, std::sin(a0) * topR}, n0), t1 = add(base + float3{std::cos(a1) * topR, hgt, std::sin(a1) * topR}, n1);
        float3 mid = normalize(n0 + n1);
        face(b0, b1, t1, mid); face(b0, t1, t0, mid);
    }
    if (caps) {
        disc(base + float3{0, hgt, 0}, topR, seg);
        uint32_t c = add(base, {0, -1, 0});
        for (int i = 0; i < seg; ++i) {
            float a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
            uint32_t p0 = add(base + float3{std::cos(a0) * r, 0, std::sin(a0) * r}, {0, -1, 0});
            uint32_t p1 = add(base + float3{std::cos(a1) * r, 0, std::sin(a1) * r}, {0, -1, 0});
            face(c, p0, p1, {0, -1, 0});
        }
    }
    return *this;
}

MeshBuilder& MeshBuilder::cone(float3 base, float r, float hgt, int seg) {
    const float slope = r / hgt;
    for (int i = 0; i < seg; ++i) {
        float a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg, am = (a0 + a1) * 0.5f;
        float3 n0 = normalize({std::cos(a0), slope, std::sin(a0)}), n1 = normalize({std::cos(a1), slope, std::sin(a1)});
        float3 nm = normalize({std::cos(am), slope, std::sin(am)});
        uint32_t p0 = add(base + float3{std::cos(a0) * r, 0, std::sin(a0) * r}, n0);
        uint32_t p1 = add(base + float3{std::cos(a1) * r, 0, std::sin(a1) * r}, n1);
        uint32_t tip = add(base + float3{0, hgt, 0}, nm);
        face(p0, p1, tip, nm);
    }
    uint32_t c = add(base, {0, -1, 0});
    for (int i = 0; i < seg; ++i) {
        float a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
        uint32_t p0 = add(base + float3{std::cos(a0) * r, 0, std::sin(a0) * r}, {0, -1, 0});
        uint32_t p1 = add(base + float3{std::cos(a1) * r, 0, std::sin(a1) * r}, {0, -1, 0});
        face(c, p0, p1, {0, -1, 0});
    }
    return *this;
}

MeshBuilder& MeshBuilder::sphere(float3 c, float r, int rings, int seg) {
    uint32_t start = uint32_t(verts_.size());
    for (int y = 0; y <= rings; ++y) {
        float phi = float(y) / rings * kPi;
        for (int x = 0; x <= seg; ++x) {
            float th = float(x) / seg * 2 * kPi;
            float3 n{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
            add(c + n * r, n);
        }
    }
    for (int y = 0; y < rings; ++y)
        for (int x = 0; x < seg; ++x) {
            uint32_t a = start + uint32_t(y * (seg + 1) + x), b = a + uint32_t(seg + 1);
            float3 n = normalize(verts_[a].nrm + verts_[b].nrm + verts_[a + 1].nrm);
            if (y != 0) face(a, b, a + 1, n);
            if (y != rings - 1) face(a + 1, b, b + 1, normalize(verts_[a + 1].nrm + verts_[b].nrm + verts_[b + 1].nrm));
        }
    return *this;
}

MeshBuilder& MeshBuilder::disc(float3 c, float r, int seg) {
    uint32_t ci = add(c, {0, 1, 0});
    for (int i = 0; i < seg; ++i) {
        float a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
        uint32_t p0 = add(c + float3{std::cos(a0) * r, 0, std::sin(a0) * r}, {0, 1, 0});
        uint32_t p1 = add(c + float3{std::cos(a1) * r, 0, std::sin(a1) * r}, {0, 1, 0});
        face(ci, p0, p1, {0, 1, 0});
    }
    return *this;
}

Mesh MeshBuilder::upload(gfx::Device& dev) const {
    Mesh m;
    m.boundsMin = {1e9f, 1e9f, 1e9f}; m.boundsMax = {-1e9f, -1e9f, -1e9f};
    for (auto& v : verts_) {
        m.boundsMin = {std::min(m.boundsMin.x, v.pos.x), std::min(m.boundsMin.y, v.pos.y), std::min(m.boundsMin.z, v.pos.z)};
        m.boundsMax = {std::max(m.boundsMax.x, v.pos.x), std::max(m.boundsMax.y, v.pos.y), std::max(m.boundsMax.z, v.pos.z)};
    }
    gfx::BufferDesc vb; vb.type = gfx::BufferType::Vertex; vb.size = verts_.size() * sizeof(MeshVertex); vb.data = verts_.data();
    m.vb = dev.createBuffer(vb);
    // Indices are always uploaded as 32-bit: GLES3/WebGL2/Metal all support them and it keeps one pipeline.
    gfx::BufferDesc ib; ib.type = gfx::BufferType::Index; ib.size = idx_.size() * sizeof(uint32_t); ib.data = idx_.data();
    m.ib = dev.createBuffer(ib);
    m.indexCount = uint32_t(idx_.size());
    return m;
}

} // namespace rts::render
