// scenario_run: a mechanic test an agent can write without code. Start from the open map (or a
// cleared copy of it), place named objects, give commands at given seconds, optionally let bots play
// some slots, and get back the event log, trigger firings, watched objects over time and the result
// of checks ("t1 is dead by 10 s", "player 1 has >= 500 gold"). Runs headless and deterministic on
// copies of the open rules and map; nothing is saved.
#include "ScenarioOps.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include "SimTools.h"
#include "modules/rts/RtsAI.h"

namespace rts::editor {

using namespace rts::game;

namespace {
Json str(const std::string& s) { return Json::string(s); }
Json num(double v, int d = 2) { return Json::number(v, d); }

const char* kOps[] = {">=", ">", "<=", "<", "==", "!="};
// Comparison of a check: {">=": 3}. Returns false (and op "") when the check has none.
bool compare(const Json& c, double actual, std::string& op, double& want) {
    for (const char* o : kOps) {
        if (!c.has(o)) continue;
        op = o;
        want = c[o].toDouble();
        if (op == ">=") return actual >= want;
        if (op == ">") return actual > want;
        if (op == "<=") return actual <= want;
        if (op == "<") return actual < want;
        if (op == "==") return std::fabs(actual - want) < 1e-6;
        return std::fabs(actual - want) >= 1e-6;
    }
    op.clear();
    return true;
}

struct Run {
    explicit Run(Workspace& w) : ws(w) {}
    Workspace& ws;
    std::unique_ptr<Simulation> sim;
    RtsModule* mod = nullptr;
    std::unique_ptr<SimRefs> refs;
    std::map<std::string, int64_t> eventCounts;
    std::map<std::string, int64_t> triggerCounts;
    Json eventLog = Json::array(), triggerLog = Json::array(), messages = Json::array();
    int64_t eventLogDropped = 0;

    double seconds() const { return double(sim->tick()) / sim->tickRate(); }
};

// Value a check looks at; throws on a malformed check.
double measure(Run& r, const Json& c, std::string& what) {
    World& w = r.sim->world();
    GameState& gs = r.mod->state(*r.sim);
    const Rules& R = r.mod->rules();
    if (c.has("alive") || c.has("dead")) {
        const Json& ref = c.has("alive") ? c["alive"] : c["dead"];
        std::vector<Entity> v;
        r.refs->resolve(ref, v);
        int64_t n = 0;
        for (Entity e : v) if (w.alive(e) && w.has<Unit>(e) && w.get<Unit>(e).hp.raw > 0) ++n;
        what = (c.has("alive") ? "alive " : "dead ") + ref.dump(-1);
        return double(n);
    }
    if (c.has("count")) { std::vector<Entity> v; r.refs->resolve(c["count"], v); what = "count " + c["count"].dump(-1); return double(v.size()); }
    if (c.has("hp")) {
        Entity e = r.refs->one(c["hp"]);
        what = "hp " + c["hp"].dump(-1);
        return w.alive(e) && w.has<Unit>(e) ? w.get<Unit>(e).hp.toDouble() : 0.0;
    }
    if (c.has("resource")) {
        int res = -1;
        for (size_t i = 0; i < R.resources.size(); ++i) if (R.resources[i].id == c.getStr("resource")) res = int(i);
        if (res < 0) throw std::runtime_error("check: unknown resource '" + c.getStr("resource") + "'");
        size_t pl = size_t(c["player"].integer(1));
        if (pl >= gs.players.size()) throw std::runtime_error("check: no player " + std::to_string(pl));
        what = "resource " + c.getStr("resource") + " of player " + std::to_string(pl);
        return gs.players[pl].stock[size_t(res)].toDouble();
    }
    if (c.has("event")) {
        if (simEventByName(c.getStr("event")) < 0) throw std::runtime_error("check: unknown event '" + c.getStr("event") + "' (" + simEventNames() + ")");
        what = "events " + c.getStr("event");
        return double(r.eventCounts[c.getStr("event")]);
    }
    if (c.has("trigger")) { what = "trigger " + c.getStr("trigger") + " fired"; return double(r.triggerCounts[c.getStr("trigger")]); }
    if (c.has("variable")) {
        auto it = gs.trig.vars.find(c.getStr("variable"));
        what = "variable " + c.getStr("variable");
        return it == gs.trig.vars.end() ? 0.0 : it->second.toDouble();
    }
    if (c.has("distance")) {   // {"distance": "t1", "to": [x, z] | ref}
        Entity e = r.refs->one(c["distance"]);
        what = "distance " + c["distance"].dump(-1);
        if (!w.alive(e)) return 1e9;
        Vec3 p = w.get<Transform>(e).pos, q{};
        if (c["to"].isArray() && c["to"].size() == 2 && c["to"][0].isNumber()) q = {toFixed(c["to"][0].toDouble()), Fixed{}, toFixed(c["to"][1].toDouble())};
        else { Entity t = r.refs->one(c["to"]); if (!w.alive(t)) return 1e9; q = w.get<Transform>(t).pos; }
        double dx = p.x.toDouble() - q.x.toDouble(), dz = p.z.toDouble() - q.z.toDouble();
        return std::sqrt(dx * dx + dz * dz);
    }
    if (c.has("status")) {   // {"status": "frost", "on": ref}
        if (std::none_of(R.statuses.begin(), R.statuses.end(), [&](const StatusDef& d) { return d.id == c.getStr("status"); }))
            throw std::runtime_error("check: unknown status '" + c.getStr("status") + "' (rules statuses)");
        Entity e = r.refs->one(c["on"]);
        what = "status " + c.getStr("status") + " on " + c["on"].dump(-1);
        int64_t n = 0;
        if (auto* st = w.alive(e) ? w.tryGet<Statuses>(e) : nullptr)
            for (auto& si : st->list) if (si.def < R.statuses.size() && R.statuses[si.def].id == c.getStr("status")) n += si.stacks;
        return double(n);
    }
    if (c.has("result")) { what = "result (winning team, -1 draw, 0 none)"; return double(r.sim->result()); }
    throw std::runtime_error("check needs one of: alive, dead, count, hp, resource, event, trigger, variable, distance, status, result");
}

Json evalCheck(Run& r, const Json& c) {
    Json out = Json::object();
    std::string what, op;
    double want = 0;
    double actual = measure(r, c, what);
    bool pass;
    if (c.has("alive") && !std::any_of(std::begin(kOps), std::end(kOps), [&](const char* o) { return c.has(o); })) {
        std::vector<Entity> v; r.refs->resolve(c["alive"], v);
        pass = actual > 0 && actual == double(v.size()); op = "all"; want = double(v.size());
    } else if (c.has("dead") && !std::any_of(std::begin(kOps), std::end(kOps), [&](const char* o) { return c.has(o); })) {
        pass = actual == 0; op = "alive =="; want = 0;
    } else {
        pass = compare(c, actual, op, want);
        if (op.empty()) { pass = actual != 0; op = "!="; want = 0; }   // bare check: non-zero
    }
    out.set("check", str(c.has("name") ? c.getStr("name") : what + " " + op + " " + Json::number(want, 2).dump(-1)));
    out.set("t", num(r.seconds(), 2));
    out.set("pass", Json::fromBool(pass));
    out.set("actual", num(actual, 2));
    return out;
}

void collectEvents(Run& r, const std::vector<int>& logTypes, bool logAll, int64_t maxLog) {
    GameState& gs = r.mod->state(*r.sim);
    const TriggerSet& ts = r.mod->triggers();
    for (auto& ev : r.sim->events()) {
        const char* name = simEventName(ev.type);
        r.eventCounts[name]++;
        if (ev.type == kEvTriggerFired) {
            std::string id = ev.value >= 0 && size_t(ev.value) < ts.defs.size() ? ts.defs[size_t(ev.value)].id : "";
            if (id.empty()) id = "#" + std::to_string(ev.value);
            r.triggerCounts[id]++;
            if (r.triggerLog.size() < 500) {
                Json t = Json::object();
                t.set("t", num(r.seconds())); t.set("trigger", str(id)); t.set("player", Json::number(int64_t(ev.player)));
                if (ev.a.valid()) t.set("unit", r.refs->refJson(ev.a));
                r.triggerLog.push(t);
            }
        }
        if (ev.type == kEvMessage && ev.value >= 0 && size_t(ev.value) < gs.trig.messages.size() && r.messages.size() < 200) {
            Json m = Json::object();
            m.set("t", num(r.seconds())); m.set("player", Json::number(int64_t(ev.player))); m.set("text", str(gs.trig.messages[size_t(ev.value)]));
            r.messages.push(m);
        }
        const bool want = logAll || std::find(logTypes.begin(), logTypes.end(), int(ev.type)) != logTypes.end();
        if (!want) continue;
        if (int64_t(r.eventLog.size()) >= maxLog) { r.eventLogDropped++; continue; }
        r.eventLog.push(eventJson(*r.refs, ev));
        r.eventLog.at(r.eventLog.size() - 1).set("t", num(r.seconds()));
    }
}

Json scenarioRun(Workspace& ws, const Json& p) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
    Json map = mapForSim(ws.map.toJson(), ws.project.dir);
    if (p.getBool("clear")) {
        map.set("entities", Json::array());
        map.erase("roads");
        map.erase("waves");
        for (auto& pl : map.member("players").itemsMut()) { pl.erase("start"); pl.erase("waves"); }
        Json v = Json::object();
        v.set("mode", str("score")); v.set("seconds", Json::number(int64_t(1000000))); v.set("keyLoses", Json::fromBool(false));
        map.set("victory", v);
    }
    if (p.has("victory")) map.set("victory", p["victory"]);
    if (p.has("fogOfWar")) { Json f = Json::object(); f.set("mode", p["fogOfWar"]); map.set("fogOfWar", f); }
    for (auto& [slot, res] : p["resources"].members()) {
        size_t i = size_t(std::atoi(slot.c_str()));
        if (i < 1 || i > map["players"].size()) throw std::runtime_error("resources: no player " + slot);
        Json& pl = map.member("players").at(i - 1);
        for (auto& [rid, amt] : res.members()) pl.member("resources").set(rid, amt);
    }
    // named spawns go into the map, so the real loader places them (structures snap to the grid)
    std::vector<std::pair<std::string, size_t>> spawnNames;   // name -> index in map entities
    for (auto& s : p["spawn"].items()) {
        if (ws.project.rules.findType(s.getStr("type")) < 0) throw std::runtime_error("spawn: unknown type '" + s.getStr("type") + "'");
        const int count = int(std::clamp<int64_t>(s.getInt("count", 1), 1, 500));
        const double ax = s["at"][0].toDouble(), az = s["at"][1].toDouble();
        const int side = int(std::ceil(std::sqrt(double(count))));
        const double gap = s["spacing"].toDouble(1.0);
        for (int k = 0; k < count; ++k) {
            Json e = Json::object();
            e.set("type", s["type"]);
            e.set("owner", Json::number(s.getInt("owner", 1)));
            Json pos = Json::array();
            pos.push(num(ax + (k % side - (side - 1) / 2.0) * gap, 3)); pos.push(num(az + (k / side - (side - 1) / 2.0) * gap, 3));
            e.set("pos", pos);
            for (const char* key : {"garrison", "amount", "inventory"}) if (s.has(key)) e.set(key, s[key]);
            std::string name = s.has("id") ? (count == 1 ? s.getStr("id") : s.getStr("id") + "#" + std::to_string(k + 1)) : "";
            if (!name.empty()) { e.set("id", str(name)); spawnNames.push_back({name, map["entities"].size()}); }
            map.member("entities").push(e);
        }
        if (s.has("id") && count > 1) spawnNames.push_back({s.getStr("id"), size_t(-1)});   // group name, filled below
    }
    if (!map["entities"].isArray()) map.set("entities", Json::array());

    Run r(ws);
    SimConfig cfg;
    cfg.seed = uint64_t(std::max<int64_t>(0, p["seed"].integer(1)));
    r.sim = std::make_unique<Simulation>(cfg, std::make_unique<RtsModule>());
    r.sim->setup(ws.project.rulesJson, map);
    r.mod = dynamic_cast<RtsModule*>(&r.sim->module());
    r.refs = std::make_unique<SimRefs>(SimRefs{*r.sim, *r.mod, mapEntityIds(*r.sim, *r.mod, map)});
    std::map<std::string, std::vector<Entity>> groups;   // "squad" -> squad#1..N
    for (auto& [name, idx] : spawnNames) {
        auto h = name.find('#');
        if (h != std::string::npos) { auto it = r.refs->ids.find(name); if (it != r.refs->ids.end()) groups[name.substr(0, h)].push_back(it->second); }
    }

    const uint32_t rate = r.sim->tickRate();
    const double maxSeconds = std::clamp(p["seconds"].toDouble(30), 0.05, 3600.0);
    const uint32_t limit = uint32_t(std::llround(maxSeconds * rate));
    const bool stopOnResult = p.getBool("stopOnResult", true);

    // a group name resolves to all of its members: expand string references before building commands
    std::function<Json(const Json&)> expand = [&](const Json& ref) -> Json {
        if (ref.isString()) {
            auto g = groups.find(ref.str());
            if (g != groups.end() && !r.refs->ids.count(ref.str())) {
                Json a = Json::array();
                for (Entity e : g->second) a.push(r.refs->refJson(e));
                return a;
            }
            return ref;
        }
        if (ref.isArray()) { Json a = Json::array(); for (auto& x : ref.items()) { Json y = expand(x); if (y.isArray()) for (auto& z : y.items()) a.push(z); else a.push(y); } return a; }
        return ref;
    };
    auto expandKeys = [&](const Json& c) {
        Json o = c;
        for (const char* k : {"units", "target", "alive", "dead", "count", "hp", "distance", "to", "on"})
            if (o.has(k) && (o[k].isString() || o[k].isArray())) o.set(k, expand(o[k]));
        return o;
    };

    // commands at their tick (stable: same-time commands keep their order)
    struct Timed { uint32_t tick; Json cmd; };
    std::vector<Timed> cmds;
    for (auto& c : p["commands"].items()) cmds.push_back({uint32_t(std::llround(std::max(0.0, c["t"].toDouble(0)) * rate)), expandKeys(c)});
    std::stable_sort(cmds.begin(), cmds.end(), [](const Timed& a, const Timed& b) { return a.tick < b.tick; });
    std::vector<Timed> checks, finalChecks;
    for (auto& c : p["checks"].items()) {
        if (c.has("t")) checks.push_back({uint32_t(std::llround(c["t"].toDouble() * rate)), expandKeys(c)});
        else finalChecks.push_back({0, expandKeys(c)});
    }
    std::stable_sort(checks.begin(), checks.end(), [](const Timed& a, const Timed& b) { return a.tick < b.tick; });

    // bots
    std::vector<RtsAI> bots;
    AIConfig ai;
    const Json& bj = p["bots"];
    for (auto& pl : r.sim->players()) {
        bool on = bj.isString() ? bj.str() == "all" : std::any_of(bj.items().begin(), bj.items().end(), [&](const Json& b) { return b.integer() == pl.slot; });
        if (on) bots.emplace_back(pl.slot, cfg.seed, ai);
    }

    // log options
    std::vector<int> logTypes;
    bool logAll = false;
    if ((p["events"].type() == Json::Type::Bool)) logAll = p["events"].boolean();
    for (auto& e : p["events"].items()) {
        int t = simEventByName(e.str());
        if (t < 0) throw std::runtime_error("events: unknown event '" + e.str() + "' (" + simEventNames() + ")");
        logTypes.push_back(t);
    }
    const int64_t maxLog = std::clamp<int64_t>(p["maxEvents"].integer(300), 1, 5000);
    const double every = p["every"].toDouble(0);
    const uint32_t everyTicks = every > 0 ? std::max<uint32_t>(1, uint32_t(std::llround(every * rate))) : 0;
    Json watchRefs = Json::array();
    for (auto& wref : p["watch"].items()) { Json e = expand(wref); if (e.isArray()) for (auto& x : e.items()) watchRefs.push(x); else watchRefs.push(e); }
    auto watchState = [&]() {
        Json arr = Json::array();
        for (auto& wref : watchRefs.items()) {
            std::vector<Entity> v;
            r.refs->resolve(wref, v);
            if (v.empty() && wref.isObject()) { Json j = Json::object(); j.set("filter", wref); j.set("count", Json::number(int64_t(0))); arr.push(j); continue; }
            for (Entity e : v) arr.push(entityJson(*r.refs, e));
        }
        return arr;
    };

    Json timeline = Json::array(), checkResults = Json::array(), errors = Json::array();
    size_t ci = 0, ki = 0;
    auto runChecksUpTo = [&](uint32_t tick) {
        while (ki < checks.size() && checks[ki].tick <= tick) {
            try { checkResults.push(evalCheck(r, checks[ki].cmd)); }
            catch (const std::exception& e) { Json x = Json::object(); x.set("check", checks[ki].cmd); x.set("error", str(e.what())); x.set("pass", Json::fromBool(false)); checkResults.push(x); }
            ++ki;
        }
    };
    runChecksUpTo(0);
    while (r.sim->tick() < limit) {
        while (ci < cmds.size() && cmds[ci].tick <= r.sim->tick()) {
            try {
                Command c = commandFromJson(*r.refs, cmds[ci].cmd, 1);
                c.tick = r.sim->tick();
                r.sim->inject(c);   // executes this very tick (no input delay), like a recorded command
            } catch (const std::exception& e) {
                Json x = Json::object(); x.set("command", cmds[ci].cmd); x.set("error", str(e.what())); errors.push(x);
            }
            ++ci;
        }
        for (auto& b : bots) b.update(*r.sim);
        r.sim->step();
        collectEvents(r, logTypes, logAll, maxLog);
        runChecksUpTo(r.sim->tick());
        if (everyTicks && r.sim->tick() % everyTicks == 0 && watchRefs.size()) {
            Json snap = Json::object();
            snap.set("t", num(r.seconds()));
            snap.set("objects", watchState());
            if (timeline.size() < 400) timeline.push(snap);
        }
        if (stopOnResult && r.sim->result() != 0) break;
    }
    for (; ki < checks.size(); ++ki) {   // checks scheduled after the end
        Json x = evalCheck(r, checks[ki].cmd);
        x.set("note", str("run ended before this time"));
        checkResults.push(x);
    }
    for (auto& c : finalChecks) {
        try { checkResults.push(evalCheck(r, c.cmd)); }
        catch (const std::exception& e) { Json x = Json::object(); x.set("check", c.cmd); x.set("error", str(e.what())); x.set("pass", Json::fromBool(false)); checkResults.push(x); }
    }

    Json out = Json::object();
    int passed = 0;
    for (auto& c : checkResults.items()) if (c.getBool("pass")) ++passed;
    out.set("passed", Json::fromBool(passed == int(checkResults.size()) && errors.size() == 0));
    out.set("seconds", num(r.seconds()));
    out.set("result", Json::number(int64_t(r.sim->result())));
    if (checkResults.size()) out.set("checks", checkResults);
    if (errors.size()) out.set("commandErrors", errors);
    if (watchRefs.size()) out.set("watch", watchState());
    if (timeline.size()) out.set("timeline", timeline);
    Json ec = Json::object();
    for (auto& [k, v] : r.eventCounts) if (!k.empty()) ec.set(k, Json::number(v));
    out.set("events", ec);
    if (r.eventLog.size()) out.set("eventLog", r.eventLog);
    if (r.eventLogDropped) out.set("eventLogDropped", Json::number(r.eventLogDropped));
    if (r.triggerLog.size()) out.set("triggers", r.triggerLog);
    if (r.messages.size()) out.set("messages", r.messages);
    Json names = Json::object();
    for (auto& [k, e] : r.refs->ids) names.set(k, Json::number(int64_t(e.packed())));
    if (names.size()) out.set("ids", names);
    char cs[32]; std::snprintf(cs, sizeof cs, "%016llx", (unsigned long long)r.sim->checksum());
    out.set("checksum", str(cs));
    out.set("summary", str("Сценарий: " + std::to_string(int(r.seconds())) + " с, проверок пройдено " + std::to_string(passed) + " из " +
                           std::to_string(checkResults.size()) + (errors.size() ? ", ошибок в командах: " + std::to_string(errors.size()) : "")));
    return out;
}
} // namespace

Json eventJson(const SimRefs& refs, const SimEvent& ev) {
    Json j = Json::object();
    j.set("event", str(simEventName(ev.type)));
    if (ev.player) j.set("player", Json::number(int64_t(ev.player == kHostileNeutral ? -1 : ev.player)));
    World& w = refs.sim.world();
    auto ref = [&](const char* key, Entity e) {
        if (!e.valid()) return;
        j.set(key, refs.refJson(e));
        if (w.alive(e) && w.has<Unit>(e)) j.set(std::string(key) + "Type", str(refs.mod.rules().types[w.get<Unit>(e).type].id));
    };
    ref("a", ev.a);
    ref("b", ev.b);
    if (ev.value) j.set("value", Json::number(ev.value));
    if ((ev.type == kEvDied || ev.type == kEvTrainDone || ev.type == kEvBuildDone || ev.type == kEvTrainStarted || ev.type == kEvBuildPlaced) &&
        ev.value >= 0 && size_t(ev.value) < refs.mod.rules().types.size())
        j.set("unitType", str(refs.mod.rules().types[size_t(ev.value)].id));
    if (ev.pos.x.raw || ev.pos.z.raw) { j.set("x", num(ev.pos.x.toDouble())); j.set("z", num(ev.pos.z.toDouble())); }
    return j;
}

void registerScenarioOps(Workspace& ws) {
    std::string schema = std::string(R"J({"properties": {
        "clear": {"type": "boolean", "description": "start from an empty copy of the map: no objects, roads, waves or start units; victory never triggers"},
        "spawn": {"type": "array", "description": "[{id, type, owner (1..N, 0 neutral, -1 creeps), at [x, z], count, spacing, garrison, amount}] - a count > 1 names members id#1..id#N and 'id' refers to all of them"},
        "resources": {"type": "object", "description": "{\"1\": {\"gold\": 1000}} starting stock per player slot"},
        "commands": {"type": "array", "description": "[{t: seconds, command, player (default 1), ...}] - same keys as play_command"},
        "checks": {"type": "array", "description": "[{t?: seconds (default: at the end), name?, one of alive|dead|count|hp|resource+player|event|trigger|variable|distance+to|status+on|result, comparison '>=' '>' '<=' '<' '==' '!=': number}]"},
        "watch": {"type": "array", "description": "references to report at the end (and every 'every' seconds in a timeline)"},
        "every": {"type": "number"},
        "events": {"description": "true = log every event, or a list of event names"},
        "maxEvents": {"type": "integer"},
        "bots": {"description": "player slots played by the AI, or \"all\""},
        "seconds": {"type": "number", "description": "length, default 30"},
        "seed": {"type": "integer"},
        "stopOnResult": {"type": "boolean", "description": "stop when the match is decided (default true)"},
        "victory": {"type": "object", "description": "override the map's victory block"},
        "fogOfWar": {"type": "string", "description": "full | explored | none"}}})J");
    ws.registerOp({"scenario_run",
        "Headless scripted test of a mechanic on a copy of the open map (nothing is saved): place named objects, give commands at "
        "given seconds, let bots play some slots, then get event and trigger logs, watched objects and pass/fail checks. "
        "Commands: " + commandNames() + ". " + kCommandHelp + " Event names: " + simEventNames() + ".",
        Json::parse(schema), [&ws](const Json& p) { return scenarioRun(ws, p); }});
}

} // namespace rts::editor
