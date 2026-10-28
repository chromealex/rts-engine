#include "TerrainOps.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include "modules/rts/view/RtsTerrainSurface.h"

// Private copies (static functions) of stb_image (16-bit PNG decode) and stb_image_write (zlib for
// the 16-bit PNG encoder), so this file links next to the other copies.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace rts::editor {

namespace {
constexpr float kUnit = 16.0f;            // corner values are 1/16 world units
Json num(int64_t v) { return Json::number(v); }
Json str(const std::string& s) { return Json::string(s); }
int16_t toCorner(float h) { return int16_t(std::clamp(std::lround(h * kUnit), -32768l, 32767l)); }

// The visual surface of the current map data (levels mode), as the view computes it.
game::MapGrid gridOf(const MapData& d) {
    game::MapGrid g;
    g.init(d.w, d.h);
    for (int i = 0; i < d.w * d.h; ++i) {
        g.flags[size_t(i)] = game::cellFlagsFromChar(d.cells[size_t(i)]);
        g.level[size_t(i)] = d.levels[size_t(i)];
        g.height[size_t(i)] = d.heights[size_t(i)];
    }
    return g;
}

uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float valueNoise(float x, float y, uint32_t seed) {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - float(xi), fy = y - float(yi);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    auto v = [&](int a, int b) { return float(hash2(a, b, seed) & 0xffff) / 65535.0f * 2.0f - 1.0f; };
    float a = v(xi, yi) + (v(xi + 1, yi) - v(xi, yi)) * fx;
    float b = v(xi, yi + 1) + (v(xi + 1, yi + 1) - v(xi, yi + 1)) * fx;
    return a + (b - a) * fy;
}
} // namespace

bool parseSculpt(const std::string& s, Sculpt& out) {
    static const std::pair<const char*, Sculpt> k[] = {{"raise", Sculpt::Raise}, {"lower", Sculpt::Lower}, {"smooth", Sculpt::Smooth},
                                                       {"flatten", Sculpt::Flatten}, {"noise", Sculpt::Noise}, {"set", Sculpt::Set}};
    for (auto& [n, v] : k) if (s == n) { out = v; return true; }
    return false;
}
const char* sculptName(Sculpt s) {
    switch (s) {
        case Sculpt::Raise: return "raise"; case Sculpt::Lower: return "lower"; case Sculpt::Smooth: return "smooth";
        case Sculpt::Flatten: return "flatten"; case Sculpt::Noise: return "noise"; default: return "set";
    }
}

void ensureCorners(MapDoc& doc, float lh) {
    MapData& d = doc.data;
    const size_t n = size_t((d.w + 1) * (d.h + 1));
    if (d.corners.size() == n) return;
    game::MapGrid g = gridOf(d);
    d.corners.assign(n, 0);
    for (int Y = 0; Y <= d.h; ++Y)
        for (int X = 0; X <= d.w; ++X) d.corners[size_t(Y * (d.w + 1) + X)] = toCorner(game::terrain::gridCorner(g, lh, X, Y));
}

CornerRect sculpt(MapDoc& doc, float lh, Sculpt op, float x, float z, float radius, float strength, uint32_t seed) {
    ensureCorners(doc, lh);
    MapData& d = doc.data;
    const int W = d.w + 1;
    radius = std::max(radius, 0.5f);
    CornerRect r;
    r.x0 = std::max(0, int(std::floor(x - radius))); r.x1 = std::min(d.w, int(std::ceil(x + radius)));
    r.y0 = std::max(0, int(std::floor(z - radius))); r.y1 = std::min(d.h, int(std::ceil(z + radius)));
    if (r.x0 > r.x1 || r.y0 > r.y1) return r;
    auto at = [&](int X, int Y) -> int16_t& { return d.corners[size_t(std::clamp(Y, 0, d.h) * W + std::clamp(X, 0, d.w))]; };
    // flatten target: weighted mean under the brush; smooth: 3x3 mean of the original values
    float target = 0, wsum = 0;
    std::vector<int16_t> orig;
    if (op == Sculpt::Flatten || op == Sculpt::Smooth) {
        orig.reserve(size_t((r.x1 - r.x0 + 3) * (r.y1 - r.y0 + 3)));
        for (int Y = r.y0; Y <= r.y1; ++Y)
            for (int X = r.x0; X <= r.x1; ++X) {
                float dd = std::hypot(float(X) - x, float(Y) - z);
                if (dd > radius) continue;
                float w = 1 - dd / radius;
                target += float(at(X, Y)) * w; wsum += w;
            }
        target = wsum > 0 ? target / wsum : 0;
    }
    std::vector<int16_t> snapshot;
    if (op == Sculpt::Smooth) {
        for (int Y = r.y0 - 1; Y <= r.y1 + 1; ++Y) for (int X = r.x0 - 1; X <= r.x1 + 1; ++X) snapshot.push_back(at(X, Y));
    }
    const int sw = r.x1 - r.x0 + 3;
    auto snap = [&](int X, int Y) { return float(snapshot[size_t((Y - r.y0 + 1) * sw + (X - r.x0 + 1))]); };
    for (int Y = r.y0; Y <= r.y1; ++Y)
        for (int X = r.x0; X <= r.x1; ++X) {
            float dd = std::hypot(float(X) - x, float(Y) - z);
            if (dd > radius) continue;
            float t = 1 - dd / radius;
            float k = t * t * (3 - 2 * t);       // smooth falloff
            float h = float(at(X, Y));
            switch (op) {
                case Sculpt::Raise: h += strength * kUnit * k; break;
                case Sculpt::Lower: h -= strength * kUnit * k; break;
                case Sculpt::Noise: {
                    float nv = valueNoise(float(X) * 0.35f, float(Y) * 0.35f, seed) * 0.65f + valueNoise(float(X) * 0.9f, float(Y) * 0.9f, seed + 7) * 0.35f;
                    h += strength * kUnit * k * nv;
                    break;
                }
                case Sculpt::Set: h += (strength * kUnit - h) * k; break;
                case Sculpt::Flatten: h += (target - h) * k * std::clamp(strength, 0.0f, 1.0f); break;
                case Sculpt::Smooth: {
                    float m = 0;
                    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) m += snap(X + dx, Y + dy);
                    h += (m / 9.0f - h) * k * std::clamp(strength, 0.0f, 1.0f);
                    break;
                }
            }
            at(X, Y) = int16_t(std::clamp(std::lround(h), -32768l, 32767l));
        }
    doc.touched();
    return r;
}

std::vector<uint8_t> materialLayer(const MapData& d) {
    const Json& m = d.extra["terrainMaterial"];
    if (!m.isArray()) return {};
    std::vector<uint8_t> out(size_t(d.w * d.h), 255);
    for (size_t y = 0; y < m.size() && int(y) < d.h; ++y) {
        std::string row = m[y].str();
        for (size_t x = 0; x < row.size() && int(x) < d.w; ++x)
            if (row[x] >= '0' && row[x] <= '3') out[y * size_t(d.w) + x] = uint8_t(row[x] - '0');
    }
    return out;
}

void paintMaterial(MapDoc& doc, int cx, int cy, int radius, int layer) {
    MapData& d = doc.data;
    std::vector<uint8_t> m = materialLayer(d);
    if (m.empty()) m.assign(size_t(d.w * d.h), 255);
    for (int y = cy - radius; y <= cy + radius; ++y)
        for (int x = cx - radius; x <= cx + radius; ++x)
            if (d.inside(x, y) && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius + radius)
                m[d.idx(x, y)] = layer < 0 || layer > 3 ? 255 : uint8_t(layer);
    bool any = std::any_of(m.begin(), m.end(), [](uint8_t v) { return v != 255; });
    // top-level map key (round-trips through MapDoc::extra; "terrain" itself is the structured block)
    if (!any) d.extra.erase("terrainMaterial");
    else {
        Json rows = Json::array();
        for (int y = 0; y < d.h; ++y) {
            std::string row(size_t(d.w), '.');
            for (int x = 0; x < d.w; ++x) if (m[d.idx(x, y)] != 255) row[size_t(x)] = char('0' + m[d.idx(x, y)]);
            rows.push(str(row));
        }
        d.extra.set("terrainMaterial", rows);
    }
    doc.touched();
}

bool exportHeightmap(const MapData& d, const std::string& file, float& scale, float& offset, std::string* err) {
    if (d.corners.size() != size_t((d.w + 1) * (d.h + 1))) { if (err) *err = "the map has no heightfield (terrain_sculpt creates one)"; return false; }
    auto [mn, mx] = std::minmax_element(d.corners.begin(), d.corners.end());
    offset = float(*mn) / kUnit;
    if (scale <= 0) scale = std::max(1.0f, float(*mx - *mn) / kUnit);
    const int W = d.w + 1, H = d.h + 1;
    // PNG: 16-bit grayscale, rows big-endian, filter 0
    std::vector<unsigned char> raw;
    raw.reserve(size_t(H) * size_t(W * 2 + 1));
    for (int y = 0; y < H; ++y) {
        raw.push_back(0);
        for (int x = 0; x < W; ++x) {
            float h = float(d.corners[size_t(y * W + x)]) / kUnit;
            int v = int(std::clamp(std::lround((h - offset) / scale * 65535.0f), 0l, 65535l));
            raw.push_back(uint8_t(v >> 8)); raw.push_back(uint8_t(v & 255));
        }
    }
    int zlen = 0;
    unsigned char* z = stbi_zlib_compress(raw.data(), int(raw.size()), &zlen, 8);
    if (!z) { if (err) *err = "png: compression failed"; return false; }
    auto be32 = [](std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back(char((v >> (i * 8)) & 255)); };
    auto chunk = [&](std::string& out, const char* type, const std::string& body) {
        be32(out, uint32_t(body.size()));
        std::string tb = std::string(type, 4) + body;
        out += tb;
        be32(out, stbiw__crc32(reinterpret_cast<unsigned char*>(tb.data()), int(tb.size())));
    };
    std::string png("\x89PNG\r\n\x1a\n", 8), ihdr;
    be32(ihdr, uint32_t(W)); be32(ihdr, uint32_t(H));
    ihdr += std::string("\x10\x00\x00\x00\x00", 5); // depth 16, grayscale, deflate, filter 0, no interlace
    chunk(png, "IHDR", ihdr);
    // tEXt: how to read the values back
    chunk(png, "tEXt", std::string("rtsengine") + '\0' + "scale=" + std::to_string(scale) + " offset=" + std::to_string(offset));
    chunk(png, "IDAT", std::string(reinterpret_cast<char*>(z), size_t(zlen)));
    STBIW_FREE(z);
    chunk(png, "IEND", "");
    std::ofstream f(file, std::ios::binary);
    if (!f || !f.write(png.data(), std::streamsize(png.size()))) { if (err) *err = "cannot write " + file; return false; }
    return true;
}

bool importHeightmap(MapDoc& doc, const std::string& file, float scale, float offset, std::string* err) {
    std::ifstream f(file, std::ios::binary);
    if (!f) { if (err) *err = "cannot read " + file; return false; }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int w = 0, h = 0, n = 0;
    stbi_us* px = stbi_load_16_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &w, &h, &n, 1);
    if (!px || w < 2 || h < 2) { if (px) stbi_image_free(px); if (err) *err = "not a PNG heightmap: " + file; return false; }
    MapData& d = doc.data;
    const int W = d.w + 1, H = d.h + 1;
    d.corners.assign(size_t(W * H), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            // bilinear resample of the image onto the corner grid
            float sx = float(x) * float(w - 1) / float(std::max(1, W - 1)), sy = float(y) * float(h - 1) / float(std::max(1, H - 1));
            int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
            float fx = sx - float(x0), fy = sy - float(y0);
            auto v = [&](int a, int b) { return float(px[size_t(b) * size_t(w) + size_t(a)]); };
            float val = (v(x0, y0) * (1 - fx) + v(x1, y0) * fx) * (1 - fy) + (v(x0, y1) * (1 - fx) + v(x1, y1) * fx) * fy;
            d.corners[size_t(y * W + x)] = toCorner(offset + val / 65535.0f * scale);
        }
    stbi_image_free(px);
    doc.touched();
    return true;
}

// ------------------------------------------------------------------ operations (MCP + editor)

void registerTerrainOps(Workspace& ws) {
    Workspace* w = &ws;
    auto lh = [w] { return float(w->project.rules.levelHeight.toDouble()); };
    auto needMap = [w] { if (w->project.dir.empty()) throw std::runtime_error("no game is open: call open_game first"); };
    auto add = [&ws](const char* name, const char* desc, const char* sch, Workspace::OpFn fn) { ws.registerOp({name, desc, Json::parse(sch), std::move(fn)}); };

    add("terrain_sculpt",
        "Sculpt the map's heightfield (corner heights; smooth hills and valleys). op: raise | lower (strength = world units at the centre), "
        "smooth | flatten (strength 0..1 blend), noise (strength = amplitude), set (strength = target height). x, y: centre in cells "
        "(fractions allowed), radius in cells; 'points' [[x,y],...] applies the same dab along a stroke. The first sculpt on a map without "
        "a heightfield creates it from the current surface (levels, ramps, fine heights). Visual only unless rules terrain.model is "
        "'heightfield' (then it is gameplay: slopes, line of sight). Undoable.",
        R"({"properties": {"op": {"type": "string", "enum": ["raise", "lower", "smooth", "flatten", "noise", "set"]},
            "x": {"type": "number"}, "y": {"type": "number"}, "radius": {"type": "number"}, "strength": {"type": "number"},
            "points": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}}, "seed": {"type": "integer"}},
            "required": ["op"]})",
        [w, lh, needMap](const Json& p) {
            needMap();
            Sculpt op = Sculpt::Raise;
            if (!parseSculpt(p.getStr("op"), op)) throw std::runtime_error("op must be raise, lower, smooth, flatten, noise or set");
            const float radius = float(p["radius"].toDouble(4)), strength = float(p["strength"].toDouble(op == Sculpt::Smooth || op == Sculpt::Flatten ? 0.5 : 1.0));
            std::vector<std::pair<float, float>> pts;
            if (p["points"].isArray()) for (auto& q : p["points"].items()) pts.push_back({float(q[0].toDouble()), float(q[1].toDouble())});
            else {
                if (!p.has("x") || !p.has("y")) throw std::runtime_error("expected x, y (or points)");
                pts.push_back({float(p["x"].toDouble()), float(p["y"].toDouble())});
            }
            w->map.beginEdit();
            const bool created = w->map.data.corners.empty();
            CornerRect all{1 << 30, 1 << 30, -1, -1};
            for (auto [x, z] : pts) {
                CornerRect r = sculpt(w->map, lh(), op, x, z, radius, strength, uint32_t(p.getInt("seed", 1)));
                all.x0 = std::min(all.x0, r.x0); all.y0 = std::min(all.y0, r.y0); all.x1 = std::max(all.x1, r.x1); all.y1 = std::max(all.y1, r.y1);
            }
            w->note({Change::Cells, all.x0, all.y0, all.x1 - 1, all.y1 - 1, -1, std::string("Рельеф: ") + sculptName(op)});
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            if (created) r.set("createdHeightfield", Json::fromBool(true));
            r.set("summary", str(std::string("Рельеф: ") + sculptName(op) + (created ? " (создан heightfield)" : "")));
            return r;
        });

    add("terrain_heights",
        "Read the visual surface height (world units) at points [[x,y],...] (or a w x h grid summary when 'grid': step in cells): min, "
        "max, mean. Uses the same surface as the renderer (heightfield corners, otherwise levels + ramps + fine heights).",
        R"({"properties": {"points": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}}, "grid": {"type": "integer"}}})",
        [w, lh, needMap](const Json& p) {
            needMap();
            const MapData& d = w->map.data;
            game::MapGrid g = gridOf(d);
            if (d.corners.size() == size_t((d.w + 1) * (d.h + 1))) g.corner = d.corners;
            Json r = Json::object();
            if (p["points"].isArray()) {
                Json hs = Json::array();
                for (auto& q : p["points"].items()) hs.push(Json::number(double(game::terrain::heightAt(g, lh(), float(q[0].toDouble()), float(q[1].toDouble()))), 2));
                r.set("heights", hs);
            }
            int step = int(p.getInt("grid", 0));
            if (step > 0 || !p["points"].isArray()) {
                step = std::max(1, step > 0 ? step : std::max(d.w, d.h) / 16);
                float mn = 1e9f, mx = -1e9f, sum = 0; int n = 0;
                Json rows = Json::array();
                for (int y = 0; y < d.h; y += step) {
                    Json row = Json::array();
                    for (int x = 0; x < d.w; x += step) {
                        float h = game::terrain::heightAt(g, lh(), float(x) + 0.5f, float(y) + 0.5f);
                        mn = std::min(mn, h); mx = std::max(mx, h); sum += h; ++n;
                        row.push(Json::number(double(h), 1));
                    }
                    rows.push(row);
                }
                r.set("step", num(step)); r.set("rows", rows);
                r.set("min", Json::number(double(mn), 2)); r.set("max", Json::number(double(mx), 2)); r.set("mean", Json::number(double(sum / float(std::max(n, 1))), 2));
            }
            r.set("heightfield", Json::fromBool(!d.corners.empty()));
            return r;
        });

    add("terrain_material",
        "Paint the terrain material layer (index 0..3 into the game's visual terrain.layers, see rules/game.json 'terrain'; -1 erases = "
        "by cell kind) on a circle x, y, radius. Visual only (map key terrainMaterial). Undoable.",
        R"({"properties": {"layer": {"type": "integer"}, "x": {"type": "integer"}, "y": {"type": "integer"}, "radius": {"type": "integer"}}, "required": ["layer", "x", "y"]})",
        [w, needMap](const Json& p) {
            needMap();
            int x = int(p.getInt("x")), y = int(p.getInt("y")), rad = int(p.getInt("radius", 2)), layer = int(p.getInt("layer", -1));
            w->map.beginEdit();
            paintMaterial(w->map, x, y, rad, layer);
            w->note({Change::Cells, x - rad, y - rad, x + rad, y + rad, -1, "Материал земли"});
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str(layer < 0 ? "Материал земли стёрт" : "Материал земли: слой " + std::to_string(layer)));
            return r;
        });

    add("heightmap_export",
        "Write the map's heightfield as a 16-bit grayscale PNG ((w+1) x (h+1) pixels) to 'file' (relative to the game folder). "
        "Returns scale (world units of the 0..65535 range) and offset (height of 0) needed to import it back.",
        R"({"properties": {"file": {"type": "string"}, "scale": {"type": "number"}}, "required": ["file"]})",
        [w, needMap](const Json& p) {
            needMap();
            float scale = float(p["scale"].toDouble(0)), offset = 0;
            std::string err, file = p.getStr("file");
            if (file.find("..") != std::string::npos) throw std::runtime_error("file must stay inside the game folder");
            if (!exportHeightmap(w->map.data, w->project.dir + "/" + file, scale, offset, &err)) throw std::runtime_error(err);
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("scale", Json::number(double(scale), 4)); r.set("offset", Json::number(double(offset), 4));
            r.set("summary", str("Карта высот сохранена: " + file));
            return r;
        });

    add("heightmap_import",
        "Replace the map's heightfield with a grayscale PNG (8 or 16 bit; resampled to (w+1) x (h+1) corners). height = offset + "
        "value / 65535 * scale (world units). 'file' is relative to the game folder. Undoable.",
        R"({"properties": {"file": {"type": "string"}, "scale": {"type": "number"}, "offset": {"type": "number"}}, "required": ["file"]})",
        [w, needMap](const Json& p) {
            needMap();
            std::string err, file = p.getStr("file");
            if (file.find("..") != std::string::npos) throw std::runtime_error("file must stay inside the game folder");
            w->map.beginEdit();
            if (!importHeightmap(w->map, w->project.dir + "/" + file, float(p["scale"].toDouble(8)), float(p["offset"].toDouble(0)), &err)) throw std::runtime_error(err);
            w->note({Change::Map, 0, 0, w->map.data.w - 1, w->map.data.h - 1, -1, "Карта высот"});
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str("Карта высот загружена: " + file));
            return r;
        });
}

} // namespace rts::editor
