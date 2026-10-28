// Westwood 3D .w3d loader (C&C Generals / Zero Hour, Renegade-era files share the chunks), read from
// the user's own copy of the game.
//
// Supported chunks: MESH (header 3, vertices, normals, texcoords, triangles, vertex influences incl.
// the second bone + weights of later exporters, vertex materials, shaders, textures, material pass 0:
// vertex material / shader / texture ids, stage texcoords, DCG colours), HIERARCHY (pivots -> nodes,
// names kept exactly: "Turret", "Barrel", "FireFX01" ...), HLOD (the highest-detail LOD array and the
// aggregate array; sub-objects "Container.Mesh" bound to bones), HMODEL (older files), ANIMATION
// (classic X/Y/Z/Q channels) and COMPRESSED_ANIMATION (time-coded and adaptive-delta channels).
// Ignored: bit channels (visibility), morph animations, AABTrees, emitters, boxes, extra material
// passes (warning), Euler channels (warning).
//
// Axes: W3D is Z up with units facing +X (3ds Max). Everything (pivots, vertices, keys) is mapped
// into the engine frame with engine (x, y, z) = w3d (y, z, x): a proper rotation, so a pivot that is
// unrotated in the file stays unrotated and procedural turret aim (local Y) still turns about up.
// Native W3D scale is kept.
// Rigid meshes are in the space of their bone; skinned meshes store each vertex in the space of its
// (first) bone: they are moved to model space and get inverse bind matrices from the hierarchy.
// Animation keys follow WW3D: local = pivot base (T, R) * translate(channel) * rotate(channel).
//
// External files (same folder, name as written / lower / upper case):
//   hierarchy   "<HierarchyName>.w3d" when the model references a hierarchy it doesn't contain;
//               animations inside that file are used too
//   animations  "<model>.anims.txt" next to the model: one .w3d per line (relative to the model
//               folder, '#' comments), e.g. "AVTank_SKL.AVTank_D1.w3d"; their ANIMATION /
//               COMPRESSED_ANIMATION chunks for the model's hierarchy become clips named after the
//               animation (the part after a '.', as in "Hierarchy.Anim").
// Textures: the name from the file, then with .dds / .tga, in the model folder, ../Textures/,
// ../textures/, textures/, Art/Textures/ (Generals layout: Art/W3D + Art/Textures).
// House (team) colour, as in Generals' W3DAssetManager: meshes named "HOUSECOLOR..." get their
// colour replaced -> TeamMode::Tint; textures "ZHCA*" carry it in alpha 0 -> TeamMode::BaseAlpha;
// palette-row "ZHCD*" textures -> Tint (approximation, warning).
#include "Model.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace rts::render {

namespace {

constexpr uint32_t kMesh = 0x0, kVertices = 0x2, kNormals = 0x3, kTexcoords = 0x5, kInfluences = 0xE,
                   kMeshHeader3 = 0x1F, kTriangles = 0x20, kShaders = 0x29, kVertexMaterials = 0x2A,
                   kVertexMaterial = 0x2B, kVertexMaterialInfo = 0x2D, kTextures = 0x30, kTexture = 0x31,
                   kTextureName = 0x32, kMaterialPass = 0x38, kVertexMaterialIds = 0x39, kShaderIds = 0x3A,
                   kDcg = 0x3B, kTextureStage = 0x48, kTextureIds = 0x49, kStageTexcoords = 0x4A,
                   kHierarchy = 0x100, kHierarchyHeader = 0x101, kPivots = 0x102,
                   kAnimation = 0x200, kAnimationHeader = 0x201, kAnimationChannel = 0x202,
                   kCompressedAnimation = 0x280, kCompressedAnimationHeader = 0x281, kCompressedAnimationChannel = 0x282,
                   kHModel = 0x300, kHModelHeader = 0x301, kHModelNode = 0x302, kHModelSkinNode = 0x304,
                   kHlod = 0x700, kHlodHeader = 0x701, kHlodLodArray = 0x702,
                   kHlodSubObject = 0x704, kHlodAggregateArray = 0x705;
constexpr uint32_t kFlagHidden = 0x1000, kFlagTwoSided = 0x2000, kGeomMask = 0x00FF0000, kGeomSkin = 0x00020000;

std::string lowerStr(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string upperStr(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool startsWithCi(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && lowerStr(s.substr(0, n)) == lowerStr(prefix);
}
std::string dirOf(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
std::string stemPath(const std::string& p) {
    const size_t dot = p.find_last_of('.'), slash = p.find_last_of("/\\");
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? p : p.substr(0, dot);
}
std::string afterDot(const std::string& s) {
    const size_t d = s.find('.');
    return d == std::string::npos ? s : s.substr(d + 1);
}
float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float3 conv(float3 w) { return {w.y, w.z, w.x}; }                 // w3d -> engine axes
quat convQ(quat q) { return {q.y, q.z, q.x, q.w}; }              // same change of basis on a rotation

// A chunk payload [at, at + size) inside the file.
struct Span { size_t at = 0, size = 0; };

struct Reader {
    const std::string& b;
    const std::string& path;
    void need(Span s, size_t off, size_t n, const char* what) const {
        if (off > s.size || n > s.size - off) throw std::runtime_error(path + ": " + what + " chunk is too short (" + std::to_string(s.size) + " bytes)");
    }
    uint32_t u32(size_t at) const { uint32_t v; std::memcpy(&v, b.data() + at, 4); return v; }
    uint16_t u16(size_t at) const { uint16_t v; std::memcpy(&v, b.data() + at, 2); return v; }
    uint8_t u8(size_t at) const { return uint8_t(b[at]); }
    float f32(size_t at) const { float v; std::memcpy(&v, b.data() + at, 4); return std::isfinite(v) ? v : 0.0f; }
    float3 vec(size_t at) const { return {f32(at), f32(at + 4), f32(at + 8)}; }
    std::string name(size_t at, size_t len) const {
        std::string s;
        for (size_t i = 0; i < len && b[at + i]; ++i) s += b[at + i];
        return s;
    }
    std::string str(Span s) const { return name(s.at, s.size); }
    // Children of a container chunk (or the whole file); throws on chunks running past their parent.
    template <class F> void each(Span parent, F&& f) const {
        size_t p = parent.at, end = parent.at + parent.size;
        while (p < end) {
            if (end - p < 8) throw std::runtime_error(path + ": truncated chunk header at offset " + std::to_string(p));
            const uint32_t id = u32(p), size = u32(p + 4) & 0x7fffffffu;
            if (size > end - p - 8) throw std::runtime_error(path + ": chunk 0x" + hex(id) + " at offset " + std::to_string(p) + " runs past the end of its parent (truncated file?)");
            f(id, Span{p + 8, size});
            p += 8 + size;
        }
    }
    static std::string hex(uint32_t v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%X", v);
        return buf;
    }
};

struct VertMat { float3 diffuse{1, 1, 1}, emissive{0, 0, 0}; float opacity = 1; };
struct Shader { uint8_t depthMask = 1, destBlend = 0, priGradient = 1, srcBlend = 1, texturing = 0, alphaTest = 0; };

struct W3dMesh {
    std::string name, container, full;
    uint32_t attrs = 0;
    std::vector<float3> pos, nrm;
    std::vector<float2> uv;
    std::vector<uint32_t> tris;          // 3 per triangle
    std::vector<uint16_t> bone, bone2;
    std::vector<float> w1;               // weight of the first bone (1 = rigid to it)
    std::vector<VertMat> vms;
    std::vector<Shader> shaders;
    std::vector<std::string> textures;
    std::vector<uint32_t> vmIds, shaderIds, texIds, dcg;
    std::vector<float2> stageUv;
    int passes = 0;
    bool skin() const { return (attrs & kGeomMask) == kGeomSkin || !bone.empty(); }
};

struct Pivot { std::string name; uint32_t parent = 0xffffffffu; float3 t; quat r; };
struct Hierarchy { std::string name; std::vector<Pivot> pivots; };
struct SubObject { uint32_t bone = 0; std::string name; };
struct Lod { std::string name, hierarchy; std::vector<SubObject> objects; };

// Dense per-frame samples of one pivot.
struct PivotTrack { bool t = false, r = false; std::vector<float3> dt; std::vector<quat> q; };
struct Anim { std::string name, hierarchy; int frames = 0; float fps = 30; std::map<uint32_t, PivotTrack> tracks; };

class W3dLoader {
public:
    W3dLoader(const std::string& path, const FileReader& read, std::vector<std::string>* warnings)
        : path_(path), dir_(dirOf(path)), read_(read), warnings_(warnings) {}

    std::unique_ptr<Model> load(const std::string& bytes) {
        File main;
        parseFile(path_, bytes, main, true);
        if (main.meshes.empty()) throw std::runtime_error(path_ + ": no meshes (a hierarchy or animation file? load the model that uses it)");
        // which hierarchy the model uses
        std::string hierName = main.lod ? main.lod->hierarchy : std::string();
        const Hierarchy* hier = nullptr;
        for (auto& h : main.hierarchies) if (hierName.empty() || lowerStr(h.name) == lowerStr(hierName)) { hier = &h; break; }
        File ext;
        if (!hier && !hierName.empty()) {
            std::string extPath, extBytes;
            for (const auto& n : {hierName, lowerStr(hierName), upperStr(hierName)})
                if (read_(dir_ + n + ".w3d", extBytes) || read_(dir_ + n + ".W3D", extBytes)) { extPath = dir_ + n + ".w3d"; break; }
            if (!extPath.empty()) {
                parseFile(extPath, extBytes, ext, false);
                for (auto& h : ext.hierarchies) if (lowerStr(h.name) == lowerStr(hierName)) { hier = &h; break; }
                if (!hier && !ext.hierarchies.empty()) hier = &ext.hierarchies[0];
            }
            if (!hier) warn("hierarchy '" + hierName + "' not found (looked for " + dir_ + hierName + ".w3d); meshes are placed at the origin");
        }
        model_ = std::make_unique<Model>();
        buildNodes(hier);
        placeMeshes(main, hier);
        if (hier) {
            std::vector<Anim> anims = main.anims;
            anims.insert(anims.end(), ext.anims.begin(), ext.anims.end());
            loadSidecarAnims(anims);
            for (const Anim& a : anims) addClip(a, *hier);
        } else if (!main.anims.empty()) warn("animations ignored: the model has no hierarchy");
        if (extraPasses_ > 0) warn(std::to_string(extraPasses_) + " mesh(es) with more than one material pass: only the first pass is used");
        if (model_->meshes.empty()) throw std::runtime_error(path_ + ": no visible meshes");
        model_->finalize();
        return std::move(model_);
    }

private:
    struct File {
        std::vector<W3dMesh> meshes;
        std::vector<Hierarchy> hierarchies;
        std::vector<Anim> anims;
        std::unique_ptr<Lod> lod;
    };

    void warn(const std::string& w) {
        if (warned_.insert(w).second && warnings_) warnings_->push_back(path_ + ": " + w);
    }

    // ---- parsing ----

    void parseFile(const std::string& path, const std::string& bytes, File& out, bool isModel) {
        Reader r{bytes, path};
        r.each(Span{0, bytes.size()}, [&](uint32_t id, Span s) {
            switch (id) {
                case kMesh: if (isModel) out.meshes.push_back(parseMesh(r, s)); break;
                case kHierarchy: out.hierarchies.push_back(parseHierarchy(r, s)); break;
                case kAnimation: out.anims.push_back(parseAnim(r, s)); break;
                case kCompressedAnimation: out.anims.push_back(parseCompressedAnim(r, s)); break;
                case kHlod: if (isModel) parseHlod(r, s, out); break;
                case kHModel: if (isModel && !out.lod) parseHModel(r, s, out); break;
                default: break;
            }
        });
        if (isModel && bytes.size() >= 8 && out.meshes.empty() && out.hierarchies.empty() && out.anims.empty() && !out.lod)
            throw std::runtime_error(path + ": no W3D chunks found (not a .w3d file?)");
    }

    template <class T, class F> static void array(const Reader& r, Span s, size_t stride, std::vector<T>& out, F&& f) {
        out.resize(s.size / stride);
        for (size_t i = 0; i < out.size(); ++i) out[i] = f(s.at + i * stride);
        (void)r;
    }

    W3dMesh parseMesh(const Reader& r, Span s) {
        W3dMesh m;
        bool header = false;
        r.each(s, [&](uint32_t id, Span c) {
            switch (id) {
                case kMeshHeader3:
                    r.need(c, 0, 116, "MESH_HEADER3");
                    m.attrs = r.u32(c.at + 4);
                    m.name = r.name(c.at + 8, 16);
                    m.container = r.name(c.at + 24, 16);
                    header = true;
                    break;
                case kVertices: array(r, c, 12, m.pos, [&](size_t a) { return r.vec(a); }); break;
                case kNormals: array(r, c, 12, m.nrm, [&](size_t a) { return r.vec(a); }); break;
                case kTexcoords: array(r, c, 8, m.uv, [&](size_t a) { return float2{r.f32(a), 1 - r.f32(a + 4)}; }); break;
                case kTriangles:
                    m.tris.resize(c.size / 32 * 3);
                    for (size_t i = 0; i < m.tris.size(); ++i) m.tris[i] = r.u32(c.at + (i / 3) * 32 + (i % 3) * 4);
                    break;
                case kInfluences: {
                    const size_t n = c.size / 8;
                    m.bone.resize(n); m.bone2.resize(n); m.w1.resize(n);
                    for (size_t i = 0; i < n; ++i) {
                        const size_t a = c.at + i * 8;
                        m.bone[i] = r.u16(a);
                        m.bone2[i] = r.u16(a + 2);
                        const float wa = r.u16(a + 4), wb = r.u16(a + 6);   // percent (BFME-era exporters), zero in Generals
                        m.w1[i] = wb > 0 && wa + wb > 0 ? wa / (wa + wb) : 1.0f;
                    }
                    break;
                }
                case kVertexMaterials:
                    r.each(c, [&](uint32_t vid, Span v) {
                        if (vid != kVertexMaterial) return;
                        VertMat vm;
                        r.each(v, [&](uint32_t iid, Span info) {
                            if (iid != kVertexMaterialInfo) return;
                            r.need(info, 0, 32, "VERTEX_MATERIAL_INFO");
                            auto rgb = [&](size_t a) { return float3{srgbToLinear(r.u8(a) / 255.0f), srgbToLinear(r.u8(a + 1) / 255.0f), srgbToLinear(r.u8(a + 2) / 255.0f)}; };
                            vm.diffuse = rgb(info.at + 8);
                            vm.emissive = rgb(info.at + 16);
                            vm.opacity = std::clamp(r.f32(info.at + 24), 0.0f, 1.0f);
                        });
                        m.vms.push_back(vm);
                    });
                    break;
                case kShaders:
                    array(r, c, 16, m.shaders, [&](size_t a) {
                        Shader sh;
                        sh.depthMask = r.u8(a + 1); sh.destBlend = r.u8(a + 3); sh.priGradient = r.u8(a + 5);
                        sh.srcBlend = r.u8(a + 7); sh.texturing = r.u8(a + 8); sh.alphaTest = r.u8(a + 12);
                        return sh;
                    });
                    break;
                case kTextures:
                    r.each(c, [&](uint32_t tid, Span t) {
                        if (tid != kTexture) return;
                        std::string name;
                        r.each(t, [&](uint32_t nid, Span n) { if (nid == kTextureName) name = r.str(n); });
                        m.textures.push_back(name);
                    });
                    break;
                case kMaterialPass:
                    if (m.passes++ > 0) return;   // only pass 0 is drawn
                    r.each(c, [&](uint32_t pid, Span p) {
                        switch (pid) {
                            case kVertexMaterialIds: array(r, p, 4, m.vmIds, [&](size_t a) { return r.u32(a); }); break;
                            case kShaderIds: array(r, p, 4, m.shaderIds, [&](size_t a) { return r.u32(a); }); break;
                            case kDcg: array(r, p, 4, m.dcg, [&](size_t a) { return r.u32(a); }); break;
                            case kTextureStage: {
                                if (!m.texIds.empty() || !m.stageUv.empty()) break;   // stage 0 only
                                r.each(p, [&](uint32_t sid, Span st) {
                                    if (sid == kTextureIds) array(r, st, 4, m.texIds, [&](size_t a) { return r.u32(a); });
                                    else if (sid == kStageTexcoords) array(r, st, 8, m.stageUv, [&](size_t a) { return float2{r.f32(a), 1 - r.f32(a + 4)}; });
                                });
                                break;
                            }
                            default: break;
                        }
                    });
                    break;
                default: break;
            }
        });
        if (!header) throw std::runtime_error(path_ + ": mesh without MESH_HEADER3 (pre-3.0 W3D files are not supported)");
        m.full = m.container.empty() ? m.name : m.container + "." + m.name;
        if (m.passes > 1) ++extraPasses_;
        const size_t nv = m.pos.size();
        for (uint32_t i : m.tris)
            if (i >= nv) throw std::runtime_error(path_ + ": mesh '" + m.full + "': triangle vertex index " + std::to_string(i) + " out of range (" + std::to_string(nv) + " vertices)");
        return m;
    }

    Hierarchy parseHierarchy(const Reader& r, Span s) {
        Hierarchy h;
        r.each(s, [&](uint32_t id, Span c) {
            if (id == kHierarchyHeader) { r.need(c, 0, 36, "HIERARCHY_HEADER"); h.name = r.name(c.at + 4, 16); }
            else if (id == kPivots) {
                array(r, c, 60, h.pivots, [&](size_t a) {
                    Pivot p;
                    p.name = r.name(a, 16);
                    p.parent = r.u32(a + 16);
                    p.t = r.vec(a + 20);
                    p.r = qnormalize({r.f32(a + 44), r.f32(a + 48), r.f32(a + 52), r.f32(a + 56)});
                    return p;
                });
            }
        });
        for (size_t i = 0; i < h.pivots.size(); ++i) {
            const uint32_t p = h.pivots[i].parent;
            if (p != 0xffffffffu && p >= i)
                throw std::runtime_error(path_ + ": hierarchy '" + h.name + "': pivot " + std::to_string(i) + " ('" + h.pivots[i].name + "') has parent " + std::to_string(p) + " (must come before it)");
        }
        return h;
    }

    void parseHlod(const Reader& r, Span s, File& out) {
        auto lod = std::make_unique<Lod>();
        std::vector<SubObject> aggregates;
        r.each(s, [&](uint32_t id, Span c) {
            if (id == kHlodHeader) {
                r.need(c, 0, 40, "HLOD_HEADER");
                lod->name = r.name(c.at + 8, 16);
                lod->hierarchy = r.name(c.at + 24, 16);
            } else if (id == kHlodLodArray || id == kHlodAggregateArray) {
                std::vector<SubObject> objs;
                r.each(c, [&](uint32_t sid, Span o) {
                    if (sid == kHlodSubObject) { r.need(o, 0, 36, "HLOD_SUB_OBJECT"); objs.push_back({r.u32(o.at), r.name(o.at + 4, 32)}); }
                });
                if (id == kHlodAggregateArray) aggregates.insert(aggregates.end(), objs.begin(), objs.end());
                else lod->objects = std::move(objs);   // WW3D: the last LOD array is the highest detail
            }
        });
        lod->objects.insert(lod->objects.end(), aggregates.begin(), aggregates.end());
        out.lod = std::move(lod);
    }

    void parseHModel(const Reader& r, Span s, File& out) {
        auto lod = std::make_unique<Lod>();
        r.each(s, [&](uint32_t id, Span c) {
            if (id == kHModelHeader) { r.need(c, 0, 38, "HMODEL_HEADER"); lod->name = r.name(c.at + 4, 16); lod->hierarchy = r.name(c.at + 20, 16); }
            else if (id == kHModelNode || id == kHModelSkinNode) { r.need(c, 0, 18, "HMODEL_NODE"); lod->objects.push_back({r.u16(c.at + 16), lod->name + "." + r.name(c.at, 16)}); }
        });
        out.lod = std::move(lod);
    }

    static void channelKind(uint32_t flags, int& comp) {
        switch (flags) {
            case 0: case 7: case 11: comp = 0; break;
            case 1: case 8: case 12: comp = 1; break;
            case 2: case 9: case 13: comp = 2; break;
            case 6: case 10: case 14: comp = 3; break;
            default: comp = -1; break;   // Euler XR/YR/ZR
        }
    }
    PivotTrack& track(Anim& a, uint32_t pivot) {
        PivotTrack& t = a.tracks[pivot];
        if (t.dt.empty()) { t.dt.assign(size_t(a.frames), float3{}); t.q.assign(size_t(a.frames), quat{}); }
        return t;
    }
    // value of component comp (0..2 translation axis, 3 rotation) at frame f
    static void store(PivotTrack& t, int comp, int f, const float* v) {
        if (comp == 3) { t.r = true; t.q[size_t(f)] = qnormalize({v[0], v[1], v[2], v[3]}); }
        else { t.t = true; (comp == 0 ? t.dt[size_t(f)].x : comp == 1 ? t.dt[size_t(f)].y : t.dt[size_t(f)].z) = v[0]; }
    }

    Anim parseAnim(const Reader& r, Span s) {
        Anim a;
        bool header = false;
        r.each(s, [&](uint32_t id, Span c) {
            if (id == kAnimationHeader) {
                r.need(c, 0, 44, "ANIMATION_HEADER");
                a.name = r.name(c.at + 4, 16); a.hierarchy = r.name(c.at + 20, 16);
                a.frames = int(std::min<uint32_t>(r.u32(c.at + 36), 100000));
                a.fps = float(std::max<uint32_t>(1, r.u32(c.at + 40)));
                header = true;
            } else if (id == kAnimationChannel && header) {
                r.need(c, 0, 12, "ANIMATION_CHANNEL");
                const int first = r.u16(c.at), last = r.u16(c.at + 2), len = r.u16(c.at + 4);
                const uint32_t flags = r.u16(c.at + 6), pivot = r.u16(c.at + 8);
                int comp;
                channelKind(flags, comp);
                if (comp < 0) { warn("animation '" + a.name + "': Euler rotation channels are ignored"); return; }
                if (len != (comp == 3 ? 4 : 1) || last < first) return;
                const size_t n = size_t(last - first + 1);
                r.need(c, 12, n * size_t(len) * 4, "ANIMATION_CHANNEL");
                PivotTrack& t = track(a, pivot);
                // outside [first, last] the channel is identity (WW3D MotionChannelClass)
                for (int f = 0; f < a.frames; ++f) {
                    float v[4] = {0, 0, 0, 1};
                    if (f >= first && f <= last)
                        for (int k = 0; k < len; ++k) v[k] = r.f32(c.at + 12 + (size_t(f - first) * size_t(len) + size_t(k)) * 4);
                    store(t, comp, f, v);
                }
            }
        });
        if (!header) throw std::runtime_error(path_ + ": animation without ANIMATION_HEADER");
        return a;
    }

    Anim parseCompressedAnim(const Reader& r, Span s) {
        Anim a;
        int flavor = -1;
        r.each(s, [&](uint32_t id, Span c) {
            if (id == kCompressedAnimationHeader) {
                r.need(c, 0, 44, "COMPRESSED_ANIMATION_HEADER");
                a.name = r.name(c.at + 4, 16); a.hierarchy = r.name(c.at + 20, 16);
                a.frames = int(std::min<uint32_t>(r.u32(c.at + 36), 100000));
                a.fps = float(std::max<uint16_t>(1, r.u16(c.at + 40)));
                flavor = r.u16(c.at + 42);
                if (flavor > 1) warn("animation '" + a.name + "': unknown compression flavor " + std::to_string(flavor) + "; skipped");
            } else if (id == kCompressedAnimationChannel && (flavor == 0 || flavor == 1)) {
                if (flavor == 0) timeCodedChannel(r, c, a);
                else adaptiveDeltaChannel(r, c, a);
            }
        });
        if (flavor < 0) throw std::runtime_error(path_ + ": compressed animation without header");
        return a;
    }

    void timeCodedChannel(const Reader& r, Span c, Anim& a) {
        r.need(c, 0, 8, "COMPRESSED_ANIMATION_CHANNEL");
        const uint32_t count = r.u32(c.at), pivot = r.u16(c.at + 4);
        const int len = r.u8(c.at + 6);
        int comp;
        channelKind(r.u8(c.at + 7), comp);
        if (comp < 0) { warn("animation '" + a.name + "': Euler rotation channels are ignored"); return; }
        if (len != (comp == 3 ? 4 : 1) || count == 0) return;
        const size_t packet = size_t(len + 1) * 4;
        r.need(c, 8, size_t(count) * packet, "COMPRESSED_ANIMATION_CHANNEL");
        auto timeOf = [&](uint32_t i) { return r.u32(c.at + 8 + i * packet) & 0x7fffffffu; };
        auto binary = [&](uint32_t i) { return (r.u32(c.at + 8 + i * packet) & 0x80000000u) != 0; };
        auto value = [&](uint32_t i, int k) { return r.f32(c.at + 8 + i * packet + 4 + size_t(k) * 4); };
        PivotTrack& t = track(a, pivot);
        uint32_t i = 0;
        for (int f = 0; f < a.frames; ++f) {
            while (i + 1 < count && timeOf(i + 1) <= uint32_t(f)) ++i;
            float v[4] = {0, 0, 0, 1};
            for (int k = 0; k < len; ++k) v[k] = value(i, k);
            if (i + 1 < count && !binary(i + 1) && timeOf(i + 1) > timeOf(i) && uint32_t(f) > timeOf(i)) {
                const float u = float(uint32_t(f) - timeOf(i)) / float(timeOf(i + 1) - timeOf(i));
                if (comp == 3) {
                    const quat q = slerp({v[0], v[1], v[2], v[3]}, {value(i + 1, 0), value(i + 1, 1), value(i + 1, 2), value(i + 1, 3)}, u);
                    v[0] = q.x; v[1] = q.y; v[2] = q.z; v[3] = q.w;
                } else v[0] += (value(i + 1, 0) - v[0]) * u;
            }
            store(t, comp, f, v);
        }
    }

    // Adaptive delta: initial values, then per 16 frames and component a 9-byte packet (filter
    // index + 16 signed nybbles); value += nybble * filter[index] * scale (WW3D motchan.cpp).
    void adaptiveDeltaChannel(const Reader& r, Span c, Anim& a) {
        r.need(c, 0, 12, "COMPRESSED_ANIMATION_CHANNEL");
        const uint32_t frames = r.u32(c.at), pivot = r.u16(c.at + 4);
        const int len = r.u8(c.at + 6);
        int comp;
        channelKind(r.u8(c.at + 7), comp);
        const float scale = r.f32(c.at + 8);
        if (comp < 0) { warn("animation '" + a.name + "': Euler rotation channels are ignored"); return; }
        if (len != (comp == 3 ? 4 : 1) || frames == 0) return;
        const size_t blocks = (frames - 1 + 15) / 16;
        r.need(c, 12, size_t(len) * 4 + blocks * 9 * size_t(len), "COMPRESSED_ANIMATION_CHANNEL");
        static const std::array<float, 256> table = [] {
            std::array<float, 256> t{};
            for (int i = 0; i < 16; ++i) t[size_t(i)] = std::pow(10.0f, float(i - 8));
            for (int i = 16; i < 256; ++i) t[size_t(i)] = 1.0f - std::sin(float(i - 16) / 240.0f * 1.5707963f);
            return t;
        }();
        PivotTrack& t = track(a, pivot);
        float cur[4] = {0, 0, 0, 1};
        for (int k = 0; k < len; ++k) cur[k] = r.f32(c.at + 12 + size_t(k) * 4);
        const size_t packets = c.at + 12 + size_t(len) * 4;
        for (int f = 0; f < a.frames; ++f) {
            if (f > 0 && uint32_t(f) < frames) {
                const size_t blk = size_t(f - 1) / 16, fi = size_t(f - 1) % 16;
                for (int k = 0; k < len; ++k) {
                    const size_t pk = packets + blk * 9 * size_t(len) + size_t(k) * 9;
                    int nyb = (r.u8(pk + 1 + fi / 2) >> ((fi & 1) * 4)) & 15;
                    if (nyb & 8) nyb -= 16;
                    cur[k] += float(nyb) * table[r.u8(pk)] * scale;
                }
            }
            float v[4] = {cur[0], cur[1], cur[2], cur[3]};
            store(t, comp, f, v);
        }
    }

    // ---- model ----

    void buildNodes(const Hierarchy* hier) {
        Model& m = *model_;
        if (!hier || hier->pivots.empty()) {
            Node root; root.name = "root";
            m.nodes.push_back(root);
            return;
        }
        for (const Pivot& p : hier->pivots) {
            Node n;
            n.name = p.name;
            n.parent = p.parent == 0xffffffffu ? -1 : int(p.parent);
            n.t = conv(p.t);
            n.r = convQ(p.r);
            m.nodes.push_back(n);
        }
        // bind pose world matrices (parents come first) for skins
        bindWorld_.resize(m.nodes.size());
        for (size_t i = 0; i < m.nodes.size(); ++i) {
            const Node& n = m.nodes[i];
            const mat4 l = mat4::trs(n.t, n.r, n.s);
            bindWorld_[i] = n.parent >= 0 ? bindWorld_[size_t(n.parent)] * l : l;
        }
    }

    void placeMeshes(const File& f, const Hierarchy* hier) {
        std::map<std::string, size_t> byName;
        for (size_t i = 0; i < f.meshes.size(); ++i) {
            byName[lowerStr(f.meshes[i].full)] = i;
            byName.emplace(lowerStr(f.meshes[i].name), i);
        }
        const int pivots = hier ? int(hier->pivots.size()) : 0;
        if (f.lod) {
            std::set<size_t> used;
            for (const SubObject& o : f.lod->objects) {
                auto it = byName.find(lowerStr(o.name));
                if (it == byName.end()) {
                    if (o.name.find('.') != std::string::npos && !byName.count(lowerStr(afterDot(o.name)))) continue;   // emitters, boxes, other files
                    it = byName.find(lowerStr(afterDot(o.name)));
                    if (it == byName.end()) continue;
                }
                if (!used.insert(it->second).second) continue;
                int bone = int(o.bone);
                if (bone >= pivots) {
                    if (pivots > 0) warn("sub-object '" + o.name + "' bound to missing bone " + std::to_string(o.bone));
                    bone = 0;
                }
                addMesh(f.meshes[it->second], bone, hier);
            }
        } else {
            for (const W3dMesh& mesh : f.meshes) addMesh(mesh, 0, hier);
        }
    }

    int material(const W3dMesh& mesh, uint32_t vmId, uint32_t shId, uint32_t texId) {
        const bool house = startsWithCi(mesh.name, "HOUSECOLOR");
        const bool twoSided = (mesh.attrs & kFlagTwoSided) != 0;
        const VertMat vm = vmId < mesh.vms.size() ? mesh.vms[vmId] : VertMat{};
        const Shader sh = shId < mesh.shaders.size() ? mesh.shaders[shId] : Shader{1, 0, 1, 1, 1, 0};
        std::string tex = texId < mesh.textures.size() && sh.texturing ? mesh.textures[texId] : std::string();
        if (texId < mesh.textures.size() && mesh.shaders.empty()) tex = mesh.textures[texId];
        char key[512];
        std::snprintf(key, sizeof key, "%s|%d%d|%.4f %.4f %.4f %.4f|%.4f %.4f %.4f|%d %d %d %d %d", tex.c_str(), house, twoSided,
                      vm.diffuse.x, vm.diffuse.y, vm.diffuse.z, vm.opacity, vm.emissive.x, vm.emissive.y, vm.emissive.z,
                      sh.depthMask, sh.destBlend, sh.srcBlend, sh.alphaTest, sh.priGradient);
        auto it = materials_.find(key);
        if (it != materials_.end()) return it->second;
        Material mt;
        mt.name = tex.empty() ? mesh.name : stemPath(tex);
        mt.baseColor = {vm.diffuse.x, vm.diffuse.y, vm.diffuse.z, vm.opacity};
        mt.emissive = vm.emissive;
        mt.roughness = 0.7f;
        mt.doubleSided = twoSided;
        if (sh.priGradient == 0) mt.shading = ShadingModel::Unlit;
        if (sh.alphaTest) mt.alpha = AlphaMode::Mask;
        if (sh.destBlend != 0 || sh.srcBlend == 2 || vm.opacity < 0.999f) {
            mt.alpha = AlphaMode::Blend;
            if (sh.destBlend == 1 || sh.destBlend == 2) warn("additive / multiplicative shader blending approximated by alpha blending");
            if (!sh.depthMask) mt.castShadow = false;
        }
        if (!tex.empty()) {
            mt.baseTex = image(tex);
            if (startsWithCi(tex, "ZHCA") && mt.baseTex >= 0) { mt.team = TeamMode::BaseAlpha; if (mt.alpha == AlphaMode::Mask) mt.alpha = AlphaMode::Opaque; }
            else if (startsWithCi(tex, "ZHCD")) { mt.team = TeamMode::Tint; warn("house colour palette texture '" + tex + "' approximated by a team tint"); }
        }
        if (house) { mt.team = TeamMode::Tint; mt.baseColor = {1, 1, 1, mt.baseColor.w}; }
        const int idx = int(model_->materials.size());
        model_->materials.push_back(mt);
        materials_[key] = idx;
        return idx;
    }

    int image(const std::string& tex) {
        auto it = images_.find(lowerStr(tex));
        if (it != images_.end()) return it->second;
        const std::string stem = stemPath(tex);
        std::vector<std::string> names = {tex, stem + ".dds", stem + ".tga", lowerStr(tex), lowerStr(stem) + ".dds", lowerStr(stem) + ".tga",
                                          stem + ".DDS", stem + ".TGA"};
        const std::vector<std::string> dirs = {dir_, dir_ + "../Textures/", dir_ + "../textures/", dir_ + "textures/", dir_ + "Textures/", dir_ + "Art/Textures/"};
        int id = -1;
        std::string bytes;
        bool found = false;
        for (const auto& d : dirs) {
            for (const auto& n : names) {
                if (!read_(d + n, bytes)) continue;
                found = true;
                Image img;
                img.name = tex;
                if (decodeImage(bytes, img)) { id = int(model_->images.size()); model_->images.push_back(std::move(img)); }
                else warn("texture '" + d + n + "': unsupported image format");
                break;
            }
            if (found) break;
        }
        if (!found) warn("texture '" + tex + "' not found (looked next to the model, in ../Textures/, textures/ and Art/Textures/)");
        images_[lowerStr(tex)] = id;
        return id;
    }

    struct RawVertex { ModelVertex v; int j[2] = {0, 0}; float w[2] = {1, 0}; };

    void addMesh(const W3dMesh& mesh, int bone, const Hierarchy* hier) {
        if (mesh.attrs & kFlagHidden) return;   // collision / hidden helper geometry
        if (mesh.tris.empty() || mesh.pos.empty()) return;
        Model& m = *model_;
        const size_t nv = mesh.pos.size(), nt = mesh.tris.size() / 3;
        const bool skinned = mesh.skin() && hier && !hier->pivots.empty();
        if (mesh.skin() && !skinned) warn("mesh '" + mesh.full + "' is skinned but there is no hierarchy; drawn unskinned");
        if (skinned && mesh.bone.size() < nv) throw std::runtime_error(path_ + ": skinned mesh '" + mesh.full + "' has " + std::to_string(mesh.bone.size()) + " vertex influences for " + std::to_string(nv) + " vertices");
        // normals: from the file, else smooth from the triangles
        std::vector<float3> nrm = mesh.nrm;
        if (nrm.size() < nv) {
            nrm.assign(nv, float3{});
            for (size_t t = 0; t < nt; ++t) {
                const uint32_t a = mesh.tris[t * 3], b = mesh.tris[t * 3 + 1], c = mesh.tris[t * 3 + 2];
                const float3 fn = cross(mesh.pos[b] - mesh.pos[a], mesh.pos[c] - mesh.pos[a]);
                nrm[a] += fn; nrm[b] += fn; nrm[c] += fn;
            }
        }
        const std::vector<float2>& uv = mesh.stageUv.size() >= nv ? mesh.stageUv : mesh.uv;
        const int pivots = skinned ? int(hier->pivots.size()) : 0;
        std::vector<RawVertex> raw(nv);
        for (size_t i = 0; i < nv; ++i) {
            RawVertex& rv = raw[i];
            float3 p = conv(mesh.pos[i]), n = normalize(conv(nrm[i]));
            if (skinned) {
                int b0 = mesh.bone[i] < pivots ? mesh.bone[i] : 0, b1 = mesh.bone2[i] < pivots ? mesh.bone2[i] : 0;
                if (mesh.bone[i] >= pivots) warn("mesh '" + mesh.full + "': vertex bound to missing bone " + std::to_string(mesh.bone[i]));
                // vertices are stored in the space of their (first) bone
                p = bindWorld_[size_t(b0)].transformPoint(p);
                n = normalize(bindWorld_[size_t(b0)].transformDir(n));
                rv.j[0] = b0; rv.j[1] = b1;
                rv.w[0] = mesh.w1[i]; rv.w[1] = 1 - mesh.w1[i];
            }
            rv.v.pos = p;
            rv.v.nrm = n;
            if (i < uv.size()) rv.v.uv = uv[i];
            if (i < mesh.dcg.size()) {
                const uint32_t c = mesh.dcg[i];
                auto ch = [&](int s) { return uint32_t(std::lround(srgbToLinear(float((c >> s) & 255u) / 255.0f) * 255.0f)); };
                rv.v.color = ch(0) | ch(8) << 8 | ch(16) << 16 | (c & 0xff000000u);
            }
        }
        // triangles by material
        auto pick = [](const std::vector<uint32_t>& ids, size_t perItem, size_t item) -> uint32_t {
            if (ids.empty()) return 0;
            if (ids.size() == 1) return ids[0];
            return item < perItem && item < ids.size() ? ids[item] : 0;
        };
        std::map<int, std::vector<uint32_t>> byMat;
        for (size_t t = 0; t < nt; ++t) {
            const uint32_t v0 = mesh.tris[t * 3];
            const int mat = material(mesh, pick(mesh.vmIds, nv, v0), pick(mesh.shaderIds, nt, t), mesh.textures.empty() ? 0xffffffffu : pick(mesh.texIds, nt, t));
            // W3D triangles are counter-clockwise in a right-handed frame: unchanged by the axis rotation
            auto& v = byMat[mat];
            v.insert(v.end(), {mesh.tris[t * 3], mesh.tris[t * 3 + 1], mesh.tris[t * 3 + 2]});
        }
        MeshDef md;
        md.name = mesh.name;
        for (auto& [mat, idx] : byMat) {
            if (!skinned) { appendChunk(md, mat, raw, idx, false); continue; }
            std::vector<uint32_t> chunk;
            std::set<int> used;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                std::set<int> merged = used;
                for (int k = 0; k < 3; ++k) {
                    const RawVertex& v = raw[idx[t + size_t(k)]];
                    merged.insert(v.j[0]);
                    if (v.w[1] > 0) merged.insert(v.j[1]);
                }
                if (int(merged.size()) > kMaxPaletteJoints && !chunk.empty()) {
                    appendChunk(md, mat, raw, chunk, true);
                    chunk.clear();
                    merged.clear();
                    for (int k = 0; k < 3; ++k) {
                        const RawVertex& v = raw[idx[t + size_t(k)]];
                        merged.insert(v.j[0]);
                        if (v.w[1] > 0) merged.insert(v.j[1]);
                    }
                }
                used = std::move(merged);
                chunk.insert(chunk.end(), {idx[t], idx[t + 1], idx[t + 2]});
            }
            if (!chunk.empty()) appendChunk(md, mat, raw, chunk, true);
        }
        Node node;
        node.mesh = int(m.meshes.size());
        m.meshes.push_back(std::move(md));
        if (skinned) {
            if (m.skins.empty()) {
                Skin s;
                s.name = hier->name;
                for (int j = 0; j < pivots; ++j) { s.joints.push_back(j); s.inverseBind.push_back(bindWorld_[size_t(j)].inverse()); }
                m.skins.push_back(std::move(s));
            }
            node.name = mesh.name;
            node.skin = 0;
            node.parent = -1;
            m.nodes.push_back(node);
            return;
        }
        // rigid: on the bone node itself when it has no mesh yet, else on a child named after the mesh
        Node& b = m.nodes[size_t(bone)];
        if (b.mesh < 0 && b.skin < 0) { b.mesh = node.mesh; return; }
        node.name = mesh.name;
        node.parent = bone;
        m.nodes.push_back(node);
    }

    void appendChunk(MeshDef& md, int mat, const std::vector<RawVertex>& raw, const std::vector<uint32_t>& idx, bool skinned) {
        Model& m = *model_;
        Primitive prim;
        prim.material = mat;
        std::vector<int32_t> remap(raw.size(), -1);
        std::map<int, int> palette;
        const uint32_t base = uint32_t(m.vertices.size());
        prim.firstIndex = uint32_t(m.indices.size());
        prim.indexCount = uint32_t(idx.size());
        prim.firstVertex = base;
        for (uint32_t i : idx) {
            if (remap[i] < 0) {
                remap[i] = int32_t(m.vertices.size() - base);
                ModelVertex v = raw[i].v;
                if (skinned)
                    for (int e = 0; e < 2; ++e) {
                        if (raw[i].w[e] <= 0) continue;
                        auto it = palette.find(raw[i].j[e]);
                        if (it == palette.end()) it = palette.emplace(raw[i].j[e], int(palette.size())).first;
                        v.joints[e] = uint8_t(it->second);
                        v.weights[e] = uint8_t(std::lround(raw[i].w[e] * 255.0f));
                    }
                m.vertices.push_back(v);
            }
            m.indices.push_back(base + uint32_t(remap[i]));
        }
        prim.vertexCount = uint32_t(m.vertices.size()) - base;
        if (skinned) {
            prim.jointPalette.resize(palette.size());
            for (auto& [j, slot] : palette) prim.jointPalette[size_t(slot)] = uint16_t(j);
            if (prim.jointPalette.empty()) prim.jointPalette.push_back(0);
        }
        if (m.materials[size_t(mat)].normalTex >= 0) generateTangents(m.vertices, m.indices, prim.firstIndex, prim.indexCount);
        md.primitives.push_back(std::move(prim));
    }

    // ---- animation ----

    void loadSidecarAnims(std::vector<Anim>& anims) {
        std::string list;
        if (!read_(stemPath(path_) + ".anims.txt", list)) return;
        std::string line;
        for (size_t i = 0; i <= list.size(); ++i) {
            if (i < list.size() && list[i] != '\n' && list[i] != '\r') { line += list[i]; continue; }
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            const size_t a = line.find_first_not_of(" \t"), b = line.find_last_not_of(" \t");
            const std::string file = a == std::string::npos ? std::string() : line.substr(a, b - a + 1);
            line.clear();
            if (file.empty()) continue;
            std::string bytes;
            if (!read_(dir_ + file, bytes)) { warn("animation file '" + dir_ + file + "' (from " + stemPath(path_) + ".anims.txt) not found"); continue; }
            File f;
            try { parseFile(dir_ + file, bytes, f, false); }
            catch (const std::exception& e) { warn(std::string("animation file skipped: ") + e.what()); continue; }
            if (f.anims.empty()) warn("'" + dir_ + file + "' contains no animation");
            anims.insert(anims.end(), f.anims.begin(), f.anims.end());
        }
    }

    void addClip(const Anim& a, const Hierarchy& h) {
        if (!a.hierarchy.empty() && lowerStr(a.hierarchy) != lowerStr(h.name)) {
            warn("animation '" + a.name + "' is for hierarchy '" + a.hierarchy + "', not '" + h.name + "'; skipped");
            return;
        }
        if (a.frames <= 0 || a.tracks.empty()) return;
        AnimClip clip;
        clip.name = afterDot(a.name);
        if (clip.name.empty()) clip.name = "anim" + std::to_string(model_->clips.size());
        if (model_->findClip(clip.name) >= 0) clip.name += "_" + std::to_string(model_->clips.size());
        clip.duration = a.frames > 1 ? float(a.frames - 1) / a.fps : 0;
        for (const auto& [pivot, t] : a.tracks) {
            if (pivot == 0 || pivot >= h.pivots.size()) continue;   // WW3D never animates the root pivot
            const Pivot& p = h.pivots[pivot];
            AnimChannel ct, cr;
            ct.node = cr.node = int(pivot);
            ct.path = AnimChannel::Translation; cr.path = AnimChannel::Rotation;
            ct.comps = 3; cr.comps = 4;
            quat prev;
            for (int f = 0; f < a.frames; ++f) {
                const float time = float(f) / a.fps;
                if (t.t) {
                    const float3 v = conv(p.t + p.r.rotate(t.dt[size_t(f)]));
                    ct.times.push_back(time);
                    ct.values.insert(ct.values.end(), {v.x, v.y, v.z});
                }
                if (t.r) {
                    quat q = convQ(qnormalize(p.r * t.q[size_t(f)]));
                    if (f > 0 && dot(prev, q) < 0) q = {-q.x, -q.y, -q.z, -q.w};
                    prev = q;
                    cr.times.push_back(time);
                    cr.values.insert(cr.values.end(), {q.x, q.y, q.z, q.w});
                }
            }
            if (t.t) clip.channels.push_back(std::move(ct));
            if (t.r) clip.channels.push_back(std::move(cr));
        }
        if (!clip.channels.empty()) model_->clips.push_back(std::move(clip));
    }

    std::string path_, dir_;
    const FileReader& read_;
    std::vector<std::string>* warnings_;
    std::set<std::string> warned_;
    std::unique_ptr<Model> model_;
    std::vector<mat4> bindWorld_;
    std::map<std::string, int> materials_, images_;
    int extraPasses_ = 0;
};

} // namespace

std::unique_ptr<Model> loadW3d(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings) {
    W3dLoader l(path, read, warnings);
    return l.load(bytes);
}

} // namespace rts::render
