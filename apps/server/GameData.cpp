#include "GameData.h"
#include <fstream>
#include <iterator>
#include "engine/sim/Replay.h"
#include "modules/rts/RtsModule.h"
#include "modules/rts/RtsTiles.h"
#include "modules/rts/RtsScript.h"

namespace rts::netgame {

MatchData loadMatchData(const std::string& gameDir, const std::string& mapFile) {
    MatchData d;
    d.manifest = Json::parseFile(gameDir + "/game.json");
    d.module = d.manifest.getStr("module", "rts");
    d.rules = Json::parseFile(gameDir + "/" + d.manifest.getStr("rules", "rules.json"));
    d.map = Json::parseFile(gameDir + "/" + mapFile);
    auto read = [&](const std::string& p, std::string& out) {
        std::ifstream f(gameDir + "/" + p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    };
    std::string err;
    if (!game::applyTilesToMap(d.map, read, &err)) throw std::runtime_error(mapFile + ": " + err);
    if (!game::embedScripts(d.rules, d.map, read, &err)) throw std::runtime_error(err);
    return d;
}

std::unique_ptr<Simulation> makeSimulation(const std::string& module, const SimConfig& cfg) {
    if (module == "rts") return std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
    return nullptr;
}

net::Arbiter makeArbiter(std::map<std::string, std::string> gameDirs) {
    return [dirs = std::move(gameDirs)](const net::ArbiterJob& job) {
        net::ArbiterResult res;
        res.tick = job.endTick;
        auto it = dirs.find(job.gameId);
        if (it == dirs.end()) { res.error = "no data for game " + job.gameId; return res; }
        try {
            const std::string& dir = it->second;
            MatchData data = loadMatchData(dir, job.mapFile);
            const std::string rulesPath = dir + "/" + data.manifest.getStr("rules", "rules.json");
            if (hashFileContents({rulesPath, dir + "/" + job.mapFile}) != job.dataHash) { res.error = "game data differs from the players'"; return res; }
            SimConfig cfg;
            cfg.seed = job.seed; cfg.tickRate = job.tickRate; cfg.inputDelayTicks = job.inputDelay;
            auto sim = makeSimulation(data.module, cfg);
            if (!sim) { res.error = "unknown module " + data.module; return res; }
            sim->setup(data.rules, data.map);
            for (auto& c : job.commands) sim->inject(c);
            size_t ci = 0;
            while (sim->tick() < job.endTick) {
                sim->step();
                while (ci < job.checkpoints.size() && job.checkpoints[ci].first < sim->tick()) ++ci;
                if (ci < job.checkpoints.size() && job.checkpoints[ci].first == sim->tick()) {
                    if (!res.mismatchTick && sim->checksum() != job.checkpoints[ci].second) res.mismatchTick = sim->tick();
                    ++ci;
                }
                if (int r = sim->result()) { res.result = r; break; }   // the same tick clients would report
            }
            res.tick = sim->tick();
            res.hash = sim->checksum();
            res.ok = true;
        } catch (const std::exception& e) {
            res.error = e.what();
        }
        return res;
    };
}

} // namespace rts::netgame
