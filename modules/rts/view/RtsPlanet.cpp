#include "RtsPlanet.h"
#include <algorithm>
#include <cmath>
#include "../RtsMap.h"
#include "../RtsSphere.h"

namespace rts::game {

using namespace render;

namespace {
constexpr float kQuarterPi = 0.785398163f;
constexpr float kTwoPi = 6.28318531f;
float3 toF(const Vec3& v) { return {float(v.x.toDouble()), float(v.y.toDouble()), float(v.z.toDouble())}; }
float3 toF(IVec3 v) { return {float(v.x), float(v.y), float(v.z)}; }
float num(const Json& j, float def) { return j.isNumber() ? float(j.toDouble(def)) : def; }
float3 color255(const Json& j, float3 def) {
    if (!j.isArray() || j.size() < 3) return def;
    float3 c{float(j[0].toDouble()), float(j[1].toDouble()), float(j[2].toDouble())};
    return c.x > 1.0f || c.y > 1.0f || c.z > 1.0f ? c * (1.0f / 255.0f) : c;
}
} // namespace

int PlanetWorld::find(const std::string& id) const {
    for (size_t i = 0; i < bodies.size(); ++i) if (bodies[i].id == id) return int(i);
    return -1;
}

float3 PlanetWorld::center(int body, double seconds) const {
    float3 c;
    for (int guard = 0; body >= 0 && body < int(bodies.size()) && guard < 16; ++guard) {
        const PlanetBody& b = bodies[size_t(body)];
        if (b.parent < 0) return c + b.pos;
        const double turns = double(b.phase) + (b.period > 0 ? seconds / double(b.period) : 0.0);
        const float a = float((turns - std::floor(turns)) * double(kTwoPi));
        c += float3{std::cos(a), 0, std::sin(a)} * b.orbitRadius;
        body = b.parent;
    }
    return c;
}

int PlanetWorld::starOf(int) const {
    for (size_t i = 0; i < bodies.size(); ++i) if (bodies[i].star()) return int(i);
    return -1;
}

PlanetWorld loadPlanetWorld(const Json& map, std::vector<std::string>* warnings) {
    PlanetWorld w;
    const Json& wj = map["world"];
    if (!wj.isObject() || !wj["bodies"].isArray()) return w;
    auto warn = [&](const std::string& m) { if (warnings) warnings->push_back("world: " + m); };
    std::vector<std::string> parents;
    for (const Json& bj : wj["bodies"].items()) {
        PlanetBody b;
        b.id = bj.getStr("id", "body" + std::to_string(w.bodies.size()));
        const std::string kind = bj.getStr("kind", "planet");
        b.kind = kind == "star" ? PlanetBody::Kind::Star : kind == "moon" ? PlanetBody::Kind::Moon : kind == "plane" ? PlanetBody::Kind::Plane : PlanetBody::Kind::Planet;
        b.radius = std::max(1.0f, num(bj["radius"], 32));
        b.faceSize = b.star() ? 0 : int(std::clamp<int64_t>(bj["faceSize"].integer(0), 0, 1024));
        if (bj.has("seaLevel")) b.seaLevel = num(bj["seaLevel"], 0);
        b.orbitAltitude = num(bj["orbitAltitude"], b.radius * 0.15f);
        const Json& o = bj["orbit"];
        parents.push_back(o.isObject() ? o.getStr("around") : "");
        if (o.isObject()) { b.orbitRadius = num(o["radius"], 0); b.period = num(o["period"], 0); b.phase = num(o["phase"], 0); }
        if (bj["pos"].isArray() && bj["pos"].size() >= 3) b.pos = {num(bj["pos"][0], 0), num(bj["pos"][1], 0), num(bj["pos"][2], 0)};
        const Json& vis = bj["visual"];
        b.color = color255(vis["color"], b.star() ? float3{1.0f, 0.86f, 0.63f} : float3{0.7f, 0.7f, 0.7f});
        if (const Json& a = vis["atmosphere"]; a.isObject() || (a.type() == Json::Type::Bool && a.boolean())) {
            b.atmosphere = true;
            b.atmoColor = color255(a["color"], b.atmoColor);
            b.atmoHeight = std::clamp(num(a["height"], b.atmoHeight), 0.005f, 0.5f);
        } else if (!vis.has("atmosphere") && b.kind == PlanetBody::Kind::Planet) b.atmosphere = true;
        if (b.surface()) {
            const int N = b.faceSize, S = (N + 1) * (N + 1);
            const Json& t = bj["terrain"];
            b.corners.assign(size_t(6 * S), 0);
            const Json& cj = t["corners"];
            if (cj.isArray() && cj.size() > 0) {
                if (cj.size() != b.corners.size()) warn("body '" + b.id + "': terrain.corners has " + std::to_string(cj.size()) + " values, expected " + std::to_string(b.corners.size()));
                for (size_t i = 0; i < cj.size() && i < b.corners.size(); ++i) b.corners[i] = int16_t(cj[i].integer());
            }
            b.flags.assign(size_t(6 * N * N), kCellGround);
            const Json& faces = t["faces"];
            for (size_t f = 0; f < faces.size() && f < 6; ++f) {
                const Json& rows = faces[f]["rows"];
                for (size_t y = 0; y < rows.size() && int(y) < N; ++y) {
                    const std::string r = rows[y].str();
                    for (size_t x = 0; x < r.size() && int(x) < N; ++x) b.flags[(f * size_t(N) + y) * size_t(N) + x] = cellFlagsFromChar(r[x]);
                }
            }
        }
        w.bodies.push_back(std::move(b));
    }
    for (size_t i = 0; i < w.bodies.size(); ++i) {
        if (parents[i].empty()) continue;
        int p = w.find(parents[i]);
        if (p < 0 || p == int(i)) warn("body '" + w.bodies[i].id + "': unknown orbit.around '" + parents[i] + "'");
        else w.bodies[i].parent = p;
    }
    w.start = std::max(0, w.find(wj.getStr("start")));
    if (wj.getStr("start").empty()) for (size_t i = 0; i < w.bodies.size(); ++i) if (w.bodies[i].surface()) { w.start = int(i); break; }
    return w;
}

// ------------------------------------------------------------------ surface

void PlanetSurface::init(const PlanetBody& b) {
    n_ = std::max(1, b.faceSize);
    radius_ = b.radius;
    const int N = n_, S = (N + 1) * (N + 1);
    CubeSphere cs(N);
    dirs_.assign(size_t(6 * S), {});
    heights_.assign(size_t(6 * S), 0.0f);
    canon_.assign(size_t(6 * S), 0);
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                const size_t k = size_t(cornerIndex(f, i, j));
                dirs_[k] = toF(cs.cornerDir(f, i, j));
                int dup[3];
                const int cnt = cs.cornerDuplicates(f, i, j, dup);
                float h = 0;
                for (int d = 0; d < cnt; ++d) h += size_t(dup[d]) < b.corners.size() ? float(b.corners[size_t(dup[d])]) : 0.0f;
                heights_[k] = h / float(cnt) / 16.0f;   // duplicates averaged defensively (docs/planets.md §2)
                canon_[k] = dup[0];
            }
    // duplicates share the canonical direction exactly, so seam vertices of different faces coincide
    for (size_t k = 0; k < dirs_.size(); ++k) dirs_[k] = dirs_[size_t(canon_[k])];
    minH_ = 1e9f; maxH_ = -1e9f;
    for (float h : heights_) { minH_ = std::min(minH_, h); maxH_ = std::max(maxH_, h); }
    flags_ = b.flags;
    normals_.assign(dirs_.size(), {});
    for (size_t k = 0; k < dirs_.size(); ++k) normals_[k] = size_t(canon_[k]) < k ? normals_[size_t(canon_[k])] : normalAt(dirs_[k]);
}

uint8_t PlanetSurface::cellFlags(int face, int x, int y) const {
    const size_t i = (size_t(face) * size_t(n_) + size_t(y)) * size_t(n_) + size_t(x);
    return i < flags_.size() ? flags_[i] : uint8_t(kCellGround);
}

float3 PlanetSurface::dirFromFace(int face, float a, float b) const {
    const SphereFace& F = kSphereFaces[std::clamp(face, 0, 5)];
    const float s = std::tan((2.0f * a / float(n_) - 1.0f) * kQuarterPi), t = std::tan((2.0f * b / float(n_) - 1.0f) * kQuarterPi);
    return normalize(toF(F.n) + toF(F.u) * s + toF(F.v) * t);
}

void PlanetSurface::faceFromDir(float3 d, int& face, float& a, float& b) const {
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    if (ax >= ay && ax >= az) face = d.x >= 0 ? 0 : 1;
    else if (ay >= az) face = d.y >= 0 ? 2 : 3;
    else face = d.z >= 0 ? 4 : 5;
    const SphereFace& F = kSphereFaces[face];
    const float dn = std::max(dot(d, toF(F.n)), 1e-6f);
    auto unwarp = [&](float du) { return std::clamp((std::atan2(du, dn) / kQuarterPi + 1.0f) * 0.5f * float(n_), 0.0f, float(n_)); };
    a = unwarp(dot(d, toF(F.u)));
    b = unwarp(dot(d, toF(F.v)));
}

float PlanetSurface::heightAt(float3 dir) const {
    if (heights_.empty()) return 0;
    int f; float a, b;
    faceFromDir(dir, f, a, b);
    const int x = std::min(int(a), n_ - 1), y = std::min(int(b), n_ - 1);
    const float fx = a - float(x), fz = b - float(y);
    const float h00 = cornerHeight(f, x, y), h10 = cornerHeight(f, x + 1, y), h01 = cornerHeight(f, x, y + 1), h11 = cornerHeight(f, x + 1, y + 1);
    if (fx >= fz) return h00 + (h10 - h00) * fx + (h11 - h10) * fz;
    return h00 + (h11 - h01) * fx + (h01 - h00) * fz;
}

float3 PlanetSurface::surfacePoint(float3 dir) const {
    dir = normalize(dir);
    return dir * (radius_ + heightAt(dir));
}

float3 PlanetSurface::normalAt(float3 dir) const {
    dir = normalize(dir);
    float3 t1 = normalize(cross(std::fabs(dir.y) < 0.9f ? float3{0, 1, 0} : float3{1, 0, 0}, dir)), t2 = cross(dir, t1);
    const float e = 0.7f / float(n_);   // ~ half a cell (a face spans ~pi/2)
    const float3 a = surfacePoint(dir + t1 * e), b = surfacePoint(dir - t1 * e), c = surfacePoint(dir + t2 * e), d = surfacePoint(dir - t2 * e);
    float3 n = normalize(cross(a - b, c - d));
    return dot(n, dir) < 0 ? -n : n;
}

bool PlanetSurface::raycast(float3 o, float3 d, float3& hit, float seaLevel) const {
    const float outer = radius_ + std::max(maxH_, seaLevel) + 0.01f;
    // entry into the bounding sphere
    const float bq = dot(o, d), cq = dot(o, o) - outer * outer, disc = bq * bq - cq;
    if (disc < 0) return false;
    const float sq = std::sqrt(disc);
    float t = std::max(0.0f, -bq - sq);
    const float tEnd = -bq + sq;
    if (tEnd < 0) return false;
    auto above = [&](float tt) {
        float3 p = o + d * tt;
        float r = length(p);
        float g = radius_ + std::max(heightAt(p), seaLevel);
        return r - g;
    };
    const float step = std::max(radius_ * 1.2f / float(n_), 0.05f);
    float prev = t, fPrev = above(t);
    if (fPrev <= 0) { hit = o + d * t; return true; }
    for (float tt = t + step; prev < tEnd; tt += step) {
        tt = std::min(tt, tEnd);
        const float f = above(tt);
        if (f <= 0) {
            float lo = prev, hi = tt;
            for (int i = 0; i < 24; ++i) { float m = 0.5f * (lo + hi); (above(m) > 0 ? lo : hi) = m; }
            hit = o + d * hi;
            return true;
        }
        prev = tt; fPrev = f;
        if (tt >= tEnd) break;
    }
    return false;
}

} // namespace rts::game
