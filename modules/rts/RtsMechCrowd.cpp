// agent-mech stage 4, BA: separation for dense armies. Gives exactly the pushes of the generic loop in
// sysMovement (same candidates, same arithmetic; the sum of fixed-point terms does not depend on their order)
// but looks neighbours up in a one-cell grid over flat arrays instead of the 4-cell spatial hash with
// component lookups per pair.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

void RtsModule::mechSeparation(Simulation& sim, const std::vector<Fixed>& weight, const std::vector<uint8_t>& anchored, Fixed deadZone, size_t,
                               const std::function<void(Entity, Vec3)>& push) {
    World& w = sim.world();
    const MapGrid& g = state(sim).grid;
    const SpatialHash& hs = state(sim).hash;   // the generic loop's candidates: bucket of the position at insertion
    const Fixed dt = sim.dt();
    struct Body { Entity e; Vec3 pos; Fixed r; uint8_t layer; int bx, by; };
    std::vector<Body> bodies;
    w.each<Mobile, Unit, Transform>([&](Entity e, Mobile&, Unit& u, Transform& tr) {
        if (u.flags & kUnitOffMap) return;
        const UnitType& t = rules_.types[u.type];
        const int bx = std::clamp(int(tr.prevPos.x.floorInt()) / hs.cell, 0, hs.w - 1), by = std::clamp(int(tr.prevPos.z.floorInt()) / hs.cell, 0, hs.h - 1);
        bodies.push_back({e, tr.pos, t.radius, t.layer, bx, by});
    });
    if (bodies.empty()) return;
    Fixed maxR{};
    for (auto& b : bodies) maxR = fxMax(maxR, b.r);
    const int gw = std::max(1, g.w), gh = std::max(1, g.h);
    auto cellOf = [&](Vec3 p, int& cx, int& cy) {
        cx = std::clamp(int(p.x.floorInt()), 0, gw - 1);
        cy = std::clamp(int(p.z.floorInt()), 0, gh - 1);
    };
    std::vector<int32_t> head(size_t(gw) * size_t(gh), -1), next(bodies.size(), -1);
    for (size_t i = bodies.size(); i-- > 0;) {
        int cx, cy; cellOf(bodies[i].pos, cx, cy);
        const size_t c = size_t(cy) * size_t(gw) + size_t(cx);
        next[i] = head[c];
        head[c] = int32_t(i);
    }
    for (size_t i = 0; i < bodies.size(); ++i) {
        const Body& a = bodies[i];
        if (rules_.lanes.enabled && w.has<LaneWalker>(a.e)) continue;   // lane walkers keep strictly to their lane
        const Fixed reach = a.r + maxR;
        const int x0 = std::clamp(int((a.pos.x - reach).floorInt()), 0, gw - 1), x1 = std::clamp(int((a.pos.x + reach).floorInt()), 0, gw - 1);
        const int y0 = std::clamp(int((a.pos.z - reach).floorInt()), 0, gh - 1), y1 = std::clamp(int((a.pos.z + reach).floorInt()), 0, gh - 1);
        Vec3 acc{};
        bool any = false;
        const Fixed we = weight[a.e.index];
        const Fixed qr = a.r * 2 + kOne;   // the generic loop's query radius
        const int hr = int((qr.raw >> Fixed::kFracBits) / hs.cell) + 1;
        const int hcx = int(a.pos.x.floorInt()) / hs.cell, hcy = int(a.pos.z.floorInt()) / hs.cell;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (int32_t j = head[size_t(y) * size_t(gw) + size_t(x)]; j >= 0; j = next[size_t(j)]) {
                    if (size_t(j) == i) continue;
                    const Body& b = bodies[size_t(j)];
                    if (std::abs(b.bx - hcx) > hr || std::abs(b.by - hcy) > hr) continue;                    if (!(b.layer & a.layer) && !((b.layer | a.layer) == (kLayerGround | kLayerSandOnly))) continue;
                    Vec3 d = flat(a.pos - b.pos);
                    Fixed len = d.length();
                    Fixed overlap = a.r + b.r - len - deadZone;
                    if (overlap.raw <= 0) continue;
                    if (len.raw == 0) {
                        d = (a.e.index < b.e.index) ? Vec3{kOne, Fixed{}, Fixed{}} : Vec3{-kOne, Fixed{}, Fixed{}};
                        len = kOne;
                    }
                    Fixed wo = b.e.index < weight.size() ? weight[b.e.index] : kOne;
                    Fixed share = wo / fxMax(we + wo, Fixed::fromRatio(1, 100));
                    acc += d * (overlap * share / len);
                    any = true;
                }
        if (any) {
            const UnitType& t = rules_.types[w.get<Unit>(a.e).type];
            Fixed len = acc.length();
            Fixed maxPush = anchored[a.e.index] ? Fixed::fromRatio(1, 100) : t.speed * dt / 2 + Fixed::fromRatio(1, 50);
            if (len > maxPush) acc = acc * (maxPush / len);
            push(a.e, acc);
        }
    }
}

} // namespace rts::game
