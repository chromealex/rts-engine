// agent-mech stage 2, economy: market (prices move with every trade), trade carts between markets, tribute,
// diplomacy (alliances change in game), items (inventory, shops, use, drop, pick up, creep drops) and the
// card deck (only the first `hand` player abilities of a player's deck can be cast; a played card goes last).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

void RtsModule::mechSetup(Simulation& sim, const Json& map) {
    GameState& gs = state(sim);
    MechState& m = gs.mech;
    const MechRules& M = rules_.mech;
    if (M.flowFields) { gs.grid.flowFields = true; gs.grid.flowMinCells = M.flowMinCells; gs.grid.flowCacheSize = M.flowCache; }
    if (M.market) m.prices = M.basePrice;
    if (M.conveyors) mechLogisticsSetup(sim, map);
    const size_t np = sim.players().size();
    if (M.diplomacy) m.allyRequest.assign(np + 1, std::vector<uint8_t>(np + 1, 0));
    if (M.deck) {
        m.decks.assign(np + 1, {});
        for (size_t p = 1; p <= np; ++p) {
            const Json& dj = map["players"][p - 1]["deck"];
            if (dj.isArray()) {
                for (auto& id : dj.items()) {
                    int a = rules_.findPlayerAbility(id.str());
                    if (a < 0) throw std::runtime_error("map player " + std::to_string(p) + " deck: unknown player ability '" + id.str() + "'");
                    m.decks[p].push_back(a);
                }
            } else for (size_t a = 0; a < rules_.playerAbilities.size(); ++a) m.decks[p].push_back(int(a));
        }
    }
    if (!M.cycles.empty()) m.cyclePhase.assign(M.cycles.size(), -1);
    if (!M.coverLevels.empty()) {   // map "cover": {"rows": ["0012..", ...]} digits = cover level per cell
        const MapGrid& g = gs.grid;
        m.cover.assign(size_t(g.w) * size_t(g.h), 0);
        const Json& rows = map["cover"]["rows"];
        for (size_t y = 0; y < rows.size() && int(y) < g.h; ++y) {
            const std::string row = rows[y].str();
            for (size_t x = 0; x < row.size() && int(x) < g.w; ++x)
                if (row[x] >= '1' && row[x] <= '9') m.cover[y * size_t(g.w) + x] = uint8_t(row[x] - '0');
        }
    }
    if (M.rounds) { m.roundPhase = 0; m.phaseEnd = uint32_t((M.prepareTime * int64_t(sim.tickRate())).floorInt()); }
}

Fixed RtsModule::marketPrice(Simulation& sim, int res) const {
    const MechState& m = sim.world().resource<GameState>()->mech;
    return res >= 0 && size_t(res) < m.prices.size() ? m.prices[size_t(res)] : Fixed{};
}

// ================================================================= deck

bool RtsModule::mechInHand(Simulation& sim, uint8_t player, int ability) const {
    const MechState& m = sim.world().resource<GameState>()->mech;
    if (player >= m.decks.size() || m.decks[player].empty()) return true;
    const auto& d = m.decks[player];
    for (size_t i = 0; i < d.size() && int(i) < rules_.mech.hand; ++i) if (d[i] == ability) return true;
    return false;
}

bool RtsModule::mechDeckAllows(Simulation& sim, uint8_t player, int ability, Vec3 point) const {
    if (!mechInHand(sim, player, ability)) return false;
    const MechRules& M = rules_.mech;
    if (M.deployNear.raw <= 0 && !M.deployOwnHalf) return true;
    World& w = sim.world();
    const GameState& gs = *w.resource<GameState>();
    bool near = M.deployNear.raw <= 0;
    Fixed sumX{};
    int n = 0;
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
        if (o.player != player || u.hp.raw <= 0 || !rules_.types[u.type].structure) return;
        sumX += t.pos.x; ++n;
        if (M.deployNear.raw > 0 && dist2D(t.pos, point) <= M.deployNear) near = true;
    });
    if (!near) return false;
    if (M.deployOwnHalf && n > 0) {
        const Fixed mid = Fixed::fromInt(gs.grid.w) / 2;
        const bool left = sumX / int64_t(n) < mid;
        if (left != (point.x < mid)) return false;
    }
    return true;
}

void RtsModule::mechDeckPlayed(Simulation& sim, uint8_t player, int ability) {
    MechState& m = state(sim).mech;
    if (player >= m.decks.size()) return;
    auto& d = m.decks[player];
    auto it = std::find(d.begin(), d.end(), ability);
    if (it == d.end()) return;
    d.erase(it);
    d.push_back(ability);
}

// ================================================================= items

namespace {
int freeSlot(const ItemBag& inv) {
    for (size_t i = 0; i < inv.items.size(); ++i) if (inv.items[i] < 0) return int(i);
    return -1;
}
} // namespace

void RtsModule::mechDropItems(Simulation& sim, Entity victim) {
    World& w = sim.world();
    MechState& m = state(sim).mech;
    const Vec3 at = w.get<Transform>(victim).pos;
    if (auto* inv = w.tryGet<ItemBag>(victim))   // heroes drop what they carry
        for (size_t i = 0; i < inv->items.size(); ++i)
            if (inv->items[i] >= 0) m.groundItems.push_back({at, uint16_t(inv->items[i]), inv->charges[i]});
    for (auto& d : rules_.types[w.get<Unit>(victim).type].mech.drops)
        if (d.chance >= kOne || sim.rng().unit() < d.chance) m.groundItems.push_back({at, d.item, rules_.mech.items[d.item].charges});
}

bool RtsModule::runPickItem(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    auto* inv = w.tryGet<ItemBag>(e);
    if (!inv || freeSlot(*inv) < 0) return true;
    auto* m = w.tryGet<Mobile>(e);
    const Vec3 p = w.get<Transform>(e).pos;
    if (dist2D(p, o.point) > kOne) {
        if (m && (o.state == 0 || !m->moving)) { moveTo(sim, e, o.point, kHalf); o.state = 1; return false; }
        return !m;
    }
    MechState& ms = state(sim).mech;
    size_t best = ms.groundItems.size();
    Fixed bd{};
    for (size_t i = 0; i < ms.groundItems.size(); ++i) {
        Fixed d = dist2D(ms.groundItems[i].pos, p);
        if (d <= Fixed::fromRatio(3, 2) && (best == ms.groundItems.size() || d < bd)) { best = i; bd = d; }
    }
    if (best == ms.groundItems.size()) return true;
    const int slot = freeSlot(*inv);
    inv->items[size_t(slot)] = int16_t(ms.groundItems[best].item);
    inv->charges[size_t(slot)] = ms.groundItems[best].charges;
    sim.emit({kEvItem, w.get<Owner>(e).player, e, kNullEntity, ms.groundItems[best].item, p});
    ms.groundItems.erase(ms.groundItems.begin() + ptrdiff_t(best));
    return true;
}

// ================================================================= trade carts

bool RtsModule::runTrade(Simulation& sim, Entity e, Order& o) {
    (void)o;
    World& w = sim.world();
    auto* tr = w.tryGet<Trader>(e);
    if (!tr || !w.alive(tr->dest) || !w.has<Unit>(tr->dest)) return true;
    const uint8_t owner = w.get<Owner>(e).player;
    if (!w.alive(tr->home) || !w.has<Unit>(tr->home)) {   // home market lost: the nearest other own market
        tr->home = kNullEntity;
        Fixed bd{};
        const Vec3 p = w.get<Transform>(e).pos;
        w.each<Unit, Owner, Transform>([&](Entity x, Unit& u, Owner& ow, Transform& t) {
            if (ow.player != owner || x == tr->dest || !rules_.types[u.type].mech.market || u.hp.raw <= 0) return;
            Fixed d = dist2D(t.pos, p);
            if (!tr->home.valid() || d < bd) { tr->home = x; bd = d; }
        });
        if (!tr->home.valid()) return true;
    }
    const Entity goal = tr->loaded ? tr->home : tr->dest;
    auto* m = w.tryGet<Mobile>(e);
    if (!m) return true;
    if (contactDistance(sim, e, goal) > Fixed::fromRatio(8, 10)) {
        const Vec3 gp = w.get<Transform>(goal).pos;
        if (!m->moving || dist2D(m->goal, gp) > kOne || (sim.tick() + e.index) % 40 == 0) moveTo(sim, e, gp, kHalf);
        return false;
    }
    if (!tr->loaded) { tr->loaded = true; return false; }
    tr->loaded = false;
    const Fixed dist = dist2D(w.get<Transform>(tr->home).pos, w.get<Transform>(tr->dest).pos);
    const Fixed gain = rules_.types[w.get<Unit>(e).type].mech.tradeRate * dist;
    const int16_t cur = rules_.mech.currency >= 0 ? rules_.mech.currency : int16_t(0);
    addResource(sim, owner, uint16_t(cur), gain);
    sim.emit({kEvTraded, owner, e, tr->dest, cur, w.get<Transform>(e).pos});
    return false;
}

// ================================================================= commands

bool RtsModule::applyMechEconomyCommand(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MechState& ms = gs.mech;
    const MechRules& M = rules_.mech;
    auto own = [&](Entity e) { return w.alive(e) && w.has<Unit>(e) && w.get<Owner>(e).player == cmd.player && w.get<Unit>(e).hp.raw > 0; };
    switch (cmd.type) {
    case kCmdTrade: {
        if (!M.market || cmd.units.empty() || !own(cmd.units[0])) return true;
        const Unit& mu = w.get<Unit>(cmd.units[0]);
        if (!rules_.types[mu.type].mech.market || (mu.flags & kUnitUnderConstruction)) return true;
        const int res = int(cmd.param & 0xFF);
        const bool sell = (cmd.param >> 8) & 1;
        if (res >= int(ms.prices.size()) || ms.prices[size_t(res)].raw <= 0 || res == M.currency) return true;
        Fixed& price = ms.prices[size_t(res)];
        auto& stock = gs.players[cmd.player].stock;
        const size_t cur = size_t(M.currency);
        const Fixed base = M.basePrice[size_t(res)];
        if (sell) {
            if (stock[size_t(res)] < M.lot) return true;
            stock[size_t(res)] -= M.lot;
            addResource(sim, cmd.player, uint16_t(cur), price * (kOne - M.marketFee));
            price = fxMax(base / 5, price - M.priceStep);
        } else {
            const Fixed cost = price * (kOne + M.marketFee);
            if (stock[cur] < cost) { sim.emit({kEvNotEnough, cmd.player, kNullEntity, kNullEntity, int64_t(cur), {}}); return true; }
            stock[cur] -= cost;
            addResource(sim, cmd.player, uint16_t(res), M.lot);
            price = fxMin(base * 10, price + M.priceStep);
        }
        sim.emit({kEvTraded, cmd.player, cmd.units[0], kNullEntity, int64_t(res) | (sell ? 256 : 0), w.get<Transform>(cmd.units[0]).pos});
        return true;
    }
    case kCmdTradeRoute: {
        if (!w.alive(cmd.target) || !w.has<Unit>(cmd.target) || !rules_.types[w.get<Unit>(cmd.target).type].mech.market) return true;
        const uint8_t to = w.get<Owner>(cmd.target).player;
        if (to != cmd.player && !(to != 0 && sim.allied(to, cmd.player))) return true;
        std::vector<Entity> go;
        for (Entity e : cmd.units) if (own(e) && rules_.types[w.get<Unit>(e).type].mech.tradeRate.raw > 0 && w.has<Mobile>(e)) go.push_back(e);
        for (Entity e : go) {
            if (!w.has<Trader>(e)) w.add<Trader>(e, Trader{});
            w.get<Trader>(e) = Trader{kNullEntity, cmd.target, false};
            Order o;
            o.type = OrderType::Trade;
            o.target = cmd.target;
            issue(sim, e, o, false);
        }
        return true;
    }
    case kCmdTribute: {
        if (!M.tribute) return true;
        const int res = int(cmd.param & 0xFF);
        const uint8_t to = uint8_t((cmd.param >> 8) & 0xFF);
        const Fixed amount = Fixed::fromInt(int64_t(cmd.param >> 16));
        auto* pt = sim.player(to);
        if (!pt || !pt->alive || to == cmd.player || res >= int(rules_.resources.size()) || amount.raw <= 0) return true;
        auto& stock = gs.players[cmd.player].stock;
        if (stock[size_t(res)] < amount) return true;
        stock[size_t(res)] -= amount;
        addResource(sim, to, uint16_t(res), amount * (kOne - M.tributeFee));
        return true;
    }
    case kCmdDiplomacy: {
        if (!M.diplomacy) return true;
        const uint8_t other = uint8_t(cmd.param & 0xFF);
        const bool ally = (cmd.param >> 8) & 1;
        auto* me = sim.player(cmd.player);
        auto* them = sim.player(other);
        if (!me || !them || other == cmd.player || ms.allyRequest.size() <= std::max(cmd.player, other)) return true;
        if (ally) {
            ms.allyRequest[cmd.player][other] = 1;
            if (ms.allyRequest[other][cmd.player] && me->team != them->team) {
                me->team = them->team;
                ms.allyRequest[cmd.player][other] = ms.allyRequest[other][cmd.player] = 0;
                sim.emit({kEvDiplomacy, cmd.player, kNullEntity, kNullEntity, int64_t(other) | 256, {}});
            }
        } else {
            ms.allyRequest[cmd.player][other] = 0;
            if (me->team == them->team) {
                uint8_t t = 0;
                for (auto& p : sim.players()) t = std::max(t, p.team);
                me->team = uint8_t(t + 1);
                if (gs.tickets.size() <= me->team) gs.tickets.resize(size_t(me->team) + 1, gs.victory.tickets);
                if (gs.score.size() <= me->team) gs.score.resize(size_t(me->team) + 1, Fixed{});
                sim.emit({kEvDiplomacy, cmd.player, kNullEntity, kNullEntity, int64_t(other), {}});
            }
        }
        return true;
    }
    case kCmdBuyItem: {
        if (cmd.units.empty() || !own(cmd.units[0]) || !w.alive(cmd.target) || !w.has<Unit>(cmd.target)) return true;
        Entity buyer = cmd.units[0], shop = cmd.target;
        auto* inv = w.tryGet<ItemBag>(buyer);
        const MechType& st = rules_.types[w.get<Unit>(shop).type].mech;
        const uint8_t so = w.get<Owner>(shop).player;
        if (!inv || freeSlot(*inv) < 0 || cmd.param < 0 || size_t(cmd.param) >= M.items.size()) return true;
        if (so != 0 && so != cmd.player && !sim.allied(so, cmd.player)) return true;
        if (std::find(st.shopItems.begin(), st.shopItems.end(), uint16_t(cmd.param)) == st.shopItems.end()) return true;
        if (contactDistance(sim, buyer, shop) > st.shopRadius) return true;
        const ItemDef& it = M.items[size_t(cmd.param)];
        if (!pay(sim, cmd.player, it.cost, {})) return true;
        const int slot = freeSlot(*inv);
        inv->items[size_t(slot)] = int16_t(cmd.param);
        inv->charges[size_t(slot)] = it.charges;
        sim.emit({kEvItem, cmd.player, buyer, shop, cmd.param, w.get<Transform>(buyer).pos});
        return true;
    }
    case kCmdUseItem: {
        if (cmd.units.empty() || !own(cmd.units[0])) return true;
        Entity u = cmd.units[0];
        auto* inv = w.tryGet<ItemBag>(u);
        if (!inv || cmd.param < 0 || size_t(cmd.param) >= inv->items.size() || inv->items[size_t(cmd.param)] < 0) return true;
        const size_t slot = size_t(cmd.param);
        const ItemDef& it = M.items[size_t(inv->items[slot])];
        if (inv->charges[slot] <= 0) return true;
        Entity target = w.alive(cmd.target) && w.has<Unit>(cmd.target) ? cmd.target : u;
        Unit& tu = w.get<Unit>(target);
        if (it.heal.raw > 0) tu.hp = fxMin(stat(sim, target, Stat::MaxHp), tu.hp + it.heal);
        if (it.energy.raw > 0) tu.energy = fxMin(rules_.types[tu.type].maxEnergy, tu.energy + it.energy);
        if (it.status >= 0 && rules_.advancedEffects) {
            EffectCtx ctx; ctx.caster = u; ctx.owner = cmd.player; ctx.target = target; ctx.point = ctx.origin = w.get<Transform>(target).pos;
            addStatus(sim, target, it.status, it.statusDuration, ctx);
        }
        sim.emit({kEvItem, cmd.player, u, kNullEntity, inv->items[slot], w.get<Transform>(u).pos});
        if (--inv->charges[slot] <= 0 && it.consumable) inv->items[slot] = -1;
        return true;
    }
    case kCmdDropItem: {
        if (cmd.units.empty() || !own(cmd.units[0])) return true;
        auto* inv = w.tryGet<ItemBag>(cmd.units[0]);
        if (!inv || cmd.param < 0 || size_t(cmd.param) >= inv->items.size() || inv->items[size_t(cmd.param)] < 0) return true;
        ms.groundItems.push_back({w.get<Transform>(cmd.units[0]).pos, uint16_t(inv->items[size_t(cmd.param)]), inv->charges[size_t(cmd.param)]});
        inv->items[size_t(cmd.param)] = -1;
        return true;
    }
    case kCmdPickItem: {
        for (Entity e : cmd.units) {
            if (!own(e) || !w.has<ItemBag>(e)) continue;
            Order o;
            o.type = OrderType::PickItem;
            o.point = flat(cmd.point);
            issue(sim, e, o, cmd.flags & kCmdQueue);
        }
        return true;
    }
    default: return false;
    }
}

void RtsModule::mechTickEconomy(Simulation& sim) {
    World& w = sim.world();
    if (rules_.mech.hasItems || rules_.mech.any) {
        std::vector<Entity> add;
        w.each<Unit>([&](Entity e, Unit& u) {
            if (rules_.types[u.type].mech.inventorySlots > 0 && u.hp.raw > 0 && !w.has<ItemBag>(e)) add.push_back(e);
        });
        for (Entity e : add) {
            ItemBag inv;
            const int n = rules_.types[w.get<Unit>(e).type].mech.inventorySlots;
            inv.items.assign(size_t(n), -1);
            inv.charges.assign(size_t(n), 0);
            w.add<ItemBag>(e, inv);
        }
    }
}

} // namespace rts::game
