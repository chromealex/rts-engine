// Local avoidance: ORCA / RVO2-style reciprocal velocity obstacles in fixed point (no floats), plus
// the yield rule: idle allies in the way are asked to step to a free side cell (and walk back later)
// instead of being treated as walls, so a unit can pass a gap of standing units.
//
// Per moving agent: up to `movement.neighbors` nearest agents (spatial hash, sorted by distance then
// entity index) become half-plane constraints on its velocity; the 2D linear program (RVO2's
// linearProgram1/2/3) picks the velocity closest to the preferred one. Cost is O(agents * K^2).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
struct Line { Vec2 point, dir; };

const Fixed kEps = Fixed::fromRaw(16);

Fixed det(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
Vec2 normalize(Vec2 v) { Fixed l = v.length(); return l.raw > 0 ? v / l : Vec2{}; }

bool lp1(const std::vector<Line>& lines, size_t i, Fixed radius, Vec2 opt, bool dirOpt, Vec2& result) {
    const Line& L = lines[i];
    Fixed dot = L.point.dot(L.dir);
    Fixed disc = dot * dot + radius * radius - L.point.lengthSq();
    if (disc.raw < 0) return false;
    Fixed sq = fxSqrt(disc);
    Fixed tLeft = -dot - sq, tRight = -dot + sq;
    for (size_t j = 0; j < i; ++j) {
        Fixed den = det(L.dir, lines[j].dir);
        Fixed num = det(lines[j].dir, L.point - lines[j].point);
        if (fxAbs(den) <= kEps) {
            if (num.raw < 0) return false;
            continue;
        }
        Fixed t = num / den;
        if (den.raw >= 0) tRight = fxMin(tRight, t); else tLeft = fxMax(tLeft, t);
        if (tLeft > tRight) return false;
    }
    if (dirOpt) result = L.point + L.dir * (opt.dot(L.dir).raw > 0 ? tRight : tLeft);
    else result = L.point + L.dir * fxClamp(L.dir.dot(opt - L.point), tLeft, tRight);
    return true;
}

size_t lp2(const std::vector<Line>& lines, Fixed radius, Vec2 opt, bool dirOpt, Vec2& result) {
    if (dirOpt) result = opt * radius;
    else if (opt.lengthSq() > radius * radius) result = normalize(opt) * radius;
    else result = opt;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (det(lines[i].dir, lines[i].point - result).raw > 0) {
            Vec2 keep = result;
            if (!lp1(lines, i, radius, opt, dirOpt, result)) { result = keep; return i; }
        }
    }
    return lines.size();
}

void lp3(const std::vector<Line>& lines, size_t begin, Fixed radius, Vec2& result) {
    Fixed distance{};
    for (size_t i = begin; i < lines.size(); ++i) {
        if (det(lines[i].dir, lines[i].point - result) <= distance) continue;
        std::vector<Line> proj;
        for (size_t j = 0; j < i; ++j) {
            Line l;
            Fixed d = det(lines[i].dir, lines[j].dir);
            if (fxAbs(d) <= kEps) {
                if (lines[i].dir.dot(lines[j].dir).raw > 0) continue;
                l.point = (lines[i].point + lines[j].point) * kHalf;
            } else {
                l.point = lines[i].point + lines[i].dir * (det(lines[j].dir, lines[i].point - lines[j].point) / d);
            }
            l.dir = normalize(lines[j].dir - lines[i].dir);
            proj.push_back(l);
        }
        Vec2 keep = result;
        if (lp2(proj, radius, Vec2{-lines[i].dir.y, lines[i].dir.x}, true, result) < proj.size()) result = keep;
        distance = det(lines[i].dir, lines[i].point - result);
    }
}

// ORCA half-plane for agent A against B (RVO2 computeNewVelocity, agent part).
Line orcaLine(Vec2 pa, Vec2 va, Fixed ra, Vec2 pb, Vec2 vb, Fixed rb, Fixed invTH, Fixed invDt, Fixed responsibility) {
    Vec2 relPos = pb - pa, relVel = va - vb;
    Fixed distSq = relPos.lengthSq(), r = ra + rb, rSq = r * r;
    Line line;
    Vec2 u;
    if (distSq > rSq) {
        Vec2 w = relVel - relPos * invTH;
        Fixed wLenSq = w.lengthSq(), dot1 = w.dot(relPos);
        if (dot1.raw < 0 && dot1 * dot1 > rSq * wLenSq) {
            Fixed wLen = fxSqrt(wLenSq);
            Vec2 unitW = wLen.raw > 0 ? w / wLen : Vec2{Fixed::fromInt(1), Fixed{}};
            line.dir = {unitW.y, -unitW.x};
            u = unitW * (r * invTH - wLen);
        } else {
            Fixed leg = fxSqrt(distSq - rSq);
            if (det(relPos, w).raw > 0) line.dir = Vec2{relPos.x * leg - relPos.y * r, relPos.x * r + relPos.y * leg} / distSq;
            else line.dir = Vec2{-(relPos.x * leg + relPos.y * r), -(-relPos.x * r + relPos.y * leg)} / distSq;
            Fixed dot2 = relVel.dot(line.dir);
            u = line.dir * dot2 - relVel;
        }
    } else {
        // already overlapping: resolve within one time step
        Vec2 w = relVel - relPos * invDt;
        Fixed wLen = w.length();
        Vec2 unitW = wLen.raw > 0 ? w / wLen : Vec2{Fixed::fromInt(1), Fixed{}};
        line.dir = {unitW.y, -unitW.x};
        u = unitW * (r * invDt - wLen);
    }
    line.point = va + u * responsibility;
    return line;
}
} // namespace

void RtsModule::moveAgents(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const Fixed dt = sim.dt();
    const uint32_t tick = sim.tick();
    const Fixed invTH = kOne / fxMax(rules_.movement.timeHorizon, Fixed::fromRatio(1, 10));
    const Fixed invDt = kOne / dt;
    const int K = rules_.movement.neighbors;

    struct Agent { Entity e; Vec2 p, v, pref, nv; Fixed r, speed; uint8_t layer, owner; bool moving, idle; };
    std::vector<Agent> agents;
    std::vector<int32_t> byIndex;
    w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform& tr) {
        if (u.flags & (kUnitInHand | kUnitPossessed)) return;
        if (rules_.lanes.enabled && w.has<LaneWalker>(e)) return;
        const UnitType& t = rules_.types[u.type];
        Agent a{e, tr.pos.xz(), m.vel.xz(), {}, {}, t.radius, Fixed{}, t.layer, w.get<Owner>(e).player, false, false};
        auto* b = w.tryGet<Buffs>(e);
        const bool stunned = b && b->stunUntil > tick;
        if (m.moving && !stunned) {
            if (m.goalRadius.raw > 0 && dist2D(tr.pos, m.goal) <= m.goalRadius) { m.moving = false; m.path.clear(); }
            Fixed speed = stat(sim, e, Stat::Speed);
            int cx = g.cellX(tr.pos.x), cy = g.cellY(tr.pos.z);
            if (rules_.roadNetwork.enabled && g.inside(cx, cy) && (g.flags[size_t(g.index(cx, cy))] & kCellRoad)) speed = speed * rules_.roadNetwork.speed;
            a.speed = speed;
            while (m.moving && m.pathPos < m.path.size()) {
                Vec2 d = (m.path[m.pathPos] - tr.pos).xz();
                Fixed len = d.length();
                if (len > Fixed::fromRatio(1, 20)) {
                    if (g.hf && (t.layer & kLayerGround)) a.speed = speed = speed * slopeSpeed(sim, tr.pos, d / len); // uphill slower
                    a.pref = d / len * fxMin(speed, len * invDt);
                    break;
                }
                if (++m.pathPos >= m.path.size()) { m.moving = false; m.path.clear(); }
            }
            if (m.pathPos >= m.path.size() && m.moving) { m.moving = false; m.path.clear(); }
            a.moving = m.moving;
        }
        if (!a.moving) { m.vel = {}; a.v = {}; }
        auto* ord = w.tryGet<Orders>(e);
        a.idle = !a.moving && (!ord || ord->queue.empty()) && !(u.flags & kUnitHold) && !(w.tryGet<Combat>(e) && w.alive(w.get<Combat>(e).target));
        if (byIndex.size() <= e.index) byIndex.resize(e.index + 1, -1);
        byIndex[e.index] = int32_t(agents.size());
        agents.push_back(a);
    });

    struct YieldReq { Entity who, mover; Vec2 moverPos, dir; Fixed clear; };
    std::vector<YieldReq> yields;
    std::vector<Line> lines;
    std::vector<std::pair<Fixed, int32_t>> nb;
    for (auto& a : agents) {
        a.nv = a.pref;
        if (!a.moving) continue;
        const bool air = a.layer & kLayerAir;
        Fixed reach = a.speed * rules_.movement.timeHorizon + a.r + Fixed::fromRatio(3, 2);
        nb.clear();
        gs.hash.query({a.p.x, Fixed{}, a.p.y}, reach, [&](Entity o) {
            if (o == a.e || o.index >= byIndex.size() || byIndex[o.index] < 0) return;
            const Agent& b = agents[size_t(byIndex[o.index])];
            if (!(b.layer & a.layer) && !((b.layer | a.layer) == (kLayerGround | kLayerSandOnly))) return;
            Fixed d2 = (b.p - a.p).lengthSq();
            if (d2 > reach * reach) return;
            nb.push_back({d2, byIndex[o.index]});
        });
        std::sort(nb.begin(), nb.end(), [&](auto& x, auto& y) { return x.first != y.first ? x.first < y.first : agents[size_t(x.second)].e.index < agents[size_t(y.second)].e.index; });
        lines.clear();
        Vec2 dir = normalize(a.pref);
        int used = 0;
        for (auto& [d2, bi] : nb) {
            if (used >= K) break;
            const Agent& b = agents[size_t(bi)];
            const bool ally = b.owner == a.owner || (b.owner != 0 && a.owner != 0 && b.owner != kHostileNeutral && a.owner != kHostileNeutral && sim.allied(a.owner, b.owner));
            if (ally && b.idle && !air) {
                // idle ally in the way: ask it to step aside, push through meanwhile (separation)
                Vec2 rel = b.p - a.p;
                Fixed ahead = rel.dot(dir), side = fxAbs(det(dir, rel)), clear = a.r + b.r;
                if (ahead.raw > 0 && ahead < clear + fxMax(kHalf, a.speed) && side < clear && (tick + a.e.index) % 5 == 0)
                    yields.push_back({b.e, a.e, a.p, dir, clear});
                continue;
            }
            ++used;
            Fixed resp = b.moving ? kHalf : kOne;   // standing units don't dodge: take the whole avoidance
            lines.push_back(orcaLine(a.p, a.v, a.r, b.p, b.v, b.r, invTH, invDt, resp));
        }
        if (lines.empty()) continue;
        Vec2 res;
        size_t fail = lp2(lines, a.speed, a.pref, false, res);
        if (fail < lines.size()) lp3(lines, fail, a.speed, res);
        a.nv = res;
    }

    // integrate with the grid (slide along walls), advance waypoints, detect being stuck
    for (auto& a : agents) {
        if (!a.moving) continue;
        Mobile& m = w.get<Mobile>(a.e);
        Transform& tr = w.get<Transform>(a.e);
        Unit& u = w.get<Unit>(a.e);
        const UnitType& t = rules_.types[u.type];
        const bool ignoreGrid = (t.layer & kLayerAir) || (gs.roadsOnly && t.entersGarrison);
        Vec3 step{a.nv.x * dt, Fixed{}, a.nv.y * dt};
        auto ok = [&](Vec3 np) {
            if (ignoreGrid) return true;
            int cx0 = g.cellX(tr.pos.x), cy0 = g.cellY(tr.pos.z), cx = g.cellX(np.x), cy = g.cellY(np.z);
            if (cx == cx0 && cy == cy0) return true;
            if (!g.passable(cx0, cy0, t.pathLayer)) return true; // leaving a blocked cell
            if (cx != cx0 && cy != cy0) return g.canStep(cx0, cy0, cx, cy0, t.pathLayer) && g.canStep(cx, cy0, cx, cy, t.pathLayer) && g.canStep(cx0, cy0, cx, cy, t.pathLayer);
            return g.canStep(cx0, cy0, cx, cy, t.pathLayer);
        };
        Vec3 np = tr.pos + step;
        if (!ok(np)) {
            Vec3 sx = tr.pos + Vec3{step.x, Fixed{}, Fixed{}}, sz = tr.pos + Vec3{Fixed{}, Fixed{}, step.z};
            np = ok(sx) ? sx : ok(sz) ? sz : tr.pos;
        }
        Vec3 moved = np - tr.pos;
        if (moved.length().raw > 0) u.facing = moved / moved.length();
        tr.pos = np;
        m.vel = moved * invDt;
        // waypoint reached?
        if (m.pathPos < m.path.size()) {
            Vec2 d = (m.path[m.pathPos] - tr.pos).xz();
            if (d.length() <= fxMax(a.speed * dt, Fixed::fromRatio(1, 10)) || d.dot(a.pref).raw < 0) {
                if (++m.pathPos >= m.path.size()) { m.moving = false; m.path.clear(); m.vel = {}; }
            }
        }
        // stuck behind something that doesn't yield: re-path around standing units
        if (moved.length() * invDt < a.pref.length() / 5) m.stuck += dt;
        else m.stuck = fxMax(Fixed{}, m.stuck - dt);
        if (m.stuck > Fixed::fromRatio(3, 2) && m.moving) {
            m.stuck = Fixed{};
            g.findPath(tr.pos, m.goal, t.pathLayer, m.path, 40000, MapGrid::clearanceFor(t.radius), true);
            m.pathPos = 0;
            m.moving = !m.path.empty();
        }
    }

    // yield: step to a free side cell away from the mover's line, come back later
    std::sort(yields.begin(), yields.end(), [](const YieldReq& x, const YieldReq& y) { return x.who.index != y.who.index ? x.who.index < y.who.index : x.mover.index < y.mover.index; });
    Entity last;
    for (auto& y : yields) {
        if (y.who == last || !w.alive(y.who)) continue;
        last = y.who;
        Mobile& m = w.get<Mobile>(y.who);
        if (m.yieldUntil > tick || m.moving) continue;
        Vec3 p = w.get<Transform>(y.who).pos;
        int cx = g.cellX(p.x), cy = g.cellY(p.z);
        const UnitType& t = rules_.types[w.get<Unit>(y.who).type];
        int best = -1;
        Fixed bestScore{};
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                int x = cx + dx, z = cy + dy;
                if ((dx == 0 && dy == 0) || !g.passable(x, z, t.pathLayer)) continue;
                size_t i = size_t(g.index(x, z));
                if (g.soft[i]) continue;                                   // someone stands there
                Vec2 c = g.cellCenter(x, z).xz();
                Fixed side = fxAbs(det(y.dir, c - y.moverPos));
                Fixed score = fxMin(side, y.clear + kHalf) * 4 - (c - p.xz()).length() - (y.dir.dot(c - y.moverPos).raw < 0 ? kOne : Fixed{});
                if (best < 0 || score > bestScore) { best = int(i); bestScore = score; }
            }
        if (best < 0) continue;
        if (!m.yielded) m.yieldHome = flat(p);
        m.yielded = true;
        m.yieldUntil = tick + 2 * sim.tickRate();
        moveTo(sim, y.who, g.cellCenter(best % g.w, best / g.w), Fixed::fromRatio(1, 5));
        if (byIndex.size() > y.who.index && byIndex[y.who.index] >= 0) g.soft[size_t(best)] = 1;
    }
    // walk back to where they stood once the way is clear
    if (tick % 10 == 0)
        for (auto& a : agents) {
            Mobile& m = w.get<Mobile>(a.e);
            if (!m.yielded || m.moving || tick < m.yieldUntil) continue;
            auto* ord = w.tryGet<Orders>(a.e);
            if (ord && !ord->queue.empty()) { m.yielded = false; continue; }
            bool busy = false;
            for (auto& b : agents) if (b.moving && (b.p - m.yieldHome.xz()).length() < b.r + a.r + kHalf) busy = true;
            if (busy) { m.yieldUntil = tick + sim.tickRate(); continue; }
            m.yielded = false;
            moveTo(sim, a.e, m.yieldHome, Fixed::fromRatio(1, 5));
        }
}

} // namespace rts::game
