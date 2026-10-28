// Computer player for the agent-mech mechanics: wounded units retreat, infantry mans bunkers and town
// centres when enemies come close and leaves them when the danger is gone.
#include "RtsAI.h"
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

void RtsAI::mech(Simulation& sim, RtsModule& m) {
    if (sim.tick() < lastMechTick_ + sim.tickRate()) return;
    lastMechTick_ = sim.tick();
    World& w = sim.world();
    const Rules& R = m.rules();
    GameState& gs = m.state(sim);
    auto enemyNear = [&](Vec3 p, Fixed r) {
        bool found = false;
        gs.hash.query(p, r, [&](Entity c) {
            if (found || !w.has<Unit>(c) || w.get<Unit>(c).hp.raw <= 0) return;
            const UnitType& ct = R.types[w.get<Unit>(c).type];
            if (ct.weapons.empty() || ct.invulnerable) return;
            if (m.hostile(sim, player_, w.get<Owner>(c).player) && dist2D(w.get<Transform>(c).pos, p) <= r) found = true;
        });
        return found;
    };

    // wounded fighters retreat
    if (R.mech.retreat) {
        Command c; c.player = player_; c.type = kCmdRetreat;
        w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
            if (o.player != player_ || u.hp.raw <= 0 || (u.flags & kUnitOffMap) || w.has<Retreating>(e) || !w.has<Mobile>(e)) return;
            const UnitType& ut = R.types[u.type];
            if (ut.weapons.empty() || u.hp * 10 > m.stat(sim, e, Stat::MaxHp) * 3) return;   // above 30% hp
            if (enemyNear(t.pos, m.stat(sim, e, Stat::Sight))) c.units.push_back(e);
        });
        if (!c.units.empty()) send(sim, c);
    }

    mechEconomy(sim, m);
    // morale: an idle general walks to the shakiest group of own soldiers (faster recovery near him)
    if (R.mech.morale && R.mech.generalTags) {
        Entity worst = kNullEntity; Fixed wv{};
        std::vector<Entity> generals;
        w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
            if (o.player != player_ || u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
            if (R.types[u.type].tags & R.mech.generalTags) {
                if (w.has<Mobile>(e) && (!w.has<Orders>(e) || w.get<Orders>(e).queue.empty())) generals.push_back(e);
                return;
            }
            const auto* mo = w.tryGet<Morale>(e);
            if (mo && mo->init && mo->state != uint8_t(MoraleMode::Steady) && (!worst.valid() || mo->value < wv)) { worst = e; wv = mo->value; }
        });
        if (worst.valid() && !generals.empty()) {
            const Vec3 p = w.get<Transform>(worst).pos;
            Entity g = generals[0];
            for (Entity x : generals) if (dist2D(w.get<Transform>(x).pos, p) < dist2D(w.get<Transform>(g).pos, p)) g = x;
            if (dist2D(w.get<Transform>(g).pos, p) > R.mech.generalRadius / 2) { Command c; c.player = player_; c.type = kCmdMove; c.units = {g}; c.point = p; send(sim, c); }
        }
    }
    // man shooting structures under threat, leave town centres when it is over
    if (!R.mech.transports) return;
    struct Spot { Entity s; int room; bool shoots; bool busy; Vec3 pos; };
    std::vector<Spot> spots;
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (o.player != player_ || u.hp.raw <= 0 || (u.flags & kUnitUnderConstruction)) return;
        const UnitType& ut = R.types[u.type];
        const TransportDef& d = ut.mech.transport;
        if (d.slots == 0 || !ut.structure || (d.fireFrom == 0 && d.damagePerCargo.raw == 0)) return;
        auto* tp = w.tryGet<Transport>(e);
        spots.push_back({e, d.slots - (tp ? tp->used : 0), d.fireFrom > 0, tp && !tp->cargo.empty(), t.pos});
    });
    for (auto& s : spots) {
        const bool danger = enemyNear(s.pos, Fixed::fromInt(10));
        if (!danger) {
            // the town centre lets its workers out again; bunkers stay manned
            if (!s.shoots && s.busy && !enemyNear(s.pos, Fixed::fromInt(14))) {
                Command c; c.player = player_; c.type = kCmdUnload; c.units = {s.s};
                send(sim, c);
            }
            continue;
        }
        if (s.room <= 0) continue;
        std::vector<std::pair<Fixed, Entity>> cand;
        w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
            if (o.player != player_ || u.hp.raw <= 0 || !m.accepts(sim, s.s, e)) return;
            const UnitType& ut = R.types[u.type];
            if (s.shoots == ut.weapons.empty()) return;   // bunkers: fighters; town centres: workers
            auto* ord = w.tryGet<Orders>(e);
            if (s.shoots && ord && !ord->queue.empty()) return;   // fighters busy with orders keep them
            Fixed d = dist2D(t.pos, s.pos);
            if (d <= Fixed::fromInt(12)) cand.push_back({d, e});
        });
        std::sort(cand.begin(), cand.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first < b.first : a.second.index < b.second.index; });
        Command c; c.player = player_; c.type = kCmdLoad; c.target = s.s;
        int room = s.room;
        for (auto& [d, e] : cand) {
            int size = R.types[w.get<Unit>(e).type].mech.cargoSize;
            if (size > room) continue;
            room -= size;
            c.units.push_back(e);
        }
        if (!c.units.empty()) send(sim, c);
    }
}

// Stage 2: market balancing, trade carts, hero items, merging pairs.
void RtsAI::mechEconomy(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    const MechRules& M = R.mech;
    GameState& gs = m.state(sim);
    auto& stock = gs.players[player_].stock;
    auto ownOf = [&](auto pred) {
        std::vector<Entity> v;
        w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) { if (o.player == player_ && u.hp.raw > 0 && !(u.flags & kUnitUnderConstruction) && pred(e, u)) v.push_back(e); });
        return v;
    };
    // market: sell what piles up, buy what is missing
    if (M.market) {
        auto markets = ownOf([&](Entity, Unit& u) { return R.types[u.type].mech.market; });
        if (!markets.empty()) {
            int most = -1, least = -1;
            for (size_t r = 0; r < M.basePrice.size(); ++r) {
                if (M.basePrice[r].raw <= 0 || int(r) == M.currency) continue;
                if (stock[r] > Fixed::fromInt(1200) && (most < 0 || stock[r] > stock[size_t(most)])) most = int(r);
                if (stock[r] < Fixed::fromInt(100) && (least < 0 || stock[r] < stock[size_t(least)])) least = int(r);
            }
            Command c; c.player = player_; c.type = kCmdTrade; c.units = {markets[0]};
            if (most >= 0) { c.param = most | 256; send(sim, c); }
            else if (least >= 0 && stock[size_t(M.currency)] > m.marketPrice(sim, least) * 3) { c.param = least; send(sim, c); }
        }
    }
    // idle trade carts go to the farthest own / allied market
    if (M.traders) {
        auto carts = ownOf([&](Entity e, Unit& u) { return R.types[u.type].mech.tradeRate.raw > 0 && (!w.has<Orders>(e) || w.get<Orders>(e).queue.empty()); });
        if (!carts.empty()) {
            Entity best; Fixed bd{};
            const Vec3 p = w.get<Transform>(carts[0]).pos;
            w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
                if (!R.types[u.type].mech.market || u.hp.raw <= 0 || (o.player != player_ && !(o.player != 0 && sim.allied(o.player, player_)))) return;
                Fixed d = dist2D(t.pos, p);
                if (d > bd) { best = e; bd = d; }
            });
            if (best.valid() && bd > Fixed::fromInt(12)) { Command c; c.player = player_; c.type = kCmdTradeRoute; c.units = carts; c.target = best; send(sim, c); }
        }
    }
    // heroes: drink when hurt, buy potions at a nearby shop
    if (M.hasItems) {
        int potion = -1;
        for (size_t i = 0; i < M.items.size(); ++i) if (M.items[i].heal.raw > 0) { potion = int(i); break; }
        auto heroes = ownOf([&](Entity e, Unit&) { return w.has<ItemBag>(e); });
        for (Entity h : heroes) {
            auto& bag = w.get<ItemBag>(h);
            const Unit& u = w.get<Unit>(h);
            for (size_t s = 0; s < bag.items.size(); ++s)
                if (bag.items[s] >= 0 && M.items[size_t(bag.items[s])].heal.raw > 0 && u.hp * 10 < m.stat(sim, h, Stat::MaxHp) * 4) {
                    Command c; c.player = player_; c.type = kCmdUseItem; c.units = {h}; c.param = int64_t(s); send(sim, c);
                    break;
                }
            if (potion < 0 || std::find(bag.items.begin(), bag.items.end(), int16_t(-1)) == bag.items.end()) continue;
            w.each<Unit, Owner>([&](Entity s, Unit& su, Owner& so) {
                const MechType& st = R.types[su.type].mech;
                if (st.shopItems.empty() || (so.player != 0 && so.player != player_)) return;
                if (std::find(st.shopItems.begin(), st.shopItems.end(), uint16_t(potion)) == st.shopItems.end()) return;
                if (m.contactDistance(sim, h, s) > st.shopRadius || !m.canAfford(sim, player_, M.items[size_t(potion)].cost)) return;
                Command c; c.player = player_; c.type = kCmdBuyItem; c.units = {h}; c.target = s; c.param = potion; send(sim, c);
            });
        }
    }
    // staffed buildings (mills, workshops): idle workers fill the free places, nearest first
    if (M.staffing) {
        auto idle = ownOf([&](Entity e, Unit& u) { return !R.types[u.type].structure && w.has<Mobile>(e) && (!w.has<Orders>(e) || w.get<Orders>(e).queue.empty()); });
        auto sites = ownOf([&](Entity, Unit& u) { return R.types[u.type].mech.staff > 0; });
        for (Entity s : sites) {
            const auto* tp = w.tryGet<Transport>(s);
            int need = R.types[w.get<Unit>(s).type].mech.staff - (tp ? int(tp->cargo.size()) : 0);
            if (need <= 0) continue;
            const Vec3 sp = w.get<Transform>(s).pos;
            Command c; c.player = player_; c.type = kCmdLoad; c.target = s;
            while (need-- > 0) {
                size_t bi = idle.size(); Fixed bd{};
                for (size_t i = 0; i < idle.size(); ++i) {
                    if (!m.accepts(sim, s, idle[i])) continue;
                    Fixed d = dist2D(w.get<Transform>(idle[i]).pos, sp);
                    if (bi == idle.size() || d < bd) { bi = i; bd = d; }
                }
                if (bi == idle.size()) break;
                c.units.push_back(idle[bi]);
                idle.erase(idle.begin() + std::ptrdiff_t(bi));
            }
            if (!c.units.empty()) send(sim, c);
        }
    }
    // pairs of idle mergeable units merge
    if (M.merge) {
        auto pairs = ownOf([&](Entity e, Unit& u) { return R.types[u.type].mech.mergeResult != kNoType && !w.has<Merging>(e) && (!w.has<Orders>(e) || w.get<Orders>(e).queue.empty()); });
        if (pairs.size() >= 2) { Command c; c.player = player_; c.type = kCmdMerge; c.units = pairs; send(sim, c); }
    }
}

} // namespace rts::game
