// Stanford PLY and STL loaders (static meshes, one node, one material).
//
// PLY: ascii / binary_little_endian / binary_big_endian; any elements (unknown ones are skipped),
// scalar and list properties of every type. vertex: x y z, nx ny nz, s/t | u/v | texture_u/texture_v
// | texture_s/texture_t, red green blue alpha (uchar = sRGB, converted to linear; float = linear) or
// diffuse_red/...; face: vertex_indices | vertex_index (fan-triangulated), optional per-corner
// `texcoord` lists and per-face colours (then corners are not shared). `comment TextureFile x.png`
// becomes the base colour texture. Missing normals are smoothed (area-weighted).
//
// STL: ASCII ("solid ... facet normal ... vertex ...", several solids -> several nodes) and binary
// (80-byte header + count + 50-byte facets; binary files whose header starts with "solid" are told
// apart by size). Facets stay flat (STL has no shared vertices or smoothing): facet normals are
// used, recomputed from the winding when zero. Colours: VisCAM / SolidView (attribute bit 15 = valid,
// 5-bit BGR) and Materialise Magics ("COLOR=" / "MATERIAL=" in the header, bit 15 = 0 means the face
// has its own RGB colour).
//
// Axes: STL is Z up in practice (CAD, 3D printing) and is rotated to the engine's Y up:
// (x, y, z) -> (x, z, -y). PLY has no convention; the common sources (Stanford scans, MeshLab,
// older Blender exports) are Y up, so PLY keeps its axes unless a header comment says
// "up_axis Z" (or "z_up"). Units are kept as is (the view fits size).
#include "Model.h"
#include "ModelInterchange.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace rts::render {

using namespace interchange;

namespace {

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

std::string stem(const std::string& path) {
    std::string b = baseName(path);
    size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

void finishSingleMesh(Model& m, const std::string& name, uint32_t firstVertex) {
    MeshDef md;
    md.name = name;
    Primitive p;
    p.firstIndex = 0;
    p.indexCount = uint32_t(m.indices.size());
    p.firstVertex = firstVertex;
    p.vertexCount = uint32_t(m.vertices.size()) - firstVertex;
    p.material = 0;
    md.primitives.push_back(p);
    Node n;
    n.name = name;
    n.mesh = 0;
    m.meshes.push_back(std::move(md));
    m.nodes.push_back(n);
}

// ---------------------------------------------------------------- PLY

enum class PlyType : uint8_t { I8, U8, I16, U16, I32, U32, F32, F64 };

bool plyType(const std::string& s, PlyType& t) {
    static const struct { const char* n; PlyType t; } k[] = {
        {"char", PlyType::I8}, {"int8", PlyType::I8}, {"uchar", PlyType::U8}, {"uint8", PlyType::U8},
        {"short", PlyType::I16}, {"int16", PlyType::I16}, {"ushort", PlyType::U16}, {"uint16", PlyType::U16},
        {"int", PlyType::I32}, {"int32", PlyType::I32}, {"uint", PlyType::U32}, {"uint32", PlyType::U32},
        {"float", PlyType::F32}, {"float32", PlyType::F32}, {"double", PlyType::F64}, {"float64", PlyType::F64}};
    for (auto& e : k) if (s == e.n) { t = e.t; return true; }
    return false;
}

struct PlyProp {
    std::string name;
    PlyType type = PlyType::F32;
    bool list = false;
    PlyType countType = PlyType::U8;
};
struct PlyElement {
    std::string name;
    uint64_t count = 0;
    std::vector<PlyProp> props;
};

class PlyValues {   // reads values of the body in one of the three encodings
public:
    PlyValues(const std::string& path, const std::string& b, size_t pos, int format) : r_(path, b, pos), b_(b), format_(format) {}

    double read(PlyType t) {
        if (format_ == 0) return ascii();
        const bool be = format_ == 2;
        switch (t) {
        case PlyType::I8: return double(be ? r_.be<int8_t>() : r_.le<int8_t>());
        case PlyType::U8: return double(r_.u8());
        case PlyType::I16: return double(be ? r_.be<int16_t>() : r_.le<int16_t>());
        case PlyType::U16: return double(be ? r_.be<uint16_t>() : r_.le<uint16_t>());
        case PlyType::I32: return double(be ? r_.be<int32_t>() : r_.le<int32_t>());
        case PlyType::U32: return double(be ? r_.be<uint32_t>() : r_.le<uint32_t>());
        case PlyType::F32: return double(be ? r_.be<float>() : r_.le<float>());
        case PlyType::F64: return be ? r_.be<double>() : r_.le<double>();
        }
        return 0;
    }
    [[noreturn]] void fail(const std::string& w) const { r_.fail(w); }

private:
    double ascii() {
        const char* p = b_.data() + r_.pos();
        const char* e = b_.data() + r_.end();
        while (p < e && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p >= e) r_.fail("unexpected end of file");
        const char* s = p;
        while (p < e && !std::isspace(static_cast<unsigned char>(*p))) ++p;
        // strtod on a copy: floating-point std::from_chars needs macOS 26 / iOS 26 in Apple's libc++
        std::string tok(s, p);
        char* end = nullptr;
        double v = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str()) r_.fail("bad number '" + tok.substr(0, 20) + "'");
        r_.seek(size_t(p - b_.data()));
        return v;
    }
    ByteReader r_;
    const std::string& b_;
    int format_;   // 0 ascii, 1 little endian, 2 big endian
};

float colorValue(double v, PlyType t) {
    if (t == PlyType::F32 || t == PlyType::F64) return float(v);
    const double maxv = t == PlyType::U16 || t == PlyType::I16 ? 65535.0 : t == PlyType::U8 || t == PlyType::I8 ? 255.0 : 4294967295.0;
    return srgbToLinear(float(v / maxv));
}

float alphaValue(double v, PlyType t) {
    if (t == PlyType::F32 || t == PlyType::F64) return float(v);
    return float(v / (t == PlyType::U16 || t == PlyType::I16 ? 65535.0 : 255.0));
}

} // namespace

std::unique_ptr<Model> loadPly(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings) {
    auto fail = [&](const std::string& w) -> void { throw std::runtime_error(path + ": " + w); };
    if (bytes.compare(0, 3, "ply") != 0) fail("not a PLY file (missing 'ply' magic)");
    const size_t hdrEnd = bytes.find("end_header");
    if (hdrEnd == std::string::npos) fail("PLY header without end_header");
    size_t body = bytes.find('\n', hdrEnd);
    body = body == std::string::npos ? bytes.size() : body + 1;
    // ---- header
    int format = -1;
    bool zUp = false;
    std::string textureFile;
    std::vector<PlyElement> elements;
    {
        size_t p = 0;
        while (p < hdrEnd) {
            size_t e = bytes.find('\n', p);
            if (e == std::string::npos || e > hdrEnd) e = hdrEnd;
            std::string line = bytes.substr(p, e - p);
            p = e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            std::vector<std::string> tok;
            for (size_t a = 0; a < line.size();) {
                while (a < line.size() && std::isspace(static_cast<unsigned char>(line[a]))) ++a;
                size_t b = a;
                while (b < line.size() && !std::isspace(static_cast<unsigned char>(line[b]))) ++b;
                if (b > a) tok.push_back(line.substr(a, b - a));
                a = b;
            }
            if (tok.empty()) continue;
            if (tok[0] == "format" && tok.size() >= 2) {
                if (tok[1] == "ascii") format = 0;
                else if (tok[1] == "binary_little_endian") format = 1;
                else if (tok[1] == "binary_big_endian") format = 2;
                else fail("unknown PLY format '" + tok[1] + "'");
            } else if (tok[0] == "comment" || tok[0] == "obj_info") {
                if (tok.size() >= 3 && lowerStr(tok[1]) == "texturefile") {
                    textureFile = line.substr(line.find(tok[1]) + tok[1].size());
                    textureFile = textureFile.substr(std::min(textureFile.find_first_not_of(" \t"), textureFile.size()));
                }
                const std::string l = lowerStr(line);
                if (l.find("up_axis z") != std::string::npos || l.find("z_up") != std::string::npos) zUp = true;
            } else if (tok[0] == "element" && tok.size() >= 3) {
                PlyElement el;
                el.name = tok[1];
                char* end = nullptr;
                el.count = std::strtoull(tok[2].c_str(), &end, 10);
                elements.push_back(el);
            } else if (tok[0] == "property" && tok.size() >= 3) {
                if (elements.empty()) fail("property before any element");
                PlyProp pr;
                if (tok[1] == "list") {
                    if (tok.size() < 5 || !plyType(tok[2], pr.countType) || !plyType(tok[3], pr.type)) fail("bad list property '" + line + "'");
                    pr.list = true;
                    pr.name = tok[4];
                } else {
                    if (!plyType(tok[1], pr.type)) fail("unknown property type '" + tok[1] + "'");
                    pr.name = tok[2];
                }
                elements.back().props.push_back(pr);
            }
        }
    }
    if (format < 0) fail("PLY header without a format line");
    // a count that cannot fit in the file is broken (and would make us allocate forever)
    for (auto& el : elements) {
        // every item takes at least one byte (binary) or two characters (ascii)
        if (!el.props.empty() && el.count > uint64_t(bytes.size() - std::min(body, bytes.size())) / (format == 0 ? 2 : 1) + 1)
            fail("element '" + el.name + "' count " + std::to_string(el.count) + " exceeds the file size");
    }

    auto model = std::make_unique<Model>();
    Model& m = *model;
    std::vector<ModelVertex> verts;
    std::vector<uint32_t> idx;
    bool hasNormals = false, hasUv = false, hasColor = false, perCorner = false;
    PlyValues in(path, bytes, body, format);
    std::vector<std::vector<int>> faces;            // vertex indices per face
    std::vector<std::vector<float>> faceUv;         // per-corner uv (texcoord lists)
    std::vector<uint32_t> faceColor;                // per face (0 = none)
    for (const PlyElement& el : elements) {
        const bool isVertex = el.name == "vertex", isFace = el.name == "face";
        // property slots of the vertex element
        int px = -1, py = -1, pz = -1, nx = -1, ny = -1, nz = -1, pu = -1, pv = -1, cr = -1, cg = -1, cb = -1, ca = -1;
        int fIdx = -1, fUv = -1, fr = -1, fg = -1, fb = -1, fa = -1;
        for (size_t i = 0; i < el.props.size(); ++i) {
            const std::string& n = el.props[i].name;
            const int k = int(i);
            if (isVertex && !el.props[i].list) {
                if (n == "x") px = k; else if (n == "y") py = k; else if (n == "z") pz = k;
                else if (n == "nx") nx = k; else if (n == "ny") ny = k; else if (n == "nz") nz = k;
                else if (n == "s" || n == "u" || n == "texture_u" || n == "texture_s") pu = k;
                else if (n == "t" || n == "v" || n == "texture_v" || n == "texture_t") pv = k;
                else if (n == "red" || n == "diffuse_red" || n == "r") cr = k;
                else if (n == "green" || n == "diffuse_green" || n == "g") cg = k;
                else if (n == "blue" || n == "diffuse_blue" || n == "b") cb = k;
                else if (n == "alpha" || n == "diffuse_alpha" || n == "a") ca = k;
            }
            if (isFace) {
                if (el.props[i].list && (n == "vertex_indices" || n == "vertex_index")) fIdx = k;
                else if (el.props[i].list && n == "texcoord") fUv = k;
                else if (!el.props[i].list && (n == "red" || n == "diffuse_red")) fr = k;
                else if (!el.props[i].list && (n == "green" || n == "diffuse_green")) fg = k;
                else if (!el.props[i].list && (n == "blue" || n == "diffuse_blue")) fb = k;
                else if (!el.props[i].list && (n == "alpha" || n == "diffuse_alpha")) fa = k;
            }
        }
        if (isVertex) {
            if (px < 0 || py < 0 || pz < 0) fail("vertex element without x, y, z");
            hasNormals = nx >= 0 && ny >= 0 && nz >= 0;
            hasUv = pu >= 0 && pv >= 0;
            hasColor = cr >= 0 && cg >= 0 && cb >= 0;
            verts.reserve(size_t(std::min<uint64_t>(el.count, 1u << 24)));
        }
        if (isFace) {
            if (fIdx < 0) fail("face element without vertex_indices");
            faces.reserve(size_t(std::min<uint64_t>(el.count, 1u << 24)));
            perCorner = fUv >= 0 || (fr >= 0 && fg >= 0 && fb >= 0);
        }
        std::vector<double> vals(el.props.size());
        std::vector<double> list;
        for (uint64_t item = 0; item < el.count; ++item) {
            std::vector<int> fv;
            std::vector<float> fuv;
            for (size_t i = 0; i < el.props.size(); ++i) {
                const PlyProp& pr = el.props[i];
                if (!pr.list) { vals[i] = in.read(pr.type); continue; }
                const double cnt = in.read(pr.countType);
                if (!(cnt >= 0) || cnt > 1e6) in.fail("bad list length in element '" + el.name + "'");
                const int n = int(cnt);
                if (isFace && int(i) == fIdx) {
                    fv.resize(size_t(n));
                    for (int k = 0; k < n; ++k) {
                        const double x = in.read(pr.type);
                        fv[size_t(k)] = x >= 0 && x < 2147483647.0 ? int(x) : -1;
                    }
                } else if (isFace && int(i) == fUv) {
                    fuv.resize(size_t(n));
                    for (int k = 0; k < n; ++k) fuv[size_t(k)] = float(in.read(pr.type));
                } else for (int k = 0; k < n; ++k) in.read(pr.type);
            }
            if (isVertex) {
                ModelVertex v;
                v.pos = {float(vals[size_t(px)]), float(vals[size_t(py)]), float(vals[size_t(pz)])};
                if (hasNormals) v.nrm = {float(vals[size_t(nx)]), float(vals[size_t(ny)]), float(vals[size_t(nz)])};
                if (hasUv) v.uv = {float(vals[size_t(pu)]), 1.0f - float(vals[size_t(pv)])};
                if (hasColor) {
                    const auto& P = el.props;
                    v.color = packColor(colorValue(vals[size_t(cr)], P[size_t(cr)].type), colorValue(vals[size_t(cg)], P[size_t(cg)].type),
                                        colorValue(vals[size_t(cb)], P[size_t(cb)].type),
                                        ca >= 0 ? alphaValue(vals[size_t(ca)], P[size_t(ca)].type) : 1.0f);
                }
                verts.push_back(v);
            } else if (isFace) {
                faces.push_back(std::move(fv));
                if (fUv >= 0) faceUv.push_back(std::move(fuv));
                if (fr >= 0 && fg >= 0 && fb >= 0) {
                    const auto& P = el.props;
                    faceColor.push_back(packColor(colorValue(vals[size_t(fr)], P[size_t(fr)].type), colorValue(vals[size_t(fg)], P[size_t(fg)].type),
                                                  colorValue(vals[size_t(fb)], P[size_t(fb)].type),
                                                  fa >= 0 ? alphaValue(vals[size_t(fa)], P[size_t(fa)].type) : 1.0f));
                }
            }
        }
    }
    if (verts.empty()) fail("no vertices");
    // ---- triangles
    for (size_t f = 0; f < faces.size(); ++f) {
        const auto& fv = faces[f];
        for (int k : fv)
            if (k < 0 || size_t(k) >= verts.size()) fail("face " + std::to_string(f) + ": vertex index " + std::to_string(k) + " out of range");
        if (fv.size() < 3) continue;
        for (size_t i = 2; i < fv.size(); ++i) {
            const size_t c[3] = {0, i - 1, i};
            for (size_t k : c) {
                if (!perCorner) { idx.push_back(uint32_t(fv[k])); continue; }
                ModelVertex v = verts[size_t(fv[k])];
                if (f < faceUv.size() && faceUv[f].size() >= fv.size() * 2) v.uv = {faceUv[f][k * 2], 1.0f - faceUv[f][k * 2 + 1]};
                if (f < faceColor.size()) v.color = faceColor[f];
                idx.push_back(uint32_t(m.vertices.size()));
                m.vertices.push_back(v);
            }
        }
    }
    if (!perCorner) m.vertices = std::move(verts);
    if (!faceUv.empty()) hasUv = true;
    m.indices = std::move(idx);
    if (m.indices.empty()) {
        // point cloud: nothing to draw as triangles
        fail("no faces (point clouds are not supported)");
    }
    if (zUp)
        for (auto& v : m.vertices) { v.pos = zUpToYUp(v.pos); v.nrm = zUpToYUp(v.nrm); }
    if (hasNormals) for (auto& v : m.vertices) v.nrm = normalize(v.nrm);
    else smoothNormals(m.vertices, m.indices, 0, m.indices.size());
    Material mat;
    mat.name = "default";
    if (!textureFile.empty()) {
        TextureLoader tl{path, dirOf(path), read, warnings, m, {}};
        mat.baseTex = tl.load(textureFile);
        if (mat.baseTex >= 0 && !hasUv) tl.warn("TextureFile without texture coordinates");
    }
    m.materials.push_back(mat);
    finishSingleMesh(m, stem(path), 0);
    m.finalize();
    return model;
}

// ---------------------------------------------------------------- STL

namespace {

struct StlBuilder {
    Model& m;
    uint32_t firstIndex = 0, firstVertex = 0;
    void tri(float3 n, float3 a, float3 b, float3 c, uint32_t color) {
        a = zUpToYUp(a); b = zUpToYUp(b); c = zUpToYUp(c); n = zUpToYUp(n);
        if (!(dot(n, n) > 1e-12f) || !std::isfinite(n.x)) n = faceNormal(a, b, c);
        else n = normalize(n);
        for (float3 p : {a, b, c}) {
            ModelVertex v;
            v.pos = p;
            v.nrm = n;
            v.color = color;
            m.indices.push_back(uint32_t(m.vertices.size()));
            m.vertices.push_back(v);
        }
    }
    void node(const std::string& name) {   // closes the triangles added since the last node
        if (m.indices.size() == firstIndex) return;
        MeshDef md;
        md.name = name;
        Primitive p;
        p.firstIndex = firstIndex;
        p.indexCount = uint32_t(m.indices.size()) - firstIndex;
        p.firstVertex = firstVertex;
        p.vertexCount = uint32_t(m.vertices.size()) - firstVertex;
        md.primitives.push_back(p);
        Node n;
        n.name = name.empty() ? "solid" + std::to_string(m.nodes.size()) : name;
        n.mesh = int(m.meshes.size());
        m.meshes.push_back(std::move(md));
        m.nodes.push_back(n);
        firstIndex = uint32_t(m.indices.size());
        firstVertex = uint32_t(m.vertices.size());
    }
};

uint32_t rgb5(uint32_t r5, uint32_t g5, uint32_t b5) {
    return packColor(srgbToLinear(float(r5) / 31.0f), srgbToLinear(float(g5) / 31.0f), srgbToLinear(float(b5) / 31.0f), 1);
}

} // namespace

std::unique_ptr<Model> loadStl(const std::string& path, const std::string& bytes, const FileReader&, std::vector<std::string>* warnings) {
    auto model = std::make_unique<Model>();
    Model& m = *model;
    StlBuilder sb{m};
    uint32_t count = 0;
    if (bytes.size() >= 84) std::memcpy(&count, bytes.data() + 80, 4);
    const bool sizeMatches = bytes.size() >= 84 && uint64_t(count) * 50 + 84 == bytes.size();
    // binary files may start with "solid" too (some exporters write it into the header)
    const bool ascii = !sizeMatches && bytes.compare(0, 5, "solid") == 0 &&
                       (bytes.find("facet", 0) < 4096 || bytes.find("endsolid") != std::string::npos);
    if (!ascii) {
        if (bytes.size() < 84) throw std::runtime_error(path + ": not an STL file (too short for binary, no 'solid' keyword)");
        ByteReader r(path, bytes, 84);
        if (uint64_t(count) * 50 > r.left()) r.fail("binary STL: " + std::to_string(count) + " facets but the file holds " + std::to_string(r.left() / 50));
        if (!sizeMatches && warnings) warnings->push_back(path + ": binary STL with trailing data after " + std::to_string(count) + " facets");
        const std::string header = bytes.substr(0, 80);
        uint32_t defColor = 0xffffffffu;
        const size_t cpos = header.find("COLOR=");
        const bool magics = cpos != std::string::npos && cpos + 10 <= header.size();
        if (magics) {
            const uint8_t* c = reinterpret_cast<const uint8_t*>(header.data() + cpos + 6);
            defColor = packColor(srgbToLinear(c[0] / 255.0f), srgbToLinear(c[1] / 255.0f), srgbToLinear(c[2] / 255.0f), c[3] / 255.0f);
        }
        m.vertices.reserve(size_t(count) * 3);
        m.indices.reserve(size_t(count) * 3);
        for (uint32_t i = 0; i < count; ++i) {
            float f[12];
            for (float& x : f) x = r.f32();
            const uint16_t attr = r.u16();
            uint32_t col = defColor;
            if (magics) {
                // Materialise: bit 15 = 0 -> the face's own colour, red in the low bits
                if (!(attr & 0x8000u)) col = rgb5(attr & 31u, (attr >> 5) & 31u, (attr >> 10) & 31u);
            } else if (attr & 0x8000u) {
                // VisCAM / SolidView: bit 15 = valid, blue in the low bits
                col = rgb5((attr >> 10) & 31u, (attr >> 5) & 31u, attr & 31u);
            }
            sb.tri({f[0], f[1], f[2]}, {f[3], f[4], f[5]}, {f[6], f[7], f[8]}, {f[9], f[10], f[11]}, col);
        }
        sb.node(stem(path));
    } else {
        // ASCII: a tiny tokenizer; keywords are case-insensitive in the wild
        size_t p = 0;
        const size_t n = bytes.size();
        auto next = [&]() -> std::string {
            while (p < n && std::isspace(static_cast<unsigned char>(bytes[p]))) ++p;
            size_t s = p;
            while (p < n && !std::isspace(static_cast<unsigned char>(bytes[p]))) ++p;
            return bytes.substr(s, p - s);
        };
        auto restOfLine = [&]() {
            size_t e = bytes.find('\n', p);
            if (e == std::string::npos) e = n;
            std::string s = bytes.substr(p, e - p);
            p = e;
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        auto num = [&]() {
            const std::string t = next();
            char* end = nullptr;
            float v = std::strtof(t.c_str(), &end);
            if (t.empty() || end == t.c_str()) throw std::runtime_error(path + ": ASCII STL: expected a number, got '" + t.substr(0, 20) + "'");
            return v;
        };
        std::string name;
        float3 normal;
        std::vector<float3> loop;
        bool inSolid = false;
        for (std::string t = next(); !t.empty(); t = next()) {
            t = lowerStr(t);
            if (t == "solid") { name = restOfLine(); inSolid = true; }
            else if (t == "facet") {
                if (lowerStr(next()) != "normal") throw std::runtime_error(path + ": ASCII STL: 'facet' without 'normal'");
                normal.x = num(); normal.y = num(); normal.z = num();
                loop.clear();
            } else if (t == "vertex") {
                float3 v; v.x = num(); v.y = num(); v.z = num();
                loop.push_back(v);
            } else if (t == "endfacet") {
                for (size_t i = 2; i < loop.size(); ++i) sb.tri(normal, loop[0], loop[i - 1], loop[i], 0xffffffffu);
                loop.clear();
            } else if (t == "endsolid") { restOfLine(); sb.node(name); inSolid = false; }
            else if (t == "outer" || t == "loop" || t == "endloop") {}
            else throw std::runtime_error(path + ": ASCII STL: unexpected '" + t.substr(0, 20) + "'");
        }
        if (inSolid) {
            if (warnings) warnings->push_back(path + ": ASCII STL without 'endsolid'");
            sb.node(name);
        }
    }
    if (m.meshes.empty()) throw std::runtime_error(path + ": no facets");
    Material mat;
    mat.name = "default";
    m.materials.push_back(mat);
    m.finalize();
    return model;
}

} // namespace rts::render
