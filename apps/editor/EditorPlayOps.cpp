// Agent ops that act inside the play-test: list live objects and issue player commands, so an agent
// can test a mechanic by playing it (surrender, abilities, digging, roads, ...). Commands go through
// Simulation::submit like player input, so the play-test stays deterministic and replayable.
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "Editor.h"
#include "core/SimTools.h"

namespace rts::editor {

using namespace rts::game;

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
            sim_->world().each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
                if (p.has("owner") && o.player != p["owner"].integer()) return;
                if (wantType >= 0 && u.type != wantType) return;
                if (++total > limit) return;
                SimRefs refs{*sim_, *simModule(), playIds_};
                Json j = entityJson(refs, e);
                j.set("id", Json::number(int64_t(e.packed())));
                for (auto& [name, pe] : playIds_) if (pe == e) j.set("name", Json::string(name));
                arr.push(j);
            });
            Json r = Json::object();
            r.set("units", arr);
            r.set("total", Json::number(total));
            return r;
        });
    add("play_command",
        "Issue a player command in the running play-test, exactly like player input. command: " + commandNames() + ". " +
        kCommandHelp + " Map entity names ('id' in the map) work as references. player defaults to the human slot or 1.",
        (std::string(R"({"properties": {)") + commandSchemaProps() + R"(}, "required": ["command"]})").c_str(),
        [this](const Json& p) {
            if (!sim_) throw std::runtime_error("no play-test running (play_start)");
            SimRefs refs{*sim_, *simModule(), playIds_};
            Command c = commandFromJson(refs, p, uint8_t(human_ > 0 ? human_ : 1));
            sim_->submit(c);
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", Json::string("Команда " + p.getStr("command") + " от игрока " + std::to_string(c.player)));
            r.set("tick", Json::number(int64_t(sim_->tick())));
            r.set("units", Json::number(int64_t(c.units.size())));
            return r;
        });
}

} // namespace rts::editor
