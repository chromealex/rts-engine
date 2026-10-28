// RtsView on planet maps (docs/planets.md §5): bodies from the map's `world` block, the orbit camera,
// objects placed on the sphere (oriented to the local up), selection rings, bars, fog of war, picking.
// The flat path (RtsView.cpp / RtsTerrain.cpp) is untouched: planetMode() is false without a world block.
//
// Until the simulation provides bodies (agent-planet-sim), the flat sim map is laid onto face 2 of the
// start body (flatToBody): objects with SceneObject::body < 0 are mapped there. Objects with body >= 0
// are body-local already (pos, facing, target).
#include <algorithm>
#include <cmath>
#include "../RtsModule.h"
#include "RtsView.h"

namespace rts::game {

using namespace render;

namespace {
uint32_t ringColor(const SceneObject& o) { return o.selected ? (o.owner == 0 ? rgba(255, 255, 160, 230) : rgba(120, 255, 120, 230)) : rgba(255, 255, 255, 130); }
}

void RtsView::setWorld(const Json& map) {
    PlanetWorld w = loadPlanetWorld(map, &warnings_);
    planets_.setWorld(w);
    demoBody_ = -1;
    if (!planets_.active()) return;
    demoBody_ = w.start;
    planetCam_.reset(planets_.world(), w.start, planets_.surface(w.start), 0);
}

void RtsView::updateWorld(const Json& map) {
    PlanetWorld w = loadPlanetWorld(map);
    const PlanetWorld& cur = planets_.world();
    bool same = w.bodies.size() == cur.bodies.size() && !w.empty();
    for (size_t i = 0; same && i < w.bodies.size(); ++i) {
        const PlanetBody &a = w.bodies[i], &b = cur.bodies[i];
        same = a.id == b.id && a.kind == b.kind && a.faceSize == b.faceSize && a.radius == b.radius && a.seaLevel == b.seaLevel &&
               a.parent == b.parent && a.orbitRadius == b.orbitRadius && a.period == b.period && a.phase == b.phase && a.atmosphere == b.atmosphere;
    }
    if (!same) {
        const int body = planetCam_.body();
        const float3 dir = planetCam_.dir();
        const float dist = planetCam_.distance;
        setWorld(map);
        if (planets_.active() && body < int(planets_.world().bodies.size())) { // keep the view while editing
            planetCam_.reset(planets_.world(), body, planets_.surface(body), planetSeconds_, dir);
            planetCam_.distance = dist;
        }
        return;
    }
    for (size_t i = 0; i < w.bodies.size(); ++i) {
        if (!w.bodies[i].surface()) continue;
        planets_.updateFlags(int(i), w.bodies[i].flags);
        planets_.updateHeights(int(i), w.bodies[i].corners);
    }
}

bool RtsView::flatToBody(float3 flat, int& body, float3& local) const {
    const PlanetSurface* s = planets_.surface(demoBody_);
    if (!s) return false;
    body = demoBody_;
    const float a = demoOff_[0] + flat.x * demoScale_, b = demoOff_[1] + flat.z * demoScale_;
    local = s->surfacePoint(s->dirFromFace(2, a, b));
    return true;
}

bool RtsView::bodyToFlat(int body, float3 local, float3& flat) const {
    const PlanetSurface* s = planets_.surface(body);
    if (!s || body != demoBody_) return false;
    int f; float a, b;
    s->faceFromDir(normalize(local), f, a, b);
    if (f != 2) return false;
    flat = {(a - demoOff_[0]) / demoScale_, 0, (b - demoOff_[1]) / demoScale_};
    return flat.x >= 0 && flat.z >= 0 && flat.x <= float(demoW_) && flat.z <= float(demoH_);
}

float3 RtsView::bodyToView(int body, float3 local) const {
    return pframe_.toView(planets_.world().center(body, planetSeconds_) + local);
}

const RTSCamera& RtsView::updatePlanetCamera(double seconds, float dt, int viewH) {
    (void)viewH;
    planetSeconds_ = seconds;
    const int b = planetCam_.body();
    planetCam_.update(planets_.world(), planets_.surface(b), seconds, dt);
    pframe_ = planetCam_.frame();
    return planetCam_.camera();
}

bool RtsView::pickPlanet(const RTSCamera& cam, float sx, float sy, float w, float h, int& body, float3& local) const {
    float3 o, d;
    if (!planets_.active() || !cam.screenRay(sx, sy, w, h, o, d)) return false;
    float best = 1e30f;
    const PlanetWorld& W = planets_.world();
    for (size_t i = 0; i < W.bodies.size(); ++i) {
        const PlanetSurface* s = planets_.surface(int(i));
        if (!s) continue;
        const float3 c = bodyToView(int(i), {0, 0, 0});
        // body-local ray (world orientation)
        const float3 lo = pframe_.dirToWorld(o - c), ld = pframe_.dirToWorld(d);
        float3 hit;
        if (!s->raycast(lo, ld, hit, W.bodies[i].seaLevel)) continue;
        const float t = length(hit - lo);
        if (t < best) { best = t; body = int(i); local = hit; }
    }
    return best < 1e30f;
}

bool RtsView::placeObject(const SceneObject& o, int& body, float3& ground, float3& up, float3& east, float3& south, float& lift) const {
    float3 local;
    lift = 0;
    if (o.body >= 0) { body = o.body; local = o.pos; }
    else if (!flatToBody(o.pos, body, local)) return false;
    const PlanetSurface* s = planets_.surface(body);
    if (!s) return false;
    up = normalize(local);
    ground = s->surfacePoint(up);
    const float sea = planets_.world().bodies[size_t(body)].seaLevel;
    if (sea > -1e8f && length(ground) < s->radius() + sea) ground = up * (s->radius() + sea);   // naval units float
    lift = std::max(0.0f, length(local) - length(ground));   // orbit layer / flyers above the ground (sim positions)
    if (o.body < 0) lift = 0;
    // tangent frame: flat-map axes (x east = +a, z south = +b of the face grid) so flat facings keep working
    int f; float a, b;
    s->faceFromDir(up, f, a, b);
    const float e = 0.25f;
    float3 da = s->dirFromFace(f, std::min(a + e, float(s->faceSize())), b) - s->dirFromFace(f, std::max(a - e, 0.0f), b);
    da = da - up * dot(da, up);
    east = normalize(da);
    south = cross(east, up);
    return true;
}

bool RtsView::planetPlacement(const SceneObject& o, mat4& m) const {
    int body; float3 g, up, east, south; float lift;
    if (!placeObject(o, body, g, up, east, south, lift)) return false;
    const float3 p = bodyToView(body, g + up * lift), U = pframe_.dirToView(up), E = pframe_.dirToView(east), S = pframe_.dirToView(south);
    m = mat4{};
    m.at(0, 0) = E.x; m.at(1, 0) = E.y; m.at(2, 0) = E.z;
    m.at(0, 1) = U.x; m.at(1, 1) = U.y; m.at(2, 1) = U.z;
    m.at(0, 2) = S.x; m.at(1, 2) = S.y; m.at(2, 2) = S.z;
    m.at(0, 3) = p.x; m.at(1, 3) = p.y; m.at(2, 3) = p.z;
    return true;
}

float3 RtsView::objectViewPos(const SceneObject& o, float lift) const {
    if (!planets_.active()) return {o.pos.x, lift, o.pos.z};
    int body; float3 g, up, e, sth; float l;
    if (!placeObject(o, body, g, up, e, sth, l)) return {o.pos.x, lift, o.pos.z};
    return bodyToView(body, g + up * (l + lift));
}

void RtsView::drawPlanets(Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid) {
    const float dt = lastTime_ < 0 ? 0.0f : float(std::clamp(time - lastTime_, 0.0, 0.1));
    lastTime_ = time;
    ++frame_;
    viewProj_ = r.viewProj();
    camPos_ = r.cameraPos();
    events_.clear();
    effects_.clear();
    bars_.assign(s.objects.size(), {});
    barOk_.assign(s.objects.size(), 0);
    planetSeconds_ = s.seconds;
    const float blend = std::max(planetCam_.planetBlend(), planetCam_.systemBlend());
    planetNoShadow_ = blend > 0.5f;
    // fog of war: the flat vision (faded by RtsFog) laid onto the demo body; the rest of the planet
    // counts as explored. Sim bodies will hand per-body vision directly.
    if (vision && !vision->empty() && grid.w > 0 && planets_.surface(demoBody_)) {
        fog_.update(*vision, grid.w, grid.h, time);
        const PlanetSurface& sf = *planets_.surface(demoBody_);
        const int N = sf.faceSize();
        planetVision_.assign(size_t(6 * N * N), 128);
        const std::vector<uint8_t>& fld = fog_.field();
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const int gx = int(std::floor((float(x) + 0.5f - demoOff_[0]) / demoScale_)), gy = int(std::floor((float(y) + 0.5f - demoOff_[1]) / demoScale_));
                if (grid.inside(gx, gy) && fld.size() == size_t(grid.w * grid.h)) planetVision_[size_t((2 * N + y) * N + x)] = fld[size_t(grid.index(gx, gy))];
            }
        planets_.setVision(demoBody_, &planetVision_, fog_.look());
    } else planets_.setVision(demoBody_, nullptr, fog_.look());
    planets_.draw(r, pframe_, s.seconds, planetCam_.body(), blend);
    handleEvents(s);
    const float3 eye = r.camera().eye();
    const PlanetWorld& W = planets_.world();
    // visibility of a body-local point: not behind its body (horizon test against the sphere)
    auto occluded = [&](int body, float3 pv) {
        const float3 c = bodyToView(body, {0, 0, 0});
        const float R = W.bodies[size_t(body)].radius * 0.985f;
        const float3 d = pv - eye;
        const float len = length(d);
        if (len < 1e-4f) return false;
        const float3 dir = d * (1.0f / len), oc = eye - c;
        const float bq = dot(oc, dir), cq = dot(oc, oc) - R * R, disc = bq * bq - cq;
        if (disc <= 0) return false;
        const float t = -bq - std::sqrt(disc);
        return t > 0 && t < len;
    };
    auto frameAt = [&](int body, float3 g, float3 up, float3 east, float3 south) {
        const float3 p = bodyToView(body, g), U = pframe_.dirToView(up), E = pframe_.dirToView(east), S = pframe_.dirToView(south);
        mat4 m;
        m.at(0, 0) = E.x; m.at(1, 0) = E.y; m.at(2, 0) = E.z;
        m.at(0, 1) = U.x; m.at(1, 1) = U.y; m.at(2, 1) = U.z;
        m.at(0, 2) = S.x; m.at(1, 2) = S.y; m.at(2, 2) = S.z;
        m.at(0, 3) = p.x; m.at(1, 3) = p.y; m.at(2, 3) = p.z;
        return m;
    };
    auto ring = [&](const mat4& m, float rad, uint32_t col, float lift) {
        const int seg = 32;
        float3 prev;
        for (int i = 0; i <= seg; ++i) {
            const float a = 6.2831853f * float(i) / float(seg);
            const float3 p = m.transformPoint({std::cos(a) * rad, lift, std::sin(a) * rad});
            if (i) r.line(prev, p, col);
            prev = p;
        }
    };
    for (size_t i = 0; i < s.objects.size(); ++i) {
        const SceneObject& o = s.objects[i];
        int body; float3 g, up, east, south; float lift;
        if (!placeObject(o, body, g, up, east, south, lift)) continue;
        const mat4 place = frameAt(body, g + up * lift, up, east, south);
        const float3 pv = place.transformPoint({0, 0, 0});
        if (o.isProjectile) {
            if (o.visible) r.drawMesh(projectile_, mat4::translate(place.transformPoint({0, 0.6f, 0})), {1, 0.95f, 0.8f, 1}, kDrawUnlit);
            continue;
        }
        if (o.type >= looks_.size()) continue;
        const uint64_t key = o.entity.valid() ? o.entity.packed() : (1ull << 63) | uint64_t(i);
        // the object in its tangent frame: facing / target as flat-map style vectors
        SceneObject lo = o;
        if (o.body >= 0) {
            lo.facing = {dot(o.facing, east), 0, dot(o.facing, south)};
            const float3 t = o.target - o.pos;
            lo.target = {dot(t, east), dot(t, up), dot(t, south)};
            lo.pos = {0, 0, 0};
        }
        lo.pos = g;   // body-local: speed from the 3D displacement
        if (o.body >= 0) { const float3 t = lo.target; lo.target = lo.pos + t; }
        else if (o.hasTarget) { int tb; float3 tl; if (flatToBody(o.target, tb, tl)) { const float3 d = tl - g; lo.target = g + float3{dot(d, east), dot(d, up), dot(d, south)}; } }
        EntityAnim& st = stateFor(key, lo);
        st.last = lo;
        if (!o.visible) continue;
        const TypeLook& L = looks_[o.type];
        animate(st, lo, dt, nullptr, &pv);
        drawObject(r, st, lo, 0, 1.0f, nullptr, &place);
        if (L.air && !planetNoShadow_) ring(place, 0.3f, rgba(0, 0, 0, 90), 0.03f);
        if (o.selected || o.hovered) ring(place, selectionRadius(o.type), ringColor(o), 0.05f);
        const UnitType& t = rules_->types[o.type];
        float top = t.structure ? 1.6f * L.scale * std::max(1.0f, std::max(t.footprintW, t.footprintH) * 0.6f) : 1.2f * L.scale;
        if (L.air) top += L.flyHeight;
        bars_[i] = place.transformPoint({0, top, 0});
        barOk_[i] = !occluded(body, bars_[i]) && blend < 0.6f;
    }
    // corpses: last known placement (body-local in st.last), fading out
    for (auto it = anims_.begin(); it != anims_.end();) {
        EntityAnim& st = it->second;
        if (st.seen == frame_ && !st.dead) { ++it; continue; }
        if (!st.dead || !st.initialized) { it = st.seen + 2 < frame_ ? anims_.erase(it) : std::next(it); continue; }
        st.deathAge += dt;
        if (st.type >= looks_.size() || st.deathAge > looks_[st.type].corpse) { it = anims_.erase(it); continue; }
        const TypeLook& L = looks_[st.type];
        if (st.seen != frame_ && st.last.visible && demoBody_ >= 0) {
            SceneObject o = st.last;
            o.selected = o.hovered = false;
            const PlanetSurface* sf = planets_.surface(demoBody_);
            if (sf) {
                const float3 up = normalize(o.pos);
                int f; float a, b;
                sf->faceFromDir(up, f, a, b);
                float3 da = sf->dirFromFace(f, std::min(a + 0.25f, float(sf->faceSize())), b) - sf->dirFromFace(f, std::max(a - 0.25f, 0.0f), b);
                const float3 east = normalize(da - up * dot(da, up)), south = cross(east, up);
                const mat4 place = frameAt(demoBody_, o.pos, up, east, south);
                const float3 pv = place.transformPoint({0, 0, 0});
                animate(st, o, dt, nullptr, &pv);
                drawObject(r, st, o, 0, L.dissolveDeath ? 1.0f : std::clamp((L.corpse - st.deathAge) / 0.6f, 0.0f, 1.0f), nullptr, &place);
            }
        }
        ++it;
    }
    for (auto& mk : s.moveMarkers) {
        int body; float3 l;
        if (!flatToBody(mk, body, l)) continue;
        SceneObject o; o.pos = mk;
        int b2; float3 g, up, e, so; float lift;
        if (placeObject(o, b2, g, up, e, so, lift)) ring(frameAt(b2, g, up, e, so), 0.35f, rgba(120, 255, 120, 200), 0.06f);
    }
}

} // namespace rts::game
