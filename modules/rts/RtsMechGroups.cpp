// agent-mech stage 2, groups: formations (line, box, wedge, loose; marching at the pace of the slowest),
// walls built in a line, merging two units into one (archon) and splitting one into several.
#include "RtsInternal.h"
#include <algorithm>
#include <cmath>

namespace rts::game {
using namespace detail;

namespace {
// Offsets in formation space: x to the right of the march direction, z behind the front rank.
std::vector<Vec3> shapeOffsets(int shape, int n, Fixed spacing) {
    std::vector<Vec3> out;
    if (shape == 2) {   // wedge: 1, 3, 5, ... per rank
        int row = 0, inRow = 0;
        for (int i = 0; i < n; ++i) {
            const int width = 2 * row + 1;
            out.push_back({spacing * (inRow - row), Fixed{}, spacing * row});
            if (++inRow == width) { ++row; inRow = 0; }
        }
        return out;
    }
    int cols = shape == 0 ? n : std::max(1, int(std::ceil(std::sqrt(double(n)))));   // tools-free: n is small, deterministic
    if (shape == 3) spacing = spacing * 2;
    for (int i = 0; i < n; ++i) {
        const int r = i / cols, c = i % cols;
        const int inRow = std::min(cols, n - r * cols);
        out.push_back({spacing * (2 * c - (inRow - 1)) / 2, Fixed{}, spacing * r});
    }
    return out;
}
} // namespace

bool RtsModule::applyMechCommand3(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    GameState& gs = state(sim);
    auto own = [&](Entity e) { return w.alive(e) && w.has<Unit>(e) && w.get<Owner>(e).player == cmd.player && w.get<Unit>(e).hp.raw > 0; };
    switch (cmd.type) {
    case kCmdFormation: {
        std::vector<Entity> us;
        for (Entity e : cmd.units) if (own(e) && w.has<Mobile>(e) && !(w.get<Unit>(e).flags & kUnitOffMap)) us.push_back(e);
        if (cmd.param < 0) { for (Entity e : us) if (w.has<Formation>(e)) w.remove<Formation>(e); return true; }
        if (us.size() < 2) return true;
        gs.mechActive = true;
        std::sort(us.begin(), us.end(), [](Entity a, Entity b) { return a.index < b.index; });
        Fixed maxR{};
        for (Entity e : us) maxR = fxMax(maxR, rules_.types[w.get<Unit>(e).type].radius);
        const auto offs = shapeOffsets(int(std::clamp<int64_t>(cmd.param, 0, 3)), int(us.size()), maxR * 2 + Fixed::fromRatio(2, 5));
        const uint32_t group = gs.mech.nextGroup++;
        for (size_t i = 0; i < us.size(); ++i) {
            if (!w.has<Formation>(us[i])) w.add<Formation>(us[i], Formation{});
            w.get<Formation>(us[i]) = Formation{group, uint8_t(cmd.param), offs[i]};
        }
        return true;
    }
    case kCmdMove:
    case kCmdAttack:
    case kCmdPatrol: {
        // a whole formation ordered to a point keeps its shape, facing the march direction
        if (cmd.units.size() < 2 || (cmd.type == kCmdAttack && w.alive(cmd.target))) return false;
        uint32_t group = 0;
        Vec3 c{};
        for (Entity e : cmd.units) {
            auto* f = own(e) ? w.tryGet<Formation>(e) : nullptr;
            if (!f || (group && f->group != group)) return false;
            group = f->group;
            c += w.get<Transform>(e).pos;
        }
        c = c / Fixed::fromInt(int64_t(cmd.units.size()));
        Vec3 dir = flat(cmd.point - c);
        Fixed dl = dir.length();
        dir = dl.raw > 0 ? dir / dl : Vec3{Fixed{}, Fixed{}, kOne};
        const Vec3 right{-dir.z, Fixed{}, dir.x};
        for (Entity e : cmd.units) {
            const Vec3 off = w.get<Formation>(e).offset;
            Order o;
            o.type = cmd.type == kCmdMove ? OrderType::Move : cmd.type == kCmdAttack ? OrderType::AttackMove : OrderType::Patrol;
            o.point = flat(cmd.point + right * off.x - dir * off.z);
            o.point2 = w.get<Transform>(e).pos;
            o.param = 1;
            issue(sim, e, o, cmd.flags & kCmdQueue);
        }
        return true;
    }
    case kCmdBuildLine: {
        const int type = int(cmd.param & 0xFFFF);
        if (type < 0 || size_t(type) >= rules_.types.size() || !(rules_.types[size_t(type)].mech.wall || rules_.types[size_t(type)].mech.beltSpeed.raw > 0)) return true;
        const bool belt = rules_.types[size_t(type)].mech.beltSpeed.raw > 0;
        const UnitType& t = rules_.types[size_t(type)];
        int x0 = gs.grid.cellX(cmd.point.x), y0 = gs.grid.cellY(cmd.point.z);
        int x1 = int((cmd.param >> 16) & 0xFFFF), y1 = int((cmd.param >> 32) & 0xFFFF);
        const int step = std::max(1, std::max(t.footprintW, t.footprintH));
        std::vector<Entity> builders;
        for (Entity e : cmd.units) {
            if (!own(e)) continue;
            auto& b = rules_.types[w.get<Unit>(e).type].builds;
            if (std::find(b.begin(), b.end(), uint16_t(type)) != b.end()) builders.push_back(e);
        }
        if (builders.empty()) return true;
        uint8_t dir = 1;
        {
            const int ddx = x1 - x0, ddy = y1 - y0;
            if (ddx != 0 || ddy != 0) dir = std::abs(ddx) >= std::abs(ddy) ? (ddx > 0 ? 1 : 3) : (ddy > 0 ? 2 : 4);
        }
        // Bresenham over cells, one segment every `step` cells
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy, k = 0;
        bool first = !(cmd.flags & kCmdQueue);
        for (int guard = 0; guard < 512; ++guard) {
            if (k++ % step == 0) {
                Vec3 centre = snapToFootprint(t, gs.grid.cellCenter(x0, y0));
                if (belt) {   // conveyors point along the line (its main axis)
                    auto& plan = gs.mech.beltPlan;
                    const int32_t c = gs.grid.index(x0, y0);
                    bool found = false;
                    for (auto& p : plan) if (p.first == c) { p.second = dir; found = true; }
                    if (!found) plan.push_back({c, dir});
                }
                if (canPlace(sim, cmd.player, uint16_t(type), centre)) {
                    for (Entity b : builders) {
                        Order o;
                        o.type = OrderType::Build; o.param = type; o.point = flat(centre);
                        issue(sim, b, o, !first);
                    }
                    first = false;
                }
            }
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
        return true;
    }
    case kCmdMerge: {
        std::vector<Entity> us;
        for (Entity e : cmd.units) if (own(e) && rules_.types[w.get<Unit>(e).type].mech.mergeResult != kNoType && !w.has<Merging>(e)) us.push_back(e);
        std::sort(us.begin(), us.end(), [](Entity a, Entity b) { return a.index < b.index; });
        std::vector<uint8_t> used(us.size(), 0);
        std::vector<std::pair<Entity, Entity>> pairs;
        for (size_t i = 0; i < us.size(); ++i) {
            if (used[i]) continue;
            const MechType& a = rules_.types[w.get<Unit>(us[i]).type].mech;
            size_t best = us.size(); Fixed bd{};
            for (size_t j = i + 1; j < us.size(); ++j) {
                if (used[j]) continue;
                const uint16_t tj = w.get<Unit>(us[j]).type;
                if (a.mergeWith != tj) continue;
                Fixed d = dist2D(w.get<Transform>(us[i]).pos, w.get<Transform>(us[j]).pos);
                if (best == us.size() || d < bd) { best = j; bd = d; }
            }
            if (best == us.size()) continue;
            used[i] = used[best] = 1;
            pairs.push_back({us[i], us[best]});
        }
        for (auto& [a, b] : pairs) {
            w.add<Merging>(a, Merging{b});
            w.add<Merging>(b, Merging{a});
            Order oa; oa.type = OrderType::Merge; oa.target = b; issue(sim, a, oa, false);
            Order ob; ob.type = OrderType::Merge; ob.target = a; issue(sim, b, ob, false);
        }
        if (!pairs.empty()) gs.mechActive = true;
        return true;
    }
    case kCmdSplit: {
        std::vector<Entity> us;
        for (Entity e : cmd.units) if (own(e) && rules_.types[w.get<Unit>(e).type].mech.splitType != kNoType) us.push_back(e);
        for (Entity e : us) {
            const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
            const Vec3 p = w.get<Transform>(e).pos;
            for (int k = 0; k < mt.splitCount; ++k) {
                static const int dx[8] = {1, -1, 0, 0, 1, -1, 1, -1}, dz[8] = {0, 0, 1, -1, 1, -1, -1, 1};
                spawn(sim, mt.splitType, cmd.player, p + Vec3{Fixed::fromRatio(dx[k % 8], 2), Fixed{}, Fixed::fromRatio(dz[k % 8], 2)});
            }
            Unit& u = w.get<Unit>(e);
            u.hp = Fixed{};
            u.flags |= kUnitSilent;
        }
        return true;
    }
    default: return applyMechEconomyCommand(sim, cmd);
    }
}

bool RtsModule::runMerge(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    if (!w.alive(o.target) || !w.has<Unit>(o.target) || w.get<Unit>(o.target).hp.raw <= 0) { if (w.has<Merging>(e)) w.remove<Merging>(e); return true; }
    if (contactDistance(sim, e, o.target) > Fixed::fromRatio(1, 2)) {
        auto* m = w.tryGet<Mobile>(e);
        if (!m) return true;
        const Vec3 tp = w.get<Transform>(o.target).pos;
        if (!m->moving || dist2D(m->goal, tp) > kHalf || (sim.tick() + e.index) % 10 == 0) moveTo(sim, e, tp, Fixed::fromRatio(1, 4));
        return false;
    }
    if (o.target.index < e.index) return false;   // the lower index performs the merge
    const uint16_t result = rules_.types[w.get<Unit>(e).type].mech.mergeResult;
    const uint8_t owner = w.get<Owner>(e).player;
    const Vec3 mid = (w.get<Transform>(e).pos + w.get<Transform>(o.target).pos) * Fixed::fromRatio(1, 2);
    for (Entity x : {e, o.target}) { Unit& u = w.get<Unit>(x); u.hp = Fixed{}; u.flags |= kUnitSilent; }
    Entity n = spawn(sim, result, owner, flat(mid));
    sim.emit({kEvMerged, owner, n, kNullEntity, result, mid});
    return true;
}

void RtsModule::mechTickGroups(Simulation& sim) {
    World& w = sim.world();
    auto& gsp = state(sim).mech.groupSpeed;
    gsp.clear();
    w.each<Formation, Unit>([&](Entity, Formation& f, Unit& u) {
        const Fixed sp = rules_.types[u.type].speed;
        for (auto& [g, s] : gsp) if (g == f.group) { s = fxMin(s, sp); return; }
        gsp.push_back({f.group, sp});
    });
}

} // namespace rts::game
