#include "Model.h"
#include "Renderer3D.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

// stb_image with static linkage: the UI layer has its own copy, both may end up in one binary.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace rts::render {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
} // namespace

int Model::findNode(const std::string& name) const {
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i].name == name) return int(i);
    return -1;
}
int Model::findClip(const std::string& name) const {
    for (size_t i = 0; i < clips.size(); ++i) if (clips[i].name == name) return int(i);
    const std::string l = lower(name);
    for (size_t i = 0; i < clips.size(); ++i) if (lower(clips[i].name) == l) return int(i);
    return -1;
}
int Model::findMaterial(const std::string& name) const {
    for (size_t i = 0; i < materials.size(); ++i) if (materials[i].name == name) return int(i);
    return -1;
}
int Model::findVariant(const std::string& name) const {
    for (size_t i = 0; i < variants.size(); ++i) if (variants[i] == name) return int(i);
    return -1;
}

void Model::finalize() {
    if (materials.empty()) { Material m; m.name = "default"; materials.push_back(m); }
    for (auto& n : nodes) n.children.clear();
    for (size_t i = 0; i < nodes.size(); ++i) {
        int p = nodes[i].parent;
        if (p >= 0 && p < int(nodes.size())) nodes[size_t(p)].children.push_back(int(i));
        else nodes[i].parent = -1;
    }
    order.clear();
    std::vector<uint8_t> seen(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent >= 0) continue;
        std::vector<int> stack{int(i)};
        while (!stack.empty()) {
            int n = stack.back(); stack.pop_back();
            if (seen[size_t(n)]) continue; // cycles in broken files
            seen[size_t(n)] = 1;
            order.push_back(n);
            for (auto it = nodes[size_t(n)].children.rbegin(); it != nodes[size_t(n)].children.rend(); ++it) stack.push_back(*it);
        }
    }
    // primitive bounds from the vertices they reference
    for (auto& mesh : meshes)
        for (auto& p : mesh.primitives) {
            p.boundsMin = {1e30f, 1e30f, 1e30f}; p.boundsMax = {-1e30f, -1e30f, -1e30f};
            for (uint32_t i = 0; i < p.indexCount; ++i) {
                const float3 v = vertices[indices[p.firstIndex + i]].pos;
                p.boundsMin = min3(p.boundsMin, v); p.boundsMax = max3(p.boundsMax, v);
            }
            if (p.indexCount == 0) p.boundsMin = p.boundsMax = {0, 0, 0};
        }
    // model bounds in bind pose
    std::vector<mat4> world(nodes.size());
    for (int n : order) {
        const Node& nd = nodes[size_t(n)];
        mat4 l = mat4::trs(nd.t, nd.r, nd.s);
        world[size_t(n)] = nd.parent >= 0 ? world[size_t(nd.parent)] * l : l;
    }
    bindWorld = world;
    boundsMin = {1e30f, 1e30f, 1e30f}; boundsMax = {-1e30f, -1e30f, -1e30f};
    for (int n : order) {
        const Node& nd = nodes[size_t(n)];
        if (nd.mesh < 0) continue;
        for (auto& p : meshes[size_t(nd.mesh)].primitives) {
            mat4 xf = world[size_t(n)];
            if (nd.skin >= 0 && !p.jointPalette.empty()) { // bind pose: joint world * inverse bind
                const Skin& sk = skins[size_t(nd.skin)];
                int j = p.jointPalette[0];
                if (j < int(sk.joints.size())) xf = world[size_t(sk.joints[size_t(j)])] * sk.inverseBind[size_t(j)];
            }
            for (int c = 0; c < 8; ++c) {
                float3 v{c & 1 ? p.boundsMax.x : p.boundsMin.x, c & 2 ? p.boundsMax.y : p.boundsMin.y, c & 4 ? p.boundsMax.z : p.boundsMin.z};
                v = xf.transformPoint(v);
                boundsMin = min3(boundsMin, v); boundsMax = max3(boundsMax, v);
            }
        }
    }
    if (boundsMin.x > boundsMax.x) boundsMin = boundsMax = {0, 0, 0};
}

namespace {
using Loader = std::unique_ptr<Model> (*)(const std::string&, const std::string&, const FileReader&, std::vector<std::string>*);
struct Format { const char* ext; Loader load; };
const Format kFormats[] = {
    {".gltf", loadGltf}, {".glb", loadGltf}, {".obj", loadObj},
#if !defined(RTS_NO_FBX)
    {".fbx", loadFbx},
#endif
    {".vox", loadVox}, {".vxl", loadVxl}, {".ply", loadPly}, {".stl", loadStl}, {".3ds", load3ds}, {".dae", loadDae},
    {".mdx", loadMdx}, {".mdl", loadMdl}, {".3do", load3do}, {".w3d", loadW3d},
};
std::string extOf(const std::string& path) {
    const size_t dot = path.find_last_of('.'), slash = path.find_last_of("/\\");
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? std::string() : lower(path.substr(dot));
}
Loader loaderFor(const std::string& ext) {
    for (const auto& f : kFormats) if (ext == f.ext) return f.load;
    return nullptr;
}
} // namespace

bool isNativeModelFormat(const std::string& path) { return loaderFor(extOf(path)) != nullptr; }

std::unique_ptr<Model> loadModel(const std::string& path, const FileReader& read, std::vector<std::string>* warnings) {
    const std::string ext = extOf(path);
    Loader load = loaderFor(ext);
    std::string bytes;
    if (!load) {
        // converted on the host (tools/models/modelconv): "<file>.glb" next to the source
        if (read(path + ".glb", bytes)) {
            auto m = loadGltf(path + ".glb", bytes, read, warnings);
            m->path = path;
            return m;
        }
#if defined(RTS_NO_FBX)
        if (ext == ".fbx") throw std::runtime_error(path + ": FBX import is disabled in this build (RTS_FBX=OFF); convert to .glb");
#endif
        if (!read(path, bytes)) throw std::runtime_error(path + ": file not found");
        throw std::runtime_error(path + ": unsupported model format '" + ext + "' (convert it with tools/models/modelconv, "
                                 "or use .gltf, .glb, .obj, .fbx, .dae, .3ds, .ply, .stl, .vox, .vxl, .mdx, .mdl, .3do, .w3d)");
    }
    if (!read(path, bytes)) throw std::runtime_error(path + ": file not found");
    auto m = load(path, bytes, read, warnings);
    m->path = path;
    return m;
}

bool decodeImage(const std::string& bytes, Image& out) {
    if (isKtx2(bytes)) {
        auto ci = std::make_shared<CompressedImage>();
        std::string err;
        std::vector<uint8_t> rgba;
        if (!parseKtx2(bytes, *ci, err)) return false;
        // no CPU decoder (ASTC): usable only where the GPU samples it natively (uploadModel)
        if (!decodeBlocks(*ci, 0, rgba) && ci->format != BlockFormat::ASTC_4x4) return false;
        out.width = ci->width; out.height = ci->height;
        out.rgba = std::move(rgba);
        out.compressed = std::move(ci);
        return true;
    }
    if (bytes.size() >= 4 && (bytes.compare(0, 4, "BLP1") == 0 || bytes.compare(0, 4, "BLP2") == 0)) return decodeBlp(bytes, out);
    if (bytes.size() >= 4 && bytes.compare(0, 4, "DDS ") == 0) return decodeDds(bytes, out);
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return false;
    out.width = w; out.height = h;
    out.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return true;
}

std::unique_ptr<Model> modelFromParts(const std::string& name, const std::vector<ModelPart>& parts) {
    auto m = std::make_unique<Model>();
    m->path = name;
    Node root; root.name = "root";
    m->nodes.push_back(root);
    for (const auto& part : parts) {
        const auto& sv = part.mesh->vertices();
        const auto& si = part.mesh->indices();
        Primitive p;
        p.firstIndex = uint32_t(m->indices.size());
        p.indexCount = uint32_t(si.size());
        p.material = int(m->materials.size());
        Material mat = part.material;
        if (mat.name.empty()) mat.name = part.name;
        m->materials.push_back(mat);
        const uint32_t base = uint32_t(m->vertices.size());
        for (const auto& v : sv) {
            ModelVertex mv;
            mv.pos = v.pos; mv.nrm = v.nrm;
            mv.uv = {v.pos.x + v.pos.z, v.pos.y}; // planar, only for tiling detail textures
            auto ch = [&](int s) { return uint32_t(std::lround(srgbToLinear(float((v.rgba >> s) & 255u) / 255.0f) * 255.0f)); };
            mv.color = ch(0) | (ch(8) << 8) | (ch(16) << 16) | (v.rgba & 0xff000000u);
            m->vertices.push_back(mv);
        }
        for (uint32_t i : si) m->indices.push_back(base + i);
        MeshDef md; md.name = part.name; md.primitives.push_back(p);
        Node n; n.name = part.name; n.parent = 0; n.mesh = int(m->meshes.size());
        // decompose a translate/scale/rotateY part transform (enough for the built-in shapes)
        const mat4& x = part.xf;
        n.t = {x.at(0, 3), x.at(1, 3), x.at(2, 3)};
        float sx = length({x.at(0, 0), x.at(1, 0), x.at(2, 0)}), sy = length({x.at(0, 1), x.at(1, 1), x.at(2, 1)}), sz = length({x.at(0, 2), x.at(1, 2), x.at(2, 2)});
        n.s = {sx, sy, sz};
        n.r = quat::axisAngle({0, 1, 0}, std::atan2(x.at(0, 2), x.at(0, 0)));
        m->meshes.push_back(md);
        m->nodes.push_back(n);
    }
    m->finalize();
    return m;
}

void generateTangents(std::vector<ModelVertex>& v, const std::vector<uint32_t>& idx, uint32_t first, uint32_t count) {
    std::vector<float3> tan(v.size()), bit(v.size());
    for (uint32_t i = first; i + 2 < first + count; i += 3) {
        const uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        const float3 e1 = v[b].pos - v[a].pos, e2 = v[c].pos - v[a].pos;
        const float du1 = v[b].uv.x - v[a].uv.x, dv1 = v[b].uv.y - v[a].uv.y;
        const float du2 = v[c].uv.x - v[a].uv.x, dv2 = v[c].uv.y - v[a].uv.y;
        float d = du1 * dv2 - du2 * dv1;
        if (std::fabs(d) < 1e-12f) continue;
        d = 1.0f / d;
        const float3 t = (e1 * dv2 - e2 * dv1) * d, bt = (e2 * du1 - e1 * du2) * d;
        for (uint32_t k : {a, b, c}) { tan[k] += t; bit[k] += bt; }
    }
    for (uint32_t i = first; i < first + count; ++i) {
        const uint32_t k = idx[i];
        const float3 n = v[k].nrm;
        float3 t = tan[k] - n * dot(n, tan[k]);
        if (dot(t, t) < 1e-12f) t = std::fabs(n.x) < 0.9f ? cross(n, {1, 0, 0}) : cross(n, {0, 1, 0});
        t = normalize(t);
        const float w = dot(cross(n, t), bit[k]) < 0 ? -1.0f : 1.0f;
        v[k].tangent = {t.x, t.y, t.z, w};
    }
}

namespace {
gfx::PixelFormat gpuFormat(const CompressedImage& c) {
    switch (c.format) {
        case BlockFormat::ETC2_RGB8: return gfx::PixelFormat::ETC2_RGB8;
        case BlockFormat::ETC2_RGBA8: return gfx::PixelFormat::ETC2_RGBA8;
        case BlockFormat::BC1: return c.vkFormat == 131 || c.vkFormat == 132 ? gfx::PixelFormat::RGBA8 : gfx::PixelFormat::BC1;
        case BlockFormat::BC3: return gfx::PixelFormat::BC3;
        case BlockFormat::BC7: return gfx::PixelFormat::BC7;
        case BlockFormat::ASTC_4x4: return gfx::PixelFormat::ASTC_4x4;
        default: return gfx::PixelFormat::RGBA8; // EAC R11 / BC4 / RGBA8: the decoded copy
    }
}

void uploadImage(gfx::Device& dev, Image& img) {
    if (img.tex.valid()) return;
    if (img.compressed && !img.compressed->levels.empty()) {
        // keep KTX2 blocks compressed in VRAM when the GPU samples them (shaders decode sRGB themselves)
        const CompressedImage& c = *img.compressed;
        const gfx::PixelFormat pf = gpuFormat(c);
        if (pf != gfx::PixelFormat::RGBA8 && dev.supports(pf)) {
            std::vector<const void*> data;
            std::vector<size_t> sizes;
            for (const auto& l : c.levels) { data.push_back(l.data()); sizes.push_back(l.size()); }
            gfx::TextureDesc td;
            td.width = c.width; td.height = c.height;
            td.format = pf;
            td.filter = img.nearest ? gfx::Filter::Nearest : gfx::Filter::Linear;
            td.repeat = img.repeat;
            td.levelCount = int(c.levels.size());
            td.levelData = data.data();
            td.levelSize = sizes.data();
            td.label = img.name.c_str();
            img.tex = dev.createTexture(td);
            img.gpuCompressed = img.tex.valid();
            if (img.tex.valid()) return;
        }
    }
    if (img.rgba.empty()) return;
    gfx::TextureDesc td;
    td.width = img.width; td.height = img.height;
    td.format = gfx::PixelFormat::RGBA8;
    td.filter = img.nearest ? gfx::Filter::Nearest : gfx::Filter::Linear;
    td.repeat = img.repeat;
    td.mipmaps = !img.nearest;
    td.data = img.rgba.data();
    td.label = img.name.c_str();
    img.tex = dev.createTexture(td);
}
} // namespace

bool uploadModel(gfx::Device& dev, Model& m, bool keepCpu) {
    if (!m.vertices.empty() && !m.vb.valid()) {
        gfx::BufferDesc vb; vb.type = gfx::BufferType::Vertex; vb.size = m.vertices.size() * sizeof(ModelVertex); vb.data = m.vertices.data(); vb.label = m.path.c_str();
        m.vb = dev.createBuffer(vb);
        gfx::BufferDesc ib; ib.type = gfx::BufferType::Index; ib.size = m.indices.size() * sizeof(uint32_t); ib.data = m.indices.data();
        m.ib = dev.createBuffer(ib);
    }
    for (auto& img : m.images) {
        uploadImage(dev, img);
        if (!keepCpu) { img.rgba.clear(); img.rgba.shrink_to_fit(); img.compressed.reset(); }
    }
    // morph deltas of every primitive in one float texture: 2 texels (position, normal) per vertex and target
    size_t texels = 0;
    for (auto& mesh : m.meshes)
        for (auto& p : mesh.primitives) if (p.morphTargets > 0 && !p.morphDeltas.empty()) texels += p.morphDeltas.size() / 3;
    if (texels > 0 && !m.morphTex.valid()) {
        const int width = 2048, height = int((texels + width - 1) / width);
        std::vector<float> data(size_t(width) * size_t(height) * 4, 0.0f);
        size_t at = 0;
        for (auto& mesh : m.meshes)
            for (auto& p : mesh.primitives) {
                if (p.morphTargets <= 0 || p.morphDeltas.empty()) continue;
                p.morphTexel = uint32_t(at);
                for (size_t i = 0; i + 2 < p.morphDeltas.size(); i += 3, ++at)
                    for (int k = 0; k < 3; ++k) data[at * 4 + size_t(k)] = p.morphDeltas[i + size_t(k)];
                if (!keepCpu) { p.morphDeltas.clear(); p.morphDeltas.shrink_to_fit(); }
            }
        gfx::TextureDesc td;
        td.width = width; td.height = height;
        td.format = gfx::PixelFormat::RGBA32F;
        td.filter = gfx::Filter::Nearest;
        td.data = data.data();
        td.label = "morph targets";
        m.morphTex = dev.createTexture(td);
        m.morphTexWidth = width;
    }
    if (!keepCpu) {
        m.vertices.clear(); m.vertices.shrink_to_fit();
        m.indices.clear(); m.indices.shrink_to_fit();
    }
    return m.vertices.empty() || m.vb.valid();
}

int addImage(gfx::Device& dev, Model& m, Image img) {
    for (size_t i = 0; i < m.images.size(); ++i) if (!img.name.empty() && m.images[i].name == img.name) return int(i);
    uploadImage(dev, img);
    img.rgba.clear(); img.rgba.shrink_to_fit();
    m.images.push_back(std::move(img));
    return int(m.images.size() - 1);
}

void releaseModel(gfx::Device& dev, Model& m) {
    if (m.vb.valid()) dev.destroyBuffer(m.vb);
    if (m.ib.valid()) dev.destroyBuffer(m.ib);
    m.vb = {}; m.ib = {};
    for (auto& img : m.images) if (img.tex.valid()) { dev.destroyTexture(img.tex); img.tex = {}; }
    if (m.morphTex.valid()) dev.destroyTexture(m.morphTex);
    if (m.baked.tex.valid()) dev.destroyTexture(m.baked.tex);
    m.morphTex = {}; m.baked = Model::Baked{};
}

} // namespace rts::render
