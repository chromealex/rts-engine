// Planet renderer (see RtsPlanetRender.h).
#include "RtsPlanetRender.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../../engine/platform/Platform.h"
#include "../RtsMap.h"
#include "../RtsSphere.h"
#include "RtsPlanetShaders.h"

namespace rts::game {

using namespace render;

namespace {
constexpr int kGrid = 16;               // quads per chunk side
constexpr float kPi = 3.14159265f;

uint32_t hashCell(int f, int x, int y) { uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(f) * 83492791u; h ^= h >> 13; h *= 0x5bd1e995u; return h ^ (h >> 15); }

struct PU0 {
    float model[16];
    float morph[4], body[4], opts[4], scale[4], tint[4][4], cliff[4], cliffTint[4], water[4], fog[4], fogColor[4], atmo[4], sun[4];
};
struct SU0 { float inv[16]; float eye[4], star[4], starColor[4]; };
struct AU0 { float model[16]; float body[4], shell[4], color[4], sun[4]; };
void put(float* d, float3 v, float w) { d[0] = v.x; d[1] = v.y; d[2] = v.z; d[3] = w; }

// Kind of a planet cell from its flags (CellKind of the flat terrain; no levels / dig on planets).
int kindOfFlags(uint8_t f) {
    if (!(f & kCellGround) && (f & kCellWater)) return kKindLakebed;
    if (!(f & kCellGround)) return kKindBlocked;
    if (f & kCellRock) return kKindRock;
    if (f & kCellRamp) return kKindRamp;
    if (f & kCellWater) return kKindShallows;
    if (f & kCellNoBuild) return kKindSand;
    return kKindGrass;
}
} // namespace

void PlanetRenderer::init(gfx::Device& dev, const TerrainLook& look, const FileReader& read, const std::string& contentBase) {
    if (dev_) shutdown(*dev_);
    dev_ = &dev;
    look_ = look;
    read_ = read;
    base_ = contentBase;
    textures_.init(dev);
    texturesLoaded_ = false;
    auto tex1 = [&](uint32_t px, const char* label) {
        gfx::TextureDesc td; td.width = td.height = 1; td.data = &px; td.repeat = true; td.label = label;
        return dev.createTexture(td);
    };
    white_ = tex1(0xffffffffu, "planet white");
    gray_ = tex1(0xff808080u, "planet gray");
    black_ = tex1(0x00000000u, "planet black");
    MeshBuilder mb;
    mb.color(0xffffffffu).sphere({0, 0, 0}, 1.0f, 24, 40);
    sphere_ = mb.upload(dev);
}

void PlanetRenderer::shutdown(gfx::Device& dev) {
    for (auto& b : bodies_) {
        for (auto& c : b.chunks) freeChunk(dev, c);
        if (b.vision.valid()) dev.destroyTexture(b.vision);
    }
    bodies_.clear();
    world_ = {};
    for (Mesh* m : {&sphere_, &atmoSphere_}) { dev.destroyBuffer(m->vb); dev.destroyBuffer(m->ib); *m = {}; }
    for (auto t : {white_, gray_, black_}) if (t.valid()) dev.destroyTexture(t);
    white_ = gray_ = black_ = {};
    for (int i = 0; i < 3; ++i) {
        for (auto& p : pipes_[i]) { if (p.valid()) dev.destroyPipeline(p); p = {}; }
        if (shaders_[i].valid()) dev.destroyShader(shaders_[i]);
        shaders_[i] = {}; shaderFailed_[i] = false;
    }
    for (auto p : {skyPipe_, atmoPipe_}) if (p.valid()) dev.destroyPipeline(p);
    for (auto s : {skyShader_, atmoShader_}) if (s.valid()) dev.destroyShader(s);
    skyPipe_ = atmoPipe_ = {}; skyShader_ = atmoShader_ = {};
    textures_.shutdown();
    dev_ = nullptr;
}

void PlanetRenderer::freeChunk(gfx::Device& dev, Chunk& c) {
    dev.destroyBuffer(c.vb); dev.destroyBuffer(c.ib);
    c.vb = {}; c.ib = {}; c.indexCount = 0; c.built = false;
}

void PlanetRenderer::setWorld(const PlanetWorld& w) {
    if (dev_) for (auto& b : bodies_) { for (auto& c : b.chunks) freeChunk(*dev_, c); if (b.vision.valid()) dev_->destroyTexture(b.vision); }
    bodies_.clear();
    world_ = w;
    bodies_.resize(world_.bodies.size());
    for (size_t i = 0; i < world_.bodies.size(); ++i) {
        const PlanetBody& pb = world_.bodies[i];
        if (!pb.surface()) continue;
        Body& b = bodies_[i];
        b.surf = std::make_unique<PlanetSurface>();
        b.surf->init(pb);
        const int N = pb.faceSize;
        // levels: root chunk covers the face; leaf chunks are kGrid cells (one quad per cell)
        b.levels = 1;
        while ((kGrid << (b.levels - 1)) < N) ++b.levels;
        for (int f = 0; f < 6; ++f) { Chunk c; c.face = f; c.size = N; c.level = b.levels - 1; b.chunks.push_back(c); }
        for (int f = 0; f < 6; ++f) buildTree(b, f);
    }
}

// Quadtree topology + bounds (cheap: corner samples only); meshes are built lazily.
void PlanetRenderer::buildTree(Body& b, int idx) {
    computeBounds(b, b.chunks[size_t(idx)]);
    {
        const Chunk& c = b.chunks[size_t(idx)];
        if (c.level == 0 || c.size <= kGrid) return;
    }
    const int first = int(b.chunks.size());
    const Chunk parent = b.chunks[size_t(idx)];
    const int half = parent.size / 2;
    for (int k = 0; k < 4; ++k) {
        Chunk ch;
        ch.face = parent.face; ch.size = half; ch.level = parent.level - 1;
        ch.x0 = parent.x0 + (k & 1) * half; ch.y0 = parent.y0 + (k >> 1) * half;
        b.chunks.push_back(ch);
    }
    b.chunks[size_t(idx)].child = first;
    for (int k = 0; k < 4; ++k) buildTree(b, first + k);
}

void PlanetRenderer::computeBounds(Body& b, Chunk& c) {
    const PlanetSurface& s = *b.surf;
    {
        const int x1 = std::min(c.x0 + c.size, s.faceSize()), y1 = std::min(c.y0 + c.size, s.faceSize());
        float3 sum{};
        int cnt = 0;
        float minR = 1e9f, maxR = -1e9f;
        const int step = std::max(1, (x1 - c.x0) / 8);
        for (int j = c.y0; j <= y1; j += step)
            for (int i = c.x0; i <= x1; i += step) {
                sum += s.cornerDir(c.face, i, j); ++cnt;
            }
        // radial range over every corner (heights decide horizon culling)
        for (int j = c.y0; j <= y1; ++j)
            for (int i = c.x0; i <= x1; ++i) { float h = s.cornerHeight(c.face, i, j); minR = std::min(minR, h); maxR = std::max(maxR, h); }
        c.dir = normalize(sum);
        float cosA = 1;
        for (int j : {c.y0, y1}) for (int i : {c.x0, x1}) cosA = std::min(cosA, dot(c.dir, s.cornerDir(c.face, i, j)));
        for (int j = c.y0; j <= y1; j += step) for (int i = c.x0; i <= x1; i += step) cosA = std::min(cosA, dot(c.dir, s.cornerDir(c.face, i, j)));
        c.cosAngle = cosA;
        c.minR = s.radius() + minR - 1.0f;   // skirts
        c.maxR = s.radius() + maxR;
        c.center = c.dir * (0.5f * (c.minR + c.maxR));
        const float sinA = std::sqrt(std::max(0.0f, 1 - cosA * cosA));
        c.boundR = std::max(c.maxR * sinA, 0.0f) + (c.maxR - c.minR * cosA) + 0.5f;
    }
}

float3 PlanetRenderer::cornerColor(const PlanetBody& pb, const PlanetSurface& s, int face, int i, int j, uint32_t& splat) const {
    const int N = s.faceSize();
    float3 sum{};
    float cnt = 0;
    float w[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; ++k) {
        const int x = i - 1 + (k & 1), y = j - 1 + (k >> 1);
        if (x < 0 || y < 0 || x >= N || y >= N) continue;
        const int kind = kindOfFlags(s.cellFlags(face, x, y));
        const float n = float(hashCell(face, x, y) & 255) / 255.0f;
        float3 c;
        if (look_.hasColor[kind]) c = look_.color[kind] * (0.96f + n * 0.08f);
        else switch (kind) {
            case kKindLakebed: c = {0.32f, 0.40f, 0.30f}; break;
            case kKindBlocked: c = {0.47f + n * 0.05f, 0.45f + n * 0.05f, 0.42f}; break;
            case kKindRock: c = {0.56f + n * 0.04f, 0.50f + n * 0.04f, 0.44f}; break;
            case kKindRamp: c = {0.55f + n * 0.03f, 0.50f, 0.36f}; break;
            case kKindShallows: c = {0.42f, 0.55f, 0.50f}; break;
            case kKindSand: c = {0.60f + n * 0.04f, 0.52f, 0.38f}; break;
            default: c = {0.33f + n * 0.05f, 0.52f + n * 0.06f, 0.24f + n * 0.03f}; break;
        }
        if (pb.kind == PlanetBody::Kind::Moon && !look_.hasColor[kind]) { float g = (c.x + c.y + c.z) / 3.0f; c = float3{g, g, g} * 1.05f; }
        sum += c; cnt += 1;
        w[std::clamp(look_.cellLayer[kind], 0, 3)] += 1;
    }
    if (cnt == 0) { // a cube corner seen from a face with no cell at (i, j): use the neighbour cell
        const int x = std::clamp(i, 0, N - 1), y = std::clamp(j, 0, N - 1);
        uint32_t sp;
        (void)sp;
        const int kind = kindOfFlags(s.cellFlags(face, x, y));
        sum = {0.4f, 0.48f, 0.3f}; cnt = 1; w[std::clamp(look_.cellLayer[kind], 0, 3)] = 1;
    }
    auto q = [&](float v) { return uint32_t(std::clamp(v / cnt, 0.0f, 1.0f) * 255.0f + 0.5f); };
    splat = q(w[0]) | (q(w[1]) << 8) | (q(w[2]) << 16) | (q(w[3]) << 24);
    return sum * (1.0f / cnt);
}

void PlanetRenderer::buildChunk(gfx::Device& dev, const PlanetBody& pb, Body& b, Chunk& c) {
    const PlanetSurface& s = *b.surf;
    const int N = s.faceSize();
    const int step = std::max(1, c.size / kGrid);
    const int x1 = std::min(c.x0 + c.size, N), y1 = std::min(c.y0 + c.size, N);
    std::vector<int> xs, ys;
    for (int x = c.x0; x < x1; x += step) xs.push_back(x);
    xs.push_back(x1);
    for (int y = c.y0; y < y1; y += step) ys.push_back(y);
    ys.push_back(y1);
    const int nx = int(xs.size()), ny = int(ys.size());
    const float3 origin = c.center;   // chunk-relative positions keep float precision on big bodies
    // fog-of-war atlas: faces in a 3 x 2 grid of N x N cells
    auto atlasUV = [&](int face, float a, float bb) { return std::array<float, 2>{(float(face % 3) * N + a) / float(3 * N), (float(face / 3) * N + bb) / float(2 * N)}; };
    auto morphPos = [&](int X, int Y) {
        // position on the next coarser grid (2 * step): odd vertices slide onto the coarse triangle
        const int S = 2 * step;
        const int X0 = c.x0 + ((X - c.x0) / S) * S, Y0 = c.y0 + ((Y - c.y0) / S) * S;
        if (X0 == X && Y0 == Y) return s.cornerPos(c.face, X, Y);
        const int X1 = std::min(X0 + S, x1), Y1 = std::min(Y0 + S, y1);
        const float fx = X1 > X0 ? float(X - X0) / float(X1 - X0) : 0.0f, fz = Y1 > Y0 ? float(Y - Y0) / float(Y1 - Y0) : 0.0f;
        const float3 p00 = s.cornerPos(c.face, X0, Y0), p10 = s.cornerPos(c.face, X1, Y0), p01 = s.cornerPos(c.face, X0, Y1), p11 = s.cornerPos(c.face, X1, Y1);
        if (fx >= fz) return p00 + (p10 - p00) * fx + (p11 - p10) * fz;
        return p00 + (p11 - p01) * fx + (p01 - p00) * fz;
    };
    std::vector<PV> v;
    std::vector<uint32_t> idx;
    v.reserve(size_t(nx * ny) + size_t(nx + ny) * 8);
    bool sea = false;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int X = xs[size_t(i)], Y = ys[size_t(j)];
            uint32_t splat = 0;
            const float3 col = cornerColor(pb, s, c.face, X, Y, splat);
            const float3 p = s.cornerPos(c.face, X, Y);
            if (s.cornerHeight(c.face, X, Y) < pb.seaLevel + 0.2f) sea = true;
            const auto uv = atlasUV(c.face, float(X), float(Y));
            PV pv;
            pv.pos = p - origin;
            pv.nrm = s.cornerNormal(c.face, X, Y);
            pv.rgba = rgbaf({col.x, col.y, col.z, 1});
            pv.morph = (step < c.size ? morphPos(X, Y) : p) - origin;
            pv.uv[0] = uv[0]; pv.uv[1] = uv[1];
            pv.splat = splat;
            v.push_back(pv);
        }
    auto vi = [&](int i, int j) { return uint32_t(j * nx + i); };
    // outward winding: face basis u x v = -n (docs table), so (a, b, d) of the flat terrain becomes CCW from outside
    for (int j = 0; j + 1 < ny; ++j)
        for (int i = 0; i + 1 < nx; ++i) {
            const uint32_t a = vi(i, j), bq = vi(i + 1, j), cq = vi(i, j + 1), d = vi(i + 1, j + 1);
            idx.insert(idx.end(), {a, d, bq, a, cq, d});
        }
    // fix winding once from the first triangle against the outward direction
    if (idx.size() >= 3) {
        const float3 p0 = v[idx[0]].pos, p1 = v[idx[1]].pos, p2 = v[idx[2]].pos;
        if (dot(cross(p1 - p0, p2 - p0), c.dir) < 0) for (size_t k = 0; k + 2 < idx.size(); k += 3) std::swap(idx[k + 1], idx[k + 2]);
    }
    // skirts: every chunk edge drops a strip towards the centre (cracks between LODs and across seams)
    const float depth = 0.4f + 0.35f * float(step);
    auto skirt = [&](uint32_t ia, uint32_t ib) {
        PV a = v[ia], bb = v[ib];
        PV a2 = a, b2 = bb;
        const float3 da = normalize(a.pos + origin), db = normalize(bb.pos + origin);
        a2.pos = a.pos - da * depth; b2.pos = bb.pos - db * depth;
        a2.morph = a.morph - da * depth; b2.morph = bb.morph - db * depth;
        const uint32_t base = uint32_t(v.size());
        v.push_back(a); v.push_back(bb); v.push_back(a2); v.push_back(b2);
        // face away from the chunk centre (seen from outside the chunk's edge)
        const float3 mid = (a.pos + bb.pos) * 0.5f;
        const float3 out = mid - (c.dir * dot(mid, c.dir));
        const float3 n = cross(bb.pos - a.pos, a2.pos - a.pos);
        if (dot(n, out) >= 0) idx.insert(idx.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
        else idx.insert(idx.end(), {base, base + 2, base + 1, base + 1, base + 2, base + 3});
    };
    for (int i = 0; i + 1 < nx; ++i) { skirt(vi(i, 0), vi(i + 1, 0)); skirt(vi(i, ny - 1), vi(i + 1, ny - 1)); }
    for (int j = 0; j + 1 < ny; ++j) { skirt(vi(0, j), vi(0, j + 1)); skirt(vi(nx - 1, j), vi(nx - 1, j + 1)); }
    c.hasSea = sea && pb.seaLevel > -1e8f;
    dev.destroyBuffer(c.vb); dev.destroyBuffer(c.ib);
    gfx::BufferDesc vb; vb.type = gfx::BufferType::Vertex; vb.size = v.size() * sizeof(PV); vb.data = v.data(); vb.label = "planet";
    c.vb = dev.createBuffer(vb);
    gfx::BufferDesc ib; ib.type = gfx::BufferType::Index; ib.size = idx.size() * sizeof(uint32_t); ib.data = idx.data(); ib.label = "planet";
    c.ib = dev.createBuffer(ib);
    c.indexCount = uint32_t(idx.size());
    c.built = true;
    ++stats_.built;
}

void PlanetRenderer::updateHeights(int body, const std::vector<int16_t>& corners) {
    if (body < 0 || size_t(body) >= bodies_.size() || !bodies_[size_t(body)].surf || !dev_) return;
    PlanetBody& pb = world_.bodies[size_t(body)];
    if (corners.size() != pb.corners.size()) return;
    Body& b = bodies_[size_t(body)];
    const int N = pb.faceSize;
    PlanetSurface::Rect rect[6];
    bool any = false;
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                const size_t k = size_t((f * (N + 1) + j) * (N + 1) + i);
                if (corners[k] == pb.corners[k]) continue;
                PlanetSurface::Rect& r = rect[f];
                if (r.x1 < r.x0) r = {i, j, i, j};
                else { r.x0 = std::min(r.x0, i); r.y0 = std::min(r.y0, j); r.x1 = std::max(r.x1, i); r.y1 = std::max(r.y1, j); }
                any = true;
            }
    if (!any) return;
    pb.corners = corners;
    // seams: a changed corner on a face edge changes its duplicates on the neighbour faces too
    CubeSphere cs(N);
    PlanetSurface::Rect grown[6];
    for (int f = 0; f < 6; ++f) grown[f] = rect[f];
    for (int f = 0; f < 6; ++f) {
        const PlanetSurface::Rect r = rect[f];
        if (r.x1 < r.x0) continue;
        for (int j = r.y0; j <= r.y1; ++j)
            for (int i = r.x0; i <= r.x1; ++i) {
                if (i != 0 && j != 0 && i != N && j != N) continue;
                int dup[3];
                const int cnt = cs.cornerDuplicates(f, i, j, dup);
                for (int d = 0; d < cnt; ++d) {
                    const int g = dup[d] / ((N + 1) * (N + 1)), jj = (dup[d] % ((N + 1) * (N + 1))) / (N + 1), ii = dup[d] % (N + 1);
                    PlanetSurface::Rect& q = grown[g];
                    if (q.x1 < q.x0) q = {ii, jj, ii, jj};
                    else { q.x0 = std::min(q.x0, ii); q.y0 = std::min(q.y0, jj); q.x1 = std::max(q.x1, ii); q.y1 = std::max(q.y1, jj); }
                }
            }
    }
    b.surf->updateHeights(pb, grown);
    // chunks over the changed corners (+1 for normals) rebuild on demand; their bounds are refreshed
    for (auto& c : b.chunks) {
        const PlanetSurface::Rect& r = grown[c.face];
        if (r.x1 < r.x0) continue;
        if (c.x0 > r.x1 + 1 || c.y0 > r.y1 + 1 || c.x0 + c.size < r.x0 - 1 || c.y0 + c.size < r.y0 - 1) continue;
        freeChunk(*dev_, c);
        computeBounds(b, c);
    }
}

void PlanetRenderer::updateFlags(int body, const std::vector<uint8_t>& flags) {
    if (body < 0 || size_t(body) >= bodies_.size() || !bodies_[size_t(body)].surf || !dev_) return;
    PlanetBody& pb = world_.bodies[size_t(body)];
    if (flags == pb.flags) return;
    pb.flags = flags;
    Body& b = bodies_[size_t(body)];
    b.surf->init(pb);   // flags live in the surface (cheap compared to the mesh rebuild)
    for (auto& c : b.chunks) freeChunk(*dev_, c);
}

void PlanetRenderer::setVision(int body, const std::vector<uint8_t>* field, const FogLook& look) {
    if (body < 0 || size_t(body) >= bodies_.size() || !bodies_[size_t(body)].surf || !dev_) return;
    Body& b = bodies_[size_t(body)];
    b.fogLook = look;
    const int N = b.surf->faceSize();
    if (!field || field->size() != size_t(6 * N * N) || !look.show) { b.fog = false; return; }
    const int W = 3 * N, H = 2 * N;
    if (!b.vision.valid()) {
        gfx::TextureDesc td; td.width = W; td.height = H; td.format = gfx::PixelFormat::R8; td.filter = gfx::Filter::Linear; td.label = "planet vision";
        b.vision = dev_->createTexture(td);
    }
    b.visionPx.resize(size_t(W * H));
    for (int f = 0; f < 6; ++f)
        for (int y = 0; y < N; ++y)
            std::memcpy(&b.visionPx[size_t((f / 3 * N + y) * W + f % 3 * N)], &(*field)[size_t((f * N + y) * N)], size_t(N));
    dev_->updateTexture(b.vision, 0, 0, W, H, b.visionPx.data(), W);
    b.fog = true;
}

int PlanetRenderer::tierFor(const Renderer3D& r) const {
    if (!look_.materials()) return 0;
    if (const char* e = std::getenv("RTS_TERRAIN")) { std::string v = e; return v == "low" ? 0 : 1; }
    auto tier = [](MaterialQuality q) { return q == MaterialQuality::Low ? 0 : 1; };
    static const int deviceTier = dev_ ? tier(Renderer3D::defaultMaterialQualityFor(*dev_, platform::isTouchDevice())) : 1;
    return std::min(tier(r.materialQuality()), deviceTier);
}

gfx::PipelineHandle PlanetRenderer::terrainPipe(gfx::Device& dev, int tier, bool ocean) {
    for (; tier >= 0; --tier) {
        if (pipes_[tier][ocean].valid()) return pipes_[tier][ocean];
        if (shaderFailed_[tier]) continue;
        if (!shaders_[tier].valid()) {
            const std::string def = "#define PLANET_TIER " + std::to_string(tier) + "\n";
            const std::string vs = def + shaders::kPlanetVS, fs = def + shaders::kPlanetFS, msl = def + shaders::kPlanetMSL;
            gfx::ShaderDesc sd;
            sd.glslVertex = vs.c_str(); sd.glslFragment = fs.c_str(); sd.msl = msl.c_str();
            sd.uniformSlots = 3; sd.textureSlots = 14; sd.label = "planet";
            shaders_[tier] = dev.createShader(sd);
            if (!shaders_[tier].valid()) {
                std::fprintf(stderr, "[planet] tier %d shader: %s\n", tier, dev.lastError().c_str());
                shaderFailed_[tier] = true;
                continue;
            }
        }
        gfx::PipelineDesc p;
        p.shader = shaders_[tier];
        p.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
        p.attrs[1] = {1, gfx::VertexFormat::Float3, 12};
        p.attrs[2] = {2, gfx::VertexFormat::UByte4N, 24};
        p.attrs[3] = {3, gfx::VertexFormat::Float3, 28};
        p.attrs[4] = {4, gfx::VertexFormat::Float2, 40};
        p.attrs[5] = {5, gfx::VertexFormat::UByte4N, 48};
        p.attrCount = 6; p.stride = sizeof(PV);
        p.indexType = gfx::IndexType::UInt32;
        if (ocean) { p.blend = gfx::BlendMode::Alpha; p.depthWrite = false; }
        p.label = ocean ? "planet ocean" : "planet";
        pipes_[tier][ocean] = dev.createPipeline(p);
        if (pipes_[tier][ocean].valid()) return pipes_[tier][ocean];
        shaderFailed_[tier] = true;
    }
    return {};
}

void PlanetRenderer::initSpace(gfx::Device& dev) {
    if (!skyShader_.valid()) {
        gfx::ShaderDesc sd; sd.glslVertex = shaders::kSkyVS; sd.glslFragment = shaders::kSkyFS; sd.msl = shaders::kSkyMSL;
        sd.uniformSlots = 1; sd.label = "planet sky";
        skyShader_ = dev.createShader(sd);
        if (skyShader_.valid()) {
            // one unused attribute over the bound sphere buffer: Metal wants a valid vertex layout (stride > 0)
            gfx::PipelineDesc p; p.shader = skyShader_; p.attrs[0] = {0, gfx::VertexFormat::Float3, 0}; p.attrCount = 1;
            p.stride = sizeof(MeshVertex); p.indexType = gfx::IndexType::None;
            p.cull = gfx::CullMode::None; p.depthWrite = false; p.label = "planet sky";
            skyPipe_ = dev.createPipeline(p);
        } else std::fprintf(stderr, "[planet] sky shader: %s\n", dev.lastError().c_str());
    }
    if (!atmoShader_.valid()) {
        gfx::ShaderDesc sd; sd.glslVertex = shaders::kAtmoVS; sd.glslFragment = shaders::kAtmoFS; sd.msl = shaders::kAtmoMSL;
        sd.uniformSlots = 2; sd.label = "planet atmosphere";
        atmoShader_ = dev.createShader(sd);
        if (atmoShader_.valid()) {
            gfx::PipelineDesc p; p.shader = atmoShader_;
            p.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
            p.attrCount = 1; p.stride = sizeof(MeshVertex); p.indexType = gfx::IndexType::UInt32;
            p.cull = gfx::CullMode::Front;   // back faces of the shell: also works from inside the atmosphere
            p.blend = gfx::BlendMode::Additive; p.depthWrite = false; p.depthTest = false; p.label = "planet atmosphere";
            atmoPipe_ = dev.createPipeline(p);
        } else std::fprintf(stderr, "[planet] atmosphere shader: %s\n", dev.lastError().c_str());
    }
}

void PlanetRenderer::draw(Renderer3D& r, const PlanetViewFrame& fr, double seconds, int lightBody, float planetBlend) {
    if (!dev_ || world_.empty()) return;
    gfx::Device& dev = *dev_;
    const auto t0 = std::chrono::steady_clock::now();
    if (!texturesLoaded_) {
        texturesLoaded_ = true;
        auto load = [&](const std::string& p) { return p.empty() ? TextureRef{} : textures_.resolve(p, false, base_, true); };
        for (size_t i = 0; i < look_.layers.size() && i < 4; ++i) layerTex_[i] = load(look_.layers[i].texture);
        cliffTex_ = load(look_.cliffTexture);
    }
    initSpace(dev);
    stats_.drawn = stats_.culledFrustum = stats_.culledHorizon = 0;
    stats_.triangles = 0;
    stats_.chunks = 0;
    const int built0 = stats_.built;
    // view-world positions of the bodies
    std::vector<float3> centers(world_.bodies.size());
    for (size_t i = 0; i < world_.bodies.size(); ++i) centers[i] = fr.toView(world_.center(int(i), seconds));
    const int star = world_.starOf(lightBody);
    const float3 eye = r.camera().eye();
    // the star lights the focused body: Renderer3D's sun (shadows, models) points to it
    float3 sunDirFocus = normalize(float3{0.55f, 1.0f, 0.35f});
    if (star >= 0 && lightBody >= 0 && lightBody != star) sunDirFocus = normalize(centers[size_t(star)] - centers[size_t(lightBody)]);
    r.lighting.sunDir = sunDirFocus;
    r.lighting.fogDensity = 0;   // distance fog would grey out the planet from orbit; the shader has its own haze
    if (star >= 0) r.lighting.sunColor = lerp(r.lighting.sunColor, world_.bodies[size_t(star)].color, 0.35f);
    // frustum planes (view world)
    const mat4& vp = r.viewProj();
    float4 planes[4];
    for (int i = 0; i < 4; ++i) {
        const int row = i / 2;
        const float sg = (i % 2) ? -1.0f : 1.0f;
        planes[i] = {vp.at(3, 0) + sg * vp.at(row, 0), vp.at(3, 1) + sg * vp.at(row, 1), vp.at(3, 2) + sg * vp.at(row, 2), vp.at(3, 3) + sg * vp.at(row, 3)};
        const float l = std::sqrt(planes[i].x * planes[i].x + planes[i].y * planes[i].y + planes[i].z * planes[i].z);
        planes[i] = {planes[i].x / l, planes[i].y / l, planes[i].z / l, planes[i].w / l};
    }
    auto sphereVisible = [&](float3 c, float rad) {
        for (auto& p : planes) if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -rad) return false;
        return true;
    };
    const mat4 rot = [&] { mat4 m = fr.matrix(); m.at(0, 3) = m.at(1, 3) = m.at(2, 3) = 0; return m; }();
    const float pxScale = float(std::max(r.sceneHeight(), 1)) / (2.0f * std::tan(r.camera().fovY * 0.5f));

    struct Item { const Chunk* c; float morphStart, morphEnd; int body; };
    auto items = std::make_shared<std::vector<Item>>();
    struct BodyDraw { int body; float3 center; float sea; bool fog; };
    auto bodyDraws = std::make_shared<std::vector<BodyDraw>>();
    struct Sphere { float3 center; float radius; float4 color; bool emissive; };
    std::vector<Sphere> spheres;
    int buildBudget = 24;   // lazily built chunks per frame (first frames / fast zoom), coarse parents stand in

    for (size_t bi = 0; bi < world_.bodies.size(); ++bi) {
        const PlanetBody& pb = world_.bodies[bi];
        const float3 C = centers[bi];
        const float camD = length(eye - C);
        const float projR = pb.radius / std::max(camD, 1.0f) * pxScale;   // body radius in pixels
        if (!sphereVisible(C, pb.radius * 1.2f + 2)) continue;
        if (pb.star()) { spheres.push_back({C, pb.radius, {pb.color.x * 1.6f, pb.color.y * 1.6f, pb.color.z * 1.5f, 1}, true}); continue; }
        Body& b = bodies_[bi];
        if (!b.surf || projR < 3.0f) { // a few pixels: a plain sphere is enough
            spheres.push_back({C, pb.radius, {0.5f, 0.55f, 0.5f, 1}, false});
            continue;
        }
        stats_.chunks += int(b.chunks.size());
        bodyDraws->push_back({int(bi), C, pb.radius + pb.seaLevel, b.fog});
        // eye in body-local coordinates (rotation of the frame)
        const float3 eyeL = fr.dirToWorld(eye - C);
        const float eyeR = length(eyeL);
        const float occR = pb.radius + std::min(b.surf->minHeight(), 0.0f) - 1.0f;
        // CDLOD selection: leaf range lodRange, doubling per level
        std::vector<std::pair<int, int>> stack;   // chunk, parent-drawn flag
        for (int f = 0; f < 6; ++f) stack.push_back({f, 0});
        while (!stack.empty()) {
            const int ci = stack.back().first;
            stack.pop_back();
            Chunk& c = b.chunks[size_t(ci)];
            // horizon: hidden when the whole angular cap of the chunk lies beyond the horizon of the
            // occluder (the lowest ground of the body): angle(eye, chunk) - cap > acos(R0/eye) + acos(R0/maxR)
            if (eyeR > occR) {
                const float3 eyeDir = eyeL * (1.0f / eyeR);
                const float aE = std::acos(std::clamp(dot(eyeDir, c.dir), -1.0f, 1.0f)), aC = std::acos(std::clamp(c.cosAngle, -1.0f, 1.0f));
                const float aH = std::acos(occR / eyeR) + std::acos(std::clamp(occR / std::max(c.maxR, occR), -1.0f, 1.0f));
                if (aE - aC > aH) { ++stats_.culledHorizon; continue; }
            }
            const float3 cv = C + rot.transformDir(c.center);
            if (!sphereVisible(cv, c.boundR)) { ++stats_.culledFrustum; continue; }
            const float d = std::max(length(cv - eye) - c.boundR, 0.0f);
            const float range = options.lodRange * float(1 << c.level);
            const bool split = c.child >= 0 && d < range;
            if (split) {
                bool ready = true;
                for (int k = 0; k < 4; ++k) if (!b.chunks[size_t(c.child + k)].built) ready = false;
                if (!ready && buildBudget > 0) {
                    for (int k = 0; k < 4; ++k) { Chunk& ch = b.chunks[size_t(c.child + k)]; if (!ch.built) { buildChunk(dev, pb, b, ch); --buildBudget; } }
                    ready = true;
                }
                if (ready) { for (int k = 0; k < 4; ++k) stack.push_back({c.child + k, 0}); continue; }
            }
            if (!c.built) buildChunk(dev, pb, b, c);
            // morph towards the parent level over the last quarter of this level's range
            const float end = range * 2.0f, start = end * 0.75f;
            items->push_back({&c, c.level + 1 < b.levels ? start : 1e9f, c.level + 1 < b.levels ? end : 2e9f, int(bi)});
            ++stats_.drawn;
            stats_.triangles += c.indexCount / 3;
        }
    }
    const int tier = tierFor(r);
    gfx::PipelineHandle pipe = terrainPipe(dev, tier, false), ocean = terrainPipe(dev, tier, true);
    // ---- uniforms per body
    PU0 base{};
    base.opts[0] = 0.05f;
    base.opts[1] = tier >= 1 && look_.materials() ? 1.0f : 0.0f;
    for (int i = 0; i < 4; ++i) {
        const TerrainLook::Layer* L = i < int(look_.layers.size()) ? &look_.layers[size_t(i)] : nullptr;
        base.scale[i] = L ? L->scale : 4.0f;
        put(base.tint[i], L ? L->tint : float3{1, 1, 1}, 1);
    }
    put(base.cliff, {look_.cliffScale, look_.splatStrength, cliffTex_.valid() ? look_.slopeStart : 2.0f}, cliffTex_.valid() ? look_.slopeEnd : 3.0f);
    put(base.cliffTint, look_.cliffTint, 1);
    base.water[0] = look_.waterColor.x; base.water[1] = look_.waterColor.y; base.water[2] = look_.waterColor.z; base.water[3] = look_.waterColor.w;
    const bool shadows = r.shadowQuality() != ShadowQuality::Off && planetBlend < 0.5f;
    struct BodyU { PU0 u; int body; };
    auto bodyU = std::make_shared<std::vector<BodyU>>();
    for (auto& bd : *bodyDraws) {
        const PlanetBody& pb = world_.bodies[size_t(bd.body)];
        const Body& b = bodies_[size_t(bd.body)];
        PU0 u = base;
        std::memcpy(u.model, rot.m, sizeof u.model);
        put(u.body, bd.center, pb.radius);
        u.morph[2] = bd.sea;
        const float3 L = star >= 0 && bd.body != star ? normalize(centers[size_t(star)] - bd.center) : sunDirFocus;
        put(u.sun, L, shadows && bd.body == lightBody ? 1.0f : 0.0f);
        // haze: atmosphere colour, thickness relative to the radius (none on airless moons)
        put(u.atmo, pb.atmosphere ? pb.atmoColor : float3{0, 0, 0}, 0.45f);   // w: ambient on the night side (gameplay stays readable)
        u.opts[3] = pb.atmosphere ? 0.6f / std::max(pb.radius * 6.0f, 1.0f) : 0.0f;
        if (bd.fog) {
            const FogLook& fl = b.fogLook;
            u.opts[2] = 1;
            u.fog[0] = fl.explored; u.fog[1] = fl.unexplored;
            u.fog[2] = float(3 * pb.faceSize); u.fog[3] = float(2 * pb.faceSize);
            put(u.fogColor, {fl.color[0], fl.color[1], fl.color[2]}, fl.style == FogLook::Cells || fl.style == FogLook::Grid ? 1.0f : 0.0f);
        }
        bodyU->push_back({u, bd.body});
    }
    // ---- terrain (opaque, before meshes and models)
    if (pipe.valid() && !items->empty()) {
        r.drawCustomOpaque([this, &r, items, bodyU, pipe](gfx::Device& d) {
            d.setPipeline(pipe);
            r.bindLitFrame();
            for (int i = 0; i < 4; ++i) d.setTexture(5 + i, layerTex_[i].valid() ? layerTex_[i].tex : white_);
            d.setTexture(13, cliffTex_.valid() ? cliffTex_.tex : gray_);
            int cur = -1;
            PU0 u{};
            for (auto& it : *items) {
                if (it.body != cur) {
                    cur = it.body;
                    for (auto& bu : *bodyU) if (bu.body == cur) u = bu.u;
                    d.setTexture(2, bodies_[size_t(cur)].vision.valid() ? bodies_[size_t(cur)].vision : white_);
                }
                PU0 uu = u;
                // chunk-relative vertices: model = rotation * translate(chunk centre) + body centre
                const float3 cc = float3{u.body[0], u.body[1], u.body[2]};
                mat4 m;
                std::memcpy(m.m, u.model, sizeof m.m);
                const float3 off = cc + m.transformDir(it.c->center);
                m.at(0, 3) = off.x; m.at(1, 3) = off.y; m.at(2, 3) = off.z;
                std::memcpy(uu.model, m.m, sizeof uu.model);
                uu.morph[0] = it.morphStart; uu.morph[1] = it.morphEnd; uu.morph[3] = 0;
                d.setUniforms(0, &uu, sizeof uu);
                d.setVertexBuffer(it.c->vb);
                d.setIndexBuffer(it.c->ib);
                d.drawIndexed(it.c->indexCount);
            }
        });
    }
    // ---- space: sky first (behind), stars + far bodies as meshes, atmosphere + ocean blended
    const bool atmoTier = r.materialQuality() != MaterialQuality::Low && options.atmosphere;
    if (options.sky && skyPipe_.valid()) {
        SU0 su{};
        const float aspect = r.sceneHeight() > 0 ? float(r.sceneWidth()) / float(r.sceneHeight()) : 1.0f;
        RTSCamera rc = r.camera();
        mat4 vpRot = rc.proj(aspect, dev.info().clipDepthZeroToOne) * rc.view();
        const mat4 inv = vpRot.inverse();
        std::memcpy(su.inv, inv.m, sizeof su.inv);
        put(su.eye, eye, 1.0f);
        const float3 sd = star >= 0 ? normalize(centers[size_t(star)] - eye) : sunDirFocus;
        const float sc = star >= 0 ? world_.bodies[size_t(star)].radius / std::max(length(centers[size_t(star)] - eye), 1.0f) : 0.02f;
        put(su.star, sd, std::clamp(2.0f / std::max(sc, 1e-4f), 8.0f, 4000.0f));
        put(su.starColor, star >= 0 ? world_.bodies[size_t(star)].color : float3{1, 0.9f, 0.7f}, 0.8f);
        // drawn first in the opaque list (depth 0.9999, no depth write): everything else covers it
        auto skyPipe = skyPipe_;
        const gfx::BufferHandle anyVb = sphere_.vb;   // GL: keep a buffer bound for the attribute-less draw
        r.drawCustomOpaque([su, skyPipe, anyVb](gfx::Device& d) { d.setPipeline(skyPipe); d.setVertexBuffer(anyVb); d.setUniforms(0, &su, sizeof su); d.draw(3); });
    }
    for (auto& s : spheres) r.drawMesh(sphere_, mat4::translate(s.center) * mat4::scale({s.radius, s.radius, s.radius}), s.color, s.emissive ? kDrawUnlit | kDrawNoCastShadow : kDrawNoCastShadow, 0.1f, s.emissive ? 1.5f : 0.0f);
    // ocean + atmosphere after the opaque scene (custom draws run after translucent meshes)
    struct OceanItem { const Chunk* c; float s, e; int body; };
    auto oceanItems = std::make_shared<std::vector<OceanItem>>();
    for (auto& it : *items) if (it.c->hasSea) oceanItems->push_back({it.c, it.morphStart, it.morphEnd, it.body});
    if (ocean.valid() && !oceanItems->empty()) {
        r.drawCustom([this, &r, oceanItems, bodyU, ocean](gfx::Device& d) {
            d.setPipeline(ocean);
            r.bindLitFrame();
            for (int i = 0; i < 4; ++i) d.setTexture(5 + i, white_);
            d.setTexture(13, gray_);
            int cur = -1;
            PU0 u{};
            for (auto& it : *oceanItems) {
                if (it.body != cur) {
                    cur = it.body;
                    for (auto& bu : *bodyU) if (bu.body == cur) u = bu.u;
                    d.setTexture(2, bodies_[size_t(cur)].vision.valid() ? bodies_[size_t(cur)].vision : white_);
                }
                PU0 uu = u;
                const float3 cc{u.body[0], u.body[1], u.body[2]};
                mat4 m;
                std::memcpy(m.m, u.model, sizeof m.m);
                const float3 off = cc + m.transformDir(it.c->center);
                m.at(0, 3) = off.x; m.at(1, 3) = off.y; m.at(2, 3) = off.z;
                std::memcpy(uu.model, m.m, sizeof uu.model);
                uu.morph[0] = it.s; uu.morph[1] = it.e; uu.morph[3] = 1;
                d.setUniforms(0, &uu, sizeof uu);
                d.setVertexBuffer(it.c->vb);
                d.setIndexBuffer(it.c->ib);
                d.drawIndexed(it.c->indexCount);
            }
        });
    }
    if (atmoTier && atmoPipe_.valid()) {
        auto atmos = std::make_shared<std::vector<AU0>>();
        for (size_t bi = 0; bi < world_.bodies.size(); ++bi) {
            const PlanetBody& pb = world_.bodies[bi];
            if (!pb.atmosphere || pb.star()) continue;
            const float R = pb.radius * (1.0f + pb.atmoHeight) + std::max(0.0f, bodies_[bi].surf ? bodies_[bi].surf->maxHeight() * 0.5f : 0.0f);
            const float3 C = centers[bi];
            if (!sphereVisible(C, R)) continue;
            AU0 a{};
            const mat4 m = mat4::translate(C) * mat4::scale({R * 1.02f, R * 1.02f, R * 1.02f});
            std::memcpy(a.model, m.m, sizeof a.model);
            put(a.body, C, pb.radius + std::max(pb.seaLevel > -1e8f ? pb.seaLevel : 0.0f, 0.0f));
            const float k = std::clamp((length(eye - C) - R) / (pb.radius * 0.5f), 0.0f, 1.0f);  // fades in from orbit
            put(a.shell, {R, 0.55f + 0.45f * k, 0}, 0);
            put(a.color, mul(pb.atmoColor, pb.atmoColor), 1);
            put(a.sun, star >= 0 ? normalize(centers[size_t(star)] - C) : sunDirFocus, 0);
            atmos->push_back(a);
        }
        if (!atmos->empty()) {
            auto pipeA = atmoPipe_;
            const Mesh* sph = &sphere_;
            r.drawCustom([&r, atmos, pipeA, sph](gfx::Device& d) {
                d.setPipeline(pipeA);
                r.bindLitFrame();
                for (auto& a : *atmos) {
                    d.setUniforms(0, &a, sizeof a);
                    d.setVertexBuffer(sph->vb);
                    d.setIndexBuffer(sph->ib);
                    d.drawIndexed(sph->indexCount);
                }
            });
        }
    }
    // ---- orbit lines (system zoom)
    if (options.orbits && planetBlend > 0.3f) {
        const uint8_t alpha = uint8_t(std::clamp((planetBlend - 0.3f) * 1.6f, 0.0f, 1.0f) * 120.0f);
        for (size_t bi = 0; bi < world_.bodies.size(); ++bi) {
            const PlanetBody& pb = world_.bodies[bi];
            if (pb.parent < 0 || pb.orbitRadius <= 0) continue;
            const float3 pc = centers[size_t(pb.parent)];
            const int seg = 128;
            float3 prev;
            for (int i = 0; i <= seg; ++i) {
                const float a = 2 * kPi * float(i) / float(seg);
                const float3 p = pc + fr.dirToView(float3{std::cos(a), 0, std::sin(a)}) * pb.orbitRadius;
                if (i) r.line(prev, p, rgba(150, 180, 230, alpha));
                prev = p;
            }
        }
    }
    stats_.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static const bool printStats = std::getenv("RTS_TERRAIN_STATS") != nullptr;
    if (printStats) {
        static int frames = 0;
        static auto last = std::chrono::steady_clock::now();
        if (++frames == 60) {
            const auto now = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[planet] chunks %d drawn %d culled horizon %d frustum %d tris %llu built %d (+%d) cpu %.2f ms frame %.2f ms\n", stats_.chunks, stats_.drawn,
                         stats_.culledHorizon, stats_.culledFrustum, (unsigned long long)stats_.triangles, stats_.built, stats_.built - built0, stats_.buildMs,
                         std::chrono::duration<double, std::milli>(now - last).count() / frames);
            frames = 0; last = now;
        }
    }
}

} // namespace rts::game
