// Mushroom Wars 2 style movement lanes: for every pair of garrison buildings an A* path around
// obstacles and buildings is smoothed into a fixed-point Catmull-Rom spline and sampled into a
// polyline; parallel lanes are offset left/right of it (pulled back where they would hit an
// obstacle). Units sent between buildings are spread over the lanes round-robin and follow their
// lane strictly. Lanes regenerate when buildings or obstacles change.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
Vec3 catmullRom(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, Fixed t) {
    Fixed t2 = t * t, t3 = t2 * t;
    auto axis = [&](Fixed a, Fixed b, Fixed c, Fixed d) {
        return (b * 2 + (c - a) * t + (a * 2 - b * 5 + c * 4 - d) * t2 + (b * 3 - a - c * 3 + d) * t3) / 2;
    };
    return {axis(p0.x, p1.x, p2.x, p3.x), Fixed{}, axis(p0.z, p1.z, p2.z, p3.z)};
}

// Ring cell around a footprint closest to a point (deterministic: lowest index on ties).
Vec3 exitCell(const MapGrid& g, const Footprint& f, Vec3 toward) {
    Vec3 best = g.cellCenter(f.x + f.w / 2, f.y + f.h / 2);
    Fixed bestD{};
    bool found = false;
    for (int y = f.y - 1; y <= f.y + f.h; ++y)
        for (int x = f.x - 1; x <= f.x + f.w; ++x) {
            bool edge = x == f.x - 1 || x == f.x + f.w || y == f.y - 1 || y == f.y + f.h;
            if (!edge || !g.passable(x, y, kLayerGround)) continue;
            Vec3 c = g.cellCenter(x, y);
            Fixed d = (c - toward).xz().length();
            if (!found || d < bestD) { best = c; bestD = d; found = true; }
        }
    return best;
}
} // namespace

std::vector<int> RtsModule::lanesBetween(Simulation& sim, Entity a, Entity b) const {
    auto* gs = sim.world().resource<GameState>();
    std::vector<int> out;
    for (size_t i = 0; i < gs->lanes.size(); ++i) {
        const Lane& l = gs->lanes[i];
        if ((l.a == a && l.b == b) || (l.a == b && l.b == a)) out.push_back(int(i));
    }
    std::stable_sort(out.begin(), out.end(), [&](int x, int y) { return std::abs(gs->lanes[size_t(x)].offset) < std::abs(gs->lanes[size_t(y)].offset); });
    return out;
}

void RtsModule::rebuildLanes(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    gs.lanes.clear();
    std::vector<Entity> nodes;
    w.each<Garrison, Footprint>([&](Entity e, Garrison&, Footprint&) { nodes.push_back(e); });
    const Fixed spacing = rules_.lanes.spacing;
    for (size_t i = 0; i < nodes.size(); ++i)
        for (size_t j = i + 1; j < nodes.size(); ++j) {
            Entity a = nodes[i], b = nodes[j];
            Vec3 ca = w.get<Transform>(a).pos, cb = w.get<Transform>(b).pos;
            Vec3 sa = exitCell(g, w.get<Footprint>(a), cb), sb = exitCell(g, w.get<Footprint>(b), ca);
            std::vector<Vec3> route;
            g.findPath(sa, sb, kLayerGround, route);
            std::vector<Vec3> ctrl{flat(ca), sa};
            for (auto& p : route) if ((p - ctrl.back()).xz().length() > Fixed::fromRatio(1, 4)) ctrl.push_back(flat(p));
            if ((sb - ctrl.back()).xz().length() > Fixed::fromRatio(1, 4)) ctrl.push_back(sb);
            ctrl.push_back(flat(cb));
            // spline through the control points, ~2 samples per cell
            std::vector<Vec3> main;
            for (size_t k = 0; k + 1 < ctrl.size(); ++k) {
                Vec3 p0 = ctrl[k ? k - 1 : 0], p1 = ctrl[k], p2 = ctrl[k + 1], p3 = ctrl[std::min(k + 2, ctrl.size() - 1)];
                int n = std::max<int>(1, int(((p2 - p1).xz().length() * 2).toInt()));
                for (int s = 0; s < n; ++s) main.push_back(catmullRom(p0, p1, p2, p3, Fixed::fromInt(s) / n));
            }
            main.push_back(flat(cb));
            for (int off = -rules_.lanes.left; off <= rules_.lanes.right; ++off) {
                Lane lane{a, b, off, {}};
                lane.points.reserve(main.size());
                for (size_t k = 0; k < main.size(); ++k) {
                    if (off == 0 || k == 0 || k + 1 == main.size()) { lane.points.push_back(main[k]); continue; }
                    Vec3 t = main[std::min(k + 1, main.size() - 1)] - main[k - 1];
                    Fixed tl = t.xz().length();
                    if (tl.raw <= 0) { lane.points.push_back(main[k]); continue; }
                    Vec3 right{-t.z / tl, Fixed{}, t.x / tl};
                    // ease in/out near the buildings so lanes fan out from the door
                    Fixed ease = fxMin(kOne, fxMin(Fixed::fromInt(int64_t(k)), Fixed::fromInt(int64_t(main.size() - 1 - k))) / 4);
                    Fixed d = spacing * off * ease;
                    Vec3 p = main[k] + right * d;
                    for (int tries = 0; tries < 4 && !g.passable(g.cellX(p.x), g.cellY(p.z), kLayerGround); ++tries) {
                        d = d / 2;                       // pushed back towards the main lane by obstacles
                        p = main[k] + right * d;
                    }
                    lane.points.push_back(p);
                }
                gs.lanes.push_back(std::move(lane));
            }
        }
    gs.lanesDirty = true;
    sim.emit({kEvLanesChanged, 0, kNullEntity, kNullEntity, int64_t(gs.lanes.size()), {}});
}

void RtsModule::sysLanes(Simulation& sim) {
    if (sim.tick() % 20 != 0) return;
    World& w = sim.world();
    GameState& gs = state(sim);
    uint32_t h = 2166136261u;
    auto mix = [&](uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= uint32_t(v >> (8 * i)) & 0xFF; h *= 16777619u; } };
    w.each<Garrison, Footprint>([&](Entity e, Garrison&, Footprint& f) { mix(e.packed()); mix(uint64_t(uint32_t(f.x)) | uint64_t(uint32_t(f.y)) << 32); });
    for (size_t i = 0; i < gs.grid.blocked.size(); ++i) if (gs.grid.blocked[i] || !(gs.grid.flags[i] & kCellGround)) mix(i);
    if (h == gs.laneSignature && !gs.lanes.empty()) return;
    gs.laneSignature = h;
    rebuildLanes(sim);
}

// Follows the lane polyline at the unit's speed; true when the end is reached.
bool RtsModule::runLaneWalk(Simulation& sim, Entity e, Order&) {
    World& w = sim.world();
    GameState& gs = state(sim);
    auto* lw = w.tryGet<LaneWalker>(e);
    if (!lw || lw->lane < 0 || size_t(lw->lane) >= gs.lanes.size()) return true;
    const auto& pts = gs.lanes[size_t(lw->lane)].points;
    Transform& tr = w.get<Transform>(e);
    Unit& u = w.get<Unit>(e);
    Fixed step = stat(sim, e, Stat::Speed) * sim.dt();
    if (rules_.advancedEffects) {
        // flying units leave the lane and go straight for the end of it
        if (statusFlags(sim, e) & kStFlying) {
            Vec3 goal = pts[lw->reverse ? 0 : pts.size() - 1];
            Vec3 d = flat(goal - tr.pos);
            Fixed len = d.length();
            if (len.raw > 0) u.facing = d / len;
            if (len <= step) { tr.pos = {goal.x, tr.pos.y, goal.z}; lw->pos = uint16_t(pts.size()); return true; }
            tr.pos = tr.pos + d * (step / len);
            return false;
        }
        // barriers on the lane stop hostile walkers; each walker that runs into one damages it and is spent
        bool blocked = false;
        const uint8_t me = w.get<Owner>(e).player;
        gs.hash.query(tr.pos, Fixed::fromInt(2), [&](Entity b) {
            if (blocked || !w.has<Unit>(b)) return;
            const Unit& bu = w.get<Unit>(b);
            if (!rules_.types[bu.type].blocksLanes || bu.hp.raw <= 0 || !hostile(sim, me, w.get<Owner>(b).player)) return;
            if (dist2D(w.get<Transform>(b).pos, tr.pos) <= rules_.types[bu.type].radius + rules_.types[u.type].radius + Fixed::fromRatio(1, 5)) {
                blocked = true;
                dealDamage(sim, b, rules_.types[u.type].weapons.empty() ? kOne : rules_.types[u.type].weapons[0].damage, 0, e, me);
                u.hp = Fixed{}; u.flags |= kUnitSilent;
            }
        });
        if (blocked) return false;
    }
    while (step.raw > 0 && lw->pos < pts.size()) {
        Vec3 wp = pts[lw->reverse ? pts.size() - 1 - lw->pos : lw->pos];
        Vec3 d = flat(wp - tr.pos);
        Fixed len = d.length();
        if (len.raw > 0) u.facing = d / len;
        if (len <= step) { tr.pos = {wp.x, tr.pos.y, wp.z}; step -= len; ++lw->pos; }
        else { tr.pos = tr.pos + d * (step / len); step = Fixed{}; }
    }
    return lw->pos >= pts.size();
}

} // namespace rts::game
