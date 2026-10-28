// Headless runner: bot-vs-bot matches, replay recording and verification for any game project.
//   rts_headless --project templates/skirmish [--map maps/valley.json] --seed 7 --record m.rpl
//   rts_headless --replay m.rpl
#include <chrono>
#include <cmath>
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
#include "modules/rts/gen/MapGen.h"

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
    std::string project = "templates/skirmish", map, replay, record;
    std::string generate, symmetry = "auto", genOut, genParams;   // --generate PRESET: a generated map (template = the map)
    int players = 0;
    uint64_t seed = 1;
    uint32_t maxSeconds = 1800;
    std::string saveFile, loadFile;   // save states (Simulation::serialize)
    uint32_t saveAt = 0;              // seconds
    int benchPaths = 0;   // >0: time that many path queries (HPA* vs plain A*) and exit
    int benchBody = -1;
    int benchGroup = 0;   // >0: spawn that many units of player 1, order them to one far goal, time movement (--bench-flow on|off)
    int benchFlow = -1;   // -1: rules default, 0: off, 1: on (minGroup 1)   // planets: body index for --bench-paths (default: first surface body)
    bool quiet = false;
    bool dump = false;    // print every player's objects by type at the end (data debugging)
};

static void dumpTypes(Simulation& sim) {
    auto* m = dynamic_cast<game::RtsModule*>(&sim.module());
    for (auto& p : sim.players()) {
        std::map<std::string, int> n;
        sim.world().each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) { if (o.player == p.slot) n[m->rules().types[u.type].id]++; });
        std::printf("  %s:", p.name.c_str());
        for (auto& [k, v] : n) std::printf(" %s=%d", k.c_str(), v);
        std::printf("\n    income:");
        for (size_t r = 0; r < m->rules().resources.size(); ++r)
            std::printf(" %s=%.2f/s", m->rules().resources[r].id.c_str(), m->state(sim).players[p.slot].income[r].toDouble());
        std::printf("  supply %d/%d", int(m->state(sim).players[p.slot].supplyUsed.toInt()), int(m->state(sim).players[p.slot].supplyCap.toInt()));
        if (!m->state(sim).grid.foundation.empty()) {
            int paved = 0;
            for (auto f : m->state(sim).grid.foundation) if (f == p.slot) ++paved;
            std::printf("  concrete %d", paved);
        }
        std::printf("\n");
        sim.world().each<game::Unit, Owner, game::Footprint>([&](Entity, game::Unit& u, Owner& o, game::Footprint& f) {
            if (o.player == p.slot) std::printf("    %s [%d,%d %dx%d]\n", m->rules().types[u.type].id.c_str(), f.x, f.y, f.w, f.h);
        });
        sim.world().each<game::Unit, Owner, Transform, game::Orders>([&](Entity, game::Unit& u, Owner& o, Transform& t, game::Orders& ord) {
            if (o.player != p.slot || !m->rules().types[u.type].worker) return; // workers
            std::printf("    %s at (%.1f, %.1f) order=%d state=%d\n", m->rules().types[u.type].id.c_str(), t.pos.x.toDouble(), t.pos.z.toDouble(),
                        ord.queue.empty() ? -1 : int(ord.queue.front().type), ord.queue.empty() ? -1 : int(ord.queue.front().state));
        });
    }
}

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
    if (g.surf) {   // planets: random cell pairs on the same body (the first body with a surface unless --bench-body)
        int body = a.benchBody;
        if (body < 0) for (size_t b = 0; b < g.surf->bodies.size() && body < 0; ++b) if (g.surf->bodies[b].hasSurface()) body = int(b);
        const game::BodyDef& bd = g.surf->bodies[size_t(body)];
        const int N = bd.faceSize;
        for (int tries = 0; int(q.size()) < a.benchPaths && tries < a.benchPaths * 1000; ++tries) {
            const int c0 = g.surf->gridCell(body, int(rng.below(6)), int(rng.below(uint32_t(N))), int(rng.below(uint32_t(N))));
            const int c1 = g.surf->gridCell(body, int(rng.below(6)), int(rng.below(uint32_t(N))), int(rng.below(uint32_t(N))));
            if (g.passable(c0 % g.w, c0 / g.w, layer) && g.passable(c1 % g.w, c1 / g.w, layer)) q.push_back({g.centerOf(c0), g.centerOf(c1)});
        }
        std::printf("Body %s: faceSize %d, %d cells\n", bd.id.c_str(), N, 6 * N * N);
    }
    while (!g.surf && int(q.size()) < a.benchPaths) {
        int x0 = int(rng.below(uint32_t(g.w))), y0 = int(rng.below(uint32_t(g.h))), x1 = int(rng.below(uint32_t(g.w))), y1 = int(rng.below(uint32_t(g.h)));
        if (g.passable(x0, y0, layer) && g.passable(x1, y1, layer)) q.push_back({g.cellCenter(x0, y0), g.cellCenter(x1, y1)});
    }
    auto t0 = std::chrono::steady_clock::now();
    g.updateClearance();
    double clearMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto runAll = [&](const char* name, int minCells, int maxNodes = 40000) {
        g.hpaMinCells = minCells;
        g.sphereHpaMinCells = minCells;
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
            for (auto& wp : path) { length += (g.surf ? (wp - p).length() : (wp - p).xz().length()).toDouble(); p = wp; }
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


// Group movement benchmark: N units of the first mobile combat type of player 1 spread over a block, one move order to
// the far corner (flat) / the antipode (planet). Reports ticks/s, path ms per tick, arrivals and the field cache size.
static int benchGroup(Simulation& sim, const Args& a) {
    auto* m = dynamic_cast<game::RtsModule*>(&sim.module());
    auto& gs = m->state(sim);
    game::MapGrid& g = gs.grid;
    int type = -1;
    for (size_t t = 0; t < m->rules().types.size() && type < 0; ++t) {
        const auto& ut = m->rules().types[t];
        if (!ut.structure && ut.speed.raw > 0 && !(ut.layer & game::kLayerAir) && !ut.weapons.empty() && ut.radius <= Fixed::fromRatio(1, 2) && !ut.worker) type = int(t);
    }
    if (type < 0) { std::printf("no unit type\n"); return 1; }
    const uint8_t layer = m->rules().types[size_t(type)].pathLayer;
    // start block and goal: flat = corners, planet = face 4 centre -> face 5 centre
    std::vector<int> cells;
    int goal = -1;
    const int side = int(std::ceil(std::sqrt(double(a.benchGroup)) * 1.6));
    if (g.surf) {
        const int body = gs.surface->startBody, N = g.surf->bodies[size_t(body)].faceSize;
        for (int y = 0; y < N && int(cells.size()) < a.benchGroup * 3; ++y)
            for (int x = 0; x < N; ++x) {
                const int c = g.surf->gridCell(body, 4, (N - std::min(side, N)) / 2 + x % std::min(side, N), (N - std::min(side, N)) / 2 + y);
                if (x < std::min(side, N) && g.passable(c % g.w, c / g.w, layer)) cells.push_back(c);
            }
        for (int r = 0; r < N / 2 && goal < 0; ++r) { const int c = g.surf->gridCell(body, 5, N / 2 + r, N / 2); if (g.passable(c % g.w, c / g.w, layer)) goal = c; }
    } else {
        for (int y = 4; y < g.h && int(cells.size()) < a.benchGroup * 3; ++y)
            for (int x = 4; x < 4 + side && x < g.w; ++x) if (g.passable(x, y, layer)) cells.push_back(g.index(x, y));
        // goal well inside the map (a crowd needs room around it), the start block in the opposite corner
        for (int r = g.w / 8; r < g.w / 2 && goal < 0; ++r) if (g.passable(g.w - r, g.h - r, layer)) goal = g.index(g.w - r, g.h - r);
    }
    if (goal < 0 || int(cells.size()) < a.benchGroup) { std::printf("no room for %d units\n", a.benchGroup); return 1; }
    Command c;
    c.player = 1; c.type = game::kCmdMove;
    for (int i = 0; i < a.benchGroup; ++i) {
        const int cell = cells[size_t(i) * cells.size() / size_t(a.benchGroup)];
        c.units.push_back(m->spawn(sim, uint16_t(type), 1, g.centerOf(cell)));
    }
    c.point = g.centerOf(goal);
    const Vec3 goalPos = c.point;
    // arrived = inside the area a crowd of this size occupies (~0.8 x 0.8 units per body) plus a margin
    const Fixed arriveR = Fixed::fromRatio(int64_t(std::sqrt(double(a.benchGroup)) * 60) + 600, 100);
    sim.submit(c);
    auto clock = [] { return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
    g.clockNs = clock;
    const uint32_t ticks = a.maxSeconds * sim.tickRate();
    const auto t0 = std::chrono::steady_clock::now();
    size_t peakFields = 0, peakBytes = 0;
    uint32_t done = 0;
    int64_t peakPathNs = 0, movementNs = 0;
    for (uint32_t t = 0; t < ticks; ++t) {
        const int64_t before = g.pathNs, s0 = clock();
        sim.step();
        movementNs += clock() - s0;
        peakPathNs = std::max(peakPathNs, g.pathNs - before);
        peakFields = std::max(peakFields, g.flowCount()); peakBytes = std::max(peakBytes, g.flowBytes());
        if (t % 20 == 0 && a.quiet == false && t % 400 == 0) {
            Fixed sum{};
            int alive = 0;
            for (Entity e : c.units) if (sim.world().alive(e)) { sum += (sim.world().get<Transform>(e).pos - goalPos).length(); ++alive; }
            std::printf("  t=%3us mean distance to goal %.1f\n", t / sim.tickRate(), alive ? (sum / alive).toDouble() : 0.0);
        }
        if (t % 20 == 0) {
            int near = 0;
            for (Entity e : c.units) if (sim.world().alive(e) && (sim.world().get<Transform>(e).pos - goalPos).length() < arriveR) ++near;
            if (near >= a.benchGroup * 9 / 10 && !done) done = t;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    int near = 0;
    for (Entity e : c.units) if (sim.world().alive(e) && (sim.world().get<Transform>(e).pos - goalPos).length() < arriveR) ++near;
    if (std::getenv("RTS_BENCH_HIST")) {   // where the stragglers are: distance histogram, moving / stopped
        int hist[12][2] = {};
        for (Entity e : c.units) {
            if (!sim.world().alive(e)) continue;
            const int b = std::min(11, int((sim.world().get<Transform>(e).pos - goalPos).length().toInt() / 10));
            hist[b][sim.world().get<game::Mobile>(e).moving ? 1 : 0]++;
        }
        for (int b = 0; b < 12; ++b) std::printf("  dist %3d-%3d: stopped %d moving %d\n", b * 10, b * 10 + 10, hist[b][0], hist[b][1]);
    }
    (void)movementNs;
    std::printf("Group %d x %s on %dx%d%s: %u ticks %.0f ms = %.0f ticks/s | path total %.0f ms, %.3f ms/tick, worst tick %.1f ms | arrived %d/%d (90%% at %.0f s) | fields peak %zu, %.2f MB | %016llx\n",
                a.benchGroup, m->rules().types[size_t(type)].id.c_str(), g.w, g.h, g.surf ? " planet" : "", ticks, ms, ticks / (ms / 1000.0),
                double(g.pathNs) / 1e6, double(g.pathNs) / 1e6 / ticks, double(peakPathNs) / 1e6, near, a.benchGroup, done ? double(done) / sim.tickRate() : -1.0,
                peakFields, double(peakBytes) / 1048576.0, (unsigned long long)sim.checksum());
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
    if (!a.generate.empty()) {
        // the map with a "generate" block is written next to the replay (or the scratch output) so that
        // --replay regenerates exactly the same terrain from the recorded seed
        Json m = Json::parseFile(mapPath);
        Json g = Json::object();
        g.set("preset", Json::string(a.generate));
        g.set("seed", Json::string("match"));
        g.set("players", Json::number(int64_t(a.players > 0 ? a.players : std::max<int>(2, int(m["players"].size())))));
        g.set("symmetry", Json::string(a.symmetry));
        if (!a.genParams.empty()) g.set("params", Json::parse(a.genParams));
        g.set("template", Json::string(fs::relative(mapPath, a.project).generic_string()));
        m.set("generate", g);
        mapPath = (a.record.empty() ? (fs::temp_directory_path() / ("rts_gen_" + a.generate + "_" + std::to_string(a.seed) + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json")).string() : a.record + ".map.json");
        std::ofstream(mapPath) << m.dump();
    }
    Json mapJson = loadMap(mapPath, a.project);
    const auto setupT0 = std::chrono::steady_clock::now();
    Json rulesJson = Json::parseFile(rulesPath);
    if (a.benchFlow >= 0) {   // benchmark switch: shared flow fields for every group order (on) or none (off)
        Json ff = Json::object();
        ff.set("minGroup", Json::number(int64_t(a.benchFlow ? 2 : 0)));
        rulesJson.member("movement").set("flowField", ff);
    }
    sim.setup(rulesJson, mapJson);
    if (!a.quiet && (a.benchPaths > 0 || mapJson["world"].isObject()))   // planets / benchmarks: setup cost incl. the HPA* graphs
        std::printf("Setup: %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - setupT0).count());
    if (auto* rm = dynamic_cast<game::RtsModule*>(&sim.module()); rm && rm->generatedMap()) {
        const Json& gm = *rm->generatedMap();
        if (!a.quiet) {
            const Json& gi = gm["generated"];
            std::string size = std::to_string(gm["size"][size_t(0)].integer()) + "x" + std::to_string(gm["size"][size_t(1)].integer());
            if (gm["world"].isObject()) {   // planet maps: bodies and face sizes
                size = gi.getStr("layout");
                for (auto& b : gm["world"]["bodies"].items())
                    if (b.has("faceSize")) size += " " + b.getStr("id") + ":" + std::to_string(b.getInt("faceSize"));
            }
            std::printf("Generated: preset=%s seed=%lld players=%lld symmetry=%s size=%s objects=%zu hash=%016llx\n", gi.getStr("preset").c_str(),
                        (long long)gi.getInt("seed"), (long long)gi.getInt("players"), gi.getStr("symmetry").c_str(), size.c_str(),
                        gm["entities"].size(), (unsigned long long)gen::mapHash(gm));
            if (gi.has("paths")) std::printf("  paths %s  connected=%d\n", gi["paths"].dump(-1).c_str(), int(gi.getBool("connected")));
            for (auto& w : gi["warnings"].items()) std::printf("  warning: %s\n", w.str().c_str());
        }
        if (!a.genOut.empty()) { std::ofstream(a.genOut) << gm.dump(); if (!a.quiet) std::printf("Generated map written: %s\n", a.genOut.c_str()); }
    }
    if (a.maxSeconds == 0) return 0;   // --max-seconds 0: set up (and generate) only

    if (a.benchPaths > 0) return benchPaths(sim, a);
    if (a.benchGroup > 0) return benchGroup(sim, a);
    std::vector<game::RtsAI> bots;
    for (auto& p : sim.players()) bots.emplace_back(p.slot, a.seed);

    Replay rep;
    rep.module = module; rep.rulesPath = rulesPath; rep.mapPath = mapPath;
    rep.dataHash = hashFileContents({rulesPath, mapPath});
    rep.seed = a.seed; rep.tickRate = cfg.tickRate; rep.inputDelay = cfg.inputDelayTicks;

    if (!a.loadFile.empty()) {
        std::ifstream f(a.loadFile, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)), {}), err;
        if (!sim.deserialize(bytes, &err)) { std::printf("Load failed: %s\n", err.c_str()); return 2; }
        if (!a.quiet) std::printf("Loaded %s: t=%us\n", a.loadFile.c_str(), sim.tick() / sim.tickRate());
    }
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
        if (a.saveAt && sim.tick() == a.saveAt * sim.tickRate() && !a.saveFile.empty()) {
            std::string bytes = sim.serialize();
            std::ofstream(a.saveFile, std::ios::binary) << bytes;
            std::printf("Saved %s: t=%us, %zu bytes, checksum %016llx\n", a.saveFile.c_str(), a.saveAt, bytes.size(), (unsigned long long)sim.checksum());
        }
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
    if (a.dump) dumpTypes(sim);
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
        else if (k == "--save") a.saveFile = next();
        else if (k == "--save-at") a.saveAt = uint32_t(std::stoul(next()));
        else if (k == "--load") a.loadFile = next();
        else if (k == "--replay") a.replay = next();
        else if (k == "--quiet") a.quiet = true;
        else if (k == "--dump") a.dump = true;
        else if (k == "--bench-paths") a.benchPaths = std::stoi(next());
        else if (k == "--bench-body") a.benchBody = std::stoi(next());
        else if (k == "--bench-group") a.benchGroup = std::stoi(next());
        else if (k == "--bench-flow") a.benchFlow = next() == "on" ? 1 : 0;
        else if (k == "--planet-hpa") setenv("RTS_PLANET_HPA", next() == "off" ? "0" : "1", 1);   // force planet HPA* on / off
        else if (k == "--generate") a.generate = next();
        else if (k == "--players") a.players = std::stoi(next());
        else if (k == "--symmetry") a.symmetry = next();
        else if (k == "--gen-params") a.genParams = next();
        else if (k == "--gen-out") a.genOut = next();
        else { std::printf("usage: rts_headless --project DIR [--map FILE] [--seed N] [--record F] [--save F --save-at S] [--load F] [--generate PRESET [--players N] [--symmetry S] [--gen-params JSON] [--gen-out F]] | --replay F\n"); return 2; }
    }
    try { return a.replay.empty() ? play(a) : replay(a); }
    catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 2; }
}
