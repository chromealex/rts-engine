// RtsView on planet maps (docs/planets.md §5): bodies from the map's `world` block, the orbit camera,
// objects placed on the sphere (oriented to the local up), selection rings, bars, fog of war, picking.
// The flat path (RtsView.cpp / RtsTerrain.cpp) is untouched: planetMode() is false without a world block.
//
// Match maps: bodies, orbits, corner heights and cell flags come from the simulation's Surface
// (setSimWorld); objects carry their body and body-local position (sceneFromSphere), units in transit their
// world position. Editor maps without a simulation read the map JSON (setWorld).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../RtsModule.h"
#include "RtsView.h"

namespace rts::game {

using namespace render;

namespace {
uint32_t ringColor(const SceneObject& o) { return o.selected ? (o.owner == 0 ? rgba(255, 255, 160, 230) : rgba(120, 255, 120, 230)) : rgba(255, 255, 255, 130); }
}

void RtsView::setWorld(const Json& map) {
    fog_.setBlur({});
    PlanetWorld w = loadPlanetWorld(map, &warnings_);
    planets_.setWorld(w);
    planetVision_.clear();
    if (!planets_.active()) return;
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

void RtsView::setSimWorld(std::shared_ptr<const Surface> surf, const MapGrid& grid, const Json& map) {
    if (surf) { // fog blur over the cube-sphere neighbourhood (across face seams, never into other bodies / void cells)
        std::shared_ptr<const Surface> S = surf;
        fog_.setBlur([S, tmp = std::vector<uint16_t>()](std::vector<uint8_t>& f, int radius) mutable {
            const int n = S->W * S->H;
            if (int(f.size()) != n || radius <= 0) return;
            tmp.resize(size_t(n));
            for (int pass = 0; pass < radius; ++pass) { // repeated 3x3 means ~ a box of the radius
                for (int c = 0; c < n; ++c) {
                    int b, fc, x, y;
                    if (!S->cellFace(c, b, fc, x, y)) { tmp[size_t(c)] = f[size_t(c)]; continue; }
                    int sum = f[size_t(c)], cnt = 1;
                    for (int k = 0; k < 8; ++k) { const int o = S->neighbor(c, k); if (o >= 0) { sum += f[size_t(o)]; ++cnt; } }
                    tmp[size_t(c)] = uint16_t(sum / cnt);
                }
                for (int c = 0; c < n; ++c) f[size_t(c)] = uint8_t(tmp[size_t(c)]);
            }
        });
    } else fog_.setBlur({});
    PlanetWorld w = planetWorldFromSim(std::move(surf), grid.flags, loadPlanetWorld(map, &warnings_));
    planets_.setWorld(w);
    planetVision_.clear();
    if (!planets_.active()) return;
    planetCam_.reset(planets_.world(), w.start, planets_.surface(w.start), 0);
}

bool RtsView::simToBody(float3 sim, int& body, float3& local) {
    if (!onSphereF(sim)) return false;
    body = bodyAtF(sim);
    local = sim - anchorF(body);
    return true;
}

bool RtsView::facePoint(int body, int face, float x, float y, float3& local) const {
    const PlanetSurface* s = planets_.surface(body);
    if (!s || face < 0 || face > 5) return false;
    local = s->surfacePoint(s->dirFromFace(face, x, y));
    return true;
}

mat4 RtsView::planetFxSpace() const {
    return pframe_.matrix() * mat4::translate(planets_.world().center(planetCam_.body(), planetSeconds_));
}

bool RtsView::planetFxPlace(float3 sim, float lift, float3& out, float3& up) const {
    int body; float3 local;
    if (!simToBody(sim, body, local) || body != planetCam_.body()) return false;
    const PlanetSurface* s = planets_.surface(body);
    up = normalize(local);
    if (!s || lift < -1e8f) { out = local; return true; }
    const float3 ground = s->surfacePoint(up);
    const float above = length(local) - length(ground);
    out = above > 0.6f ? local + up * lift : ground + up * lift;   // orbit / flying: from where it is
    return true;
}

float RtsView::planetFxAltitude(float3 local) const {
    const PlanetSurface* s = planets_.surface(planetCam_.body());
    if (!s) return 1e9f;
    const float l = length(local);
    return l - (s->radius() + s->heightAt(local * (1.0f / std::max(l, 1e-6f))));
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
    if (o.body < 0) return false;
    body = o.body; local = o.pos;
    const PlanetSurface* s = planets_.surface(body);
    if (!s) return false;
    up = normalize(local);
    ground = s->surfacePoint(up);
    const float sea = planets_.world().bodies[size_t(body)].seaLevel;
    if (sea > -1e8f && length(ground) < s->radius() + sea) ground = up * (s->radius() + sea);   // naval units float
    lift = std::max(0.0f, length(local) - length(ground));   // orbit layer above the ground (sim positions)
    if (lift < 0.6f) lift = 0;                                 // ground units: on the drawn surface
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
    if (o.body == SceneObject::kBodyTransit) return pframe_.toView(o.pos);
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
    // fog of war per body: the viewer's vision of the stacked grid, faded by RtsFog and blurred over the
    // cube-sphere neighbourhood (setSimWorld), then cut per body
    const PlanetWorld& Wd = planets_.world();
    planetVision_.resize(Wd.bodies.size());
    const bool fogOn = vision && !vision->empty() && Wd.sim && grid.w > 0 && vision->size() == size_t(grid.w * grid.h);
    if (fogOn) fog_.update(*vision, grid.w, grid.h, time);
    for (size_t bi = 0; bi < Wd.bodies.size(); ++bi) {
        if (!Wd.bodies[bi].surface()) continue;
        if (!fogOn) { planetVision_[bi].clear(); planets_.setVision(int(bi), nullptr, fog_.look()); continue; }
        const int N = Wd.bodies[bi].faceSize;
        const std::vector<uint8_t>& fld = fog_.field();
        std::vector<uint8_t>& pv = planetVision_[bi];
        pv.assign(size_t(6 * N * N), 0);
        for (int f = 0; f < 6; ++f)
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x) {
                    const size_t c = size_t(Wd.sim->gridCell(int(bi), f, x, y));
                    pv[(size_t(f) * size_t(N) + size_t(y)) * size_t(N) + size_t(x)] = c < fld.size() ? fld[c] : 0;
                }
        planets_.setVision(int(bi), &pv, fog_.look());
    }
    if (std::getenv("RTS_PLANET_DEBUG")) { // RTS_PLANET_DEBUG: what the scene holds (screenshot set-ups)
        int tr = 0, orbit = 0, moon = 0;
        for (auto& o : s.objects) {
            if (o.isProjectile) continue;
            if (o.body == SceneObject::kBodyTransit) ++tr;
            else if (o.body >= 0 && o.body != Wd.start) ++moon;
            else if (o.type < rules_->types.size() && (rules_->types[o.type].layer & kLayerOrbit)) ++orbit;
        }
        std::fprintf(stderr, "[planet] t=%.0f transit %d orbit-units %d on-other-bodies %d\n", s.seconds, tr, orbit, moon);
    }
    planets_.draw(r, pframe_, s.seconds, planetCam_.body(), blend);
    static const bool terrDemo = std::getenv("RTS_PLANET_TERRITORY_DEMO") != nullptr;
    if (terrDemo && Wd.sim) { // testing: owner slots within 4 cells of each player's structures (the sim does not claim on spheres yet)
        territoryDemo_.assign(grid.flags.size(), 0);
        for (auto& o : s.objects) {
            if (o.body < 0 || o.owner == 0 || o.isProjectile || o.type >= rules_->types.size() || !rules_->types[o.type].structure) continue;
            const PlanetSurface* sf = planets_.surface(o.body);
            if (!sf) continue;
            int f; float a, b;
            sf->faceFromDir(normalize(o.pos), f, a, b);
            const int cx = int(a), cy = int(b);
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx) {
                    if (dx * dx + dy * dy > 16) continue;
                    const int c = Wd.sim->offset(Wd.sim->gridCell(o.body, f, std::clamp(cx, 0, sf->faceSize() - 1), std::clamp(cy, 0, sf->faceSize() - 1)), dx, dy);
                    if (c >= 0 && (grid.flags[size_t(c)] & kCellGround) && !territoryDemo_[size_t(c)]) territoryDemo_[size_t(c)] = o.owner;
                }
        }
    }
    if (dev_ && rules_ && planetGroundFx_.draw(*dev_, r, planets_, pframe_, s, time, vision, grid, groundFx_, *rules_, terrDemo ? &territoryDemo_ : nullptr))
        for (size_t bi = 0; bi < Wd.bodies.size(); ++bi) { // rock dug out (seen): the terrain under it becomes ground
            if (!Wd.bodies[bi].surface()) continue;
            const int N = Wd.bodies[bi].faceSize;
            std::vector<uint8_t> fl(size_t(6 * N * N), kCellGround);
            for (int f = 0; f < 6; ++f)
                for (int y = 0; y < N; ++y)
                    for (int x = 0; x < N; ++x) {
                        const size_t c = size_t(Wd.sim->gridCell(int(bi), f, x, y));
                        if (c < grid.flags.size()) fl[(size_t(f) * size_t(N) + size_t(y)) * size_t(N) + size_t(x)] = grid.flags[c];
                    }
            planets_.updateFlags(int(bi), fl);
        }
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
    auto tangentFrame = [&](int body, float3 up, float3& east, float3& south) {
        const PlanetSurface* sf = planets_.surface(body);
        int f; float a, b;
        sf->faceFromDir(up, f, a, b);
        float3 da = sf->dirFromFace(f, std::min(a + 0.25f, float(sf->faceSize())), b) - sf->dirFromFace(f, std::max(a - 0.25f, 0.0f), b);
        east = normalize(da - up * dot(da, up));
        south = cross(east, up);
    };
    for (size_t i = 0; i < s.objects.size(); ++i) {
        const SceneObject& o = s.objects[i];
        const uint64_t key = o.entity.valid() ? o.entity.packed() : (1ull << 63) | uint64_t(i);
        int body = -1; float3 g, up, east, south; float lift = 0;
        mat4 place;
        if (o.body == SceneObject::kBodyTransit) { // between bodies: world position, nose along the travel direction
            const float3 U = pframe_.dirToView({0, 1, 0});
            float3 F = pframe_.dirToView(o.facing);
            F = F - U * dot(F, U);
            if (dot(F, F) < 1e-6f) F = pframe_.dirToView({1, 0, 0});
            const float3 E = normalize(F), S = cross(E, U), P = pframe_.toView(o.pos);
            place.at(0, 0) = E.x; place.at(1, 0) = E.y; place.at(2, 0) = E.z;
            place.at(0, 1) = U.x; place.at(1, 1) = U.y; place.at(2, 1) = U.z;
            place.at(0, 2) = S.x; place.at(1, 2) = S.y; place.at(2, 2) = S.z;
            place.at(0, 3) = P.x; place.at(1, 3) = P.y; place.at(2, 3) = P.z;
        } else {
            if (!placeObject(o, body, g, up, east, south, lift)) continue;
            place = frameAt(body, g + up * lift, up, east, south);
        }
        const float3 pv = place.transformPoint({0, 0, 0});
        if (o.isProjectile) {
            if (o.visible) r.drawMesh(projectile_, mat4::translate(place.transformPoint({0, 0.3f, 0})), {1, 0.95f, 0.8f, 1}, kDrawUnlit);
            continue;
        }
        if (o.type >= looks_.size()) continue;
        // the object in its tangent frame (flat-map style vectors: x east, y up, z south), so the flat
        // animation code (speed, yaw, aim) works unchanged; the frame turns slowly with the position
        SceneObject lo = o;
        if (body >= 0) {
            lo.pos = {dot(g, east), dot(g, up), dot(g, south)};
            lo.facing = {dot(o.facing, east), 0, dot(o.facing, south)};
            const float3 t = o.target - o.pos;
            lo.target = lo.pos + float3{dot(t, east), dot(t, up), dot(t, south)};
            planetLast_[key] = {body, g, lift};
        } else { // transit: the frame above has east = travel direction
            lo.pos = o.pos; lo.facing = {1, 0, 0}; lo.hasTarget = false;
        }
        EntityAnim& st = stateFor(key, lo);
        st.last = lo;
        if (!o.visible) continue;
        const TypeLook& L = looks_[o.type];
        animate(st, lo, dt, nullptr, &pv);
        drawObject(r, st, lo, 0, 1.0f, nullptr, &place);
        if (o.selected || o.hovered) ring(place, selectionRadius(o.type), ringColor(o), 0.05f);
        if (lift > 0 && !planetNoShadow_) ring(frameAt(body, g, up, east, south), 0.35f, rgba(0, 0, 0, 110), 0.04f);   // orbit units: mark below
        const UnitType& t = rules_->types[o.type];
        float top = t.structure ? 1.6f * L.scale * std::max(1.0f, std::max(t.footprintW, t.footprintH) * 0.6f) : 1.2f * L.scale;
        if (L.air) top += L.flyHeight;
        bars_[i] = place.transformPoint({0, top, 0});
        barOk_[i] = (body < 0 || !occluded(body, bars_[i])) && blend < 0.6f;
    }
    // corpses: last known placement, fading out
    for (auto it = anims_.begin(); it != anims_.end();) {
        EntityAnim& st = it->second;
        if (st.seen == frame_ && !st.dead) { ++it; continue; }
        if (!st.dead || !st.initialized) {
            if (st.seen + 2 < frame_) { planetLast_.erase(it->first); it = anims_.erase(it); } else ++it;
            continue;
        }
        st.deathAge += dt;
        if (st.type >= looks_.size() || st.deathAge > looks_[st.type].corpse) { planetLast_.erase(it->first); it = anims_.erase(it); continue; }
        const TypeLook& L = looks_[st.type];
        auto pl = planetLast_.find(it->first);
        if (st.seen != frame_ && st.last.visible && pl != planetLast_.end() && planets_.surface(pl->second.body)) {
            SceneObject o = st.last;
            o.selected = o.hovered = false;
            const float3 up = normalize(pl->second.ground);
            float3 east, south;
            tangentFrame(pl->second.body, up, east, south);
            const mat4 place = frameAt(pl->second.body, pl->second.ground + up * pl->second.lift, up, east, south);
            const float3 pv = place.transformPoint({0, 0, 0});
            animate(st, o, dt, nullptr, &pv);
            drawObject(r, st, o, 0, L.dissolveDeath ? 1.0f : std::clamp((L.corpse - st.deathAge) / 0.6f, 0.0f, 1.0f), nullptr, &place);
        }
        ++it;
    }
    for (auto& mk : s.moveMarkers) { // sim-space points (client picking)
        int body; float3 l;
        if (!simToBody(mk, body, l) || !planets_.surface(body)) continue;
        const float3 up = normalize(l);
        float3 east, south;
        tangentFrame(body, up, east, south);
        ring(frameAt(body, planets_.surface(body)->surfacePoint(up), up, east, south), 0.35f, rgba(120, 255, 120, 200), 0.06f);
    }
}

// ------------------------------------------------------------------ scene from the simulation

void sceneFromSphere(Simulation& sim, RtsModule& m, const Surface& S, RtsScene& out, float alpha, uint8_t viewer) {
    World& w = sim.world();
    const Rules& R = m.rules();
    GameState& gs = m.state(sim);
    auto toF = [](const Vec3& v) { return float3{float(v.x.toDouble()), float(v.y.toDouble()), float(v.z.toDouble())}; };
    // body-local float position of a sim position; prev/cur on different bodies (landed): take cur
    auto local = [&](Vec3 prev, Vec3 cur, int& body) {
        body = S.bodyAt(cur);
        const float3 c = toF(cur - Surface::anchor(body));
        if (S.bodyAt(prev) != body) return c;
        return lerp(toF(prev - Surface::anchor(body)), c, alpha);
    };
    auto visibleAt = [&](Vec3 p) -> uint8_t {
        const int c = S.cellAt(p);
        return c >= 0 && size_t(c) < gs.vision[viewer].size() ? gs.vision[viewer][size_t(c)] : 0;
    };
    const uint32_t tick = sim.tick();
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        SceneObject so;
        so.entity = e; so.type = u.type; so.owner = o.player;
        const UnitType& ut = R.types[u.type];
        if (auto* tx = w.tryGet<Transit>(e)) { // between bodies: world position (interpolated like worldPosition)
            const float span = float(std::max<uint32_t>(1, tx->arrive - tx->depart));
            const float k = std::clamp((float(tick > tx->depart ? tick - tx->depart : 0) + alpha) / span, 0.0f, 1.0f);
            const float3 a = toF(tx->fromWorld), b = toF(tx->toWorld);
            so.body = SceneObject::kBodyTransit;
            so.pos = lerp(a, b, k);
            so.facing = normalize(b - a);
            so.moving = true; so.action = kActMove;
            so.hp = ut.maxHp.raw > 0 ? float((u.hp / ut.maxHp).toDouble()) : 1.0f;
            so.visible = viewer == 0 || o.player == viewer || sim.allied(o.player, viewer);
            out.objects.push_back(so);
            return;
        }
        if ((u.flags & kUnitInHand) || !Surface::onSphere(t.pos)) return;
        int body;
        so.pos = local(t.prevPos, t.pos, body);
        so.body = int16_t(body);
        so.facing = toF(u.facing);
        so.hp = ut.maxHp.raw > 0 ? float((u.hp / ut.maxHp).toDouble()) : 1.0f;
        if (auto* c = w.tryGet<Construction>(e)) so.build = float(c->progress.toDouble());
        if (auto* g = w.tryGet<Garrison>(e)) so.garrison = g->count;
        if (auto* c = w.tryGet<Cargo>(e)) { so.carry = float(c->amount.toDouble()); so.carryRes = c->res; }
        if (auto* od = w.tryGet<Orders>(e); od && !od->queue.empty()) so.order = uint8_t(od->queue.front().type) + 1;
        if (auto* mb = w.tryGet<Mobile>(e)) so.moving = mb->moving;
        if (auto* pr = w.tryGet<Production>(e)) so.producing = !pr->queue.empty();
        if (auto* cb = w.tryGet<Combat>(e); cb && cb->target.valid() && w.alive(cb->target)) {
            if (auto* tt = w.tryGet<Transform>(cb->target); tt && S.bodyAt(tt->pos) == body) {
                int tb;
                so.hasTarget = true; so.target = local(tt->prevPos, tt->pos, tb);
            }
        }
        if (so.moving) so.action = kActMove;
        else if (so.hasTarget) so.action = kActAttack;
        else if (so.order) {
            OrderType ot = OrderType(so.order - 1);
            if (ot == OrderType::Gather || ot == OrderType::Build || ot == OrderType::Repair) so.action = kActWork;
        }
        if (viewer != 0 && o.player != viewer && !sim.allied(o.player, viewer)) {
            const uint8_t v = visibleAt(t.pos);
            so.visible = ut.structure && o.player == 0 ? v >= 1 : v == 2;
        }
        out.objects.push_back(so);
    });
    w.each<Projectile, Transform>([&](Entity e, Projectile& pr, Transform& t) {
        if (!Surface::onSphere(t.pos)) return;
        SceneObject so;
        so.entity = e; so.isProjectile = true; so.source = pr.source;
        if (auto* su = w.tryGet<Unit>(pr.source)) so.shooterType = su->type;
        int body;
        so.pos = local(t.prevPos, t.pos, body);
        so.body = int16_t(body);
        if (viewer != 0) so.visible = visibleAt(t.pos) == 2;
        out.objects.push_back(so);
    });
    for (size_t i = 0; i < gs.roads.nodes.size(); ++i)   // map roads between buildings (sim positions; RtsPlanetGroundFx drapes them)
        for (uint16_t j : gs.roads.adj[i]) {
            if (j <= i) continue;
            auto* ta = w.tryGet<Transform>(gs.roads.nodes[i]);
            auto* tb = w.tryGet<Transform>(gs.roads.nodes[j]);
            if (ta && tb) out.roads.push_back({toF(ta->pos), toF(tb->pos)});
        }
}

} // namespace rts::game
