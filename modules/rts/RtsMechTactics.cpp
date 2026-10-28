// agent-mech stage 3, tactics: weapons that suppress / kill crews / leave craters / rotate turrets, crush,
// ruins and infection on death (crews: RtsOriginals.cpp).
#include "RtsInternal.h"
#include <algorithm>
#include "../../engine/core/FixedMath.h"

namespace rts::game {
using namespace detail;

namespace {
Fixed at(const std::vector<Fixed>& v, size_t i) { return i < v.size() ? v[i] : Fixed{}; }
// Rotate unit vector `from` towards `to` by at most `maxTurns` (fraction of a full turn).
Vec3 rotateTowards(Vec3 from, Vec3 to, Fixed maxTurns) {
    const Fixed a = fxAtan2Turns(from.z, from.x), b = fxAtan2Turns(to.z, to.x);
    Fixed d = b - a;
    if (d > Fixed::fromRatio(1, 2)) d -= kOne;
    if (d < -Fixed::fromRatio(1, 2)) d += kOne;
    if (fxAbs(d) <= maxTurns) return to;
    const Fixed na = a + (d.raw > 0 ? maxTurns : -maxTurns);
    return {fxCosTurns(na), Fixed{}, fxSinTurns(na)};
}
} // namespace

bool RtsModule::mechOnFire(Simulation& sim, Entity e, size_t wi, Entity target) {
    World& w = sim.world();
    const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
    // turret must face the target (rotated in mechTickTactics)
    if (at(mt.turretRate, wi).raw > 0 && w.alive(target)) {
        auto* tu = w.tryGet<Turret>(e);
        if (!tu) return false;
        Vec3 d = flat(w.get<Transform>(target).pos - w.get<Transform>(e).pos);
        Fixed len = d.length();
        if (len.raw > 0 && tu->dir[wi % 4].dot(d / len) < Fixed::fromRatio(985, 1000)) return false;   // ~10 degrees
    }
    if (rules_.mech.ammo && !mechUseAmmo(sim, e, wi)) return false;
    if (!w.alive(target) || !w.has<Unit>(target)) return true;
    const UnitType& tt = rules_.types[w.get<Unit>(target).type];
    // suppression builds up on the target
    const Fixed sup = at(mt.suppress, wi);
    if (sup.raw > 0 && rules_.mech.suppression && (!rules_.mech.supTags || (tt.tags & rules_.mech.supTags))) {
        if (auto* s = w.tryGet<Suppression>(target)) s->value = fxMin(Fixed::fromInt(100), s->value + sup);
    }
    // craters: the ground at the target becomes cover
    const Fixed cr = at(mt.crater, wi);
    if (cr.raw > 0 && !rules_.mech.coverLevels.empty()) {
        GameState& gs = state(sim);
        MapGrid& g = gs.grid;
        if (gs.mech.cover.size() != size_t(g.w) * size_t(g.h)) gs.mech.cover.assign(size_t(g.w) * size_t(g.h), 0);
        const Vec3 p = w.get<Transform>(target).pos;
        int cx, cy; g.cellXY(p, cx, cy);
        const int r = std::max(0, int(cr.floorInt()));
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x)
                if (g.inside(x, y)) { uint8_t& c = gs.mech.cover[size_t(g.index(x, y))]; if (c == 0) c = 1; }
    }
    return true;
}

void RtsModule::mechTickTactics(Simulation& sim) {
    World& w = sim.world();
    const MechRules& M = rules_.mech;
    const Fixed dt = sim.dt();
    std::vector<Entity> addSup, addTurret;
    w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& tr) {
        if (u.hp.raw <= 0) return;
        const UnitType& t = rules_.types[u.type];
        if (M.suppression) {
            if (auto* s = w.tryGet<Suppression>(e)) s->value = fxMax(Fixed{}, s->value - M.supDecay * dt);
            else if (!t.structure && (!M.supTags || (t.tags & M.supTags))) addSup.push_back(e);
        }
        if (!t.mech.turretRate.empty()) {
            auto* tu = w.tryGet<Turret>(e);
            if (!tu) { addTurret.push_back(e); return; }
            if (!tu->init) { for (auto& d : tu->dir) d = u.facing; tu->init = true; }
            auto* c = w.tryGet<Combat>(e);
            if (!c || !w.alive(c->target)) return;
            Vec3 d = flat(w.get<Transform>(c->target).pos - tr.pos);
            Fixed len = d.length();
            if (len.raw <= 0) return;
            d = d / len;
            for (size_t i = 0; i < t.mech.turretRate.size() && i < 4; ++i) {
                const Fixed rate = t.mech.turretRate[i];
                if (rate.raw > 0) tu->dir[i] = rotateTowards(tu->dir[i], d, rate * dt / 360);
            }
        }
    });
    for (Entity e : addSup) w.add<Suppression>(e, Suppression{});
    for (Entity e : addTurret) w.add<Turret>(e, Turret{});
    // crush: moving crushers run over small enemies they touch
    if (M.combat3) {
        GameState& gs = state(sim);
        struct Hit { Entity v, by; Fixed dmg; uint8_t owner; };
        std::vector<Hit> hits;
        w.each<Unit, Mobile, Transform>([&](Entity e, Unit& u, Mobile& m, Transform& tr) {
            const MechType& mt = rules_.types[u.type].mech;
            if (mt.crushDamage.raw <= 0 || !m.moving || u.hp.raw <= 0) return;
            const uint8_t owner = w.get<Owner>(e).player;
            const Fixed r = rules_.types[u.type].radius;
            gs.hash.query(tr.pos, r + kOne, [&](Entity v) {
                if (v == e || !w.has<Unit>(v) || !w.has<Mobile>(v)) return;
                const UnitType& vt = rules_.types[w.get<Unit>(v).type];
                if (mt.crushTags && !(vt.tags & mt.crushTags)) return;
                if (!hostile(sim, owner, w.get<Owner>(v).player)) return;
                if (dist2D(w.get<Transform>(v).pos, tr.pos) <= r + vt.radius + Fixed::fromRatio(1, 4)) hits.push_back({v, e, mt.crushDamage, owner});
            });
        });
        for (auto& h : hits) if (w.alive(h.v)) dealDamage(sim, h.v, h.dmg, kDamagePure, h.by, h.owner);
    }
}

void RtsModule::mechTacticsDeath(Simulation& sim, Entity e) {
    World& w = sim.world();
    const Unit& u = w.get<Unit>(e);
    if (u.flags & kUnitSilent) return;
    const UnitType& t = rules_.types[u.type];
    const Vec3 p = w.get<Transform>(e).pos;
    if (t.mech.ruins != kNoType) {
        Entity r = spawn(sim, t.mech.ruins, 0, t.structure ? snapToFootprint(rules_.types[t.mech.ruins], p) : p);
        (void)r;
    }
    if (!rules_.mech.infection) return;
    auto* vv = w.tryGet<Veteran>(e);
    Entity killer = vv ? vv->lastHit : kNullEntity;
    if (!w.alive(killer) || !w.has<Unit>(killer)) return;
    const MechType& km = rules_.types[w.get<Unit>(killer).type].mech;
    uint16_t into = kNoType;
    for (uint16_t x : km.infect) if (x != kNoType) { into = x; break; }
    if (into == kNoType) return;
    const uint8_t owner = w.get<Owner>(killer).player;
    if (t.structure) {   // an overrun building spawns infected (They Are Billions)
        for (int k = 0; k < t.mech.infectCount; ++k)
            spawn(sim, t.mech.infectSpawn, owner, p + Vec3{Fixed::fromRatio(k % 3 - 1, 2), Fixed{}, Fixed::fromRatio(k / 3 - 1, 2)});
    } else {
        spawn(sim, into, owner, p);
    }
}

} // namespace rts::game
