// Computer player, agent-ai-terrain: terrain-aware army control on top of RtsAI's economy.
//   - bases: our anchor and the nearest enemy base; terrain routes between them (RtsAITerrain), recomputed
//     when the enemy base moves or every two minutes;
//   - rally on high ground in front of the base, waves along waypoints of the route (climbing into the enemy
//     and cells overlooked by higher ground cost extra), a siege spot on high ground before the target,
//     holding back instead of charging uphill into a stronger enemy;
//   - defence of the base, a scout on the highest post along the route, towers at the chokepoint;
//   - escalation when nothing breaks for a while: bigger waves, siege / long range units, economy targets,
//     alternate routes, no retreat;
//   - planets: orbit fleets gathered and sent together to the body with enemy structures, fighting there
//     until the body is clear (separate-planet layouts have no other way to win);
//   - build spots chosen by the bot (lanes between dropoffs and resources kept free, random tie-breaks so
//     mirrored starts don't favour one side), stuck builders released.
#include "RtsAI.h"
#include "RtsAITerrain.h"
#include "../../engine/core/Archive.h"
#include <algorithm>

namespace rts::game {

namespace {
Fixed dist(Vec3 a, Vec3 b) { return Surface::onSphere(a) ? (a - b).length() : (a - b).xz().length(); }
bool controllable(World& w, Entity e, const Unit& u) {
    if (u.flags & (kUnitOffMap | kUnitPossessed)) return false;
    auto* n = w.tryGet<Needs>(e);
    return !n || !n->leaving;
}
bool idleOrders(const Orders& ord) {
    return ord.queue.empty() || (ord.queue.front().type == OrderType::Need && (ord.queue.front().param & 0xFF) == kNeedWork);
}
int64_t strengthOf(const UnitType& t, const Unit& u) {
    Fixed dps{};
    for (auto& wd : t.weapons) dps += wd.damage / fxMax(wd.cooldown, Fixed::fromRatio(1, 10));
    return (u.hp.toInt() + 1) * (dps.toInt() + 1);
}
Fixed bestRange(const UnitType& t) {
    Fixed r{};
    for (auto& wd : t.weapons) r = fxMax(r, wd.range);
    return r;
}
} // namespace

void RtsAI::applyRulesConfig(RtsModule& m) {
    if (configured_) return;
    configured_ = true;
    const AIRules& a = m.rules().ai;
    if (a.workersPerBase >= 0) cfg_.workersPerBase = a.workersPerBase;
    if (a.firstAttackArmy >= 0) cfg_.firstAttackArmy = a.firstAttackArmy;
    if (a.armyGrowth >= 0) cfg_.armyGrowth = a.armyGrowth;
    if (a.maxAttackArmy >= 0) cfg_.maxAttackArmy = a.maxAttackArmy;
    if (a.towers >= 0) cfg_.towers = a.towers;
    if (a.scouts >= 0) cfg_.scouts = a.scouts;
    if (a.invasionSize >= 0) cfg_.invasionSize = a.invasionSize;
    if (a.climbWeight >= 0) cfg_.climbWeight = a.climbWeight;
    if (a.exposureWeight >= 0) cfg_.exposureWeight = a.exposureWeight;
    if (a.escalateSeconds >= 0) cfg_.escalateSeconds = uint32_t(a.escalateSeconds);
    if (a.terrain >= 0) cfg_.terrain = a.terrain != 0;
}

void RtsAI::ensureTerrain(Simulation& sim, RtsModule& m) {
    if (terr_) return;
    auto t = std::make_shared<AITerrain>();
    t->build(sim, m);
    terr_ = t;
}

// Our anchor (key structure, else the first structure) and the nearest enemy base; routes and the posts on them.
void RtsAI::refreshBases(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    const MapGrid& g = m.state(sim).grid;
    Entity anchor;
    bool anchorKey = false;
    w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        if (o.player != player_ || !R.types[u.type].structure || (u.flags & kUnitOffMap)) return;
        const bool k = R.types[u.type].key;
        if (!anchor.valid() || (k && !anchorKey)) { anchor = e; anchorKey = k; }
    });
    if (!anchor.valid()) {   // no structures (MCV, commander only): the first own unit
        w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) { if (o.player == player_ && !anchor.valid() && !(u.flags & kUnitOffMap)) anchor = e; });
    }
    hasHome_ = anchor.valid();
    if (!hasHome_) return;
    home_ = w.get<Transform>(anchor).pos;
    homeCell_ = g.cellOf(home_);
    // nearest enemy base: their key structure (else any structure) closest to us, same body first
    const Surface* S = m.surface(sim);
    const int hb = S ? S->bodyAt(home_) : 0;
    Entity best;
    Fixed bestD{};
    bool bestSame = false;
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player == kHostileNeutral || !m.hostile(sim, player_, o.player) || u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
        const UnitType& t = R.types[u.type];
        if (!t.structure && !t.key) return;
        const bool same = !S || S->bodyAt(tr.pos) == hb;
        Fixed d = dist(tr.pos, home_) - (t.key ? Fixed::fromInt(6) : Fixed{});
        if (!same) d = Fixed::fromInt(100000) + (tr.pos - home_).length();
        if (!best.valid() || d < bestD) { best = e; bestD = d; bestSame = same; }
    });
    hasEnemy_ = best.valid();
    enemyOffBody_ = hasEnemy_ && !bestSame;
    if (!hasEnemy_) return;
    enemyHome_ = w.get<Transform>(best).pos;
    enemyCell_ = bestSame ? g.cellOf(enemyHome_) : -1;
    if (!cfg_.terrain || !terr_ || enemyCell_ < 0 || homeCell_ < 0) { mainRoute_.clear(); altRoute_.clear(); return; }
    const uint32_t now = sim.tick();
    const bool moved = routeEnemyCell_ < 0 || dist(g.centerOf(routeEnemyCell_), enemyHome_) > Fixed::fromInt(8);
    if (!moved && now < routeTick_ + 120 * sim.tickRate() && !mainRoute_.empty()) return;
    routeTick_ = now;
    routeEnemyCell_ = enemyCell_;
    const AITerrain& T = *terr_;
    T.route(g, homeCell_, enemyCell_, cfg_.climbWeight * 10, cfg_.exposureWeight * 10, nullptr, mainRoute_);
    // alternate route: the main route's corridor (3 cells around it) costs extra
    altRoute_.clear();
    if (!mainRoute_.empty()) {
        std::vector<uint16_t> avoid(size_t(T.cells), 0);
        for (size_t i = mainRoute_.size() / 5; i + mainRoute_.size() / 5 < mainRoute_.size(); ++i)
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) { const int n = g.offsetCell(mainRoute_[i], dx, dy); if (n >= 0) avoid[size_t(n)] = 60; }
        T.route(g, homeCell_, enemyCell_, cfg_.climbWeight * 10, cfg_.exposureWeight * 10, &avoid, altRoute_);
        if (altRoute_.size() > mainRoute_.size() * 8 / 5) altRoute_.clear();   // much longer: not worth it
    }
    // posts along the main route
    hasRally_ = false; chokeCell_ = -1; watchCell_ = -1;
    const int n = int(mainRoute_.size());
    if (n < 4) return;
    auto around = [&](int idx, int r, auto&& score) {   // best walkable cell within r of route cell idx
        int bc = -1;
        int64_t bs = 0;
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const int c = g.offsetCell(mainRoute_[size_t(idx)], dx, dy);
                if (c < 0 || !T.pass[size_t(c)] || g.blocked[size_t(c)]) continue;
                const int64_t s = score(c) * 8 + int64_t(rng_.below(8));
                if (bc < 0 || s > bs) { bc = c; bs = s; }
            }
        return bc;
    };
    // rally: 9..14 steps out of the base, the highest well-covered ground near the route
    {
        int bestC = -1;
        int64_t bestS = 0;
        for (int i = std::min(9, n / 3); i <= std::min(14, n / 3 + 2) && i < n; i += 1) {
            const int c = around(i, 3, [&](int c) { return int64_t(T.elev[size_t(c)]) * 4 - int64_t(T.over[size_t(c)]) * 4 + T.clear[size_t(c)] * 8; });
            if (c < 0) continue;
            const int64_t s = int64_t(T.elev[size_t(c)]) * 4 - int64_t(T.over[size_t(c)]) * 4 + T.clear[size_t(c)] * 8;
            if (bestC < 0 || s > bestS) { bestC = c; bestS = s; }
        }
        if (bestC >= 0) { rally_ = g.centerOf(bestC); hasRally_ = true; }
    }
    // chokepoint on our side: the narrowest cell in the first 45% of the route (outside the base)
    {
        int bestW = 99;
        for (int i = 6; i < n * 45 / 100; ++i) {
            const int c = mainRoute_[size_t(i)];
            if (T.clear[size_t(c)] < bestW) { bestW = T.clear[size_t(c)]; chokeCell_ = c; }
        }
        if (bestW > 4) chokeCell_ = -1;
    }
    // watch post: the most prominent ground in the middle of the route (on flat maps: 40% of the way)
    if (T.relief) {
        int64_t bs = 0;
        for (int i = n / 4; i <= n * 65 / 100; i += 2) {
            const int c = around(i, 5, [&](int c) { return int64_t(T.crest[size_t(c)]) * 4 + T.elev[size_t(c)] - int64_t(T.over[size_t(c)]) * 4; });
            if (c < 0) continue;
            const int64_t s = int64_t(T.crest[size_t(c)]) * 4 + T.elev[size_t(c)] - int64_t(T.over[size_t(c)]) * 4;
            if (watchCell_ < 0 || s > bs) { watchCell_ = c; bs = s; }
        }
    } else watchCell_ = mainRoute_[size_t(n * 2 / 5)];
}

// Build spot near `near`: valid placement with a one-cell gap, off the lanes between dropoffs and resources,
// close to `near` (behind it, away from the enemy, unless defensive: then on high ground), random tie-breaks.
bool RtsAI::findSpot(Simulation& sim, RtsModule& m, uint16_t type, Vec3 near, int radius, Vec3& out, bool defensive) {
    World& w = sim.world();
    const Rules& R = m.rules();
    MapGrid& g = m.state(sim).grid;
    const Surface* S = m.surface(sim);
    const UnitType& t = R.types[type];
    const int c0 = g.cellOf(near);
    if (c0 < 0) return false;
    // lanes: straight lines from own dropoffs to resource nodes within 14 cells of them
    std::vector<int> laneCells;
    if (!defensive && !S) {
        std::vector<Vec3> drops, nodes;
        w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
            const UnitType& ut = R.types[u.type];
            if (o.player == player_ && ut.structure && ut.dropoffMask && dist(tr.pos, near) < Fixed::fromInt(radius + 16)) drops.push_back(tr.pos);
            if (w.has<ResourceStock>(e) && dist(tr.pos, near) < Fixed::fromInt(radius + 20)) nodes.push_back(tr.pos);
        });
        for (Vec3 d : drops)
            for (Vec3 nd : nodes) {
                if (dist(d, nd) > Fixed::fromInt(14)) continue;
                int x0 = g.cellX(d.x), y0 = g.cellY(d.z), x1 = g.cellX(nd.x), y1 = g.cellY(nd.z);
                const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
                for (int s = 0; s <= steps; ++s) {
                    const int x = x0 + (steps ? (x1 - x0) * s / steps : 0), y = y0 + (steps ? (y1 - y0) * s / steps : 0);
                    for (int oy = -1; oy <= 1; ++oy)   // workers need a corridor, not a line
                        for (int ox = -1; ox <= 1; ++ox)
                            if (g.inside(x + ox, y + oy)) laneCells.push_back(g.index(x + ox, y + oy));
                }
            }
        std::sort(laneCells.begin(), laneCells.end());
        laneCells.erase(std::unique(laneCells.begin(), laneCells.end()), laneCells.end());
    }
    auto onLane = [&](int x0, int y0) {
        int hits = 0;
        for (int y = y0 - 1; y <= y0 + t.footprintH; ++y)
            for (int x = x0 - 1; x <= x0 + t.footprintW; ++x)
                if (g.inside(x, y) && std::binary_search(laneCells.begin(), laneCells.end(), g.index(x, y))) ++hits;
        return hits;
    };
    Vec2 away{};
    if (hasEnemy_ && !enemyOffBody_) {
        away = S ? S->toPlane(near, near - enemyHome_).xz() : (near - enemyHome_).xz();
        const Fixed al = away.length();
        if (al.raw > 0) away = away / al;
    }
    const uint32_t now = sim.tick();
    bool found = false;
    int64_t bestScore = 0;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            Vec3 p;
            if (S) {
                const int n = S->offset(c0, dx, dy);
                if (n < 0) continue;
                p = RtsModule::snapToFootprint(t, S->cellCenter(n), S);
            } else {
                int cx, cy; g.cellXY(near, cx, cy);
                p = RtsModule::snapToFootprint(t, {Fixed::fromInt(cx + dx) + Fixed::fromRatio(1, 2), Fixed{}, Fixed::fromInt(cy + dy) + Fixed::fromRatio(1, 2)});
            }
            int x0, y0;
            RtsModule::footprintAt(t, p, x0, y0, S);
            if (x0 < 0 && S) continue;
            const int gap = defensive ? 0 : 1;   // towers / outposts may stand close to trees and cliffs
            if (!g.areaBuildable(x0 - gap, y0 - gap, t.footprintW + 2 * gap, t.footprintH + 2 * gap)) continue;
            const int pc = g.cellOf(p);
            bool bad = false;
            for (auto& [c, until] : badSpots_) if (until > now && c == pc) bad = true;
            if (bad || !m.canPlace(sim, player_, type, p)) continue;
            if (!defensive && hasRally_ && dist(p, rally_) < Fixed::fromInt(5)) continue;   // the army gathers there
            const Vec2 d = S ? S->toPlane(near, p - near).xz() : (p - near).xz();
            int64_t score = (d.length() * 16).toInt();
            if (defensive) {
                if (terr_ && pc >= 0) score -= int64_t(terr_->elev[size_t(pc)]) * 16 / AITerrain::kUnit;   // a level up = one cell closer
            } else {
                score -= (d.dot(away) * 8).toInt();   // behind the core
                if (!laneCells.empty()) score += int64_t(onLane(x0, y0)) * 40;
            }
            score = score * 8 + int64_t(rng_.below(8));
            if (!found || score < bestScore) { bestScore = score; out = p; found = true; }
        }
    return found;
}

// Builders that walk to a site for too long (unreachable, blocked) are stopped and the spot is avoided.
void RtsAI::watchBuilders(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const uint32_t now = sim.tick(), limit = 40 * sim.tickRate();
    std::vector<StuckBuild> next;
    w.each<Unit, Owner, Orders>([&](Entity e, Unit&, Owner& o, Orders& ord) {
        if (o.player != player_ || ord.queue.empty()) return;
        const Order& f = ord.queue.front();
        if (f.type != OrderType::Build) return;
        if (f.target.valid()) {
            // building but no progress (the builder can't get to the site, our own idle units stand around it):
            // clear the idle units off after 15 s, give up on the site after 60 s
            auto* cs = w.tryGet<Construction>(f.target);
            if (!cs || !w.alive(f.target)) return;
            StuckBuild sb{e.packed(), now, w.get<Transform>(f.target).pos, cs->progress};
            for (auto& old : buildWatch_)
                if (old.entity == sb.entity && old.point == sb.point && old.progress == sb.progress) sb.since = old.since;
            if (now > sb.since + 15 * sim.tickRate() && now <= sb.since + 15 * sim.tickRate() + cfg_.thinkTicks * 2) {
                std::vector<Entity> off;
                w.each<Unit, Owner, Transform, Orders>([&](Entity u, Unit& uu, Owner& uo, Transform& ut, Orders& uord) {
                    if (uo.player != player_ || u == e || m.rules().types[uu.type].structure || !uord.queue.empty()) return;
                    if (dist(ut.pos, sb.point) < Fixed::fromInt(4)) off.push_back(u);
                });
                if (!off.empty()) {
                    Vec3 away = hasRally_ && dist(rally_, sb.point) > Fixed::fromInt(6) ? rally_ : home_;
                    Command c; c.type = kCmdMove; c.units = off; c.point = away; send(sim, c);
                }
            }
            if (now > sb.since + 60 * sim.tickRate()) {
                Command c; c.type = kCmdStop; c.units = {e}; send(sim, c);
                return;
            }
            next.push_back(sb);
            return;
        }
        if (f.state > 1) return;
        StuckBuild sb{e.packed(), now, f.point, Fixed{}};
        for (auto& old : buildWatch_)
            if (old.entity == sb.entity && old.point == f.point && old.progress.raw == 0) sb.since = old.since;
        if (now > sb.since + limit) {
            Command c; c.type = kCmdStop; c.units = {e}; send(sim, c);
            badSpots_.push_back({m.state(sim).grid.cellOf(f.point), now + 180 * sim.tickRate()});
            return;
        }
        next.push_back(sb);
    });
    buildWatch_ = next;
    badSpots_.erase(std::remove_if(badSpots_.begin(), badSpots_.end(), [&](auto& b) { return b.second <= now; }), badSpots_.end());
}

// Towers: a defensive structure or two at the chokepoint toward the enemy (or in front of the base); on
// planets whose enemies come only from orbit, next to the base with weapons that reach the orbit.
bool RtsAI::buildTowers(Simulation& sim, RtsModule& m) {
    if (cfg_.towers <= 0 || !hasHome_ || !hasEnemy_) return false;
    World& w = sim.world();
    const Rules& R = m.rules();
    const MapGrid& g = m.state(sim).grid;
    int towers = 0;
    std::vector<uint8_t> canBuild(R.types.size(), 0);
    w.each<Unit, Owner>([&](Entity, Unit& u, Owner& o) {
        if (o.player != player_) return;
        const UnitType& t = R.types[u.type];
        if (t.structure && !t.weapons.empty() && t.produces.empty()) ++towers;
        for (uint16_t b : t.builds) canBuild[b] = 1;
    });
    // only after the enemy came to our base, and not in a stalemate (towers there only make it last)
    if (towers >= cfg_.towers || !raided_ || escalation_ > 0) return false;
    const uint8_t want = enemyOffBody_ ? kLayerOrbit : kLayerGround;
    int pick = -1;
    int64_t bestCost = 0;
    for (size_t ti = 0; ti < R.types.size(); ++ti) {
        const UnitType& t = R.types[ti];
        if (!canBuild[ti] || !t.structure || t.weapons.empty() || !t.produces.empty() || t.placeOn != kNoType) continue;
        bool hits = false;
        for (auto& wd : t.weapons) if (wd.targets & want) hits = true;
        if (!hits || !m.canTrain(sim, player_, uint16_t(ti))) continue;
        int64_t cost = 0;
        for (auto& a : t.cost) cost += a.value.toInt();
        if (pick < 0 || cost < bestCost) { pick = int(ti); bestCost = cost; }
    }
    if (pick < 0) return false;
    Vec3 at = home_;
    if (!enemyOffBody_) {
        if (chokeCell_ >= 0 && dist(g.centerOf(chokeCell_), home_) < Fixed::fromInt(24)) at = g.centerOf(chokeCell_);
        else if (mainRoute_.size() > 12) at = g.centerOf(mainRoute_[std::min<size_t>(8, mainRoute_.size() / 3)]);
    }
    Vec3 spot;
    if (!findSpot(sim, m, uint16_t(pick), at, 5, spot, true) && !findSpot(sim, m, uint16_t(pick), home_, 8, spot, true)) return false;
    return build(sim, m, uint16_t(pick), 0, &spot);
}

// Weighted choice among trainable combat units: long range / siege when escalating, orbit units when the
// enemy can only be reached through orbit.
int RtsAI::pickUnit(Simulation& sim, RtsModule& m, const std::vector<uint16_t>& opts) {
    (void)sim;
    const Rules& R = m.rules();
    std::vector<int64_t> wt(opts.size(), 4);
    bool anyOrbit = false;
    for (uint16_t o : opts) if (R.types[o].layer & kLayerOrbit) anyOrbit = true;
    for (size_t i = 0; i < opts.size(); ++i) {
        const UnitType& t = R.types[opts[i]];
        if (escalation_ > 0 && !t.structure) wt[i] += bestRange(t).toInt() / 2;   // stalemate: a little more reach
        if (enemyOffBody_ && anyOrbit) wt[i] = (t.layer & kLayerOrbit) ? wt[i] * 12 : 1;
    }
    int64_t total = 0;
    for (auto v : wt) total += v;
    int64_t r = int64_t(rng_.below(uint32_t(std::max<int64_t>(1, total))));
    for (size_t i = 0; i < opts.size(); ++i) { if (r < wt[i]) return opts[i]; r -= wt[i]; }
    return opts.back();
}

// Planets: orbit fleets invade other bodies. Returns true when it used units.
bool RtsAI::planetOps(Simulation& sim, RtsModule& m, std::vector<Entity>& idleArmy) {
    const Surface* S = m.surface(sim);
    if (!S || !hasHome_) return false;
    World& w = sim.world();
    const Rules& R = m.rules();
    const uint32_t now = sim.tick();
    const int nb = int(S->bodies.size());
    std::vector<int> enemyStructs(size_t(nb), 0);
    std::vector<uint8_t> hasAny(size_t(nb), 0);
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
        if (o.player == kHostileNeutral || !m.hostile(sim, player_, o.player) || u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
        const int b = S->bodyAt(tr.pos);
        if (b < 0 || b >= nb) return;
        if (R.types[u.type].structure) enemyStructs[size_t(b)]++;
        hasAny[size_t(b)] = 1;
    });
    const int hb = S->bodyAt(home_);
    bool used = false;
    // keep invaders busy: in transit -> wait; on the target body -> attack the nearest enemy there; body clear -> next
    if (!invaders_.empty()) {
        if (invadeBody_ >= 0 && invadeBody_ < nb && !hasAny[size_t(invadeBody_)]) {
            int next = -1;
            for (int b = 0; b < nb; ++b) if (enemyStructs[size_t(b)] > 0 && (next < 0 || enemyStructs[size_t(b)] > enemyStructs[size_t(next)])) next = b;
            invadeBody_ = next >= 0 ? next : hb;
        }
        std::vector<Entity> transit;
        for (Entity e : invaders_) {
            const Unit& u = w.get<Unit>(e);
            if (u.flags & kUnitInTransit) continue;
            auto* ord = w.tryGet<Orders>(e);
            if (!ord || !idleOrders(*ord)) continue;
            const Vec3 p = w.get<Transform>(e).pos;
            const int b = S->bodyAt(p);
            if (b != invadeBody_) { if (m.canTransit(sim, e, invadeBody_)) transit.push_back(e); continue; }
            if (!hasAny[size_t(b)]) continue;
            // nearest enemy structure on this body (else any enemy)
            Entity best; Fixed bd{};
            w.each<Unit, Owner, Transform>([&](Entity t, Unit& tu, Owner& o, Transform& tr) {
                if (o.player == kHostileNeutral || !m.hostile(sim, player_, o.player) || tu.hp.raw <= 0 || (tu.flags & kUnitOffMap)) return;
                if (S->bodyAt(tr.pos) != b) return;
                const Fixed d = dist(tr.pos, p) - (R.types[tu.type].structure ? Fixed::fromInt(4) : Fixed{});
                if (!best.valid() || d < bd) { best = t; bd = d; }
            });
            if (best.valid()) { Command c; c.type = kCmdAttack; c.units = {e}; c.point = w.get<Transform>(best).pos; send(sim, c); }
        }
        if (!transit.empty()) { Command t; t.type = kCmdTransit; t.units = transit; t.param = invadeBody_; send(sim, t); }
        if (invadeBody_ == hb) invaders_.clear();   // nothing left elsewhere: they become home units again
    }
    // launch: enough idle orbit units at home and a body with enemy structures elsewhere
    int target = -1;
    for (int b = 0; b < nb; ++b) {
        if (b == hb || enemyStructs[size_t(b)] == 0) continue;
        if (target < 0 || (escalation_ % 2 ? enemyStructs[size_t(b)] < enemyStructs[size_t(target)] : enemyStructs[size_t(b)] > enemyStructs[size_t(target)])) target = b;
    }
    if (target < 0) return used;
    std::vector<Entity> fleet;
    for (Entity e : idleArmy)
        if ((R.types[w.get<Unit>(e).type].layer & kLayerOrbit) && S->bodyAt(w.get<Transform>(e).pos) == hb && m.canTransit(sim, e, target)) fleet.push_back(e);
    const int need = std::min(cfg_.invasionSize + 2 * std::min(invasions_, 6) + (enemyStructs[size_t(hb)] > 0 ? 4 : 0), 30);
    const bool maxed = m.state(sim).players[player_].supplyUsed * 10 >= m.state(sim).players[player_].supplyCap * 9 && m.state(sim).players[player_].supplyCap.raw > 0;
    if (int(fleet.size()) < need && !(maxed && fleet.size() >= 3)) return used;
    Command t; t.type = kCmdTransit; t.units = fleet; t.param = target; send(sim, t);
    for (Entity e : fleet) {
        invaders_.push_back(e);
        idleArmy.erase(std::remove(idleArmy.begin(), idleArmy.end(), e), idleArmy.end());
    }
    invadeBody_ = target;
    invadeTick_ = now;
    ++invasions_;
    return true;
}

void RtsAI::tactics(Simulation& sim, RtsModule& m, std::vector<Entity>& idleArmy, std::vector<Entity>& army, bool defend) {
    World& w = sim.world();
    const Rules& R = m.rules();
    GameState& gs = m.state(sim);
    const MapGrid& g = gs.grid;
    const Surface* S = m.surface(sim);
    const uint32_t now = sim.tick(), rate = sim.tickRate();
    if (cfg_.terrain) ensureTerrain(sim, m);
    refreshBases(sim, m);
    if (!hasHome_) return;
    auto usable = [&](Entity e) {
        if (!w.alive(e) || !w.has<Unit>(e) || !w.has<Owner>(e) || w.get<Owner>(e).player != player_) return false;
        const Unit& u = w.get<Unit>(e);
        return u.hp.raw > 0 && (controllable(w, e, u) || (u.flags & kUnitInTransit));
    };
    auto prune = [&](std::vector<Entity>& v) { v.erase(std::remove_if(v.begin(), v.end(), [&](Entity e) { return !usable(e); }), v.end()); };
    prune(wave_units_); prune(scouts_); prune(invaders_);
    auto inList = [](const std::vector<Entity>& v, Entity e) { return std::find(v.begin(), v.end(), e) != v.end(); };
    auto attack = [&](const std::vector<Entity>& units, Vec3 p) {
        if (units.empty()) return;
        Command c; c.type = kCmdAttack; c.units = units; c.point = p; send(sim, c);
    };
    // enemy structures: kills reset the stalemate clock
    int enemyStructs = 0;
    w.each<Unit, Owner>([&](Entity, Unit& u, Owner& o) {
        if (o.player != kHostileNeutral && m.hostile(sim, player_, o.player) && u.hp.raw > 0 && R.types[u.type].structure) ++enemyStructs;
    });
    if (enemyStructsSeen_ >= 0 && enemyStructs < enemyStructsSeen_) lastKillTick_ = now;
    enemyStructsSeen_ = enemyStructs;
    const uint32_t calm = std::max(lastKillTick_, lastEscalateTick_);
    if (now > 480 * rate && now > calm + cfg_.escalateSeconds * rate && escalation_ < 3) { ++escalation_; lastEscalateTick_ = now; }

    // nothing left to defend: every unit attacks (a hidden survivor would only drag the match out)
    {
        bool anyStructure = false;
        w.each<Unit, Owner>([&](Entity, Unit& u, Owner& o) { if (o.player == player_ && R.types[u.type].structure) anyStructure = true; });
        std::vector<Entity> all;
        w.each<Unit, Owner, Orders>([&](Entity e, Unit& u, Owner& o, Orders&) {
            if (o.player == player_ && !R.types[u.type].structure && !R.types[u.type].weapons.empty() && u.hp.raw > 0 && controllable(w, e, u) && !(u.flags & kUnitInTransit)) all.push_back(e);
        });
        if (!anyStructure && !all.empty()) {
            if (now < allInTick_) return;
            allInTick_ = now + 20 * rate;   // also units stuck in other orders (walking into a lost garrison...)
            const Vec3 from = w.get<Transform>(all[0]).pos;
            Entity best; Fixed bd{};
            w.each<Unit, Owner, Transform>([&](Entity t, Unit& u, Owner& o, Transform& tr) {
                if (!m.hostile(sim, player_, o.player) || o.player == kHostileNeutral || u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
                if (S && S->bodyAt(tr.pos) != S->bodyAt(from)) return;
                const Fixed d = dist(tr.pos, from) + (R.types[u.type].structure ? Fixed{} : Fixed::fromInt(4));
                if (!best.valid() || d < bd) { best = t; bd = d; }
            });
            if (best.valid()) {
                std::vector<Entity> armed, bare;
                for (Entity e : all) (R.types[w.get<Unit>(e).type].weapons.empty() ? bare : armed).push_back(e);
                attack(armed, w.get<Transform>(best).pos);   // units without weapons (garrison walkers) are left alone
            }
            return;
        }
    }
    if (S) planetOps(sim, m, idleArmy);
    std::vector<Entity> home, homeIdle;
    for (Entity e : army) if (!inList(wave_units_, e) && !inList(scouts_, e) && !inList(invaders_, e)) home.push_back(e);
    for (Entity e : idleArmy) if (!inList(wave_units_, e) && !inList(scouts_, e) && !inList(invaders_, e)) homeIdle.push_back(e);
    const int hb = S ? S->bodyAt(home_) : 0;
    auto bodyOk = [&](Entity e, Vec3 p) { return !S || S->bodyAt(w.get<Transform>(e).pos) == S->bodyAt(p); };

    // ---- defence: hostile units close to our structures
    {
        Vec3 sum{};
        int n = 0;
        int64_t threat = 0;
        std::vector<Vec3> mine;
        w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
            if (o.player == player_ && R.types[u.type].structure) mine.push_back(tr.pos);
        });
        w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
            if (o.player == kHostileNeutral || !m.hostile(sim, player_, o.player) || u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
            const UnitType& t = R.types[u.type];
            if (t.structure || t.weapons.empty()) return;
            for (Vec3 p : mine)
                if (dist(p, tr.pos) < Fixed::fromInt(14)) { sum += tr.pos; ++n; threat += strengthOf(t, u); return; }
        });
        if (n > 0) {
            if (n >= 3) raided_ = true;
            const Vec3 at = sum / Fixed::fromInt(n);
            std::vector<Entity> go;
            int64_t ours = 0;
            for (Entity e : home) {
                if (!bodyOk(e, at)) continue;
                ours += strengthOf(R.types[w.get<Unit>(e).type], w.get<Unit>(e));
                auto* ord = w.tryGet<Orders>(e);
                if (ord && (idleOrders(*ord) || ord->queue.front().type == OrderType::Move)) go.push_back(e);
            }
            attack(go, at);
            for (Entity e : go) homeIdle.erase(std::remove(homeIdle.begin(), homeIdle.end(), e), homeIdle.end());
            // overwhelmed at home while a wave is away: call it back
            if (threat > ours * 3 / 2 && !wave_units_.empty() && escalation_ < 3) {
                attack(wave_units_, at);
                wave_units_.clear(); waypoints_.clear();
            }
        }
    }

    // ---- waves
    int needed = defend ? 1 : std::min(cfg_.firstAttackArmy + wave_ * cfg_.armyGrowth, cfg_.maxAttackArmy + escalation_ * 4);
    auto& eco = gs.players[player_];
    const bool maxed = eco.supplyCap.raw > 0 && eco.supplyUsed * 10 >= eco.supplyCap * 9 && eco.supplyCap * 10 >= R.supplyCap * 8;
    if (maxed) needed = std::min(needed, std::max(4, int(home.size()) * 3 / 4));
    // clearly stronger than everything the enemy fields: go now with what we have
    if (!defend && hasEnemy_ && int(homeIdle.size()) >= 3) {
        int64_t ours = 0, theirs = 0;
        for (Entity e : army) ours += strengthOf(R.types[w.get<Unit>(e).type], w.get<Unit>(e));
        w.each<Unit, Owner>([&](Entity, Unit& u, Owner& o) {
            const UnitType& t = R.types[u.type];
            if (o.player == kHostileNeutral || !m.hostile(sim, player_, o.player) || u.hp.raw <= 0 || t.weapons.empty() || t.worker) return;
            theirs += strengthOf(t, u);   // towers count too
        });
        if (ours >= theirs * 2 + 1) needed = std::min(needed, int(homeIdle.size()));
        enemyDefenceless_ = theirs == 0;
    }
    if (!wave_units_.empty()) {
        // ---------- manage the wave in flight
        Vec3 c{};
        int n = 0, idle = 0;
        int64_t ours = 0;
        for (Entity e : wave_units_) {
            if (w.get<Unit>(e).flags & kUnitInTransit) continue;
            c += w.get<Transform>(e).pos; ++n;
            ours += strengthOf(R.types[w.get<Unit>(e).type], w.get<Unit>(e));
            auto* ord = w.tryGet<Orders>(e);
            if (ord && idleOrders(*ord)) ++idle;
        }
        if (n > 0) c = c / Fixed::fromInt(n);
        const bool killed = lastKillTick_ > waveTick_;
        // stuck watchdog: the wave's centre hasn't moved for 30 s (blocked crowd, unreachable point) -> next step
        if (n > 0 && dist(c, waveCentre_) > Fixed::fromInt(2)) { waveCentre_ = c; waveMoveTick_ = now; }
        const bool stuck = n > 0 && now > waveMoveTick_ + 30 * rate;
        if (stuck) waveMoveTick_ = now;
        if (n * 100 < waveStart_ * retreatPct_ && escalation_ < 3 && !killed) {
            // beaten: back to the rally point, bigger next time
            Command mv; mv.type = kCmdMove; mv.units = wave_units_; mv.point = hasRally_ ? rally_ : home_; send(sim, mv);
            wave_units_.clear(); waypoints_.clear();
            ++failedWaves_;
        } else if (n > 0 && wp_ < waypoints_.size()) {
            const Vec3 wp = waypoints_[wp_];
            const Fixed nearR = Fixed::fromInt(5) + fxSqrt(Fixed::fromInt(n));
            int near = 0;
            for (Entity e : wave_units_) if (!(w.get<Unit>(e).flags & kUnitInTransit) && dist(w.get<Transform>(e).pos, wp) < nearR) ++near;
            const bool last = wp_ + 1 >= waypoints_.size();
            bool advance = near * 10 >= n * 6 || (idle * 10 >= n * 8 && now > wpTick_ + 5 * rate) || now > wpTick_ + 45 * rate || stuck;
            if (advance && !last && terr_ && cfg_.terrain) {
                // don't charge uphill into a stronger enemy: wait (and gather reinforcements) for up to 30 s
                const Vec3 nx = waypoints_[wp_ + 1];
                int64_t theirs = 0, theirElev = 0, ourElev = 0;
                int tn = 0;
                w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
                    if (o.player == kHostileNeutral || !m.hostile(sim, player_, o.player) || u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
                    const UnitType& t = R.types[u.type];
                    if (t.weapons.empty() || dist(tr.pos, nx) > Fixed::fromInt(10)) return;
                    theirs += strengthOf(t, u);
                    const int cc = g.cellOf(tr.pos);
                    if (cc >= 0) theirElev += terr_->elev[size_t(cc)];
                    ++tn;
                });
                const int cc = g.cellOf(c);
                if (cc >= 0) ourElev = terr_->elev[size_t(cc)];
                if (tn > 0 && theirElev / tn > ourElev + AITerrain::kUnit / 2 && theirs * 13 / 10 > ours && escalation_ < 3) {
                    if (holdUntil_ == 0) holdUntil_ = now + uint32_t(holdSec_) * rate;
                    if (now < holdUntil_) {
                        advance = false;
                        // reinforce from home
                        std::vector<Entity> join;
                        for (Entity e : homeIdle) if (bodyOk(e, wp)) join.push_back(e);
                        if (join.size() >= 2) {
                            attack(join, wp);
                            for (Entity e : join) wave_units_.push_back(e);
                            homeIdle.clear();
                        }
                    }
                }
            }
            if (advance && !last) {
                ++wp_; wpTick_ = now; holdUntil_ = 0;
                std::vector<Entity> go;
                for (Entity e : wave_units_) if (!(w.get<Unit>(e).flags & kUnitInTransit)) go.push_back(e);
                attack(go, waypoints_[wp_]);
            } else if (last && (stuck || (idle * 10 >= n * 5 && (now > wpTick_ + 8 * rate || !w.alive(waveTarget_) || w.get<Unit>(waveTarget_).hp.raw <= 0)))) {
                // target gone: the next enemy structure close to the wave (else any enemy), straight there
                Entity best; Fixed bd{};
                w.each<Unit, Owner, Transform>([&](Entity t, Unit& u, Owner& o, Transform& tr) {
                    if (!m.hostile(sim, player_, o.player) || u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
                    if (S && S->bodyAt(tr.pos) != S->bodyAt(c)) return;
                    Fixed d = dist(tr.pos, c) + (R.types[u.type].structure ? Fixed{} : Fixed::fromInt(8));
                    if (o.player == kHostileNeutral) d += Fixed::fromInt(20);
                    if (!best.valid() || d < bd) { best = t; bd = d; }
                });
                if (best.valid()) {
                    waveTarget_ = best;
                    waypoints_.push_back(w.get<Transform>(best).pos);
                    ++wp_; wpTick_ = now;
                    if (dist(waypoints_[wp_], c) < Fixed::fromInt(10)) {
                        // close: attack it by name (attack-moves skip what they don't pick up by themselves,
                        // e.g. unfinished construction sites)
                        Command a; a.type = kCmdAttack; a.units = wave_units_; a.target = best; send(sim, a);
                    } else attack(wave_units_, waypoints_[wp_]);
                } else { wave_units_.clear(); waypoints_.clear(); }
            } else {
                // stragglers and units done fighting catch up
                std::vector<Entity> go;
                for (Entity e : wave_units_) {
                    auto* ord = w.tryGet<Orders>(e);
                    if (!(w.get<Unit>(e).flags & kUnitInTransit) && ord && idleOrders(*ord) && dist(w.get<Transform>(e).pos, wp) > nearR) go.push_back(e);
                }
                attack(go, wp);
            }
            // reinforcements while the wave is short of its start size
            if (!wave_units_.empty() && int(homeIdle.size()) >= std::max(4, needed / 3) && int(wave_units_.size()) < waveStart_) {
                std::vector<Entity> join;
                for (Entity e : homeIdle) if (bodyOk(e, waypoints_[wp_])) join.push_back(e);
                attack(join, waypoints_[wp_]);
                for (Entity e : join) { wave_units_.push_back(e); homeIdle.erase(std::remove(homeIdle.begin(), homeIdle.end(), e), homeIdle.end()); }
            }
        } else if (n == 0 && invaders_.empty()) {
            // all in transit or gone
            bool anyTransit = false;
            for (Entity e : wave_units_) if (w.get<Unit>(e).flags & kUnitInTransit) anyTransit = true;
            if (!anyTransit) { wave_units_.clear(); waypoints_.clear(); }
        }
    } else if (int(homeIdle.size()) >= needed && hasEnemy_) {
        // ---------- launch a wave
        Vec3 c{};
        for (Entity e : homeIdle) c += w.get<Transform>(e).pos;
        c = c / Fixed::fromInt(int64_t(homeIdle.size()));
        Entity best;
        Fixed bestD{};
        w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
            if (!m.hostile(sim, player_, o.player) || u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
            if (defend && dist(t.pos, home_) > Fixed::fromInt(25)) return;
            if (S && S->bodyAt(t.pos) != hb) return;   // other bodies: invasions (planetOps)
            const UnitType& ut = R.types[u.type];
            // the nearest enemy, structures a little preferred: their army in the field is a fine target too
            Fixed d = dist(t.pos, c) - (ut.structure ? Fixed::fromInt(5) : Fixed{});
            if (o.player == kHostileNeutral) d += Fixed::fromInt(10);
            if (escalation_ >= 2 && (ut.worker || ut.dropoffMask || !ut.income.empty())) d -= Fixed::fromInt(8);   // the economy
            d += Fixed::fromRatio(int64_t(rng_.below(64)), 32);   // up to 2 cells: no fixed preference
            if (!best.valid() || d < bestD) { best = e; bestD = d; }
        });
        if (best.valid()) {
            const Vec3 goal = w.get<Transform>(best).pos;
            waypoints_.clear();
            std::vector<int> routeCells;
            if (terr_ && cfg_.terrain && !enemyDefenceless_) {   // nothing left to fight: straight there
                const bool toBase = !mainRoute_.empty() && enemyCell_ >= 0 && dist(goal, enemyHome_) < Fixed::fromInt(14);
                const bool alt = escalation_ >= 1 && (wave_ % 2) && !altRoute_.empty();
                if (toBase) routeCells = alt ? altRoute_ : mainRoute_;
                else terr_->route(g, g.cellOf(c), g.cellOf(goal), cfg_.climbWeight * 10, cfg_.exposureWeight * 10, nullptr, routeCells, 1 << 18);
            }
            if (routeCells.size() > 8) {
                // waypoints every 12 cells, a siege spot on high ground short of the target, then the target
                const size_t n = routeCells.size();
                size_t siege = n - 1;
                while (siege > 0 && dist(g.centerOf(routeCells[siege]), goal) < Fixed::fromInt(8)) --siege;
                for (size_t i = 12; i + 6 < siege; i += 12) waypoints_.push_back(g.centerOf(routeCells[i]));
                int sc = routeCells[siege];
                int64_t bs = 0;
                for (int dy = -3; dy <= 3; ++dy)
                    for (int dx = -3; dx <= 3; ++dx) {
                        const int cc = g.offsetCell(routeCells[siege], dx, dy);
                        if (cc < 0 || !terr_->pass[size_t(cc)] || g.blocked[size_t(cc)]) continue;
                        const int64_t s = (int64_t(terr_->elev[size_t(cc)]) - terr_->over[size_t(cc)]) * 8 + int64_t(rng_.below(8));
                        if (s > bs || cc == routeCells[siege]) { if (s >= bs) { bs = s; sc = cc; } }
                    }
                waypoints_.push_back(g.centerOf(sc));
            }
            waypoints_.push_back(goal);
            std::vector<Entity> go;
            for (Entity e : homeIdle) if (bodyOk(e, goal)) go.push_back(e);
            if (!go.empty()) {
                // planets: orbit units elsewhere take a transfer first (as before)
                attack(go, waypoints_[0]);
                wave_units_ = go;
                wp_ = 0; wpTick_ = now; waveTick_ = now; holdUntil_ = 0; waveMoveTick_ = now; waveCentre_ = c;
                waveStart_ = int(go.size());
                waveEnemyStructs_ = enemyStructs;
                waveTarget_ = best;
                for (Entity e : go) homeIdle.erase(std::remove(homeIdle.begin(), homeIdle.end(), e), homeIdle.end());
                if (!defend) ++wave_;
            }
        }
    }

    // ---- scout on the watch post
    if (cfg_.scouts > 0 && watchCell_ >= 0 && int(scouts_.size()) < cfg_.scouts && now > scoutTick_ && int(home.size()) >= 6 && !defend) {
        Entity pick;
        Fixed best{};
        for (Entity e : homeIdle) {
            const UnitType& t = R.types[w.get<Unit>(e).type];
            if (t.hero || !bodyOk(e, g.centerOf(watchCell_))) continue;
            const Fixed s = t.speed * 4 + t.sight - t.maxHp / 100;
            if (!pick.valid() || s > best) { pick = e; best = s; }
        }
        if (pick.valid()) {
            Command c; c.type = kCmdMove; c.units = {pick}; c.point = g.centerOf(watchCell_); send(sim, c);
            scouts_.push_back(pick);
            scoutTick_ = now + 90 * rate;
            homeIdle.erase(std::remove(homeIdle.begin(), homeIdle.end(), pick), homeIdle.end());
        }
    }
    for (Entity e : scouts_) {
        auto* ord = w.tryGet<Orders>(e);
        if (ord && idleOrders(*ord) && dist(w.get<Transform>(e).pos, g.centerOf(watchCell_ >= 0 ? watchCell_ : homeCell_)) > Fixed::fromInt(3)) {
            Command c; c.type = kCmdMove; c.units = {e}; c.point = g.centerOf(watchCell_ >= 0 ? watchCell_ : homeCell_); send(sim, c);
        }
    }
    // ---- producers' rally points: workers come out on the side of their nearest resource, the army on the
    //      side of the rally point (the sim spawns on the side facing the rally: mirrored bases play alike)
    if (now >= rallyTick_ && !S) {
        rallyTick_ = now + 20 * rate;
        w.each<Production, Unit, Owner, Transform>([&](Entity e, Production& pr, Unit& u, Owner& o, Transform& tr) {
            if (o.player != player_ || (u.flags & kUnitUnderConstruction)) return;
            const UnitType& pt = R.types[u.type];
            bool workers = false, fighters = false;
            uint32_t gathers = 0;
            for (uint16_t k : pt.produces) {
                const UnitType& ut = R.types[k];
                if (ut.structure) continue;
                if (ut.worker) { workers = true; for (auto& gd : ut.gathers) gathers |= 1u << gd.res; }
                else if (!ut.weapons.empty()) fighters = true;
            }
            Vec3 to{};
            if (workers) {
                Fixed bd{};
                bool f = false;
                w.each<ResourceStock, Transform>([&](Entity, ResourceStock& rs, Transform& nt) {
                    if (!(gathers & (1u << rs.res)) || (!rs.infinite && rs.amount.raw <= 0)) return;
                    const Fixed d = dist(nt.pos, tr.pos);
                    if (!f || d < bd) { f = true; bd = d; to = nt.pos; }
                });
                if (!f) return;
            } else if (fighters && hasRally_) to = rally_;
            else return;
            // a point just outside the structure toward `to` (workers stay idle there and get a job)
            const Vec2 d = (to - tr.pos).xz();
            const Fixed l = d.length();
            if (l.raw <= 0) return;
            const Fixed out = Fixed::fromInt(std::max(pt.footprintW, pt.footprintH)) / 2 + Fixed::fromRatio(3, 2);
            const Vec3 p = workers ? tr.pos + Vec3{d.x * out / l, Fixed{}, d.y * out / l} : to;
            if (pr.hasRally && dist(pr.rally, p) < Fixed::fromInt(1) && !w.alive(pr.rallyTarget)) return;
            Command c; c.type = kCmdRally; c.units = {e}; c.point = p; send(sim, c);
        });
    }
    // (the army gathers on the rally point through the producers' rally; idle units are not re-sent there, a unit
    //  walking back would not count as ready for the next wave)
}

// ---------------------------------------------------------------- save states
std::string RtsAI::saveState() const {
    std::string out;
    Archive a(&out);
    RtsAI& b = const_cast<RtsAI&>(*this);   // io() takes references; writing does not modify
    b.stateIo(a);
    return out;
}

bool RtsAI::loadState(const std::string& bytes) {
    Archive a(bytes);
    stateIo(a);
    return a.ok();
}

void RtsAI::stateIo(Archive& a) {
    uint32_t version = 1;
    io(a, version);
    if (version != 1) { a.fail(); return; }
    uint64_t rs = rng_.state(), ri = rng_.inc();
    io(a, rs); io(a, ri);
    if (!a.writing()) rng_.restore(rs, ri);
    io(a, player_); io(a, next_); io(a, wave_); io(a, lastBuildTick_); io(a, lastTechTick_); io(a, saveForWorkers_);
    io(a, lastDungeonTick_); io(a, lastSkillTick_); io(a, savingFor_); io(a, savingUntil_); io(a, lastSettlersTick_);
    io(a, lastMechTick_); io(a, turnActed_);
    io(a, rallyTick_); io(a, lastSiteTick_); io(a, allInTick_); io(a, spotRetryTick_); io(a, expandTick_); io(a, rebalanceTick_); io(a, raided_);
    io(a, home_); io(a, enemyHome_); io(a, hasHome_); io(a, hasEnemy_); io(a, homeCell_); io(a, enemyCell_);
    io(a, mainRoute_); io(a, altRoute_); io(a, routeTick_); io(a, routeEnemyCell_); io(a, rally_); io(a, hasRally_);
    io(a, chokeCell_); io(a, watchCell_); io(a, wave_units_); io(a, scouts_); io(a, waypoints_);
    uint64_t wp = wp_; io(a, wp); wp_ = size_t(wp);
    io(a, wpTick_); io(a, waveTick_); io(a, holdUntil_); io(a, waveStart_); io(a, waveEnemyStructs_); io(a, waveTarget_);
    io(a, escalation_); io(a, failedWaves_); io(a, lastKillTick_); io(a, lastEscalateTick_); io(a, scoutTick_); io(a, enemyStructsSeen_);
    uint32_t n = a.count(buildWatch_.size(), 8);
    if (!a.writing()) buildWatch_.assign(n, StuckBuild{});
    for (uint32_t i = 0; i < n && a.ok(); ++i) { io(a, buildWatch_[i].entity); io(a, buildWatch_[i].since); io(a, buildWatch_[i].point); io(a, buildWatch_[i].progress); }
    io(a, waveCentre_); io(a, waveMoveTick_);
    io(a, badSpots_); io(a, invaders_); io(a, invadeBody_); io(a, invasions_); io(a, invadeTick_); io(a, enemyOffBody_);
}

} // namespace rts::game
