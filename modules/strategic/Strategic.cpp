#include "Strategic.h"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include "engine/core/Random.h"
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsModule.h"

namespace rts::strategic {

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error("strategic " + where + ": " + what); }
Json str(const std::string& s) { return Json::string(s); }
Json fx(Fixed v) { return Json::number(v); }
Json raw(Fixed v) { return Json::number(int64_t(v.raw)); }
Fixed fromRaw(const Json& j) { return Fixed::fromRaw(j.integer(0)); }
void setErr(std::string* err, const std::string& s) { if (err) *err = s; }
Json stacksJson(const std::vector<Stack>& v) {
    Json o = Json::object();
    for (auto& s : v) if (s.count > 0) o.set(s.type, Json::number(int64_t(s.count)));
    return o;
}
std::vector<Stack> stacksOf(const Json& j) {
    std::vector<Stack> v;
    for (auto& [t, n] : j.members()) if (n.integer(0) > 0) v.push_back({t, int(n.integer(0))});
    return v;
}
} // namespace

int StrategicGame::findProvince(const std::string& id) const {
    for (size_t i = 0; i < provinces_.size(); ++i) if (provinces_[i].id == id) return int(i);
    return -1;
}

int StrategicGame::typeIndex(const std::string& id) const {
    int i = 0;
    for (auto& [k, v] : rules_["types"].members()) { if (k == id) return i; ++i; }
    return -1;
}

Fixed StrategicGame::treasury(int player, int res) const {
    if (player <= 0 || size_t(player) >= treasury_.size() || res < 0 || size_t(res) >= treasury_[size_t(player)].size()) return Fixed{};
    return treasury_[size_t(player)][size_t(res)];
}

StrategicGame StrategicGame::create(const Json& def, const Json& rules, uint64_t seed) {
    StrategicGame g;
    g.rules_ = rules;
    g.seed_ = def.has("seed") ? uint64_t(def.getInt("seed")) : seed;
    for (auto& [id, r] : rules["resources"].members()) { (void)r; g.resources_.push_back(id); }
    auto resIndex = [&](const std::string& where, const std::string& id) {
        for (size_t i = 0; i < g.resources_.size(); ++i) if (g.resources_[i] == id) return int(i);
        fail(where, "unknown resource '" + id + "'");
    };
    // players
    const Json& ps = def["players"];
    if (!ps.isArray() || ps.size() < 2) fail("players", "needs at least two players");
    g.playerNames_.push_back("");
    for (auto& p : ps.items()) g.playerNames_.push_back(p.getStr("name", "Player " + std::to_string(g.playerNames_.size())));
    const size_t np = g.playerNames_.size();
    g.treasury_.assign(np, std::vector<Fixed>(g.resources_.size(), Fixed{}));
    for (size_t pi = 1; pi < np; ++pi) {
        const Json& p = ps[pi - 1];
        for (auto& [res, n] : def["start"].members()) g.treasury_[pi][size_t(resIndex("start", res))] = n.fx();
        for (auto& [res, n] : p["resources"].members()) g.treasury_[pi][size_t(resIndex("players.resources", res))] = n.fx();
    }
    if (def.has("treasuryRaw"))
        for (size_t pi = 1; pi < np && pi - 1 < def["treasuryRaw"].size(); ++pi)
            for (size_t r = 0; r < g.resources_.size() && r < def["treasuryRaw"][pi - 1].size(); ++r) g.treasury_[pi][r] = fromRaw(def["treasuryRaw"][pi - 1][r]);
    // provinces
    for (auto& pj : def["provinces"].items()) {
        Province p;
        p.id = pj.getStr("id");
        if (p.id.empty()) fail("provinces", "every province needs an id");
        if (g.findProvince(p.id) >= 0) fail("provinces", "duplicate id '" + p.id + "'");
        p.name = pj.getStr("name", p.id);
        p.owner = int(pj.getInt("owner", 0));
        if (p.owner < 0 || size_t(p.owner) >= np) fail("province '" + p.id + "'", "owner must be 0.." + std::to_string(np - 1));
        for (auto& [res, n] : pj["income"].members()) p.income.push_back({resIndex("province '" + p.id + "' income", res), n.fx()});
        p.terrain = pj["terrain"];
        if (pj["pos"].isArray()) { p.x = pj["pos"][size_t(0)].fx(); p.y = pj["pos"][size_t(1)].fx(); }
        g.provinces_.push_back(p);
    }
    if (g.provinces_.empty()) fail("provinces", "no provinces");
    size_t pi = 0;
    for (auto& pj : def["provinces"].items()) {
        for (auto& a : pj["adj"].items()) {
            const int j = g.findProvince(a.str());
            if (j < 0) fail("province '" + g.provinces_[pi].id + "' adj", "unknown province '" + a.str() + "'");
            auto add = [&](size_t x, int y) { auto& v = g.provinces_[x].adj; if (std::find(v.begin(), v.end(), y) == v.end()) v.push_back(y); };
            add(pi, j);
            add(size_t(j), int(pi));
        }
        ++pi;
    }
    for (auto& p : g.provinces_) std::sort(p.adj.begin(), p.adj.end());
    // recruitable types and their cost (default: the type's cost in rules.json)
    auto costOf = [&](const std::string& t, const Json& c) {
        std::vector<std::pair<int, Fixed>> v;
        for (auto& [res, n] : c.members()) {
            if (res == "time" || res == "supply") continue;
            v.push_back({resIndex("recruit '" + t + "'", res), n.fx()});
        }
        return v;
    };
    if (def["recruit"].isObject()) {
        for (auto& [t, c] : def["recruit"].members()) {
            if (g.typeIndex(t) < 0) fail("recruit", "unknown type '" + t + "'");
            g.recruitCost_.push_back({t, costOf(t, c.isObject() && c.size() > 0 ? c : rules["types"][t]["cost"])});
        }
    }
    g.movesPerTurn_ = int(def.getInt("movesPerTurn", 1));
    // armies
    for (auto& aj : def["armies"].items()) {
        Army a;
        a.id = int(aj.getInt("id", 0));
        if (a.id == 0) a.id = g.nextArmy_;
        g.nextArmy_ = std::max(g.nextArmy_, a.id + 1);
        a.owner = int(aj.getInt("owner", 0));
        if (a.owner <= 0 || size_t(a.owner) >= np) fail("armies", "owner must be 1.." + std::to_string(np - 1));
        a.at = g.findProvince(aj.getStr("at"));
        if (a.at < 0) fail("armies", "unknown province '" + aj.getStr("at") + "'");
        a.units = stacksOf(aj["units"]);
        for (auto& s : a.units) if (g.typeIndex(s.type) < 0) fail("armies", "unknown type '" + s.type + "'");
        a.movesLeft = int(aj.getInt("movesLeft", g.movesPerTurn_));
        g.armies_.push_back(a);
    }
    if (const Json& b = def["battle"]; b.isObject()) {
        g.battleMode = b.getStr("mode", g.battleMode);
        if (g.battleMode != "auto" && g.battleMode != "simulate") fail("battle.mode", "expected auto or simulate");
        g.battleSeconds = int(b.getInt("seconds", g.battleSeconds));
        if (b["size"].isArray()) { g.battleW = int(b["size"][size_t(0)].integer(48)); g.battleH = int(b["size"][size_t(1)].integer(32)); }
    }
    g.turn_ = int(def.getInt("turn", 1));
    g.current_ = int(def.getInt("current", 1));
    g.winner_ = int(def.getInt("winner", 0));
    g.nextArmy_ = std::max(g.nextArmy_, int(def.getInt("nextArmy", 1)));
    for (auto& bj : def["battles"].items()) {
        BattleReport r;
        r.turn = int(bj.getInt("turn")); r.province = g.findProvince(bj.getStr("province"));
        r.attacker = int(bj.getInt("attacker")); r.defender = int(bj.getInt("defender")); r.winner = int(bj.getInt("winner"));
        r.simulated = bj.getBool("simulated");
        r.attackerBefore = stacksOf(bj["attackerBefore"]); r.defenderBefore = stacksOf(bj["defenderBefore"]);
        r.attackerAfter = stacksOf(bj["attackerAfter"]); r.defenderAfter = stacksOf(bj["defenderAfter"]);
        g.battles_.push_back(r);
    }
    return g;
}

StrategicGame StrategicGame::fromJson(const Json& state, const Json& rules) { return create(state, rules); }

Json StrategicGame::toJson() const {
    Json j = Json::object();
    j.set("seed", Json::number(int64_t(seed_)));
    j.set("turn", Json::number(int64_t(turn_)));
    j.set("current", Json::number(int64_t(current_)));
    j.set("winner", Json::number(int64_t(winner_)));
    j.set("nextArmy", Json::number(int64_t(nextArmy_)));
    j.set("movesPerTurn", Json::number(int64_t(movesPerTurn_)));
    Json ps = Json::array(), tr = Json::array();
    for (size_t p = 1; p < playerNames_.size(); ++p) {
        Json pj = Json::object();
        pj.set("name", str(playerNames_[p]));
        Json res = Json::object();
        Json rawRow = Json::array();
        for (size_t r = 0; r < resources_.size(); ++r) { res.set(resources_[r], fx(treasury_[p][r])); rawRow.push(raw(treasury_[p][r])); }
        pj.set("resources", res);
        ps.push(pj);
        tr.push(rawRow);
    }
    j.set("players", ps);
    j.set("treasuryRaw", tr);
    Json pv = Json::array();
    for (auto& p : provinces_) {
        Json pj = Json::object();
        pj.set("id", str(p.id));
        if (p.name != p.id) pj.set("name", str(p.name));
        pj.set("owner", Json::number(int64_t(p.owner)));
        Json adj = Json::array();
        for (int a : p.adj) adj.push(str(provinces_[size_t(a)].id));
        pj.set("adj", adj);
        if (!p.income.empty()) {
            Json inc = Json::object();
            for (auto& [r, n] : p.income) inc.set(resources_[size_t(r)], fx(n));
            pj.set("income", inc);
        }
        if (p.x.raw || p.y.raw) { Json pos = Json::array(); pos.push(fx(p.x)); pos.push(fx(p.y)); pj.set("pos", pos); }
        if (!p.terrain.isNull()) pj.set("terrain", p.terrain);
        pv.push(pj);
    }
    j.set("provinces", pv);
    Json ar = Json::array();
    for (auto& a : armies_) {
        Json aj = Json::object();
        aj.set("id", Json::number(int64_t(a.id)));
        aj.set("owner", Json::number(int64_t(a.owner)));
        aj.set("at", str(provinces_[size_t(a.at)].id));
        aj.set("movesLeft", Json::number(int64_t(a.movesLeft)));
        aj.set("units", stacksJson(a.units));
        aj.set("strength", fx(strength(a)));
        ar.push(aj);
    }
    j.set("armies", ar);
    if (!recruitCost_.empty()) {
        Json rc = Json::object();
        for (auto& [t, c] : recruitCost_) {
            Json cj = Json::object();
            for (auto& [r, n] : c) cj.set(resources_[size_t(r)], fx(n));
            rc.set(t, cj);
        }
        j.set("recruit", rc);
    }
    Json b = Json::object();
    b.set("mode", str(battleMode));
    b.set("seconds", Json::number(int64_t(battleSeconds)));
    Json sz = Json::array(); sz.push(Json::number(int64_t(battleW))); sz.push(Json::number(int64_t(battleH)));
    b.set("size", sz);
    j.set("battle", b);
    Json bl = Json::array();
    for (auto& r : battles_) {
        Json rj = Json::object();
        rj.set("turn", Json::number(int64_t(r.turn)));
        rj.set("province", str(r.province >= 0 ? provinces_[size_t(r.province)].id : ""));
        rj.set("attacker", Json::number(int64_t(r.attacker)));
        rj.set("defender", Json::number(int64_t(r.defender)));
        rj.set("winner", Json::number(int64_t(r.winner)));
        rj.set("simulated", Json::fromBool(r.simulated));
        rj.set("attackerBefore", stacksJson(r.attackerBefore)); rj.set("defenderBefore", stacksJson(r.defenderBefore));
        rj.set("attackerAfter", stacksJson(r.attackerAfter)); rj.set("defenderAfter", stacksJson(r.defenderAfter));
        bl.push(rj);
    }
    j.set("battles", bl);
    return j;
}

uint64_t StrategicGame::checksum() const {
    const std::string s = toJson().dump(0);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// ---------------------------------------------------------------- strength and battles

Fixed StrategicGame::strength(const Army& a) const {
    Fixed s{};
    for (auto& st : a.units) {
        const Json& t = rules_["types"][st.type];
        const Fixed hp = t["health"].getFx("max", Fixed::fromInt(100));
        Fixed dps{};
        for (auto& wpn : t["weapons"].items()) {
            const Fixed cd = wpn.getFx("cooldown", Fixed::fromInt(1));
            dps += wpn.getFx("damage", Fixed::fromInt(10)) / (cd.raw > 0 ? cd : Fixed::fromInt(1));
        }
        s += hp * fxMax(dps, Fixed::fromRatio(1, 10)) * st.count / 100;
    }
    return s;
}

int StrategicGame::armyIndexAt(int province, int ownerNot) const {
    for (size_t i = 0; i < armies_.size(); ++i)
        if (armies_[i].at == province && armies_[i].owner != ownerNot && armies_[i].total() > 0) return int(i);
    return -1;
}

void StrategicGame::autoResolve(Army& a, Army& d, BattleReport& r) {
    // Lanchester square law: the stronger side wins with sqrt(1 - (weak/strong)^2) of its army
    const Fixed sa = strength(a), sd = strength(d);
    Army& win = sa > sd ? a : d;
    Army& lose = sa > sd ? d : a;
    const Fixed sw = fxMax(strength(win), Fixed::fromRatio(1, 100));
    const Fixed ratio = fxMin(Fixed::fromInt(1), strength(lose) / sw);
    const Fixed keep = fxSqrt(fxMax(Fixed{}, Fixed::fromInt(1) - ratio * ratio));
    int left = 0;
    for (auto& s : win.units) { s.count = int((Fixed::fromInt(s.count) * keep).floorInt()); left += s.count; }
    if (left == 0 && !win.units.empty()) win.units[0].count = 1;
    for (auto& s : lose.units) s.count = 0;
    r.winner = win.owner;
}

Json StrategicGame::battleMap(const Army& att, const Army& def, int province) const {
    const int w = battleW, h = battleH;
    Json m = Json::object();
    Json size = Json::array(); size.push(Json::number(int64_t(w))); size.push(Json::number(int64_t(h)));
    m.set("size", size);
    Json players = Json::array();
    Json p1 = Json::object(); p1.set("name", str(playerNames_[size_t(att.owner)])); p1.set("team", Json::number(int64_t(1))); players.push(p1);
    Json p2 = Json::object(); p2.set("name", str(playerNames_[size_t(def.owner)])); p2.set("team", Json::number(int64_t(2))); players.push(p2);
    m.set("players", players);
    const Province& pr = provinces_[size_t(province)];
    if (pr.terrain.isObject()) m.set("terrain", pr.terrain);
    else {
        Json rows = Json::array();
        for (int y = 0; y < h; ++y) rows.push(str(std::string(size_t(w), '.')));
        Json t = Json::object(); t.set("rows", rows);
        m.set("terrain", t);
    }
    Json ents = Json::array();
    auto place = [&](const Army& a, int slot, bool left) {
        int k = 0;
        const int perCol = std::max(1, (h - 4) * 2 / 3);
        for (auto& s : a.units)
            for (int i = 0; i < s.count; ++i, ++k) {
                const int col = k / perCol, row = k % perCol;
                const Fixed x = Fixed::fromInt(left ? 4 + col * 2 : w - 5 - col * 2) + Fixed::fromRatio(1, 2);
                const Fixed y = Fixed::fromInt(2) + Fixed::fromRatio(3, 2) * row;
                Json e = Json::object();
                e.set("type", str(s.type));
                e.set("owner", Json::number(int64_t(slot)));
                Json pos = Json::array(); pos.push(fx(x)); pos.push(fx(y));
                e.set("pos", pos);
                ents.push(e);
            }
    };
    place(att, 1, true);
    place(def, 2, false);
    m.set("entities", ents);
    return m;
}

void StrategicGame::simulate(Army& a, Army& d, BattleReport& r) {
    Json rules = rules_;
    Json v = Json::object();
    v.set("mode", str("score")); v.set("seconds", Json::number(int64_t(100000))); v.set("keyLoses", Json::fromBool(false));
    rules.set("victory", v);
    const Json map = battleMap(a, d, r.province);
    SimConfig cfg;
    cfg.seed = seed_ ^ (uint64_t(turn_) * 1000003ull) ^ (uint64_t(r.province) * 7919ull);
    Simulation sim(cfg, std::make_unique<game::RtsModule>());
    sim.setup(rules, map);
    auto& mod = static_cast<game::RtsModule&>(sim.module());
    World& w = sim.world();
    auto alive = [&](uint8_t side) {
        int n = 0;
        w.each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) { if (o.player == side && u.hp.raw > 0) ++n; });
        return n;
    };
    const int ticks = battleSeconds * int(sim.tickRate());
    for (int i = 0; i < ticks && alive(1) > 0 && alive(2) > 0; ++i) {
        if (i % int(sim.tickRate()) == 0) {   // every second: idle fighters attack-move to the nearest enemy
            for (uint8_t side = 1; side <= 2; ++side) {
                std::vector<Vec3> foes;
                w.each<game::Unit, Owner, Transform>([&](Entity, game::Unit& u, Owner& o, Transform& t) { if (o.player != side && o.player != 0 && u.hp.raw > 0) foes.push_back(t.pos); });
                if (foes.empty()) continue;
                w.each<game::Unit, Owner, Transform>([&](Entity e, game::Unit& u, Owner& o, Transform& t) {
                    if (o.player != side || u.hp.raw <= 0) return;
                    auto* ord = w.tryGet<game::Orders>(e);
                    auto* c = w.tryGet<game::Combat>(e);
                    if ((ord && !ord->queue.empty()) || (c && w.alive(c->target))) return;
                    Vec3 best = foes[0];
                    for (auto& f : foes) if ((f - t.pos).xz().lengthSq() < (best - t.pos).xz().lengthSq()) best = f;
                    Command cmd; cmd.player = side; cmd.type = game::kCmdAttack; cmd.units = {e}; cmd.point = best;
                    sim.submit(cmd);
                });
            }
        }
        sim.step();
    }
    (void)mod;
    auto survivors = [&](uint8_t side, Army& army) {
        for (auto& s : army.units) s.count = 0;
        w.each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) {
            if (o.player != side || u.hp.raw <= 0) return;
            const std::string& id = mod.rules().types[u.type].id;
            for (auto& s : army.units) if (s.type == id) { ++s.count; return; }
            army.units.push_back({id, 1});
        });
    };
    survivors(1, a);
    survivors(2, d);
    const int na = a.total(), nd = d.total();
    r.winner = na > 0 && nd == 0 ? a.owner : nd > 0 && na == 0 ? d.owner : (strength(a) > strength(d) ? a.owner : d.owner);
    // both sides standing at the time limit: the weaker one retreats beaten (loses its army)
    if (na > 0 && nd > 0) { Army& lose = r.winner == a.owner ? d : a; for (auto& s : lose.units) s.count = 0; }
    r.simulated = true;
}

void StrategicGame::fight(int ai, int di) {
    Army a = armies_[size_t(ai)], d = armies_[size_t(di)];
    BattleReport r;
    r.turn = turn_; r.province = d.at; r.attacker = a.owner; r.defender = d.owner;
    r.attackerBefore = a.units; r.defenderBefore = d.units;
    if (battleMode == "simulate") simulate(a, d, r); else autoResolve(a, d, r);
    r.attackerAfter = a.units; r.defenderAfter = d.units;
    armies_[size_t(ai)].units = a.units;
    armies_[size_t(di)].units = d.units;
    battles_.push_back(r);
}

// ---------------------------------------------------------------- orders

bool StrategicGame::move(int player, int army, int to, std::string* err) {
    if (winner_) { setErr(err, "the campaign is over"); return false; }
    if (player != current_) { setErr(err, "not your turn"); return false; }
    int ai = -1;
    for (size_t i = 0; i < armies_.size(); ++i) if (armies_[i].id == army) ai = int(i);
    if (ai < 0 || armies_[size_t(ai)].owner != player) { setErr(err, "no such army of yours"); return false; }
    if (to < 0 || size_t(to) >= provinces_.size()) { setErr(err, "unknown province"); return false; }
    Army& a = armies_[size_t(ai)];
    const auto& adj = provinces_[size_t(a.at)].adj;
    if (std::find(adj.begin(), adj.end(), to) == adj.end()) { setErr(err, "not adjacent"); return false; }
    if (a.movesLeft <= 0) { setErr(err, "no moves left this turn"); return false; }
    a.movesLeft--;
    const int di = armyIndexAt(to, player);
    if (di >= 0) {
        fight(ai, di);
        Army& att = armies_[size_t(ai)];
        if (att.total() > 0 && armies_[size_t(di)].total() == 0) { att.at = to; provinces_[size_t(to)].owner = player; }
    } else {
        a.at = to;
        provinces_[size_t(to)].owner = player;
    }
    // merge own armies standing together, drop empty ones
    armies_.erase(std::remove_if(armies_.begin(), armies_.end(), [](const Army& x) { return x.total() == 0; }), armies_.end());
    for (size_t i = 0; i < armies_.size(); ++i)
        for (size_t j = i + 1; j < armies_.size();) {
            if (armies_[j].owner == armies_[i].owner && armies_[j].at == armies_[i].at) {
                for (auto& s : armies_[j].units) {
                    bool found = false;
                    for (auto& t : armies_[i].units) if (t.type == s.type) { t.count += s.count; found = true; }
                    if (!found) armies_[i].units.push_back(s);
                }
                armies_[i].movesLeft = std::min(armies_[i].movesLeft, armies_[j].movesLeft);
                armies_.erase(armies_.begin() + std::ptrdiff_t(j));
            } else ++j;
        }
    checkWinner();
    return true;
}

bool StrategicGame::recruit(int player, int province, const std::string& type, int count, std::string* err) {
    if (winner_) { setErr(err, "the campaign is over"); return false; }
    if (player != current_) { setErr(err, "not your turn"); return false; }
    if (province < 0 || size_t(province) >= provinces_.size() || provinces_[size_t(province)].owner != player) { setErr(err, "not your province"); return false; }
    if (count <= 0) { setErr(err, "count must be > 0"); return false; }
    const std::vector<std::pair<int, Fixed>>* cost = nullptr;
    for (auto& [t, c] : recruitCost_) if (t == type) cost = &c;
    if (!cost) { setErr(err, "'" + type + "' can't be recruited (campaign \"recruit\")"); return false; }
    for (auto& [r, n] : *cost) if (treasury_[size_t(player)][size_t(r)] < n * count) { setErr(err, "not enough " + resources_[size_t(r)]); return false; }
    for (auto& [r, n] : *cost) treasury_[size_t(player)][size_t(r)] -= n * count;
    for (auto& a : armies_)
        if (a.owner == player && a.at == province) {
            for (auto& s : a.units) if (s.type == type) { s.count += count; return true; }
            a.units.push_back({type, count});
            return true;
        }
    Army a;
    a.id = nextArmy_++; a.owner = player; a.at = province; a.movesLeft = 0;
    a.units.push_back({type, count});
    armies_.push_back(a);
    return true;
}

void StrategicGame::checkWinner() {
    std::vector<int> standing;
    for (size_t p = 1; p < playerNames_.size(); ++p) {
        bool has = false;
        for (auto& pr : provinces_) if (pr.owner == int(p)) has = true;
        for (auto& a : armies_) if (a.owner == int(p) && a.total() > 0) has = true;
        if (has) standing.push_back(int(p));
    }
    if (standing.size() == 1) winner_ = standing[0];
}

bool StrategicGame::endTurn(int player, std::string* err) {
    if (winner_) { setErr(err, "the campaign is over"); return false; }
    if (player != current_) { setErr(err, "not your turn"); return false; }
    const int np = int(playerNames_.size()) - 1;
    for (int k = 1; k <= np; ++k) {
        const int next = (current_ - 1 + k) % np + 1;
        bool has = false;
        for (auto& pr : provinces_) if (pr.owner == next) has = true;
        for (auto& a : armies_) if (a.owner == next) has = true;
        if (!has) continue;
        if (next <= current_) ++turn_;
        current_ = next;
        break;
    }
    for (auto& pr : provinces_)
        if (pr.owner == current_) for (auto& [r, n] : pr.income) treasury_[size_t(current_)][size_t(r)] += n;
    for (auto& a : armies_) if (a.owner == current_) a.movesLeft = movesPerTurn_;
    checkWinner();
    return true;
}

// ---------------------------------------------------------------- strategic AI

void StrategicGame::botTurn(int player) {
    if (winner_ || player != current_) return;
    // recruit: the cheapest unit, as many as affordable (up to 10), in an owned province next to an enemy
    if (!recruitCost_.empty()) {
        int where = -1;
        for (size_t i = 0; i < provinces_.size() && where < 0; ++i) {
            if (provinces_[i].owner != player) continue;
            for (int a : provinces_[i].adj) if (provinces_[size_t(a)].owner != player) { where = int(i); break; }
        }
        for (size_t i = 0; i < provinces_.size() && where < 0; ++i) if (provinces_[i].owner == player) where = int(i);
        if (where >= 0) {
            size_t best = 0; Fixed bestCost{};
            for (size_t k = 0; k < recruitCost_.size(); ++k) {
                Fixed c{};
                for (auto& [r, n] : recruitCost_[k].second) c += n;
                if (k == 0 || c < bestCost) { best = k; bestCost = c; }
            }
            int n = 10;
            for (auto& [r, c] : recruitCost_[best].second)
                if (c.raw > 0) n = std::min(n, int((treasury_[size_t(player)][size_t(r)] / c).floorInt()));
            if (n > 0) recruit(player, where, recruitCost_[best].first, n);
        }
    }
    // armies: attack a weaker neighbour army, else take an undefended neighbour, else step towards the enemy
    std::vector<int> ids;
    for (auto& a : armies_) if (a.owner == player) ids.push_back(a.id);
    for (int id : ids) {
        for (int guard = 0; guard < 4; ++guard) {
            int ai = -1;
            for (size_t i = 0; i < armies_.size(); ++i) if (armies_[i].id == id) ai = int(i);
            if (ai < 0 || armies_[size_t(ai)].movesLeft <= 0 || winner_) break;
            const Army& a = armies_[size_t(ai)];
            const Fixed mine = strength(a);
            int target = -1;
            Fixed weakest{};
            for (int p : provinces_[size_t(a.at)].adj) {
                const int di = armyIndexAt(p, player);
                if (di < 0) continue;
                const Fixed s = strength(armies_[size_t(di)]);
                if (s * 13 / 10 < mine && (target < 0 || s < weakest)) { target = p; weakest = s; }
            }
            if (target < 0)
                for (int p : provinces_[size_t(a.at)].adj)
                    if (provinces_[size_t(p)].owner != player && armyIndexAt(p, player) < 0) { target = p; break; }
            if (target < 0) {
                // breadth-first towards the nearest province we don't own
                std::vector<int> from(provinces_.size(), -2);
                std::vector<int> q{a.at};
                from[size_t(a.at)] = -1;
                int found = -1;
                for (size_t qi = 0; qi < q.size() && found < 0; ++qi)
                    for (int nb : provinces_[size_t(q[qi])].adj) {
                        if (from[size_t(nb)] != -2) continue;
                        from[size_t(nb)] = q[qi];
                        if (provinces_[size_t(nb)].owner != player) { found = nb; break; }
                        q.push_back(nb);
                    }
                if (found >= 0) {
                    int step = found;
                    while (from[size_t(step)] != a.at && from[size_t(step)] >= 0) step = from[size_t(step)];
                    const int di = armyIndexAt(step, player);
                    if (di < 0 || strength(armies_[size_t(di)]) * 13 / 10 < mine) target = step;
                }
            }
            if (target < 0 || !move(player, id, target)) break;
        }
    }
    endTurn(player);
}

} // namespace rts::strategic
