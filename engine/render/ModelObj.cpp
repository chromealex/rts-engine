// Wavefront OBJ + MTL loader: positions, UVs, normals, polygon faces (fan-triangulated), negative
// indices, per-vertex colours ("v x y z r g b"), one primitive per material, groups/objects become
// nodes. MTL: Kd, d/Tr, Ns (-> roughness), Ke, map_Kd, map_Bump/norm, map_Ke, and the PBR
// extension Pr/Pm. Static only (OBJ has no skeleton or animation).
#include "Model.h"

#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>
#include <sstream>
#include <stdexcept>

namespace rts::render {

namespace {

std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// texture statements may carry options ("map_Kd -s 1 1 1 file.png"): the file is the last token
std::string mapFile(const std::string& rest) {
    std::string r = trim(rest);
    size_t sp = r.find_last_of(" \t");
    return sp == std::string::npos ? r : r.substr(sp + 1);
}

struct ObjParser {
    std::string path, dir;
    const FileReader& read;
    std::vector<std::string>* warnings;
    Model& m;
    std::map<std::string, int> materialIds, imageIds;

    void warn(const std::string& w) { if (warnings) warnings->push_back(path + ": " + w); }

    int image(const std::string& file) {
        auto it = imageIds.find(file);
        if (it != imageIds.end()) return it->second;
        std::string bytes;
        Image img;
        img.name = file;
        int id = -1;
        if (!read(dir + file, bytes)) warn("texture '" + dir + file + "' not found");
        else if (!decodeImage(bytes, img)) warn("texture '" + file + "': unsupported format");
        else { id = int(m.images.size()); m.images.push_back(std::move(img)); }
        imageIds[file] = id;
        return id;
    }

    void loadMtl(const std::string& file) {
        std::string text;
        if (!read(dir + file, text)) { warn("material library '" + dir + file + "' not found"); return; }
        std::istringstream in(text);
        std::string line;
        Material* cur = nullptr;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            std::string k;
            ls >> k;
            std::string rest;
            std::getline(ls, rest);
            std::istringstream rs(rest);
            if (k == "newmtl") {
                Material mt;
                mt.name = trim(rest);
                mt.roughness = 0.8f;
                materialIds[mt.name] = int(m.materials.size());
                m.materials.push_back(mt);
                cur = &m.materials.back();
                continue;
            }
            if (!cur) continue;
            float a = 0, b = 0, c = 0;
            if (k == "Kd" && (rs >> a >> b >> c)) cur->baseColor = {a, b, c, cur->baseColor.w};
            else if (k == "d" && (rs >> a)) cur->baseColor.w = a;
            else if (k == "Tr" && (rs >> a)) cur->baseColor.w = 1 - a;
            else if (k == "Ns" && (rs >> a)) cur->roughness = std::sqrt(2.0f / (a + 2.0f));
            else if (k == "Pr" && (rs >> a)) cur->roughness = a;
            else if (k == "Pm" && (rs >> a)) cur->metallic = a;
            else if (k == "Ke" && (rs >> a >> b >> c)) cur->emissive = {a, b, c};
            else if (k == "map_Kd") cur->baseTex = image(mapFile(rest));
            else if (k == "map_Bump" || k == "map_bump" || k == "bump" || k == "norm") cur->normalTex = image(mapFile(rest));
            else if (k == "map_Ke") { cur->emissiveTex = image(mapFile(rest)); if (cur->emissive.x + cur->emissive.y + cur->emissive.z == 0) cur->emissive = {1, 1, 1}; }
            else if (k == "map_Pr" || k == "map_Pm" || k == "map_Ks") warn("material '" + cur->name + "': " + k + " ignored (use glTF for PBR textures)");
        }
        for (auto& mt : m.materials) if (mt.baseColor.w < 0.999f) mt.alpha = AlphaMode::Blend;
    }
};

} // namespace

std::unique_ptr<Model> loadObj(const std::string& path, const std::string& text, const FileReader& read, std::vector<std::string>* warnings) {
    auto model = std::make_unique<Model>();
    ObjParser P{path, dirOf(path), read, warnings, *model, {}, {}};
    std::vector<float3> pos, nrm, col;
    std::vector<float2> uv;
    bool anyColor = false;
    // current object -> material -> triangle corners (v, vt, vn)
    struct Corner { int v, t, n; };
    struct Group { std::string name; std::map<int, std::vector<Corner>> byMat; };
    std::vector<Group> groups(1);
    groups[0].name = "obj";
    int curMat = -1;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    auto resolve = [&](int i, size_t n) { return i < 0 ? int(n) + i : i - 1; };
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string k;
        ls >> k;
        if (k == "v") {
            float3 p; float3 c{1, 1, 1};
            ls >> p.x >> p.y >> p.z;
            if (ls >> c.x >> c.y >> c.z) anyColor = true;
            pos.push_back(p); col.push_back(c);
        } else if (k == "vt") {
            float2 t; ls >> t.x >> t.y; t.y = 1 - t.y; uv.push_back(t);
        } else if (k == "vn") {
            float3 n; ls >> n.x >> n.y >> n.z; nrm.push_back(normalize(n));
        } else if (k == "f") {
            std::vector<Corner> poly;
            std::string tok;
            while (ls >> tok) {
                Corner c{-1, -1, -1};
                int vals[3] = {0, 0, 0};
                size_t s1 = tok.find('/');
                vals[0] = std::atoi(tok.substr(0, s1).c_str());
                if (s1 != std::string::npos) {
                    size_t s2 = tok.find('/', s1 + 1);
                    std::string t = tok.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
                    if (!t.empty()) vals[1] = std::atoi(t.c_str());
                    if (s2 != std::string::npos) vals[2] = std::atoi(tok.substr(s2 + 1).c_str());
                }
                c.v = resolve(vals[0], pos.size());
                c.t = vals[1] ? resolve(vals[1], uv.size()) : -1;
                c.n = vals[2] ? resolve(vals[2], nrm.size()) : -1;
                if (c.v < 0 || c.v >= int(pos.size()) || c.t >= int(uv.size()) || c.n >= int(nrm.size()))
                    throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": face index out of range");
                poly.push_back(c);
            }
            auto& tris = groups.back().byMat[curMat];
            for (size_t i = 2; i < poly.size(); ++i) tris.insert(tris.end(), {poly[0], poly[i - 1], poly[i]});
        } else if (k == "usemtl") {
            std::string name; std::getline(ls, name); name = trim(name);
            auto it = P.materialIds.find(name);
            if (it == P.materialIds.end()) { P.warn("unknown material '" + name + "'"); curMat = -1; }
            else curMat = it->second;
        } else if (k == "mtllib") {
            std::string f; std::getline(ls, f); P.loadMtl(trim(f));
        } else if (k == "o" || k == "g") {
            std::string name; std::getline(ls, name); name = trim(name);
            if (!groups.back().byMat.empty()) groups.push_back({});
            groups.back().name = name.empty() ? "obj" : name;
        }
    }
    Model& m = *model;
    int defaultMat = -1;
    Node root; root.name = "root";
    m.nodes.push_back(root);
    for (auto& g : groups) {
        if (g.byMat.empty()) continue;
        MeshDef md; md.name = g.name;
        for (auto& [mat, corners] : g.byMat) {
            Primitive p;
            if (mat < 0) {
                if (defaultMat < 0) { Material d; d.name = "default"; defaultMat = int(m.materials.size()); m.materials.push_back(d); }
                p.material = defaultMat;
            } else p.material = mat;
            p.firstIndex = uint32_t(m.indices.size());
            std::map<std::tuple<int, int, int>, uint32_t> dedup;
            for (size_t i = 0; i < corners.size(); ++i) {
                const Corner& c = corners[i];
                auto key = std::make_tuple(c.v, c.t, c.n);
                auto it = dedup.find(key);
                if (it == dedup.end() || c.n < 0) {
                    ModelVertex v;
                    v.pos = pos[size_t(c.v)];
                    if (c.t >= 0) v.uv = uv[size_t(c.t)];
                    if (c.n >= 0) v.nrm = nrm[size_t(c.n)];
                    else { // face normal (no smoothing info)
                        size_t t0 = i / 3 * 3;
                        float3 a = pos[size_t(corners[t0].v)], b = pos[size_t(corners[t0 + 1].v)], cc = pos[size_t(corners[t0 + 2].v)];
                        v.nrm = normalize(cross(b - a, cc - a));
                    }
                    if (anyColor) {
                        float3 cl = col[size_t(c.v)];
                        auto q = [](float x) { return uint32_t(std::lround(std::fmin(std::fmax(x, 0.0f), 1.0f) * 255.0f)); };
                        v.color = q(cl.x) | (q(cl.y) << 8) | (q(cl.z) << 16) | 0xff000000u;
                    }
                    uint32_t id = uint32_t(m.vertices.size());
                    m.vertices.push_back(v);
                    if (c.n >= 0) dedup[key] = id;
                    m.indices.push_back(id);
                } else m.indices.push_back(it->second);
            }
            p.indexCount = uint32_t(m.indices.size()) - p.firstIndex;
            if (m.materials[size_t(p.material)].normalTex >= 0) generateTangents(m.vertices, m.indices, p.firstIndex, p.indexCount);
            md.primitives.push_back(p);
        }
        Node n; n.name = g.name; n.parent = 0; n.mesh = int(m.meshes.size());
        m.meshes.push_back(std::move(md));
        m.nodes.push_back(n);
    }
    if (m.meshes.empty()) throw std::runtime_error(path + ": no faces");
    m.finalize();
    return model;
}

} // namespace rts::render
