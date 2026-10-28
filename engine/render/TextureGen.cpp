#include "TextureGen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace rts::render {

namespace {

struct Rgb { float r = 0, g = 0, b = 0; };
Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Rgb scale(Rgb a, float k) { return {a.r * k, a.g * k, a.b * k}; }
float clamp01(float x) { return std::min(1.0f, std::max(0.0f, x)); }
float smooth(float e0, float e1, float x) { float t = clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }
float fract(float x) { return x - std::floor(x); }

uint32_t hash3(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float rand01(int x, int y, uint32_t seed) { return float(hash3(x, y, seed) & 0xffffffu) / float(0xffffff); }
int wrap(int i, int p) { return ((i % p) + p) % p; }

// value noise on a lattice of period p (tileable over u, v in [0, 1))
float vnoise(float u, float v, int p, uint32_t seed) {
    const float x = u * float(p), y = v * float(p);
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    auto at = [&](int i, int j) { return rand01(wrap(i, p), wrap(j, p), seed); };
    const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}
float fbm(float u, float v, int p, int octaves, uint32_t seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; ++o) {
        sum += vnoise(u, v, p << o, seed + uint32_t(o) * 101u) * amp;
        norm += amp;
        amp *= 0.5f;
    }
    return sum / norm;
}

struct Spec {
    std::string pattern = "noise", output = "color";
    int size = 256;
    std::vector<Rgb> colors;
    float scale = 8, width = 0.5f, variation = 0.15f, strength = 2, angle = 0, roughness = 0.5f;
    int octaves = 4;
    uint32_t seed = 1;
};

// One sample: mix factor between colours (or a colour index for camo), relief height 0..1, and a
// brightness multiplier (per-cell / per-plank variation).
struct Sample { float t = 0; int index = -1; float h = 0; float shade = 1; };

using PatternFn = Sample (*)(const Spec&, float u, float v);

Sample patSolid(const Spec&, float, float) { return {}; }

Sample patNoise(const Spec& s, float u, float v) {
    const float n = fbm(u, v, std::max(1, int(s.scale)), s.octaves, s.seed);
    return {smooth(0.5f - s.width * 0.5f, 0.5f + s.width * 0.5f, n), -1, n, 1};
}

Sample patStripes(const Spec& s, float u, float v) {
    const int n = std::max(1, int(s.scale));
    float c = s.angle >= 67.5f ? v : s.angle >= 22.5f ? (u + v) : u;   // 0, 45 or 90 degrees stay tileable
    const float f = fract(c * float(n));
    const float edge = 0.5f / float(s.size) * float(n) * 2;
    const float t = smooth(s.width - edge, s.width + edge, f) * (1 - smooth(1 - edge, 1, f));
    return {1 - t, -1, 1 - t, 1};
}

Sample patChecker(const Spec& s, float u, float v) {
    const int n = std::max(1, int(s.scale));
    const int ix = int(std::floor(u * float(n))), iy = int(std::floor(v * float(n)));
    const float t = float((ix + iy) & 1);
    return {t, -1, t, 1 + (rand01(ix, iy, s.seed) - 0.5f) * s.variation};
}

// bricks (offset rows) and tiles (aligned) share the grid code; mortar = second colour
Sample bricksOrTiles(const Spec& s, float u, float v, bool offset) {
    const int rows = std::max(1, int(s.scale)), cols = offset ? std::max(1, rows / 2) : rows;
    const float y = v * float(rows);
    const int row = int(std::floor(y));
    const float x = u * float(cols) + (offset && (row & 1) ? 0.5f : 0.0f);
    const int col = int(std::floor(x));
    const float fx = fract(x), fy = fract(y);
    const float mortar = std::max(0.02f, s.width * 0.15f);
    const float dx = std::min(fx, 1 - fx) * float(rows) / float(cols), dy = std::min(fy, 1 - fy);
    const float d = std::min(dx, dy);
    const float t = 1 - smooth(mortar * 0.6f, mortar, d);
    const float bevel = smooth(mortar, mortar * 2.5f, d);
    const float grain = (fbm(u, v, rows * 4, 3, s.seed + 7) - 0.5f) * 0.12f;
    return {t, -1, bevel * 0.85f + grain, 1 + (rand01(wrap(col, cols), row, s.seed) - 0.5f) * s.variation * 2 + grain};
}
Sample patBrick(const Spec& s, float u, float v) { return bricksOrTiles(s, u, v, true); }
Sample patTiles(const Spec& s, float u, float v) { return bricksOrTiles(s, u, v, false); }

// metal panels: a grid where some cells merge, seams (second colour) and rivets near corners
Sample patPanels(const Spec& s, float u, float v) {
    const int n = std::max(1, int(s.scale));
    const float x = u * float(n), y = v * float(n);
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = fract(x), fy = fract(y);
    // merge pairs of cells horizontally / vertically for larger plates
    const bool wide = rand01(ix / 2, iy, s.seed + 3) > 0.5f, tall = !wide && rand01(ix, iy / 2, s.seed + 5) > 0.6f;
    if (wide) fx = (fx + float(ix & 1)) * 0.5f;
    if (tall) fy = (fy + float(iy & 1)) * 0.5f;
    const float sx = wide ? 0.5f : 1.0f, sy = tall ? 0.5f : 1.0f;
    const float d = std::min(std::min(fx, 1 - fx) / sx, std::min(fy, 1 - fy) / sy);
    const float seam = std::max(0.015f, s.width * 0.06f);
    const float t = 1 - smooth(seam * 0.5f, seam, d);
    float rivet = 0;
    for (float cx : {0.08f * sx, 1 - 0.08f * sx})
        for (float cy : {0.08f * sy, 1 - 0.08f * sy}) {
            const float r = std::hypot((fx - cx) / sx, (fy - cy) / sy);
            rivet = std::max(rivet, 1 - smooth(0.02f, 0.035f, r));
        }
    const int key = wide ? ix / 2 : ix, keyY = tall ? iy / 2 : iy;
    const float scratches = (fbm(u, v, n * 8, 2, s.seed + 11) - 0.5f) * 0.08f;
    return {t, -1, (1 - t) * 0.7f + rivet * 0.3f + scratches, 1 + (rand01(key, keyY, s.seed) - 0.5f) * s.variation + rivet * 0.25f};
}

// brushed metal: fine streaks along u
Sample patMetal(const Spec& s, float u, float v) {
    const int n = std::max(1, int(s.scale));
    const float streak = fbm(u * 0.0625f, v, n * 16, 3, s.seed) * 0.7f + fbm(u, v, n * 2, 2, s.seed + 9) * 0.3f;
    return {0, -1, streak * 0.3f, 1 + (streak - 0.5f) * s.variation * 2};
}

// planks with rings and grain; second colour = dark grain
Sample patWood(const Spec& s, float u, float v) {
    const int planks = std::max(1, int(s.scale) / 2);
    const float y = v * float(planks);
    const int plank = int(std::floor(y));
    const float fy = fract(y);
    const float warp = fbm(u, v, 4, 3, s.seed + uint32_t(plank)) * 3;
    const float rings = fract((fy + warp) * 6 + rand01(plank, 0, s.seed) * 4);
    const float grain = fbm(u * 0.125f, v, 64, 2, s.seed + 21);
    const float t = clamp01(smooth(0.6f, 1.0f, rings) * 0.7f + (grain - 0.5f) * 0.6f);
    const float gap = smooth(0.0f, 0.04f, std::min(fy, 1 - fy));
    return {t, -1, gap * (0.8f - t * 0.2f), (0.75f + 0.25f * gap) * (1 + (rand01(plank, 1, s.seed) - 0.5f) * s.variation * 2)};
}

// camouflage: blobs from thresholded noise, one colour per band (up to the number of colours)
Sample patCamo(const Spec& s, float u, float v) {
    const float n = fbm(u, v, std::max(1, int(s.scale) / 2), 3, s.seed);
    const int k = std::max(2, int(s.colors.size()));
    const float lo = 0.3f, hi = 0.7f;
    const int idx = std::clamp(int((n - lo) / (hi - lo) * float(k)), 0, k - 1);
    return {0, idx, float(idx) / float(k - 1), 1};
}

Sample patGradient(const Spec&, float, float v) { return {v, -1, v, 1}; }

Sample patDots(const Spec& s, float u, float v) {
    const int n = std::max(1, int(s.scale));
    const float fx = fract(u * float(n)) - 0.5f, fy = fract(v * float(n)) - 0.5f;
    const float r = std::hypot(fx, fy), rad = clamp01(s.width) * 0.5f;
    const float t = 1 - smooth(rad - 0.03f, rad, r);
    return {t, -1, t * std::sqrt(std::max(0.0f, 1 - (r / std::max(rad, 1e-3f)) * (r / std::max(rad, 1e-3f)))), 1};
}

// offset-row lattice (every other row shifted half a cell): nearest and second-nearest centre
struct Cells { float d1 = 1e9f, d2 = 1e9f; int i = 0, j = 0; float dy = 0; };
Cells offsetCells(float x, float y, float yScale) {
    Cells c;
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            const int j = int(std::floor(y)) + dj, i = int(std::floor(x)) + di;
            const float cx = float(i) + ((j & 1) ? 1.0f : 0.5f), cy = float(j) + 0.5f;
            const float d = std::hypot(x - cx, (y - cy) * yScale);
            if (d < c.d1) { c.d2 = c.d1; c.d1 = d; c.i = i; c.j = j; c.dy = y - cy; }
            else if (d < c.d2) c.d2 = d;
        }
    return c;
}

// hexagon cells (Voronoi of the offset lattice); rows rounded to an even count so it tiles
Sample patHex(const Spec& s, float u, float v) {
    const int n = std::max(2, int(s.scale));
    const int rows = std::max(2, int(std::lround(float(n) * 1.1547f / 2)) * 2);
    const Cells c = offsetCells(u * float(n), v * float(rows), 0.866f * float(rows) / (float(n) * 1.1547f));
    const float edge = (c.d2 - c.d1) * 0.5f;
    const float line = 0.02f + s.width * 0.08f;
    const float t = 1 - smooth(line * 0.5f, line, edge);
    return {t, -1, smooth(line, line * 3, edge) * 0.8f, 1 + (rand01(wrap(c.i, n), wrap(c.j, rows), s.seed) - 0.5f) * s.variation};
}

// overlapping round scales (dragons, armour): rows further down cover the ones above
Sample patScales(const Spec& s, float u, float v) {
    const int n = std::max(1, int(s.scale)), rows = n * 2;
    const float x = u * float(n), y = v * float(rows);
    const float R = 0.62f;
    int bi = 0, bj = -1000000;
    float bd = 1;
    for (int dj = -2; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            const int j = int(std::floor(y)) + dj, i = int(std::floor(x)) + di;
            const float cx = float(i) + ((j & 1) ? 1.0f : 0.5f), cy = float(j);
            const float d = std::hypot(x - cx, (y - cy) * 0.5f);
            if (d < R && (j > bj || (j == bj && d < bd))) { bj = j; bi = i; bd = d; }
        }
    if (bj == -1000000) return {1, -1, 0, 1};
    const float rim = smooth(R * 0.82f, R, bd);
    return {rim, -1, 1 - bd / R, 1 + (rand01(wrap(bi, n), wrap(bj, rows), s.seed) - 0.5f) * s.variation * 2};
}

struct Pattern { const char* name; PatternFn fn; const char* help; std::vector<Rgb> colors; };
const Pattern kPatterns[] = {
    {"solid", patSolid, "one colour (colors[0])", {{0.6f, 0.6f, 0.6f}}},
    {"noise", patNoise, "cloudy noise between two colours; scale = blob count, width = contrast 0..1, octaves", {{0.35f, 0.33f, 0.3f}, {0.6f, 0.57f, 0.52f}}},
    {"stripes", patStripes, "stripes; scale = count, width = share of the first colour, angle 0 / 45 / 90", {{0.85f, 0.75f, 0.1f}, {0.1f, 0.1f, 0.1f}}},
    {"checker", patChecker, "checkerboard; scale = squares per side, variation", {{0.8f, 0.8f, 0.8f}, {0.25f, 0.25f, 0.25f}}},
    {"brick", patBrick, "bricks in offset rows; colors = [brick, mortar], scale = rows, width = mortar, variation", {{0.55f, 0.27f, 0.2f}, {0.78f, 0.75f, 0.7f}}},
    {"tiles", patTiles, "square tiles; colors = [tile, grout], scale = tiles per side, width = grout, variation", {{0.75f, 0.72f, 0.66f}, {0.35f, 0.33f, 0.3f}}},
    {"panels", patPanels, "riveted metal plates (vehicles, sci-fi); colors = [plate, seam], scale = grid, width = seam", {{0.45f, 0.48f, 0.5f}, {0.15f, 0.16f, 0.17f}}},
    {"metal", patMetal, "brushed metal streaks; colors[0], scale, variation = streak strength", {{0.62f, 0.63f, 0.65f}}},
    {"wood", patWood, "planks with rings; colors = [wood, grain], scale = 2 x planks", {{0.6f, 0.42f, 0.25f}, {0.35f, 0.22f, 0.12f}}},
    {"camo", patCamo, "camouflage blobs, one band per colour (2..4 colours); scale = blob size", {{0.33f, 0.36f, 0.22f}, {0.48f, 0.45f, 0.3f}, {0.2f, 0.2f, 0.14f}}},
    {"gradient", patGradient, "vertical gradient colors[0] (top) -> colors[1] (bottom); not tileable vertically", {{0.9f, 0.9f, 0.9f}, {0.3f, 0.3f, 0.3f}}},
    {"dots", patDots, "dots on a grid; scale = dots per side, width = dot size 0..1", {{0.9f, 0.9f, 0.9f}, {0.2f, 0.2f, 0.25f}}},
    {"hex", patHex, "hexagon cells (shields, sci-fi floors); scale = cells, width = line", {{0.3f, 0.5f, 0.7f}, {0.1f, 0.15f, 0.2f}}},
    {"scales", patScales, "overlapping round scales (dragons, armour); scale = scales per row, variation", {{0.25f, 0.45f, 0.3f}, {0.08f, 0.12f, 0.08f}}},
};

const Pattern* findPattern(const std::string& name) {
    for (const auto& p : kPatterns) if (name == p.name) return &p;
    return nullptr;
}

Rgb colorOf(const Json& j, Rgb def) {
    if (j.size() < 3) return def;
    return {float(j[size_t(0)].toDouble(def.r)), float(j[size_t(1)].toDouble(def.g)), float(j[size_t(2)].toDouble(def.b))};
}
uint8_t q(float x) { return uint8_t(std::lround(clamp01(x) * 255.0f)); }

} // namespace

std::vector<std::string> texturePatterns() {
    std::vector<std::string> out;
    for (const auto& p : kPatterns) out.push_back(p.name);
    return out;
}

std::string texturePatternHelp() {
    std::string out;
    for (const auto& p : kPatterns) out += std::string(p.name) + ": " + p.help + "\n";
    return out;
}

void generateTexture(const Json& j, Image& out) {
    Spec s;
    s.pattern = j.getStr("pattern", "noise");
    const Pattern* pat = findPattern(s.pattern);
    if (!pat) {
        std::string list;
        for (const auto& n : texturePatterns()) list += (list.empty() ? "" : ", ") + n;
        throw std::runtime_error("unknown pattern '" + s.pattern + "' (" + list + ")");
    }
    s.output = j.getStr("output", "color");
    if (s.output != "color" && s.output != "normal" && s.output != "mask") throw std::runtime_error("output must be color, normal or mask");
    s.size = int(j.getInt("size", 256));
    if (s.size < 8 || s.size > 2048) throw std::runtime_error("size must be 8..2048");
    s.scale = float(j["scale"].toDouble(8));
    s.width = float(j["width"].toDouble(0.5));
    s.variation = float(j["variation"].toDouble(0.15));
    s.strength = float(j["strength"].toDouble(2));
    s.angle = float(j["angle"].toDouble(0));
    s.octaves = std::clamp(int(j.getInt("octaves", 4)), 1, 8);
    s.seed = uint32_t(j.getInt("seed", 1));
    s.colors = pat->colors;
    if (const Json& c = j["colors"]; c.isArray() && c.size() > 0) {
        s.colors.clear();
        for (size_t i = 0; i < c.size() && i < 8; ++i) s.colors.push_back(colorOf(c[i], {0.5f, 0.5f, 0.5f}));
    }
    while (s.colors.size() < 2) s.colors.push_back(s.colors.empty() ? Rgb{0.5f, 0.5f, 0.5f} : scale(s.colors[0], 0.5f));

    const int n = s.size;
    std::vector<Sample> px(size_t(n) * size_t(n));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            px[size_t(y) * size_t(n) + size_t(x)] = pat->fn(s, (float(x) + 0.5f) / float(n), (float(y) + 0.5f) / float(n));

    out.width = out.height = n;
    out.rgba.assign(size_t(n) * size_t(n) * 4, 255);
    out.name = "generated:" + s.pattern;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const Sample& p = px[size_t(y) * size_t(n) + size_t(x)];
            uint8_t* o = &out.rgba[(size_t(y) * size_t(n) + size_t(x)) * 4];
            if (s.output == "mask") {
                const float m = p.index >= 0 ? (p.index == 1 ? 1.0f : 0.0f) : p.t;
                o[0] = o[1] = o[2] = q(m);
            } else if (s.output == "normal") {
                auto h = [&](int xx, int yy) { return px[size_t(wrap(yy, n)) * size_t(n) + size_t(wrap(xx, n))].h; };
                const float k = s.strength * float(n) / 256.0f;
                const float dx = (h(x + 1, y) - h(x - 1, y)) * k, dy = (h(x, y + 1) - h(x, y - 1)) * k;
                const float len = std::sqrt(dx * dx + dy * dy + 1);
                o[0] = q(-dx / len * 0.5f + 0.5f);
                o[1] = q(dy / len * 0.5f + 0.5f);   // +Y up in the texture (glTF tangent space)
                o[2] = q(1 / len * 0.5f + 0.5f);
            } else {
                Rgb c = p.index >= 0 ? s.colors[size_t(std::min<int>(p.index, int(s.colors.size()) - 1))] : mix(s.colors[0], s.colors[1], clamp01(p.t));
                c = scale(c, std::max(0.0f, p.shade));
                o[0] = q(c.r); o[1] = q(c.g); o[2] = q(c.b);
            }
        }
}

} // namespace rts::render
