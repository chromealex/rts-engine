// Warcraft III models: binary .mdx and text .mdl (format versions 800 classic, 900-1100 Reforged),
// read from the user's own game copy or community models. Both are parsed into one intermediate
// structure (Mdlx, ModelMdxData.h; the text reader is ModelMdl.cpp) and converted to a Model by
// the same code (here).
//
// Axes and units: Warcraft III is Z up with units facing +X, 128 units = one map cell. Positions
// map (x, y, z) -> (y, z, x) / 128: a proper rotation (Y up, facing +Z, right-handed), one cell = 1.
//
// Skeleton: every object (bone, helper, attachment, emitter, light, event, collision shape)
// becomes a node under a "root" node, so attachment names ("Hand Right Ref", "Overhead Ref")
// work as sockets. Warcraft III transforms a node around its pivot (model-space point):
//   W = Wparent * T(pivot) * T(t) * R * S * T(-pivot).
// With E = W * T(pivot) this is E = Eparent * T(pivot - parentPivot + t) * R * S, i.e. a plain
// TRS node whose bind translation is pivot - parentPivot; translation keys are added to it and
// the inverse bind matrix is T(-pivot).
//
// Skinning: classic vertex groups (matrix groups averaged equally, at most 4 bones) or Reforged
// SKIN (4 bones + weights). Primitives are split so every palette has <= kMaxPaletteJoints joints.
//
// Animation: each sequence becomes a clip (times from 0, seconds) with keys of every track in
// [start, end] plus boundary keys (looping sequences: the value interpolated across the loop seam,
// non-looping: held). Linear / DontInterp -> Linear / Step; Hermite and Bezier vector tracks ->
// CubicSpline (exact); Hermite / Bezier rotations (squad) -> CubicSpline with the cubic that
// squad becomes when its slerps are replaced by lerps (close for the small angles between keys).
// Global-sequence tracks are tiled over each clip by their own duration (restarting with the clip).
//
// Materials: the first textured layer is the base colour; a team-colour layer (replaceable id 1)
// under it -> TeamMode::BaseAlpha (texture alpha 0 = team colour); only a team-colour texture ->
// TeamMode::Tint on white; team glow (id 2) -> tinted procedural glow sprite. Filter modes:
// None -> opaque, Transparent -> alpha test 0.75, Blend / Additive / AddAlpha / Modulate -> alpha
// blend (additive and modulate are approximated). Reforged HD materials: diffuse, normal, ORM
// (+ team mask from ORM alpha), emissive.
//
// Not supported (skipped quietly, or one warning): particle emitters, ribbons, lights, cameras,
// event objects, collision shapes (kept as nodes only), texture animations, layer alpha / texture
// animation, geoset alpha / colour animation (geosets hidden in "Stand" are dropped), billboards,
// "don't inherit" flags, LODs other than 0.
#include "Model.h"
#include "ModelMdxData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace rts::render {

namespace {

using namespace mdx;

constexpr float kUnit = 1.0f / 128.0f;

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
std::string baseName(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// ---------------------------------------------------------------- binary .mdx

class MdxReader {
public:
    MdxReader(const std::string& path, const std::string& bytes)
        : path_(path), d_(reinterpret_cast<const uint8_t*>(bytes.data())), end_(bytes.size()) {}

    Mdlx parse() {
        if (end_ < 4 || std::memcmp(d_, "MDLX", 4) != 0) fail("not an MDX file (no MDLX header)");
        p_ = 4;
        while (p_ < end_) {
            need(8, "chunk header");
            const std::string tag = tag4();
            const uint32_t size = u32();
            need(size, "chunk " + tag);
            const size_t chunkEnd = p_ + size;
            const size_t outer = end_;
            end_ = chunkEnd;
            chunk(tag);
            end_ = outer;
            p_ = chunkEnd;
        }
        return std::move(m_);
    }

private:
    [[noreturn]] void fail(const std::string& what) const { throw std::runtime_error(path_ + ": " + what); }
    void need(size_t k, const std::string& what) const {
        if (k > end_ - p_) fail(what + ": truncated or broken (needs " + std::to_string(k) + " bytes at offset " + std::to_string(p_) + ")");
    }
    // count elements of `each` bytes must fit in what is left
    void needN(uint64_t count, size_t each, const std::string& what) const {
        if (count > (end_ - p_) / std::max<size_t>(each, 1)) fail(what + ": count " + std::to_string(count) + " exceeds the data");
    }
    uint32_t u32() { need(4, "value"); uint32_t v; std::memcpy(&v, d_ + p_, 4); p_ += 4; return v; }
    int32_t i32() { return int32_t(u32()); }
    float f32() { const uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return std::isfinite(f) ? f : 0.0f; }
    float3 vec3() { float3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
    std::string tag4() { need(4, "tag"); std::string t(reinterpret_cast<const char*>(d_ + p_), 4); p_ += 4; return t; }
    bool peek(const char* t) const { return end_ - p_ >= 4 && std::memcmp(d_ + p_, t, 4) == 0; }
    void expect(const char* t, const std::string& where) { if (!peek(t)) fail(where + ": expected " + t); p_ += 4; }
    std::string str(size_t len) {
        need(len, "string");
        const char* s = reinterpret_cast<const char*>(d_ + p_);
        size_t n = 0;
        while (n < len && s[n]) ++n;
        p_ += len;
        return std::string(s, n);
    }
    void skipTo(size_t pos) { p_ = std::min(std::max(pos, p_), end_); }
    size_t sized(const std::string& what) {   // inclusive size at the current position -> end offset
        const size_t start = p_;
        const uint32_t size = u32();
        if (size < 4 || size > end_ - start) fail(what + ": bad size " + std::to_string(size));
        return start + size;
    }

    static int trackComps(const std::string& tag) {
        if (tag == "KGRT") return 4;
        if (tag == "KGTR" || tag == "KGSC" || tag == "KGAC" || tag == "KFC3" || tag == "KTAT" || tag == "KTAS" || tag == "KLAC" || tag == "KLBC" || tag == "KRCO") return 3;
        if (tag == "KTAR") return 4;
        return 1;
    }
    static bool intTrack(const std::string& tag) { return tag == "KMTF" || tag == "KRTX" || tag == "KCRL"; }

    Track track(const std::string& tag) {
        Track t;
        t.comps = trackComps(tag);
        const uint32_t count = u32();
        t.interp = int(u32());
        t.globalSeq = i32();
        if (t.interp > 3) fail("track " + tag + ": unknown interpolation " + std::to_string(t.interp));
        const size_t each = 4 + size_t(t.comps) * 4 * (t.interp > 1 ? 3 : 1);
        needN(count, each, "track " + tag);
        const bool asInt = intTrack(tag);
        auto vals = [&](std::vector<float>& out) {
            for (int c = 0; c < t.comps; ++c) out.push_back(asInt ? float(u32()) : f32());
        };
        for (uint32_t i = 0; i < count; ++i) {
            t.frames.push_back(i32());
            vals(t.values);
            if (t.interp > 1) { vals(t.inTans); vals(t.outTans); }
        }
        return t;
    }

    // Generic object header with its own inclusive size and its KGTR / KGRT / KGSC tracks.
    Object object(Object::Kind kind) {
        Object o;
        o.kind = kind;
        const size_t objEnd = sized("object");
        const size_t outer = end_;
        end_ = objEnd;
        o.name = str(80);
        o.id = i32();
        o.parent = i32();
        o.flags = u32();
        while (end_ - p_ >= 4) {
            const std::string tag = tag4();
            if (tag == "KGTR") o.t = track(tag);
            else if (tag == "KGRT") o.r = track(tag);
            else if (tag == "KGSC") o.s = track(tag);
            else break;
        }
        end_ = outer;
        p_ = objEnd;
        return o;
    }

    void chunk(const std::string& tag) {
        if (tag == "VERS") m_.version = u32();
        else if (tag == "SEQS") {
            const size_t n = (end_ - p_) / 132;
            for (size_t i = 0; i < n; ++i) {
                Sequence s;
                s.name = str(80);
                s.start = i32(); s.end = i32();
                f32();                        // move speed
                s.nonLooping = (u32() & 1) != 0;
                p_ += 4 + 4 + 28;             // rarity, sync point, extent
                m_.seqs.push_back(s);
            }
        } else if (tag == "GLBS") {
            while (end_ - p_ >= 4) m_.globals.push_back(i32());
        } else if (tag == "TEXS") {
            const size_t n = (end_ - p_) / 268;
            for (size_t i = 0; i < n; ++i) {
                Texture t;
                t.replaceable = u32();
                t.file = str(260);
                t.flags = u32();
                m_.textures.push_back(t);
            }
        } else if (tag == "MTLS") materials();
        else if (tag == "GEOS") { while (p_ < end_) geoset(); }
        else if (tag == "GEOA") {
            while (p_ < end_) {
                const size_t ge = sized("geoset animation");
                const size_t outer = end_;
                end_ = ge;
                GeosetAnim a;
                a.alpha = f32();
                u32(); vec3();                // flags, colour
                a.geoset = i32();
                while (end_ - p_ >= 4) {
                    const std::string t = tag4();
                    if (t == "KGAO") a.alphaTrack = track(t);
                    else if (t == "KGAC") track(t);
                    else break;
                }
                end_ = outer;
                p_ = ge;
                m_.geoAnims.push_back(std::move(a));
            }
        } else if (tag == "BONE") {
            while (p_ < end_) { m_.objects.push_back(object(Object::Bone)); i32(); i32(); }
        } else if (tag == "HELP") {
            while (p_ < end_) m_.objects.push_back(object(Object::Helper));
        } else if (tag == "ATCH" || tag == "LITE" || tag == "PREM" || tag == "PRE2" || tag == "RIBB" || tag == "CORN") {
            while (p_ < end_) {
                const size_t oe = sized(tag);
                const size_t outer = end_;
                end_ = oe;
                m_.objects.push_back(object(tag == "ATCH" ? Object::Attachment : Object::Other));
                end_ = outer;
                p_ = oe;
            }
            if (tag != "ATCH") m_.emitters = true;
        } else if (tag == "EVTS") {
            while (p_ < end_) {
                m_.objects.push_back(object(Object::Other));
                if (peek("KEVT")) { p_ += 4; const uint32_t n = u32(); i32(); needN(n, 4, "event track"); p_ += size_t(n) * 4; }
            }
        } else if (tag == "CLID") {
            while (p_ < end_) {
                m_.objects.push_back(object(Object::Other));
                const uint32_t type = u32();
                need(type == 2 ? 12 : 24, "collision shape");
                p_ += type == 2 ? 12 : 24;
                if (type == 2 || type == 3) f32();
            }
        } else if (tag == "PIVT") {
            while (end_ - p_ >= 12) m_.pivots.push_back(vec3());
        }
        // MODL, CAMS, TXAN, BPOS, FAFX, SNDS, unknown: nothing the model needs
    }

    void materials() {
        while (p_ < end_) {
            const size_t me = sized("material");
            const size_t outer = end_;
            end_ = me;
            Mat mt;
            i32(); u32();                       // priority plane, flags
            if (!peek("LAYS")) mt.shader = str(80);   // 900 / 1000
            expect("LAYS", "material");
            const uint32_t nl = u32();
            needN(nl, 28, "material layers");
            for (uint32_t i = 0; i < nl; ++i) {
                const size_t le = sized("layer");
                const size_t outer2 = end_;
                end_ = le;
                Layer l;
                l.filter = u32(); l.shading = u32(); l.texture = i32();
                i32(); u32();                   // texture animation, coord id
                l.alpha = f32();
                auto trackTag = [&] { return peek("KMTA") || peek("KMTF") || peek("KMTE") || peek("KFC3") || peek("KFCA") || peek("KFTC"); };
                if (m_.version > 800 && end_ - p_ >= 4 && !trackTag()) p_ += 4;               // emissive gain
                if (m_.version > 800 && end_ - p_ >= 20 && !trackTag()) p_ += 20;              // fresnel colour, opacity, team colour
                if (m_.version >= 1100 && end_ - p_ >= 8 && !trackTag()) {
                    u32();                      // shader type
                    const uint32_t nt = u32();
                    needN(nt, 8, "layer textures");
                    for (uint32_t k = 0; k < nt; ++k) {
                        l.textures.push_back(i32());
                        u32();                  // texture slot
                        if (peek("KMTF")) { p_ += 4; track("KMTF"); }
                    }
                    if (!l.textures.empty()) l.texture = l.textures[0];
                }
                while (end_ - p_ >= 4 && trackTag()) {
                    const std::string t = tag4();
                    const Track tr = track(t);
                    if (t == "KMTF" && l.texture < 0 && !tr.values.empty()) l.texture = int32_t(tr.values[0]);
                }
                end_ = outer2;
                p_ = le;
                mt.layers.push_back(std::move(l));
            }
            end_ = outer;
            p_ = me;
            m_.materials.push_back(std::move(mt));
        }
    }

    void geoset() {
        const size_t ge = sized("geoset");
        const size_t outer = end_;
        end_ = ge;
        Geoset g;
        const std::string where = "geoset " + std::to_string(m_.geosets.size());
        auto count = [&](const char* t, size_t each) { expect(t, where); const uint32_t n = u32(); needN(n, each, where + " " + t); return n; };
        uint32_t n = count("VRTX", 12);
        for (uint32_t i = 0; i < n; ++i) g.pos.push_back(vec3());
        n = count("NRMS", 12);
        for (uint32_t i = 0; i < n; ++i) g.nrm.push_back(vec3());
        n = count("PTYP", 4);
        std::vector<uint32_t> types(n);
        for (auto& t : types) t = u32();
        n = count("PCNT", 4);
        std::vector<uint32_t> counts(n);
        for (auto& c : counts) c = u32();
        n = count("PVTX", 2);
        std::vector<uint16_t> idx(n);
        for (auto& v : idx) { need(2, where); v = uint16_t(d_[p_] | d_[p_ + 1] << 8); p_ += 2; }
        size_t at = 0;
        for (size_t i = 0; i < counts.size() && i < types.size(); ++i) {
            const size_t c = std::min<size_t>(counts[i], idx.size() - at);
            appendFaces(g, types[i], idx.data() + at, c, m_.faceTypesSkipped);
            at += c;
        }
        n = count("GNDX", 1);
        for (uint32_t i = 0; i < n; ++i) g.vgroup.push_back(d_[p_++]);
        n = count("MTGC", 4);
        for (uint32_t i = 0; i < n; ++i) g.groupSizes.push_back(u32());
        n = count("MATS", 4);
        for (uint32_t i = 0; i < n; ++i) g.groupIndices.push_back(u32());
        g.material = i32();
        u32(); u32();                           // selection group, flags
        if (m_.version > 800) { g.lod = i32(); str(80); }
        need(28, where);
        p_ += 28;                               // extent
        n = u32();
        needN(n, 28, where + " extents");
        p_ += size_t(n) * 28;
        if (peek("TANG")) {
            p_ += 4;
            n = u32();
            needN(n, 16, where + " TANG");
            for (uint32_t i = 0; i < n; ++i) { float4 t; t.x = f32(); t.y = f32(); t.z = f32(); t.w = f32(); g.tangents.push_back(t); }
        }
        if (peek("SKIN")) {
            p_ += 4;
            n = u32();
            needN(n, 1, where + " SKIN");
            g.skin.assign(d_ + p_, d_ + p_ + n);
            p_ += n;
        }
        n = count("UVAS", 0);
        for (uint32_t s = 0; s < n; ++s) {
            const uint32_t nv = count("UVBS", 8);
            for (uint32_t i = 0; i < nv; ++i) {
                float2 uv; uv.x = f32(); uv.y = f32();
                if (s == 0) g.uv.push_back(uv);
            }
        }
        end_ = outer;
        p_ = ge;
        m_.geosets.push_back(std::move(g));
    }

    std::string path_;
    const uint8_t* d_;
    size_t p_ = 0, end_;
    Mdlx m_;
};

// ---------------------------------------------------------------- conversion to Model

float3 conv(float3 v) { return {v.y, v.z, v.x}; }                          // WC3 (x fwd, y left, z up) -> engine
quat convq(const float* q) { return {q[1], q[2], q[0], q[3]}; }
float3 convScale(const float* s) { return {s[1], s[2], s[0]}; }

struct RawVertex {
    ModelVertex v;
    int joints[4] = {0, 0, 0, 0};
    float weights[4] = {0, 0, 0, 0};
};

// One converted key: glTF-style in / out tangents (per second) for cubic channels.
struct Key { float t = 0; float v[4] = {}, in[4] = {}, out[4] = {}; };

class Builder {
public:
    Builder(const std::string& path, const FileReader& read, std::vector<std::string>* warnings, const Mdlx& x)
        : path_(path), dir_(dirOf(path)), read_(read), warnings_(warnings), x_(x) {}

    std::unique_ptr<Model> build() {
        if (x_.geosets.empty()) fail("no geosets (nothing to draw)");
        m_ = std::make_unique<Model>();
        nodes();
        if (x_.faceTypesSkipped) warn("polygon types other than triangles / strips / fans are skipped");
        materials();
        meshes();
        clips();
        if (x_.emitters) warn("particle emitters, ribbons and lights are not rendered (their nodes are kept)");
        m_->finalize();
        return std::move(m_);
    }

private:
    [[noreturn]] void fail(const std::string& msg) const { throw std::runtime_error(path_ + ": " + msg); }
    void warn(const std::string& msg) { if (warnings_ && warned_.insert(msg).second) warnings_->push_back(path_ + ": " + msg); }

    // ---- skeleton

    void nodes() {
        Model& m = *m_;
        Node root;
        root.name = "root";
        m.nodes.push_back(root);
        pivots_.push_back({0, 0, 0});
        for (size_t i = 0; i < x_.objects.size(); ++i) {
            const Object& o = x_.objects[i];
            if (idToNode_.count(o.id)) { warn("duplicate object id " + std::to_string(o.id) + " ('" + o.name + "') ignored"); continue; }
            idToNode_[o.id] = int(m.nodes.size());
            objOfNode_.push_back(int(i));
            Node n;
            n.name = o.name.empty() ? "object" + std::to_string(o.id) : o.name;
            m.nodes.push_back(n);
            float3 p{0, 0, 0};
            if (o.id >= 0 && size_t(o.id) < x_.pivots.size()) p = conv(x_.pivots[size_t(o.id)]) * kUnit;
            else if (!x_.pivots.empty()) warn("object '" + o.name + "' has no pivot point");
            pivots_.push_back(p);
            if (o.flags & 0xF8) warn("billboarded nodes are not billboarded");
            if (o.flags & 0x07) warn("'don't inherit' node flags are ignored");
        }
        // parents (cycles in broken files are cut at the root)
        for (size_t n = 1; n < m.nodes.size(); ++n) {
            const Object& o = x_.objects[size_t(objOfNode_[n - 1])];
            auto it = idToNode_.find(o.parent);
            m.nodes[n].parent = it == idToNode_.end() ? 0 : it->second;
        }
        for (size_t n = 1; n < m.nodes.size(); ++n) {
            int p = m.nodes[n].parent, steps = 0;
            while (p > 0 && p != int(n) && steps++ < int(m.nodes.size())) p = m.nodes[size_t(p)].parent;
            if (p > 0) { warn("node hierarchy has a cycle; cut at '" + m.nodes[n].name + "'"); m.nodes[n].parent = 0; }
        }
        for (size_t n = 1; n < m.nodes.size(); ++n) m.nodes[n].t = pivots_[n] - pivots_[size_t(m.nodes[n].parent)];
        Skin sk;
        sk.name = "skeleton";
        for (size_t n = 0; n < m.nodes.size(); ++n) { sk.joints.push_back(int(n)); sk.inverseBind.push_back(mat4::translate(-pivots_[n])); }
        m.skins.push_back(std::move(sk));
    }
    int joint(uint32_t objectId) const {
        auto it = idToNode_.find(int32_t(objectId));
        return it == idToNode_.end() ? 0 : it->second;
    }

    // ---- textures and materials

    std::vector<std::string> candidates(const std::string& file) const {
        std::string rel = file;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        std::vector<std::string> c;
        const std::string base = baseName(rel);
        for (const std::string& r : {rel, lower(rel)}) c.push_back(dir_ + r);
        c.push_back(dir_ + base);
        c.push_back(dir_ + lower(base));
        std::string d = dir_;
        while (!d.empty()) {   // relative to the parent directories, up to the content root
            d.pop_back();
            const size_t s = d.find_last_of('/');
            d = s == std::string::npos ? std::string() : d.substr(0, s + 1);
            c.push_back(d + rel);
            c.push_back(d + lower(rel));
        }
        // converted copies next to the model (BLP -> PNG / TGA / DDS)
        const size_t dot = base.find_last_of('.');
        const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
        for (const char* ext : {".png", ".tga", ".dds"}) c.push_back(dir_ + stem + ext);
        std::vector<std::string> out;
        for (auto& s : c) if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        return out;
    }

    int loadImage(const std::string& file, bool repeat) {
        if (file.empty()) return -1;
        const std::string key = file + (repeat ? "|r" : "|c");
        if (auto it = images_.find(key); it != images_.end()) return it->second;
        int& slot = images_[key];
        slot = -1;
        std::string bytes;
        bool found = false;
        for (const std::string& c : candidates(file)) if ((found = read_(c, bytes))) break;
        if (!found) { warn("texture '" + file + "' not found"); return -1; }
        Image img;
        img.name = file;
        std::replace(img.name.begin(), img.name.end(), '\\', '/');
        if (!decodeImage(bytes, img)) { warn("texture '" + file + "': unsupported or broken image"); return -1; }
        img.repeat = repeat;
        slot = int(m_->images.size());
        m_->images.push_back(std::move(img));
        return slot;
    }

    // Replaceable textures that are not player colours (trees, cliffs): best effort from the game paths.
    static const char* replaceablePath(uint32_t id) {
        switch (id) {
            case 11: return "ReplaceableTextures/Cliff/Cliff0.blp";
            case 21: return "ReplaceableTextures/Cliff/Cliff1.blp";
            case 31: return "ReplaceableTextures/LordaeronTree/LordaeronSummerTree.blp";
            case 32: return "ReplaceableTextures/AshenvaleTree/AshenTree.blp";
            case 33: return "ReplaceableTextures/BarrensTree/BarrensTree.blp";
            case 34: return "ReplaceableTextures/NorthrendTree/NorthTree.blp";
            case 35: return "ReplaceableTextures/Mushroom/MushroomTree.blp";
            case 36: return "ReplaceableTextures/RuinsTree/RuinsTree.blp";
            case 37: return "ReplaceableTextures/OutlandMushroomTree/MushroomTree.blp";
            default: return nullptr;
        }
    }

    const Texture* tex(int32_t id) const { return id >= 0 && size_t(id) < x_.textures.size() ? &x_.textures[size_t(id)] : nullptr; }

    int texImage(int32_t id) {
        const Texture* t = tex(id);
        if (!t) return -1;
        const bool repeat = (t->flags & 3) != 0;
        if (t->replaceable == 0) return loadImage(t->file, repeat);
        if (const char* p = replaceablePath(t->replaceable)) return loadImage(p, repeat);
        warn("replaceable texture " + std::to_string(t->replaceable) + " is not supported");
        return -1;
    }

    int glowImage() {   // soft round sprite, tinted with the team colour
        if (glowImage_ >= 0) return glowImage_;
        Image img;
        img.name = "#teamglow";
        img.width = img.height = 64;
        img.repeat = false;
        img.rgba.resize(64 * 64 * 4);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const float dx = (float(x) + 0.5f) / 32 - 1, dy = (float(y) + 0.5f) / 32 - 1;
                const float a = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                uint8_t* p = &img.rgba[size_t(y * 64 + x) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = uint8_t(std::lround(a * a * 255));
            }
        glowImage_ = int(m_->images.size());
        m_->images.push_back(std::move(img));
        return glowImage_;
    }

    int teamMaskFromAlpha(int orm) {   // HD: team colour mask in the ORM alpha -> red channel image
        if (orm < 0) return -1;
        Image mask = m_->images[size_t(orm)];
        mask.name += "#team";
        for (size_t i = 0; i + 3 < mask.rgba.size(); i += 4) { mask.rgba[i] = mask.rgba[i + 3]; mask.rgba[i + 1] = mask.rgba[i + 2] = 0; mask.rgba[i + 3] = 255; }
        m_->images.push_back(std::move(mask));
        return int(m_->images.size()) - 1;
    }

    void materials() {
        for (size_t i = 0; i < x_.materials.size(); ++i) m_->materials.push_back(material(x_.materials[i], i));
        defaultMat_ = int(m_->materials.size());
        Material d;
        d.name = "default";
        d.roughness = 0.9f;
        m_->materials.push_back(d);
    }

    Material material(const Mat& wm, size_t index) {
        Material m;
        m.name = "material" + std::to_string(index);
        m.roughness = 0.9f;
        m.vertexColors = false;
        // Reforged HD: one texture per layer (900 / 1000) or one layer with a texture list (1100)
        std::vector<int32_t> hd;
        if (!wm.layers.empty() && wm.layers[0].textures.size() > 1) hd = wm.layers[0].textures;
        else if (lower(wm.shader).find("_hd") != std::string::npos) for (const Layer& l : wm.layers) hd.push_back(l.texture);
        if (!hd.empty()) {
            const Layer& l = wm.layers[0];
            m.baseTex = texImage(hd[0]);
            if (hd.size() > 1) m.normalTex = texImage(hd[1]);
            if (hd.size() > 2) {
                m.mrTex = m.occlusionTex = texImage(hd[2]);
                m.roughness = 1; m.metallic = 1;
                if (m.mrTex >= 0) { m.teamTex = teamMaskFromAlpha(m.mrTex); if (m.teamTex >= 0) m.team = TeamMode::Mask; }
            }
            if (hd.size() > 3) { m.emissiveTex = texImage(hd[3]); if (m.emissiveTex >= 0) m.emissive = {1, 1, 1}; }
            applyFilter(m, l);
            return m;
        }
        int team = -1, glow = -1, main = -1;
        for (size_t k = 0; k < wm.layers.size(); ++k) {
            const Texture* t = tex(wm.layers[k].texture);
            if (!t) continue;
            if (t->replaceable == 1) { if (team < 0) team = int(k); }
            else if (t->replaceable == 2) { if (glow < 0) glow = int(k); }
            else if (main < 0) main = int(k);
        }
        bool twoSided = false;
        for (const Layer& l : wm.layers) twoSided |= (l.shading & 0x10) != 0;
        m.doubleSided = twoSided;
        if (main >= 0) {
            const Layer& l = wm.layers[size_t(main)];
            m.baseTex = texImage(l.texture);
            if (team >= 0 && team < main) { m.team = TeamMode::BaseAlpha; applyFilter(m, wm.layers[size_t(team)]); }
            else applyFilter(m, l);
            if (team > main) m.team = TeamMode::Tint;   // team colour drawn on top: tint (approximation)
        } else if (team >= 0) {
            m.team = TeamMode::Tint;
            applyFilter(m, wm.layers[size_t(team)]);
        } else if (glow >= 0) {
            m.team = TeamMode::Tint;
            m.baseTex = glowImage();
            m.alpha = AlphaMode::Blend;
            m.shading = ShadingModel::Unlit;
            m.castShadow = false;
        } else if (!wm.layers.empty()) applyFilter(m, wm.layers[0]);
        return m;
    }

    void applyFilter(Material& m, const Layer& l) {
        if (l.shading & 1) m.shading = ShadingModel::Unlit;
        if (l.shading & 0x10) m.doubleSided = true;
        switch (l.filter) {
            case 0: m.alpha = AlphaMode::Opaque; break;
            case 1: m.alpha = AlphaMode::Mask; m.alphaCutoff = 0.75f; break;
            case 2: m.alpha = AlphaMode::Blend; break;
            case 3: case 4: m.alpha = AlphaMode::Blend; m.castShadow = false; warn("additive layers are approximated by alpha blending"); break;
            default: m.alpha = AlphaMode::Blend; m.castShadow = false; warn("modulate layers are approximated by alpha blending"); break;
        }
        if (l.alpha < 0.999f) { m.baseColor.w = std::clamp(l.alpha, 0.0f, 1.0f); m.alpha = AlphaMode::Blend; }
        if (m.alpha == AlphaMode::Blend) m.castShadow = false;
    }

    // ---- geometry

    bool geosetHidden(size_t gi) {
        // the first "Stand" sequence decides (geoset alpha animation is not supported)
        const Sequence* stand = nullptr;
        for (const Sequence& s : x_.seqs) if (lower(s.name).rfind("stand", 0) == 0) { stand = &s; break; }
        if (!stand && !x_.seqs.empty()) stand = &x_.seqs[0];
        for (const GeosetAnim& a : x_.geoAnims) {
            if (a.geoset != int32_t(gi)) continue;
            const Track& t = a.alphaTrack;
            if (t.frames.empty()) return a.alpha < 0.01f;
            warn("geoset alpha animation is not supported (geosets keep their visibility in 'Stand')");
            bool any = false, visible = false;
            for (size_t k = 0; k < t.frames.size(); ++k) {
                const bool in = t.globalSeq >= 0 || !stand || (t.frames[k] >= stand->start && t.frames[k] <= stand->end);
                if (!in) continue;
                any = true;
                visible |= t.values[k] >= 0.01f;
            }
            return any && !visible;
        }
        return false;
    }

    void meshes() {
        // winding: Warcraft III front faces are the opposite of glTF's on some exporters; decide by the normals
        double facing = 0;
        for (const Geoset& g : x_.geosets) {
            if (g.nrm.size() != g.pos.size()) continue;
            for (size_t t = 0; t + 2 < g.tris.size(); t += 3) {
                const uint32_t a = g.tris[t], b = g.tris[t + 1], c = g.tris[t + 2];
                if (a >= g.pos.size() || b >= g.pos.size() || c >= g.pos.size()) continue;
                const float3 n = cross(g.pos[b] - g.pos[a], g.pos[c] - g.pos[a]);
                facing += double(dot(n, g.nrm[a] + g.nrm[b] + g.nrm[c]));
            }
        }
        const bool flip = facing < 0;
        MeshDef md;
        md.name = "mesh";
        bool skippedLod = false;
        for (size_t gi = 0; gi < x_.geosets.size(); ++gi) {
            const Geoset& g = x_.geosets[gi];
            if (g.lod > 0) { skippedLod = true; continue; }
            if (g.tris.empty()) continue;
            if (geosetHidden(gi)) { warn("geoset " + std::to_string(gi) + " is hidden in 'Stand' and was skipped"); continue; }
            geoset(g, gi, flip, md);
        }
        if (skippedLod) warn("Reforged LOD geosets other than LOD 0 are skipped");
        if (md.primitives.empty()) fail("no visible geometry");
        Node n;
        n.name = "mesh";
        n.parent = 0;
        n.mesh = 0;
        n.skin = 0;
        m_->meshes.push_back(std::move(md));
        m_->nodes.push_back(n);
    }

    void geoset(const Geoset& g, size_t gi, bool flip, MeshDef& md) {
        const std::string where = "geoset " + std::to_string(gi);
        const size_t nv = g.pos.size();
        for (uint32_t i : g.tris) if (i >= nv) fail(where + ": face index " + std::to_string(i) + " out of range (" + std::to_string(nv) + " vertices)");
        // matrix groups -> bone lists
        std::vector<std::vector<int>> groups;
        size_t at = 0;
        for (uint32_t sz : g.groupSizes) {
            std::vector<int> grp;
            for (uint32_t k = 0; k < sz && at < g.groupIndices.size(); ++k, ++at) grp.push_back(joint(g.groupIndices[at]));
            if (grp.size() > 4) { warn("vertex groups with more than 4 bones keep the first 4"); grp.resize(4); }
            groups.push_back(std::move(grp));
        }
        const bool reforgedSkin = g.skin.size() >= nv * 8 && nv > 0;
        bool badGroup = false;
        std::vector<RawVertex> verts(nv);
        for (size_t i = 0; i < nv; ++i) {
            RawVertex& rv = verts[i];
            rv.v.pos = conv(g.pos[i]) * kUnit;
            rv.v.nrm = i < g.nrm.size() ? normalize(conv(g.nrm[i])) : float3{0, 1, 0};
            if (i < g.uv.size()) rv.v.uv = g.uv[i];
            if (i < g.tangents.size()) {
                const float4& t = g.tangents[i];
                const float3 tc = normalize(conv({t.x, t.y, t.z}));
                rv.v.tangent = {tc.x, tc.y, tc.z, (t.w < 0) != flip ? -1.0f : 1.0f};
            }
            if (reforgedSkin) {
                const uint8_t* s = &g.skin[i * 8];
                for (int k = 0; k < 4; ++k) { rv.joints[k] = joint(s[k]); rv.weights[k] = float(s[4 + k]) / 255.0f; }
            } else {
                const uint32_t gr = i < g.vgroup.size() ? g.vgroup[i] : 0xffffffffu;
                if (gr < groups.size() && !groups[gr].empty()) {
                    const auto& grp = groups[gr];
                    for (size_t k = 0; k < grp.size(); ++k) { rv.joints[k] = grp[k]; rv.weights[k] = 1.0f / float(grp.size()); }
                } else {
                    badGroup = true;
                    rv.joints[0] = 0; rv.weights[0] = 1;
                }
            }
        }
        if (badGroup && !groups.empty()) warn(where + ": vertices without a valid matrix group stay static");
        Primitive prim;
        prim.material = g.material >= 0 && size_t(g.material) < x_.materials.size() ? g.material : defaultMat_;
        if (prim.material == defaultMat_ && !x_.materials.empty()) warn(where + ": material " + std::to_string(g.material) + " out of range");
        std::vector<uint32_t> idx = g.tris;
        if (flip) for (size_t t = 0; t + 2 < idx.size(); t += 3) std::swap(idx[t + 1], idx[t + 2]);
        const bool needTangents = g.tangents.size() < nv && m_->materials[size_t(prim.material)].normalTex >= 0;
        // split so each chunk's joints fit one palette
        std::vector<uint32_t> chunk;
        std::set<int> used;
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            std::set<int> merged = used;
            for (int k = 0; k < 3; ++k) {
                const RawVertex& v = verts[idx[t + size_t(k)]];
                for (int e = 0; e < 4; ++e) if (v.weights[e] > 0) merged.insert(v.joints[e]);
            }
            if (int(merged.size()) > kMaxPaletteJoints && !chunk.empty()) {
                append(md, prim, verts, chunk, needTangents);
                chunk.clear();
                merged.clear();
                for (int k = 0; k < 3; ++k) {
                    const RawVertex& v = verts[idx[t + size_t(k)]];
                    for (int e = 0; e < 4; ++e) if (v.weights[e] > 0) merged.insert(v.joints[e]);
                }
            }
            used = std::move(merged);
            chunk.insert(chunk.end(), {idx[t], idx[t + 1], idx[t + 2]});
        }
        if (!chunk.empty()) append(md, prim, verts, chunk, needTangents);
    }

    void append(MeshDef& md, Primitive prim, const std::vector<RawVertex>& verts, const std::vector<uint32_t>& idx, bool needTangents) {
        Model& m = *m_;
        std::vector<int32_t> remap(verts.size(), -1);
        std::map<int, int> palette;
        const uint32_t base = uint32_t(m.vertices.size());
        prim.firstIndex = uint32_t(m.indices.size());
        prim.indexCount = uint32_t(idx.size());
        for (uint32_t i : idx) {
            if (remap[i] < 0) {
                remap[i] = int32_t(m.vertices.size() - base);
                const RawVertex& rv = verts[i];
                ModelVertex v = rv.v;
                float w[4], sum = 0;
                for (int e = 0; e < 4; ++e) { w[e] = std::max(0.0f, rv.weights[e]); sum += w[e]; }
                int js[4] = {rv.joints[0], rv.joints[1], rv.joints[2], rv.joints[3]};
                if (sum <= 0) { w[0] = sum = 1; js[0] = 0; }
                int total = 0, last = -1;
                for (int e = 0; e < 4; ++e) {
                    if (w[e] <= 0) continue;
                    auto it = palette.find(js[e]);
                    if (it == palette.end()) it = palette.emplace(js[e], int(palette.size())).first;
                    v.joints[e] = uint8_t(it->second);
                    v.weights[e] = uint8_t(std::lround(w[e] / sum * 255.0f));
                    total += v.weights[e];
                    last = e;
                }
                if (last >= 0) v.weights[last] = uint8_t(std::clamp(int(v.weights[last]) + 255 - total, 0, 255));   // exact sum 255
                m.vertices.push_back(v);
            }
            m.indices.push_back(base + uint32_t(remap[i]));
        }
        prim.jointPalette.resize(palette.size());
        for (auto& [j, slot] : palette) prim.jointPalette[size_t(slot)] = uint16_t(j);
        if (prim.jointPalette.empty()) prim.jointPalette.push_back(0);
        if (needTangents) generateTangents(m.vertices, m.indices, prim.firstIndex, prim.indexCount);
        md.primitives.push_back(std::move(prim));
    }

    // ---- animation

    // Converted value of key k of a track (translation: plus the bind offset `base`).
    static void value(AnimChannel::Path path, const float* src, float3 base, bool isPoint, float* out) {
        if (path == AnimChannel::Rotation) { const quat q = convq(src); out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w; return; }
        float3 v = path == AnimChannel::Scale ? convScale(src) : conv({src[0], src[1], src[2]}) * kUnit;
        if (path == AnimChannel::Translation && isPoint) v = v + base;
        out[0] = v.x; out[1] = v.y; out[2] = v.z;
    }

    // Keys of one window [w0, w1] (ms) as a clip-relative key list; loop: wrap boundary values.
    std::vector<Key> windowKeys(const Track& t, AnimChannel::Path path, float3 base, int32_t w0, int32_t w1, bool loop, bool& cubic) const {
        const int comps = path == AnimChannel::Rotation ? 4 : 3;
        const int sc = t.comps;
        struct Raw { float t; float v[4], a[4], b[4]; bool synthetic; };   // a = in control, b = out control
        std::vector<Raw> raw;
        for (size_t k = 0; k < t.frames.size(); ++k) {
            if (t.frames[k] < w0 || t.frames[k] > w1) continue;
            if (!raw.empty() && float(t.frames[k] - w0) <= raw.back().t) continue;   // keys must increase
            Raw r{};
            r.t = float(t.frames[k] - w0);
            const bool point = t.interp != 2;   // Hermite tangents are derivatives, Bezier controls are points
            value(path, &t.values[k * size_t(sc)], base, true, r.v);
            if (t.interp > 1 && t.inTans.size() >= (k + 1) * size_t(sc)) {
                value(path, &t.inTans[k * size_t(sc)], base, point, r.a);
                value(path, &t.outTans[k * size_t(sc)], base, point, r.b);
            }
            raw.push_back(r);
        }
        if (raw.empty()) return {};
        // shortest rotation path between keys (control quaternions follow their key)
        if (path == AnimChannel::Rotation)
            for (size_t k = 1; k < raw.size(); ++k) {
                const float d = raw[k].v[0] * raw[k - 1].v[0] + raw[k].v[1] * raw[k - 1].v[1] + raw[k].v[2] * raw[k - 1].v[2] + raw[k].v[3] * raw[k - 1].v[3];
                if (d < 0) for (int c = 0; c < 4; ++c) { raw[k].v[c] = -raw[k].v[c]; raw[k].a[c] = -raw[k].a[c]; raw[k].b[c] = -raw[k].b[c]; }
            }
        const float len = float(w1 - w0);
        auto synth = [&](float time, const float* v) {
            Raw r{};
            r.t = time;
            r.synthetic = true;
            for (int c = 0; c < comps; ++c) { r.v[c] = v[c]; r.a[c] = r.b[c] = t.interp == 2 && path != AnimChannel::Rotation ? 0.0f : v[c]; }
            return r;
        };
        if (raw.front().t > 0 || raw.back().t < len) {
            float s[4], e[4];
            if (loop && raw.size() > 1) {
                const Raw &last = raw.back(), &first = raw.front();
                const float gap = (len - last.t) + first.t;
                const float u = gap > 0 ? (len - last.t) / gap : 0;
                float sign = 1;
                if (path == AnimChannel::Rotation) {
                    float d = 0;
                    for (int c = 0; c < 4; ++c) d += first.v[c] * last.v[c];
                    if (d < 0) sign = -1;
                }
                for (int c = 0; c < comps; ++c) s[c] = e[c] = last.v[c] + (first.v[c] * sign - last.v[c]) * u;
                if (path == AnimChannel::Rotation) {
                    const quat q = qnormalize({s[0], s[1], s[2], s[3]});
                    s[0] = e[0] = q.x; s[1] = e[1] = q.y; s[2] = e[2] = q.z; s[3] = e[3] = q.w;
                    // the start key must be on the first key's hemisphere
                    float d = 0;
                    for (int c = 0; c < 4; ++c) d += s[c] * first.v[c];
                    if (d < 0) for (int c = 0; c < 4; ++c) s[c] = -s[c];
                }
            } else {
                std::memcpy(s, raw.front().v, sizeof s);
                std::memcpy(e, raw.back().v, sizeof e);
            }
            if (raw.front().t > 0) raw.insert(raw.begin(), synth(0, s));
            if (raw.back().t < len) raw.push_back(synth(len, e));
        }
        cubic = t.interp > 1;
        std::vector<Key> keys(raw.size());
        for (size_t k = 0; k < raw.size(); ++k) {
            keys[k].t = raw[k].t / 1000.0f;
            std::memcpy(keys[k].v, raw[k].v, sizeof keys[k].v);
        }
        if (!cubic) return keys;
        // per segment Hermite derivatives (per unit parameter) -> glTF tangents (per second)
        for (size_t k = 0; k + 1 < raw.size(); ++k) {
            const Raw &p = raw[k], &q = raw[k + 1];
            const float dt = (q.t - p.t) / 1000.0f;
            for (int c = 0; c < comps; ++c) {
                float m0, m1;
                if (path == AnimChannel::Rotation) {          // squad with lerps: a cubic
                    m0 = (q.v[c] - p.v[c]) + 2 * (p.b[c] - p.v[c]);
                    m1 = (q.v[c] - p.v[c]) - 2 * (q.a[c] - q.v[c]);
                } else if (t.interp == 2) { m0 = p.b[c]; m1 = q.a[c]; }   // Hermite
                else { m0 = 3 * (p.b[c] - p.v[c]); m1 = 3 * (q.v[c] - q.a[c]); }   // Bezier
                keys[k].out[c] = dt > 0 ? m0 / dt : 0;
                keys[k + 1].in[c] = dt > 0 ? m1 / dt : 0;
            }
        }
        return keys;
    }

    static void sampleKeys(const std::vector<Key>& k, bool cubic, int comps, float t, float* out) {
        if (t <= k.front().t || k.size() == 1) { std::memcpy(out, k.front().v, sizeof(float) * size_t(comps)); return; }
        if (t >= k.back().t) { std::memcpy(out, k.back().v, sizeof(float) * size_t(comps)); return; }
        size_t i = 0;
        while (i + 2 < k.size() && k[i + 1].t <= t) ++i;
        const float dt = k[i + 1].t - k[i].t, u = dt > 0 ? (t - k[i].t) / dt : 0;
        for (int c = 0; c < comps; ++c) {
            if (!cubic) { out[c] = k[i].v[c] + (k[i + 1].v[c] - k[i].v[c]) * u; continue; }
            const float u2 = u * u, u3 = u2 * u;
            out[c] = (2 * u3 - 3 * u2 + 1) * k[i].v[c] + (u3 - 2 * u2 + u) * k[i].out[c] * dt + (-2 * u3 + 3 * u2) * k[i + 1].v[c] + (u3 - u2) * k[i + 1].in[c] * dt;
        }
    }

    void channel(AnimClip& clip, const Sequence& sq, const Track& t, int node, AnimChannel::Path path) {
        if (t.frames.empty() || t.values.size() < t.frames.size() * size_t(t.comps)) return;
        if ((path == AnimChannel::Rotation) != (t.comps == 4)) return;
        const int comps = path == AnimChannel::Rotation ? 4 : 3;
        const float3 base = m_->nodes[size_t(node)].t;
        const float clipLen = float(sq.end - sq.start) / 1000.0f;
        bool cubic = false;
        std::vector<Key> keys;
        if (t.globalSeq >= 0) {
            if (size_t(t.globalSeq) >= x_.globals.size()) { warn("track uses a missing global sequence"); return; }
            const int32_t dur = x_.globals[size_t(t.globalSeq)];
            std::vector<Key> period;
            if (dur > 0) period = windowKeys(t, path, base, 0, dur, true, cubic);
            if (period.empty()) {   // first key beyond the global sequence: it is the constant value
                if (dur > 0 && t.frames[0] <= dur) return;
                cubic = false;
                Key k;
                value(path, t.values.data(), base, true, k.v);
                keys.push_back(k);
            } else {
                const float pd = float(dur) / 1000.0f;
                for (int rep = 0; float(rep) * pd < clipLen + 1e-6f && rep < 10000; ++rep) {
                    for (size_t k = 0; k < period.size(); ++k) {
                        Key key = period[k];
                        key.t += float(rep) * pd;
                        if (!keys.empty() && key.t <= keys.back().t + 1e-6f) {
                            // seam between periods: one key when continuous, else a jump (as the game wraps the time)
                            if (std::equal(key.v, key.v + comps, keys.back().v)) { std::memcpy(keys.back().out, key.out, sizeof key.out); continue; }
                            key.t = keys.back().t + 1e-4f;
                        }
                        if (key.t > clipLen) break;
                        keys.push_back(key);
                    }
                }
                if (keys.back().t < clipLen - 1e-6f) {
                    // cut the tiled curve at the clip end
                    Key end;
                    end.t = clipLen;
                    sampleKeys(period, cubic, comps, std::fmod(clipLen, pd), end.v);
                    keys.push_back(end);
                }
            }
        } else {
            keys = windowKeys(t, path, base, sq.start, sq.end, !sq.nonLooping, cubic);
            if (keys.empty()) return;   // no keys in this sequence: the node keeps its bind pose
        }
        AnimChannel c;
        c.node = node;
        c.path = path;
        c.comps = uint8_t(comps);
        c.interp = t.interp == 0 ? AnimChannel::Step : cubic ? AnimChannel::CubicSpline : AnimChannel::Linear;
        for (const Key& k : keys) {
            c.times.push_back(k.t);
            if (cubic) c.values.insert(c.values.end(), k.in, k.in + comps);
            c.values.insert(c.values.end(), k.v, k.v + comps);
            if (cubic) c.values.insert(c.values.end(), k.out, k.out + comps);
        }
        clip.channels.push_back(std::move(c));
    }

    void clips() {
        for (const Sequence& sq : x_.seqs) {
            AnimClip clip;
            clip.name = sq.name.empty() ? "sequence" + std::to_string(m_->clips.size()) : sq.name;
            if (sq.end < sq.start) { warn("sequence '" + clip.name + "' ends before it starts; skipped"); continue; }
            clip.duration = float(sq.end - sq.start) / 1000.0f;
            for (size_t n = 1; n <= objOfNode_.size(); ++n) {
                const Object& o = x_.objects[size_t(objOfNode_[n - 1])];
                channel(clip, sq, o.t, int(n), AnimChannel::Translation);
                channel(clip, sq, o.r, int(n), AnimChannel::Rotation);
                channel(clip, sq, o.s, int(n), AnimChannel::Scale);
            }
            m_->clips.push_back(std::move(clip));
        }
    }

    std::string path_, dir_;
    const FileReader& read_;
    std::vector<std::string>* warnings_;
    std::set<std::string> warned_;
    const Mdlx& x_;
    std::unique_ptr<Model> m_;
    std::map<int32_t, int> idToNode_;
    std::vector<int> objOfNode_;     // node n (>= 1) -> object index (n - 1)
    std::vector<float3> pivots_;     // per node, engine space
    std::map<std::string, int> images_;
    int glowImage_ = -1, defaultMat_ = 0;
};

} // namespace

std::unique_ptr<Model> loadMdx(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings) {
    const Mdlx x = MdxReader(path, bytes).parse();
    return Builder(path, read, warnings, x).build();
}

std::unique_ptr<Model> loadMdl(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings) {
    const Mdlx x = parseMdl(path, bytes);
    return Builder(path, read, warnings, x).build();
}

} // namespace rts::render
