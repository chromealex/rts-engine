// Strategic map tools for agents (agent-mech stage 5, TB): load a campaign map of provinces and armies, give
// orders, let bots play turns, fight battles auto-resolved or simulated on the game's own rules. The state lives
// in the workspace process; strategic_save writes it to the project. See modules/strategic/Strategic.h.
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include "Workspace.h"
#include "modules/strategic/Strategic.h"

namespace rts::editor {

namespace fs = std::filesystem;
using strategic::StrategicGame;

namespace {
Json str(const std::string& s) { return Json::string(s); }
std::map<const Workspace*, std::unique_ptr<StrategicGame>>& games() { static std::map<const Workspace*, std::unique_ptr<StrategicGame>> g; return g; }
StrategicGame& game(Workspace& ws) {
    auto it = games().find(&ws);
    if (it == games().end() || !it->second) throw std::runtime_error("no strategic map loaded (strategic_load)");
    return *it->second;
}
Json summary(const StrategicGame& g) {
    Json r = Json::object();
    r.set("turn", Json::number(int64_t(g.turn())));
    r.set("current", Json::number(int64_t(g.current())));
    r.set("winner", Json::number(int64_t(g.winner())));
    Json owners = Json::object();
    for (auto& p : g.provinces()) owners.set(p.id, Json::number(int64_t(p.owner)));
    r.set("owners", owners);
    r.set("armies", Json::number(int64_t(g.armies().size())));
    r.set("battles", Json::number(int64_t(g.battles().size())));
    r.set("summary", str("Ход " + std::to_string(g.turn()) + ", ходит игрок " + std::to_string(g.current()) +
                         (g.winner() ? ", победил игрок " + std::to_string(g.winner()) : "")));
    return r;
}
} // namespace

void registerStrategicOps(Workspace& ws) {
    ws.registerOp({"strategic_load",
        "Load a strategic (campaign) map for the open game: provinces {id, adj, owner, income, pos, terrain}, armies {owner, at, "
        "units: {type: count}}, players, start resources, recruit costs, movesPerTurn, battle {mode: auto|simulate, seconds, size}. "
        "'file' (project-relative, e.g. strategic.json) or 'def' inline. Battles use the game's rules.json.",
        Json::parse(R"({"properties": {"file": {"type": "string"}, "def": {"type": "object"}, "seed": {"type": "integer"}}})"),
        [&ws](const Json& p) {
            if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            Json def = p["def"];
            if (!def.isObject()) {
                const std::string f = p.getStr("file", "strategic.json");
                const fs::path path = fs::path(ws.project.dir) / f;
                if (!fs::exists(path)) throw std::runtime_error("file not found: " + f);
                def = Json::parseFile(path.string());
            }
            games()[&ws] = std::make_unique<StrategicGame>(StrategicGame::create(def, ws.project.rulesJson, uint64_t(p.getInt("seed", 1))));
            return summary(game(ws));
        }});
    ws.registerOp({"strategic_state", "The whole strategic state: provinces with owners, armies with units and strength, treasury, battle log.",
        Json::parse("{}"), [&ws](const Json&) { Json r = game(ws).toJson(); r.set("checksum", Json::number(int64_t(game(ws).checksum() & 0x7FFFFFFFFFFFull))); return r; }});
    ws.registerOp({"strategic_order",
        "Give an order for the current player (or 'player'): {\"move\": {\"army\": id, \"to\": province}} (an enemy army there means a "
        "battle), {\"recruit\": {\"province\", \"type\", \"count\"}}, or {\"end_turn\": true}.",
        Json::parse(R"({"properties": {"player": {"type": "integer"}, "move": {"type": "object"}, "recruit": {"type": "object"}, "end_turn": {"type": "boolean"}}})"),
        [&ws](const Json& p) {
            StrategicGame& g = game(ws);
            const int player = int(p.getInt("player", g.current()));
            std::string err;
            const size_t battles = g.battles().size();
            bool ok = true;
            if (p["move"].isObject()) {
                const int to = g.findProvince(p["move"].getStr("to"));
                ok = g.move(player, int(p["move"].getInt("army")), to, &err);
            } else if (p["recruit"].isObject()) {
                ok = g.recruit(player, g.findProvince(p["recruit"].getStr("province")), p["recruit"].getStr("type"), int(p["recruit"].getInt("count", 1)), &err);
            } else if (p.getBool("end_turn")) {
                ok = g.endTurn(player, &err);
            } else throw std::runtime_error("give move, recruit or end_turn");
            if (!ok) throw std::runtime_error(err);
            Json r = summary(g);
            if (g.battles().size() > battles) r.set("battle", g.toJson()["battles"][g.battles().size() - 1]);
            return r;
        }});
    ws.registerOp({"strategic_bots",
        "Bots play 'turns' player turns (default 10) for 'players' (default: all). 'battle': auto | simulate (play every battle on "
        "the RTS simulation with bots; slower). Returns the battles fought and the result.",
        Json::parse(R"({"properties": {"turns": {"type": "integer"}, "players": {"type": "array"}, "battle": {"type": "string"}}})"),
        [&ws](const Json& p) {
            StrategicGame& g = game(ws);
            if (p.has("battle")) {
                const std::string m = p.getStr("battle");
                if (m != "auto" && m != "simulate") throw std::runtime_error("battle: auto or simulate");
                g.battleMode = m;
            }
            std::vector<int> who;
            for (auto& x : p["players"].items()) who.push_back(int(x.integer(0)));
            const size_t b0 = g.battles().size();
            const int turns = int(p.getInt("turns", 10));
            for (int k = 0; k < turns && !g.winner(); ++k) {
                const int cur = g.current();
                if (!who.empty() && std::find(who.begin(), who.end(), cur) == who.end()) break;   // a human's turn
                g.botTurn(cur);
            }
            Json r = summary(g);
            Json bl = Json::array();
            Json all = g.toJson()["battles"];
            for (size_t i = b0; i < all.size(); ++i) bl.push(all[i]);
            r.set("newBattles", bl);
            return r;
        }});
    ws.registerOp({"strategic_battle_map",
        "The RTS map of the battle the army 'army' would fight by moving to province 'to' (attacker = player 1 on the left). "
        "Save it with map_save / play it in the Game window, then report the result with strategic_order as usual (auto) or "
        "use battle mode simulate.",
        Json::parse(R"({"properties": {"army": {"type": "integer"}, "to": {"type": "string"}}, "required": ["army", "to"]})"),
        [&ws](const Json& p) {
            StrategicGame& g = game(ws);
            const int to = g.findProvince(p.getStr("to"));
            if (to < 0) throw std::runtime_error("unknown province");
            const strategic::Army* a = nullptr;
            const strategic::Army* d = nullptr;
            for (auto& x : g.armies()) if (x.id == int(p.getInt("army"))) a = &x;
            if (!a) throw std::runtime_error("no such army");
            for (auto& x : g.armies()) if (x.at == to && x.owner != a->owner) d = &x;
            if (!d) throw std::runtime_error("no enemy army there");
            Json r = Json::object();
            r.set("map", g.battleMap(*a, *d, to));
            return r;
        }});
    ws.registerOp({"strategic_save", "Write the strategic state to the project ('file', default strategic_save.json); strategic_load reads it back.",
        Json::parse(R"({"properties": {"file": {"type": "string"}}})"), [&ws](const Json& p) {
            if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            const std::string f = p.getStr("file", "strategic_save.json");
            std::ofstream(fs::path(ws.project.dir) / f) << game(ws).toJson().dump(2);
            Json r = summary(game(ws));
            r.set("file", str(f));
            return r;
        }});
}

} // namespace rts::editor
