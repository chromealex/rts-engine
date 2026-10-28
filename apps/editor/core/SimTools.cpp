// See SimTools.h.
#include "SimTools.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include "modules/rts/RtsTiles.h"
#include <stdexcept>

namespace rts::editor {

using namespace rts::game;

namespace {
// Index = GameEvent value (RtsModule.h). Keep in the enum's order; new events go at the end.
const char* const kEventNames[] = {
    "", "spawned", "died", "attack", "hit", "trainStarted", "trained", "buildPlaced", "built",
    "gathered", "captured", "playerDefeated", "notEnough", "researched", "levelUp", "ability", "miss",
    "readyToPlace", "sold", "regionClaimed", "wave", "lowPower", "upgraded",
    "message", "dug", "territory", "roomPainted", "recruited", "creatureLeft",
    "pickedUp", "dropped", "slapped", "possessed", "payday", "playerAbility", "produced", "delivered",
    "roadBuilt", "lanesChanged",
    "statusStart", "statusEnd", "areaStart", "areaTrigger", "areaEnd", "projectileLaunch", "impact",
    "chainJump", "castStart", "channelStart", "channelEnd", "alert", "garrisonChanged",
    "triggerFired",
};
constexpr size_t kEventCount = sizeof(kEventNames) / sizeof(kEventNames[0]);
static_assert(kEventCount == size_t(kEvTriggerFired) + 1, "kEventNames must list every GameEvent");

struct CmdName { const char* name; uint16_t type; };
const CmdName kCommands[] = {
    {"move", kCmdMove}, {"attack", kCmdAttack}, {"stop", kCmdStop}, {"hold", kCmdHold}, {"patrol", kCmdPatrol},
    {"gather", kCmdGather}, {"build", kCmdBuild}, {"train", kCmdTrain}, {"cancel_train", kCmdCancelTrain},
    {"rally", kCmdRally}, {"smart", kCmdSmart}, {"send", kCmdSend}, {"research", kCmdResearch}, {"place", kCmdPlace},
    {"sell", kCmdSell}, {"deploy", kCmdDeploy}, {"ability", kCmdAbility}, {"reinforce", kCmdReinforce},
    {"claim", kCmdClaim}, {"auto_build", kCmdAutoBuild}, {"upgrade", kCmdUpgrade}, {"surrender", kCmdSurrender},
    {"dig", kCmdDig}, {"paint_room", kCmdPaintRoom}, {"pick_up", kCmdPickUp}, {"drop", kCmdDrop}, {"slap", kCmdSlap},
    {"possess", kCmdPossess}, {"direct", kCmdDirect}, {"player_ability", kCmdPlayerAbility},
    {"build_road", kCmdBuildRoad}, {"connect_road", kCmdConnectRoad},
};

Vec3 readPoint(const Json& p) {
    if (p["at"].isArray()) return {toFixed(p["at"][0].toDouble(0)), Fixed{}, toFixed(p["at"][1].toDouble(0))};
    return {toFixed(p["x"].toDouble(0)), Fixed{}, toFixed(p["z"].toDouble(0))};
}
} // namespace

Json mapForSim(Json map, const std::string& gameDir) {
    auto read = [&gameDir](const std::string& p, std::string& out) {
        std::ifstream f(gameDir + "/" + p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    };
    std::string err;
    if (!game::applyTilesToMap(map, read, &err)) throw std::runtime_error(err);
    return map;
}

Fixed toFixed(double v) { return Fixed::fromRaw(int64_t(std::llround(v * double(Fixed::kOne)))); }

const char* simEventName(uint16_t type) { return type < kEventCount ? kEventNames[type] : ""; }
int simEventByName(const std::string& name) {
    for (size_t i = 1; i < kEventCount; ++i) if (name == kEventNames[i]) return int(i);
    return -1;
}
std::string simEventNames() {
    std::string s;
    for (size_t i = 1; i < kEventCount; ++i) s += (i > 1 ? ", " : "") + std::string(kEventNames[i]);
    return s;
}
const char* orderName(OrderType t) {
    static const char* const n[] = {"move", "attackMove", "attack", "patrol", "gather", "return", "build", "repair",
                                    "enter", "cast", "dig", "claimCell", "need", "haul"};
    size_t i = size_t(t);
    return i < sizeof(n) / sizeof(n[0]) ? n[i] : "?";
}
std::string commandNames() {
    std::string s;
    for (auto& c : kCommands) s += (s.empty() ? "" : ", ") + std::string(c.name);
    return s;
}

// ---------------------------------------------------------------- references
bool SimRefs::matches(const Json& f, Entity e) const {
    World& w = sim.world();
    if (!w.alive(e) || !w.has<Unit>(e)) return false;
    const Unit& u = w.get<Unit>(e);
    if (u.hp.raw <= 0) return false;
    const Rules& R = mod.rules();
    if (f.has("type") && R.types[u.type].id != f.getStr("type")) return false;
    if (f.has("tags")) {
        for (auto& t : f["tags"].items()) {
            auto it = std::find(R.tagNames.begin(), R.tagNames.end(), t.str());
            if (it == R.tagNames.end() || !(R.types[u.type].tags & (uint64_t(1) << (it - R.tagNames.begin())))) return false;
        }
    }
    if (f.has("owner")) {
        int64_t o = f["owner"].integer();
        uint8_t want = o == -1 ? kHostileNeutral : uint8_t(o);
        if (!w.has<Owner>(e) || w.get<Owner>(e).player != want) return false;
    }
    if (f.has("kind")) {
        bool st = R.types[u.type].structure;
        if ((f.getStr("kind") == "structure") != st) return false;
    }
    const Vec3 pos = w.get<Transform>(e).pos;
    if (f["area"].isArray()) {
        const Json& a = f["area"];
        double x = pos.x.toDouble(), z = pos.z.toDouble(), ax = a[0].toDouble(), az = a[1].toDouble();
        if (x < ax || z < az || x >= ax + a[2].toDouble() || z >= az + a[3].toDouble()) return false;
    }
    if (f["near"].isArray()) {
        double dx = pos.x.toDouble() - f["near"][0].toDouble(), dz = pos.z.toDouble() - f["near"][1].toDouble();
        double r = f["radius"].toDouble(3);
        if (dx * dx + dz * dz > r * r) return false;
    }
    return true;
}

void SimRefs::resolve(const Json& ref, std::vector<Entity>& out) const {
    if (ref.isArray()) { for (auto& r : ref.items()) resolve(r, out); return; }
    if (ref.isNumber()) { out.push_back(Entity::fromPacked(uint64_t(ref.integer()))); return; }
    if (ref.isString()) {
        auto it = ids.find(ref.str());
        if (it == ids.end()) throw std::runtime_error("unknown object id '" + ref.str() + "'");
        out.push_back(it->second);
        return;
    }
    if (ref.isObject()) {
        sim.world().each<Unit>([&](Entity e, Unit&) { if (matches(ref, e)) out.push_back(e); });
        return;
    }
    throw std::runtime_error("object reference must be an id number, a name or a filter object");
}

Entity SimRefs::one(const Json& ref) const {
    std::vector<Entity> v;
    resolve(ref, v);
    return v.empty() ? kNullEntity : v.front();
}

Json SimRefs::refJson(Entity e) const {
    for (auto& [k, v] : ids) if (v == e) return Json::string(k);
    return Json::number(int64_t(e.packed()));
}

std::map<std::string, Entity> mapEntityIds(Simulation& sim, RtsModule& mod, const Json& map) {
    std::map<std::string, Entity> out;
    std::vector<Entity> used;
    World& w = sim.world();
    for (auto& ej : map["entities"].items()) {
        if (!ej.has("id")) continue;
        int t = mod.rules().findType(ej.getStr("type"));
        int64_t o = ej.getInt("owner", 0);
        uint8_t owner = o == -1 ? kHostileNeutral : uint8_t(o);
        double px = ej["pos"][0].toDouble(), pz = ej["pos"][1].toDouble();
        Entity best = kNullEntity;
        double bd = 1e18;
        w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& ow, Transform& tr) {
            if (int(u.type) != t || ow.player != owner) return;
            if (std::find(used.begin(), used.end(), e) != used.end()) return;
            double dx = tr.pos.x.toDouble() - px, dz = tr.pos.z.toDouble() - pz, d = dx * dx + dz * dz;
            if (d < bd) { bd = d; best = e; }
        });
        if (best.valid()) { used.push_back(best); out[ej.getStr("id")] = best; }
    }
    return out;
}

// ---------------------------------------------------------------- commands
const char* kCommandHelp =
    "units: object references (packed id numbers from play_units, names from map/scenario 'id', or a filter "
    "{type, owner, tags, kind, area [x,z,w,h], near [x,z] + radius}); target: one reference; point: 'at' [x, z] or x, z. "
    "param by name: 'type' (build/train/upgrade/place), 'tech' (research), 'ability' (ability id of the first unit's type), "
    "'player_ability' (id), 'room' (paint_room), 'percent' (send); or a raw 'param'. Rect commands (dig, paint_room, "
    "build_road): 'rect' [w, h] at the point with 'extra' (dig: 1 unmark; build_road: 1 remove). 'queue': true appends "
    "to the current orders (Shift).";

const char* commandSchemaProps() {
    return R"("command": {"type": "string"}, "player": {"type": "integer"},
        "units": {"description": "references: id numbers, names or a filter object"}, "target": {"description": "one reference"},
        "at": {"type": "array", "items": {"type": "number"}}, "x": {"type": "number"}, "z": {"type": "number"},
        "type": {"type": "string"}, "tech": {"type": "string"}, "ability": {"type": "string"}, "room": {"type": "string"},
        "player_ability": {"type": "string"}, "percent": {"type": "integer"}, "param": {"type": "integer"},
        "rect": {"type": "array", "items": {"type": "integer"}}, "extra": {"type": "integer"}, "queue": {"type": "boolean"})";
}

Command commandFromJson(const SimRefs& refs, const Json& p, uint8_t defPlayer) {
    const Rules& R = refs.mod.rules();
    const std::string name = p.getStr("command");
    auto it = std::find_if(std::begin(kCommands), std::end(kCommands), [&](const CmdName& c) { return name == c.name; });
    if (it == std::end(kCommands)) throw std::runtime_error("unknown command '" + name + "' (" + commandNames() + ")");
    Command c;
    c.type = it->type;
    c.player = uint8_t(p["player"].integer(defPlayer));
    if (p.has("units")) refs.resolve(p["units"], c.units);
    if (p.has("target")) {
        c.target = refs.one(p["target"]);
        if (!c.target.valid()) throw std::runtime_error("target matches no live object");
    }
    c.point = readPoint(p);
    if (!p.has("at") && !p.has("x") && c.target.valid() && refs.sim.world().alive(c.target))
        c.point = refs.sim.world().get<Transform>(c.target).pos;
    c.param = p["param"].integer(0);
    if (p.getBool("queue")) c.flags |= kCmdQueue;
    auto need = [](int idx, const std::string& what, const std::string& id) {
        if (idx < 0) throw std::runtime_error("unknown " + what + " '" + id + "'");
        return idx;
    };
    if (p.has("percent")) c.param = p["percent"].integer(50);
    if (p.has("type")) c.param = need(R.findType(p.getStr("type")), "type", p.getStr("type"));
    if (p.has("tech")) c.param = need(R.findTech(p.getStr("tech")), "tech", p.getStr("tech"));
    if (p.has("player_ability")) c.param = need(R.findPlayerAbility(p.getStr("player_ability")), "player ability", p.getStr("player_ability"));
    if (p.has("ability")) {
        const std::string id = p.getStr("ability");
        int idx = -1;
        World& w = refs.sim.world();
        for (Entity e : c.units) {
            if (!w.alive(e) || !w.has<Unit>(e)) continue;
            auto& ab = R.types[w.get<Unit>(e).type].abilities;
            for (size_t i = 0; i < ab.size(); ++i) if (ab[i].id == id) { idx = int(i); break; }
            break;
        }
        c.param = need(idx, "ability of the first unit's type", id);
    }
    if (p.has("rect")) {
        const Json& r = p["rect"];
        int extra = int(p["extra"].integer(0));
        if (p.has("room")) extra = p.getStr("room").empty() ? -1 : need(R.findRoom(p.getStr("room")), "room", p.getStr("room"));
        c.param = rectParam(int(r[0].integer(1)), int(r[1].integer(1)), extra);
    }
    return c;
}

// ---------------------------------------------------------------- state
Json entityJson(const SimRefs& refs, Entity e) {
    World& w = refs.sim.world();
    const Rules& R = refs.mod.rules();
    Json j = Json::object();
    j.set("id", refs.refJson(e));
    if (!w.alive(e) || !w.has<Unit>(e) || w.get<Unit>(e).hp.raw <= 0) { j.set("alive", Json::fromBool(false)); return j; }
    const Unit& u = w.get<Unit>(e);
    j.set("alive", Json::fromBool(true));
    j.set("type", Json::string(R.types[u.type].id));
    if (w.has<Owner>(e)) j.set("owner", Json::number(int64_t(w.get<Owner>(e).player == kHostileNeutral ? -1 : w.get<Owner>(e).player)));
    const Vec3 pos = w.get<Transform>(e).pos;
    j.set("x", Json::number(pos.x.toDouble(), 2));
    j.set("z", Json::number(pos.z.toDouble(), 2));
    j.set("hp", Json::number(u.hp.toDouble(), 1));
    j.set("maxHp", Json::number(refs.mod.stat(refs.sim, e, Stat::MaxHp).toDouble(), 1));
    if (R.types[u.type].maxEnergy.raw > 0) j.set("energy", Json::number(u.energy.toDouble(), 1));
    if (u.flags & kUnitUnderConstruction) j.set("underConstruction", Json::fromBool(true));
    if (u.flags & kUnitHold) j.set("hold", Json::fromBool(true));
    if (auto* od = w.tryGet<Orders>(e); od && !od->queue.empty()) {
        Json q = Json::array();
        for (auto& o : od->queue) q.push(Json::string(orderName(o.type)));
        j.set("orders", q);
    }
    if (auto* st = w.tryGet<Statuses>(e); st && !st->list.empty()) {
        Json s = Json::array();
        for (auto& si : st->list) s.push(Json::string(si.def < R.statuses.size() ? R.statuses[si.def].id : "?"));
        j.set("statuses", s);
    }
    if (auto* g = w.tryGet<Garrison>(e)) j.set("garrison", Json::number(int64_t(g->count)));
    if (auto* c = w.tryGet<Cargo>(e); c && c->res >= 0 && c->amount.raw > 0) {
        Json cj = Json::object();
        cj.set(R.resources[size_t(c->res)].id, Json::number(c->amount.toDouble(), 1));
        j.set("carrying", cj);
    }
    if (auto* h = w.tryGet<Hero>(e)) j.set("level", Json::number(int64_t(h->level)));
    if (auto* rs = w.tryGet<ResourceStock>(e); rs && !rs->infinite) j.set("amount", Json::number(rs->amount.toDouble(), 0));
    return j;
}

} // namespace rts::editor
