// Collada (.dae, 1.4 / 1.5) loader on a small built-in XML reader (Xml.h).
//
// Supported:
//   asset         <unit meter> and <up_axis> (Y_UP / Z_UP / X_UP): a "root" node scales and rotates
//                 the scene to metres, Y up; everything below keeps the file's own values.
//   geometry      <mesh> with <source> float_array + accessor (stride / offset), <vertices>,
//                 <triangles>, <polylist>, <polygons> (holes ignored), <trifans>, <tristrips>;
//                 inputs VERTEX / POSITION, NORMAL, TEXCOORD (lowest set), COLOR. Missing normals
//                 are smoothed per position (area weighted).
//   materials     library_materials -> library_effects profile_COMMON phong / blinn / lambert /
//                 constant: diffuse colour or texture (sampler2D -> surface -> library_images),
//                 emission, transparent + transparency with the four opaque modes, shininess ->
//                 roughness, double_sided and bump (normal map) from the common <extra> techniques;
//                 bound per instance through <bind_material> / <instance_material symbol target>.
//   scene         visual scene hierarchy with <matrix> / <translate> / <rotate> / <scale> / <lookat>
//                 stacks, <instance_geometry>, <instance_controller>, <instance_node>.
//   skins         <skin>: bind_shape_matrix (baked into the vertices), joints (Name_array sids /
//                 names / IDREF_array ids, searched below <skeleton> first), INV_BIND_MATRIX,
//                 vertex_weights -> 4 strongest influences, primitives split so each uses at most
//                 kMaxPaletteJoints joints.
//   animation     nested <animation>, samplers (INPUT / OUTPUT / INTERPOLATION: LINEAR, STEP and
//                 BEZIER with IN_ / OUT_TANGENT, resampled; HERMITE / CARDINAL / BSPLINE are treated
//                 as linear), channels on whole transform elements or
//                 members (matrix, translate.X, rotate.ANGLE, (i)(j), ...). Each animated node's
//                 transform stack is re-evaluated at the union of its key times and decomposed
//                 into T / R / S keys. library_animation_clips become named clips; without clips
//                 everything is one clip "Default".
// Not supported (warned): morph controllers (the base mesh is used), non-transform animation
// targets (material colours, morph weights, visibility), <skew>, extra UV sets, lines, physics,
// cameras and lights (silently ignored).
#include "Model.h"
#include "ModelInterchange.h"
#include "Xml.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace rts::render {

using namespace interchange;
using xml::Element;

namespace {

constexpr float kPi = 3.14159265358979f;

// ---- small helpers

mat4 rowMajor(const float* f) {
    mat4 m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m.at(r, c) = f[r * 4 + c];
    return m;
}

quat quatFromMatrix(const mat4& m) {   // rotation-only 3x3
    const float tr = m.at(0, 0) + m.at(1, 1) + m.at(2, 2);
    quat q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2;
        q = {(m.at(2, 1) - m.at(1, 2)) / s, (m.at(0, 2) - m.at(2, 0)) / s, (m.at(1, 0) - m.at(0, 1)) / s, 0.25f * s};
    } else if (m.at(0, 0) > m.at(1, 1) && m.at(0, 0) > m.at(2, 2)) {
        float s = std::sqrt(1.0f + m.at(0, 0) - m.at(1, 1) - m.at(2, 2)) * 2;
        q = {0.25f * s, (m.at(0, 1) + m.at(1, 0)) / s, (m.at(0, 2) + m.at(2, 0)) / s, (m.at(2, 1) - m.at(1, 2)) / s};
    } else if (m.at(1, 1) > m.at(2, 2)) {
        float s = std::sqrt(1.0f + m.at(1, 1) - m.at(0, 0) - m.at(2, 2)) * 2;
        q = {(m.at(0, 1) + m.at(1, 0)) / s, 0.25f * s, (m.at(1, 2) + m.at(2, 1)) / s, (m.at(0, 2) - m.at(2, 0)) / s};
    } else {
        float s = std::sqrt(1.0f + m.at(2, 2) - m.at(0, 0) - m.at(1, 1)) * 2;
        q = {(m.at(0, 2) + m.at(2, 0)) / s, (m.at(1, 2) + m.at(2, 1)) / s, 0.25f * s, (m.at(1, 0) - m.at(0, 1)) / s};
    }
    return qnormalize(q);
}

// TRS decomposition; returns false when the matrix has shear the TRS form cannot hold.
bool decompose(const mat4& m, float3& t, quat& r, float3& s) {
    t = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    float3 c[3];
    for (int i = 0; i < 3; ++i) c[i] = {m.at(0, i), m.at(1, i), m.at(2, i)};
    s = {length(c[0]), length(c[1]), length(c[2])};
    if (dot(cross(c[0], c[1]), c[2]) < 0) s.x = -s.x;   // mirror: one negative scale
    mat4 rot;
    for (int i = 0; i < 3; ++i) {
        const float k = [&] { const float v = i == 0 ? s.x : i == 1 ? s.y : s.z; return std::fabs(v) > 1e-12f ? 1.0f / v : 0.0f; }();
        rot.at(0, i) = c[i].x * k; rot.at(1, i) = c[i].y * k; rot.at(2, i) = c[i].z * k;
    }
    r = quatFromMatrix(rot);
    const float d01 = std::fabs(dot(normalize(c[0]), normalize(c[1]))), d02 = std::fabs(dot(normalize(c[0]), normalize(c[2]))),
                d12 = std::fabs(dot(normalize(c[1]), normalize(c[2])));
    return d01 < 1e-3f && d02 < 1e-3f && d12 < 1e-3f;
}

// UTF-16 files (with a BOM) to UTF-8; anything else is returned as is
std::string toUtf8(const std::string& b) {
    if (b.size() < 2) return b;
    const bool le = uint8_t(b[0]) == 0xFF && uint8_t(b[1]) == 0xFE, be = uint8_t(b[0]) == 0xFE && uint8_t(b[1]) == 0xFF;
    if (!le && !be) return b;
    std::string o;
    o.reserve(b.size() / 2);
    for (size_t i = 2; i + 1 < b.size(); i += 2) {
        uint32_t c = le ? uint8_t(b[i]) | (uint8_t(b[i + 1]) << 8) : (uint8_t(b[i]) << 8) | uint8_t(b[i + 1]);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < b.size()) {
            const uint32_t d = le ? uint8_t(b[i + 2]) | (uint8_t(b[i + 3]) << 8) : (uint8_t(b[i + 2]) << 8) | uint8_t(b[i + 3]);
            if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
        }
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
        else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 63)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    }
    return o;
}

std::string urlId(const std::string& u) { return !u.empty() && u[0] == '#' ? u.substr(1) : u; }

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Whitespace-separated numbers. Returns the number of tokens that were not numbers (read as 0).
template <class T> size_t parseNumbers(const std::string& s, std::vector<T>& out) {
    size_t bad = 0;
    const char* p = s.data();
    const char* e = p + s.size();
    while (true) {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        if (p >= e) break;
        const char* b = p;
        while (p < e && !(*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        T v{};
        if (*b == '+') ++b;
        if constexpr (std::is_floating_point_v<T>) {
            // strtod: floating-point std::from_chars needs macOS 26 / iOS 26 in Apple's libc++
            std::string tok(b, p);
            char* end = nullptr;
            v = T(std::strtod(tok.c_str(), &end));   // also "1.#QNAN", "inf", hex floats
            if (end == tok.c_str() || !std::isfinite(v)) { v = 0; ++bad; }
        } else {
            auto res = std::from_chars(b, p, v);
            if (res.ec != std::errc() || res.ptr != p) { v = 0; ++bad; }
        }
        out.push_back(v);
    }
    return bad;
}

std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t b = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

struct Source {
    std::vector<float> f;             // count * stride values (accessor offset applied)
    std::vector<std::string> names;   // Name_array / IDREF_array, one per element
    size_t count = 0;
    int stride = 1;
    float at(size_t i, int k) const { return i < count && k < stride ? f[i * size_t(stride) + size_t(k)] : 0.0f; }
};

struct RawVertex {   // vertex before skinning palettes: position index for weights
    ModelVertex v;
    int pos = 0;
};
struct GeoPrim {
    std::string symbol;
    std::vector<RawVertex> verts;
    std::vector<uint32_t> idx;
};
struct Geometry {
    std::string name;
    std::vector<GeoPrim> prims;
    size_t positions = 0;
};

struct Xform {          // one element of a node's transform stack
    enum Kind : uint8_t { Matrix, Translate, Rotate, Scale, LookAt } kind = Matrix;
    std::string sid;
    float v[16] = {};
    int n = 0;
};

struct Sampler {
    enum Interp : uint8_t { Linear, Step, Bezier };
    std::vector<float> in, out;
    std::vector<uint8_t> interp;            // per key (the segment that starts there)
    std::vector<float> inTan, outTan;       // BEZIER control points, tanStride floats per key
    int stride = 1, tanStride = 0;
    bool step = false, bezier = false;      // all keys STEP / any key BEZIER
};
struct Track {
    int node = -1;      // engine node
    int xform = -1;     // index in that node's stack
    int first = 0, comps = 0;
    int sampler = -1;
};

class DaeReader {
public:
    DaeReader(const std::string& path, const FileReader& read, std::vector<std::string>* w)
        : path_(path), warnings_(w), model_(std::make_unique<Model>()), tex_{path, dirOf(path), read, w, *model_, {}} {}

    std::unique_ptr<Model> load(const std::string& bytes) {
        try { doc_ = xml::parse(toUtf8(bytes)); }
        catch (const std::exception& e) { fail(e.what()); }
        if (doc_.name != "COLLADA") fail("not a Collada file (root element <" + doc_.name + ">)");
        indexIds(doc_, 0);
        readAsset();
        readScene();
        readAnimations();
        Model& m = *model_;
        if (m.meshes.empty() && m.clips.empty()) fail("no geometry in the visual scene");
        if (m.meshes.empty()) warn("no geometry: only the skeleton and its animation are loaded");
        m.finalize();
        return std::move(model_);
    }

private:
    [[noreturn]] void fail(const std::string& w) const { throw std::runtime_error(path_ + ": " + w); }
    void warn(const std::string& w) {
        if (warnings_ && warned_.insert(w).second) warnings_->push_back(path_ + ": " + w);
    }

    void indexIds(const Element& e, int depth) {
        if (depth > 256) return;
        if (const std::string* id = e.findAttr("id")) ids_.emplace(*id, &e);
        for (auto& c : e.children) indexIds(c, depth + 1);
    }
    const Element* byId(const std::string& url) const {
        auto it = ids_.find(urlId(url));
        return it == ids_.end() ? nullptr : it->second;
    }

    // ---- asset

    void readAsset() {
        const Element* asset = doc_.child("asset");
        float meter = 1;
        std::string up = "Y_UP";
        if (asset) {
            if (const Element* u = asset->child("unit")) {
                const float v = std::strtof(u->attr("meter", "1").c_str(), nullptr);
                if (v > 0 && std::isfinite(v)) meter = v;
            }
            if (const Element* a = asset->child("up_axis")) up = trimmed(a->text);
        }
        quat r;
        if (up == "Z_UP") r = quat::axisAngle({1, 0, 0}, -kPi / 2);
        else if (up == "X_UP") r = quat::axisAngle({0, 0, 1}, kPi / 2);
        else if (up != "Y_UP") warn("unknown up_axis '" + up + "', assuming Y_UP");
        if (std::fabs(meter - 1) > 1e-6f || up == "Z_UP" || up == "X_UP") {
            Node root;
            root.name = "root";
            root.r = r;
            root.s = {meter, meter, meter};
            model_->nodes.push_back(root);
            info_.push_back({});
            rootNode_ = 0;
        }
    }

    // ---- sources

    const Source& source(const std::string& url) {
        const std::string id = urlId(url);
        if (auto it = sources_.find(id); it != sources_.end()) return it->second;
        Source& s = sources_[id];
        const Element* e = byId(id);
        if (!e || e->name != "source") fail("source '" + id + "' not found");
        const Element* acc = nullptr;
        if (const Element* tc = e->child("technique_common")) acc = tc->child("accessor");
        const Element* arr = nullptr;
        for (const char* k : {"float_array", "Name_array", "IDREF_array", "int_array", "bool_array", "SIDREF_array"})
            if ((arr = e->child(k))) break;
        if (!arr) fail("source '" + id + "' without a data array");
        std::vector<float> raw;
        std::vector<std::string> rawNames;
        const bool isNames = arr->name == "Name_array" || arr->name == "IDREF_array" || arr->name == "SIDREF_array";
        if (isNames) rawNames = tokens(arr->text);
        else if (arr->name == "bool_array") for (auto& t : tokens(arr->text)) raw.push_back(t == "true" || t == "1" ? 1.0f : 0.0f);
        else if (parseNumbers(arr->text, raw)) warn("source '" + id + "': non-numeric values read as 0");
        const size_t total = isNames ? rawNames.size() : raw.size();
        size_t count = total, offset = 0;
        int stride = 1;
        if (acc) {
            count = size_t(std::strtoull(acc->attr("count", "0").c_str(), nullptr, 10));
            offset = size_t(std::strtoull(acc->attr("offset", "0").c_str(), nullptr, 10));
            const long st = std::strtol(acc->attr("stride", "1").c_str(), nullptr, 10);
            if (st < 1 || st > 64) fail("source '" + id + "': bad accessor stride");
            stride = int(st);
            if (offset > total || count > (total - offset) / size_t(stride)) {
                warn("source '" + id + "': accessor larger than its array; clamped");
                count = offset > total ? 0 : (total - offset) / size_t(stride);
            }
        }
        s.count = count;
        s.stride = stride;
        if (isNames) s.names.assign(rawNames.begin() + std::ptrdiff_t(offset), rawNames.begin() + std::ptrdiff_t(offset + count * size_t(stride)));
        else s.f.assign(raw.begin() + std::ptrdiff_t(offset), raw.begin() + std::ptrdiff_t(offset + count * size_t(stride)));
        return s;
    }

    // ---- materials

    struct ColorTex { bool has = false; float4 c{1, 1, 1, 1}; std::string tex; };
    ColorTex colorTex(const Element* e) {
        ColorTex r;
        if (!e) return r;
        if (const Element* c = e->child("color")) {
            std::vector<float> v;
            parseNumbers(c->text, v);
            if (v.size() >= 3) { r.has = true; r.c = {v[0], v[1], v[2], v.size() >= 4 ? v[3] : 1.0f}; }
        }
        if (const Element* t = e->child("texture")) { r.has = true; r.tex = t->attr("texture"); }
        return r;
    }
    static float floatOf(const Element* e, float def) {
        if (!e) return def;
        const Element* f = e->child("float");
        if (!f) return def;
        char* end = nullptr;
        const std::string t = trimmed(f->text);
        float v = std::strtof(t.c_str(), &end);
        return end == t.c_str() || !std::isfinite(v) ? def : v;
    }
    // texture attribute -> file: sampler2D newparam -> surface newparam -> image, or an image id
    std::string textureFile(const Element& effect, const std::string& ref) {
        std::map<std::string, const Element*> params;
        std::function<void(const Element&, int)> collect = [&](const Element& e, int d) {
            if (d > 8) return;
            for (auto& c : e.children) {
                if (c.name == "newparam") params[c.attr("sid")] = &c;
                else if (c.name == "profile_COMMON" || c.name == "technique") collect(c, d + 1);
            }
        };
        collect(effect, 0);
        std::string imageId = ref;
        for (int hop = 0; hop < 4; ++hop) {
            auto it = params.find(imageId);
            if (it == params.end()) break;
            const Element* np = it->second;
            if (const Element* s2 = np->child("sampler2D")) {
                if (const Element* src = s2->child("source")) { imageId = trimmed(src->text); continue; }
                if (const Element* ii = s2->child("instance_image")) { imageId = urlId(ii->attr("url")); break; }
            }
            if (const Element* sf = np->child("surface")) {
                if (const Element* init = sf->child("init_from")) { imageId = trimmed(init->text); break; }
            }
            break;
        }
        const Element* img = byId(imageId);
        if (!img || img->name != "image") { warn("texture '" + ref + "': image not found"); return {}; }
        const Element* init = img->child("init_from");
        if (!init) { warn("image '" + imageId + "' has no init_from (embedded images are not supported)"); return {}; }
        std::string file = init->child("ref") ? init->child("ref")->text : init->text;
        file = urlDecode(trimmed(file));
        if (file.rfind("file:///", 0) == 0) file = file.substr(8);
        else if (file.rfind("file://", 0) == 0) file = file.substr(7);
        if (file.rfind("./", 0) == 0) file = file.substr(2);
        return file;
    }

    int material(const std::string& id) {
        if (auto it = materials_.find(id); it != materials_.end()) return it->second;
        const Element* me = byId(id);
        if (!me || me->name != "material") { warn("material '" + id + "' not found"); return materials_[id] = defaultMaterial(); }
        int& slot = materials_[id];
        Material m;
        m.name = me->attr("name", id);
        const Element* ie = me->child("instance_effect");
        const Element* fx = ie ? byId(ie->attr("url")) : nullptr;
        if (!fx || fx->name != "effect") { warn("material '" + m.name + "': effect not found"); }
        const Element* prof = fx ? fx->child("profile_COMMON") : nullptr;
        if (fx && !prof) warn("material '" + m.name + "': only profile_COMMON effects are supported");
        const Element* tech = prof ? prof->child("technique") : nullptr;
        const Element* sh = nullptr;
        if (tech)
            for (const char* k : {"phong", "blinn", "lambert", "constant"})
                if ((sh = tech->child(k))) break;
        if (sh) {
            const ColorTex diffuse = colorTex(sh->child("diffuse"));
            const ColorTex emission = colorTex(sh->child("emission"));
            if (diffuse.has) m.baseColor = diffuse.c;
            if (!diffuse.tex.empty()) {
                const std::string f = textureFile(*fx, diffuse.tex);
                m.baseTex = f.empty() ? -1 : tex_.load(f);
                if (m.baseTex >= 0) m.baseColor = {1, 1, 1, 1};
            }
            if (sh->name == "constant") {
                m.shading = ShadingModel::Unlit;
                if (emission.has && emission.tex.empty()) m.baseColor = emission.c;
            } else if (emission.has) {
                if (!emission.tex.empty()) {
                    const std::string f = textureFile(*fx, emission.tex);
                    m.emissiveTex = f.empty() ? -1 : tex_.load(f);
                    if (m.emissiveTex >= 0) m.emissive = {1, 1, 1};
                } else m.emissive = {emission.c.x, emission.c.y, emission.c.z};
            }
            if (sh->name == "lambert") m.roughness = 0.9f;
            else if (sh->child("shininess")) {
                float ns = floatOf(sh->child("shininess"), 20);
                if (ns > 0 && ns <= 1) ns *= 128;   // some exporters write glossiness 0..1
                m.roughness = shininessToRoughness(ns);
            }
            // transparency (the four opaque modes of the spec)
            const Element* te = sh->child("transparent");
            const float tr = floatOf(sh->child("transparency"), 1);
            float alpha = 1;
            if (te) {
                const ColorTex t = colorTex(te);
                const std::string mode = te->attr("opaque", "A_ONE");
                const float lum = 0.2126f * t.c.x + 0.7152f * t.c.y + 0.0722f * t.c.z;
                if (!t.tex.empty()) {
                    if (t.tex == diffuse.tex) m.alpha = AlphaMode::Blend;
                    else warn("material '" + m.name + "': separate transparency texture ignored (put alpha into the diffuse texture)");
                } else if (mode == "RGB_ZERO") alpha = 1 - lum * tr;
                else if (mode == "A_ZERO") alpha = 1 - t.c.w * tr;
                else if (mode == "RGB_ONE") alpha = lum * tr;
                else alpha = t.c.w * tr;
            } else if (sh->child("transparency")) alpha = tr;
            if (alpha < 0.01f) alpha = 1;   // exporters with the inverted convention (3ds Max: transparency 0 = opaque)
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            m.baseColor.w *= alpha;
            if (m.baseColor.w < 0.999f) m.alpha = AlphaMode::Blend;
        } else if (tech) warn("material '" + m.name + "': unknown shading technique");
        // vendor <extra> techniques: double sided, bump
        if (fx) {
            if (const Element* ds = fx->find("double_sided")) m.doubleSided = trimmed(ds->text) == "1" || trimmed(ds->text) == "true";
            if (const Element* bump = fx->find("bump"))
                if (const Element* t = bump->child("texture")) {
                    const std::string f = textureFile(*fx, t->attr("texture"));
                    m.normalTex = f.empty() ? -1 : tex_.load(f);
                }
        }
        slot = int(model_->materials.size());
        model_->materials.push_back(m);
        return slot;
    }

    int defaultMaterial() {
        if (defaultMat_ < 0) {
            Material m; m.name = "default";
            defaultMat_ = int(model_->materials.size());
            model_->materials.push_back(m);
        }
        return defaultMat_;
    }

    // ---- geometry

    const Geometry& geometry(const std::string& url) {
        const std::string id = urlId(url);
        if (auto it = geometries_.find(id); it != geometries_.end()) return it->second;
        Geometry& g = geometries_[id];
        const Element* ge = byId(id);
        if (!ge || ge->name != "geometry") { warn("geometry '" + id + "' not found"); return g; }
        g.name = ge->attr("name", id);
        const Element* mesh = ge->child("mesh");
        if (!mesh) { warn("geometry '" + g.name + "': only <mesh> is supported"); return g; }
        // <vertices>: semantic -> source, all indexed by the VERTEX index
        std::string vertsId;
        std::vector<std::pair<std::string, std::string>> vinputs;
        if (const Element* v = mesh->child("vertices")) {
            vertsId = v->attr("id");
            v->each("input", [&](const Element& in) { vinputs.push_back({in.attr("semantic"), in.attr("source")}); });
        }
        mesh->each("lines", [&](const Element&) { warn("geometry '" + g.name + "': lines are ignored"); });
        mesh->each("linestrips", [&](const Element&) { warn("geometry '" + g.name + "': lines are ignored"); });
        for (const Element& pe : mesh->children)
            if (pe.name == "triangles" || pe.name == "polylist" || pe.name == "polygons" || pe.name == "trifans" || pe.name == "tristrips")
                primitive(g, pe, vertsId, vinputs);
        return g;
    }

    void primitive(Geometry& g, const Element& pe, const std::string& vertsId, const std::vector<std::pair<std::string, std::string>>& vinputs) {
        struct In { const Source* src = nullptr; int offset = 0; };
        In pos, nrm, uv, col;
        int uvSet = 1 << 30, maxOffset = 0;
        auto assign = [&](const std::string& sem, const std::string& src, int off, int set) {
            if (sem == "POSITION") pos = {&source(src), off};
            else if (sem == "NORMAL" && !nrm.src) nrm = {&source(src), off};
            else if (sem == "TEXCOORD") {
                if (set < uvSet) { uv = {&source(src), off}; if (uvSet != (1 << 30)) warn("geometry '" + g.name + "': only the first UV set is used"); uvSet = set; }
                else warn("geometry '" + g.name + "': only the first UV set is used");
            } else if (sem == "COLOR" && !col.src) col = {&source(src), off};
        };
        pe.each("input", [&](const Element& in) {
            const std::string sem = in.attr("semantic");
            const int off = int(std::strtol(in.attr("offset", "0").c_str(), nullptr, 10));
            const int set = int(std::strtol(in.attr("set", "0").c_str(), nullptr, 10));
            if (off < 0 || off > 64) fail("geometry '" + g.name + "': bad input offset");
            maxOffset = std::max(maxOffset, off);
            if (sem == "VERTEX") {
                if (urlId(in.attr("source")) != vertsId) warn("geometry '" + g.name + "': VERTEX input does not reference <vertices>");
                for (auto& [s, src] : vinputs) assign(s, src, off, 0);
            } else assign(sem, in.attr("source"), off, set);
        });
        if (!pos.src) fail("geometry '" + g.name + "': no POSITION input");
        if (pos.src->stride < 3) fail("geometry '" + g.name + "': positions with fewer than 3 components");
        g.positions = std::max(g.positions, pos.src->count);
        const size_t stride = size_t(maxOffset) + 1;
        // polygons as lists of corner numbers into one index array per <p>
        std::vector<int> ix;
        std::vector<std::vector<size_t>> polys;   // corners (index of the first int of the corner)
        auto addP = [&](const Element& p) -> size_t {
            const size_t base = ix.size();
            parseNumbers(p.text, ix);
            return base;
        };
        const std::string kind = pe.name;
        if (kind == "triangles" || kind == "polylist") {
            const Element* p = pe.child("p");
            if (p) addP(*p);
            const size_t corners = ix.size() / stride;
            std::vector<int> vc;
            if (kind == "polylist") { if (const Element* v = pe.child("vcount")) parseNumbers(v->text, vc); }
            else vc.assign(corners / 3, 3);
            size_t c = 0;
            for (int n : vc) {
                if (n < 0 || c + size_t(n) > corners) fail("geometry '" + g.name + "': <" + kind + "> has fewer indices than its counts need");
                std::vector<size_t> poly;
                for (int k = 0; k < n; ++k) poly.push_back((c + size_t(k)) * stride);
                c += size_t(n);
                polys.push_back(std::move(poly));
            }
        } else {
            bool holes = false;
            auto oneP = [&](const Element& p) {
                const size_t base = addP(p);
                const size_t n = (ix.size() - base) / stride;
                if (kind == "tristrips") {
                    for (size_t k = 2; k < n; ++k) {
                        size_t a = base + (k - 2) * stride, b = base + (k - 1) * stride, c = base + k * stride;
                        if (k % 2) std::swap(a, b);
                        polys.push_back({a, b, c});
                    }
                    return;
                }
                std::vector<size_t> poly;
                for (size_t k = 0; k < n; ++k) poly.push_back(base + k * stride);
                polys.push_back(std::move(poly));
            };
            for (const Element& c : pe.children) {
                if (c.name == "p") oneP(c);
                else if (c.name == "ph") { if (const Element* p = c.child("p")) oneP(*p); holes = true; }
            }
            if (holes) warn("geometry '" + g.name + "': polygon holes are ignored");
        }
        GeoPrim prim;
        prim.symbol = pe.attr("material");
        std::map<std::array<int, 4>, uint32_t> dedup;
        auto value = [&](const In& in, size_t corner) -> int {
            if (!in.src) return -1;
            const int i = ix[corner + size_t(in.offset)];
            if (i < 0 || size_t(i) >= in.src->count) fail("geometry '" + g.name + "': index " + std::to_string(i) + " out of range");
            return i;
        };
        auto vertexOf = [&](size_t corner) -> uint32_t {
            if (corner + stride > ix.size()) fail("geometry '" + g.name + "': truncated index list");
            const std::array<int, 4> key{value(pos, corner), value(nrm, corner), value(uv, corner), value(col, corner)};
            auto [it, inserted] = dedup.emplace(key, uint32_t(prim.verts.size()));
            if (inserted) {
                RawVertex rv;
                rv.pos = key[0];
                ModelVertex& v = rv.v;
                v.pos = {pos.src->at(size_t(key[0]), 0), pos.src->at(size_t(key[0]), 1), pos.src->at(size_t(key[0]), 2)};
                if (key[1] >= 0) v.nrm = {nrm.src->at(size_t(key[1]), 0), nrm.src->at(size_t(key[1]), 1), nrm.src->at(size_t(key[1]), 2)};
                if (key[2] >= 0) v.uv = {uv.src->at(size_t(key[2]), 0), 1.0f - uv.src->at(size_t(key[2]), 1)};
                if (key[3] >= 0) {
                    const Source& s = *col.src;
                    v.color = packColor(s.at(size_t(key[3]), 0), s.at(size_t(key[3]), 1), s.at(size_t(key[3]), 2), s.stride >= 4 ? s.at(size_t(key[3]), 3) : 1.0f);
                }
                prim.verts.push_back(rv);
            }
            return it->second;
        };
        for (const auto& poly : polys)
            for (size_t k = 2; k < poly.size(); ++k)
                prim.idx.insert(prim.idx.end(), {vertexOf(poly[0]), vertexOf(poly[k - 1]), vertexOf(poly[k])});
        if (prim.idx.empty()) return;
        if (!nrm.src) {
            // smooth per position: corners that share a position share the normal
            std::vector<float3> acc(pos.src->count);
            for (size_t i = 0; i + 2 < prim.idx.size(); i += 3) {
                const RawVertex &a = prim.verts[prim.idx[i]], &b = prim.verts[prim.idx[i + 1]], &c = prim.verts[prim.idx[i + 2]];
                const float3 n = cross(b.v.pos - a.v.pos, c.v.pos - a.v.pos);
                acc[size_t(a.pos)] += n; acc[size_t(b.pos)] += n; acc[size_t(c.pos)] += n;
            }
            for (auto& rv : prim.verts) rv.v.nrm = normalize(acc[size_t(rv.pos)]);
        } else for (auto& rv : prim.verts) rv.v.nrm = normalize(rv.v.nrm);
        g.prims.push_back(std::move(prim));
    }

    // ---- scene

    struct NodeInfo { std::string id, sid; std::vector<Xform> stack; };

    static mat4 xformMatrix(const Xform& x) {
        switch (x.kind) {
        case Xform::Matrix: return rowMajor(x.v);
        case Xform::Translate: return mat4::translate({x.v[0], x.v[1], x.v[2]});
        case Xform::Scale: return mat4::scale({x.v[0], x.v[1], x.v[2]});
        case Xform::Rotate: {
            const float3 axis{x.v[0], x.v[1], x.v[2]};
            if (dot(axis, axis) < 1e-12f) return {};
            return mat4::rotate(quat::axisAngle(axis, x.v[3] * kPi / 180.0f));
        }
        case Xform::LookAt: {   // eye, interest, up: the node sits at the eye looking at the interest
            const float3 eye{x.v[0], x.v[1], x.v[2]}, at{x.v[3], x.v[4], x.v[5]}, up{x.v[6], x.v[7], x.v[8]};
            return mat4::lookAt(eye, at, up).inverse();
        }
        }
        return {};
    }
    static mat4 stackMatrix(const std::vector<Xform>& st) {
        mat4 m;
        for (const Xform& x : st) m = m * xformMatrix(x);
        return m;
    }
    void setLocal(Node& n, const mat4& m) {
        if (!decompose(m, n.t, n.r, n.s)) warn("node '" + n.name + "': sheared transform approximated by translate / rotate / scale");
    }

    void readScene() {
        const Element* vs = nullptr;
        if (const Element* sc = doc_.child("scene"))
            if (const Element* ivs = sc->child("instance_visual_scene")) vs = byId(ivs->attr("url"));
        if (!vs)
            if (const Element* lib = doc_.child("library_visual_scenes")) vs = lib->child("visual_scene");
        if (!vs) fail("no visual scene");
        for (const Element& n : vs->children)
            if (n.name == "node") node(n, rootNode_, 0);
        for (auto& p : pending_) instance(p);
    }

    struct Pending { int node; const Element* inst; };

    void node(const Element& e, int parent, int depth) {
        if (depth > 128) fail("node hierarchy too deep (instance_node cycle?)");
        Model& m = *model_;
        const int idx = int(m.nodes.size());
        Node n;
        n.name = e.attr("name", e.attr("id", e.attr("sid", "node" + std::to_string(idx))));
        n.parent = parent;
        NodeInfo info;
        info.id = e.attr("id");
        info.sid = e.attr("sid");
        for (const Element& c : e.children) {
            Xform x;
            x.sid = c.attr("sid");
            int need = 0;
            if (c.name == "matrix") { x.kind = Xform::Matrix; need = 16; }
            else if (c.name == "translate") { x.kind = Xform::Translate; need = 3; }
            else if (c.name == "rotate") { x.kind = Xform::Rotate; need = 4; }
            else if (c.name == "scale") { x.kind = Xform::Scale; need = 3; }
            else if (c.name == "lookat") { x.kind = Xform::LookAt; need = 9; }
            else if (c.name == "skew") { warn("node '" + n.name + "': <skew> is ignored"); continue; }
            else continue;
            std::vector<float> v;
            parseNumbers(c.text, v);
            if (int(v.size()) < need) fail("node '" + n.name + "': <" + c.name + "> needs " + std::to_string(need) + " numbers");
            std::copy(v.begin(), v.begin() + need, x.v);
            x.n = need;
            info.stack.push_back(x);
        }
        setLocal(n, stackMatrix(info.stack));
        m.nodes.push_back(n);
        info_.push_back(std::move(info));
        if (!info_.back().id.empty()) idToNode_.emplace(info_.back().id, idx);
        for (const Element& c : e.children) {
            if (c.name == "node") node(c, idx, depth + 1);
            else if (c.name == "instance_node") {
                const Element* t = byId(c.attr("url"));
                if (t && t->name == "node") node(*t, idx, depth + 1);
                else warn("instance_node '" + c.attr("url") + "' not found");
            } else if (c.name == "instance_geometry" || c.name == "instance_controller") pending_.push_back({idx, &c});
        }
    }

    // symbol -> material from <bind_material>
    std::map<std::string, std::string> bindings(const Element& inst) {
        std::map<std::string, std::string> b;
        if (const Element* bm = inst.child("bind_material"))
            if (const Element* tc = bm->child("technique_common"))
                tc->each("instance_material", [&](const Element& im) { b[im.attr("symbol")] = urlId(im.attr("target")); });
        return b;
    }
    int primMaterial(const std::map<std::string, std::string>& b, const std::string& symbol) {
        auto it = b.find(symbol);
        if (it != b.end()) return material(it->second);
        if (!symbol.empty() && byId(symbol) && byId(symbol)->name == "material") return material(symbol);
        if (!symbol.empty()) warn("material symbol '" + symbol + "' is not bound; default material used");
        return defaultMaterial();
    }

    // attaches a mesh to the instancing node (or a child when it already has one)
    void attach(int node, int mesh, int skin) {
        Model& m = *model_;
        int target = node;
        if (m.nodes[size_t(node)].mesh >= 0) {
            Node c;
            c.name = m.nodes[size_t(node)].name + "#" + std::to_string(m.meshes.size());
            c.parent = node;
            target = int(m.nodes.size());
            m.nodes.push_back(c);
            info_.push_back({});
        }
        m.nodes[size_t(target)].mesh = mesh;
        m.nodes[size_t(target)].skin = skin;
    }

    void instance(const Pending& p) {
        const Element& inst = *p.inst;
        const auto b = bindings(inst);
        if (inst.name == "instance_geometry") {
            const Geometry& g = geometry(inst.attr("url"));
            if (g.prims.empty()) return;
            std::string key = urlId(inst.attr("url"));
            for (auto& [s, t] : b) key += "|" + s + "=" + t;
            auto it = meshCache_.find(key);
            if (it == meshCache_.end()) it = meshCache_.emplace(key, staticMesh(g, b)).first;
            attach(p.node, it->second, -1);
            return;
        }
        // instance_controller
        const Element* ctrl = byId(inst.attr("url"));
        if (!ctrl || ctrl->name != "controller") { warn("controller '" + inst.attr("url") + "' not found"); return; }
        const Element* skin = ctrl->child("skin");
        if (!skin) {
            if (const Element* morph = ctrl->child("morph")) {
                warn("morph controller '" + ctrl->attr("id") + "': morph targets ignored, base mesh used");
                const Geometry& g = geometry(morph->attr("source"));
                if (!g.prims.empty()) attach(p.node, staticMesh(g, b), -1);
            }
            return;
        }
        std::string geoUrl = skin->attr("source");
        if (const Element* src = byId(geoUrl); src && src->name == "controller") {
            warn("controller '" + ctrl->attr("id") + "': morph under the skin ignored, base mesh used");
            if (const Element* morph = src->child("morph")) geoUrl = morph->attr("source");
        }
        const Geometry& g = geometry(geoUrl);
        if (g.prims.empty()) return;
        std::vector<int> roots;
        inst.each("skeleton", [&](const Element& s) {
            auto it = idToNode_.find(urlId(trimmed(s.text)));
            if (it != idToNode_.end()) roots.push_back(it->second);
            else warn("skeleton '" + trimmed(s.text) + "' not found");
        });
        skinnedMesh(p.node, *ctrl, *skin, g, b, roots);
    }

    int staticMesh(const Geometry& g, const std::map<std::string, std::string>& b) {
        Model& m = *model_;
        MeshDef md;
        md.name = g.name;
        for (const GeoPrim& gp : g.prims) {
            Primitive prim;
            prim.material = primMaterial(b, gp.symbol);
            prim.firstIndex = uint32_t(m.indices.size());
            prim.firstVertex = uint32_t(m.vertices.size());
            for (const RawVertex& rv : gp.verts) m.vertices.push_back(rv.v);
            for (uint32_t i : gp.idx) m.indices.push_back(prim.firstVertex + i);
            prim.indexCount = uint32_t(gp.idx.size());
            prim.vertexCount = uint32_t(gp.verts.size());
            if (m.materials[size_t(prim.material)].normalTex >= 0) generateTangents(m.vertices, m.indices, prim.firstIndex, prim.indexCount);
            md.primitives.push_back(prim);
        }
        m.meshes.push_back(std::move(md));
        return int(m.meshes.size()) - 1;
    }

    // joint name -> node: sid below the skeleton roots, then sid / id / name anywhere
    int findJoint(const std::string& name, const std::vector<int>& roots, bool idref) {
        const Model& m = *model_;
        if (idref) { auto it = idToNode_.find(name); return it == idToNode_.end() ? -1 : it->second; }
        auto below = [&](int n, int root) {
            for (int k = n, guard = 0; k >= 0 && guard < 1024; k = m.nodes[size_t(k)].parent, ++guard) if (k == root) return true;
            return false;
        };
        for (int r : roots)
            for (size_t i = 0; i < info_.size(); ++i)
                if (info_[i].sid == name && below(int(i), r)) return int(i);
        for (size_t i = 0; i < info_.size(); ++i) if (info_[i].sid == name) return int(i);
        if (auto it = idToNode_.find(name); it != idToNode_.end()) return it->second;
        for (size_t i = 0; i < m.nodes.size(); ++i) if (m.nodes[i].name == name) return int(i);
        return -1;
    }

    void skinnedMesh(int node, const Element& ctrl, const Element& skin, const Geometry& g, const std::map<std::string, std::string>& b, const std::vector<int>& roots) {
        Model& m = *model_;
        const std::string cname = ctrl.attr("name", ctrl.attr("id"));
        mat4 bindShape;
        if (const Element* bs = skin.child("bind_shape_matrix")) {
            std::vector<float> v;
            parseNumbers(bs->text, v);
            if (v.size() >= 16) bindShape = rowMajor(v.data());
        }
        // joints + inverse bind matrices
        const Element* je = skin.child("joints");
        if (!je) fail("skin '" + cname + "' without <joints>");
        const Source* jointSrc = nullptr;
        const Source* ibmSrc = nullptr;
        je->each("input", [&](const Element& in) {
            if (in.attr("semantic") == "JOINT") jointSrc = &source(in.attr("source"));
            else if (in.attr("semantic") == "INV_BIND_MATRIX") ibmSrc = &source(in.attr("source"));
        });
        if (!jointSrc || jointSrc->names.empty()) fail("skin '" + cname + "' without joint names");
        bool idref = false;   // IDREF_array: node ids instead of sids / names
        je->each("input", [&](const Element& in) {
            if (in.attr("semantic") != "JOINT") return;
            if (const Element* js = byId(in.attr("source"))) idref = js->child("IDREF_array") != nullptr;
        });
        Skin sk;
        sk.name = cname;
        for (size_t j = 0; j < jointSrc->names.size(); ++j) {
            int n = findJoint(jointSrc->names[j], roots, idref);
            if (n < 0) { warn("skin '" + cname + "': joint '" + jointSrc->names[j] + "' not found; bound to the skin's node"); n = node; }
            sk.joints.push_back(n);
            mat4 ibm;
            if (ibmSrc && j < ibmSrc->count && ibmSrc->stride == 16) ibm = rowMajor(&ibmSrc->f[j * 16]);
            else if (j == 0 && !ibmSrc) warn("skin '" + cname + "': no INV_BIND_MATRIX, identity used");
            sk.inverseBind.push_back(ibm);
        }
        // per-position influences
        const Element* vw = skin.child("vertex_weights");
        if (!vw) fail("skin '" + cname + "' without <vertex_weights>");
        int jOff = -1, wOff = -1, maxOff = 0;
        const Source* wSrc = nullptr;
        vw->each("input", [&](const Element& in) {
            const int off = int(std::strtol(in.attr("offset", "0").c_str(), nullptr, 10));
            maxOff = std::max(maxOff, off);
            if (in.attr("semantic") == "JOINT") jOff = off;
            else if (in.attr("semantic") == "WEIGHT") { wOff = off; wSrc = &source(in.attr("source")); }
        });
        if (jOff < 0 || wOff < 0 || !wSrc || maxOff > 16) fail("skin '" + cname + "': vertex_weights without JOINT / WEIGHT inputs");
        std::vector<int> vcount, v;
        if (const Element* e = vw->child("vcount")) parseNumbers(e->text, vcount);
        if (const Element* e = vw->child("v")) parseNumbers(e->text, v);
        struct Influence { int joint; float w; };
        std::vector<std::array<Influence, 4>> infl(g.positions, std::array<Influence, 4>{{{0, 0}, {0, 0}, {0, 0}, {0, 0}}});
        const size_t stride = size_t(maxOff) + 1;
        size_t at = 0;
        bool many = false, badJoint = false;
        for (size_t pi = 0; pi < vcount.size(); ++pi) {
            const int n = vcount[pi];
            if (n < 0 || at + size_t(n) * stride > v.size()) fail("skin '" + cname + "': <v> shorter than <vcount> needs");
            std::vector<Influence> list;
            for (int k = 0; k < n; ++k, at += stride) {
                const int j = v[at + size_t(jOff)], wi = v[at + size_t(wOff)];
                if (wi < 0 || size_t(wi) >= wSrc->count) fail("skin '" + cname + "': weight index out of range");
                if (j < 0 || size_t(j) >= sk.joints.size()) { badJoint = true; continue; }   // -1 = bind shape
                const float w = wSrc->at(size_t(wi), 0);
                if (w > 0) list.push_back({j, w});
            }
            if (pi >= infl.size()) continue;
            std::sort(list.begin(), list.end(), [](const Influence& a, const Influence& c) { return a.w > c.w || (a.w == c.w && a.joint < c.joint); });
            if (list.size() > 4) many = true;
            float sum = 0;
            for (size_t k = 0; k < list.size() && k < 4; ++k) sum += list[k].w;
            for (size_t k = 0; k < list.size() && k < 4; ++k) infl[pi][k] = {list[k].joint, list[k].w / sum};
        }
        if (many) warn("skin '" + cname + "': more than 4 influences per vertex; the 4 strongest are kept");
        if (badJoint) warn("skin '" + cname + "': influences on the bind shape (joint -1) ignored");
        if (vcount.size() < g.positions) warn("skin '" + cname + "': fewer weights than vertices; the rest follow the first joint");
        const int skinIndex = int(m.skins.size());
        m.skins.push_back(std::move(sk));
        // mesh: bind shape baked in, palettes of at most kMaxPaletteJoints joints
        const mat4 bsn = bindShape.normalMatrix();
        MeshDef md;
        md.name = g.name;
        for (const GeoPrim& gp : g.prims) {
            Primitive prim;
            prim.material = primMaterial(b, gp.symbol);
            const bool tangents = m.materials[size_t(prim.material)].normalTex >= 0;
            auto jointsOf = [&](uint32_t vi, std::set<int>& s) {
                const auto& in = infl[size_t(gp.verts[vi].pos)];
                s.insert(in[0].joint);
                for (int k = 1; k < 4; ++k) if (in[size_t(k)].w > 0) s.insert(in[size_t(k)].joint);
            };
            std::vector<uint32_t> chunk;
            std::set<int> used;
            auto flush = [&]() {
                if (chunk.empty()) return;
                Primitive pr = prim;
                std::map<int, int> palette;
                std::vector<int32_t> remap(gp.verts.size(), -1);
                pr.firstIndex = uint32_t(m.indices.size());
                pr.firstVertex = uint32_t(m.vertices.size());
                for (uint32_t i : chunk) {
                    if (remap[i] < 0) {
                        remap[i] = int32_t(m.vertices.size() - pr.firstVertex);
                        const RawVertex& rv = gp.verts[i];
                        ModelVertex mv = rv.v;
                        mv.pos = bindShape.transformPoint(mv.pos);
                        mv.nrm = normalize(bsn.transformDir(mv.nrm));
                        const auto& in = infl[size_t(rv.pos)];
                        int total = 0;
                        for (int k = 0; k < 4; ++k) {
                            const bool use = k == 0 || in[size_t(k)].w > 0;
                            if (!use) { mv.joints[k] = 0; mv.weights[k] = 0; continue; }
                            auto it = palette.emplace(in[size_t(k)].joint, int(palette.size())).first;
                            mv.joints[k] = uint8_t(it->second);
                            const float w = k == 0 && in[0].w <= 0 ? 1.0f : in[size_t(k)].w;
                            mv.weights[k] = uint8_t(std::lround(std::clamp(w, 0.0f, 1.0f) * 255.0f));
                            total += mv.weights[k];
                        }
                        mv.weights[0] = uint8_t(std::clamp(int(mv.weights[0]) + 255 - total, 0, 255));   // exact sum 255
                        m.vertices.push_back(mv);
                    }
                    m.indices.push_back(pr.firstVertex + uint32_t(remap[i]));
                }
                pr.indexCount = uint32_t(m.indices.size()) - pr.firstIndex;
                pr.vertexCount = uint32_t(m.vertices.size()) - pr.firstVertex;
                pr.jointPalette.resize(palette.size());
                for (auto& [j, slot] : palette) pr.jointPalette[size_t(slot)] = uint16_t(j);
                if (tangents) generateTangents(m.vertices, m.indices, pr.firstIndex, pr.indexCount);
                md.primitives.push_back(std::move(pr));
                chunk.clear();
                used.clear();
            };
            for (size_t t = 0; t + 2 < gp.idx.size(); t += 3) {
                std::set<int> merged = used;
                for (int k = 0; k < 3; ++k) jointsOf(gp.idx[t + size_t(k)], merged);
                if (int(merged.size()) > kMaxPaletteJoints) {
                    flush();
                    merged.clear();
                    for (int k = 0; k < 3; ++k) jointsOf(gp.idx[t + size_t(k)], merged);
                }
                used = std::move(merged);
                chunk.insert(chunk.end(), {gp.idx[t], gp.idx[t + 1], gp.idx[t + 2]});
            }
            flush();
        }
        m.meshes.push_back(std::move(md));
        attach(node, int(m.meshes.size()) - 1, skinIndex);
    }

    // ---- animation

    void readAnimations() {
        const Element* lib = doc_.child("library_animations");
        if (!lib) return;
        std::map<const Element*, std::vector<int>> tracksOf;   // <animation> -> tracks (with nested)
        std::function<std::vector<int>(const Element&, int)> anim = [&](const Element& a, int depth) {
            std::vector<int> mine;
            if (depth > 32) return mine;
            a.each("channel", [&](const Element& ch) {
                const int t = channel(ch);
                if (t >= 0) mine.push_back(t);
            });
            a.each("animation", [&](const Element& sub) {
                auto s = anim(sub, depth + 1);
                mine.insert(mine.end(), s.begin(), s.end());
            });
            tracksOf[&a] = mine;
            return mine;
        };
        lib->each("animation", [&](const Element& a) { anim(a, 0); });
        if (tracks_.empty()) return;
        bool named = false;
        if (const Element* clips = doc_.child("library_animation_clips"))
            clips->each("animation_clip", [&](const Element& c) {
                std::vector<int> ts;
                c.each("instance_animation", [&](const Element& ia) {
                    const Element* a = byId(ia.attr("url"));
                    auto it = a ? tracksOf.find(a) : tracksOf.end();
                    if (it != tracksOf.end()) ts.insert(ts.end(), it->second.begin(), it->second.end());
                    else warn("animation clip '" + c.attr("name", c.attr("id")) + "': animation '" + ia.attr("url") + "' not found");
                });
                const float start = std::strtof(c.attr("start", "0").c_str(), nullptr);
                const std::string endAttr = c.attr("end");
                const float end = endAttr.empty() ? -1.0f : std::strtof(endAttr.c_str(), nullptr);
                bake(c.attr("name", c.attr("id", "clip")), ts, std::isfinite(start) ? start : 0.0f, std::isfinite(end) ? end : -1.0f);
                named = true;
            });
        if (!named) {
            std::vector<int> all(tracks_.size());
            for (size_t i = 0; i < all.size(); ++i) all[i] = int(i);
            bake("Default", all, 0, -1);
        }
    }

    int sampler(const std::string& url) {
        const std::string id = urlId(url);
        if (auto it = samplerIds_.find(id); it != samplerIds_.end()) return it->second;
        int& slot = samplerIds_[id];
        slot = -1;
        const Element* se = byId(id);
        if (!se || se->name != "sampler") { warn("animation sampler '" + id + "' not found"); return -1; }
        Sampler s;
        const Source *in = nullptr, *out = nullptr, *interp = nullptr, *inTan = nullptr, *outTan = nullptr;
        se->each("input", [&](const Element& i) {
            const std::string sem = i.attr("semantic");
            if (sem == "INPUT") in = &source(i.attr("source"));
            else if (sem == "OUTPUT") out = &source(i.attr("source"));
            else if (sem == "INTERPOLATION") interp = &source(i.attr("source"));
            else if (sem == "IN_TANGENT") inTan = &source(i.attr("source"));
            else if (sem == "OUT_TANGENT") outTan = &source(i.attr("source"));
        });
        if (!in || !out || in->count == 0 || out->count < in->count) { warn("animation sampler '" + id + "': INPUT / OUTPUT missing or too short"); return -1; }
        s.in.assign(in->f.begin(), in->f.begin() + std::ptrdiff_t(in->count));
        for (size_t i = 1; i < s.in.size(); ++i) if (s.in[i] < s.in[i - 1]) { warn("animation sampler '" + id + "': key times not sorted"); return -1; }
        s.stride = out->stride;
        s.out.assign(out->f.begin(), out->f.begin() + std::ptrdiff_t(in->count * size_t(out->stride)));
        s.interp.assign(s.in.size(), Sampler::Linear);
        // tangents: (time, value) pairs per component, or values only (times at thirds)
        const bool tangents = inTan && outTan && inTan->count >= in->count && outTan->count >= in->count && inTan->stride == outTan->stride &&
                              (inTan->stride == out->stride * 2 || inTan->stride == out->stride);
        if (tangents) {
            s.tanStride = inTan->stride;
            s.inTan.assign(inTan->f.begin(), inTan->f.begin() + std::ptrdiff_t(in->count * size_t(s.tanStride)));
            s.outTan.assign(outTan->f.begin(), outTan->f.begin() + std::ptrdiff_t(in->count * size_t(s.tanStride)));
        }
        if (interp && !interp->names.empty()) {
            bool allStep = true;
            for (size_t k = 0; k < s.interp.size(); ++k) {
                const std::string& n = interp->names[std::min(k, interp->names.size() - 1)];
                if (n == "STEP") s.interp[k] = Sampler::Step;
                else if (n == "BEZIER" && tangents) { s.interp[k] = Sampler::Bezier; s.bezier = true; }
                else if (n != "LINEAR") warn("animation: " + n + " interpolation (without usable tangents) is treated as linear");
                allStep = allStep && s.interp[k] == Sampler::Step;
            }
            s.step = allStep;
        }
        slot = int(samplers_.size());
        samplers_.push_back(std::move(s));
        return slot;
    }

    int channel(const Element& ch) {
        std::string target = ch.attr("target");
        if (target.rfind("./", 0) == 0) target = target.substr(2);
        const size_t slash = target.find('/');
        if (slash == std::string::npos) { warn("animation target '" + target + "' is not a node transform; ignored"); return -1; }
        const std::string nodeId = target.substr(0, slash);
        std::string rest = target.substr(slash + 1);
        auto nit = idToNode_.find(nodeId);
        if (nit == idToNode_.end()) { warn("animation target '" + target + "': node not in the scene; ignored"); return -1; }
        // sid then ".member" or "(i)(j)"
        size_t cut = rest.find_first_of(".(");
        const std::string sid = rest.substr(0, cut);
        const std::string member = cut == std::string::npos ? std::string() : rest.substr(cut);
        const NodeInfo& info = info_[size_t(nit->second)];
        int xi = -1;
        for (size_t i = 0; i < info.stack.size(); ++i) if (info.stack[i].sid == sid) { xi = int(i); break; }
        if (xi < 0) { warn("animation target '" + target + "' is not a node transform; ignored"); return -1; }
        const Xform& x = info.stack[size_t(xi)];
        Track t;
        t.node = nit->second;
        t.xform = xi;
        t.first = 0;
        t.comps = x.n;
        if (!member.empty()) {
            if (member[0] == '.') {
                const std::string mm = member.substr(1);
                static const std::map<std::string, int> kIdx = {{"X", 0}, {"Y", 1}, {"Z", 2}, {"ANGLE", 3}, {"W", 3}};
                auto it = kIdx.find(mm);
                if (it == kIdx.end() || it->second >= x.n) { warn("animation target '" + target + "': unknown member; ignored"); return -1; }
                t.first = it->second;
                t.comps = 1;
            } else {
                std::vector<int> ids;
                for (size_t p = 0; p < member.size();) {
                    if (member[p] != '(') break;
                    size_t q = member.find(')', p);
                    if (q == std::string::npos) break;
                    ids.push_back(std::atoi(member.substr(p + 1, q - p - 1).c_str()));
                    p = q + 1;
                }
                const int k = ids.size() == 2 ? ids[0] * 4 + ids[1] : ids.size() == 1 ? ids[0] : -1;
                if (k < 0 || k >= x.n) { warn("animation target '" + target + "': bad index; ignored"); return -1; }
                t.first = k;
                t.comps = 1;
            }
        }
        t.sampler = sampler(ch.attr("source"));
        if (t.sampler < 0) return -1;
        if (samplers_[size_t(t.sampler)].stride < t.comps) { warn("animation target '" + target + "': output has " + std::to_string(samplers_[size_t(t.sampler)].stride) + " values per key, " + std::to_string(t.comps) + " needed; ignored"); return -1; }
        tracks_.push_back(t);
        return int(tracks_.size()) - 1;
    }

    static void evalSampler(const Sampler& s, float t, int comps, float* out, int outFirst) {
        const size_t n = s.in.size();
        size_t i = 0;
        float f = 0;
        if (t <= s.in.front()) i = 0;
        else if (t >= s.in.back()) i = n - 1;
        else {
            i = size_t(std::upper_bound(s.in.begin(), s.in.end(), t) - s.in.begin()) - 1;
            const float d = s.in[i + 1] - s.in[i];
            f = s.interp[i] == Sampler::Step || d <= 0 ? 0 : (t - s.in[i]) / d;
        }
        for (int c = 0; c < comps; ++c) {
            const float a = s.out[i * size_t(s.stride) + size_t(c)];
            const float b = i + 1 < n ? s.out[(i + 1) * size_t(s.stride) + size_t(c)] : a;
            if (f > 0 && s.interp[i] == Sampler::Bezier) {
                // cubic Bezier through (t0, a) (c0) (c1) (t1, b); solve x(u) = t by bisection
                const bool pairs = s.tanStride == s.stride * 2;
                const float t0 = s.in[i], t1 = s.in[i + 1];
                const size_t o0 = i * size_t(s.tanStride), o1 = (i + 1) * size_t(s.tanStride);
                const float cx0 = pairs ? s.outTan[o0 + size_t(c) * 2] : t0 + (t1 - t0) / 3;
                const float cy0 = pairs ? s.outTan[o0 + size_t(c) * 2 + 1] : s.outTan[o0 + size_t(c)];
                const float cx1 = pairs ? s.inTan[o1 + size_t(c) * 2] : t1 - (t1 - t0) / 3;
                const float cy1 = pairs ? s.inTan[o1 + size_t(c) * 2 + 1] : s.inTan[o1 + size_t(c)];
                auto bez = [](float p0, float p1, float p2, float p3, float u) {
                    const float v = 1 - u;
                    return v * v * v * p0 + 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u * p3;
                };
                float lo = 0, hi = 1, u = f;
                for (int it = 0; it < 24; ++it) {
                    u = (lo + hi) * 0.5f;
                    (bez(t0, cx0, cx1, t1, u) < t ? lo : hi) = u;
                }
                out[outFirst + c] = bez(a, cy0, cy1, b, u);
                continue;
            }
            out[outFirst + c] = a + (b - a) * f;
        }
    }

    void bake(const std::string& name, const std::vector<int>& trackIds, float start, float end) {
        Model& m = *model_;
        std::map<int, std::vector<int>> byNode;
        for (int t : trackIds) byNode[tracks_[size_t(t)].node].push_back(t);
        AnimClip clip;
        clip.name = name;
        float maxT = 0;
        for (auto& [node, ts] : byNode) {
            std::vector<float> times;
            bool allStep = true;
            for (int t : ts) {
                const Sampler& s = samplers_[size_t(tracks_[size_t(t)].sampler)];
                allStep = allStep && s.step;
                for (float k : s.in) if (k >= start - 1e-5f && (end < 0 || k <= end + 1e-5f)) times.push_back(k);
            }
            if (end >= 0) { times.push_back(start); times.push_back(end); }
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end(), [](float a, float b) { return std::fabs(a - b) < 1e-5f; }), times.end());
            if (times.empty()) continue;
            // rotate angles may turn more than half a turn between keys (full spins): sample in between
            std::vector<float> fine;
            for (size_t i = 0; i < times.size(); ++i) {
                if (i > 0) {
                    float turn = 0;
                    for (int t : ts) {
                        const Track& tr = tracks_[size_t(t)];
                        const Xform& x = info_[size_t(node)].stack[size_t(tr.xform)];
                        if (x.kind != Xform::Rotate || tr.first > 3 || tr.first + tr.comps <= 3) continue;
                        float a[4] = {}, b[4] = {};
                        evalSampler(samplers_[size_t(tr.sampler)], times[i - 1], tr.comps, a, 0);
                        evalSampler(samplers_[size_t(tr.sampler)], times[i], tr.comps, b, 0);
                        turn = std::max(turn, std::fabs(b[3 - tr.first] - a[3 - tr.first]));
                    }
                    int steps = std::min(256, int(std::ceil(turn / 30.0f)));
                    for (int t : ts) {   // curves: a few samples per Bezier segment
                        const Sampler& sm = samplers_[size_t(tracks_[size_t(t)].sampler)];
                        if (!sm.bezier) continue;
                        const float mid = (times[i - 1] + times[i]) * 0.5f;
                        const size_t k = size_t(std::upper_bound(sm.in.begin(), sm.in.end(), mid) - sm.in.begin());
                        if (k > 0 && k < sm.in.size() && sm.interp[k - 1] == Sampler::Bezier) steps = std::max(steps, 4);
                    }
                    for (int k = 1; k < steps; ++k) fine.push_back(times[i - 1] + (times[i] - times[i - 1]) * float(k) / float(steps));
                }
                fine.push_back(times[i]);
            }
            times.swap(fine);
            const Node& rest = m.nodes[size_t(node)];
            AnimChannel ct, cr, cs;
            ct.node = cr.node = cs.node = node;
            ct.path = AnimChannel::Translation; cr.path = AnimChannel::Rotation; cs.path = AnimChannel::Scale;
            cr.comps = 4;
            ct.interp = cr.interp = cs.interp = allStep ? AnimChannel::Step : AnimChannel::Linear;
            bool movesT = false, movesR = false, movesS = false, sheared = false;
            quat prev = rest.r;
            for (float tm : times) {
                std::vector<Xform> st = info_[size_t(node)].stack;
                for (int t : ts) {
                    const Track& tr = tracks_[size_t(t)];
                    evalSampler(samplers_[size_t(tr.sampler)], tm, tr.comps, st[size_t(tr.xform)].v, tr.first);
                }
                float3 T, S;
                quat R;
                sheared |= !decompose(stackMatrix(st), T, R, S);
                if (dot(prev, R) < 0) R = {-R.x, -R.y, -R.z, -R.w};
                prev = R;
                const float k = std::max(0.0f, tm - start);
                for (AnimChannel* c : {&ct, &cr, &cs}) c->times.push_back(k);
                ct.values.insert(ct.values.end(), {T.x, T.y, T.z});
                cr.values.insert(cr.values.end(), {R.x, R.y, R.z, R.w});
                cs.values.insert(cs.values.end(), {S.x, S.y, S.z});
                movesT |= length(T - rest.t) > 1e-5f * std::max(1.0f, length(rest.t));
                movesR |= std::fabs(dot(R, rest.r)) < 1 - 1e-6f;
                movesS |= length(S - rest.s) > 1e-5f;
                maxT = std::max(maxT, k);
            }
            if (sheared) warn("node '" + rest.name + "': sheared animated transform approximated");
            if (movesT) clip.channels.push_back(std::move(ct));
            if (movesR) clip.channels.push_back(std::move(cr));
            if (movesS) clip.channels.push_back(std::move(cs));
        }
        clip.duration = end >= 0 ? std::max(0.0f, end - start) : maxT;
        if (clip.channels.empty()) { warn("animation clip '" + name + "' does not move anything; skipped"); return; }
        m.clips.push_back(std::move(clip));
    }

    std::string path_;
    std::vector<std::string>* warnings_;
    std::set<std::string> warned_;
    std::unique_ptr<Model> model_;
    TextureLoader tex_;
    Element doc_;
    std::unordered_map<std::string, const Element*> ids_;
    std::map<std::string, Source> sources_;
    std::map<std::string, Geometry> geometries_;
    std::map<std::string, int> materials_, meshCache_, samplerIds_;
    std::map<std::string, int> idToNode_;
    std::vector<NodeInfo> info_;        // per engine node (parallel to Model::nodes once the scene is read)
    std::vector<Pending> pending_;
    std::vector<Sampler> samplers_;
    std::vector<Track> tracks_;
    int defaultMat_ = -1, rootNode_ = -1;
};

} // namespace

std::unique_ptr<Model> loadDae(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings) {
    DaeReader r(path, read, warnings);
    return r.load(bytes);
}

} // namespace rts::render
