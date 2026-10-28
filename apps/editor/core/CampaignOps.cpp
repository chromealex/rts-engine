// Campaign tools for agents: read / write campaign.json and play a mission chain headless (bots on every side,
// the campaign player's survivors and resources carried over as the campaign says). See modules/rts/RtsCampaign.h.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include "SimTools.h"
#include "Workspace.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsCampaign.h"

namespace rts::editor {

namespace fs = std::filesystem;
using namespace rts::game;

namespace {
Json str(const std::string& s) { return Json::string(s); }
std::string campaignPath(Workspace& ws) { return ws.project.dir + "/campaign.json"; }

const Json* findMission(const Json& camp, const std::string& id) {
    for (auto& m : camp["missions"].items()) if (m.getStr("id") == id) return &m;
    return nullptr;
}

std::string validate(Workspace& ws, const Json& camp) {
    if (!camp["missions"].isArray() || camp["missions"].size() == 0) return "campaign needs missions";
    for (auto& m : camp["missions"].items()) {
        const std::string id = m.getStr("id");
        if (id.empty()) return "every mission needs an id";
        if (!fs::exists(fs::path(ws.project.dir) / m.getStr("map"))) return "mission '" + id + "': map file not found '" + m.getStr("map") + "'";
        if (m.has("next") && !findMission(camp, m.getStr("next"))) return "mission '" + id + "': unknown next '" + m.getStr("next") + "'";
    }
    if (camp.has("start") && !findMission(camp, camp.getStr("start"))) return "unknown start mission '" + camp.getStr("start") + "'";
    return "";
}
} // namespace

void registerCampaignOps(Workspace& ws) {
    ws.registerOp({"campaign_get", "Read the game's campaign.json (missions: id, name, map, briefing, next, carry). Empty when the game has none.",
        Json::parse("{}"), [&ws](const Json&) {
            if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            Json r = Json::object();
            if (!fs::exists(campaignPath(ws))) { r.set("campaign", Json()); r.set("summary", str("Кампании нет")); return r; }
            r.set("campaign", Json::parseFile(campaignPath(ws)));
            return r;
        }});
    ws.registerOp({"campaign_set", "Write campaign.json: {\"start\": id, \"missions\": [{\"id\", \"name\", \"map\", \"briefing\", \"next\", "
        "\"carry\": {\"resources\": [...], \"tags\": [...], \"types\": [...], \"max\": n}}]}. Missions end with the trigger action "
        "endMission (result, next) or a normal victory; objectives, dialog and camera are trigger actions too.",
        Json::parse(R"({"properties": {"campaign": {"type": "object"}}, "required": ["campaign"]})"), [&ws](const Json& p) {
            if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            std::string err = validate(ws, p["campaign"]);
            if (!err.empty()) throw std::runtime_error(err);
            std::ofstream(campaignPath(ws)) << p["campaign"].dump(2);
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str("Кампания: " + std::to_string(p["campaign"]["missions"].size()) + " миссий"));
            return r;
        }});
    ws.registerOp({"campaign_run", "Play the campaign headless from a mission (default: start): bots on every side, each mission until it ends "
        "or 'seconds'; survivors and resources are carried as the mission's 'carry' says. Reports each mission's outcome, objectives "
        "and the carry.",
        Json::parse(R"({"properties": {"from": {"type": "string"}, "maxMissions": {"type": "integer"}, "seconds": {"type": "number"}, "seed": {"type": "integer"}}})"),
        [&ws](const Json& p) {
            if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            if (!fs::exists(campaignPath(ws))) throw std::runtime_error("the game has no campaign.json (campaign_set)");
            Json camp = Json::parseFile(campaignPath(ws));
            std::string err = validate(ws, camp);
            if (!err.empty()) throw std::runtime_error(err);
            std::string id = p.getStr("from", camp.getStr("start", camp["missions"][size_t(0)].getStr("id")));
            const int maxM = int(p.getInt("maxMissions", 10));
            const double secs = p["seconds"].toDouble(900);
            Json carry = Json::object(), report = Json::array();
            for (int k = 0; k < maxM && !id.empty(); ++k) {
                const Json* mission = findMission(camp, id);
                if (!mission) throw std::runtime_error("unknown mission '" + id + "'");
                Json map = mapForSim(Json::parseFile(ws.project.dir + "/" + mission->getStr("map")), ws.project.dir);
                applyCarry(map, carry, 1);
                SimConfig cfg;
                cfg.seed = uint64_t(p.getInt("seed", 1)) + uint64_t(k);
                Simulation sim(cfg, std::make_unique<RtsModule>());
                sim.setup(ws.project.rulesJson, map);
                restoreCarry(sim, carry, 1);
                std::vector<RtsAI> bots;
                for (auto& pl : sim.players()) bots.emplace_back(pl.slot, cfg.seed);
                MissionOutcome out;
                const uint32_t limit = uint32_t(secs * sim.tickRate());
                while (sim.tick() < limit) {
                    for (auto& b : bots) b.update(sim);
                    sim.step();
                    out = missionOutcome(sim, *mission, 1);
                    if (out.finished) break;
                }
                auto* mod = dynamic_cast<RtsModule*>(&sim.module());
                Json mj = Json::object();
                mj.set("mission", str(id));
                mj.set("finished", Json::fromBool(out.finished));
                mj.set("won", Json::fromBool(out.won));
                mj.set("seconds", Json::number(int64_t(sim.tick() / sim.tickRate())));
                Json objs = Json::array();
                for (auto& o : mod->state(sim).mech.objectives) {
                    Json oj = Json::object();
                    oj.set("id", str(o.id)); oj.set("state", str(o.state == 1 ? "done" : o.state == 2 ? "failed" : "active"));
                    objs.push(oj);
                }
                mj.set("objectives", objs);
                mj.set("dialogs", Json::number(int64_t(mod->state(sim).mech.dialogs.size())));
                if (out.won && !out.next.empty()) { carry = collectCarry(sim, (*mission)["carry"], 1); mj.set("carry", carry); }
                mj.set("next", str(out.next));
                report.push(mj);
                id = out.won ? out.next : "";
            }
            Json r = Json::object();
            r.set("missions", report);
            r.set("summary", str("Кампания: сыграно миссий " + std::to_string(report.size())));
            return r;
        }});
}

} // namespace rts::editor
