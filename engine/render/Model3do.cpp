// Total Annihilation .3do loader (+ .gaf textures and the TA palette), read from the user's own copy.
//
// A 3DO file is a tree of pieces ("base", "turret", "sleeve", "barrel", "flare", ...). Every piece
// becomes a node with exactly that name, translated by its offset from the parent, with its mesh in
// node-local space, so the engine's procedural animation (aim, recoil, spin) moves the pieces the
// way TA's COB scripts did. There is no skeletal animation in the format: no clips.
//
// Axes: TA is Y up with Z mirrored relative to the engine (Spring RTS reads it the same way), so
// positions are (x, y, -z) / 65536 (16.16 fixed point, native TA scale). The mirror makes TA's
// clockwise polygons counter-clockwise; polygons are fan-triangulated, flat shaded and one-sided.
// Skipped: the root's selection primitive, points and lines (< 3 vertices), and duplicate faces
// (same vertex set: TA draws only the last one, which carries e.g. the damaged texture).
//
// Colours: untextured polygons use their palette index in the TA palette: `<model>.pal`,
// `palette.pal` next to the model, `../palettes/PALETTE.PAL` (TA layout), 1024 bytes RGBX (or 768
// bytes RGB); otherwise a built-in approximation (warning).
// Textures by name: `<name>.gaf` (the entry with that name, else the first) in the model folder,
// `textures/` and `../textures/`, or a converted `<name>.png` / `.tga` / `.jpg` in the same folders.
// Missing textures fall back to the palette colour (one aggregated warning).
// Team colour: GAF entries with >= 10 frames hold one frame per player colour (TA logos); the
// pixels that differ between frames 0 and 1 become a team mask (TeamMode::Mask). Converted textures
// listed in `teamtex.txt` (Spring convention, one name per line, next to the model or in textures/)
// mark team colour with magenta pixels (r == b, g == 0).
#include "Model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace rts::render {

namespace {

std::string lowerStr(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string upperStr(std::string s) {
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string dirOf(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
std::string stemPath(const std::string& p) {
    const size_t dot = p.find_last_of('.'), slash = p.find_last_of("/\\");
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? p : p.substr(0, dot);
}
float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// Bounds-checked little-endian reads; `what` names the structure for the error message.
struct Bytes {
    const std::string& b;
    const std::string& path;
    bool has(size_t at, size_t n) const { return at <= b.size() && n <= b.size() - at; }
    void need(size_t at, size_t n, const char* what) const {
        if (!has(at, n)) throw std::runtime_error(path + ": " + what + " at offset " + std::to_string(at) + " is outside the file (truncated or corrupt)");
    }
    int32_t i32(size_t at) const { int32_t v; std::memcpy(&v, b.data() + at, 4); return v; }
    uint32_t u32(size_t at) const { uint32_t v; std::memcpy(&v, b.data() + at, 4); return v; }
    uint16_t u16(size_t at) const { uint16_t v; std::memcpy(&v, b.data() + at, 2); return v; }
    int16_t i16(size_t at) const { int16_t v; std::memcpy(&v, b.data() + at, 2); return v; }
    uint8_t u8(size_t at) const { return uint8_t(b[at]); }
    std::string cstr(size_t at, size_t maxLen) const {
        std::string s;
        for (size_t i = at; i < b.size() && s.size() < maxLen && b[i]; ++i) s += b[i];
        return s;
    }
};

// Generic stand-in for the TA palette: 16 greys, then 15 hue ramps of 16 shades.
std::vector<uint32_t> builtinPalette() {
    std::vector<uint32_t> p(256);
    for (int i = 0; i < 256; ++i) {
        float r, g, b;
        if (i < 16) r = g = b = float(i) / 15.0f;
        else {
            const float hue = float(i / 16 - 1) / 15.0f * 6.0f, v = 0.25f + 0.75f * float(i % 16) / 15.0f;
            const float f = hue - std::floor(hue);
            const int s = int(hue) % 6;
            const float q = v * (1 - 0.7f * f), t = v * (1 - 0.7f * (1 - f)), lo = v * 0.3f;
            const float rgb[6][3] = {{v, t, lo}, {q, v, lo}, {lo, v, t}, {lo, q, v}, {t, lo, v}, {v, lo, q}};
            r = rgb[s][0]; g = rgb[s][1]; b = rgb[s][2];
        }
        auto q8 = [](float x) { return uint32_t(std::lround(std::clamp(x, 0.0f, 1.0f) * 255)); };
        p[size_t(i)] = q8(r) | q8(g) << 8 | q8(b) << 16 | 0xff000000u;
    }
    return p;
}

struct GafImage { int w = 0, h = 0; std::vector<uint8_t> idx, opaque; };

class Ta3doLoader {
public:
    Ta3doLoader(const std::string& path, const FileReader& read, std::vector<std::string>* warnings)
        : path_(path), dir_(dirOf(path)), read_(read), warnings_(warnings) {}

    std::unique_ptr<Model> load(const std::string& bytes) {
        Bytes in{bytes, path_};
        if (bytes.size() < 52) throw std::runtime_error(path_ + ": too small for a 3DO piece header");
        if (in.i32(0) != 1) throw std::runtime_error(path_ + ": not a Total Annihilation 3DO file (piece version " + std::to_string(in.i32(0)) + ", expected 1)");
        loadPalette();
        loadTeamList();
        model_ = std::make_unique<Model>();
        paletteMat_ = -1;
        struct Todo { uint32_t at; int parent; };
        std::vector<Todo> stack{{0, -1}};
        std::set<uint32_t> seen;
        while (!stack.empty()) {
            const Todo t = stack.back();
            stack.pop_back();
            if (!seen.insert(t.at).second) throw std::runtime_error(path_ + ": piece tree has a cycle (offset " + std::to_string(t.at) + ")");
            if (seen.size() > 4096) throw std::runtime_error(path_ + ": more than 4096 pieces");
            in.need(t.at, 52, "piece header");
            if (in.i32(t.at) != 1) throw std::runtime_error(path_ + ": piece at offset " + std::to_string(t.at) + " has version " + std::to_string(in.i32(t.at)) + " (expected 1)");
            const int node = loadPiece(in, t.at, t.parent);
            const uint32_t sibling = in.u32(t.at + 44), child = in.u32(t.at + 48);
            // siblings share the parent; push the child last so the subtree comes first (file order)
            if (sibling) stack.push_back({sibling, t.parent});
            if (child) stack.push_back({child, node});
        }
        if (!missing_.empty()) {
            std::string list;
            for (const auto& n : missing_) list += (list.empty() ? "" : ", ") + n;
            warn("textures not found, palette colours used instead: " + list + " (looked for <name>.gaf / .png / .tga in the model folder, textures/ and ../textures/)");
        }
        model_->finalize();
        return std::move(model_);
    }

private:
    void warn(const std::string& w) { if (warnings_) warnings_->push_back(path_ + ": " + w); }

    bool readAny(const std::vector<std::string>& candidates, std::string& out, std::string* found = nullptr) const {
        for (const auto& c : candidates)
            if (read_(c, out)) { if (found) *found = c; return true; }
        return false;
    }

    void loadPalette() {
        std::string pal;
        const std::vector<std::string> where = {stemPath(path_) + ".pal", dir_ + "palette.pal", dir_ + "PALETTE.PAL",
                                                dir_ + "../palettes/palette.pal", dir_ + "../palettes/PALETTE.PAL",
                                                dir_ + "../palette.pal", dir_ + "../PALETTE.PAL"};
        if (readAny(where, pal) && (pal.size() >= 1024 || pal.size() == 768)) {
            const size_t stride = pal.size() == 768 ? 3 : 4;
            palette_.resize(256);
            for (size_t i = 0; i < 256; ++i)
                palette_[i] = uint32_t(uint8_t(pal[i * stride])) | uint32_t(uint8_t(pal[i * stride + 1])) << 8 |
                              uint32_t(uint8_t(pal[i * stride + 2])) << 16 | 0xff000000u;
            return;
        }
        palette_ = builtinPalette();
        warn("TA palette not found (palette.pal next to the model or ../palettes/PALETTE.PAL); using a built-in approximation");
    }

    void loadTeamList() {
        std::string text;
        if (!readAny({dir_ + "teamtex.txt", dir_ + "textures/teamtex.txt", dir_ + "../textures/teamtex.txt"}, text)) return;
        std::string line;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
                const size_t a = line.find_first_not_of(" \t"), b = line.find_last_not_of(" \t");
                if (a != std::string::npos) teamNames_.insert(lowerStr(line.substr(a, b - a + 1)));
                line.clear();
            } else line += text[i];
        }
    }

    // sRGB palette entry -> linear vertex colour
    uint32_t paletteColor(uint32_t index) const {
        const uint32_t c = palette_[index & 255u];
        auto ch = [&](int s) { return uint32_t(std::lround(srgbToLinear(float((c >> s) & 255u) / 255.0f) * 255.0f)); };
        return ch(0) | ch(8) << 8 | ch(16) << 16 | 0xff000000u;
    }

    int paletteMaterial() {
        if (paletteMat_ < 0) {
            Material m;
            m.name = "ta_palette";
            m.roughness = 0.75f;
            paletteMat_ = int(model_->materials.size());
            model_->materials.push_back(m);
        }
        return paletteMat_;
    }

    // ---- textures ----

    // Decodes one GAF frame (and its sub-frames) into palette indices + an opacity mask.
    bool gafFrame(const Bytes& g, uint32_t at, GafImage& img, int depth) const {
        if (!g.has(at, 24)) return false;
        const int w = g.u16(at), h = g.u16(at + 2);
        const int xpos = g.i16(at + 4), ypos = g.i16(at + 6);
        const uint8_t trans = g.u8(at + 8), compressed = g.u8(at + 9);
        const uint16_t subs = g.u16(at + 10);
        const uint32_t data = g.u32(at + 16);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
        if (depth == 0) {
            img.w = w; img.h = h;
            img.idx.assign(size_t(w) * size_t(h), 0);
            img.opaque.assign(size_t(w) * size_t(h), 0);
        }
        if (subs > 0) {
            if (depth > 0 || !g.has(data, size_t(subs) * 4)) return false;
            for (uint16_t s = 0; s < subs; ++s) {
                const uint32_t sub = g.u32(data + s * 4u);
                if (!g.has(sub, 24)) return false;
                GafImage part;
                if (!gafFrame(g, sub, part, 1)) return false;
                // sub-frames are placed by the difference of the origins
                const int ox = xpos - g.i16(sub + 4), oy = ypos - g.i16(sub + 6);
                for (int y = 0; y < part.h; ++y)
                    for (int x = 0; x < part.w; ++x) {
                        const int tx = ox + x, ty = oy + y;
                        const size_t si = size_t(y) * size_t(part.w) + size_t(x);
                        if (tx < 0 || ty < 0 || tx >= w || ty >= h || !part.opaque[si]) continue;
                        const size_t di = size_t(ty) * size_t(w) + size_t(tx);
                        img.idx[di] = part.idx[si];
                        img.opaque[di] = 1;
                    }
            }
            return true;
        }
        if (depth > 0) {
            img.w = w; img.h = h;
            img.idx.assign(size_t(w) * size_t(h), 0);
            img.opaque.assign(size_t(w) * size_t(h), 0);
        }
        auto put = [&](int x, int y, uint8_t v) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            img.idx[i] = v;
            img.opaque[i] = v != trans;
        };
        if (!compressed) {
            if (!g.has(data, size_t(w) * size_t(h))) return false;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) put(x, y, g.u8(data + size_t(y) * size_t(w) + size_t(x)));
            return true;
        }
        // TA run-length lines: u16 byte count, then masks: bit0 = skip (mask >> 1) transparent pixels,
        // bit1 = repeat the next byte (mask >> 2) + 1 times, else copy (mask >> 2) + 1 bytes
        size_t p = data;
        for (int y = 0; y < h; ++y) {
            if (!g.has(p, 2)) return false;
            const size_t len = g.u16(p), end = p + 2 + len;
            if (!g.has(p + 2, len)) return false;
            p += 2;
            int x = 0;
            while (p < end && x < w) {
                const uint8_t mask = g.u8(p++);
                if (mask & 1) x += mask >> 1;
                else if (mask & 2) {
                    if (p >= end) return false;
                    const uint8_t v = g.u8(p++);
                    for (int k = 0; k <= mask >> 2 && x < w; ++k) put(x++, y, v);
                } else {
                    for (int k = 0; k <= mask >> 2 && x < w; ++k) {
                        if (p >= end) return false;
                        put(x++, y, g.u8(p++));
                    }
                }
            }
            p = end;
        }
        return true;
    }

    // Finds `name` (case-insensitive; else the first entry) in a GAF archive and builds base + team mask.
    bool gafTexture(const std::string& bytes, const std::string& name, Image& img, Image& mask, bool& hasMask, std::string& err) const {
        Bytes g{bytes, path_};
        if (!g.has(0, 12) || g.u32(0) != 0x00010100u) { err = "not a GAF file"; return false; }
        const uint32_t entries = g.u32(4);
        if (entries == 0 || !g.has(12, size_t(entries) * 4)) { err = "broken GAF entry table"; return false; }
        uint32_t entry = 0;
        for (uint32_t e = 0; e < entries; ++e) {
            const uint32_t at = g.u32(12 + e * 4);
            if (!g.has(at, 40)) continue;
            if (e == 0 || lowerStr(g.cstr(at + 8, 32)) == lowerStr(name)) entry = at;
            if (lowerStr(g.cstr(at + 8, 32)) == lowerStr(name)) break;
        }
        if (!g.has(entry, 40)) { err = "broken GAF entry"; return false; }
        const uint16_t frames = g.u16(entry);
        if (frames == 0 || !g.has(entry + 40, size_t(frames) * 8)) { err = "GAF entry without frames"; return false; }
        GafImage f0;
        if (!gafFrame(g, g.u32(entry + 40), f0, 0)) { err = "broken GAF frame"; return false; }
        img.width = f0.w; img.height = f0.h;
        img.rgba.resize(size_t(f0.w) * size_t(f0.h) * 4);
        for (size_t i = 0; i < f0.idx.size(); ++i) {
            const uint32_t c = palette_[f0.idx[i]];
            std::memcpy(&img.rgba[i * 4], &c, 4);
            img.rgba[i * 4 + 3] = f0.opaque[i] ? 255 : 0;
        }
        hasMask = false;
        GafImage f1;
        if (frames >= 10 && gafFrame(g, g.u32(entry + 48), f1, 0) && f1.w == f0.w && f1.h == f0.h) {
            mask.width = f0.w; mask.height = f0.h;
            mask.rgba.assign(img.rgba.size(), 0);
            for (size_t i = 0; i < f0.idx.size(); ++i) {
                mask.rgba[i * 4 + 3] = 255;
                if (f0.idx[i] != f1.idx[i]) { mask.rgba[i * 4] = 255; hasMask = true; }
            }
        }
        return true;
    }

    // Material for a texture name; -1 when it can't be found (the caller falls back to the palette).
    int textureMaterial(const std::string& name) {
        auto it = texMats_.find(name);
        if (it != texMats_.end()) return it->second;
        int result = -1;
        std::vector<std::string> dirs = {dir_, dir_ + "textures/", dir_ + "../textures/"};
        std::vector<std::string> names = {name, lowerStr(name), upperStr(name)};
        names.erase(std::unique(names.begin(), names.end()), names.end());
        Image img, mask;
        bool hasMask = false, found = false;
        std::string bytes, err;
        for (const auto& d : dirs) {
            for (const auto& n : names) {
                for (const char* ext : {".gaf", ".GAF"})
                    if (!found && read_(d + n + ext, bytes)) {
                        if (gafTexture(bytes, name, img, mask, hasMask, err)) found = true;
                        else warn("texture '" + d + n + ext + "': " + err);
                    }
                for (const char* ext : {".png", ".tga", ".jpg", ".PNG", ".TGA", ".JPG"})
                    if (!found && read_(d + n + ext, bytes)) {
                        if (decodeImage(bytes, img)) {
                            found = true;
                            if (teamNames_.count(lowerStr(name))) magentaMask(img, mask, hasMask);
                        } else warn("texture '" + d + n + ext + "': unsupported image format");
                    }
                if (found) break;
            }
            if (found) break;
        }
        if (found) {
            Material m;
            m.name = name;
            m.roughness = 0.75f;
            img.name = name;
            m.baseTex = int(model_->images.size());
            bool transparent = false;
            for (size_t i = 3; i < img.rgba.size(); i += 4) transparent |= img.rgba[i] < 128;
            if (transparent) m.alpha = AlphaMode::Mask;
            model_->images.push_back(std::move(img));
            if (hasMask) {
                mask.name = name + "#team";
                m.team = TeamMode::Mask;
                m.teamTex = int(model_->images.size());
                model_->images.push_back(std::move(mask));
            }
            result = int(model_->materials.size());
            model_->materials.push_back(m);
        } else missing_.insert(name);
        texMats_[name] = result;
        return result;
    }

    // Spring's converted team textures: magenta (r == b, g == 0) = team colour of brightness r.
    static void magentaMask(Image& img, Image& mask, bool& hasMask) {
        mask.width = img.width; mask.height = img.height;
        mask.rgba.assign(img.rgba.size(), 0);
        for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
            mask.rgba[i + 3] = 255;
            uint8_t* p = &img.rgba[i];
            if (p[0] == p[2] && p[1] == 0 && p[0] > 0) {
                mask.rgba[i] = 255;
                p[1] = p[2] = p[0];
                hasMask = true;
            }
        }
    }

    // ---- pieces ----

    int loadPiece(const Bytes& in, uint32_t at, int parent) {
        Model& m = *model_;
        const int32_t numVerts = in.i32(at + 4), numPrims = in.i32(at + 8), selection = in.i32(at + 12);
        Node node;
        node.name = in.cstr(in.u32(at + 28), 64);
        const std::string where = "piece '" + node.name + "'";
        if (!in.has(in.u32(at + 28), 1)) throw std::runtime_error(path_ + ": " + where + " name offset outside the file");
        node.parent = parent;
        node.t = {float(in.i32(at + 16)) / 65536.0f, float(in.i32(at + 20)) / 65536.0f, -float(in.i32(at + 24)) / 65536.0f};
        if (numVerts < 0 || numPrims < 0) throw std::runtime_error(path_ + ": " + where + " has negative counts");
        const uint32_t vAt = in.u32(at + 36), pAt = in.u32(at + 40);
        in.need(vAt, size_t(numVerts) * 12, "vertex array");
        in.need(pAt, size_t(numPrims) * 32, "primitive array");
        std::vector<float3> pos(static_cast<size_t>(numVerts));
        for (size_t i = 0; i < pos.size(); ++i)
            pos[i] = {float(in.i32(vAt + i * 12)) / 65536.0f, float(in.i32(vAt + i * 12 + 4)) / 65536.0f, -float(in.i32(vAt + i * 12 + 8)) / 65536.0f};

        struct Poly { std::vector<uint16_t> idx; uint32_t color; std::string tex; };
        std::vector<Poly> polys;
        std::map<std::vector<uint16_t>, size_t> bySet;   // duplicate faces: the last one wins
        for (int32_t pi = 0; pi < numPrims; ++pi) {
            if (parent < 0 && pi == selection) continue;
            const size_t p = pAt + size_t(pi) * 32;
            const int32_t n = in.i32(p + 4);
            if (n < 3) continue;
            if (n > 1024) throw std::runtime_error(path_ + ": " + where + " primitive " + std::to_string(pi) + " has " + std::to_string(n) + " vertices");
            const uint32_t iAt = in.u32(p + 12), texAt = in.u32(p + 16);
            in.need(iAt, size_t(n) * 2, "vertex index array");
            Poly poly;
            poly.color = in.u32(p);
            for (int32_t k = 0; k < n; ++k) {
                const uint16_t v = in.u16(iAt + size_t(k) * 2);
                if (v >= numVerts) throw std::runtime_error(path_ + ": " + where + " primitive " + std::to_string(pi) + " vertex index " + std::to_string(v) + " out of range (" + std::to_string(numVerts) + " vertices)");
                poly.idx.push_back(v);
            }
            if (texAt) {
                if (!in.has(texAt, 1)) throw std::runtime_error(path_ + ": " + where + " texture name offset outside the file");
                poly.tex = in.cstr(texAt, 64);
            }
            std::vector<uint16_t> key = poly.idx;
            std::sort(key.begin(), key.end());
            auto [it, inserted] = bySet.emplace(key, polys.size());
            if (inserted) polys.push_back(std::move(poly));
            else polys[it->second] = std::move(poly);
        }

        // triangles per material, contiguous vertices per primitive
        std::map<int, std::vector<ModelVertex>> byMat;
        for (const Poly& poly : polys) {
            int mat = poly.tex.empty() ? -1 : textureMaterial(poly.tex);
            const bool textured = mat >= 0;
            if (!textured) mat = paletteMaterial();
            const size_t n = poly.idx.size();
            float3 nrm{0, 0, 0};
            for (size_t i = 2; i < n; ++i) nrm += cross(pos[poly.idx[i]] - pos[poly.idx[0]], pos[poly.idx[i - 1]] - pos[poly.idx[0]]);
            if (dot(nrm, nrm) < 1e-20f) continue;   // degenerate (collinear) polygon
            nrm = normalize(nrm);
            static const float2 kQuad[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            auto corner = [&](size_t k) {
                ModelVertex v;
                v.pos = pos[poly.idx[k]];
                v.nrm = nrm;
                v.uv = n <= 4 ? kQuad[k] : float2{0, 0};
                v.color = textured ? 0xffffffffu : paletteColor(poly.color);
                return v;
            };
            auto& tris = byMat[mat];
            for (size_t i = 2; i < n; ++i) { tris.push_back(corner(0)); tris.push_back(corner(i)); tris.push_back(corner(i - 1)); }
        }
        if (!byMat.empty()) {
            MeshDef md;
            md.name = node.name;
            for (auto& [mat, verts] : byMat) {
                Primitive prim;
                prim.material = mat;
                prim.firstIndex = uint32_t(m.indices.size());
                prim.indexCount = uint32_t(verts.size());
                prim.firstVertex = uint32_t(m.vertices.size());
                prim.vertexCount = uint32_t(verts.size());
                for (auto& v : verts) { m.indices.push_back(uint32_t(m.vertices.size())); m.vertices.push_back(v); }
                md.primitives.push_back(std::move(prim));
            }
            node.mesh = int(m.meshes.size());
            m.meshes.push_back(std::move(md));
        }
        m.nodes.push_back(std::move(node));
        return int(m.nodes.size()) - 1;
    }

    std::string path_, dir_;
    const FileReader& read_;
    std::vector<std::string>* warnings_;
    std::unique_ptr<Model> model_;
    std::vector<uint32_t> palette_;          // sRGB RGBA8
    std::set<std::string> teamNames_, missing_;
    std::map<std::string, int> texMats_;
    int paletteMat_ = -1;
};

} // namespace

std::unique_ptr<Model> load3do(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings) {
    Ta3doLoader l(path, read, warnings);
    return l.load(bytes);
}

} // namespace rts::render
