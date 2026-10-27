// Tests for networking: rollback snapshots, protocol, transports, lockstep/rollback sessions,
// server rooms and matchmaking. Run: ./rts_net_tests (from the repo root or with RTS_SOURCE_DIR).
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/sim/Simulation.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"

using namespace rts;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
static std::vector<std::pair<const char*, std::function<void()>>>& registry() { static std::vector<std::pair<const char*, std::function<void()>>> r; return r; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

#ifndef RTS_SOURCE_DIR
#define RTS_SOURCE_DIR "."
#endif
static const std::string kSkirmish = std::string(RTS_SOURCE_DIR) + "/games/skirmish";
static const std::string kMushrooms = std::string(RTS_SOURCE_DIR) + "/games/mushrooms";

static std::unique_ptr<Simulation> makeSim(const std::string& game, const std::string& map, uint64_t seed) {
    SimConfig cfg; cfg.seed = seed;
    auto sim = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
    sim->setup(Json::parseFile(game + "/rules.json"), Json::parseFile(game + "/" + map));
    return sim;
}

// ---------------- snapshots ----------------
static void snapshotRoundTrip(const std::string& game, const std::string& map) {
    auto sim = makeSim(game, map, 5);
    std::vector<game::RtsAI> bots;
    for (auto& p : sim->players()) bots.emplace_back(p.slot, 5);
    auto botsStep = [&](uint32_t n) { for (uint32_t i = 0; i < n && sim->result() == 0; ++i) { for (auto& b : bots) b.update(*sim); sim->step(); } };
    botsStep(600);
    SimSnapshot snap;
    sim->saveState(snap);
    const uint64_t atSnap = sim->checksum();
    const size_t execBefore = sim->executedCommands().size();
    botsStep(300);
    const uint64_t reference = sim->checksum();
    const uint32_t endTick = sim->tick();
    std::vector<Command> later(sim->executedCommands().begin() + long(execBefore), sim->executedCommands().end());

    for (int round = 0; round < 2; ++round) {   // twice: the second restore reuses pool memory
        sim->loadState(snap);
        CHECK(sim->checksum() == atSnap);
        CHECK(sim->executedCommands().size() == execBefore);
        for (auto& c : later) {
            bool queued = false;   // commands submitted before the snapshot are still pending in it
            auto it = snap.pending.find(c.tick);
            if (it != snap.pending.end()) for (auto& p : it->second) queued |= p.player == c.player && p.seq == c.seq;
            if (!queued) sim->inject(c);
        }
        while (sim->tick() < endTick) sim->step();
        CHECK(sim->checksum() == reference);
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) sim->saveState(snap);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 50;
    std::printf("  snapshot %s: %.3f ms, %zu entities\n", map.c_str(), ms, sim->world().count<game::Unit>());
}

TEST(snapshot_restore_resimulates_identically) {
    snapshotRoundTrip(kSkirmish, "maps/valley.json");
    snapshotRoundTrip(kMushrooms, "maps/duel.json");
}

TEST(command_sink_intercepts_submit) {
    auto sim = makeSim(kSkirmish, "maps/valley.json", 1);
    std::vector<Command> got;
    sim->setCommandSink([&](Command&& c) { got.push_back(c); return sim->tick() + 7; });
    Command c; c.player = 1; c.type = 1;
    CHECK(sim->submit(c) == 7);
    CHECK(got.size() == 1 && got[0].player == 1);
    sim->step();
    CHECK(sim->executedCommands().empty());
}

int main() {
    for (auto& [name, fn] : registry()) {
        int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
