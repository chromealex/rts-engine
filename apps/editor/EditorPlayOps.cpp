// Agent ops that act inside the play-test: list live objects and issue player commands, so an agent
// can test a mechanic by playing it (surrender, abilities, digging, roads, ...). Commands go through
// Simulation::submit like player input, so the play-test stays deterministic and replayable.
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "Editor.h"

namespace rts::editor {

namespace {
using namespace rts::game;

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

Fixed fx(double v) { return Fixed::fromRaw(int64_t(std::llround(v * double(Fixed::kOne)))); }

std::string commandNames() {
    std::string s;
    for (auto& c : kCommands) s += (s.empty() ? "" : ", ") + std::string(c.name);
    return s;
}
} // namespace

void Editor::registerPlayOps() {
    auto add = [this](const char* name, const std::string& desc, const char* schema, Workspace::OpFn fn) {
        ws_.registerOp({name, desc, Json::parse(schema), std::move(fn), true});
    };
    add("play_units",
        "Live objects of the running play-test: entity id (for play_command), type, owner, position, hp, current order. "
        "Filter by owner / type; at most 'limit' (default 200).",
        R"({"properties": {"owner": {"type": "integer"}, "type": {"type": "string"}, "limit": {"type": "integer"}}})",
        [this](const Json& p) {
            if (!sim_) throw std::runtime_error("no play-test running (play_start)");
            const auto& R = rules();
            const int wantType = p.has("type") ? R.findType(p.getStr("type")) : -1;
            if (p.has("type") && wantType < 0) throw std::runtime_error("unknown type '" + p.getStr("type") + "'");
            const int64_t limit = std::clamp<int64_t>(p["limit"].integer(200), 1, 2000);
            Json arr = Json::array();
            int64_t total = 0;
            sim_->world().each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
                if (p.has("owner") && o.player != p["owner"].integer()) return;
                if (wantType >= 0 && u.type != wantType) return;
                if (++total > limit) return;
                Json j = Json::object();
                j.set("id", Json::number(int64_t(e.packed())));
                j.set("type", Json::string(R.types[u.type].id));
                j.set("owner", Json::number(int64_t(o.player)));
                j.set("x", Json::number(t.pos.x.toDouble(), 2));
                j.set("z", Json::number(t.pos.z.toDouble(), 2));
                j.set("hp", Json::number(u.hp.toDouble(), 1));
                if (auto* od = sim_->world().tryGet<Orders>(e); od && !od->queue.empty())
                    j.set("order", Json::number(int64_t(od->queue.front().type)));
                arr.push(j);
            });
            Json r = Json::object();
            r.set("units", arr);
            r.set("total", Json::number(total));
            return r;
        });
    add("play_command",
        "Issue a player command in the running play-test, exactly like player input. command: " + commandNames() + ". "
        "units: entity ids from play_units; target: entity id; x, z: point. param by name: 'type' (build/train/upgrade), "
        "'tech' (research), 'room' (paint_room), 'player_ability' (id); or a raw 'param'. Rect commands (dig, paint_room, "
        "build_road): 'rect' [w, h] at x, z with 'extra' (dig: 1 unmark; build_road: 1 remove). player defaults to the "
        "human slot or 1.",
        R"({"properties": {"command": {"type": "string"}, "player": {"type": "integer"},
            "units": {"type": "array", "items": {"type": "integer"}}, "target": {"type": "integer"},
            "x": {"type": "number"}, "z": {"type": "number"}, "type": {"type": "string"}, "tech": {"type": "string"},
            "room": {"type": "string"}, "player_ability": {"type": "string"}, "param": {"type": "integer"},
            "rect": {"type": "array", "items": {"type": "integer"}}, "extra": {"type": "integer"}},
            "required": ["command"]})",
        [this](const Json& p) {
            if (!sim_) throw std::runtime_error("no play-test running (play_start)");
            const auto& R = rules();
            const std::string name = p.getStr("command");
            auto it = std::find_if(std::begin(kCommands), std::end(kCommands), [&](const CmdName& c) { return name == c.name; });
            if (it == std::end(kCommands)) throw std::runtime_error("unknown command '" + name + "' (" + commandNames() + ")");
            Command c;
            c.type = it->type;
            c.player = uint8_t(p["player"].integer(human_ > 0 ? human_ : 1));
            for (auto& u : p["units"].items()) c.units.push_back(Entity::fromPacked(uint64_t(u.integer())));
            if (p.has("target")) c.target = Entity::fromPacked(uint64_t(p["target"].integer()));
            c.point = {fx(p["x"].toDouble(0)), Fixed{}, fx(p["z"].toDouble(0))};
            c.param = p["param"].integer(0);
            auto need = [](int idx, const std::string& what, const std::string& id) {
                if (idx < 0) throw std::runtime_error("unknown " + what + " '" + id + "'");
                return idx;
            };
            if (p.has("type")) c.param = need(R.findType(p.getStr("type")), "type", p.getStr("type"));
            if (p.has("tech")) c.param = need(R.findTech(p.getStr("tech")), "tech", p.getStr("tech"));
            if (p.has("player_ability")) {
                const std::string id = p.getStr("player_ability");
                int idx = -1;
                for (size_t i = 0; i < R.playerAbilities.size(); ++i) if (R.playerAbilities[i].id == id) idx = int(i);
                c.param = need(idx, "player ability", id);
            }
            if (p.has("rect")) {
                const Json& r = p["rect"];
                int extra = int(p["extra"].integer(0));
                if (p.has("room")) extra = p.getStr("room").empty() ? -1 : need(R.findRoom(p.getStr("room")), "room", p.getStr("room"));
                c.param = rectParam(int(r[0].integer(1)), int(r[1].integer(1)), extra);
            }
            sim_->submit(c);
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", Json::string("Команда " + name + " от игрока " + std::to_string(c.player)));
            r.set("tick", Json::number(int64_t(sim_->tick())));
            return r;
        });
}

} // namespace rts::editor
