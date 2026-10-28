// Headless runner: bot-vs-bot matches, replay recording and verification for any game project.
//   rts_headless --project games/skirmish [--map maps/valley.json] --seed 7 --record m.rpl
//   rts_headless --replay m.rpl
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/sim/Replay.h"
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"
#include "modules/rts/RtsTiles.h"

using namespace rts;
namespace fs = std::filesystem;

static std::unique_ptr<GameModule> makeModule(const std::string& name) {
    if (name == "rts") return std::make_unique<game::RtsModule>();
    return nullptr;
}

// Map JSON with tile-map terrain applied (2D games: the tileset decides cell kinds/levels/fields).
static Json loadMap(const std::string& mapPath, const std::string& projectDir) {
    Json map = Json::parseFile(mapPath);
    std::string err;
    auto read = [&](const std::string& p, std::string& out) {
        std::ifstream f(projectDir + "/" + p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    };
    if (!game::applyTilesToMap(map, read, &err)) throw std::runtime_error(err);
    return map;
}

struct Args {
    std::string project = "games/skirmish", map, replay, record;
    uint64_t seed = 1;
    uint32_t maxSeconds = 1800;
    bool quiet = false;
};

static void status(Simulation& sim) {
    auto* m = dynamic_cast<game::RtsModule*>(&sim.module());
    auto& gs = m->state(sim);
    std::printf("  t=%4us", sim.tick() / sim.tickRate());
    for (auto& p : sim.players()) {
        int units = 0, structs = 0;
        sim.world().each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) {
            if (o.player != p.slot) return;
            if (m->rules().types[u.type].structure) ++structs; else ++units;
        });
        int garrison = 0;
        sim.world().each<game::Garrison, Owner>([&](Entity, game::Garrison& g, Owner& o) { if (o.player == p.slot) garrison += g.count; });
        std::printf(" | %-5s u=%3d s=%2d", p.name.c_str(), units, structs);
        if (garrison) std::printf(" g=%3d", garrison);
        for (size_t r = 0; r < m->rules().resources.size(); ++r)
            std::printf(" %s=%lld", m->rules().resources[r].id.c_str(), (long long)gs.players[p.slot].stock[r].toInt());
        if (!p.alive) std::printf(" (out)");
    }
    std::printf("\n");
}

static int play(const Args& a) {
    Json manifest = Json::parseFile(a.project + "/game.json");
    std::string rulesPath = a.project + "/" + manifest.getStr("rules", "rules.json");
    std::string mapPath = a.project + "/" + (a.map.empty() ? manifest["maps"][size_t(0)].getStr("file") : a.map);
    std::string module = manifest.getStr("module", "rts");

    SimConfig cfg;
    cfg.seed = a.seed;
    Simulation sim(cfg, makeModule(module));
    sim.setup(Json::parseFile(rulesPath), loadMap(mapPath, a.project));

    std::vector<game::RtsAI> bots;
    for (auto& p : sim.players()) bots.emplace_back(p.slot, a.seed);

    Replay rep;
    rep.module = module; rep.rulesPath = rulesPath; rep.mapPath = mapPath;
    rep.dataHash = hashFileContents({rulesPath, mapPath});
    rep.seed = a.seed; rep.tickRate = cfg.tickRate; rep.inputDelay = cfg.inputDelayTicks;

    std::map<uint16_t, int> ev;
    const uint32_t maxTicks = a.maxSeconds * sim.tickRate();
    auto t0 = std::chrono::steady_clock::now();
    if (!a.quiet) std::printf("Match: %s  seed=%llu\n", mapPath.c_str(), (unsigned long long)a.seed);
    while (sim.result() == 0 && sim.tick() < maxTicks) {
        for (auto& b : bots) b.update(sim);
        sim.step();
        for (auto& e : sim.events()) ev[e.type]++;
        if (sim.tick() % rep.checksumInterval == 0) rep.checksums.emplace_back(sim.tick(), sim.checksum());
        if (!a.quiet && sim.tick() % (sim.tickRate() * 60) == 0) status(sim);
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    rep.endTick = sim.tick();
    rep.finalChecksum = sim.checksum();
    rep.commands = sim.executedCommands();
    if (!a.quiet) status(sim);
    int res = sim.result();
    std::string winner = res == 0 ? "none (time limit)" : res < 0 ? "draw" : "?";
    if (res > 0) for (auto& p : sim.players()) if (p.team == res && p.alive) { winner = p.name; break; }
    std::printf("Result: winner=%s  %.0f game-s  commands=%zu  sim=%.0f ms (%.0fx realtime)\n", winner.c_str(),
                double(sim.tick()) / sim.tickRate(), rep.commands.size(), ms, double(sim.tick()) / sim.tickRate() * 1000.0 / (ms > 0 ? ms : 1));
    std::printf("Events: spawned=%d died=%d attacks=%d trained=%d built=%d gathered=%d captured=%d\n",
                ev[game::kEvSpawned], ev[game::kEvDied], ev[game::kEvAttack], ev[game::kEvTrainDone], ev[game::kEvBuildDone],
                ev[game::kEvGathered], ev[game::kEvCaptured]);
    std::printf("Checksum: %016llx\n", (unsigned long long)rep.finalChecksum);
    if (!a.record.empty()) {
        if (!rep.save(a.record)) return 2;
        std::printf("Replay saved: %s\n", a.record.c_str());
    }
    return 0;
}

static int replay(const Args& a) {
    Replay rep;
    std::string err;
    if (!rep.load(a.replay, &err)) { std::fprintf(stderr, "replay: %s\n", err.c_str()); return 2; }
    if (hashFileContents({rep.rulesPath, rep.mapPath}) != rep.dataHash) { std::fprintf(stderr, "replay: data files changed\n"); return 2; }
    SimConfig cfg;
    cfg.seed = rep.seed; cfg.tickRate = rep.tickRate; cfg.inputDelayTicks = rep.inputDelay;
    Simulation sim(cfg, makeModule(rep.module));
    sim.setup(Json::parseFile(rep.rulesPath), loadMap(rep.mapPath, fs::path(rep.rulesPath).parent_path().string()));
    for (auto& c : rep.commands) sim.submit(c);
    size_t ci = 0;
    while (sim.tick() < rep.endTick) {
        sim.step();
        if (ci < rep.checksums.size() && rep.checksums[ci].first == sim.tick()) {
            if (sim.checksum() != rep.checksums[ci].second) { std::printf("DESYNC at tick %u\n", sim.tick()); return 1; }
            ++ci;
        }
    }
    bool ok = sim.checksum() == rep.finalChecksum;
    std::printf("Replay %s: %u ticks, %zu commands, %zu checkpoints\n", ok ? "OK" : "MISMATCH", sim.tick(), rep.commands.size(), ci);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) std::exit(2); return argv[++i]; };
        if (k == "--project") a.project = next();
        else if (k == "--map") a.map = next();
        else if (k == "--seed") a.seed = std::stoull(next());
        else if (k == "--max-seconds") a.maxSeconds = uint32_t(std::stoul(next()));
        else if (k == "--record") a.record = next();
        else if (k == "--replay") a.replay = next();
        else if (k == "--quiet") a.quiet = true;
        else { std::printf("usage: rts_headless --project DIR [--map FILE] [--seed N] [--record F] | --replay F\n"); return 2; }
    }
    try { return a.replay.empty() ? play(a) : replay(a); }
    catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 2; }
}
