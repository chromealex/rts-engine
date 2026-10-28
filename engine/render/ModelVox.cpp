// Voxel models: MagicaVoxel .vox and Tiberian Sun / Red Alert 2 .vxl (+ .hva).
//
// Both are turned into ordinary meshes: visible faces only (no faces between two solid voxels),
// greedy-merged into rectangles of one colour, flat normals, the palette colour as a linear
// vertex colour. One voxel = 1 unit (the view fits the model to its "height").
//
// MagicaVoxel .vox
//   * SIZE/XYZI models, RGBA palette (the default palette when absent), MATL (_emit -> emissive,
//     _metal -> metallic/roughness; glass and the rest ignored).
//   * Scene graph: every nTRN becomes a node (name = "_name", else "node<id>"), with its frame-0
//     translation and packed rotation; nGRP children hang below it; nSHP puts the mesh on it.
//     A model used by several shapes shares one mesh. Hidden nodes / layers are skipped.
//   * Models are centred like MagicaVoxel does: the node origin is the model centre
//     (voxel x - floor(size.x / 2), same for y and z).
//   * MagicaVoxel is Z up; converted to Y up with (x, y, z) -> (x, z, -y), so the side that faces
//     MagicaVoxel's front (-Y) faces +Z, the engine's forward.
//   * Team colour convention: the LAST 8 PALETTE SLOTS (voxel colour indices 248..255, i.e. the
//     bottom row of MagicaVoxel's palette) are team-colour slots. Their faces get vertex alpha 0 and
//     the materials TeamMode::VertexAlpha; the slot colour's brightness shades the team colour.
//   * Animation: a shape with several models ("_f" frame attribute) or a transform with several
//     frames becomes the clip "Idle" at 10 fps (frames toggled by node scale 0/1, Step keys).
//
// Westwood .vxl + .hva
//   * Every section becomes a node named by the section ("Body", "Turret", ...) with its own mesh.
//     Voxel (x, y, z) sits at boundsMin + (x, y, z) * (boundsMax - boundsMin) / size.
//   * Palette: the one in the .vxl; if it is all zero, "<name>.pal", then unittem.pal / unit.pal in
//     the same or the parent directory, else a grey ramp (+ warning). 6-bit palettes are expanded.
//   * Section transform = the .hva matrix of the section (frame 0 = bind pose), the translation
//     scaled by the section scale and voxel size, the whole section scaled by the section scale
//     (what the games and OpenRA do). Without an .hva the tailer matrix is used instead. More than
//     one .hva frame -> clip "Idle" at 15 fps (Step keys). A "barrel" section is parented to the
//     "turret" section so turret aiming carries it.
//   * Remap range (palette 16..31, from the header) = team colour: vertex alpha 0, VertexAlpha.
//     The vertex RGB keeps the palette colour: the VertexAlpha shader scales the team colour by its
//     brightness, so the remap ramp shades the team colour like in the games.
//   * Westwood normal tables are ignored (flat face normals).
//   * Voxel space is Z up with units facing +X; converted with (x, y, z) -> (y, z, x): Y up, facing +Z.
#include "Model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace rts::render {

namespace {

// ------------------------------------------------------------------ shared helpers

uint8_t srgbToLinear8(uint8_t c) {
    const float s = c / 255.0f;
    const float l = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
    return uint8_t(std::lround(l * 255.0f));
}
uint32_t linearColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return uint32_t(srgbToLinear8(r)) | (uint32_t(srgbToLinear8(g)) << 8) | (uint32_t(srgbToLinear8(b)) << 16) | (uint32_t(a) << 24);
}
std::string dirOf(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
// path without its extension
std::string stemOf(const std::string& p) {
    const size_t dot = p.find_last_of('.'), slash = p.find_last_of("/\\");
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? p : p.substr(0, dot);
}
float comp(const float3& v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; }
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// Little-endian reads; callers check the range first.
uint32_t u32At(const std::string& b, size_t o) {
    return uint32_t(uint8_t(b[o])) | (uint32_t(uint8_t(b[o + 1])) << 8) | (uint32_t(uint8_t(b[o + 2])) << 16) | (uint32_t(uint8_t(b[o + 3])) << 24);
}
float f32At(const std::string& b, size_t o) {
    const uint32_t u = u32At(b, o);
    float f;
    std::memcpy(&f, &u, 4);
    return std::isfinite(f) ? f : 0.0f;
}

// Rotation part of an affine matrix -> quaternion (Shepperd).
quat quatFromMatrix(const mat4& m) {
    const float r00 = m.at(0, 0), r11 = m.at(1, 1), r22 = m.at(2, 2), tr = r00 + r11 + r22;
    quat q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        q = {(m.at(2, 1) - m.at(1, 2)) / s, (m.at(0, 2) - m.at(2, 0)) / s, (m.at(1, 0) - m.at(0, 1)) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2;
        q = {0.25f * s, (m.at(0, 1) + m.at(1, 0)) / s, (m.at(0, 2) + m.at(2, 0)) / s, (m.at(2, 1) - m.at(1, 2)) / s};
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2;
        q = {(m.at(0, 1) + m.at(1, 0)) / s, 0.25f * s, (m.at(1, 2) + m.at(2, 1)) / s, (m.at(0, 2) - m.at(2, 0)) / s};
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2;
        q = {(m.at(0, 2) + m.at(2, 0)) / s, (m.at(1, 2) + m.at(2, 1)) / s, 0.25f * s, (m.at(1, 0) - m.at(0, 1)) / s};
    }
    return qnormalize(q);
}
// Affine matrix -> T, R, S (mirroring goes into a negative x scale).
void decompose(const mat4& m, float3& t, quat& r, float3& s) {
    t = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    float3 c[3];
    for (int i = 0; i < 3; ++i) c[i] = {m.at(0, i), m.at(1, i), m.at(2, i)};
    s = {length(c[0]), length(c[1]), length(c[2])};
    if (dot(cross(c[0], c[1]), c[2]) < 0) s.x = -s.x;
    mat4 rot;
    const float sc[3] = {s.x, s.y, s.z};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) rot.at(k, i) = std::fabs(sc[i]) > 1e-12f ? comp(c[i], k) / sc[i] : (i == k ? 1.0f : 0.0f);
    r = quatFromMatrix(rot);
}
void setNodeMatrix(Node& n, const mat4& m) { decompose(m, n.t, n.r, n.s); }

// ------------------------------------------------------------------ greedy mesher

struct VoxelGrid {
    int size[3] = {0, 0, 0};
    std::vector<uint16_t> key;  // 0 = empty, else palette key
    void resize(int x, int y, int z) {
        size[0] = x; size[1] = y; size[2] = z;
        key.assign(size_t(x) * size_t(y) * size_t(z), 0);
    }
    size_t index(int x, int y, int z) const { return (size_t(z) * size_t(size[1]) + size_t(y)) * size_t(size[0]) + size_t(x); }
    uint16_t at(const int c[3]) const {
        for (int i = 0; i < 3; ++i) if (c[i] < 0 || c[i] >= size[i]) return 0;
        return key[index(c[0], c[1], c[2])];
    }
};

struct FaceStyle { uint32_t color; int material; };
using StyleFn = std::function<FaceStyle(uint16_t key)>;
using CornerFn = std::function<float3(float x, float y, float z)>;  // grid corner -> mesh space

// Appends the visible faces of the grid as merged quads (4 vertices, 2 triangles each), one
// primitive per material. Returns the mesh index or -1 when nothing is visible.
int greedyMesh(Model& m, const std::string& name, const VoxelGrid& g, const StyleFn& style, const CornerFn& corner) {
    // uv: face-plane voxel coordinates / 16 (box projection), so tiling textures of materials
    // assigned to voxel models (docs/models.md "assign") land at 16 voxels per repeat
    struct Quad { float3 p[4]; uint32_t color; int material; float2 uv[4]; };
    std::vector<Quad> quads;
    std::map<uint16_t, FaceStyle> styles;
    auto styleOf = [&](uint16_t k) {
        auto it = styles.find(k);
        if (it == styles.end()) it = styles.emplace(k, style(k)).first;
        return it->second;
    };
    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3, v = (d + 2) % 3, nu = g.size[u], nv = g.size[v];
        std::vector<uint16_t> mask(size_t(nu) * size_t(nv));
        for (int s = 0; s < g.size[d]; ++s)
            for (int dir = 1; dir >= -1; dir -= 2) {
                for (int j = 0; j < nv; ++j)
                    for (int i = 0; i < nu; ++i) {
                        int c[3]; c[d] = s; c[u] = i; c[v] = j;
                        const uint16_t k = g.at(c);
                        c[d] = s + dir;
                        mask[size_t(j) * size_t(nu) + size_t(i)] = k && !g.at(c) ? k : 0;
                    }
                for (int j = 0; j < nv; ++j)
                    for (int i = 0; i < nu;) {
                        const uint16_t k = mask[size_t(j) * size_t(nu) + size_t(i)];
                        if (!k) { ++i; continue; }
                        int w = 1, h = 1;
                        while (i + w < nu && mask[size_t(j) * size_t(nu) + size_t(i + w)] == k) ++w;
                        for (bool grow = true; grow && j + h < nv;) {
                            for (int x = 0; x < w; ++x)
                                if (mask[size_t(j + h) * size_t(nu) + size_t(i + x)] != k) { grow = false; break; }
                            if (grow) ++h;
                        }
                        for (int y = 0; y < h; ++y)
                            for (int x = 0; x < w; ++x) mask[size_t(j + y) * size_t(nu) + size_t(i + x)] = 0;
                        float b[3]; b[d] = float(dir > 0 ? s + 1 : s); b[u] = float(i); b[v] = float(j);
                        float du[3] = {0, 0, 0}, dv[3] = {0, 0, 0};
                        du[u] = float(w); dv[v] = float(h);
                        auto at = [&](float a, float bb) {
                            return corner(b[0] + du[0] * a + dv[0] * bb, b[1] + du[1] * a + dv[1] * bb, b[2] + du[2] * a + dv[2] * bb);
                        };
                        const FaceStyle st = styleOf(k);
                        auto tc = [&](float a, float bb) { return float2{(b[u] + float(w) * a) / 16.0f, (b[v] + float(h) * bb) / 16.0f}; };
                        Quad q{{at(0, 0), at(1, 0), at(1, 1), at(0, 1)}, st.color, st.material, {tc(0, 0), tc(1, 0), tc(1, 1), tc(0, 1)}};
                        if (dir < 0) { std::swap(q.p[1], q.p[3]); std::swap(q.uv[1], q.uv[3]); }  // e_u x e_v = +e_d: flip winding for -d faces
                        quads.push_back(q);
                        i += w;
                    }
            }
    }
    if (quads.empty()) return -1;
    std::stable_sort(quads.begin(), quads.end(), [](const Quad& a, const Quad& b) { return a.material < b.material; });
    MeshDef md;
    md.name = name;
    for (size_t q = 0; q < quads.size();) {
        Primitive p;
        p.material = quads[q].material;
        p.firstIndex = uint32_t(m.indices.size());
        p.firstVertex = uint32_t(m.vertices.size());
        for (; q < quads.size() && quads[q].material == p.material; ++q) {
            const Quad& qd = quads[q];
            const float3 n = normalize(cross(qd.p[1] - qd.p[0], qd.p[3] - qd.p[0]));
            const uint32_t base = uint32_t(m.vertices.size());
            for (int c = 0; c < 4; ++c) {
                ModelVertex vx;
                vx.pos = qd.p[c]; vx.nrm = n; vx.color = qd.color; vx.uv = qd.uv[c];
                m.vertices.push_back(vx);
            }
            m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        p.indexCount = uint32_t(m.indices.size()) - p.firstIndex;
        p.vertexCount = uint32_t(m.vertices.size()) - p.firstVertex;
        generateTangents(m.vertices, m.indices, p.firstIndex, p.indexCount);   // normal maps of assigned materials
        md.primitives.push_back(std::move(p));
    }
    m.meshes.push_back(std::move(md));
    return int(m.meshes.size()) - 1;
}

// ------------------------------------------------------------------ MagicaVoxel

// Default palette of MagicaVoxel (0xAABBGGRR, indexed by voxel colour index; 0 = empty).
const uint32_t kVoxDefaultPalette[256] = {
    0x00000000, 0xffffffff, 0xffccffff, 0xff99ffff, 0xff66ffff, 0xff33ffff, 0xff00ffff, 0xffffccff, 0xffccccff, 0xff99ccff, 0xff66ccff, 0xff33ccff, 0xff00ccff, 0xffff99ff, 0xffcc99ff, 0xff9999ff,
    0xff6699ff, 0xff3399ff, 0xff0099ff, 0xffff66ff, 0xffcc66ff, 0xff9966ff, 0xff6666ff, 0xff3366ff, 0xff0066ff, 0xffff33ff, 0xffcc33ff, 0xff9933ff, 0xff6633ff, 0xff3333ff, 0xff0033ff, 0xffff00ff,
    0xffcc00ff, 0xff9900ff, 0xff6600ff, 0xff3300ff, 0xff0000ff, 0xffffffcc, 0xffccffcc, 0xff99ffcc, 0xff66ffcc, 0xff33ffcc, 0xff00ffcc, 0xffffcccc, 0xffcccccc, 0xff99cccc, 0xff66cccc, 0xff33cccc,
    0xff00cccc, 0xffff99cc, 0xffcc99cc, 0xff9999cc, 0xff6699cc, 0xff3399cc, 0xff0099cc, 0xffff66cc, 0xffcc66cc, 0xff9966cc, 0xff6666cc, 0xff3366cc, 0xff0066cc, 0xffff33cc, 0xffcc33cc, 0xff9933cc,
    0xff6633cc, 0xff3333cc, 0xff0033cc, 0xffff00cc, 0xffcc00cc, 0xff9900cc, 0xff6600cc, 0xff3300cc, 0xff0000cc, 0xffffff99, 0xffccff99, 0xff99ff99, 0xff66ff99, 0xff33ff99, 0xff00ff99, 0xffffcc99,
    0xffcccc99, 0xff99cc99, 0xff66cc99, 0xff33cc99, 0xff00cc99, 0xffff9999, 0xffcc9999, 0xff999999, 0xff669999, 0xff339999, 0xff009999, 0xffff6699, 0xffcc6699, 0xff996699, 0xff666699, 0xff336699,
    0xff006699, 0xffff3399, 0xffcc3399, 0xff993399, 0xff663399, 0xff333399, 0xff003399, 0xffff0099, 0xffcc0099, 0xff990099, 0xff660099, 0xff330099, 0xff000099, 0xffffff66, 0xffccff66, 0xff99ff66,
    0xff66ff66, 0xff33ff66, 0xff00ff66, 0xffffcc66, 0xffcccc66, 0xff99cc66, 0xff66cc66, 0xff33cc66, 0xff00cc66, 0xffff9966, 0xffcc9966, 0xff999966, 0xff669966, 0xff339966, 0xff009966, 0xffff6666,
    0xffcc6666, 0xff996666, 0xff666666, 0xff336666, 0xff006666, 0xffff3366, 0xffcc3366, 0xff993366, 0xff663366, 0xff333366, 0xff003366, 0xffff0066, 0xffcc0066, 0xff990066, 0xff660066, 0xff330066,
    0xff000066, 0xffffff33, 0xffccff33, 0xff99ff33, 0xff66ff33, 0xff33ff33, 0xff00ff33, 0xffffcc33, 0xffcccc33, 0xff99cc33, 0xff66cc33, 0xff33cc33, 0xff00cc33, 0xffff9933, 0xffcc9933, 0xff999933,
    0xff669933, 0xff339933, 0xff009933, 0xffff6633, 0xffcc6633, 0xff996633, 0xff666633, 0xff336633, 0xff006633, 0xffff3333, 0xffcc3333, 0xff993333, 0xff663333, 0xff333333, 0xff003333, 0xffff0033,
    0xffcc0033, 0xff990033, 0xff660033, 0xff330033, 0xff000033, 0xffffff00, 0xffccff00, 0xff99ff00, 0xff66ff00, 0xff33ff00, 0xff00ff00, 0xffffcc00, 0xffcccc00, 0xff99cc00, 0xff66cc00, 0xff33cc00,
    0xff00cc00, 0xffff9900, 0xffcc9900, 0xff999900, 0xff669900, 0xff339900, 0xff009900, 0xffff6600, 0xffcc6600, 0xff996600, 0xff666600, 0xff336600, 0xff006600, 0xffff3300, 0xffcc3300, 0xff993300,
    0xff663300, 0xff333300, 0xff003300, 0xffff0000, 0xffcc0000, 0xff990000, 0xff660000, 0xff330000, 0xff0000ee, 0xff0000dd, 0xff0000bb, 0xff0000aa, 0xff000088, 0xff000077, 0xff000055, 0xff000044,
    0xff000022, 0xff000011, 0xff00ee00, 0xff00dd00, 0xff00bb00, 0xff00aa00, 0xff008800, 0xff007700, 0xff005500, 0xff004400, 0xff002200, 0xff001100, 0xffee0000, 0xffdd0000, 0xffbb0000, 0xffaa0000,
    0xff880000, 0xff770000, 0xff550000, 0xff440000, 0xff220000, 0xff110000, 0xffeeeeee, 0xffdddddd, 0xffbbbbbb, 0xffaaaaaa, 0xff888888, 0xff777777, 0xff555555, 0xff444444, 0xff222222, 0xff111111,
};
constexpr int kVoxTeamFirst = 248;  // colour indices 248..255 = team colour
constexpr float kVoxFps = 10;

using Dict = std::map<std::string, std::string>;

// Bounds-checked cursor over one chunk's content.
struct VoxCursor {
    const std::string& b;
    size_t pos, end;
    const std::string& path;
    const char* chunk;
    [[noreturn]] void fail() const { throw std::runtime_error(path + ": chunk '" + chunk + "' truncated"); }
    int32_t i32() {
        if (end - pos < 4) fail();
        const int32_t v = int32_t(u32At(b, pos));
        pos += 4;
        return v;
    }
    std::string str() {
        const int32_t n = i32();
        if (n < 0 || size_t(n) > end - pos) fail();
        std::string s = b.substr(pos, size_t(n));
        pos += size_t(n);
        return s;
    }
    Dict dict() {
        Dict d;
        const int32_t n = i32();
        if (n < 0 || size_t(n) > (end - pos) / 8) fail();
        for (int32_t i = 0; i < n; ++i) {
            std::string k = str();
            d[k] = str();
        }
        return d;
    }
};

struct VoxModel { int sx = 0, sy = 0, sz = 0; std::vector<uint32_t> voxels; };  // x | y<<8 | z<<16 | c<<24
struct VoxNode {
    char type = 0;  // 'T', 'G', 'S'
    Dict attrs;
    int child = -1, layer = -1;
    std::vector<Dict> frames;                     // nTRN
    std::vector<int> children;                    // nGRP
    std::vector<std::pair<int, Dict>> models;     // nSHP
};

float dictFloat(const Dict& d, const char* k, float def) {
    auto it = d.find(k);
    if (it == d.end()) return def;
    char* e = nullptr;
    const float v = std::strtof(it->second.c_str(), &e);
    return e != it->second.c_str() && std::isfinite(v) ? v : def;
}
int dictInt(const Dict& d, const char* k, int def) {
    auto it = d.find(k);
    if (it == d.end()) return def;
    char* e = nullptr;
    const long v = std::strtol(it->second.c_str(), &e, 10);
    return e != it->second.c_str() ? int(v) : def;
}

// MagicaVoxel (Z up) -> engine (Y up): (x, y, z) -> (x, z, -y)
float3 voxToEngine(float3 p) { return {p.x, p.z, -p.y}; }
mat4 voxAxis() {
    mat4 c;
    c.at(1, 1) = 0; c.at(1, 2) = 1;
    c.at(2, 1) = -1; c.at(2, 2) = 0;
    return c;
}

// Node-local transform of an nTRN frame, already in engine axes.
mat4 voxFrameMatrix(const Dict& f) {
    mat4 r;
    auto rt = f.find("_r");
    if (rt != f.end()) {
        const int bits = std::atoi(rt->second.c_str());
        const int i0 = bits & 3, i1 = (bits >> 2) & 3;
        if (i0 < 3 && i1 < 3 && i0 != i1) {
            const int i2 = 3 - i0 - i1;
            const int idx[3] = {i0, i1, i2};
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col) r.at(row, col) = col == idx[row] ? ((bits >> (4 + row)) & 1 ? -1.0f : 1.0f) : 0.0f;
        }
    }
    float3 t;
    auto tt = f.find("_t");
    if (tt != f.end()) {
        int x = 0, y = 0, z = 0;
        if (std::sscanf(tt->second.c_str(), "%d %d %d", &x, &y, &z) == 3) t = {float(x), float(y), float(z)};
    }
    const mat4 c = voxAxis(), ct = c.inverse();
    mat4 m = c * r * ct;
    const float3 te = voxToEngine(t);
    m.at(0, 3) = te.x; m.at(1, 3) = te.y; m.at(2, 3) = te.z;
    return m;
}

struct VoxLoader {
    VoxLoader(const std::string& p, const std::string& bytes, std::vector<std::string>* w, Model& model) : path(p), b(bytes), warnings(w), m(model) {}
    const std::string& path;
    const std::string& b;
    std::vector<std::string>* warnings;
    Model& m;
    std::vector<VoxModel> models;
    uint32_t palette[256] = {};
    std::map<int, Dict> matl;
    std::map<int, VoxNode> graph;
    std::set<int> hiddenLayers;
    std::vector<int> meshOfModel, materialOf;
    std::set<std::string> warned;
    // animation
    struct ShapeAnim { std::vector<std::pair<int, int>> frameNodes; };  // (frame, node)
    struct TrnAnim { int node; std::vector<std::pair<int, mat4>> frames; };
    std::vector<ShapeAnim> shapeAnims;
    std::vector<TrnAnim> trnAnims;
    std::set<int> visiting;

    void warn(const std::string& w) { if (warnings && warned.insert(w).second) warnings->push_back(path + ": " + w); }

    void parse() {
        if (b.size() < 8 || b.compare(0, 4, "VOX ") != 0) throw std::runtime_error(path + ": not a MagicaVoxel file (no 'VOX ' header)");
        if (b.size() < 20 || b.compare(8, 4, "MAIN") != 0) throw std::runtime_error(path + ": MAIN chunk missing");
        std::memcpy(palette, kVoxDefaultPalette, sizeof palette);
        const size_t mainContent = u32At(b, 12), mainChildren = u32At(b, 16);
        size_t pos = 20 + std::min<size_t>(mainContent, b.size() - 20);
        const size_t end = mainChildren > b.size() - pos ? b.size() : pos + mainChildren;
        if (end < pos + mainChildren) warn("file truncated (MAIN children past the end)");
        int pendingSize = -1;
        while (end - pos >= 12) {
            const std::string id = b.substr(pos, 4);
            const size_t n = u32At(b, pos + 4), kids = u32At(b, pos + 8);
            const size_t content = pos + 12;
            if (n > end - content) throw std::runtime_error(path + ": chunk '" + id + "' truncated");
            VoxCursor c{b, content, content + n, path, id.c_str()};
            if (id == "SIZE") {
                VoxModel vm;
                vm.sx = c.i32(); vm.sy = c.i32(); vm.sz = c.i32();
                if (vm.sx <= 0 || vm.sy <= 0 || vm.sz <= 0 || vm.sx > 2048 || vm.sy > 2048 || vm.sz > 2048 ||
                    double(vm.sx) * vm.sy * vm.sz > 64.0 * 1024 * 1024)
                    throw std::runtime_error(path + ": model " + std::to_string(models.size()) + " has a bad size " +
                                             std::to_string(vm.sx) + "x" + std::to_string(vm.sy) + "x" + std::to_string(vm.sz));
                models.push_back(std::move(vm));
                pendingSize = int(models.size()) - 1;
            } else if (id == "XYZI") {
                if (pendingSize < 0) throw std::runtime_error(path + ": XYZI chunk without a SIZE chunk");
                const int32_t count = c.i32();
                if (count < 0 || size_t(count) > (c.end - c.pos) / 4) c.fail();
                VoxModel& vm = models[size_t(pendingSize)];
                vm.voxels.resize(size_t(count));
                for (int32_t i = 0; i < count; ++i) vm.voxels[size_t(i)] = u32At(b, c.pos + size_t(i) * 4);
                pendingSize = -1;
            } else if (id == "RGBA") {
                if (n < 1024) c.fail();
                palette[0] = 0;
                for (int i = 0; i < 255; ++i) palette[i + 1] = u32At(b, content + size_t(i) * 4);
            } else if (id == "MATL") {
                const int mid = c.i32();
                matl[mid] = c.dict();
            } else if (id == "LAYR") {
                const int lid = c.i32();
                if (dictInt(c.dict(), "_hidden", 0)) hiddenLayers.insert(lid);
            } else if (id == "nTRN" || id == "nGRP" || id == "nSHP") {
                VoxNode nd;
                nd.type = id[1] == 'T' ? 'T' : id[1] == 'G' ? 'G' : 'S';
                const int nid = c.i32();
                nd.attrs = c.dict();
                if (nd.type == 'T') {
                    nd.child = c.i32();
                    c.i32();  // reserved
                    nd.layer = c.i32();
                    const int32_t frames = c.i32();
                    if (frames < 0 || size_t(frames) > (c.end - c.pos) / 4) c.fail();
                    for (int32_t i = 0; i < frames; ++i) nd.frames.push_back(c.dict());
                } else if (nd.type == 'G') {
                    const int32_t k = c.i32();
                    if (k < 0 || size_t(k) > (c.end - c.pos) / 4) c.fail();
                    for (int32_t i = 0; i < k; ++i) nd.children.push_back(c.i32());
                } else {
                    const int32_t k = c.i32();
                    if (k < 0 || size_t(k) > (c.end - c.pos) / 8) c.fail();
                    for (int32_t i = 0; i < k; ++i) {
                        const int mid = c.i32();
                        nd.models.emplace_back(mid, c.dict());
                    }
                }
                graph[nid] = std::move(nd);
            }
            // PACK, MATT, rOBJ, rCAM, NOTE, IMAP, ...: nothing to read
            if (kids > end - content - n) break;
            pos = content + n + kids;
        }
        if (models.empty()) throw std::runtime_error(path + ": no models (SIZE / XYZI chunks)");
    }

    void buildMaterials() {
        bool used[256] = {};
        for (const auto& vm : models)
            for (uint32_t v : vm.voxels) used[v >> 24] = true;
        bool team = false;
        for (int i = kVoxTeamFirst; i < 256; ++i) team = team || used[i];
        Material base;
        base.name = "voxel";
        m.materials.push_back(base);
        materialOf.assign(256, 0);
        for (const auto& [id, d] : matl) {
            if (id <= 0 || id > 255 || !used[id]) continue;
            auto ty = d.find("_type");
            const std::string type = ty == d.end() ? std::string() : ty->second;
            const uint32_t c = palette[id];
            const uint32_t lin = linearColor(uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16), 255);
            const float3 col{float(lin & 255) / 255.0f, float((lin >> 8) & 255) / 255.0f, float((lin >> 16) & 255) / 255.0f};
            Material mt = base;
            if (type == "_emit") {
                const float emit = dictFloat(d, "_emit", dictFloat(d, "_weight", 1));
                if (emit <= 0) continue;
                mt.name = "emit" + std::to_string(id);
                mt.emissive = col;
                mt.emissiveStrength = emit * (1 + dictFloat(d, "_flux", 0));
            } else if (type == "_metal") {
                mt.name = "metal" + std::to_string(id);
                mt.metallic = std::clamp(dictFloat(d, "_metal", dictFloat(d, "_weight", 1)), 0.0f, 1.0f);
                mt.roughness = std::clamp(dictFloat(d, "_rough", 0.3f), 0.04f, 1.0f);
            } else continue;  // diffuse, glass, cloud, ...: plain material
            materialOf[size_t(id)] = int(m.materials.size());
            m.materials.push_back(mt);
        }
        if (team)
            for (auto& mt : m.materials) mt.team = TeamMode::VertexAlpha;
    }

    int meshFor(int modelId) {
        if (modelId < 0 || modelId >= int(models.size())) { warn("shape references missing model " + std::to_string(modelId)); return -1; }
        if (meshOfModel[size_t(modelId)] != -2) return meshOfModel[size_t(modelId)];
        const VoxModel& vm = models[size_t(modelId)];
        VoxelGrid g;
        g.resize(vm.sx, vm.sy, vm.sz);
        for (uint32_t v : vm.voxels) {
            const int x = int(v & 255), y = int((v >> 8) & 255), z = int((v >> 16) & 255);
            const uint16_t k = uint16_t(v >> 24);
            if (!k) continue;
            if (x >= vm.sx || y >= vm.sy || z >= vm.sz) { warn("voxels outside their model size skipped"); continue; }
            g.key[g.index(x, y, z)] = k;
        }
        const float hx = float(vm.sx / 2), hy = float(vm.sy / 2), hz = float(vm.sz / 2);
        const int mesh = greedyMesh(m, "model" + std::to_string(modelId), g,
            [&](uint16_t k) {
                const uint32_t c = palette[k & 255];
                return FaceStyle{linearColor(uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16), k >= kVoxTeamFirst ? 0 : 255), materialOf[k & 255]};
            },
            [&](float x, float y, float z) { return voxToEngine({x - hx, y - hy, z - hz}); });
        meshOfModel[size_t(modelId)] = mesh;
        return mesh;
    }

    int addNode(const std::string& name, int parent) {
        Node n;
        n.name = name;
        n.parent = parent;
        m.nodes.push_back(n);
        return int(m.nodes.size()) - 1;
    }

    void attachShape(const VoxNode& s, int node) {
        if (s.models.size() == 1) { m.nodes[size_t(node)].mesh = meshFor(s.models[0].first); return; }
        ShapeAnim anim;
        for (size_t i = 0; i < s.models.size(); ++i) {
            const int f = std::max(0, dictInt(s.models[i].second, "_f", int(i)));
            const int child = addNode(m.nodes[size_t(node)].name + "_f" + std::to_string(f), node);
            m.nodes[size_t(child)].mesh = meshFor(s.models[i].first);
            anim.frameNodes.emplace_back(f, child);
        }
        std::stable_sort(anim.frameNodes.begin(), anim.frameNodes.end());
        for (size_t i = 1; i < anim.frameNodes.size(); ++i) m.nodes[size_t(anim.frameNodes[i].second)].s = {0, 0, 0};
        shapeAnims.push_back(std::move(anim));
    }

    // Graph node `id` below model node `parent`.
    void attach(int id, int parent, int depth) {
        auto it = graph.find(id);
        if (it == graph.end()) { warn("scene graph references missing node " + std::to_string(id)); return; }
        if (depth > 64 || !visiting.insert(id).second) { warn("scene graph has a cycle"); return; }
        const VoxNode& g = it->second;
        if (g.type == 'T') {
            if (!dictInt(g.attrs, "_hidden", 0) && !hiddenLayers.count(g.layer)) {
                auto nm = g.attrs.find("_name");
                const int node = addNode(nm != g.attrs.end() && !nm->second.empty() ? nm->second : id == 0 ? "root" : "node" + std::to_string(id), parent);
                if (!g.frames.empty()) setNodeMatrix(m.nodes[size_t(node)], voxFrameMatrix(g.frames[0]));
                if (g.frames.size() > 1) {
                    TrnAnim a{node, {}};
                    for (size_t i = 0; i < g.frames.size(); ++i)
                        a.frames.emplace_back(std::max(0, dictInt(g.frames[i], "_f", int(i))), voxFrameMatrix(g.frames[i]));
                    std::stable_sort(a.frames.begin(), a.frames.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
                    trnAnims.push_back(std::move(a));
                }
                attach(g.child, node, depth + 1);
            }
        } else if (g.type == 'G') {
            for (int c : g.children) attach(c, parent, depth + 1);
        } else if (parent >= 0) {
            attachShape(g, parent);
        } else {
            attachShape(g, addNode("root", -1));
        }
        visiting.erase(id);
    }

    void buildAnimation() {
        if (shapeAnims.empty() && trnAnims.empty()) return;
        AnimClip clip;
        clip.name = "Idle";
        int last = 0;
        for (const auto& a : shapeAnims) last = std::max(last, a.frameNodes.back().first);
        for (const auto& a : trnAnims) last = std::max(last, a.frames.back().first);
        clip.duration = float(last + 1) / kVoxFps;
        for (const auto& a : shapeAnims)
            for (const auto& [f, node] : a.frameNodes) {
                AnimChannel ch;
                ch.node = node; ch.path = AnimChannel::Scale; ch.interp = AnimChannel::Step; ch.comps = 3;
                for (const auto& [g, other] : a.frameNodes) {
                    const float s = other == node ? 1.0f : 0.0f;
                    ch.times.push_back(float(g) / kVoxFps);
                    ch.values.insert(ch.values.end(), {s, s, s});
                }
                clip.channels.push_back(std::move(ch));
            }
        for (const auto& a : trnAnims) {
            AnimChannel t, r, s;
            t.node = r.node = s.node = a.node;
            t.path = AnimChannel::Translation; r.path = AnimChannel::Rotation; s.path = AnimChannel::Scale;
            t.interp = r.interp = s.interp = AnimChannel::Step;
            r.comps = 4;
            for (const auto& [f, mx] : a.frames) {
                float3 tt, ss; quat q;
                decompose(mx, tt, q, ss);
                const float time = float(f) / kVoxFps;
                t.times.push_back(time); r.times.push_back(time); s.times.push_back(time);
                t.values.insert(t.values.end(), {tt.x, tt.y, tt.z});
                r.values.insert(r.values.end(), {q.x, q.y, q.z, q.w});
                s.values.insert(s.values.end(), {ss.x, ss.y, ss.z});
            }
            clip.channels.push_back(std::move(t));
            clip.channels.push_back(std::move(r));
            clip.channels.push_back(std::move(s));
        }
        m.clips.push_back(std::move(clip));
    }

    void build() {
        buildMaterials();
        meshOfModel.assign(models.size(), -2);
        if (graph.count(0)) {
            attach(0, -1, 0);
        } else {  // pre-0.99 files: no scene graph, several models = animation frames
            VoxNode s;
            s.type = 'S';
            for (size_t i = 0; i < models.size(); ++i) s.models.emplace_back(int(i), Dict{{"_f", std::to_string(i)}});
            const int root = addNode("root", -1);
            m.nodes[size_t(root)].t.y = float(models[0].sz / 2);  // standing on the ground, as MagicaVoxel shows it
            attachShape(s, root);
        }
        if (m.nodes.empty()) addNode("root", -1);
        bool anyMesh = false;
        for (const auto& n : m.nodes) anyMesh = anyMesh || n.mesh >= 0;
        if (!anyMesh) throw std::runtime_error(path + ": no visible voxels");
        for (const auto& n : m.nodes)
            if (n.s.x < 0 || n.s.y < 0 || n.s.z < 0) warn("mirrored nodes are drawn with a negative scale");
        buildAnimation();
    }
};

// ------------------------------------------------------------------ Westwood VXL / HVA

constexpr size_t kVxlHeader = 802, kVxlSectionHeader = 28, kVxlTailer = 92;
constexpr float kHvaFps = 15;

// Westwood voxel space (Z up, facing +X) -> engine (Y up, facing +Z): (x, y, z) -> (y, z, x)
mat4 vxlAxis() {
    mat4 c;
    c.at(0, 0) = 0; c.at(0, 1) = 1;
    c.at(1, 1) = 0; c.at(1, 2) = 1;
    c.at(2, 2) = 0; c.at(2, 0) = 1;
    return c;
}

std::string fixedName(const std::string& b, size_t o) {
    std::string s = b.substr(o, 16);
    s = s.substr(0, s.find('\0'));
    while (!s.empty() && (s.back() == ' ' || uint8_t(s.back()) < 32)) s.pop_back();
    return s;
}

struct VxlSection {
    std::string name;
    float scale = 1;
    float xf[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // tailer matrix, 3x4 row-major
    float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0}, cell[3] = {1, 1, 1};
    int size[3] = {0, 0, 0};
};

// 3x4 row-major section matrix (HVA or tailer) -> engine-space node matrix.
mat4 vxlSectionMatrix(const VxlSection& s, const float* r) {
    mat4 m;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) m.at(row, col) = s.scale * r[row * 4 + col];
    // HVA translations are in scaled voxel units (the games multiply by the section scale and voxel size)
    for (int row = 0; row < 3; ++row) m.at(row, 3) = s.scale * s.scale * r[row * 4 + 3] * s.cell[row];
    const mat4 c = vxlAxis();
    return c * m * c.inverse();
}

bool readPalette(const std::string& bytes, size_t off, uint8_t out[768]) {
    if (bytes.size() < off + 768) return false;
    int maxv = 0;
    for (size_t i = 0; i < 768; ++i) maxv = std::max(maxv, int(uint8_t(bytes[off + i])));
    if (maxv == 0) return false;
    for (size_t i = 0; i < 768; ++i) {
        const int v = uint8_t(bytes[off + i]);
        out[i] = uint8_t(maxv <= 63 ? (v * 255 + 31) / 63 : v);  // 6-bit VGA palettes
    }
    return true;
}

} // namespace

std::unique_ptr<Model> loadVox(const std::string& path, const std::string& bytes, const FileReader&, std::vector<std::string>* warnings) {
    auto model = std::make_unique<Model>();
    model->path = path;
    VoxLoader L(path, bytes, warnings, *model);
    L.parse();
    L.build();
    model->finalize();
    return model;
}

std::unique_ptr<Model> loadVxl(const std::string& path, const std::string& b, const FileReader& read, std::vector<std::string>* warnings) {
    auto warn = [&](const std::string& w) { if (warnings) warnings->push_back(path + ": " + w); };
    if (b.size() < kVxlHeader || b.compare(0, 15, "Voxel Animation") != 0)
        throw std::runtime_error(path + ": not a Westwood voxel file (no 'Voxel Animation' header)");
    const uint32_t count = u32At(b, 20), tailers = u32At(b, 24), bodySize = u32At(b, 28);
    if (count == 0 || count > 256) throw std::runtime_error(path + ": bad section count " + std::to_string(count));
    if (tailers != count) throw std::runtime_error(path + ": " + std::to_string(count) + " sections but " + std::to_string(tailers) + " section tailers");
    const size_t body = kVxlHeader + kVxlSectionHeader * count;
    if (bodySize > b.size() || body + bodySize + kVxlTailer * count > b.size()) throw std::runtime_error(path + ": truncated (section data past the end of the file)");
    const size_t tail = body + bodySize, bodyEnd = tail;

    // palette: own, sidecar, unittem.pal / unit.pal, grey ramp
    uint8_t pal[768];
    if (!readPalette(b, 34, pal)) {
        const std::string dir = dirOf(path), stem = stemOf(path);
        std::string parent = dir.size() > 1 ? dirOf(dir.substr(0, dir.size() - 1)) : std::string();
        if (parent == dir) parent.clear();
        bool found = false;
        for (const std::string& p : {stem + ".pal", dir + "unittem.pal", dir + "unit.pal", parent + "unittem.pal", parent + "unit.pal"}) {
            std::string pb;
            if (read(p, pb) && readPalette(pb, 0, pal)) { found = true; break; }
        }
        if (!found) {
            warn("no palette (the file's palette is empty and no .pal / unittem.pal next to it): grey ramp used");
            for (int i = 0; i < 256; ++i) pal[i * 3] = pal[i * 3 + 1] = pal[i * 3 + 2] = uint8_t(i);
        }
    }
    int remapFirst = uint8_t(b[32]), remapLast = uint8_t(b[33]);
    if (remapFirst == 0 || remapLast < remapFirst) { remapFirst = 16; remapLast = 31; }

    std::vector<VxlSection> sections(count);
    for (uint32_t i = 0; i < count; ++i) {
        VxlSection& s = sections[i];
        s.name = fixedName(b, kVxlHeader + kVxlSectionHeader * i);
        if (s.name.empty()) s.name = "Section" + std::to_string(i);
        const size_t t = tail + kVxlTailer * i;
        s.scale = f32At(b, t + 12);
        if (!(s.scale > 1e-6f)) s.scale = 1;
        for (int k = 0; k < 12; ++k) s.xf[k] = f32At(b, t + 16 + size_t(k) * 4);
        for (int k = 0; k < 3; ++k) {
            s.bmin[k] = f32At(b, t + 64 + size_t(k) * 4);
            s.bmax[k] = f32At(b, t + 76 + size_t(k) * 4);
            s.size[k] = uint8_t(b[t + 88 + size_t(k)]);
            s.cell[k] = s.size[k] ? (s.bmax[k] - s.bmin[k]) / float(s.size[k]) : 1.0f;
            if (!(std::fabs(s.cell[k]) > 1e-6f) || std::fabs(s.cell[k]) > 1e4f) s.cell[k] = 1;
        }
    }

    // .hva next to the .vxl
    std::vector<std::vector<const float*>> frames;  // [frame][section] -> 12 floats (nullptr = tailer)
    std::vector<float> hvaData;
    {
        const std::string stem = stemOf(path);
        std::string hb, hp = stem + ".hva";
        bool have = read(hp, hb);
        if (!have && read(stem + ".HVA", hb)) { hp = stem + ".HVA"; have = true; }
        if (have) {
            const uint32_t nf = hb.size() >= 24 ? u32At(hb, 16) : 0, ns = hb.size() >= 24 ? u32At(hb, 20) : 0;
            if (nf == 0 || ns == 0 || nf > 100000 || ns > 256 || hb.size() < 24 + size_t(ns) * 16 + size_t(nf) * ns * 48) {
                warn("'" + hp + "' is broken: sections use their default transforms");
            } else {
                std::vector<int> map(count, -1);
                for (uint32_t i = 0; i < count; ++i) {
                    for (uint32_t k = 0; k < ns; ++k)
                        if (lower(fixedName(hb, 24 + size_t(k) * 16)) == lower(sections[i].name)) { map[i] = int(k); break; }
                    if (map[i] < 0 && i < ns) map[i] = int(i);
                }
                if (ns != count) warn("'" + hp + "' has " + std::to_string(ns) + " sections, the .vxl " + std::to_string(count));
                hvaData.resize(size_t(nf) * ns * 12);
                const size_t base = 24 + size_t(ns) * 16;
                for (size_t k = 0; k < hvaData.size(); ++k) hvaData[k] = f32At(hb, base + k * 4);
                frames.assign(nf, std::vector<const float*>(count, nullptr));
                for (uint32_t f = 0; f < nf; ++f)
                    for (uint32_t i = 0; i < count; ++i)
                        if (map[i] >= 0) frames[f][i] = &hvaData[(size_t(f) * ns + size_t(map[i])) * 12];
            }
        } else if (count > 1) {
            warn("no '" + hp + "': sections use the transforms stored in the .vxl");
        }
    }
    if (frames.empty()) frames.assign(1, std::vector<const float*>(count, nullptr));

    auto model = std::make_unique<Model>();
    Model& m = *model;
    m.path = path;
    Material mat;
    mat.name = "voxel";
    m.materials.push_back(mat);
    Node root;
    root.name = "root";
    m.nodes.push_back(root);
    bool team = false, truncated = false;
    for (uint32_t i = 0; i < count; ++i) {
        const VxlSection& s = sections[i];
        Node n;
        n.name = s.name;
        n.parent = 0;
        const size_t t = tail + kVxlTailer * i;
        const size_t starts = body + u32At(b, t), data = body + u32At(b, t + 8);
        const size_t columns = size_t(s.size[0]) * size_t(s.size[1]);
        VoxelGrid g;
        if (columns && s.size[2] && starts <= bodyEnd && columns * 4 <= bodyEnd - starts && data <= bodyEnd) {
            g.resize(s.size[0], s.size[1], s.size[2]);
            for (size_t c = 0; c < columns; ++c) {
                const uint32_t start = u32At(b, starts + c * 4);
                if (start == 0xffffffffu) continue;
                if (start >= bodyEnd - data) { truncated = true; continue; }
                size_t p = data + start;
                const int x = int(c % size_t(s.size[0])), y = int(c / size_t(s.size[0]));
                int z = 0;
                while (z < s.size[2]) {
                    if (bodyEnd - p < 2) { truncated = true; break; }
                    const int skip = uint8_t(b[p]), run = uint8_t(b[p + 1]);
                    p += 2;
                    if (skip == 0 && run == 0) break;
                    z += skip;
                    if (size_t(run) * 2 + 1 > bodyEnd - p) { truncated = true; break; }
                    for (int k = 0; k < run; ++k, ++z) {
                        const int colour = uint8_t(b[p + size_t(k) * 2]);
                        if (z < s.size[2]) g.key[g.index(x, y, z)] = uint16_t(colour + 1);
                        team = team || (colour >= remapFirst && colour <= remapLast);
                    }
                    p += size_t(run) * 2 + 1;  // voxels + repeated run length
                }
            }
            const float* mn = s.bmin;
            const float* cl = s.cell;
            n.mesh = greedyMesh(m, s.name, g,
                [&](uint16_t k) {
                    const int c = k - 1;
                    const bool remap = c >= remapFirst && c <= remapLast;
                    return FaceStyle{linearColor(pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2], remap ? 0 : 255), 0};
                },
                [&](float x, float y, float z) {
                    return float3{mn[1] + y * cl[1], mn[2] + z * cl[2], mn[0] + x * cl[0]};
                });
        } else if (columns && s.size[2]) {
            truncated = true;
        }
        m.nodes.push_back(n);
    }
    if (truncated) warn("span data truncated or out of range (some voxels skipped)");
    if (team) m.materials[0].team = TeamMode::VertexAlpha;
    bool anyMesh = false;
    for (const auto& n : m.nodes) anyMesh = anyMesh || n.mesh >= 0;
    if (!anyMesh) throw std::runtime_error(path + ": no voxels");

    // world matrices per frame and section; a barrel follows the turret
    const size_t nf = frames.size();
    std::vector<std::vector<mat4>> world(nf, std::vector<mat4>(count));
    for (size_t f = 0; f < nf; ++f)
        for (uint32_t i = 0; i < count; ++i) world[f][i] = vxlSectionMatrix(sections[i], frames[f][i] ? frames[f][i] : sections[i].xf);
    int turret = -1, barrel = -1;
    for (uint32_t i = 0; i < count; ++i) {
        const std::string nm = lower(sections[i].name);
        if (turret < 0 && nm.find("turret") != std::string::npos) turret = int(i);
        else if (barrel < 0 && (nm.find("barrel") != std::string::npos || nm.find("barl") != std::string::npos)) barrel = int(i);
    }
    auto local = [&](size_t f, uint32_t i) {
        return turret >= 0 && barrel == int(i) ? world[f][size_t(turret)].inverse() * world[f][i] : world[f][i];
    };
    if (turret >= 0 && barrel >= 0) m.nodes[size_t(barrel) + 1].parent = turret + 1;
    for (uint32_t i = 0; i < count; ++i) setNodeMatrix(m.nodes[i + 1], local(0, i));
    if (nf > 1) {
        AnimClip clip;
        clip.name = "Idle";
        clip.duration = float(nf) / kHvaFps;
        for (uint32_t i = 0; i < count; ++i) {
            AnimChannel t, r, s;
            t.node = r.node = s.node = int(i) + 1;
            t.path = AnimChannel::Translation; r.path = AnimChannel::Rotation; s.path = AnimChannel::Scale;
            t.interp = r.interp = s.interp = AnimChannel::Step;
            r.comps = 4;
            for (size_t f = 0; f < nf; ++f) {
                float3 tt, ss; quat q;
                decompose(local(f, i), tt, q, ss);
                const float time = float(f) / kHvaFps;
                t.times.push_back(time); r.times.push_back(time); s.times.push_back(time);
                t.values.insert(t.values.end(), {tt.x, tt.y, tt.z});
                r.values.insert(r.values.end(), {q.x, q.y, q.z, q.w});
                s.values.insert(s.values.end(), {ss.x, ss.y, ss.z});
            }
            clip.channels.push_back(std::move(t));
            clip.channels.push_back(std::move(r));
            clip.channels.push_back(std::move(s));
        }
        m.clips.push_back(std::move(clip));
    }
    m.finalize();
    return model;
}

} // namespace rts::render
