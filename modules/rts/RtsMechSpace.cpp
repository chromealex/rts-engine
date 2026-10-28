// agent-mech stage 5, SP: free 3D flight on flat maps (Homeworld-style fleets): units with "altitude" keep their
// own height inside [min, max], climb / dive to the altitude of their move orders (the command point's y), and
// weapons / sight ranges to and from them are measured in 3D. The 2D movement (paths, avoidance) is unchanged.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

void RtsModule::mechAltitudeCommand(Simulation& sim, const Command& cmd) {
    if (cmd.type != kCmdMove && cmd.type != kCmdAttack && cmd.type != kCmdPatrol) return;
    if (cmd.type == kCmdAttack && sim.world().alive(cmd.target)) return;   // chasing a unit: keep the altitude
    if (cmd.point.y.raw == 0) return;
    World& w = sim.world();
    for (Entity e : cmd.units) {
        if (!w.alive(e) || !w.has<Unit>(e) || w.get<Owner>(e).player != cmd.player) continue;
        const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
        if (mt.altMax.raw <= 0) continue;
        if (auto* al = w.tryGet<Altitude>(e)) al->target = std::clamp(cmd.point.y, mt.altMin, mt.altMax);
    }
}

void RtsModule::mechTickAltitude(Simulation& sim) {
    World& w = sim.world();
    const Fixed dt = sim.dt();
    std::vector<Entity> add;
    w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& tr) {
        const MechType& mt = rules_.types[u.type].mech;
        if (mt.altMax.raw <= 0 || u.hp.raw <= 0) return;
        auto* al = w.tryGet<Altitude>(e);
        if (!al) { add.push_back(e); return; }
        const Fixed step = mt.altClimb * dt;
        const Fixed d = al->target - al->y;
        al->y = fxAbs(d) <= step ? al->target : al->y + (d.raw > 0 ? step : -step);
        tr.pos.y = al->y;   // movement keeps (or on heightfields resets) y: the altitude is ours
        // chasing a target: match its altitude so the 3D range can close
        if (auto* c = w.tryGet<Combat>(e); c && w.alive(c->target) && w.has<Transform>(c->target)) {
            const Fixed ty = w.get<Transform>(c->target).pos.y;
            al->target = std::clamp(ty, mt.altMin, mt.altMax);
        }
    });
    for (Entity e : add) {
        const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
        w.add<Altitude>(e, Altitude{mt.altStart, mt.altStart});
        w.get<Transform>(e).pos.y = mt.altStart;
        w.get<Transform>(e).prevPos.y = mt.altStart;
    }
}

Fixed RtsModule::mechContact3D(Simulation& sim, Entity a, Entity b) {
    World& w = sim.world();
    const Vec3 pa = w.get<Transform>(a).pos, pb = w.get<Transform>(b).pos;
    const UnitType& A = rules_.types[w.get<Unit>(a).type];
    const UnitType& B = rules_.types[w.get<Unit>(b).type];
    Fixed flatDist;
    if (B.structure && w.has<Footprint>(b)) {
        auto& f = w.get<Footprint>(b);
        Fixed px = fxClamp(pa.x, Fixed::fromInt(f.x), Fixed::fromInt(f.x + f.w));
        Fixed pz = fxClamp(pa.z, Fixed::fromInt(f.y), Fixed::fromInt(f.y + f.h));
        flatDist = Vec2{pa.x - px, pa.z - pz}.length();
    } else {
        flatDist = (pa - pb).xz().length();
    }
    const Fixed dy = pa.y - pb.y;
    const Fixed d3 = fxSqrt(flatDist * flatDist + dy * dy);
    return fxMax(d3 - A.radius - (B.structure ? Fixed{} : B.radius), Fixed{});
}

} // namespace rts::game
