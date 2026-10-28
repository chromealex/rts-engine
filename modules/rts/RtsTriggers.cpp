#include "RtsTriggers.h"
#include <algorithm>
#include <stdexcept>
#include "RtsInternal.h"

namespace rts::game {
using namespace detail;

// ================================================================= parsing

bool TrigFilter::matches(const Rules& r, uint16_t t) const {
    if (type != kNoType && type != t) return false;
    if (tags && !(r.types[t].tags & tags)) return false;
    if (structures && !r.types[t].structure) return false;
    if (units && r.types[t].structure) return false;
    return true;
}

int TriggerSet::findArea(const std::string& n) const {
    for (size_t i = 0; i < areaNames.size(); ++i) if (areaNames[i] == n) return int(i);
    return -1;
}
int TriggerSet::findTrigger(const std::string& id) const {
    for (size_t i = 0; i < defs.size(); ++i) if (defs[i].id == id) return int(i);
    return -1;
}

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& msg) { throw std::runtime_error("trigger " + where + ": " + msg); }

TrigPlayer parsePlayer(const Json& j, uint8_t defRel = 0) {
    TrigPlayer p;
    p.rel = defRel;
    if (j.isNumber()) { p.rel = 4; p.slot = int8_t(j.integer()); return p; }
    std::string s = j.str("");
    if (s == "all" || s == "any") p.rel = 1;
    else if (s == "enemies") p.rel = 2;
    else if (s == "allies") p.rel = 3;
    else if (s == "creeps" || s == "hostile") p.rel = 5;
    else if (s == "neutral") { p.rel = 4; p.slot = 0; }
    else if (s == "triggering" || s == "owner") p.rel = 0;
    return p;
}

bool parseCmp(const Json& j, TrigCmp& cmp, Fixed& v) {
    static const std::pair<const char*, TrigCmp> ops[] = {{">=", TrigCmp::Ge}, {">", TrigCmp::Gt}, {"<=", TrigCmp::Le}, {"<", TrigCmp::Lt}, {"==", TrigCmp::Eq}, {"!=", TrigCmp::Ne}};
    for (auto& [k, c] : ops) if (j.has(k)) { cmp = c; v = j[k].fx(); return true; }
    if (j.has("value")) { cmp = TrigCmp::Ge; v = j.getFx("value"); return true; }
    return false;
}

bool compare(Fixed a, TrigCmp c, Fixed b) {
    switch (c) {
        case TrigCmp::Ge: return a >= b;
        case TrigCmp::Gt: return a > b;
        case TrigCmp::Le: return a <= b;
        case TrigCmp::Lt: return a < b;
        case TrigCmp::Eq: return a == b;
        case TrigCmp::Ne: return a != b;
    }
    return false;
}

struct Parser {
    const Rules& r;
    TriggerSet& set;
    std::string where;

    uint16_t type(const std::string& id) {
        int k = r.findType(id);
        if (k < 0) fail(where, "unknown type '" + id + "'");
        return uint16_t(k);
    }
    TrigFilter filter(const Json& j) {
        TrigFilter f;
        if (j.has("unit")) f.type = type(j.getStr("unit"));
        for (auto& t : j["tags"].items()) f.tags |= r.tagMask(t.str());
        std::string k = j.getStr("kind", "");
        f.structures = k == "structure";
        f.units = k == "unit";
        return f;
    }
    int area(const Json& j) {
        const Json& a = j["area"];
        if (a.isNull()) return -1;
        if (a.isString()) {
            int i = set.findArea(a.str());
            if (i < 0) fail(where, "unknown area '" + a.str() + "'");
            return i;
        }
        TrigArea ar{a[0].fx(), a[1].fx(), a[2].fx(Fixed::fromInt(1)), a[3].fx(Fixed::fromInt(1))};
        set.areas.push_back(ar);
        set.areaNames.push_back("");
        return int(set.areas.size() - 1);
    }
    int16_t resource(const std::string& id) {
        int k = r.findResource(id);
        if (k < 0) fail(where, "unknown resource '" + id + "'");
        return int16_t(k);
    }
    int16_t tech(const std::string& id) {
        int k = r.findTech(id);
        if (k < 0) fail(where, "unknown tech '" + id + "'");
        return int16_t(k);
    }

    TrigCondition condition(const Json& c) {
        TrigCondition o;
        std::string t = c.getStr("type", "count");
        o.negate = c.getBool("not", false);
        o.player = parsePlayer(c["player"]);
        o.filter = filter(c);
        o.area = area(c);
        parseCmp(c, o.cmp, o.value);
        if (t == "count") o.kind = TrigCondition::Count;
        else if (t == "resource") { o.kind = TrigCondition::Resource; o.index = resource(c.getStr("resource")); }
        else if (t == "variable") { o.kind = TrigCondition::Variable; o.name = c.getStr("name"); }
        else if (t == "researched") { o.kind = TrigCondition::Researched; o.index = tech(c.getStr("tech")); }
        else if (t == "random") { o.kind = TrigCondition::Random; o.value = c.getFx("chance", Fixed::fromRatio(1, 2)); }
        else if (t == "time") o.kind = TrigCondition::Time;
        else if (t == "alive") o.kind = TrigCondition::Alive;
        else if (t == "regions") o.kind = TrigCondition::RegionsOwned;
        else fail(where, "unknown condition '" + t + "'");
        return o;
    }

    TrigAction action(const Json& a) {
        TrigAction o;
        std::string t = a.getStr("type", "");
        static const std::pair<const char*, TrigAction::Kind> kinds[] = {
            {"spawn", TrigAction::Spawn}, {"give", TrigAction::Give}, {"take", TrigAction::Take}, {"setOwner", TrigAction::SetOwner},
            {"kill", TrigAction::Kill}, {"remove", TrigAction::Remove}, {"damage", TrigAction::Damage}, {"heal", TrigAction::Heal},
            {"buff", TrigAction::Buff}, {"message", TrigAction::Message}, {"setVariable", TrigAction::SetVariable},
            {"addVariable", TrigAction::AddVariable}, {"research", TrigAction::Research}, {"enableType", TrigAction::EnableType},
            {"disableType", TrigAction::DisableType}, {"victory", TrigAction::Victory}, {"defeat", TrigAction::Defeat},
            {"enableTrigger", TrigAction::EnableTrigger}, {"disableTrigger", TrigAction::DisableTrigger}, {"addScore", TrigAction::AddScore},
            {"setRegionOwner", TrigAction::SetRegionOwner}, {"attackMove", TrigAction::AttackMove}, {"move", TrigAction::MoveUnits},
            {"setTickets", TrigAction::SetTickets}, {"castAbility", TrigAction::CastAbility},
        };
        bool known = false;
        for (auto& [k, v] : kinds) if (t == k) { o.kind = v; known = true; }
        if (!known) fail(where, "unknown action '" + t + "'");
        o.player = parsePlayer(a["player"]);
        o.filter = filter(a);
        o.area = area(a);
        o.triggeringUnit = a.getStr("target", "") == "triggeringUnit";
        if (a.has("unit")) o.type = type(a.getStr("unit"));
        o.count = int(a.getInt("count", 1));
        o.amount = a.getFx("amount", a.getFx("value"));
        o.duration = a.getFx("duration");
        o.text = a.getStr("text");
        o.name = a.getStr("name", a.getStr("trigger", a.getStr("region")));
        o.attackMove = a.getBool("attackMove", false);
        if (a.has("at")) { o.at = {a["at"][0].fx(), Fixed{}, a["at"][1].fx()}; o.hasAt = true; }
        if (a.has("resource")) o.index = resource(a.getStr("resource"));
        if (a.has("tech")) o.index = tech(a.getStr("tech"));
        if (o.kind == TrigAction::CastAbility) {
            int k = r.findPlayerAbility(a.getStr("ability"));
            if (k < 0) fail(where, "unknown player ability '" + a.getStr("ability") + "'");
            o.index = int16_t(k);
            o.attackMove = a.getBool("free", true);   // reused: free casts skip cost / cooldown / loadout
        }
        if (o.kind == TrigAction::Buff) {
            bool ok = false;
            o.mod.stat = parseStat(a.getStr("stat", "damage"), &ok);
            if (!ok) fail(where, "unknown stat '" + a.getStr("stat") + "'");
            o.mod.add = a.getFx("add");
            o.mod.mul = a.getFx("mul", Fixed::fromInt(1));
        }
        return o;
    }

    TriggerDef def(const Json& j, size_t n) {
        TriggerDef d;
        d.id = j.getStr("id", "trigger" + std::to_string(n));
        where = "'" + d.id + "'";
        d.once = j.getBool("once", true);
        d.enabled = j.getBool("enabled", true);
        d.forEachPlayer = j.getBool("forEachPlayer", false);
        const Json& e = j["event"];
        std::string t = e.isString() ? e.str() : e.getStr("type", "start");
        static const std::pair<const char*, TrigEvent> evs[] = {
            {"start", TrigEvent::Start}, {"time", TrigEvent::Time}, {"every", TrigEvent::Every}, {"enterArea", TrigEvent::EnterArea},
            {"leaveArea", TrigEvent::LeaveArea}, {"unitDied", TrigEvent::UnitDied}, {"built", TrigEvent::Built}, {"trained", TrigEvent::Trained},
            {"researched", TrigEvent::Researched}, {"captured", TrigEvent::Captured}, {"resource", TrigEvent::Resource},
            {"variable", TrigEvent::Variable}, {"wave", TrigEvent::Wave}, {"regionClaimed", TrigEvent::RegionClaimed},
            {"playerDefeated", TrigEvent::PlayerDefeated}, {"ability", TrigEvent::Ability}, {"levelUp", TrigEvent::LevelUp},
        };
        bool known = false;
        for (auto& [k, v] : evs) if (t == k) { d.event = v; known = true; }
        if (!known) fail(where, "unknown event '" + t + "'");
        if (e.isObject()) {
            d.seconds = e.getFx("seconds");
            d.area = area(e);
            d.filter = filter(e);
            d.player = parsePlayer(e["player"], 1);
            if (e.has("resource")) d.index = resource(e.getStr("resource"));
            if (e.has("tech")) d.index = tech(e.getStr("tech"));
            d.name = e.getStr("name");
            parseCmp(e, d.cmp, d.value);
        } else {
            d.player.rel = 1;
        }
        if ((d.event == TrigEvent::Time || d.event == TrigEvent::Every) && d.seconds.raw <= 0 && d.event == TrigEvent::Every) fail(where, "'every' needs seconds > 0");
        if ((d.event == TrigEvent::EnterArea || d.event == TrigEvent::LeaveArea) && d.area < 0) fail(where, "area event needs an area");
        // repeating events default to repeatable triggers
        if (!j.has("once") && (d.event == TrigEvent::Every || d.event == TrigEvent::UnitDied || d.event == TrigEvent::Trained ||
                               d.event == TrigEvent::Built || d.event == TrigEvent::EnterArea || d.event == TrigEvent::LeaveArea)) d.once = false;
        for (auto& c : j["conditions"].items()) d.conditions.push_back(condition(c));
        for (auto& a : j["actions"].items()) d.actions.push_back(action(a));
        return d;
    }
};
} // namespace

void loadTriggers(const Rules& rules, const Json& rulesJson, const Json& mapJson, TriggerSet& out) {
    out = {};
    for (auto& [name, a] : mapJson["areas"].members()) {
        out.areaNames.push_back(name);
        out.areas.push_back({a[0].fx(), a[1].fx(), a[2].fx(Fixed::fromInt(1)), a[3].fx(Fixed::fromInt(1))});
    }
    Parser p{rules, out, ""};
    size_t n = 0;
    for (auto& t : rulesJson["triggers"].items()) out.defs.push_back(p.def(t, n++));
    for (auto& t : mapJson["triggers"].items()) out.defs.push_back(p.def(t, n++));
    // trigger references are checked after all are known
    for (auto& d : out.defs)
        for (auto& a : d.actions)
            if ((a.kind == TrigAction::EnableTrigger || a.kind == TrigAction::DisableTrigger) && out.findTrigger(a.name) < 0)
                throw std::runtime_error("trigger '" + d.id + "': unknown trigger '" + a.name + "'");
}

// ================================================================= runtime

namespace {
bool playerMatches(Simulation& sim, RtsModule& m, const TrigPlayer& p, uint8_t slot, uint8_t triggering) {
    switch (p.rel) {
        case 0: return triggering == 0 || slot == triggering;
        case 1: return slot != 0;
        case 2: return triggering != 0 && m.hostile(sim, triggering, slot);
        case 3: return slot == triggering || (slot != 0 && slot != kHostileNeutral && triggering != 0 && triggering != kHostileNeutral && sim.allied(slot, triggering));
        case 4: return slot == uint8_t(p.slot);
        case 5: return slot == kHostileNeutral;
    }
    return false;
}
std::vector<uint8_t> resolvePlayers(Simulation& sim, RtsModule& m, const TrigPlayer& p, uint8_t triggering) {
    std::vector<uint8_t> out;
    if (p.rel == 4) { out.push_back(uint8_t(p.slot)); return out; }
    if (p.rel == 5) { out.push_back(kHostileNeutral); return out; }
    if (p.rel == 0) { if (triggering) out.push_back(triggering); return out; }
    for (auto& pi : sim.players()) if (playerMatches(sim, m, p, pi.slot, triggering)) out.push_back(pi.slot);
    return out;
}
} // namespace

bool RtsModule::triggerConditions(Simulation& sim, const TriggerDef& d, uint8_t player) {
    World& w = sim.world();
    GameState& gs = state(sim);
    for (auto& c : d.conditions) {
        bool ok = false;
        switch (c.kind) {
        case TrigCondition::Count: {
            int n = 0;
            w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
                if (u.hp.raw <= 0 || !c.filter.matches(rules_, u.type) || !playerMatches(sim, *this, c.player, o.player, player)) return;
                if (c.area >= 0 && !triggers_.areas[size_t(c.area)].contains(t.pos)) return;
                ++n;
            });
            ok = compare(Fixed::fromInt(n), c.cmp, c.value);
            break;
        }
        case TrigCondition::Resource: {
            ok = true;
            for (uint8_t p : resolvePlayers(sim, *this, c.player, player))
                if (p < gs.players.size() && !compare(gs.players[p].stock[size_t(c.index)], c.cmp, c.value)) ok = false;
            break;
        }
        case TrigCondition::Variable: {
            auto it = gs.trig.vars.find(c.name);
            ok = compare(it == gs.trig.vars.end() ? Fixed{} : it->second, c.cmp, c.value);
            break;
        }
        case TrigCondition::Researched: {
            ok = true;
            for (uint8_t p : resolvePlayers(sim, *this, c.player, player))
                if (p >= gs.players.size() || !gs.players[p].researched[size_t(c.index)]) ok = false;
            break;
        }
        case TrigCondition::Random: ok = sim.rng().unit() < c.value; break;
        case TrigCondition::Time: ok = compare(Fixed::fromRatio(int64_t(sim.tick()), int64_t(sim.tickRate())), c.cmp, c.value); break;
        case TrigCondition::Alive: {
            ok = true;
            for (uint8_t p : resolvePlayers(sim, *this, c.player, player)) if (auto* pi = sim.player(p); !pi || !pi->alive) ok = false;
            break;
        }
        case TrigCondition::RegionsOwned: {
            int n = 0;
            for (auto o : gs.regionOwner) if (o == player) ++n;
            ok = compare(Fixed::fromInt(n), c.cmp, c.value);
            break;
        }
        }
        if (ok == c.negate) return false;
    }
    return true;
}

void RtsModule::runTriggerActions(Simulation& sim, size_t di, uint8_t player, Entity unit, Vec3 where) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const TriggerDef& d = triggers_.defs[di];
    for (const TrigAction& a : d.actions) {
        std::vector<uint8_t> players = resolvePlayers(sim, *this, a.player, player);
        // target units (collected first: actions may spawn or destroy)
        auto targets = [&]() {
            std::vector<Entity> out;
            if (a.triggeringUnit) { if (w.alive(unit) && w.has<Unit>(unit)) out.push_back(unit); return out; }
            w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
                if (u.hp.raw <= 0 || !a.filter.matches(rules_, u.type)) return;
                if (std::find(players.begin(), players.end(), o.player) == players.end()) return;
                if (a.area >= 0 && !triggers_.areas[size_t(a.area)].contains(t.pos)) return;
                out.push_back(e);
            });
            return out;
        };
        Vec3 at = where;
        if (a.hasAt) at = a.at;
        else if (a.area >= 0) { const TrigArea& ar = triggers_.areas[size_t(a.area)]; at = {ar.x + ar.w / 2, Fixed{}, ar.z + ar.h / 2}; }
        else if (w.alive(unit) && w.has<Transform>(unit)) at = w.get<Transform>(unit).pos;
        switch (a.kind) {
        case TrigAction::Spawn: {
            if (a.type == kNoType) break;
            uint8_t owner = players.empty() ? 0 : players[0];
            std::vector<Entity> made;
            for (int k = 0; k < a.count; ++k) {
                Vec3 p = at + Vec3{Fixed::fromRatio(k % 4, 1) * Fixed::fromRatio(7, 10), Fixed{}, Fixed::fromRatio(k / 4, 1) * Fixed::fromRatio(7, 10)};
                int cx = gs.grid.cellX(p.x), cy = gs.grid.cellY(p.z);
                if (!rules_.types[a.type].structure && !gs.grid.nearestPassable(cx, cy, rules_.types[a.type].pathLayer, 6)) continue;
                made.push_back(spawn(sim, a.type, owner, rules_.types[a.type].structure ? p : gs.grid.cellCenter(cx, cy)));
            }
            if (a.attackMove) {
                for (Entity n : made) {
                    if (!w.has<Orders>(n)) continue;
                    Entity best; Fixed bestD{};
                    Vec3 np = w.get<Transform>(n).pos;
                    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
                        if (!hostile(sim, owner, o.player) || o.player == kHostileNeutral || !rules_.types[u.type].structure) return;
                        Fixed dd = dist2D(t.pos, np);
                        if (!best.valid() || dd < bestD) { best = e; bestD = dd; }
                    });
                    if (!best.valid()) continue;
                    Order o; o.type = OrderType::AttackMove; o.point = flat(w.get<Transform>(best).pos); o.param = 4;
                    issue(sim, n, o, false);
                }
            }
            break;
        }
        case TrigAction::Give:
            for (uint8_t p : players) addResource(sim, p, uint16_t(a.index), a.amount);
            break;
        case TrigAction::Take:
            for (uint8_t p : players) if (p < gs.players.size()) gs.players[p].stock[size_t(a.index)] = fxMax(Fixed{}, gs.players[p].stock[size_t(a.index)] - a.amount);
            break;
        case TrigAction::SetOwner: {
            uint8_t to = players.empty() ? player : players[0];
            std::vector<Entity> ts;
            if (a.triggeringUnit) ts = targets();
            else w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner&, Transform& t) {
                if (a.filter.matches(rules_, u.type) && (a.area < 0 || triggers_.areas[size_t(a.area)].contains(t.pos))) ts.push_back(e);
            });
            for (Entity e : ts) w.get<Owner>(e).player = to;
            break;
        }
        case TrigAction::Kill:
            for (Entity e : targets()) w.get<Unit>(e).hp = Fixed{};
            break;
        case TrigAction::Remove:
            for (Entity e : targets()) { Unit& u = w.get<Unit>(e); u.hp = Fixed{}; u.flags |= kUnitSilent; }
            break;
        case TrigAction::Damage:
            for (Entity e : targets()) dealDamage(sim, e, a.amount, 0, kNullEntity, 0);
            break;
        case TrigAction::Heal:
            for (Entity e : targets()) { Unit& u = w.get<Unit>(e); u.hp = fxMin(stat(sim, e, Stat::MaxHp), u.hp + a.amount); }
            break;
        case TrigAction::Buff: {
            uint32_t until = a.duration.raw > 0 ? sim.tick() + uint32_t((a.duration * int64_t(sim.tickRate())).toInt()) : UINT32_MAX;
            for (Entity e : targets())
                if (auto* b = w.tryGet<Buffs>(e)) b->list.push_back({a.mod.stat, a.mod.add, a.mod.mul, until, kNoType});
            break;
        }
        case TrigAction::Message: {
            auto it = std::find(gs.trig.messages.begin(), gs.trig.messages.end(), a.text);
            int64_t idx = it - gs.trig.messages.begin();
            if (it == gs.trig.messages.end()) gs.trig.messages.push_back(a.text);
            if (a.player.rel == 1 || players.empty()) sim.emit({kEvMessage, 0, unit, kNullEntity, idx, at});
            else for (uint8_t p : players) sim.emit({kEvMessage, p, unit, kNullEntity, idx, at});
            break;
        }
        case TrigAction::SetVariable: gs.trig.vars[a.name] = a.amount; break;
        case TrigAction::AddVariable: gs.trig.vars[a.name] += a.amount; break;
        case TrigAction::Research:
            for (uint8_t p : players) {
                if (p >= gs.players.size()) continue;
                gs.players[p].researched[size_t(a.index)] = 1;
                recomputeMods(sim, p);
            }
            break;
        case TrigAction::EnableType:
        case TrigAction::DisableType:
            for (uint8_t p : players) if (p < gs.players.size() && a.type != kNoType) gs.players[p].typeDisabled[a.type] = a.kind == TrigAction::DisableType;
            break;
        case TrigAction::Victory:
            if (!players.empty()) gs.winnerTeam = teamOf(sim, players[0]);
            break;
        case TrigAction::Defeat:
            for (uint8_t p : players)
                if (auto* pi = sim.player(p); pi && pi->alive) { pi->alive = false; sim.emit({kEvPlayerDefeated, p, kNullEntity, kNullEntity, 0, {}}); }
            break;
        case TrigAction::EnableTrigger:
        case TrigAction::DisableTrigger: {
            int k = triggers_.findTrigger(a.name);
            if (k >= 0) gs.trig.enabled[size_t(k)] = a.kind == TrigAction::EnableTrigger;
            break;
        }
        case TrigAction::AddScore:
            for (uint8_t p : players) { uint8_t t = teamOf(sim, p); if (t < gs.score.size()) gs.score[t] += a.amount; }
            break;
        case TrigAction::SetRegionOwner:
            for (int i = 0; i < gs.regionCount; ++i)
                if (gs.regionNames[size_t(i)] == a.name) gs.regionOwner[size_t(i)] = players.empty() ? 0 : players[0];
            break;
        case TrigAction::AttackMove:
        case TrigAction::MoveUnits:
            for (Entity e : targets()) {
                if (!w.has<Orders>(e)) continue;
                Order o; o.type = a.kind == TrigAction::AttackMove ? OrderType::AttackMove : OrderType::Move;
                o.point = flat(at); o.param = 8;
                issue(sim, e, o, false);
            }
            break;
        case TrigAction::SetTickets:
            for (uint8_t p : players) { uint8_t t = teamOf(sim, p); if (t < gs.tickets.size()) gs.tickets[t] = a.amount; }
            break;
        case TrigAction::CastAbility:
            for (uint8_t p : players) {
                if (!a.attackMove) { castPlayerAbility(sim, p, a.index, at, unit); continue; }
                EffectCtx ctx; ctx.owner = p; ctx.point = flat(at); ctx.origin = ctx.point; ctx.target = w.alive(unit) ? unit : kNullEntity;
                sim.emit({kEvPlayerAbility, p, kNullEntity, ctx.target, a.index, ctx.point});
                applyPayload(sim, playerAbilityFor(sim, p, a.index).payload, ctx);
            }
            break;
        }
    }
}

void RtsModule::sysTriggers(Simulation& sim) {
    if (triggers_.defs.empty()) return;
    World& w = sim.world();
    GameState& gs = state(sim);
    TriggerState& ts = gs.trig;
    const size_t nd = triggers_.defs.size();
    const size_t slots = 256;
    if (ts.enabled.size() != nd) {
        ts.enabled.resize(nd);
        for (size_t i = 0; i < nd; ++i) ts.enabled[i] = triggers_.defs[i].enabled;
        ts.fired.assign(nd, std::vector<uint8_t>(slots, 0));
        ts.inside.assign(nd, {});
        ts.edge.assign(nd, std::vector<uint8_t>(slots, 0));
    }
    const uint32_t tick = sim.tick();
    const uint32_t rate = sim.tickRate();

    auto fire = [&](size_t di, uint8_t player, Entity unit, Vec3 where) {
        const TriggerDef& d = triggers_.defs[di];
        if (!ts.enabled[di]) return;
        if (d.once && ts.fired[di][player]) return;
        if (!triggerConditions(sim, d, player)) return;
        if (d.once) ts.fired[di][player] = 1;
        sim.emit({kEvTriggerFired, player, unit, kNullEntity, int64_t(di), where});   // presentation/tools only
        runTriggerActions(sim, di, player, unit, where);
    };
    auto fireGlobal = [&](size_t di) {
        if (triggers_.defs[di].forEachPlayer) { for (auto& p : sim.players()) fire(di, p.slot, kNullEntity, {}); }
        else fire(di, 0, kNullEntity, {});
    };

    // time-based
    for (size_t di = 0; di < nd; ++di) {
        const TriggerDef& d = triggers_.defs[di];
        if (d.event == TrigEvent::Start && !ts.started) fireGlobal(di);
        if (d.event == TrigEvent::Time && tick == uint32_t((d.seconds * int64_t(rate)).toInt())) fireGlobal(di);
        if (d.event == TrigEvent::Every) {
            uint32_t period = std::max<uint32_t>(1, uint32_t((d.seconds * int64_t(rate)).toInt()));
            if (tick > 0 && tick % period == 0) fireGlobal(di);
        }
    }
    ts.started = true;

    // simulation events of this tick (copy: actions may emit more)
    const std::vector<SimEvent> evs = sim.events();
    for (const SimEvent& ev : evs) {
        TrigEvent kind;
        switch (ev.type) {
            case kEvDied: kind = TrigEvent::UnitDied; break;
            case kEvBuildDone: kind = TrigEvent::Built; break;
            case kEvTrainDone: kind = TrigEvent::Trained; break;
            case kEvResearched: kind = TrigEvent::Researched; break;
            case kEvCaptured: kind = TrigEvent::Captured; break;
            case kEvWave: kind = TrigEvent::Wave; break;
            case kEvRegionClaimed: kind = TrigEvent::RegionClaimed; break;
            case kEvPlayerDefeated: kind = TrigEvent::PlayerDefeated; break;
            case kEvAbility: kind = TrigEvent::Ability; break;
            case kEvLevelUp: kind = TrigEvent::LevelUp; break;
            default: continue;
        }
        for (size_t di = 0; di < nd; ++di) {
            const TriggerDef& d = triggers_.defs[di];
            if (d.event != kind || !playerMatches(sim, *this, d.player, ev.player, ev.player)) continue;
            const bool unitEvent = kind == TrigEvent::UnitDied || kind == TrigEvent::Built || kind == TrigEvent::Trained;
            if (unitEvent && (ev.value < 0 || size_t(ev.value) >= rules_.types.size() || !d.filter.matches(rules_, uint16_t(ev.value)))) continue;
            if (kind == TrigEvent::Researched && d.index >= 0 && ev.value != d.index) continue;
            if (d.area >= 0 && !triggers_.areas[size_t(d.area)].contains(ev.pos)) continue;
            fire(di, ev.player == kHostileNeutral ? 0 : ev.player, ev.a, ev.pos);
        }
    }

    // area enter / leave and edge-triggered thresholds (every 5 ticks)
    if (tick % 5 != 0) return;
    for (size_t di = 0; di < nd; ++di) {
        const TriggerDef& d = triggers_.defs[di];
        if (d.event == TrigEvent::EnterArea || d.event == TrigEvent::LeaveArea) {
            const TrigArea& ar = triggers_.areas[size_t(d.area)];
            std::vector<uint64_t> now;
            std::vector<std::pair<Entity, uint8_t>> entered;
            w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
                if (u.hp.raw <= 0 || !d.filter.matches(rules_, u.type) || !playerMatches(sim, *this, d.player, o.player, o.player)) return;
                if (!ar.contains(t.pos)) return;
                now.push_back(e.packed());
                entered.push_back({e, o.player});
            });
            std::sort(now.begin(), now.end());
            auto& prev = ts.inside[di];
            if (d.event == TrigEvent::EnterArea) {
                std::sort(entered.begin(), entered.end(), [](auto& x, auto& y) { return x.first.packed() < y.first.packed(); });
                for (auto& [e, owner] : entered)
                    if (!std::binary_search(prev.begin(), prev.end(), e.packed())) fire(di, owner == kHostileNeutral ? 0 : owner, e, w.get<Transform>(e).pos);
            } else {
                for (uint64_t pk : prev)
                    if (!std::binary_search(now.begin(), now.end(), pk)) {
                        Entity e{uint32_t(pk & 0xffffffffu), uint32_t(pk >> 32)};
                        if (!w.alive(e)) continue;
                        uint8_t owner = w.get<Owner>(e).player;
                        fire(di, owner == kHostileNeutral ? 0 : owner, e, w.get<Transform>(e).pos);
                    }
            }
            prev.swap(now);
        } else if (d.event == TrigEvent::Resource) {
            for (auto& p : sim.players()) {
                if (p.slot >= gs.players.size() || !playerMatches(sim, *this, d.player, p.slot, p.slot)) continue;
                bool v = compare(gs.players[p.slot].stock[size_t(d.index)], d.cmp, d.value);
                if (v && !ts.edge[di][p.slot]) fire(di, p.slot, kNullEntity, {});
                ts.edge[di][p.slot] = v;
            }
        } else if (d.event == TrigEvent::Variable) {
            auto it = ts.vars.find(d.name);
            bool v = compare(it == ts.vars.end() ? Fixed{} : it->second, d.cmp, d.value);
            if (v && !ts.edge[di][0]) fireGlobal(di);
            ts.edge[di][0] = v;
        }
    }
}

} // namespace rts::game
