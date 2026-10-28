// glTF 2.0 loader (.gltf with external / data-URI buffers, .glb).
//
// Supported: meshes (triangles, strips, fans; indexed or not), all accessor component types incl.
// normalized and sparse accessors (KHR_mesh_quantization), node TRS / matrix, scenes, skins,
// TRS animations (step / linear / cubic spline), PBR metallic-roughness materials, normal /
// occlusion / emissive maps, alpha modes, double sided, KHR_texture_transform,
// KHR_materials_unlit, KHR_materials_emissive_strength, KHR_materials_pbrSpecularGlossiness
// (converted), KHR_materials_variants, morph targets (position / normal deltas, up to 8 per primitive,
// animated weights). Not supported (warning, skipped): TEXCOORD_1,
// cameras, lights. Compressed geometry / textures (Draco, meshopt, KTX2, WebP) are an error when
// required and skipped otherwise.
#include "Model.h"
#include "../data/Json.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace rts::render {

namespace {

double num(const Json& j, double def) { return j.isNumber() ? j.toDouble(def) : def; }
int inum(const Json& j, int def = -1) { return j.isNumber() ? int(j.toDouble(def)) : def; }

std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            o += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

bool base64(const std::string& in, size_t start, std::string& out) {
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        std::memset(table, -1, sizeof table);
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[uint8_t(a[i])] = int8_t(i);
        table[uint8_t('-')] = 62; table[uint8_t('_')] = 63;
        init = true;
    }
    uint32_t acc = 0; int bits = 0;
    out.clear();
    out.reserve((in.size() - start) * 3 / 4);
    for (size_t i = start; i < in.size(); ++i) {
        const uint8_t c = uint8_t(in[i]);
        if (c == '=') break;
        if (c == '\n' || c == '\r' || c == ' ') continue;
        if (table[c] < 0) return false;
        acc = (acc << 6) | uint32_t(table[c]);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += char((acc >> bits) & 0xff); }
    }
    return true;
}

// Decompose a column-major TRS matrix (glTF guarantees no shear / projection).
void decompose(const mat4& m, float3& t, quat& r, float3& s) {
    t = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    float3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    s = {length(c0), length(c1), length(c2)};
    if (dot(cross(c0, c1), c2) < 0) s.x = -s.x;
    if (s.x != 0) c0 = c0 * (1 / s.x);
    if (s.y != 0) c1 = c1 * (1 / s.y);
    if (s.z != 0) c2 = c2 * (1 / s.z);
    const float tr = c0.x + c1.y + c2.z;
    if (tr > 0) {
        float k = std::sqrt(tr + 1) * 2;
        r = {(c1.z - c2.y) / k, (c2.x - c0.z) / k, (c0.y - c1.x) / k, k / 4};
    } else if (c0.x > c1.y && c0.x > c2.z) {
        float k = std::sqrt(1 + c0.x - c1.y - c2.z) * 2;
        r = {k / 4, (c1.x + c0.y) / k, (c2.x + c0.z) / k, (c1.z - c2.y) / k};
    } else if (c1.y > c2.z) {
        float k = std::sqrt(1 + c1.y - c0.x - c2.z) * 2;
        r = {(c1.x + c0.y) / k, k / 4, (c2.y + c1.z) / k, (c2.x - c0.z) / k};
    } else {
        float k = std::sqrt(1 + c2.z - c0.x - c1.y) * 2;
        r = {(c2.x + c0.z) / k, (c2.y + c1.z) / k, k / 4, (c0.y - c1.x) / k};
    }
    r = qnormalize(r);
}

class GltfReader {
public:
    GltfReader(const std::string& path, const FileReader& read, std::vector<std::string>* warnings)
        : path_(path), dir_(dirOf(path)), read_(read), warnings_(warnings) {}

    std::unique_ptr<Model> load(const std::string& bytes) {
        std::string jsonText;
        if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0) parseGlb(bytes, jsonText);
        else jsonText = bytes;
        try { doc_ = Json::parse(jsonText); }
        catch (const std::exception& e) { fail(std::string("invalid JSON: ") + e.what()); }
        const Json& asset = doc_["asset"];
        if (asset.getStr("version", "2.0").substr(0, 1) != "2") fail("only glTF 2.x is supported (asset.version " + asset.getStr("version") + ")");
        checkExtensions();
        model_ = std::make_unique<Model>();
        loadBuffers();
        loadVariants();
        loadTextures();
        loadMaterials();
        loadNodes();
        loadSkins();
        loadMeshes();
        loadAnimations();
        model_->finalize();
        return std::move(model_);
    }

private:
    [[noreturn]] void fail(const std::string& msg) const { throw std::runtime_error(path_ + ": " + msg); }
    void warn(const std::string& msg) {
        if (warnings_ && warned_.insert(msg).second) warnings_->push_back(path_ + ": " + msg);
    }

    void parseGlb(const std::string& b, std::string& jsonText) {
        auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; };
        if (u32(4) != 2) fail("unsupported GLB version " + std::to_string(u32(4)));
        size_t total = std::min<size_t>(u32(8), b.size()), off = 12;
        while (off + 8 <= total) {
            uint32_t len = u32(off), type = u32(off + 4);
            if (off + 8 + len > total) fail("truncated GLB chunk");
            if (type == 0x4E4F534Au) jsonText.assign(b.data() + off + 8, len);
            else if (type == 0x004E4942u && glbBin_.empty()) glbBin_.assign(b.data() + off + 8, len);
            off += 8 + ((len + 3) & ~3u);
        }
        if (jsonText.empty()) fail("GLB has no JSON chunk");
    }

    void checkExtensions() {
        static const std::set<std::string> supported = {
            "KHR_mesh_quantization", "KHR_texture_transform", "KHR_materials_unlit", "KHR_materials_emissive_strength",
            "KHR_materials_pbrSpecularGlossiness", "KHR_materials_variants"};
        for (const auto& e : doc_["extensionsRequired"].items())
            if (!supported.count(e.str())) fail("required extension " + e.str() + " is not supported (re-export without it)");
        for (const auto& e : doc_["extensionsUsed"].items())
            if (!supported.count(e.str())) warn("extension " + e.str() + " is ignored");
    }

    void loadBuffers() {
        const auto& bufs = doc_["buffers"].items();
        for (size_t i = 0; i < bufs.size(); ++i) {
            const Json& b = bufs[i];
            std::string uri = b.getStr("uri");
            std::string data;
            if (uri.empty()) {
                if (glbBin_.empty()) fail("buffer " + std::to_string(i) + " has no uri and there is no GLB binary chunk");
                data = glbBin_;
            } else if (uri.rfind("data:", 0) == 0) {
                size_t comma = uri.find(',');
                if (comma == std::string::npos || uri.find(";base64") > comma || !base64(uri, comma + 1, data))
                    fail("buffer " + std::to_string(i) + ": unsupported data URI");
            } else if (!read_(dir_ + urlDecode(uri), data)) {
                fail("buffer file '" + dir_ + urlDecode(uri) + "' not found");
            }
            if (data.size() < size_t(num(b["byteLength"], 0))) fail("buffer " + std::to_string(i) + " is shorter than its byteLength");
            buffers_.push_back(std::move(data));
        }
    }

    // bytes of a buffer view: pointer, length, stride (0 = tightly packed)
    struct View { const uint8_t* p = nullptr; size_t len = 0; size_t stride = 0; };
    View view(int idx) {
        const Json& v = doc_["bufferViews"][size_t(idx)];
        if (!v.isObject()) fail("bufferView " + std::to_string(idx) + " out of range");
        int b = inum(v["buffer"]);
        if (b < 0 || b >= int(buffers_.size())) fail("bufferView " + std::to_string(idx) + ": bad buffer");
        size_t off = size_t(num(v["byteOffset"], 0)), len = size_t(num(v["byteLength"], 0));
        if (off + len > buffers_[size_t(b)].size()) fail("bufferView " + std::to_string(idx) + " out of range");
        return {reinterpret_cast<const uint8_t*>(buffers_[size_t(b)].data()) + off, len, size_t(num(v["byteStride"], 0))};
    }

    static int compCount(const std::string& t) {
        if (t == "SCALAR") return 1;
        if (t == "VEC2") return 2;
        if (t == "VEC3") return 3;
        if (t == "VEC4") return 4;
        if (t == "MAT2") return 4;
        if (t == "MAT3") return 9;
        if (t == "MAT4") return 16;
        return 0;
    }
    static int compSize(int ct) { return ct == 5120 || ct == 5121 ? 1 : ct == 5122 || ct == 5123 ? 2 : 4; }
    static double readComp(const uint8_t* p, int ct, bool norm) {
        switch (ct) {
            case 5120: { int8_t v; std::memcpy(&v, p, 1); return norm ? std::max(v / 127.0, -1.0) : v; }
            case 5121: return norm ? *p / 255.0 : *p;
            case 5122: { int16_t v; std::memcpy(&v, p, 2); return norm ? std::max(v / 32767.0, -1.0) : v; }
            case 5123: { uint16_t v; std::memcpy(&v, p, 2); return norm ? v / 65535.0 : v; }
            case 5125: { uint32_t v; std::memcpy(&v, p, 4); return v; }
            case 5126: { float v; std::memcpy(&v, p, 4); return v; }
        }
        return 0;
    }

    // Accessor as doubles (count x components); handles strides, normalization and sparse data.
    std::vector<double> accessor(int idx, int& comps, size_t& count) {
        const Json& a = doc_["accessors"][size_t(idx)];
        if (!a.isObject()) fail("accessor " + std::to_string(idx) + " out of range");
        comps = compCount(a.getStr("type"));
        const int ct = inum(a["componentType"], 0);
        const bool norm = a.getBool("normalized");
        count = size_t(num(a["count"], 0));
        if (comps == 0 || (ct < 5120 || ct > 5126)) fail("accessor " + std::to_string(idx) + ": bad type");
        std::vector<double> out(count * size_t(comps), 0.0);
        const int cs = compSize(ct);
        if (a.has("bufferView")) {
            View v = view(inum(a["bufferView"]));
            size_t off = size_t(num(a["byteOffset"], 0));
            size_t elem = size_t(comps * cs);
            // matrices of 1/2-byte components have padded columns
            if (comps == 4 && a.getStr("type") == "MAT2" && cs == 1) elem = 8;
            if (comps == 9 && cs <= 2) elem = cs == 1 ? 12 : 24;
            size_t stride = v.stride ? v.stride : elem;
            if (count && off + stride * (count - 1) + elem > v.len) fail("accessor " + std::to_string(idx) + " out of range of its bufferView");
            for (size_t i = 0; i < count; ++i)
                for (int c = 0; c < comps; ++c) {
                    size_t co = size_t(c) * size_t(cs);
                    if (comps == 9 && cs <= 2) co = size_t(c / 3) * (cs == 1 ? 4 : 8) + size_t(c % 3) * size_t(cs);
                    out[i * size_t(comps) + size_t(c)] = readComp(v.p + off + i * stride + co, ct, norm);
                }
        }
        if (const Json& sp = a["sparse"]; sp.isObject()) {
            size_t n = size_t(num(sp["count"], 0));
            const Json& ix = sp["indices"]; const Json& vx = sp["values"];
            View iv = view(inum(ix["bufferView"])), vv = view(inum(vx["bufferView"]));
            int ict = inum(ix["componentType"], 5125);
            size_t io = size_t(num(ix["byteOffset"], 0)), vo = size_t(num(vx["byteOffset"], 0));
            if (io + n * size_t(compSize(ict)) > iv.len || vo + n * size_t(comps * cs) > vv.len) fail("accessor " + std::to_string(idx) + ": sparse data out of range");
            for (size_t k = 0; k < n; ++k) {
                size_t target = size_t(readComp(iv.p + io + k * size_t(compSize(ict)), ict, false));
                if (target >= count) continue;
                for (int c = 0; c < comps; ++c)
                    out[target * size_t(comps) + size_t(c)] = readComp(vv.p + vo + (k * size_t(comps) + size_t(c)) * size_t(cs), ct, norm);
            }
        }
        return out;
    }
    std::vector<double> accessorExpect(int idx, int wantComps, size_t& count, const char* what) {
        int comps = 0;
        auto v = accessor(idx, comps, count);
        if (comps != wantComps) fail(std::string(what) + ": accessor " + std::to_string(idx) + " has " + std::to_string(comps) + " components, expected " + std::to_string(wantComps));
        return v;
    }

    void loadVariants() {
        for (const auto& v : doc_["extensions"]["KHR_materials_variants"]["variants"].items())
            model_->variants.push_back(v.getStr("name", "variant" + std::to_string(model_->variants.size())));
    }

    // One Model image per glTF texture (image + sampler). Decoded images are cached.
    void loadTextures() {
        const auto& texs = doc_["textures"].items();
        std::map<int, Image> decoded;
        for (size_t i = 0; i < texs.size(); ++i) {
            const Json& t = texs[i];
            int src = inum(t["source"]);
            if (src < 0) { warn("texture " + std::to_string(i) + " uses an unsupported image format (KTX2/WebP); ignored"); texMap_.push_back(-1); continue; }
            auto it = decoded.find(src);
            if (it == decoded.end()) {
                Image img;
                if (!decodeImageIndex(src, img)) { texMap_.push_back(-1); continue; }
                it = decoded.emplace(src, std::move(img)).first;
            }
            Image img = it->second;
            const Json& s = doc_["samplers"][size_t(std::max(0, inum(t["sampler"], -1)))];
            if (t.has("sampler") && s.isObject()) {
                img.nearest = inum(s["magFilter"], 9729) == 9728;
                img.repeat = inum(s["wrapS"], 10497) != 33071 || inum(s["wrapT"], 10497) != 33071;
            }
            img.name = it->second.name + "#t" + std::to_string(i);
            texMap_.push_back(int(model_->images.size()));
            model_->images.push_back(std::move(img));
        }
    }
    bool decodeImageIndex(int idx, Image& img) {
        const Json& im = doc_["images"][size_t(idx)];
        std::string bytes;
        std::string uri = im.getStr("uri");
        img.name = uri.empty() ? "#" + std::to_string(idx) : uri;
        if (im.has("bufferView")) {
            View v = view(inum(im["bufferView"]));
            bytes.assign(reinterpret_cast<const char*>(v.p), v.len);
        } else if (uri.rfind("data:", 0) == 0) {
            size_t comma = uri.find(',');
            if (comma == std::string::npos || !base64(uri, comma + 1, bytes)) { warn("image " + std::to_string(idx) + ": bad data URI"); return false; }
        } else if (!read_(dir_ + urlDecode(uri), bytes)) {
            warn("image file '" + dir_ + urlDecode(uri) + "' not found");
            return false;
        }
        if (!decodeImage(bytes, img)) { warn("image " + img.name + ": unsupported format (PNG, JPEG, TGA only)"); return false; }
        return true;
    }

    int texRef(const Json& info, Material& m) {
        if (!info.isObject()) return -1;
        int t = inum(info["index"]);
        if (t < 0 || t >= int(texMap_.size())) return -1;
        if (inum(info["texCoord"], 0) != 0) warn("material '" + m.name + "': TEXCOORD_1 is not supported, using TEXCOORD_0");
        return texMap_[size_t(t)];
    }

    void loadMaterials() {
        const auto& mats = doc_["materials"].items();
        for (size_t i = 0; i < mats.size(); ++i) {
            const Json& j = mats[i];
            Material m;
            m.name = j.getStr("name", "material" + std::to_string(i));
            m.metallic = 1; m.roughness = 1; // glTF defaults
            const Json& pbr = j["pbrMetallicRoughness"];
            if (const Json& bc = pbr["baseColorFactor"]; bc.size() == 4)
                m.baseColor = {float(num(bc[0], 1)), float(num(bc[1], 1)), float(num(bc[2], 1)), float(num(bc[3], 1))};
            m.baseTex = texRef(pbr["baseColorTexture"], m);
            if (const Json& tt = pbr["baseColorTexture"]["extensions"]["KHR_texture_transform"]; tt.isObject()) {
                if (tt["offset"].size() == 2) m.uvOffset = {float(num(tt["offset"][0], 0)), float(num(tt["offset"][1], 0))};
                if (tt["scale"].size() == 2) m.uvScale = {float(num(tt["scale"][0], 1)), float(num(tt["scale"][1], 1))};
                m.uvRotation = float(num(tt["rotation"], 0));
            }
            m.metallic = float(num(pbr["metallicFactor"], 1));
            m.roughness = float(num(pbr["roughnessFactor"], 1));
            m.mrTex = texRef(pbr["metallicRoughnessTexture"], m);
            m.normalTex = texRef(j["normalTexture"], m);
            m.normalScale = float(num(j["normalTexture"]["scale"], 1));
            m.occlusionTex = texRef(j["occlusionTexture"], m);
            m.occlusionStrength = float(num(j["occlusionTexture"]["strength"], 1));
            m.emissiveTex = texRef(j["emissiveTexture"], m);
            if (const Json& e = j["emissiveFactor"]; e.size() == 3) m.emissive = {float(num(e[0], 0)), float(num(e[1], 0)), float(num(e[2], 0))};
            if (m.emissiveTex >= 0 && !j.has("emissiveFactor")) m.emissive = {0, 0, 0};
            std::string am = j.getStr("alphaMode", "OPAQUE");
            m.alpha = am == "MASK" ? AlphaMode::Mask : am == "BLEND" ? AlphaMode::Blend : AlphaMode::Opaque;
            m.alphaCutoff = float(num(j["alphaCutoff"], 0.5));
            m.doubleSided = j.getBool("doubleSided");
            const Json& ext = j["extensions"];
            if (ext.has("KHR_materials_unlit")) m.shading = ShadingModel::Unlit;
            m.emissiveStrength = float(num(ext["KHR_materials_emissive_strength"]["emissiveStrength"], 1));
            if (const Json& sg = ext["KHR_materials_pbrSpecularGlossiness"]; sg.isObject() && !j.has("pbrMetallicRoughness")) {
                // approximation: diffuse becomes base colour, glossiness becomes 1 - roughness, dielectric
                if (const Json& d = sg["diffuseFactor"]; d.size() == 4)
                    m.baseColor = {float(num(d[0], 1)), float(num(d[1], 1)), float(num(d[2], 1)), float(num(d[3], 1))};
                m.baseTex = texRef(sg["diffuseTexture"], m);
                m.metallic = 0;
                m.roughness = 1.0f - float(num(sg["glossinessFactor"], 1));
                if (sg.has("specularGlossinessTexture")) warn("material '" + m.name + "': specular-glossiness texture approximated by factors");
            }
            model_->materials.push_back(m);
        }
    }

    void loadNodes() {
        const auto& nodes = doc_["nodes"].items();
        model_->nodes.resize(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            const Json& j = nodes[i];
            Node& n = model_->nodes[i];
            n.name = j.getStr("name", "node" + std::to_string(i));
            n.mesh = inum(j["mesh"]);
            n.skin = inum(j["skin"]);
            if (const Json& m = j["matrix"]; m.size() == 16) {
                mat4 mm;
                for (int k = 0; k < 16; ++k) mm.m[k] = float(num(m[size_t(k)], 0));
                decompose(mm, n.t, n.r, n.s);
            } else {
                if (const Json& t = j["translation"]; t.size() == 3) n.t = {float(num(t[0], 0)), float(num(t[1], 0)), float(num(t[2], 0))};
                if (const Json& r = j["rotation"]; r.size() == 4) n.r = qnormalize(quat{float(num(r[0], 0)), float(num(r[1], 0)), float(num(r[2], 0)), float(num(r[3], 1))});
                if (const Json& s = j["scale"]; s.size() == 3) n.s = {float(num(s[0], 1)), float(num(s[1], 1)), float(num(s[2], 1))};
            }
            for (const auto& c : j["children"].items()) {
                int ci = inum(c);
                if (ci < 0 || ci >= int(nodes.size())) fail("node " + std::to_string(i) + ": child out of range");
                parentOf_[ci] = int(i);
            }
            if (const Json& w = j["weights"]; w.size() > 0) {
                for (size_t k = 0; k < std::min<size_t>(w.size(), kMaxMorphTargets); ++k) n.weights[k] = float(num(w[k], 0));
                nodeWeights_.insert(int(i));
            }
            if (j.has("camera") || j["extensions"].has("KHR_lights_punctual")) warn("node '" + n.name + "': cameras and lights are ignored");
        }
        for (auto& [c, p] : parentOf_) model_->nodes[size_t(c)].parent = p;
        // only the default scene is visible: hide meshes of nodes outside it
        const Json& scenes = doc_["scenes"];
        if (scenes.size() > 0) {
            const Json& sc = scenes[size_t(std::max(0, inum(doc_["scene"], 0)))];
            std::vector<uint8_t> in(nodes.size(), 0);
            std::vector<int> stack;
            for (const auto& r : sc["nodes"].items()) if (inum(r) >= 0 && inum(r) < int(nodes.size())) stack.push_back(inum(r));
            while (!stack.empty()) {
                int n = stack.back(); stack.pop_back();
                if (in[size_t(n)]) continue;
                in[size_t(n)] = 1;
                for (const auto& c : nodes[size_t(n)]["children"].items()) stack.push_back(inum(c));
            }
            for (size_t i = 0; i < nodes.size(); ++i) if (!in[i]) model_->nodes[i].mesh = -1;
        }
    }

    void loadSkins() {
        const auto& skins = doc_["skins"].items();
        for (size_t i = 0; i < skins.size(); ++i) {
            const Json& j = skins[i];
            Skin s;
            s.name = j.getStr("name", "skin" + std::to_string(i));
            for (const auto& jn : j["joints"].items()) {
                int n = inum(jn);
                if (n < 0 || n >= int(model_->nodes.size())) fail("skin " + std::to_string(i) + ": joint out of range");
                s.joints.push_back(n);
            }
            s.inverseBind.assign(s.joints.size(), mat4{});
            if (j.has("inverseBindMatrices")) {
                size_t count = 0;
                auto v = accessorExpect(inum(j["inverseBindMatrices"]), 16, count, "inverseBindMatrices");
                for (size_t k = 0; k < std::min(count, s.joints.size()); ++k)
                    for (int e = 0; e < 16; ++e) s.inverseBind[k].m[e] = float(v[k * 16 + size_t(e)]);
            }
            model_->skins.push_back(std::move(s));
        }
        for (auto& n : model_->nodes) if (n.skin >= int(model_->skins.size())) n.skin = -1;
    }

    void loadMeshes() {
        const auto& meshes = doc_["meshes"].items();
        // skin used with each mesh (a skinned mesh needs its palette built against that skin)
        std::map<int, int> skinOfMesh;
        for (auto& n : model_->nodes) if (n.mesh >= 0 && n.skin >= 0) skinOfMesh[n.mesh] = n.skin;
        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            const Json& jm = meshes[mi];
            MeshDef md;
            md.name = jm.getStr("name", "mesh" + std::to_string(mi));
            const auto& prims = jm["primitives"].items();
            for (size_t pi = 0; pi < prims.size(); ++pi) loadPrimitive(prims[pi], md, skinOfMesh.count(int(mi)) > 0);
            for (const auto& w : jm["weights"].items()) md.weights.push_back(float(num(w, 0)));
            model_->meshes.push_back(std::move(md));
        }
        for (size_t i = 0; i < model_->nodes.size(); ++i) {
            Node& n = model_->nodes[i];
            if (n.mesh >= int(model_->meshes.size())) n.mesh = -1;
            if (n.mesh >= 0 && !nodeWeights_.count(int(i))) {
                const auto& w = model_->meshes[size_t(n.mesh)].weights;
                for (size_t k = 0; k < std::min<size_t>(w.size(), kMaxMorphTargets); ++k) n.weights[k] = w[k];
            }
        }
    }

    void loadPrimitive(const Json& p, MeshDef& md, bool skinned) {
        const std::string where = "mesh '" + md.name + "'";
        int mode = inum(p["mode"], 4);
        if (mode < 4) { warn(where + ": points / lines primitives are skipped"); return; }
        if (p["extensions"].has("KHR_draco_mesh_compression") || p["extensions"].has("EXT_meshopt_compression"))
            fail(where + ": compressed geometry (Draco / meshopt) is not supported, export uncompressed");
        const Json& at = p["attributes"];
        if (!at.has("POSITION")) fail(where + ": primitive without POSITION");
        if (at.has("TEXCOORD_1")) warn(where + ": TEXCOORD_1 is ignored");
        size_t n = 0, c = 0;
        auto pos = accessorExpect(inum(at["POSITION"]), 3, n, "POSITION");
        std::vector<ModelVertex> verts(n);
        for (size_t i = 0; i < n; ++i) verts[i].pos = {float(pos[i * 3]), float(pos[i * 3 + 1]), float(pos[i * 3 + 2])};
        const bool hasNormals = at.has("NORMAL"), hasTangents = at.has("TANGENT");
        if (hasNormals) {
            auto v = accessorExpect(inum(at["NORMAL"]), 3, c, "NORMAL");
            for (size_t i = 0; i < std::min(n, c); ++i) verts[i].nrm = normalize(float3{float(v[i * 3]), float(v[i * 3 + 1]), float(v[i * 3 + 2])});
        }
        if (at.has("TEXCOORD_0")) {
            auto v = accessorExpect(inum(at["TEXCOORD_0"]), 2, c, "TEXCOORD_0");
            for (size_t i = 0; i < std::min(n, c); ++i) verts[i].uv = {float(v[i * 2]), float(v[i * 2 + 1])};
        }
        if (hasTangents) {
            auto v = accessorExpect(inum(at["TANGENT"]), 4, c, "TANGENT");
            for (size_t i = 0; i < std::min(n, c); ++i) verts[i].tangent = {float(v[i * 4]), float(v[i * 4 + 1]), float(v[i * 4 + 2]), v[i * 4 + 3] < 0 ? -1.0f : 1.0f};
        }
        if (at.has("COLOR_0")) {
            int comps = 0;
            auto v = accessor(inum(at["COLOR_0"]), comps, c);
            auto q = [](double x) { return uint32_t(std::lround(std::clamp(x, 0.0, 1.0) * 255.0)); };
            for (size_t i = 0; i < std::min(n, c); ++i) {
                const double* e = &v[i * size_t(comps)];
                verts[i].color = q(e[0]) | (q(comps > 1 ? e[1] : 1) << 8) | (q(comps > 2 ? e[2] : 1) << 16) | (q(comps > 3 ? e[3] : 1) << 24);
            }
        }
        std::vector<double> joints, weights;
        if (skinned && at.has("JOINTS_0") && at.has("WEIGHTS_0")) {
            joints = accessorExpect(inum(at["JOINTS_0"]), 4, c, "JOINTS_0");
            if (c < n) fail(where + ": JOINTS_0 shorter than POSITION");
            weights = accessorExpect(inum(at["WEIGHTS_0"]), 4, c, "WEIGHTS_0");
            if (c < n) fail(where + ": WEIGHTS_0 shorter than POSITION");
            if (at.has("JOINTS_1")) warn(where + ": more than 4 joint influences per vertex; extra influences are ignored");
        }
        // morph targets: per original vertex [target][vertex] position xyz, normal xyz
        std::vector<float> morph;
        int nt = 0;
        if (const Json& tg = p["targets"]; tg.size() > 0) {
            nt = int(std::min<size_t>(tg.size(), kMaxMorphTargets));
            if (tg.size() > size_t(kMaxMorphTargets)) warn(where + ": only the first 8 morph targets are used");
            morph.assign(size_t(nt) * n * 6, 0.0f);
            for (int t = 0; t < nt; ++t) {
                const Json& tj = tg[size_t(t)];
                if (tj.has("POSITION")) {
                    auto v = accessorExpect(inum(tj["POSITION"]), 3, c, "morph POSITION");
                    for (size_t i = 0; i < std::min(n, c); ++i) for (int k = 0; k < 3; ++k) morph[(size_t(t) * n + i) * 6 + size_t(k)] = float(v[i * 3 + size_t(k)]);
                }
                if (tj.has("NORMAL")) {
                    auto v = accessorExpect(inum(tj["NORMAL"]), 3, c, "morph NORMAL");
                    for (size_t i = 0; i < std::min(n, c); ++i) for (int k = 0; k < 3; ++k) morph[(size_t(t) * n + i) * 6 + 3 + size_t(k)] = float(v[i * 3 + size_t(k)]);
                }
            }
        }
        // indices, converted to a triangle list
        std::vector<uint32_t> idx;
        if (p.has("indices")) {
            auto v = accessorExpect(inum(p["indices"]), 1, c, "indices");
            idx.reserve(c);
            for (double d : v) {
                if (d < 0 || d >= double(n)) fail(where + ": index out of range");
                idx.push_back(uint32_t(d));
            }
        } else {
            idx.resize(n);
            for (size_t i = 0; i < n; ++i) idx[i] = uint32_t(i);
        }
        if (mode == 5 || mode == 6) {
            std::vector<uint32_t> list;
            for (size_t i = 2; i < idx.size(); ++i) {
                if (mode == 5) {
                    if (i % 2 == 0) list.insert(list.end(), {idx[i - 2], idx[i - 1], idx[i]});
                    else list.insert(list.end(), {idx[i - 1], idx[i - 2], idx[i]});
                } else list.insert(list.end(), {idx[0], idx[i - 1], idx[i]});
            }
            idx.swap(list);
        }
        idx.resize(idx.size() / 3 * 3);
        if (!hasNormals) {
            const std::vector<uint32_t> before = idx;
            flatNormals(verts, idx);
            if (nt) { // vertices were unshared in triangle order: follow them with the deltas
                std::vector<float> m2(size_t(nt) * before.size() * 6);
                for (int t = 0; t < nt; ++t)
                    for (size_t k = 0; k < before.size(); ++k)
                        for (int e = 0; e < 6; ++e) m2[(size_t(t) * before.size() + k) * 6 + size_t(e)] = morph[(size_t(t) * n + before[k]) * 6 + size_t(e)];
                morph.swap(m2);
            }
        }
        morph_ = nt ? &morph : nullptr;
        morphTargets_ = nt;
        Primitive prim;
        prim.material = std::clamp(inum(p["material"], -1), -1, int(model_->materials.size()) - 1);
        if (prim.material < 0) prim.material = defaultMaterial();
        for (const auto& mp : p["extensions"]["KHR_materials_variants"]["mappings"].items()) {
            int mat = inum(mp["material"]);
            if (mat < 0 || mat >= int(model_->materials.size())) continue;
            for (const auto& vi : mp["variants"].items()) {
                int v = inum(vi);
                if (v < 0) continue;
                if (int(prim.variantMaterial.size()) <= v) prim.variantMaterial.resize(size_t(v) + 1, -1);
                prim.variantMaterial[size_t(v)] = mat;
            }
        }
        const Material& mat = model_->materials[size_t(prim.material)];
        const bool needTangents = mat.normalTex >= 0 && !hasTangents;
        if (joints.empty()) {
            appendChunk(md, prim, verts, idx, nullptr, nullptr, needTangents);
            return;
        }
        // Split into chunks whose joints fit one palette (kMaxPaletteJoints).
        std::vector<uint32_t> chunk;
        std::set<int> used;
        auto flush = [&]() {
            if (chunk.empty()) return;
            appendChunk(md, prim, verts, chunk, &joints, &weights, needTangents);
            chunk.clear(); used.clear();
        };
        for (size_t t = 0; t < idx.size(); t += 3) {
            std::set<int> tri;
            for (int k = 0; k < 3; ++k)
                for (int e = 0; e < 4; ++e)
                    if (weights[idx[t + size_t(k)] * 4 + size_t(e)] > 0 || e == 0) tri.insert(int(joints[idx[t + size_t(k)] * 4 + size_t(e)]));
            std::set<int> merged = used;
            merged.insert(tri.begin(), tri.end());
            if (int(merged.size()) > kMaxPaletteJoints) { flush(); merged = tri; }
            used = merged;
            chunk.insert(chunk.end(), {idx[t], idx[t + 1], idx[t + 2]});
        }
        flush();
    }

    // Appends the vertices referenced by `idx` (re-indexed) as one primitive.
    void appendChunk(MeshDef& md, Primitive prim, const std::vector<ModelVertex>& verts, const std::vector<uint32_t>& idx,
                     const std::vector<double>* joints, const std::vector<double>* weights, bool needTangents) {
        Model& m = *model_;
        std::vector<int32_t> remap(verts.size(), -1);
        std::vector<uint32_t> orig;   // local vertex -> source vertex
        std::map<int, int> palette;
        const uint32_t base = uint32_t(m.vertices.size());
        prim.firstIndex = uint32_t(m.indices.size());
        prim.indexCount = uint32_t(idx.size());
        for (uint32_t i : idx) {
            if (remap[i] < 0) {
                remap[i] = int32_t(m.vertices.size() - base);
                orig.push_back(i);
                ModelVertex v = verts[i];
                if (joints) {
                    float w[4], sum = 0;
                    for (int e = 0; e < 4; ++e) { w[e] = float(std::max(0.0, (*weights)[i * 4 + size_t(e)])); sum += w[e]; }
                    if (sum <= 0) { w[0] = sum = 1; } // unweighted vertex: follow its first joint
                    for (int e = 0; e < 4; ++e) {
                        int j = int((*joints)[i * 4 + size_t(e)]);
                        if (w[e] <= 0) { v.joints[e] = 0; v.weights[e] = 0; continue; }
                        auto it = palette.find(j);
                        if (it == palette.end()) it = palette.emplace(j, int(palette.size())).first;
                        v.joints[e] = uint8_t(it->second);
                        v.weights[e] = uint8_t(std::lround(w[e] / (sum > 0 ? sum : 1) * 255.0f));
                    }
                }
                m.vertices.push_back(v);
            }
            m.indices.push_back(base + uint32_t(remap[i]));
        }
        if (joints) {
            prim.jointPalette.resize(palette.size());
            for (auto& [j, slot] : palette) prim.jointPalette[size_t(slot)] = uint16_t(j);
            if (prim.jointPalette.empty()) prim.jointPalette.push_back(0);
        }
        if (needTangents) generateTangents(m.vertices, m.indices, prim.firstIndex, prim.indexCount);
        prim.firstVertex = base;
        prim.vertexCount = uint32_t(orig.size());
        if (morph_ && morphTargets_ > 0) {
            const size_t src = verts.size(), cnt = orig.size();
            prim.morphTargets = morphTargets_;
            prim.morphDeltas.resize(size_t(morphTargets_) * cnt * 6);
            for (int t = 0; t < morphTargets_; ++t)
                for (size_t k = 0; k < cnt; ++k)
                    for (int e = 0; e < 6; ++e)
                        prim.morphDeltas[(size_t(t) * cnt + k) * 6 + size_t(e)] = (*morph_)[(size_t(t) * src + orig[k]) * 6 + size_t(e)];
        }
        md.primitives.push_back(std::move(prim));
    }

    static void flatNormals(std::vector<ModelVertex>& v, std::vector<uint32_t>& idx) {
        // unshare vertices so each triangle gets its face normal
        std::vector<ModelVertex> out;
        out.reserve(idx.size());
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            ModelVertex a = v[idx[t]], b = v[idx[t + 1]], c = v[idx[t + 2]];
            float3 n = normalize(cross(b.pos - a.pos, c.pos - a.pos));
            a.nrm = b.nrm = c.nrm = n;
            out.push_back(a); out.push_back(b); out.push_back(c);
        }
        v.swap(out);
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = uint32_t(i);
    }

    int defaultMaterial() {
        if (defaultMat_ < 0) {
            Material m; m.name = "default";
            defaultMat_ = int(model_->materials.size());
            model_->materials.push_back(m);
        }
        return defaultMat_;
    }

    void loadAnimations() {
        const auto& anims = doc_["animations"].items();
        for (size_t ai = 0; ai < anims.size(); ++ai) {
            const Json& ja = anims[ai];
            AnimClip clip;
            clip.name = ja.getStr("name", "animation" + std::to_string(ai));
            const Json& samplers = ja["samplers"];
            for (const auto& ch : ja["channels"].items()) {
                const Json& tgt = ch["target"];
                std::string path = tgt.getStr("path");
                int node = inum(tgt["node"]);
                if (node < 0 || node >= int(model_->nodes.size())) continue;
                const Json& s = samplers[size_t(std::max(0, inum(ch["sampler"], 0)))];
                AnimChannel c;
                c.node = node;
                c.path = path == "rotation" ? AnimChannel::Rotation : path == "scale" ? AnimChannel::Scale
                       : path == "weights" ? AnimChannel::Weights : AnimChannel::Translation;
                std::string ip = s.getStr("interpolation", "LINEAR");
                c.interp = ip == "STEP" ? AnimChannel::Step : ip == "CUBICSPLINE" ? AnimChannel::CubicSpline : AnimChannel::Linear;
                size_t nt = 0, nv = 0;
                auto t = accessorExpect(inum(s["input"]), 1, nt, "animation input");
                int comps = 0;
                auto v = accessor(inum(s["output"]), comps, nv);
                const size_t perKey = c.interp == AnimChannel::CubicSpline ? 3 : 1;
                int want = c.path == AnimChannel::Rotation ? 4 : 3;
                if (c.path == AnimChannel::Weights) { // scalar output: targets values per key
                    want = nt ? int(nv / (nt * perKey)) : 0;
                    if (comps != 1 || want <= 0) { warn("animation '" + clip.name + "': malformed weights channel skipped"); continue; }
                    comps = want;
                    nv /= size_t(want);
                }
                c.comps = uint8_t(std::min(want, 255));
                if (comps != want || nv < nt * perKey || nt == 0) { warn("animation '" + clip.name + "': malformed channel skipped"); continue; }
                c.times.assign(t.begin(), t.end());
                c.values.reserve(nt * perKey * size_t(want));
                for (size_t k = 0; k < nt * perKey * size_t(want); ++k) c.values.push_back(float(v[k]));
                clip.duration = std::max(clip.duration, c.times.back());
                clip.channels.push_back(std::move(c));
            }
            if (!clip.channels.empty()) model_->clips.push_back(std::move(clip));
        }
    }

    std::string path_, dir_;
    const FileReader& read_;
    std::vector<std::string>* warnings_;
    std::set<std::string> warned_;
    Json doc_;
    std::string glbBin_;
    std::vector<std::string> buffers_;
    std::vector<int> texMap_;
    std::map<int, int> parentOf_;
    std::set<int> nodeWeights_;           // nodes with their own morph weights
    const std::vector<float>* morph_ = nullptr;
    int morphTargets_ = 0;
    int defaultMat_ = -1;
    std::unique_ptr<Model> model_;
};

} // namespace

std::unique_ptr<Model> loadGltf(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings) {
    GltfReader r(path, read, warnings);
    return r.load(bytes);
}

} // namespace rts::render
