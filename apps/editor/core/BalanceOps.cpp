// Balance tools for game designers working through MCP: many bot matches over seeds (win rates per
// slot and faction, durations, what got built and lost), unit-vs-unit arena fights with cost
// efficiency, and a static table of every unit's numbers (DPS per armor type, value per cost).
// Everything runs headless on copies of the open rules/map; nothing is saved.
#include "BalanceOps.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"

namespace rts::editor {

namespace {
using namespace rts::game;

Json num(double v, int dec = 2) { return Json::number(v, dec); }
Json inum(int64_t v) { return Json::number(v); }
Json str(const std::string& s) { return Json::string(s); }

const Rules& rulesOf(Workspace& ws) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
    return ws.project.rules;
}

// Resource cost as one number: weights per resource id (default 1 each).
double costValue(const Rules& R, const std::vector<Amount>& cost, const Json& weights) {
    double v = 0;
    for (auto& a : cost) {
        const std::string& id = R.resources[a.res].id;
        v += a.value.toDouble() * (weights.has(id) ? weights[id].toDouble(1) : 1.0);
    }
    return v;
}
Json costJson(const Rules& R, const std::vector<Amount>& cost) {
    Json c = Json::object();
    for (auto& a : cost) c.set(R.resources[a.res].id, num(a.value.toDouble(), 1));
    return c;
}

int typeOrThrow(const Rules& R, const std::string& id) {
    int t = R.findType(id);
    if (t < 0) throw std::runtime_error("unknown type '" + id + "' (rules_list)");
    return t;
}

std::unique_ptr<Simulation> makeSim(const Json& rules, const Json& map, uint64_t seed) {
    SimConfig cfg;
    cfg.seed = seed;
    auto sim = std::make_unique<Simulation>(cfg, std::make_unique<RtsModule>());
    sim->setup(rules, map);
    return sim;
}

// ---------------------------------------------------------------- balance_batch
Json batch(Workspace& ws, const Json& p) {
    const Rules& R = rulesOf(ws);
    const int runs = std::clamp(int(p["runs"].integer(8)), 1, 64);
    const int firstSeed = int(p["seed"].integer(1));
    const double seconds = p["seconds"].toDouble(900);
    const bool mirror = p.getBool("mirror", false);
    Json baseMap = ws.map.toJson();
    if (p.has("factions")) { // {"1": "orcs", "2": "humans"}
        Json* players = baseMap.find("players");
        for (auto& [slot, f] : p["factions"].members()) {
            size_t i = size_t(std::max(1, std::atoi(slot.c_str())) - 1);
            if (!players || i >= players->size()) throw std::runtime_error("no player " + slot + " on the map");
            players->itemsMut()[i].set("faction", f);
        }
    }
    struct PlayerAgg { std::string name; int wins = 0; std::map<std::string, int> factionWins, factionGames; };
    std::map<int, PlayerAgg> slots;
    std::map<std::string, int64_t> built, lost;
    int timeouts = 0, draws = 0;
    std::vector<double> lengths;
    Json games = Json::array();
    for (int k = 0; k < runs; ++k) {
        Json map = baseMap;
        Json* players = map.find("players");
        if (mirror && (k & 1) && players && players->size() == 2) { // swap factions: separates map side from faction
            auto& arr = players->itemsMut();
            Json a = arr[0]["faction"], b = arr[1]["faction"];
            arr[0].set("faction", b); arr[1].set("faction", a);
        }
        auto sim = makeSim(ws.project.rulesJson, map, uint64_t(firstSeed + k));
        AIConfig ai;
        const int diff = int(p["difficulty"].integer(1));
        ai.thinkTicks = diff == 0 ? 20 : diff == 2 ? 6 : 10;
        std::vector<RtsAI> bots;
        for (auto& pl : sim->players()) bots.emplace_back(pl.slot, uint64_t(firstSeed + k), ai);
        const uint32_t limit = uint32_t(seconds * double(sim->tickRate()));
        while (sim->tick() < limit && sim->result() == 0) {
            for (auto& b : bots) b.update(*sim);
            sim->step();
            for (auto& ev : sim->events()) {
                if ((ev.type == kEvTrainDone || ev.type == kEvBuildDone) && size_t(ev.value) < R.types.size()) built[R.types[size_t(ev.value)].id]++;
                if (ev.type == kEvDied && size_t(ev.value) < R.types.size() && ev.player != 0) lost[R.types[size_t(ev.value)].id]++;
            }
        }
        const int res = sim->result();
        const double len = double(sim->tick()) / sim->tickRate();
        lengths.push_back(len);
        if (res == 0) ++timeouts; else if (res < 0) ++draws;
        std::string winnerName = res == 0 ? "" : res < 0 ? "draw" : "";
        for (auto& pl : sim->players()) {
            auto& a = slots[pl.slot];
            a.name = pl.name;
            std::string fac = (players && size_t(pl.slot - 1) < players->size()) ? (*players)[size_t(pl.slot - 1)].getStr("faction", "-") : "-";
            a.factionGames[fac]++;
            if (res > 0 && pl.team == res) { a.wins++; a.factionWins[fac]++; if (winnerName.empty()) winnerName = fac == "-" ? pl.name : pl.name + " (" + fac + ")"; }
        }
        Json g = Json::object();
        g.set("seed", inum(firstSeed + k));
        g.set("winner", str(res == 0 ? "none (time limit)" : winnerName));
        g.set("seconds", inum(int64_t(len)));
        games.push(g);
    }
    Json r = Json::object();
    r.set("runs", inum(runs));
    Json ps = Json::array();
    std::map<std::string, std::pair<int, int>> byFaction; // wins, games
    for (auto& [slot, a] : slots) {
        Json pj = Json::object();
        pj.set("player", inum(slot));
        pj.set("name", str(a.name));
        pj.set("wins", inum(a.wins));
        pj.set("winRate", num(double(a.wins) / runs));
        ps.push(pj);
        for (auto& [f, n] : a.factionGames) { byFaction[f].second += n; byFaction[f].first += a.factionWins[f]; }
    }
    r.set("players", ps);
    if (byFaction.size() > 1 || (byFaction.size() == 1 && byFaction.begin()->first != "-")) {
        Json fj = Json::object();
        for (auto& [f, wg] : byFaction) {
            Json o = Json::object();
            o.set("wins", inum(wg.first)); o.set("games", inum(wg.second));
            o.set("winRate", num(wg.second ? double(wg.first) / wg.second : 0));
            fj.set(f, o);
        }
        r.set("factions", fj);
    }
    std::sort(lengths.begin(), lengths.end());
    double sum = 0; for (double l : lengths) sum += l;
    Json len = Json::object();
    len.set("avg", num(sum / double(lengths.size()), 0)); len.set("min", num(lengths.front(), 0));
    len.set("median", num(lengths[lengths.size() / 2], 0)); len.set("max", num(lengths.back(), 0));
    r.set("seconds", len);
    r.set("timeouts", inum(timeouts));
    r.set("draws", inum(draws));
    Json types = Json::object();
    for (auto& t : R.types) {
        int64_t b = built.count(t.id) ? built[t.id] : 0, l = lost.count(t.id) ? lost[t.id] : 0;
        if (!b && !l) continue;
        Json o = Json::object();
        o.set("built", num(double(b) / runs, 1)); o.set("lost", num(double(l) / runs, 1));
        types.set(t.id, o);
    }
    r.set("perGame", types);
    Json unused = Json::array();
    for (auto& t : R.types) if (!t.cost.empty() && !built.count(t.id)) unused.push(str(t.id));
    r.set("neverBuilt", unused);
    r.set("games", games);
    std::string top;
    for (auto& [slot, a] : slots) top += (top.empty() ? "" : ", ") + a.name + " " + std::to_string(int(std::lround(100.0 * a.wins / runs))) + "%";
    r.set("summary", str("Баланс: " + std::to_string(runs) + " матчей — " + top + ", средняя длина " + std::to_string(int(sum / double(lengths.size()))) + " с"));
    return r;
}

// ---------------------------------------------------------------- balance_duel
struct Side { int type = 0; int count = 1; };

// One arena fight: two groups on an open field, attack-moving into each other. Returns survivors.
struct DuelResult { int winner = 0; double seconds = 0; int left[3] = {0, 0, 0}; double hpLeft[3] = {0, 0, 0}; };

DuelResult duelOnce(Workspace& ws, const Side& a, const Side& b, uint64_t seed, double limitS, int distance) {
    const Rules& R = ws.project.rules;
    const int W = std::max(32, distance + 24), H = 32;
    Json map = Json::object();
    map.set("name", str("arena"));
    Json size = Json::array(); size.push(inum(W)); size.push(inum(H)); map.set("size", size);
    Json rows = Json::array();
    for (int y = 0; y < H; ++y) rows.push(str(std::string(size_t(W), '.')));
    Json terrain = Json::object(); terrain.set("rows", rows); map.set("terrain", terrain);
    Json players = Json::array();
    for (int i = 1; i <= 2; ++i) {
        Json pl = Json::object();
        pl.set("name", str(i == 1 ? "A" : "B")); pl.set("team", inum(i));
        players.push(pl);
    }
    map.set("players", players);
    Json victory = Json::object(); victory.set("mode", str("noUnits")); map.set("victory", victory);
    Json ents = Json::array();
    auto place = [&](const Side& s, int owner, double cx) {
        const UnitType& t = R.types[size_t(s.type)];
        const double step = std::max(0.9, t.radius.toDouble() * 2.2);
        const int perRow = std::max(1, int(std::ceil(std::sqrt(double(s.count)))));
        for (int i = 0; i < s.count; ++i) {
            const int col = i / perRow, row = i % perRow;
            Json e = Json::object();
            e.set("type", str(t.id)); e.set("owner", inum(owner));
            Json pos = Json::array();
            pos.push(num(cx + (owner == 1 ? -1 : 1) * col * step, 2));
            pos.push(num(H / 2.0 + (row - (perRow - 1) / 2.0) * step, 2));
            e.set("pos", pos);
            ents.push(e);
        }
    };
    const double ax = W / 2.0 - distance / 2.0, bx = W / 2.0 + distance / 2.0;
    place(a, 1, ax); place(b, 2, bx);
    map.set("entities", ents);
    auto sim = makeSim(ws.project.rulesJson, map, seed);
    const uint32_t limit = uint32_t(limitS * double(sim->tickRate()));
    auto order = [&]() { // attack-move every group onto the other group's centre
        for (uint8_t pl = 1; pl <= 2; ++pl) {
            Command c; c.type = kCmdAttack; c.player = pl;
            Fixed sx{}, sz{}; int n = 0;
            sim->world().each<Unit, Owner, Transform>([&](Entity e, Unit&, Owner& o, Transform& t) {
                if (o.player == pl) c.units.push_back(e);
                else if (o.player != 0) { sx = sx + t.pos.x; sz = sz + t.pos.z; ++n; }
            });
            if (c.units.empty() || n == 0) continue;
            c.point = {sx / Fixed::fromInt(n), Fixed{}, sz / Fixed::fromInt(n)};
            sim->submit(c);
        }
    };
    while (sim->tick() < limit && sim->result() == 0) {
        if (sim->tick() % (sim->tickRate() * 3) == 0) order();
        sim->step();
    }
    DuelResult d;
    d.seconds = double(sim->tick()) / sim->tickRate();
    sim->world().each<Unit, Owner>([&](Entity, Unit& u, Owner& o) {
        if (o.player < 1 || o.player > 2) return;
        d.left[o.player]++;
        d.hpLeft[o.player] += u.hp.toDouble() / std::max(1.0, R.types[u.type].maxHp.toDouble());
    });
    d.winner = d.left[1] && !d.left[2] ? 1 : d.left[2] && !d.left[1] ? 2 : 0;
    return d;
}

Side readSide(const Rules& R, const Json& j, const char* what) {
    Side s;
    if (j.isString()) { s.type = typeOrThrow(R, j.str()); return s; }
    if (!j.isObject()) throw std::runtime_error(std::string("'") + what + "' is {\"type\": id, \"count\": n} or a type id");
    s.type = typeOrThrow(R, j.getStr("type"));
    s.count = std::clamp(int(j["count"].integer(1)), 1, 100);
    if (R.types[size_t(s.type)].structure) throw std::runtime_error("'" + R.types[size_t(s.type)].id + "' is a structure; duels are for units");
    return s;
}

Json duelReport(Workspace& ws, const Side& a, const Side& b, int runs, double limit, int distance, const Json& weights) {
    const Rules& R = ws.project.rules;
    int wins[3] = {0, 0, 0};
    double secs = 0, left[3] = {0, 0, 0}, hp[3] = {0, 0, 0};
    for (int k = 0; k < runs; ++k) {
        DuelResult d = duelOnce(ws, a, b, uint64_t(k + 1), limit, distance);
        wins[d.winner]++;
        secs += d.seconds;
        for (int s = 1; s <= 2; ++s) { left[s] += d.left[s]; hp[s] += d.hpLeft[s]; }
    }
    const UnitType &ta = R.types[size_t(a.type)], &tb = R.types[size_t(b.type)];
    const double costA = costValue(R, ta.cost, weights) * a.count, costB = costValue(R, tb.cost, weights) * b.count;
    // value destroyed per value lost (cost efficiency), averaged over the runs
    const double lostA = costA * (1.0 - left[1] / runs / a.count), lostB = costB * (1.0 - left[2] / runs / b.count);
    Json r = Json::object();
    auto side = [&](const Side& s, const UnitType& t, int idx, double cost, double lost, double killed) {
        Json o = Json::object();
        o.set("type", str(t.id)); o.set("count", inum(s.count));
        o.set("cost", num(cost, 0));
        o.set("winRate", num(double(wins[idx]) / runs));
        o.set("survivors", num(left[idx] / runs, 1));
        o.set("hpLeft", num(hp[idx] / runs / s.count));
        if (lost > 0) o.set("costEfficiency", num(killed / lost));
        else o.set("costEfficiency", Json());              // lost nothing: no ratio
        o.set("flawless", Json::fromBool(lost <= 0 && killed > 0));
        return o;
    };
    r.set("a", side(a, ta, 1, costA, lostA, lostB));
    r.set("b", side(b, tb, 2, costB, lostB, lostA));
    r.set("draws", inum(wins[0]));
    r.set("seconds", num(secs / runs, 1));
    r.set("runs", inum(runs));
    return r;
}

Json duel(Workspace& ws, const Json& p) {
    const Rules& R = rulesOf(ws);
    Side a = readSide(R, p["a"], "a"), b = readSide(R, p["b"], "b");
    const int runs = std::clamp(int(p["runs"].integer(5)), 1, 32);
    const double limit = p["seconds"].toDouble(120);
    const int distance = std::clamp(int(p["distance"].integer(12)), 2, 120);
    const Json& w = p["weights"];
    if (p.getBool("equalCost", false)) { // size B to the same total cost as A
        double ca = costValue(R, R.types[size_t(a.type)].cost, w) * a.count, cb = costValue(R, R.types[size_t(b.type)].cost, w);
        if (cb > 0) b.count = std::clamp(int(std::lround(ca / cb)), 1, 100);
    }
    Json r = duelReport(ws, a, b, runs, limit, distance, w);
    const Json& ra = r["a"]; const Json& rb = r["b"];
    std::string verdict = ra["winRate"].toDouble() > rb["winRate"].toDouble() ? ra.getStr("type") : rb["winRate"].toDouble() > ra["winRate"].toDouble() ? rb.getStr("type") : "ничья";
    r.set("summary", str("Дуэль " + std::to_string(a.count) + "× " + ra.getStr("type") + " против " + std::to_string(b.count) + "× " + rb.getStr("type") +
                         ": сильнее " + verdict + ", победы " + std::to_string(int(std::lround(ra["winRate"].toDouble() * 100))) + "% / " +
                         std::to_string(int(std::lround(rb["winRate"].toDouble() * 100))) + "%"));
    return r;
}

// Every listed unit against every other at equal cost: who counters whom.
Json matrix(Workspace& ws, const Json& p) {
    const Rules& R = rulesOf(ws);
    std::vector<int> types;
    for (auto& t : p["types"].items()) types.push_back(typeOrThrow(R, t.str()));
    if (types.empty())
        for (size_t i = 0; i < R.types.size(); ++i)
            if (!R.types[i].structure && !R.types[i].weapons.empty() && !R.types[i].cost.empty()) types.push_back(int(i));
    if (types.size() < 2) throw std::runtime_error("need at least two combat unit types");
    if (types.size() > 10) types.resize(10);
    const double budget = p["budget"].toDouble(600);
    const int runs = std::clamp(int(p["runs"].integer(3)), 1, 10);
    const Json& w = p["weights"];
    auto countFor = [&](int t) {
        double c = costValue(R, R.types[size_t(t)].cost, w);
        return std::clamp(c > 0 ? int(std::lround(budget / c)) : 5, 1, 60);
    };
    Json rows = Json::object();
    std::map<int, double> score;
    for (int ta : types) {
        Json row = Json::object();
        for (int tb : types) {
            if (ta == tb) continue;
            Side a{ta, countFor(ta)}, b{tb, countFor(tb)};
            Json d = duelReport(ws, a, b, runs, p["seconds"].toDouble(120), 12, w);
            row.set(R.types[size_t(tb)].id, num(d["a"]["winRate"].toDouble()));
            score[ta] += d["a"]["winRate"].toDouble();
        }
        rows.set(R.types[size_t(ta)].id, row);
    }
    Json rank = Json::array();
    std::vector<std::pair<double, int>> order;
    for (auto& [t, s] : score) order.push_back({-s, t});
    std::sort(order.begin(), order.end());
    for (auto& [s, t] : order) {
        Json o = Json::object();
        o.set("type", str(R.types[size_t(t)].id));
        o.set("avgWinRate", num(-s / double(types.size() - 1)));
        o.set("count", inum(countFor(t)));
        rank.push(o);
    }
    Json r = Json::object();
    r.set("budget", num(budget, 0));
    r.set("winRate", rows);
    r.set("ranking", rank);
    r.set("summary", str("Матрица " + std::to_string(types.size()) + "×" + std::to_string(types.size()) + " на бюджет " + std::to_string(int(budget)) +
                         ": сильнейший " + rank[0].getStr("type") + ", слабейший " + rank[rank.size() - 1].getStr("type")));
    return r;
}

// ---------------------------------------------------------------- balance_stats
Json stats(Workspace& ws, const Json& p) {
    const Rules& R = rulesOf(ws);
    const Json& w = p["weights"];
    const bool withStructures = p.getBool("structures", false);
    Json arr = Json::array();
    for (auto& t : R.types) {
        if (t.structure && !withStructures) continue;
        if (p.has("type") && t.id != p.getStr("type")) continue;
        Json o = Json::object();
        o.set("type", str(t.id)); o.set("name", str(t.name));
        o.set("cost", costJson(R, t.cost));
        const double cost = costValue(R, t.cost, w);
        o.set("costValue", num(cost, 1));
        o.set("hp", num(t.maxHp.toDouble(), 0));
        o.set("armor", num(t.armor.toDouble(), 1));
        if (t.armorType < R.armorTypes.size()) o.set("armorType", str(R.armorTypes[t.armorType]));
        if (!t.structure) o.set("speed", num(t.speed.toDouble()));
        o.set("buildTime", num(t.buildTime.toDouble(), 1));
        if (t.supplyUsed.raw) o.set("supply", num(t.supplyUsed.toDouble(), 1));
        double range = 0;
        Json dps = Json::object();
        const size_t armorKinds = std::max<size_t>(1, R.armorTypes.size());
        double best = 0;
        for (size_t at = 0; at < armorKinds; ++at) {
            double sum = 0;
            for (auto& wp : t.weapons) {
                double hit = wp.damage.toDouble() * R.damageMult(wp.damageType, uint16_t(at)).toDouble();
                sum += hit / std::max(0.05, wp.cooldown.toDouble());
                range = std::max(range, wp.range.toDouble());
            }
            best = std::max(best, sum);
            if (!t.weapons.empty()) dps.set(R.armorTypes.empty() ? "any" : R.armorTypes[at], num(sum, 1));
        }
        if (!t.weapons.empty()) {
            o.set("dps", dps);
            o.set("range", num(range, 1));
            if (cost > 0) o.set("dpsPerCost", num(best / cost * 100, 2));
        }
        // effective hp against a plain 10-damage hit: armor matters more against weak attacks
        const double hit = 10, fin = std::max(hit - t.armor.toDouble(), hit / 5);
        const double ehp = t.maxHp.toDouble() * hit / fin;
        o.set("ehpVs10", num(ehp, 0));
        if (cost > 0) o.set("ehpPerCost", num(ehp / cost, 2));
        arr.push(o);
    }
    Json r = Json::object();
    r.set("types", arr);
    r.set("note", str("dps = damage × damage-table multiplier / cooldown, before target armor (each hit loses armor, at least 20% gets through). "
                      "Per-cost values use 'weights' per resource (default 1)."));
    r.set("summary", str("Характеристики " + std::to_string(arr.size()) + " типов"));
    return r;
}
} // namespace

void registerBalanceOps(Workspace& ws) {
    ws.registerOp({"balance_batch",
        "Many bot-vs-bot matches on the open map over consecutive seeds: win rate per player slot and per faction, match "
        "length (avg/median/min/max), timeouts, what gets built and lost per game, types the bots never build. "
        "'factions' {\"1\": id, ...} overrides the map's factions; 'mirror' swaps the two factions on every other run so "
        "map-side advantage and faction strength separate. Nothing is saved.",
        Json::parse(R"({"properties": {"runs": {"type": "integer", "description": "1..64, default 8"}, "seed": {"type": "integer"},
            "seconds": {"type": "number", "description": "time limit per match, default 900"}, "difficulty": {"type": "integer"},
            "factions": {"type": "object"}, "mirror": {"type": "boolean"}}})"),
        [&ws](const Json& p) { return batch(ws, p); }});
    ws.registerOp({"balance_duel",
        "Arena fight on an empty field: group a vs group b attack-moving into each other, several seeds. Reports win rate, "
        "survivors, hp left and cost efficiency (enemy value destroyed per own value lost). 'equalCost' sizes b to a's cost. "
        "a / b: {\"type\": id, \"count\": n} or a type id.",
        Json::parse(R"({"properties": {"a": {}, "b": {}, "runs": {"type": "integer"}, "seconds": {"type": "number"},
            "distance": {"type": "integer"}, "equalCost": {"type": "boolean"}, "weights": {"type": "object"}}, "required": ["a", "b"]})"),
        [&ws](const Json& p) { return duel(ws, p); }});
    ws.registerOp({"balance_matrix",
        "Who counters whom: every listed combat unit (default: all with weapons and a cost, up to 10) fights every other "
        "with the same resource budget. Returns the win-rate table (row vs column) and a ranking.",
        Json::parse(R"({"properties": {"types": {"type": "array", "items": {"type": "string"}}, "budget": {"type": "number"},
            "runs": {"type": "integer"}, "seconds": {"type": "number"}, "weights": {"type": "object"}}})"),
        [&ws](const Json& p) { return matrix(ws, p); }});
    ws.registerOp({"balance_stats",
        "Numbers of every unit type side by side: cost, hp, armor, speed, range, DPS per armor type (with the damage table), "
        "DPS and effective hp per cost. 'structures': include buildings; 'type': one type.",
        Json::parse(R"({"properties": {"type": {"type": "string"}, "structures": {"type": "boolean"}, "weights": {"type": "object"}}})"),
        [&ws](const Json& p) { return stats(ws, p); }});
}

} // namespace rts::editor
