// 3D Studio .3ds loader (3D Studio DOS / 3ds Max r1-r3 era chunk format).
//
// Read: named objects with triangle meshes (vertices, faces, per-vertex UVs, material groups,
// smoothing groups -> split normals, local matrix), materials (diffuse / self-illumination /
// shininess / transparency / two-sided, diffuse texture map with tiling and offset), the keyframer
// (object and dummy nodes, hierarchy, pivots, position / rotation / scale tracks -> one clip
// "Take 001", TCB keys treated as linear). Ignored: cameras, lights, bump / reflection / specular
// maps (warned), morph and hide tracks.
//
// Geometry: 3DS stores mesh vertices in world space plus the object matrix (chunk 0x4160) mapping
// local -> world. With a keyframer the node transforms (frame 0) place the mesh, so vertices are
// brought back to node space: v_local = T(-pivot) * [mirror X when det < 0] * inverse(matrix) * v
// (the lib3ds / assimp convention). The stored world vertices are the pose of the frame the file
// was saved at, which need not be the first frame, so the matrix is trusted over the keyframer.
// Without a keyframer every object is a node with an identity transform and world-space vertices.
//
// Axes: 3DS is Z up; a "root" node rotates the scene to the engine's Y up. Units are kept.
#include "Model.h"
#include "ModelInterchange.h"

#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace rts::render {

using namespace interchange;

namespace {

enum : uint16_t {
    kMain = 0x4D4D, kEditor = 0x3D3D, kObject = 0x4000, kTriMesh = 0x4100, kVertices = 0x4110, kFaces = 0x4120,
    kFaceMat = 0x4130, kSmooth = 0x4150, kUv = 0x4140, kMatrix = 0x4160,
    kMaterial = 0xAFFF, kMatName = 0xA000, kDiffuse = 0xA020, kShininess = 0xA040, kShinStrength = 0xA041,
    kTransparency = 0xA050, kTwoSided = 0xA081, kSelfIllum = 0xA084, kTexMap = 0xA200, kSpecMap = 0xA204,
    kOpacityMap = 0xA210, kReflMap = 0xA220, kBumpMap = 0xA230, kMapFile = 0xA300,
    kMapUScale = 0xA354, kMapVScale = 0xA356, kMapUOffset = 0xA358, kMapVOffset = 0xA35A,
    kColorF = 0x0010, kColor24 = 0x0011, kLinColor24 = 0x0012, kLinColorF = 0x0013, kPctI = 0x0030, kPctF = 0x0031,
    kKeyframer = 0xB000, kObjectNode = 0xB002, kFrames = 0xB008, kNodeHdr = 0xB010, kInstance = 0xB011,
    kPivot = 0xB013, kPosTrack = 0xB020, kRotTrack = 0xB021, kScaleTrack = 0xB022, kNodeId = 0xB030,
};

struct Chunk { uint16_t id; size_t begin, end; };   // [begin, end) = payload

struct Object3ds {
    std::string name;
    std::vector<float3> pos;
    std::vector<float2> uv;
    std::vector<uint16_t> faces;          // 3 per face
    std::vector<uint32_t> smooth;         // per face
    std::vector<std::pair<std::string, std::vector<uint16_t>>> groups;   // material -> faces
    mat4 matrix;                          // local -> world
    bool hasMesh = false;
};

struct Key { float frame; float v[4]; };
struct Node3ds {
    int id = -1, parentId = -1;
    std::string object, instance;
    float3 pivot;
    std::vector<Key> pos, rot, scl;
};

class Reader3ds {
public:
    Reader3ds(const std::string& path, const std::string& b, const FileReader& read, std::vector<std::string>* w)
        : path_(path), b_(b), warnings_(w), model_(std::make_unique<Model>()), tex_{path, dirOf(path), read, w, *model_, {}} {}

    std::unique_ptr<Model> load() {
        if (b_.size() < 6) fail("not a 3DS file (too short)");
        ByteReader r(path_, b_);
        const Chunk main = chunk(r, b_.size());
        if (main.id != kMain) fail("not a 3DS file (main chunk 0x4D4D missing)");
        forEach(main, [&](const Chunk& c) {
            if (c.id == kEditor) editor(c);
            else if (c.id == kKeyframer) keyframer(c);
        });
        build();
        model_->finalize();
        return std::move(model_);
    }

private:
    [[noreturn]] void fail(const std::string& w) const { throw std::runtime_error(path_ + ": " + w); }
    void warn(const std::string& w) {
        if (warnings_ && warned_.insert(w).second) warnings_->push_back(path_ + ": " + w);
    }

    Chunk chunk(ByteReader& r, size_t parentEnd) {
        Chunk c;
        const size_t at = r.pos();
        c.id = r.u16();
        const uint32_t len = r.u32();
        if (len < 6 || at + len > parentEnd) fail("chunk 0x" + hex(c.id) + " at offset " + std::to_string(at) + " runs past its parent");
        c.begin = at + 6;
        c.end = at + len;
        return c;
    }
    template <class F> void forEach(const Chunk& parent, F&& f, size_t from = 0) {
        ByteReader r(path_, b_, from ? from : parent.begin, parent.end);
        if (++depth_ > 32) fail("chunks nested too deep");
        while (r.left() >= 6) {
            const Chunk c = chunk(r, parent.end);
            f(c);
            r.seek(c.end);
        }
        --depth_;
    }
    static std::string hex(uint16_t v) {
        static const char* d = "0123456789ABCDEF";
        return {d[v >> 12], d[(v >> 8) & 15], d[(v >> 4) & 15], d[v & 15]};
    }

    // ---- editor: materials and objects

    void editor(const Chunk& ed) {
        forEach(ed, [&](const Chunk& c) {
            if (c.id == kMaterial) material(c);
            else if (c.id == kObject) object(c);
        });
    }

    bool color(const Chunk& parent, float3& out) {
        bool got = false, lin = false;
        forEach(parent, [&](const Chunk& c) {
            ByteReader r(path_, b_, c.begin, c.end);
            if ((c.id == kColorF || c.id == kLinColorF) && !(lin && c.id == kColorF)) {
                out = {r.f32(), r.f32(), r.f32()};
                got = true;
                lin = lin || c.id == kLinColorF;
            } else if ((c.id == kColor24 || c.id == kLinColor24) && !lin) {
                const float x = r.u8() / 255.0f, y = r.u8() / 255.0f, z = r.u8() / 255.0f;
                // the gamma-corrected 24-bit colour is sRGB-like; the "linear" variants are exact
                if (c.id == kLinColor24) { out = {x, y, z}; lin = true; }
                else out = {srgb(x), srgb(y), srgb(z)};
                got = true;
            }
        });
        return got;
    }
    static float srgb(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
    float percent(const Chunk& parent, float def) {
        float v = def;
        forEach(parent, [&](const Chunk& c) {
            ByteReader r(path_, b_, c.begin, c.end);
            if (c.id == kPctI) v = float(r.le<int16_t>()) / 100.0f;
            else if (c.id == kPctF) v = r.f32() / 100.0f;
        });
        return v;
    }

    void material(const Chunk& mc) {
        Material m;
        float3 diffuse{0.8f, 0.8f, 0.8f};
        float selfIllum = 0, shininess = 0, strength = 1, transparency = 0;
        std::string texFile, opacityFile;
        float2 uvScale{1, 1}, uvOffset{0, 0};
        forEach(mc, [&](const Chunk& c) {
            ByteReader r(path_, b_, c.begin, c.end);
            switch (c.id) {
            case kMatName: m.name = r.cstr(); break;
            case kDiffuse: color(c, diffuse); break;
            case kShininess: shininess = percent(c, 0); break;
            case kShinStrength: strength = percent(c, 1); break;
            case kTransparency: transparency = percent(c, 0); break;
            case kTwoSided: m.doubleSided = true; break;
            case kSelfIllum: selfIllum = percent(c, 0); break;
            case kTexMap:
            case kOpacityMap:
                forEach(c, [&](const Chunk& s) {
                    ByteReader q(path_, b_, s.begin, s.end);
                    if (s.id == kMapFile) (c.id == kTexMap ? texFile : opacityFile) = q.cstr();
                    if (c.id != kTexMap) return;
                    if (s.id == kMapUScale) uvScale.x = q.f32();
                    else if (s.id == kMapVScale) uvScale.y = q.f32();
                    else if (s.id == kMapUOffset) uvOffset.x = q.f32();
                    else if (s.id == kMapVOffset) uvOffset.y = q.f32();
                });
                break;
            case kSpecMap: case kReflMap: case kBumpMap:
                warn("material '" + m.name + "': specular / reflection / bump maps are ignored");
                break;
            default: break;
            }
        });
        if (m.name.empty()) m.name = "material" + std::to_string(model_->materials.size());
        m.baseColor = {diffuse.x, diffuse.y, diffuse.z, std::clamp(1.0f - transparency, 0.0f, 1.0f)};
        // shininess % is the glossiness of the Phong highlight (100% ~ exponent 128); no strength = matte
        m.roughness = strength <= 0.001f ? 0.9f : shininessToRoughness(std::clamp(shininess, 0.0f, 1.0f) * 128.0f);
        m.emissive = diffuse * std::clamp(selfIllum, 0.0f, 1.0f);
        if (!texFile.empty()) {
            m.baseTex = tex_.load(texFile);
            if (m.baseTex >= 0) m.baseColor = {1, 1, 1, m.baseColor.w};
            if (std::isfinite(uvScale.x) && std::isfinite(uvScale.y) && uvScale.x != 0 && uvScale.y != 0) m.uvScale = uvScale;
            if (std::isfinite(uvOffset.x) && std::isfinite(uvOffset.y)) m.uvOffset = uvOffset;
        }
        if (!opacityFile.empty()) {
            if (lowerStr(opacityFile) == lowerStr(texFile)) m.alpha = AlphaMode::Blend;
            else warn("material '" + m.name + "': separate opacity map ignored (put alpha into the diffuse texture)");
        }
        if (m.baseColor.w < 0.999f) m.alpha = AlphaMode::Blend;
        matIds_[m.name] = int(model_->materials.size());
        model_->materials.push_back(m);
    }

    void object(const Chunk& oc) {
        Object3ds o;
        ByteReader r(path_, b_, oc.begin, oc.end);
        o.name = r.cstr();
        forEach(oc, [&](const Chunk& c) {
            if (c.id != kTriMesh) return;
            o.hasMesh = true;
            forEach(c, [&](const Chunk& s) { trimesh(o, s); });
        }, r.pos());
        if (!o.hasMesh) return;   // camera / light
        if (!o.uv.empty() && o.uv.size() != o.pos.size()) { warn("object '" + o.name + "': UV count differs from the vertex count; UVs dropped"); o.uv.clear(); }
        for (uint16_t i : o.faces)
            if (i >= o.pos.size()) fail("object '" + o.name + "': face index " + std::to_string(i) + " out of range");
        if (o.smooth.size() != o.faces.size() / 3) o.smooth.assign(o.faces.size() / 3, 0);
        objIds_[o.name] = int(objects_.size());
        objects_.push_back(std::move(o));
    }

    void trimesh(Object3ds& o, const Chunk& c) {
        ByteReader r(path_, b_, c.begin, c.end);
        switch (c.id) {
        case kVertices: {
            const uint16_t n = r.u16();
            r.need(size_t(n) * 12);
            o.pos.resize(n);
            for (auto& p : o.pos) p = {r.f32(), r.f32(), r.f32()};
            break;
        }
        case kUv: {
            const uint16_t n = r.u16();
            r.need(size_t(n) * 8);
            o.uv.resize(n);
            for (auto& t : o.uv) { t.x = r.f32(); t.y = 1.0f - r.f32(); }
            break;
        }
        case kMatrix: {
            float f[12];
            for (float& x : f) x = r.f32();
            mat4 m;
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 3; ++row) m.at(row, col) = f[col * 3 + row];
            o.matrix = m;
            break;
        }
        case kFaces: {
            const uint16_t n = r.u16();
            r.need(size_t(n) * 8);
            o.faces.resize(size_t(n) * 3);
            for (uint16_t f = 0; f < n; ++f) {
                for (int k = 0; k < 3; ++k) o.faces[size_t(f) * 3 + size_t(k)] = r.u16();
                r.u16();   // edge visibility flags
            }
            forEach(c, [&](const Chunk& s) {
                ByteReader q(path_, b_, s.begin, s.end);
                if (s.id == kFaceMat) {
                    std::string mat = q.cstr();
                    const uint16_t k = q.u16();
                    q.need(size_t(k) * 2);
                    std::vector<uint16_t> list(k);
                    for (auto& x : list) {
                        x = q.u16();
                        if (x >= n) fail("object '" + o.name + "': material group face " + std::to_string(x) + " out of range");
                    }
                    o.groups.push_back({std::move(mat), std::move(list)});
                } else if (s.id == kSmooth) {
                    if (q.left() < size_t(n) * 4) { warn("object '" + o.name + "': truncated smoothing groups"); return; }
                    o.smooth.resize(n);
                    for (auto& x : o.smooth) x = q.u32();
                }
            }, r.pos());
            break;
        }
        default: break;
        }
    }

    // ---- keyframer

    void keyframer(const Chunk& kf) {
        forEach(kf, [&](const Chunk& c) {
            ByteReader r(path_, b_, c.begin, c.end);
            if (c.id == kFrames) { startFrame_ = float(r.u32()); endFrame_ = float(r.u32()); hasFrames_ = true; return; }
            if (c.id < 0xB001 || c.id > 0xB007) return;
            const int ordinal = nodeOrdinal_++;
            if (c.id != kObjectNode) {   // camera / light / target / ambient node: only its id matters
                int id = ordinal;
                forEach(c, [&](const Chunk& s) { if (s.id == kNodeId) id = ByteReader(path_, b_, s.begin, s.end).u16(); });
                otherNodeIds_.insert(id);
                return;
            }
            Node3ds n;
            n.id = ordinal;
            forEach(c, [&](const Chunk& s) {
                ByteReader q(path_, b_, s.begin, s.end);
                switch (s.id) {
                case kNodeId: n.id = q.u16(); break;
                case kNodeHdr: {
                    n.object = q.cstr();
                    q.u16(); q.u16();
                    const uint16_t p = q.u16();
                    n.parentId = p == 0xFFFF ? -1 : int(p);
                    break;
                }
                case kInstance: n.instance = q.cstr(); break;
                case kPivot: n.pivot = {q.f32(), q.f32(), q.f32()}; break;
                case kPosTrack: track(q, n.pos, 3); break;
                case kRotTrack: track(q, n.rot, 4); break;
                case kScaleTrack: track(q, n.scl, 3); break;
                default: break;
                }
            });
            nodes_.push_back(std::move(n));
        });
    }

    void track(ByteReader& q, std::vector<Key>& keys, int comps) {
        q.u16();          // flags (loop / repeat)
        q.skip(8);
        const uint32_t n = q.u32();
        if (size_t(n) > q.left() / 6) fail("keyframer track with " + std::to_string(n) + " keys runs past its chunk");
        for (uint32_t i = 0; i < n; ++i) {
            Key k{};
            k.frame = float(q.u32());
            const uint16_t spline = q.u16();
            for (int b = 0; b < 5; ++b) if (spline & (1u << b)) q.f32();   // tension, continuity, bias, ease to / from
            for (int c = 0; c < comps; ++c) k.v[c] = q.f32();
            keys.push_back(k);
        }
    }

    // ---- model

    // Rotation keys hold angle + axis relative to the previous key; 3DS angles turn clockwise.
    static std::vector<quat> absoluteRotations(const std::vector<Key>& keys) {
        std::vector<quat> out;
        quat prev;
        for (size_t i = 0; i < keys.size(); ++i) {
            float3 axis{keys[i].v[1], keys[i].v[2], keys[i].v[3]};
            quat d = dot(axis, axis) > 1e-12f && std::isfinite(keys[i].v[0]) ? quat::axisAngle(axis, -keys[i].v[0]) : quat{};
            prev = i == 0 ? d : qnormalize(d * prev);
            out.push_back(prev);
        }
        return out;
    }
    static float3 vecAt(const std::vector<Key>& keys, float frame, float3 def) {
        if (keys.empty()) return def;
        if (frame <= keys.front().frame) return {keys[0].v[0], keys[0].v[1], keys[0].v[2]};
        for (size_t i = 1; i < keys.size(); ++i)
            if (frame <= keys[i].frame) {
                const Key &a = keys[i - 1], &b = keys[i];
                const float t = b.frame > a.frame ? (frame - a.frame) / (b.frame - a.frame) : 0;
                return lerp(float3{a.v[0], a.v[1], a.v[2]}, float3{b.v[0], b.v[1], b.v[2]}, t);
            }
        const Key& l = keys.back();
        return {l.v[0], l.v[1], l.v[2]};
    }
    static quat rotAt(const std::vector<Key>& keys, const std::vector<quat>& q, float frame) {
        if (q.empty()) return {};
        if (frame <= keys.front().frame) return q[0];
        for (size_t i = 1; i < keys.size(); ++i)
            if (frame <= keys[i].frame) {
                const float t = keys[i].frame > keys[i - 1].frame ? (frame - keys[i - 1].frame) / (keys[i].frame - keys[i - 1].frame) : 0;
                return slerp(q[i - 1], q[i], t);
            }
        return q.back();
    }

    int defaultMaterial() {
        if (defaultMat_ < 0) {
            Material m; m.name = "default";
            defaultMat_ = int(model_->materials.size());
            model_->materials.push_back(m);
        }
        return defaultMat_;
    }

    // Mesh of one object with vertices transformed by `xf` (world -> node space).
    int buildMesh(const Object3ds& o, const mat4& xf) {
        Model& m = *model_;
        const mat4 nx = xf.normalMatrix();
        const bool flip = det3(xf) < 0;
        const size_t nf = o.faces.size() / 3;
        // face normals (world space, area weighted) and vertex -> faces adjacency
        std::vector<float3> fn(nf);
        std::vector<std::vector<uint32_t>> adj(o.pos.size());
        for (size_t f = 0; f < nf; ++f) {
            const float3 a = o.pos[o.faces[f * 3]], b = o.pos[o.faces[f * 3 + 1]], c = o.pos[o.faces[f * 3 + 2]];
            fn[f] = cross(b - a, c - a);
            for (int k = 0; k < 3; ++k) {
                auto& l = adj[o.faces[f * 3 + size_t(k)]];
                if (l.empty() || l.back() != f) l.push_back(uint32_t(f));
            }
        }
        // faces per material (faces not in any group use the default material)
        std::vector<int> faceMat(nf, -1);
        for (const auto& [name, list] : o.groups) {
            auto it = matIds_.find(name);
            int mi = it == matIds_.end() ? -1 : it->second;
            if (it == matIds_.end() && !name.empty()) warn("object '" + o.name + "': unknown material '" + name + "'");
            for (uint16_t f : list) faceMat[f] = mi < 0 ? defaultMaterial() : mi;
        }
        std::map<int, std::vector<uint32_t>> byMat;
        for (size_t f = 0; f < nf; ++f) byMat[faceMat[f] < 0 ? defaultMaterial() : faceMat[f]].push_back(uint32_t(f));
        MeshDef md;
        md.name = o.name;
        for (auto& [mat, faces] : byMat) {
            Primitive p;
            p.material = mat;
            p.firstIndex = uint32_t(m.indices.size());
            p.firstVertex = uint32_t(m.vertices.size());
            // corner = (vertex, smoothing mask); mask 0 = flat (own corner per face)
            std::map<std::pair<uint32_t, uint64_t>, uint32_t> corners;
            for (uint32_t f : faces) {
                uint32_t tri[3];
                for (int k = 0; k < 3; ++k) {
                    const uint32_t vi = o.faces[size_t(f) * 3 + size_t(k)];
                    const uint32_t sg = o.smooth[f];
                    const uint64_t key = sg ? uint64_t(sg) : (uint64_t(1) << 32) + f;
                    auto [it, inserted] = corners.emplace(std::make_pair(vi, key), 0u);
                    if (inserted) {
                        float3 n{0, 0, 0};
                        if (sg) { for (uint32_t g : adj[vi]) if (o.smooth[g] & sg) n += fn[g]; }
                        else n = fn[f];
                        ModelVertex v;
                        v.pos = xf.transformPoint(o.pos[vi]);
                        v.nrm = normalize(nx.transformDir(n));
                        if (!o.uv.empty()) v.uv = o.uv[vi];
                        it->second = uint32_t(m.vertices.size());
                        m.vertices.push_back(v);
                    }
                    tri[k] = it->second;
                }
                if (flip) std::swap(tri[1], tri[2]);
                m.indices.insert(m.indices.end(), {tri[0], tri[1], tri[2]});
            }
            p.indexCount = uint32_t(m.indices.size()) - p.firstIndex;
            p.vertexCount = uint32_t(m.vertices.size()) - p.firstVertex;
            if (p.indexCount) md.primitives.push_back(p);
        }
        m.meshes.push_back(std::move(md));
        return int(m.meshes.size()) - 1;
    }
    static float det3(const mat4& a) {
        return a.at(0, 0) * (a.at(1, 1) * a.at(2, 2) - a.at(1, 2) * a.at(2, 1)) - a.at(0, 1) * (a.at(1, 0) * a.at(2, 2) - a.at(1, 2) * a.at(2, 0)) +
               a.at(0, 2) * (a.at(1, 0) * a.at(2, 1) - a.at(1, 1) * a.at(2, 0));
    }
    void build() {
        Model& m = *model_;
        Node root;
        root.name = "root";
        root.r = quat::axisAngle({1, 0, 0}, -1.5707963f);   // Z up -> Y up
        m.nodes.push_back(root);
        if (nodes_.empty()) {
            for (const auto& o : objects_) {
                Node n;
                n.name = o.name;
                n.parent = 0;
                n.mesh = buildMesh(o, mat4{});
                m.nodes.push_back(n);
            }
            if (m.meshes.empty()) fail("no meshes");
            return;
        }
        // keyframer nodes: hierarchy by node id, frame-0 transforms
        const float f0 = hasFrames_ ? startFrame_ : 0;
        std::map<int, int> byId;
        std::vector<std::vector<quat>> rots(nodes_.size());
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const Node3ds& k = nodes_[i];
            byId[k.id] = int(m.nodes.size());
            rots[i] = absoluteRotations(k.rot);
            Node n;
            n.name = k.instance.empty() ? k.object : k.object == "$$$DUMMY" ? k.instance : k.object + "." + k.instance;
            n.t = vecAt(k.pos, f0, {0, 0, 0});
            n.r = rotAt(k.rot, rots[i], f0);
            n.s = vecAt(k.scl, f0, {1, 1, 1});
            m.nodes.push_back(n);
        }
        for (size_t i = 0; i < nodes_.size(); ++i) {
            auto it = byId.find(nodes_[i].parentId);
            m.nodes[i + 1].parent = it != byId.end() && it->second != int(i + 1) ? it->second : 0;
        }
        // objects linked to cameras / lights (whose orientation comes from targets and roll, not
        // read here) and their children stay where the mesh data puts them, without animation
        std::vector<int8_t> detached(m.nodes.size(), -1);
        std::function<bool(int, int)> isDetached = [&](int i, int depth) -> bool {
            if (i <= 0 || depth > 256) return false;
            if (detached[size_t(i)] < 0)
                detached[size_t(i)] = otherNodeIds_.count(nodes_[size_t(i - 1)].parentId) || isDetached(m.nodes[size_t(i)].parent, depth + 1);
            return detached[size_t(i)] != 0;
        };
        int linked = 0;
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (isDetached(int(i + 1), 0)) {
                Node& n = m.nodes[i + 1];
                n.parent = 0; n.t = {}; n.r = {}; n.s = {1, 1, 1};
                nodes_[i].pos.clear(); nodes_[i].rot.clear(); nodes_[i].scl.clear();
                ++linked;
            }
        if (linked) warn(std::to_string(linked) + " object(s) linked to a camera or light: placed without their animation");
        std::set<int> used;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const Node3ds& k = nodes_[i];
            auto it = objIds_.find(k.object);
            if (it == objIds_.end()) continue;
            used.insert(it->second);
            const Object3ds& o = objects_[size_t(it->second)];
            mat4 inv = o.matrix.inverse();
            if (det3(o.matrix) < 0) inv = mat4::scale({-1, 1, 1}) * inv;
            const mat4 xf = detached[i + 1] == 1 ? mat4{} : mat4::translate(-k.pivot) * inv;
            m.nodes[i + 1].mesh = buildMesh(o, xf);
        }
        for (size_t oi = 0; oi < objects_.size(); ++oi) {   // objects the keyframer does not mention
            if (used.count(int(oi))) continue;
            Node n;
            n.name = objects_[oi].name;
            n.parent = 0;
            n.mesh = buildMesh(objects_[oi], mat4{});
            m.nodes.push_back(n);
        }
        if (m.meshes.empty()) fail("no meshes");
        animation(rots);
    }

    void animation(const std::vector<std::vector<quat>>& rots) {
        AnimClip clip;
        clip.name = "Take 001";
        const float f0 = hasFrames_ ? startFrame_ : 0, fps = 30;
        auto times = [&](const std::vector<Key>& keys, AnimChannel& c) {
            for (const Key& k : keys) c.times.push_back(std::max(0.0f, (k.frame - f0) / fps));
            for (size_t i = 1; i < c.times.size(); ++i) c.times[i] = std::max(c.times[i], c.times[i - 1]);
        };
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const Node3ds& k = nodes_[i];
            const int node = int(i + 1);
            for (int path = 0; path < 3; ++path) {
                const std::vector<Key>& keys = path == 0 ? k.pos : path == 1 ? k.rot : k.scl;
                if (keys.size() < 2) continue;
                AnimChannel c;
                c.node = node;
                c.path = path == 0 ? AnimChannel::Translation : path == 1 ? AnimChannel::Rotation : AnimChannel::Scale;
                c.comps = path == 1 ? 4 : 3;
                times(keys, c);
                for (size_t j = 0; j < keys.size(); ++j) {
                    if (path == 1) { const quat& q = rots[i][j]; c.values.insert(c.values.end(), {q.x, q.y, q.z, q.w}); }
                    else c.values.insert(c.values.end(), {keys[j].v[0], keys[j].v[1], keys[j].v[2]});
                }
                if (path == 1)   // shortest arcs between keys
                    for (size_t j = 1; j < keys.size(); ++j) {
                        float* a = &c.values[(j - 1) * 4];
                        float* b = &c.values[j * 4];
                        if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) for (int e = 0; e < 4; ++e) b[e] = -b[e];
                    }
                clip.duration = std::max(clip.duration, c.times.back());
                clip.channels.push_back(std::move(c));
            }
        }
        if (hasFrames_ && endFrame_ > startFrame_) clip.duration = std::max(clip.duration, (endFrame_ - startFrame_) / fps);
        if (!clip.channels.empty()) model_->clips.push_back(std::move(clip));
    }

    std::string path_;
    const std::string& b_;
    std::vector<std::string>* warnings_;
    std::set<std::string> warned_;
    std::unique_ptr<Model> model_;
    TextureLoader tex_;
    std::map<std::string, int> matIds_, objIds_;
    std::vector<Object3ds> objects_;
    std::vector<Node3ds> nodes_;
    float startFrame_ = 0, endFrame_ = 0;
    bool hasFrames_ = false;
    int defaultMat_ = -1, depth_ = 0, nodeOrdinal_ = 0;
    std::set<int> otherNodeIds_;
};

} // namespace

std::unique_ptr<Model> load3ds(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings) {
    Reader3ds r(path, bytes, read, warnings);
    return r.load();
}

} // namespace rts::render
