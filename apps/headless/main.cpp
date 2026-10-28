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

#include "engine/core/Random.h"
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
    int benchPaths = 0;   // >0: time that many path queries (HPA* vs plain A*) and exit
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

// Path query benchmark: random start/goal pairs on passable ground cells (fixed seed), the same queries with
// the map's pathing mode (HPA* on big maps) and with plain A* (any length, node limit as in the game).
static int benchPaths(Simulation& sim, const Args& a) {
    auto* m = dynamic_cast<game::RtsModule*>(&sim.module());
    game::MapGrid& g = m->state(sim).grid;
    const uint8_t layer = game::kLayerGround;
    Random rng(a.seed);
    std::vector<std::pair<Vec3, Vec3>> q;
    while (int(q.size()) < a.benchPaths) {
        int x0 = int(rng.below(uint32_t(g.w))), y0 = int(rng.below(uint32_t(g.h))), x1 = int(rng.below(uint32_t(g.w))), y1 = int(rng.below(uint32_t(g.h)));
        if (g.passable(x0, y0, layer) && g.passable(x1, y1, layer)) q.push_back({g.cellCenter(x0, y0), g.cellCenter(x1, y1)});
    }
    auto t0 = std::chrono::steady_clock::now();
    g.updateClearance();
    double clearMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto runAll = [&](const char* name, int minCells, int maxNodes = 40000) {
        g.hpaMinCells = minCells;
        std::vector<Vec3> path;
        int found = 0;
        double total = 0, worst = 0;
        size_t points = 0;
        double length = 0;
        for (auto& [s, e] : q) {
            auto t = std::chrono::steady_clock::now();
            if (g.findPath(s, e, layer, path, maxNodes, 1)) ++found;
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
            total += ms; worst = std::max(worst, ms); points += path.size();
            Vec3 p = s;
            for (auto& wp : path) { length += (wp - p).xz().length().toDouble(); p = wp; }
        }
        std::printf("%-6s %d queries: found=%d avg=%.2f ms max=%.2f ms waypoints=%zu length=%.0f\n", name, int(q.size()), found, total / double(q.size()), worst, points, length);
    };
    std::printf("Map %dx%d, heightfield=%d, clearance pass %.1f ms\n", g.w, g.h, int(g.hf), clearMs);
    auto t1 = std::chrono::steady_clock::now();
    runAll("hpa", 0);  // first query builds the abstract graph
    std::printf("       (graph build + queries %.0f ms, hpa=%d fallback=%d)\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count(), g.hpaStats[0], g.hpaStats[1]);
    runAll("hpa", 0);
    runAll("astar", 1 << 30);
    runAll("a*full", 1 << 30, g.w * g.h);   // reference: unbounded A* (which goals are reachable at all)
    return 0;
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

    if (a.benchPaths > 0) return benchPaths(sim, a);
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
        else if (k == "--bench-paths") a.benchPaths = std::stoi(next());
        else { std::printf("usage: rts_headless --project DIR [--map FILE] [--seed N] [--record F] | --replay F\n"); return 2; }
    }
    try { return a.replay.empty() ? play(a) : replay(a); }
    catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 2; }
}
