// Sprite shadows, pure part (see RtsSpriteShadow.h).
#include "RtsSpriteShadow.h"
#include <algorithm>
#include <cmath>
#include "RtsTerrainSurface.h"

namespace rts::game {

using namespace render;

namespace {
float num(const Json& j, float def) { return j.isNumber() ? float(j.fx().toDouble()) : def; }
} // namespace

bool parseShadowMode(const Json& j, SpriteShadowMode& out) {
    if (j.isString()) {
        const std::string s = j.str();
        if (s == "cast") { out = SpriteShadowMode::Cast; return true; }
        if (s == "blob") { out = SpriteShadowMode::Blob; return true; }
        if (s == "none" || s == "off") { out = SpriteShadowMode::None; return true; }
        return false;
    }
    if (j.boolean(true) != j.boolean(false)) return false;   // not a bool
    out = j.boolean() ? SpriteShadowMode::Blob : SpriteShadowMode::None;
    return true;
}

SpriteDefaults parseSpriteDefaults(const Json& rulesSprites, const Json& gameSprites) {
    SpriteDefaults d;
    for (const Json* b : {&rulesSprites, &gameSprites}) {
        if (!b->isObject()) continue;
        parseShadowMode((*b)["shadow"], d.shadow);
        d.lit = b->getBool("lit", d.lit);
    }
    return d;
}

SpriteShadowSpec parseSpriteShadow(const Json& s, const SpriteDefaults& d, bool structure, bool ground,
                                   float width, float height, int footW, int footH) {
    SpriteShadowSpec r;
    r.lit = s.getBool("lit", d.lit);
    const Json& sh = s["shadow"];
    // missing: the game default; "blob" keeps the old rule (units only, never buildings)
    if (sh.isNull()) r.mode = d.shadow == SpriteShadowMode::Blob && structure ? SpriteShadowMode::None : d.shadow;
    else if (sh.isObject()) { r.mode = SpriteShadowMode::Blob; parseShadowMode(sh["mode"], r.mode); }
    else if (!parseShadowMode(sh, r.mode)) r.mode = d.shadow;
    if (ground) r.mode = SpriteShadowMode::None;   // flat on the ground: nothing to cast
    if (structure) { r.blobW = float(footW) * 1.3f; r.blobH = float(footH) * 1.3f; }
    else { r.blobW = width * 0.8f; r.blobH = r.blobW * 0.55f; }
    r.blobW = num(sh["size"][0], r.blobW);
    r.blobH = num(sh["size"][1], sh["size"][0].isNumber() ? r.blobW * 0.55f : r.blobH);
    r.blobAlpha = std::clamp(num(sh["alpha"], 0.35f), 0.0f, 1.0f);
    r.castHeight = std::max(0.05f, num(s["castHeight"], height));
    r.offsetRight = num(s["shadowOffset"][0], 0.0f);
    r.offsetFront = num(s["shadowOffset"][1], 0.0f);
    // buildings cast like a box: the drawn front plane plus a copy at the back of the footprint
    r.castDepth = std::max(0.0f, num(s["castDepth"], structure ? float(footH) : 0.0f));
    return r;
}

ShadowDraw shadowDrawFor(SpriteShadowMode mode, ShadowQuality q, bool structure) {
    ShadowDraw d;
    if (mode == SpriteShadowMode::None) return d;
    if (mode == SpriteShadowMode::Blob || q == ShadowQuality::Off || (q == ShadowQuality::Low && !structure)) { d.blobK = 1; return d; }
    d.cast = true;
    d.blobK = structure ? 0.0f : 0.5f;   // contact darkening under the feet stays
    return d;
}

void surfaceTriangles(const GroundSurface& s, float minX, float minZ, float maxX, float maxZ, std::vector<float3>& out) {
    if (!s.grid || s.grid->w == 0) return;
    const MapGrid& g = *s.grid;
    const int x0 = std::max(0, int(std::floor(minX))), z0 = std::max(0, int(std::floor(minZ)));
    const int x1 = std::min(g.w - 1, int(std::floor(maxX))), z1 = std::min(g.h - 1, int(std::floor(maxZ)));
    for (int y = z0; y <= z1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float fx = float(x), fz = float(y);
            float h00, h10, h01, h11;
            bool mainDiagonal;   // (0,0)-(1,1) split, else (1,0)-(0,1)
            if (s.tiles) {       // RtsTileView: flat cells
                const size_t i = size_t(g.index(x, y));
                h00 = h10 = h01 = h11 = float(g.level[i]) * s.levelHeight + float(g.height[i]) / 16.0f;
                mainDiagonal = true;
            } else if (g.hasCorners()) {   // heightfield mesh
                h00 = terrain::cornerHeight(g, x, y); h10 = terrain::cornerHeight(g, x + 1, y);
                h01 = terrain::cornerHeight(g, x, y + 1); h11 = terrain::cornerHeight(g, x + 1, y + 1);
                mainDiagonal = true;
            } else {                       // levels mesh: per-cell top quads, split (1,0)-(0,1)
                h00 = terrain::cellCorner(g, s.levelHeight, x, y, 0, 0); h10 = terrain::cellCorner(g, s.levelHeight, x, y, 1, 0);
                h01 = terrain::cellCorner(g, s.levelHeight, x, y, 0, 1); h11 = terrain::cellCorner(g, s.levelHeight, x, y, 1, 1);
                mainDiagonal = false;
            }
            const float3 p00{fx, h00, fz}, p10{fx + 1, h10, fz}, p01{fx, h01, fz + 1}, p11{fx + 1, h11, fz + 1};
            if (mainDiagonal) out.insert(out.end(), {p00, p10, p11, p00, p11, p01});
            else out.insert(out.end(), {p00, p10, p01, p10, p11, p01});
        }
}

int clipToRect(const float3 (&tri)[3], float3 o, float3 aU, float lenU, float3 aV, float lenV, float3 (&out)[8]) {
    float3 a[8], b[8];
    int n = 3;
    for (int i = 0; i < 3; ++i) a[i] = tri[i];
    // four half planes: d(p) >= 0
    auto plane = [&](int k, float3 p) {
        float3 d{p.x - o.x, 0, p.z - o.z};
        switch (k) {
        case 0: return dot(d, aU);
        case 1: return lenU - dot(d, aU);
        case 2: return dot(d, aV);
        default: return lenV - dot(d, aV);
        }
    };
    float3* src = a;
    float3* dst = b;
    for (int k = 0; k < 4 && n > 0; ++k) {
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const float3 p = src[i], q = src[(i + 1) % n];
            const float dp = plane(k, p), dq = plane(k, q);
            if (dp >= 0 && m < 8) dst[m++] = p;
            if ((dp >= 0) != (dq >= 0) && m < 8) {
                const float t = dp / (dp - dq);
                dst[m++] = p + (q - p) * t;
            }
        }
        n = m;
        std::swap(src, dst);
    }
    if (n < 3) return 0;
    for (int i = 0; i < n; ++i) out[i] = src[i];
    return n;
}

float3 casterRight(float3 gRight, float3 gFront, float3 sunDir) {
    constexpr float kMaxAngle = 0.96f;   // ~55 degrees between the plane normal and the sun
    float3 Lh{sunDir.x, 0, sunDir.z};
    const float len = length(Lh);
    if (len < 1e-3f) return gRight;      // sun overhead: any facing works
    Lh = Lh * (1.0f / len);
    float3 n = gFront;
    float c = dot(n, Lh);
    const float sign = c < 0 ? -1.0f : 1.0f;   // the quad is two-sided
    n = n * sign; c *= sign;
    const float ang = std::acos(std::clamp(c, -1.0f, 1.0f));
    if (ang <= kMaxAngle) return gRight;
    // turn n towards Lh by (ang - kMaxAngle) around +Y
    const float turn = ang - kMaxAngle;
    const float side = cross(n, Lh).y >= 0 ? 1.0f : -1.0f;
    const float a = turn * side, ca = std::cos(a), sa = std::sin(a);
    float3 nr{n.x * ca + n.z * sa, 0, -n.x * sa + n.z * ca};
    if (dot(nr, Lh) < c) nr = {n.x * ca - n.z * sa, 0, n.x * sa + n.z * ca};   // rotated the wrong way
    nr = nr * sign;
    return cross(float3{0, 1, 0}, nr);   // gRight = up x gFront
}

} // namespace rts::game
