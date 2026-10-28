// agent-mech stage 4, field battles (Total War, Close Combat): morale with flank / rear hits, deaths nearby and
// generals; wavering and rout (routing units flee and do not fight until they rally); fatigue from running and
// fighting; charge bonus after a run-up; supply lines (units away from own depots lose hp and are slower).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
Fixed flankMult(World& w, const MechRules& M, Entity target, Entity source) {
    if (!w.alive(source) || !w.has<Transform>(source)) return Fixed::fromInt(1);
    Vec3 d = flat(w.get<Transform>(source).pos - w.get<Transform>(target).pos);
    Fixed len = d.length();
    if (len.raw <= 0) return Fixed::fromInt(1);
    const Fixed c = (d / len).dot(w.get<Unit>(target).facing);
    if (c < -Fixed::fromRatio(1, 2)) return M.flankRear;
    if (c < Fixed::fromRatio(1, 2)) return M.flankSide;
    return Fixed::fromInt(1);
}
} // namespace

Fixed RtsModule::moraleMaxOf(uint16_t type) const {
    const Fixed v = rules_.types[type].mech.moraleMax;
    return v.raw > 0 ? v : rules_.mech.moraleMax;
}

void RtsModule::mechMoraleHit(Simulation& sim, Entity target, Entity source, Fixed dmg) {
    World& w = sim.world();
    auto* mo = w.tryGet<Morale>(target);
    if (!mo || !mo->init) return;
    const MechRules& M = rules_.mech;
    const Fixed maxHp = stat(sim, target, Stat::MaxHp);
    Fixed loss = maxHp.raw > 0 ? dmg * M.hitLoss / maxHp : Fixed{};
    loss = loss * flankMult(w, M, target, source);
    if (auto* sm = w.tryGet<Morale>(source); sm && sm->chargeTick == sim.tick() && w.has<Unit>(source))
        loss += rules_.types[w.get<Unit>(source).type].mech.chargeShock;
    mo->value = fxMax(Fixed{}, mo->value - loss);
}

void RtsModule::mechMoraleDeath(Simulation& sim, Entity victim) {
    World& w = sim.world();
    const MechRules& M = rules_.mech;
    if (!M.morale || !w.has<Unit>(victim)) return;
    const uint8_t owner = w.get<Owner>(victim).player;
    const UnitType& vt = rules_.types[w.get<Unit>(victim).type];
    const bool general = M.generalTags && (vt.tags & M.generalTags);
    const Fixed r = general ? M.generalRadius * 2 : M.allyRadius;
    const Fixed loss = general ? M.generalDeath : M.allyDeath;
    const Vec3 p = w.get<Transform>(victim).pos;
    GameState& gs = state(sim);
    std::vector<Entity> hit;
    gs.hash.query(p, r, [&](Entity a) {
        if (a == victim || !w.has<Morale>(a) || w.get<Owner>(a).player != owner) return;
        if (dist2D(w.get<Transform>(a).pos, p) <= r) hit.push_back(a);
    });
    for (Entity a : hit) { auto& mo = w.get<Morale>(a); mo.value = fxMax(Fixed{}, mo.value - loss); }
}

bool RtsModule::mechBattleFire(Simulation& sim, Entity e) {
    World& w = sim.world();
    auto* mo = w.tryGet<Morale>(e);
    if (!mo) return true;
    if (mo->state == uint8_t(MoraleMode::Routing)) return false;
    mo->lastFire = sim.tick();
    const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
    if (mt.chargeBonus.raw > 0 && mo->run >= mt.chargeRun &&
        (mo->lastCharge == 0 || sim.tick() >= mo->lastCharge + uint32_t((mt.chargeCooldown * int64_t(sim.tickRate())).floorInt()))) {
        mo->chargeTick = mo->lastCharge = sim.tick();
        mo->run = Fixed{};
    }
    return true;
}

void RtsModule::mechBattleStat(Simulation& sim, Entity e, Stat s, Fixed& mul) const {
    const auto* mo = sim.world().tryGet<Morale>(e);
    if (!mo) return;
    const MechRules& M = rules_.mech;
    if (s == Stat::Speed) {
        if (mo->state == uint8_t(MoraleMode::Routing)) mul = mul * M.routSpeed;
        if (mo->outOfSupply) mul = mul * M.supplySpeed;
    }
    if (s == Stat::AttackSpeed) {
        if (mo->state == uint8_t(MoraleMode::Wavering)) mul = mul * M.waverAttack;
        if (mo->outOfSupply) mul = mul * M.supplyAttack;
    }
    if ((s == Stat::Speed || s == Stat::AttackSpeed) && M.fatigue) {
        if (mo->fatigue >= M.exhaustedAt) mul = mul * M.exhaustedMul;
        else if (mo->fatigue >= M.tiredAt) mul = mul * M.tiredMul;
    }
}

bool RtsModule::mechRouting(Simulation& sim, Entity e) const {
    const auto* mo = sim.world().tryGet<Morale>(e);
    return mo && mo->state == uint8_t(MoraleMode::Routing);
}

void RtsModule::mechTickBattle(Simulation& sim) {
    const MechRules& M = rules_.mech;
    if (!M.battle()) return;
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    const uint32_t tick = sim.tick();
    const uint64_t tags = M.moraleTags | M.fatigueTags | M.supplyTags;
    std::vector<Entity> add;
    struct Change { Entity e; uint8_t state; };
    std::vector<Change> changes;
    std::vector<std::pair<Entity, Vec3>> flee;
    std::vector<std::pair<Entity, Fixed>> attrition;
    // own supply sources (finished depots) per player
    std::vector<std::pair<uint8_t, std::pair<Vec3, Fixed>>> depots;
    const bool supplyTick = M.supply && tick % 20 == 0;
    if (supplyTick)
        w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
            const Fixed r = rules_.types[u.type].mech.supplyRadius;
            if (r.raw > 0 && u.hp.raw > 0 && !(u.flags & kUnitUnderConstruction)) depots.push_back({o.player, {t.pos, r}});
        });
    w.each<Unit, Mobile, Transform>([&](Entity e, Unit& u, Mobile& m, Transform& tr) {
        if (u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
        const UnitType& t = rules_.types[u.type];
        auto* mo = w.tryGet<Morale>(e);
        if (!mo) { if (!t.structure && (!tags || (t.tags & tags))) add.push_back(e); return; }
        const uint8_t owner = w.get<Owner>(e).player;
        if (!mo->init) { mo->value = moraleMaxOf(u.type); mo->lastPos = tr.pos; mo->init = true; }
        // run-up for a charge: distance covered while moving
        const Fixed step = dist2D(tr.pos, mo->lastPos);
        mo->lastPos = tr.pos;
        if (m.moving) mo->run = fxMin(Fixed::fromInt(100), mo->run + step); else if (!w.has<Combat>(e) || !w.alive(w.get<Combat>(e).target)) mo->run = Fixed{};
        // fatigue
        if (M.fatigue && (!M.fatigueTags || (t.tags & M.fatigueTags))) {
            const bool fighting = mo->lastFire != 0 && tick < mo->lastFire + sim.tickRate();
            if (m.moving) mo->fatigue += M.fatRun * dt;
            if (fighting) mo->fatigue += M.fatFight * dt;
            if (!m.moving && !fighting) mo->fatigue -= M.fatRecover * dt;
            mo->fatigue = std::clamp(mo->fatigue, Fixed{}, Fixed::fromInt(100));
        }
        // supply lines
        if (supplyTick && (!M.supplyTags || (t.tags & M.supplyTags))) {
            bool in = false;
            for (auto& d : depots)
                if ((d.first == owner || (d.first != 0 && sim.allied(d.first, owner))) && dist2D(d.second.first, tr.pos) <= d.second.second) { in = true; break; }
            mo->outOfSupply = !in;
            if (!in && M.supplyAttrition.raw > 0) attrition.push_back({e, stat(sim, e, Stat::MaxHp) * M.supplyAttrition});
        }
        if (!M.morale || (M.moraleTags && !(t.tags & M.moraleTags))) return;
        // the nearest enemy in sight (flee from it) and a general of ours nearby (faster recovery)
        Entity enemy = kNullEntity; Fixed ed{};
        bool general = false;
        const Fixed sight = stat(sim, e, Stat::Sight);
        const Fixed qr = fxMax(sight, M.generalRadius);
        gs.hash.query(tr.pos, qr, [&](Entity c) {
            if (c == e || !w.has<Unit>(c) || w.get<Unit>(c).hp.raw <= 0) return;
            const Fixed d = dist2D(w.get<Transform>(c).pos, tr.pos);
            const uint8_t co = w.get<Owner>(c).player;
            const UnitType& ct = rules_.types[w.get<Unit>(c).type];
            if (co == owner && M.generalTags && (ct.tags & M.generalTags) && d <= M.generalRadius) general = true;
            if (d <= sight && !ct.weapons.empty() && hostile(sim, owner, co) && (!enemy.valid() || d < ed)) { enemy = c; ed = d; }
        });
        const bool routing = mo->state == uint8_t(MoraleMode::Routing);
        Fixed regen = M.moraleRegen + (general ? M.generalRegen : Fixed{});
        if (routing && enemy.valid()) regen = Fixed{};   // no rallying with the enemy on your heels
        mo->value = fxMin(moraleMaxOf(u.type), mo->value + regen * dt);
        uint8_t st = mo->state;
        if (mo->value < M.routAt) st = uint8_t(MoraleMode::Routing);
        else if (routing) { if (mo->value >= M.rallyAt) st = uint8_t(MoraleMode::Steady); }
        else st = mo->value < M.waverAt ? uint8_t(MoraleMode::Wavering) : uint8_t(MoraleMode::Steady);
        if (st != mo->state) changes.push_back({e, st});
        if (st == uint8_t(MoraleMode::Routing) && enemy.valid() && (tick + e.index) % 10 == 0) {
            Vec3 d = flat(tr.pos - w.get<Transform>(enemy).pos);
            Fixed len = d.length();
            if (len.raw <= 0) d = Vec3{Fixed::fromInt(1), Fixed{}, Fixed{}}; else d = d / len;
            Vec3 to = tr.pos + d * Fixed::fromInt(8);
            to.x = std::clamp(to.x, kHalf, Fixed::fromInt(gs.grid.w) - kHalf);
            to.z = std::clamp(to.z, kHalf, Fixed::fromInt(gs.grid.h) - kHalf);
            flee.push_back({e, to});
        }
    });
    for (Entity e : add) w.add<Morale>(e, Morale{});
    for (auto& c : changes) {
        auto& mo = w.get<Morale>(c.e);
        mo.state = c.state;
        if (c.state == uint8_t(MoraleMode::Routing)) {
            if (auto* ord = w.tryGet<Orders>(c.e)) ord->queue.clear();
            if (auto* cb = w.tryGet<Combat>(c.e)) cb->target = kNullEntity;
        } else if (auto* mb = w.tryGet<Mobile>(c.e); mb && mb->moving && (!w.has<Orders>(c.e) || w.get<Orders>(c.e).queue.empty())) {
            stopMoving(sim, c.e);   // rallied: stop running
        }
        sim.emit({kEvMorale, w.get<Owner>(c.e).player, c.e, kNullEntity, int64_t(c.state), w.get<Transform>(c.e).pos});
    }
    for (auto& f : flee) {
        if (auto* ord = w.tryGet<Orders>(f.first)) ord->queue.clear();
        moveTo(sim, f.first, f.second, kOne);
    }
    for (auto& a : attrition) if (w.alive(a.first)) dealDamage(sim, a.first, a.second, kDamagePure, kNullEntity, 0);
}

void RtsModule::mechArmyBlock(Simulation& sim, const std::vector<Entity>& ms, Vec3 target, Vec3 dir, Vec3 perp, Fixed spacing,
                              std::vector<std::pair<Entity, Vec3>>& out) {
    const MapGrid& g = state(sim).grid;
    const int n = int(ms.size());
    int cols = 8;
    while (cols * cols < 2 * n) ++cols;            // about twice as wide as deep
    cols = std::min(cols, n);
    const int rows = (n + cols - 1) / cols;
    for (int i = 0; i < n; ++i) {
        // units are sorted by lateral position: each column takes a contiguous slice, so paths don't cross
        const int col = int(int64_t(i) * cols / n), row = i - (int(int64_t(col) * n + cols - 1) / cols);
        const Fixed lat = spacing * (2 * col - (cols - 1)) / 2;
        const Fixed back = spacing * (row - (rows - 1) / 2);
        Vec3 p = flat(target + perp * lat - dir * back);
        p.x = std::clamp(p.x, kHalf, Fixed::fromInt(g.w) - kHalf);
        p.z = std::clamp(p.z, kHalf, Fixed::fromInt(g.h) - kHalf);
        out.push_back({ms[size_t(i)], p});
    }
}

} // namespace rts::game
