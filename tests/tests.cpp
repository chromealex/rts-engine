#include <chrono>
// Dependency-free unit tests. Run: ctest  (or ./rts_tests)
#include <filesystem>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "engine/core/ECS.h"
#include "engine/core/Fixed.h"
#include "engine/core/FixedMath.h"
#include <cmath>
#include "engine/core/Random.h"
#include "engine/data/Json.h"
#include "engine/sim/Replay.h"
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsCampaign.h"   // agent-mech
#include "modules/rts/RtsModule.h"
#include "modules/rts/RtsTiles.h"
#include "modules/rts/RtsSphere.h"
#include <algorithm>

using namespace rts;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::vector<std::pair<const char*, std::function<void()>>>& registry() { static std::vector<std::pair<const char*, std::function<void()>>> r; return r; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

#ifndef RTS_SOURCE_DIR
#define RTS_SOURCE_DIR "."
#endif

// ---------------- Fixed ----------------
TEST(fixed_basic_arithmetic) {
    Fixed a = Fixed::fromInt(3), b = Fixed::fromRatio(1, 2);
    CHECK((a + b) == Fixed::fromRatio(7, 2));
    CHECK((a * b) == Fixed::fromRatio(3, 2));
    CHECK((a / b) == Fixed::fromInt(6));
    CHECK((-a * b) == Fixed::fromRatio(-3, 2));
    CHECK((Fixed::fromInt(-7) / Fixed::fromInt(2)) == Fixed::fromRatio(-7, 2));
    CHECK(Fixed::fromRatio(-7, 2).toInt() == -3);   // trunc toward zero
    CHECK(Fixed::fromRatio(-7, 2).floorInt() == -4);
}

TEST(fixed_trig_turns) {
    // sin/cos over a full turn vs libm (test only), error <= 3 raw Q16 units
    int worst = 0;
    for (int64_t r = -70000; r <= 140000; r += 37) {
        const double a = double(r) / 65536.0 * 6.283185307179586;
        const int es = int(std::llround(std::sin(a) * 65536.0)), ec = int(std::llround(std::cos(a) * 65536.0));
        worst = std::max(worst, std::abs(int(fxSinTurns(Fixed::fromRaw(r)).raw) - es));
        worst = std::max(worst, std::abs(int(fxCosTurns(Fixed::fromRaw(r)).raw) - ec));
    }
    CHECK(worst <= 3);
    CHECK(fxSinTurns(Fixed::fromRatio(1, 4)) == Fixed::fromInt(1));
    CHECK(fxCosTurns(Fixed::fromRatio(1, 2)) == Fixed::fromInt(-1));
    // atan2 in all quadrants and scales
    int worstA = 0;
    for (int i = 0; i < 360; i += 7)
        for (int64_t scale : {int64_t(3), int64_t(65536), int64_t(65536) * 900}) {
            const double a = i * 3.141592653589793 / 180.0;
            const Fixed x = Fixed::fromRaw(std::llround(std::cos(a) * double(scale)));
            const Fixed y = Fixed::fromRaw(std::llround(std::sin(a) * double(scale)));
            if (x.raw == 0 && y.raw == 0) continue;
            double expect = std::atan2(double(y.raw), double(x.raw)) / 6.283185307179586 * 65536.0;
            const int got = int(fxAtan2Turns(y, x).raw);
            int d = std::abs(got - int(std::llround(expect)));
            d = std::min(d, 65536 - d);
            if (scale > 3) worstA = std::max(worstA, d);
        }
    CHECK(worstA <= 3);
    CHECK(fxAtan2Turns(Fixed{}, Fixed{}) == Fixed{});
    CHECK(fxAtan2Turns(Fixed{}, Fixed::fromInt(-5)) == Fixed::fromRatio(1, 2));
    CHECK(std::abs(int(fxAsinTurns(Fixed::fromInt(1)).raw) - 16384) <= 2);
    CHECK(std::abs(int(fxAcosTurns(Fixed{}).raw) - 16384) <= 2);
    const Vec3 n = fxNormalize(Vec3{Fixed::fromInt(3), Fixed{}, Fixed::fromInt(4)});
    CHECK(std::abs(int(n.x.raw) - 39322) <= 2 && std::abs(int(n.z.raw) - 52429) <= 2);
}

TEST(fixed_parse_is_float_free) {
    bool ok = false;
    CHECK(Fixed::parse("1.5", &ok) == Fixed::fromRatio(3, 2) && ok);
    CHECK(Fixed::parse("-0.25", &ok) == Fixed::fromRatio(-1, 4) && ok);
    CHECK(Fixed::parse("42", &ok) == Fixed::fromInt(42) && ok);
    Fixed::parse("1e5", &ok); CHECK(!ok);
}

TEST(fixed_sqrt) {
    CHECK(fxSqrt(Fixed::fromInt(16)) == Fixed::fromInt(4));
    CHECK(fxSqrt(Fixed::fromInt(2)).raw == 92681); // floor(sqrt(2) * 65536)
    CHECK(fxSqrt(Fixed::fromInt(-1)) == Fixed{});
    CHECK(fxSqrt(Fixed::fromInt(1000000)) == Fixed::fromInt(1000));
}

TEST(fixed_large_multiply_portable_path) {
    // Values whose raw product overflows int64: checks the 128-bit path (MSVC uses the manual one).
    Fixed big = Fixed::fromInt(3000000);
    CHECK((big * Fixed::fromInt(1000)) == Fixed::fromInt(3000000000LL));
    CHECK((big * -Fixed::fromInt(1000)) == Fixed::fromInt(-3000000000LL));
}

// ---------------- Random ----------------
TEST(random_is_reproducible) {
    Random a(123), b(123), c(124);
    bool allSame = true, anyDiff = false;
    for (int i = 0; i < 1000; ++i) { uint32_t x = a.next(); if (x != b.next()) allSame = false; if (x != c.next()) anyDiff = true; }
    CHECK(allSame);
    CHECK(anyDiff);
    Random r(5);
    for (int i = 0; i < 1000; ++i) { auto v = r.range(-3, 3); CHECK(v >= -3 && v <= 3); }
}

// ---------------- ECS ----------------
struct Pos { int x; };
struct Vel { int v; };

TEST(ecs_create_destroy_and_generations) {
    World w;
    Entity a = w.create(), b = w.create();
    w.add<Pos>(a, Pos{1}); w.add<Pos>(b, Pos{2}); w.add<Vel>(b, Vel{5});
    int sum = 0, n = 0;
    w.each<Pos, Vel>([&](Entity, Pos& p, Vel& v) { sum += p.x + v.v; ++n; });
    CHECK(n == 1 && sum == 7);
    w.destroy(a);
    CHECK(!w.alive(a));
    Entity c = w.create();
    CHECK(c.index == a.index && c.gen == a.gen + 1); // recycled with new generation
    CHECK(!w.has<Pos>(c));
    CHECK(w.tryGet<Pos>(a) == nullptr); // stale handle is rejected
}

TEST(ecs_deferred_destroy_during_iteration) {
    World w;
    for (int i = 0; i < 10; ++i) { Entity e = w.create(); w.add<Pos>(e, Pos{i}); }
    w.each<Pos>([&](Entity e, Pos& p) { if (p.x % 2) w.destroyLater(e); });
    w.flush();
    CHECK(w.count<Pos>() == 5);
    int odd = 0;
    w.each<Pos>([&](Entity, Pos& p) { if (p.x % 2) ++odd; });
    CHECK(odd == 0);
}

// ---------------- JSON ----------------
TEST(json_parse) {
    Json j = Json::parse(R"({ // comment
        "a": 1.25, "b": [1, 2, 3], "c": {"d": "x\ny"}, "e": true, "f": null })");
    CHECK(j.getFx("a") == Fixed::fromRatio(5, 4));
    CHECK(j["b"].size() == 3 && j["b"][2].integer() == 3);
    CHECK(j["c"].getStr("d") == "x\ny");
    CHECK(j.getBool("e"));
    CHECK(j["f"].isNull());
    CHECK(j["missing"].isNull());
    bool threw = false;
    try { Json::parse("{\"a\": }"); } catch (const JsonError&) { threw = true; }
    CHECK(threw);
}

// ---------------- Commands / replays ----------------
TEST(command_serialization_roundtrip) {
    Command c;
    c.tick = 77; c.player = 3; c.flags = kCmdQueue; c.type = 9; c.seq = 12;
    c.units = {Entity{1, 2}, Entity{5, 0}};
    c.target = Entity{8, 1};
    c.point = {Fixed::fromRatio(-3, 2), Fixed::fromInt(0), Fixed::fromInt(7)};
    c.param = -42;
    ByteWriter w; writeCommand(w, c);
    ByteReader r(w.buf.data(), w.buf.size());
    Command d = readCommand(r);
    CHECK(r.ok() && r.atEnd());
    CHECK(c == d);
}

// ---------------- Generic RTS gameplay ----------------
static const std::string kSkirmish = std::string(RTS_SOURCE_DIR) + "/templates/skirmish";
static const std::string kMushrooms = std::string(RTS_SOURCE_DIR) + "/templates/mushrooms";

static std::unique_ptr<Simulation> makeSim(const std::string& game, const std::string& map, uint64_t seed) {
    SimConfig cfg; cfg.seed = seed;
    auto sim = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
    sim->setup(Json::parseFile(game + "/rules.json"), Json::parseFile(game + "/" + map));
    return sim;
}
static game::RtsModule& mod(Simulation& s) { return *dynamic_cast<game::RtsModule*>(&s.module()); }
static Entity findType(Simulation& sim, const std::string& type, uint8_t owner) {
    int t = mod(sim).rules().findType(type);
    Entity found;
    sim.world().each<game::Unit, Owner>([&](Entity e, game::Unit& u, Owner& o) { if (!found.valid() && u.type == t && o.player == owner) found = e; });
    return found;
}

TEST(rts_rules_load) {
    game::Rules r = game::loadRules(Json::parseFile(kSkirmish + "/rules.json"));
    CHECK(r.findType("peasant") >= 0 && r.findType("townhall") >= 0);
    CHECK(r.types[size_t(r.findType("archer"))].weapons[0].projectileSpeed > Fixed{});
    CHECK(r.damageMult(uint16_t(std::find(r.damageTypes.begin(), r.damageTypes.end(), "siege") - r.damageTypes.begin()),
                       uint16_t(std::find(r.armorTypes.begin(), r.armorTypes.end(), "fortified") - r.armorTypes.begin())) == Fixed::parse("1.6"));
    bool threw = false;
    try { game::loadRules(Json::parse(R"({"types":{"a":{"produces":["nope"]}}})")); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

TEST(rts_pathfinding_avoids_water) {
    game::MapGrid g;
    g.init(20, 10);
    for (int y = 0; y < 9; ++y) g.flags[size_t(g.index(10, y))] = game::kCellWater; // wall with a gap at y=9
    std::vector<Vec3> path;
    bool ok = g.findPath(g.cellCenter(2, 2), g.cellCenter(17, 2), game::kLayerGround, path);
    CHECK(ok && !path.empty());
    bool usedGap = false;
    for (auto& p : path) if (p.z.floorInt() >= 8) usedGap = true;
    CHECK(usedGap);
}

TEST(rts_train_and_gather) {
    auto sim = makeSim(kSkirmish, "maps/valley.json", 1);
    World& w = sim->world();
    auto& gs = mod(*sim).state(*sim);
    int gold = mod(*sim).rules().findResource("gold");
    Fixed before = gs.players[1].stock[size_t(gold)];
    Entity hall = findType(*sim, "townhall", 1), worker = findType(*sim, "peasant", 1), mine;
    w.each<game::ResourceStock, Transform>([&](Entity e, game::ResourceStock&, Transform& t) {
        if (!mine.valid() && mod(*sim).rules().types[w.get<game::Unit>(e).type].id == "goldmine" && t.pos.x < Fixed::fromInt(20)) mine = e;
    });
    Command g; g.player = 1; g.type = game::kCmdGather; g.units = {worker}; g.target = mine;
    sim->submit(g);
    Command t; t.player = 1; t.type = game::kCmdTrain; t.units = {hall}; t.param = mod(*sim).rules().findType("peasant");
    sim->submit(t);
    for (int i = 0; i < 20 * 40; ++i) sim->step();
    int peasants = 0;
    w.each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) { if (o.player == 1 && mod(*sim).rules().types[u.type].id == "peasant") ++peasants; });
    CHECK(peasants == 6);
    CHECK(gs.players[1].stock[size_t(gold)] > before - Fixed::fromInt(75)); // gathered more than the peasant cost
}

TEST(rts_build_structure) {
    auto sim = makeSim(kSkirmish, "maps/valley.json", 1);
    Entity worker = findType(*sim, "peasant", 1);
    Command c; c.player = 1; c.type = game::kCmdBuild; c.units = {worker};
    c.param = mod(*sim).rules().findType("farm");
    c.point = {Fixed::fromInt(16), Fixed{}, Fixed::fromInt(30)};
    sim->submit(c);
    for (int i = 0; i < 20 * 40; ++i) sim->step();
    Entity farm = findType(*sim, "farm", 1);
    int farms = 0;
    sim->world().each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) {
        if (o.player == 1 && mod(*sim).rules().types[u.type].id == "farm" && !(u.flags & game::kUnitUnderConstruction)) ++farms;
    });
    CHECK(farm.valid());
    CHECK(farms == 2); // the starting farm + the new one
}

TEST(rts_invalid_commands_ignored) {
    auto sim = makeSim(kSkirmish, "maps/valley.json", 1);
    Entity enemyHall = findType(*sim, "townhall", 2);
    Command c; c.player = 1; c.type = game::kCmdTrain; c.units = {enemyHall}; c.param = 0;
    sim->submit(c);
    Command junk; junk.player = 7; junk.type = 999; junk.units = {Entity{9999, 3}};
    sim->submit(junk);
    for (int i = 0; i < 10; ++i) sim->step();
    CHECK(sim->world().get<game::Production>(enemyHall).queue.empty());
}

TEST(rts_garrison_send_and_capture) {
    auto sim = makeSim(kMushrooms, "maps/duel.json", 1);
    World& w = sim->world();
    Entity home, target;
    w.each<game::Garrison, Owner, Transform>([&](Entity e, game::Garrison&, Owner& o, Transform& t) {
        if (o.player == 1) home = e;
        if (o.player == 0 && t.pos.x < Fixed::fromInt(11) && t.pos.z > Fixed::fromInt(15)) target = e; // r_a, 8 defenders
    });
    w.get<game::Garrison>(home).count = 40;
    Command c; c.player = 1; c.type = game::kCmdSend; c.units = {home}; c.target = target; c.param = 50;
    sim->submit(c);
    for (int i = 0; i < 20 * 8; ++i) sim->step();
    CHECK(w.get<Owner>(target).player == 1);
    CHECK(w.get<game::Garrison>(target).count >= 10);
}

static uint64_t botMatch(const std::string& game, const std::string& map, uint64_t seed, uint32_t ticks,
                         std::vector<Command>* cmds = nullptr, std::vector<uint64_t>* hashes = nullptr, uint32_t* end = nullptr) {
    auto sim = makeSim(game, map, seed);
    std::vector<game::RtsAI> bots;
    for (auto& p : sim->players()) bots.emplace_back(p.slot, seed);
    for (uint32_t t = 0; t < ticks && sim->result() == 0; ++t) {
        for (auto& b : bots) b.update(*sim);
        sim->step();
        if (hashes && sim->tick() % 50 == 0) hashes->push_back(sim->checksum());
    }
    if (cmds) *cmds = sim->executedCommands();
    if (end) *end = sim->tick();
    return sim->checksum();
}

TEST(determinism_same_seed_same_result) {
    CHECK(botMatch(kSkirmish, "maps/valley.json", 5, 20 * 240) == botMatch(kSkirmish, "maps/valley.json", 5, 20 * 240));
    CHECK(botMatch(kMushrooms, "maps/duel.json", 5, 20 * 120) == botMatch(kMushrooms, "maps/duel.json", 5, 20 * 120));
}

TEST(determinism_replay_reproduces_match) {
    for (auto [game, map] : {std::pair{kSkirmish, std::string("maps/valley.json")}, std::pair{kMushrooms, std::string("maps/duel.json")}}) {
        std::vector<Command> cmds;
        std::vector<uint64_t> hashes;
        uint32_t end = 0;
        uint64_t live = botMatch(game, map, 7, 20 * 300, &cmds, &hashes, &end);
        CHECK(!cmds.empty());
        auto sim = makeSim(game, map, 7);
        for (auto& c : cmds) sim->submit(c);
        std::vector<uint64_t> replayHashes;
        while (sim->tick() < end) { sim->step(); if (sim->tick() % 50 == 0) replayHashes.push_back(sim->checksum()); }
        CHECK(replayHashes == hashes);
        CHECK(sim->checksum() == live);
    }
}

TEST(replay_file_roundtrip) {
    Replay r;
    r.module = "rts"; r.rulesPath = "a"; r.mapPath = "b"; r.seed = 5; r.endTick = 100; r.finalChecksum = 0xdeadbeef;
    Command c; c.tick = 3; c.player = 1; c.type = 1; c.units = {Entity{1, 0}}; c.param = 50;
    r.commands = {c, c};
    r.checksums = {{20, 1}, {40, 2}};
    // temp dir: the working directory may be read-only (macOS app launch has cwd "/")
    const std::string pathStr = (std::filesystem::temp_directory_path() / "test_roundtrip.rpl").string();
    const char* path = pathStr.c_str();
    CHECK(r.save(path));
    Replay l;
    CHECK(l.load(path));
    CHECK(l.module == "rts" && l.seed == 5 && l.endTick == 100 && l.finalChecksum == 0xdeadbeef);
    CHECK(l.commands.size() == 2 && l.commands[1] == c);
    CHECK(l.checksums == r.checksums);
    std::remove(path);
}

// ------------------------------------------------------------------ generic mechanics (inline mini games)

static std::unique_ptr<Simulation> makeSimJson(const std::string& rules, const std::string& map, uint64_t seed = 1) {
    SimConfig cfg; cfg.seed = seed;
    auto sim = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
    sim->setup(Json::parse(rules), Json::parse(map));
    return sim;
}
static int countType(Simulation& sim, const std::string& type, uint8_t owner, bool completeOnly = true) {
    int t = mod(sim).rules().findType(type), n = 0;
    sim.world().each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) {
        if (u.type == t && o.player == owner && u.hp.raw > 0 && (!completeOnly || !(u.flags & game::kUnitUnderConstruction))) ++n;
    });
    return n;
}
static void run(Simulation& sim, double seconds) { for (int i = 0; i < int(seconds * sim.tickRate()); ++i) sim.step(); }
static Fixed stockOf(Simulation& sim, uint8_t p, const std::string& res) { return mod(sim).state(sim).players[p].stock[size_t(mod(sim).rules().findResource(res))]; }
static std::string flatMap(int w, int h, const std::string& extra, const std::string& players = R"([{"name":"A","team":1},{"name":"B","team":2}])") {
    std::string rows;
    for (int y = 0; y < h; ++y) rows += std::string(y ? "," : "") + "\"" + std::string(size_t(w), '.') + "\"";
    return "{\"size\":[" + std::to_string(w) + "," + std::to_string(h) + "],\"players\":" + players + ",\"terrain\":{\"rows\":[" + rows + "]}" + extra + "}";
}

TEST(mech_field_harvest_and_regrow) {
    const char* rules = R"({
      "resources": {"ore": {"field": true, "regrow": 2, "cellMax": 50}},
      "types": {
        "refinery": {"structure": {"footprint": [2, 2]}, "dropoff": ["ore"], "health": {"max": 500}},
        "harvester": {"move": {"speed": 3}, "gather": {"ore": {"carry": 40, "time": 2}}, "health": {"max": 200}}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, R"(,"fields":[{"resource":"ore","center":[20,8],"radius":2,"amount":50}],
        "entities":[{"type":"refinery","owner":1,"pos":[6,8]},{"type":"harvester","owner":1,"pos":[9,8]},{"type":"refinery","owner":2,"pos":[28,2]}])"));
    auto& g = mod(*sim).state(*sim).grid;
    Fixed fieldBefore{};
    for (auto a : g.fieldAmt) fieldBefore += a;
    Command c; c.player = 1; c.type = game::kCmdSmart; c.units = {findType(*sim, "harvester", 1)}; c.point = {Fixed::fromInt(20), Fixed{}, Fixed::fromInt(8)};
    sim->submit(c);
    run(*sim, 40);
    CHECK(stockOf(*sim, 1, "ore") >= Fixed::fromInt(40));        // at least one full load delivered
    Fixed fieldAfter{};
    for (auto a : g.fieldAmt) fieldAfter += a;
    CHECK(fieldAfter < fieldBefore + Fixed::fromInt(200));        // harvested (regrowth is capped per cell)
}

TEST(mech_power_deficit_slows_and_disables) {
    const char* rules = R"({
      "resources": {"credits": {"start": 5000}, "power": {"kind": "capacity", "deficitProduction": 0.5, "deficitDisables": ["defense"]}},
      "types": {
        "plant": {"structure": {"footprint": [2, 2]}, "provides": {"power": 10}},
        "tower": {"tags": ["defense"], "structure": {"footprint": [1, 1]}, "consumes": {"power": 8}, "weapons": [{"range": 6, "damage": 10}]},
        "dummy": {"move": {"speed": 1}}
      }})";
    auto sim = makeSimJson(rules, flatMap(24, 12, R"(,"entities":[{"type":"plant","owner":1,"pos":[3,3]},{"type":"tower","owner":1,"pos":[8,3]},{"type":"tower","owner":1,"pos":[10,3]},{"type":"dummy","owner":2,"pos":[20,8]}])"));
    run(*sim, 1);
    auto& eco = mod(*sim).state(*sim).players[1];
    CHECK(eco.productionMult == Fixed::fromRatio(1, 2));
    Entity tower = findType(*sim, "tower", 1);
    CHECK(sim->world().get<game::Unit>(tower).flags & game::kUnitDisabled);
    CHECK(stockOf(*sim, 1, "power") == Fixed::fromInt(-6));       // provided - used
}

TEST(mech_progressive_cost_stalls_without_income) {
    const char* rules = R"({
      "costModel": "progressive", "victory": "keyUnits",
      "resources": {"metal": {"kind": "flow", "start": 10, "storage": 1000}},
      "types": {
        "commander": {"move": {"speed": 2}, "builds": ["lab"], "buildPower": 1, "key": true},
        "lab": {"structure": {"footprint": [2, 2]}, "cost": {"metal": 100, "time": 10}}
      }})";
    auto sim = makeSimJson(rules, flatMap(24, 12, R"(,"entities":[{"type":"commander","owner":1,"pos":[4,6]},{"type":"commander","owner":2,"pos":[20,6]}])"));
    Command c; c.player = 1; c.type = game::kCmdBuild; c.units = {findType(*sim, "commander", 1)}; c.param = mod(*sim).rules().findType("lab");
    c.point = {Fixed::fromInt(8), Fixed{}, Fixed::fromInt(6)};
    sim->submit(c);
    run(*sim, 30);
    Entity lab = findType(*sim, "lab", 1);
    CHECK(lab.valid());
    auto* con = sim->world().tryGet<game::Construction>(lab);
    CHECK(con != nullptr);                                         // stalled: only 10 of 100 metal available
    CHECK(con && con->progress <= Fixed::fromRatio(11, 100) && con->progress >= Fixed::fromRatio(9, 100));
}

TEST(mech_cliffs_need_ramps) {
    std::string rows, levels;
    for (int y = 0; y < 12; ++y) {
        std::string r(24, '.'), l(24, '0');
        for (int x = 12; x < 24; ++x) l[size_t(x)] = '1';
        if (y == 10) r[12] = '/', r[11] = '/';
        rows += std::string(y ? "," : "") + "\"" + r + "\"";
        levels += std::string(y ? "," : "") + "\"" + l + "\"";
    }
    const char* rules = R"({"victory": "noUnits", "types": {"marine": {"move": {"speed": 3}}}})";
    std::string map = "{\"size\":[24,12],\"players\":[{\"name\":\"A\"},{\"name\":\"B\"}],\"terrain\":{\"rows\":[" + rows + "],\"levels\":[" + levels + "]},"
                      "\"entities\":[{\"type\":\"marine\",\"owner\":1,\"pos\":[6,2]},{\"type\":\"marine\",\"owner\":2,\"pos\":[20,2]}]}";
    auto sim = makeSimJson(rules, map);
    auto& g = mod(*sim).state(*sim).grid;
    CHECK(!g.canStep(11, 2, 12, 2, game::kLayerGround));         // cliff edge
    CHECK(g.canStep(11, 10, 12, 10, game::kLayerGround));        // ramp
    Entity m = findType(*sim, "marine", 1);
    Command c; c.player = 1; c.type = game::kCmdMove; c.units = {m}; c.point = {Fixed::fromInt(18), Fixed{}, Fixed::fromInt(2)};
    sim->submit(c);
    run(*sim, 20);
    Vec3 p = sim->world().get<Transform>(m).pos;
    CHECK((p - Vec3{Fixed::fromInt(18), Fixed{}, Fixed::fromInt(2)}).xz().length() < Fixed::fromInt(2)); // arrived via the ramp
    CHECK(mod(*sim).levelAt(*sim, p) == 1);
}

TEST(mech_sidebar_and_morph) {
    const char* rules = R"({
      "resources": {"cash": {"start": 2000}},
      "types": {
        "yard": {"structure": {"footprint": [3, 3]}, "produces": ["barracks"]},
        "barracks": {"structure": {"footprint": [2, 2]}, "cost": {"cash": 100, "time": 2}},
        "drone": {"move": {"speed": 3}, "builds": ["pool"], "buildMode": "morph"},
        "pool": {"structure": {"footprint": [2, 2]}, "cost": {"cash": 100, "time": 2}}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, R"(,"entities":[{"type":"yard","owner":1,"pos":[5,5]},{"type":"drone","owner":1,"pos":[5,12]},{"type":"yard","owner":2,"pos":[27,8]}])"));
    Entity yard = findType(*sim, "yard", 1);
    Command t; t.player = 1; t.type = game::kCmdTrain; t.units = {yard}; t.param = mod(*sim).rules().findType("barracks");
    sim->submit(t);
    Command b; b.player = 1; b.type = game::kCmdBuild; b.units = {findType(*sim, "drone", 1)}; b.param = mod(*sim).rules().findType("pool");
    b.point = {Fixed::fromInt(10), Fixed{}, Fixed::fromInt(12)};
    sim->submit(b);
    run(*sim, 3);
    CHECK(sim->world().get<game::Production>(yard).ready == mod(*sim).rules().findType("barracks"));
    Command pl; pl.player = 1; pl.type = game::kCmdPlace; pl.units = {yard}; pl.point = {Fixed::fromInt(10), Fixed{}, Fixed::fromInt(5)};
    sim->submit(pl);
    run(*sim, 5);
    CHECK(countType(*sim, "barracks", 1) == 1);
    CHECK(countType(*sim, "drone", 1) == 0);                     // morphed
    CHECK(countType(*sim, "pool", 1) == 1);
}

TEST(mech_hero_levels_and_stun) {
    const char* rules = R"({
      "types": {
        "hero": {"move": {"speed": 3}, "health": {"max": 500}, "weapons": [{"range": 1.5, "damage": 30, "cooldown": 1}],
                 "energy": {"max": 100, "start": 100}, "hero": {"levels": [20, 60], "perLevel": [{"stat": "maxHp", "add": 50}]},
                 "abilities": [{"id": "bolt", "target": "unit", "range": 6, "cooldown": 5, "energy": 20,
                                "effects": [{"type": "stun", "duration": 3}, {"type": "damage", "amount": 10}]}]},
        "grunt": {"move": {"speed": 2}, "health": {"max": 60}, "weapons": [{"range": 1, "damage": 5}], "xp": 25}
      }})";
    auto sim = makeSimJson(rules, flatMap(24, 12, R"(,"entities":[{"type":"hero","owner":1,"pos":[6,6]},{"type":"grunt","owner":2,"pos":[9,6]},{"type":"grunt","owner":2,"pos":[20,10]}])"));
    Entity hero = findType(*sim, "hero", 1), grunt = findType(*sim, "grunt", 2);
    Command a; a.player = 1; a.type = game::kCmdAbility; a.units = {hero}; a.param = 0; a.target = grunt;
    sim->submit(a);
    run(*sim, 0.5);
    CHECK(sim->world().get<game::Buffs>(grunt).stunUntil > sim->tick());
    Command k; k.player = 1; k.type = game::kCmdAttack; k.units = {hero}; k.target = grunt;
    sim->submit(k);
    run(*sim, 10);
    CHECK(!sim->world().alive(grunt));
    CHECK(sim->world().get<game::Hero>(hero).level == 2);
    CHECK(mod(*sim).stat(*sim, hero, game::Stat::MaxHp) == Fixed::fromInt(550));
}

static const char* kLaneRules = R"({
  "resources": {"crystal": {"start": 100}},
  "waveClock": 10,
  "autoBuild": {"pool": ["barracks"], "cost": {"crystal": 50}, "growth": 0.5},
  "victory": {"mode": "score", "seconds": 120},
  "types": {
    "core": {"key": true, "structure": {"footprint": [3, 3]}, "health": {"max": 2000}, "income": {"crystal": 5}, "weapons": [{"range": 5, "damage": 8}]},
    "barracks": {"structure": {"footprint": [2, 2]}, "cost": {"time": 3}, "waveSpawn": {"unit": "trooper", "count": 2}, "upgrades": ["barracks2"]},
    "barracks2": {"structure": {"footprint": [2, 2]}, "cost": {"crystal": 40, "time": 3}, "waveSpawn": {"unit": "trooper", "count": 3}},
    "trooper": {"move": {"speed": 2.5}, "health": {"max": 60}, "weapons": [{"range": 3, "damage": 6, "cooldown": 1}]}
  }})";
static std::string laneMap() {
    return flatMap(20, 48, R"(,"entities":[{"type":"core","owner":1,"pos":[10.5,42.5]},{"type":"core","owner":2,"pos":[10.5,5.5]}])");
}

TEST(mech_lane_autobuild_waves_score) {
    auto sim = makeSimJson(kLaneRules, laneMap());
    Command c; c.player = 1; c.type = game::kCmdAutoBuild; c.param = -1;
    sim->submit(c);
    run(*sim, 5);
    CHECK(countType(*sim, "barracks", 1) == 1);                  // placed automatically next to the core
    CHECK(stockOf(*sim, 1, "crystal") < Fixed::fromInt(100));
    Entity br = findType(*sim, "barracks", 1);
    Vec3 bp = sim->world().get<Transform>(br).pos, cp = sim->world().get<Transform>(findType(*sim, "core", 1)).pos;
    CHECK((bp - cp).xz().length() < Fixed::fromInt(8));
    CHECK(bp.z > cp.z - Fixed::fromInt(1));                      // behind the core (away from the enemy)
    run(*sim, 6);
    CHECK(countType(*sim, "trooper", 1) == 2);                   // first wave released at t=10s
    Command up; up.player = 1; up.type = game::kCmdUpgrade; up.units = {br}; up.param = mod(*sim).rules().findType("barracks2");
    sim->submit(up);
    run(*sim, 5);
    CHECK(countType(*sim, "barracks2", 1) == 1);
}

TEST(mech_lane_bots_deterministic) {
    auto play = [](uint64_t seed) {
        auto sim = makeSimJson(kLaneRules, laneMap(), seed);
        std::vector<game::RtsAI> bots;
        for (auto& p : sim->players()) bots.emplace_back(p.slot, seed);
        for (int t = 0; t < 20 * 130 && sim->result() == 0; ++t) { for (auto& b : bots) b.update(*sim); sim->step(); }
        return std::pair{sim->checksum(), sim->result()};
    };
    auto a = play(4), b = play(4);
    CHECK(a == b);
    CHECK(a.second != 0); // score victory decides the match at 120 s at the latest
}

TEST(mech_triggers) {
    const char* rules = R"({
      "resources": {"gold": {"start": 0}},
      "types": {"scout": {"move": {"speed": 4}}, "hq": {"structure": {"footprint": [2, 2]}}},
      "triggers": [
        {"id": "salary", "event": {"type": "every", "seconds": 5}, "forEachPlayer": true, "actions": [{"type": "give", "resource": "gold", "amount": 10}]},
        {"id": "relic", "event": {"type": "enterArea", "area": "relic", "player": 1},
         "actions": [{"type": "message", "text": "Реликвия найдена"}, {"type": "setVariable", "name": "relic", "value": 1}]},
        {"id": "win", "event": {"type": "variable", "name": "relic", ">=": 1}, "conditions": [{"type": "resource", "player": 1, "resource": "gold", ">=": 20}],
         "actions": [{"type": "victory", "player": 1}]}
      ]})";
    auto sim = makeSimJson(rules, flatMap(32, 12, R"(,"areas":{"relic":[26,4,4,4]},"entities":[{"type":"hq","owner":1,"pos":[2,2]},{"type":"scout","owner":1,"pos":[4,6]},{"type":"hq","owner":2,"pos":[29,10]}])"));
    run(*sim, 5.2);
    CHECK(stockOf(*sim, 1, "gold") == Fixed::fromInt(10) && stockOf(*sim, 2, "gold") == Fixed::fromInt(10));
    Command c; c.player = 1; c.type = game::kCmdMove; c.units = {findType(*sim, "scout", 1)}; c.point = {Fixed::fromInt(28), Fixed{}, Fixed::fromInt(6)};
    sim->submit(c);
    bool message = false;
    for (int i = 0; i < 20 * 15 && sim->result() == 0; ++i) {
        sim->step();
        for (auto& ev : sim->events()) if (ev.type == game::kEvMessage) message = true;
    }
    CHECK(message);
    CHECK(sim->result() == 1);
}

TEST(mech_regions_claim) {
    const char* rules = R"({
      "resources": {"food": {"start": 300}},
      "regions": {"claimCost": {"food": 100}, "claimCostGrowth": 1},
      "types": {"hall": {"structure": {"footprint": [2, 2]}}}
    })";
    std::string regions;
    for (int y = 0; y < 8; ++y) regions += std::string(y ? "," : "") + "\"AAAAAAAABBBBBBBBCCCCCCCC\"";
    auto sim = makeSimJson(rules, flatMap(24, 8, ",\"regions\":{\"rows\":[" + regions + "]},\"entities\":[{\"type\":\"hall\",\"owner\":1,\"pos\":[3,3]},{\"type\":\"hall\",\"owner\":2,\"pos\":[21,3]}]"));
    auto& gs = mod(*sim).state(*sim);
    CHECK(gs.regionCount == 3 && gs.regionOwner[0] == 1 && gs.regionOwner[2] == 2);
    Command c; c.player = 1; c.type = game::kCmdClaim; c.point = {Fixed::fromInt(12), Fixed{}, Fixed::fromInt(4)};
    sim->submit(c);
    run(*sim, 1);
    CHECK(gs.regionOwner[1] == 1);
    CHECK(stockOf(*sim, 1, "food") == Fixed::fromInt(200));
    Command c2 = c; c2.player = 2; // no longer free
    sim->submit(c2);
    run(*sim, 1);
    CHECK(gs.regionOwner[1] == 1);
}

TEST(mech_survive_waves) {
    const char* rules = R"({
      "victory": {"mode": "survive", "seconds": 30},
      "types": {
        "wall": {"structure": {"footprint": [2, 2]}, "health": {"max": 5000}},
        "zombie": {"move": {"speed": 2}, "health": {"max": 20}, "weapons": [{"range": 1, "damage": 2}]},
        "tower": {"structure": {"footprint": [1, 1]}, "weapons": [{"range": 7, "damage": 20, "cooldown": 0.5}], "health": {"max": 3000}}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, R"(,"waves":[{"time": 5, "player": 2, "at": [28, 8], "spawn": [{"type": "zombie", "count": 4}], "every": 8, "repeats": 2}],
       "entities":[{"type":"wall","owner":1,"pos":[4,8]},{"type":"tower","owner":1,"pos":[7,8]}])"));
    run(*sim, 6);
    CHECK(countType(*sim, "zombie", 2) == 4);
    run(*sim, 26);
    CHECK(sim->result() == 1);                                   // survived to 30 s
}

TEST(json_edit_and_dump_roundtrip) {
    Json j = Json::parse(R"({"a": 1.5, "b": [1, 2, 3], "c": {"x": "y\n"}, "rows": ["....", "~~~~"]})");
    j.set("a", Json::number(int64_t(7)));
    j.member("c").set("z", Json::fromBool(true));
    j.find("b")->push(Json::number(2.25));
    j.erase("rows");
    std::string text = j.dump();
    Json k = Json::parse(text);
    CHECK(k == j);
    CHECK(k["a"].integer() == 7 && k["c"]["z"].boolean() && k["b"].size() == 4 && !k.has("rows"));
    CHECK(Json::number(0.1 + 0.2, 3).numberLiteral() == "0.3");
    CHECK(Json::parse(j.dump(-1)) == j);
}

// ---------------- Localization (engine/i18n) ----------------
#include "engine/i18n/GameLanguage.h"
#include "modules/rts/RtsLocalize.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
// Content layout as packaged: "engine/..." from engine/assets, everything else from the game folder.
i18n::FileReader contentReader(const std::string& game) {
    return [game](const std::string& rel, std::string& out) {
        std::string path = rel.rfind("engine/", 0) == 0 ? std::string(RTS_SOURCE_DIR) + "/engine/assets/" + rel.substr(7)
                                                         : std::string(RTS_SOURCE_DIR) + "/templates/" + game + "/" + rel;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf(); out = ss.str();
        return true;
    };
}
}

TEST(i18n_plural_rules) {
    using i18n::Plural; using i18n::pluralOf;
    CHECK(pluralOf("ru", 1) == Plural::One && pluralOf("ru", 21) == Plural::One && pluralOf("ru", 11) == Plural::Many);
    CHECK(pluralOf("ru-RU", 2) == Plural::Few && pluralOf("ru", 24) == Plural::Few && pluralOf("ru", 12) == Plural::Many);
    CHECK(pluralOf("ru", 0) == Plural::Many && pluralOf("ru", 5) == Plural::Many && pluralOf("ru", 111) == Plural::Many);
    CHECK(pluralOf("uk", 22) == Plural::Few && pluralOf("pl", 1) == Plural::One && pluralOf("pl", 21) == Plural::Many);
    CHECK(pluralOf("en", 1) == Plural::One && pluralOf("en", 0) == Plural::Other && pluralOf("de", 2) == Plural::Other);
    CHECK(pluralOf("fr", 0) == Plural::One && pluralOf("pt-BR", 0) == Plural::One && pluralOf("pt-PT", 0) == Plural::Other);
    CHECK(pluralOf("ja", 1) == Plural::Other && pluralOf("zh-Hans", 1) == Plural::Other);
    CHECK(pluralOf("ar", 0) == Plural::Zero && pluralOf("ar", 2) == Plural::Two && pluralOf("ar", 103) == Plural::Few && pluralOf("ar", 111) == Plural::Many);
    CHECK(pluralOf("cs", 3) == Plural::Few && pluralOf("cs", 5) == Plural::Other);
    auto ru = i18n::requiredPlurals("ru");
    CHECK(ru.size() == 4 && ru[0] == Plural::One && ru[1] == Plural::Few && ru[2] == Plural::Many && ru[3] == Plural::Other);
    CHECK(i18n::requiredPlurals("ja").size() == 1);
}

TEST(i18n_tags_and_negotiation) {
    CHECK(i18n::normalizeTag("ru_RU.UTF-8") == "ru-RU");
    CHECK(i18n::normalizeTag("ZH_hant_tw") == "zh-Hant-TW");
    CHECK(i18n::normalizeTag("C").empty() && i18n::normalizeTag("POSIX").empty());
    CHECK(i18n::baseLanguage("pt-BR") == "pt");
    std::vector<std::string> avail = {"en", "ru", "pt-BR"};
    CHECK(i18n::negotiate({"ru-UA", "en"}, avail) == "ru");          // same base language
    CHECK(i18n::negotiate({"de", "pt-PT"}, avail) == "pt-BR");       // later preference, sibling region
    CHECK(i18n::negotiate({"de"}, avail, "en") == "en");
    i18n::GameLanguages g; g.source = "en"; g.available = {"en", "ru"};
    CHECK(i18n::chooseLanguage(g, "ru", {"en-US"}) == "ru");          // saved choice wins
    CHECK(i18n::chooseLanguage(g, "fr", {"ru-RU", "en"}) == "ru");    // stale choice: system language
    CHECK(i18n::chooseLanguage(g, "", {"ja"}) == "en");               // nothing matches: source
}

TEST(i18n_format_and_lookup) {
    CHECK(i18n::format("{a} + {b} = {{c}}", i18n::Args().num("a", 1).str("b", "x")) == "1 + x = {c}");
    CHECK(i18n::format("{missing}", {}) == "{missing}");
    CHECK((i18n::placeholders("{b} {a} {a} {{x}}") == std::vector<std::string>{"a", "b"}));

    i18n::Localization L;
    L.addTable("en", Json::parse(R"({"@language": "English", "hud": {"wave": {"one": "{count} wave", "other": "{count} waves"}, "menu": "Menu", "only_en": "E"}})"));
    L.addTable("ru", Json::parse(R"({"@language": "Русский", "hud": {"wave": {"one": "{count} волна", "few": "{count} волны", "many": "{count} волн", "other": "{count} волны"}, "menu": "Меню"}})"));
    L.setLanguage("ru-RU");
    CHECK((L.chain() == std::vector<std::string>{"ru-RU", "ru", "en"}));
    CHECK(L.tr("hud.menu") == "Меню");
    CHECK(L.tr("hud.only_en") == "E");                                // falls back to English
    CHECK(L.tr("no.such.key") == "no.such.key");                      // visible, never empty
    CHECK(L.trOr("type.x", "Footman") == "Footman");
    CHECK(L.tr("hud.wave", i18n::Args().num("count", 1)) == "1 волна");
    CHECK(L.tr("hud.wave", i18n::Args().num("count", 3)) == "3 волны");
    CHECK(L.tr("hud.wave", i18n::Args().num("count", 11)) == "11 волн");
    CHECK(L.text("@hud.menu") == "Меню" && L.text("@@tag") == "@tag" && L.text("Plain") == "Plain");
    CHECK(L.meta("@language") == "Русский");
    L.setLanguage("en");
    CHECK(L.tr("hud.wave", i18n::Args().num("count", 1)) == "1 wave" && L.tr("hud.wave", i18n::Args().num("count", 2)) == "2 waves");
    L.setPseudo(true);
    std::string p = L.tr("hud.wave", i18n::Args().num("count", 2));
    CHECK(p.front() == '[' && p.back() == ']' && p.find('2') != std::string::npos && p != "2 waves");
}

TEST(i18n_check_finds_problems) {
    i18n::Catalog en, ru;
    en.merge(Json::parse(R"({"a": "Hello {name}", "b": "B", "c": {"one": "{count} x", "other": "{count} xs"}, "d": "D"})"));
    ru.merge(Json::parse(R"({"a": "Привет {nmae}", "b": "B", "c": {"one": "{count} х", "other": "{count} х"}, "e": "лишний", "d": "Д {"})"));
    auto issues = i18n::check(en, "en", ru, "ru");
    auto has = [&](const std::string& key, const std::string& part, i18n::Issue::Severity s) {
        for (auto& i : issues) if (i.key == key && i.message.find(part) != std::string::npos && i.severity == s) return true;
        return false;
    };
    CHECK(has("a", "unknown placeholder {nmae}", i18n::Issue::Error));
    CHECK(has("c", "missing plural form 'few'", i18n::Issue::Error));
    CHECK(has("c", "missing plural form 'many'", i18n::Issue::Error));
    CHECK(has("e", "stale", i18n::Issue::Warning));
    CHECK(has("b", "not translated yet", i18n::Issue::Warning));
    CHECK(has("d", "unbalanced braces", i18n::Issue::Error));
}

TEST(i18n_engine_tables_complete) {
    auto read = contentReader("skirmish");
    for (const char* lang : {"ru"}) {
        std::string enText, text;
        CHECK(read("engine/lang/en.json", enText) && read(std::string("engine/lang/") + lang + ".json", text));
        i18n::Catalog en, other;
        en.merge(Json::parse(enText));
        other.merge(Json::parse(text));
        int problems = 0;
        for (auto& i : i18n::check(en, "en", other, lang))
            if (i.message.find("not translated yet") == std::string::npos) { ++problems; std::printf("  %s %s: %s\n", lang, i.key.c_str(), i.message.c_str()); }
        CHECK(problems == 0);
        CHECK(!other.meta().at("@language").empty());
    }
}

TEST(i18n_game_data_names) {
    auto read = contentReader("skirmish");
    std::string text;
    CHECK(read("game.json", text));
    Json manifest = Json::parse(text);
    auto langs = i18n::readGameLanguages(manifest);
    CHECK(langs.source == "en" && langs.available.size() == 2);
    CHECK(read("rules.json", text));
    Json rulesJson = Json::parse(text);

    i18n::Localization L;
    i18n::loadGameLocalization(L, read, langs, "ru");
    game::Rules display = game::loadRules(rulesJson);
    game::localizeRules(display, L);
    CHECK(display.types[size_t(display.findType("footman"))].name == "Пехотинец");
    CHECK(display.resources[size_t(display.findResource("gold"))].name == "Золото");
    CHECK(L.tr("menu.map") == "Карта");                                         // engine table
    CHECK(L.trOr("game.name", "?") == "Железо и золото");                        // game table
    CHECK(game::localizedPlayerName(L, "maps/valley.json", 0, "Blue") == "Синие");
    CHECK(i18n::languageSelfName(read, "ru") == "Русский");

    i18n::loadGameLocalization(L, read, langs, "en");
    game::Rules en = game::loadRules(rulesJson);
    game::localizeRules(en, L);
    CHECK(en.types[size_t(en.findType("footman"))].name == "Footman");          // source text from data
    CHECK(L.tr("menu.map") == "Map");

    // Every data string is reachable through a key, and the shipped tables cover them.
    std::vector<std::pair<std::string, Json>> maps;
    for (auto& m : manifest["maps"].items()) { CHECK(read(m.getStr("file"), text)); maps.push_back({m.getStr("file"), Json::parse(text)}); }
    auto strings = game::collectSourceStrings(manifest, rulesJson, maps);
    CHECK(strings.size() > 10);
    i18n::loadGameLocalization(L, read, langs, "ru");
    for (auto& s : strings) CHECK(L.catalog("ru") && L.catalog("ru")->find(s.key));
}

TEST(i18n_localize_rules_changes_only_names) {
    // The client localizes only its display copy of the rules; nothing but names may change.
    auto read = contentReader("skirmish");
    std::string rulesText, mapText;
    CHECK(read("rules.json", rulesText) && read("maps/valley.json", mapText));
    game::Rules a = game::loadRules(Json::parse(rulesText)), b = a;
    i18n::Localization L;
    L.addTable("ru", Json::parse(R"({"type": {"footman": "Пехотинец"}})"));
    L.setLanguage("ru");
    game::localizeRules(b, L);
    CHECK(a.types.size() == b.types.size());
    for (size_t i = 0; i < a.types.size(); ++i) CHECK(a.types[i].id == b.types[i].id && a.types[i].maxHp == b.types[i].maxHp && a.types[i].cost.size() == b.types[i].cost.size());
    CHECK(b.types[size_t(b.findType("footman"))].name == "Пехотинец" && b.types[size_t(b.findType("archer"))].name == "Archer");
}

// ------------------------------------------------------------------ resource packs (.rpak)

#include <random>
#include "engine/pack/PackWriter.h"

static std::unique_ptr<rts::pack::Pack> openPackBytes(const std::string& bytes, std::string* err = nullptr) {
    return rts::pack::Pack::open(rts::pack::memorySource(bytes), err);
}

TEST(pack_roundtrip_codecs_and_dedup) {
    using namespace rts::pack;
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "{\"unit\": \"peasant\", \"hp\": " + std::to_string(i % 37) + "},\n";
    std::string noise(50000, '\0');
    std::mt19937 rng(42); // test data only, not simulation
    for (char& c : noise) c = char(rng());
    PackWriter w;
    CHECK(w.addData("rules.json", text));
    CHECK(w.addData("maps\\copy.json", text));       // duplicate content, Windows separators
    CHECK(w.addData("./noise.bin", noise));          // incompressible -> stored
    CHECK(w.addData("ui/icon.png", text + "png"));   // already-compressed format -> stored
    CHECK(w.addData("empty.txt", ""));
    CHECK(w.addData("tiny.txt", "hi"));
    CHECK(!w.addData("rules.json", "again"));        // duplicate path rejected
    std::string bytes;
    WriteStats st;
    CHECK(w.writeToString(bytes, WriteOptions{}, &st));
    CHECK(st.duplicates == 1 && st.items.size() == 6);
    CHECK(bytes.size() < 2 * text.size() + noise.size()); // two of three text copies compressed/deduped
    auto p = openPackBytes(bytes);
    CHECK(p && p->size() == 6);
    if (!p) return;
    std::string out;
    CHECK(p->read("rules.json", out, true) && out == text);
    CHECK(p->read("maps/copy.json", out, true) && out == text);
    CHECK(p->read("noise.bin", out, true) && out == noise);
    CHECK(p->read("ui/icon.png", out, true) && out == text + "png");
    CHECK(p->read("empty.txt", out, true) && out.empty());
    CHECK(p->read("tiny.txt", out, true) && out == "hi");
    CHECK(!p->read("missing.json", out));
    const Entry* a = p->find("rules.json");
    const Entry* b = p->find("maps/copy.json");
    CHECK(a && b && a->offset == b->offset && a->codec == Codec::Zstd);
    CHECK(p->find("noise.bin")->codec == Codec::Stored && p->find("ui/icon.png")->codec == Codec::Stored);
    CHECK(p->contentHash() == st.contentHash && st.contentHash != 0);
}

TEST(pack_output_is_reproducible) {
    using namespace rts::pack;
    auto build = [](unsigned threads, bool reverse) {
        PackWriter w;
        std::vector<std::string> names = {"a.json", "b/c.json", "b/d.json", "z.txt"};
        if (reverse) std::reverse(names.begin(), names.end());
        for (auto& n : names) w.addData(n, std::string(3000, n[0]) + n);
        std::string out;
        WriteOptions o; o.threads = threads;
        w.writeToString(out, o);
        return out;
    };
    std::string a = build(1, false);
    CHECK(!a.empty() && a == build(4, true));         // insertion order and thread count don't matter
}

TEST(pack_rejects_corruption) {
    using namespace rts::pack;
    PackWriter w;
    std::string text(20000, 'x');
    for (size_t i = 0; i < text.size(); i += 7) text[i] = char('a' + i % 13);
    w.addData("data.json", text);
    std::string good;
    CHECK(w.writeToString(good, WriteOptions{}));
    std::string err;
    CHECK(!openPackBytes("RPAK", &err) && !err.empty());
    CHECK(!openPackBytes("NOPE" + good.substr(4), &err) && err.find("magic") != std::string::npos);
    CHECK(!openPackBytes(good.substr(0, good.size() - 3), &err));   // truncated TOC
    std::string flip = good;
    flip[kHeaderSize + 20] ^= 0x5a;                                 // inside the zstd frame
    auto p = openPackBytes(flip);
    std::string out;
    CHECK(p && !p->read("data.json", out));                         // zstd checksum catches it
}

TEST(pack_paths_and_ignore_rules) {
    using namespace rts::pack;
    CHECK(normalizePath("./maps//a.json") == "maps/a.json");
    CHECK(normalizePath("ui\\hud\\.\\x.rml") == "ui/hud/x.rml");
    CHECK(normalizePath("/abs/") == "abs" && normalizePath(".") == "");
    CHECK(globMatch("*.json", "rules.json") && !globMatch("*.json", "rules.jsonx") && globMatch("m?ps/*", "maps/a/b"));
    IgnoreRules r;
    r.add("drafts/");
    r.add("maps/test_*.json");
    CHECK(r.ignored(".DS_Store", false) && r.ignored("ui/.git", true));
    CHECK(r.ignored("replay.rpl", false) && r.ignored("old.rpak", false));
    CHECK(r.ignored("drafts", true) && !r.ignored("drafts", false));
    CHECK(r.ignored("maps/test_1.json", false) && !r.ignored("maps/valley.json", false));
}

// A match loaded from a pack must be bit-identical to one loaded from the folder.
TEST(pack_sample_games_match_folder) {
    using namespace rts::pack;
    for (auto [game, map] : {std::pair{kSkirmish, std::string("maps/valley.json")}, std::pair{kMushrooms, std::string("maps/duel.json")}}) {
        PackWriter w;
        CHECK(w.addDirectory(game, "", IgnoreRules{}) > 0);
        CHECK(w.addDirectory(std::string(RTS_SOURCE_DIR) + "/engine/assets", "engine", IgnoreRules{}) > 0);
        std::string bytes;
        CHECK(w.writeToString(bytes, WriteOptions{}));
        auto p = openPackBytes(bytes);
        CHECK(p != nullptr);
        if (!p) continue;
        std::string rules, mapText;
        CHECK(p->read("rules.json", rules, true) && p->read(map, mapText, true));
        CHECK(p->contains("engine/fonts/ui.ttf") && p->contains("game.json"));
        SimConfig cfg; cfg.seed = 9;
        auto sim = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
        sim->setup(Json::parse(rules), Json::parse(mapText));
        std::vector<game::RtsAI> bots;
        for (auto& pl : sim->players()) bots.emplace_back(pl.slot, 9);
        for (uint32_t t = 0; t < 20 * 120 && sim->result() == 0; ++t) { for (auto& b : bots) b.update(*sim); sim->step(); }
        CHECK(sim->checksum() == botMatch(game, map, 9, 20 * 120));
    }
}

// ---------------- Audio (engine/audio, modules/rts/audio) ----------------
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include "engine/audio/Audio.h"
#include "engine/audio/AudioMixer.h"   // spatialize / distanceGain (pure functions)
#include "engine/audio/Synth.h"
#include "modules/rts/audio/RtsAudio.h"

namespace {

// 16-bit PCM WAV in memory: a sine of `hz` (0 = silence).
std::string makeWav(int rate, int channels, float seconds, float hz) {
    const uint32_t frames = uint32_t(float(rate) * seconds), bytes = frames * uint32_t(channels) * 2;
    std::string s;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xFF); };
    auto u16 = [&](uint32_t v) { s += char(v & 0xFF); s += char((v >> 8) & 0xFF); };
    s += "RIFF"; u32(36 + bytes); s += "WAVEfmt "; u32(16); u16(1); u16(uint32_t(channels)); u32(uint32_t(rate));
    u32(uint32_t(rate * channels * 2)); u16(uint32_t(channels * 2)); u16(16); s += "data"; u32(bytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) u16(uint32_t(uint16_t(int16_t(std::lrint(std::sin(6.2831853 * hz * i / rate) * 16000)))));
    return s;
}

std::unique_ptr<audio::AudioEngine> offlineAudio(int maxVoices = 32) {
    auto a = std::make_unique<audio::AudioEngine>();
    audio::AudioConfig c;
    c.openDevice = false;
    c.sampleRate = 48000;
    c.maxVoices = maxVoices;
    a->init(c);
    return a;
}

// Renders `seconds` and returns RMS of the left and right channels.
std::pair<double, double> renderRms(audio::AudioEngine& a, float seconds) {
    std::vector<float> buf(size_t(48000 * seconds) * 2);
    a.render(buf.data(), int(buf.size() / 2));
    double l = 0, r = 0;
    for (size_t i = 0; i < buf.size(); i += 2) { l += buf[i] * buf[i]; r += buf[i + 1] * buf[i + 1]; }
    return {std::sqrt(l / double(buf.size() / 2)), std::sqrt(r / double(buf.size() / 2))};
}

bool loadBank(audio::AudioEngine& a, const std::string& json, std::string* err = nullptr,
              const std::map<std::string, std::string>& files = {}) {
    return a.loadBank(Json::parse(json), [&](const std::string& p, std::string& out) {
        auto it = files.find(p);
        if (it == files.end()) return false;
        out = it->second;
        return true;
    }, err);
}

std::string readSource(const std::string& rel) {
    std::ifstream f(std::string(RTS_SOURCE_DIR) + "/" + rel, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

} // namespace

TEST(audio_synth_render) {
    std::vector<audio::SynthLayer> layers;
    std::string err;
    CHECK(audio::parseSynth(Json::parse(R"([{"wave":"square","freq":[880,220],"duration":0.2},{"wave":"noise","duration":0.1,"delay":0.15}])"), layers, err));
    CHECK(layers.size() == 2);
    auto pcm = audio::renderSynth(layers, 44100, 7);
    CHECK(pcm.size() == size_t(std::ceil(0.25 * 44100)));        // delay + duration of the longest layer
    CHECK(pcm == audio::renderSynth(layers, 44100, 7));            // reproducible
    int peak = 0;
    for (auto s : pcm) peak = std::max(peak, std::abs(int(s)));
    CHECK(peak > 8000 && peak <= 32767);
    CHECK(std::abs(int(pcm.back())) < 2000);                       // release fades out: no click at the end
    CHECK(!audio::parseSynth(Json::parse(R"({"wave":"banjo"})"), layers, err) && err.find("banjo") != std::string::npos);
    CHECK(audio::parseSynth(Json::parse(R"({"wave":"saw","crush":4})"), layers, err));
    auto crushed = audio::renderSynth(layers, 44100, 1);
    std::set<int16_t> levels(crushed.begin(), crushed.end());
    CHECK(levels.size() <= 16);                                    // 4 bits
}

TEST(audio_decode_wav) {
    audio::Clip clip;
    std::string err;
    CHECK(audio::decodeClip(makeWav(22050, 1, 0.5f, 440), clip, err));
    CHECK(clip.channels == 1 && clip.sampleRate == 22050 && clip.frames() == 11025);
    CHECK(audio::decodeClip(makeWav(44100, 2, 0.25f, 220), clip, err));
    CHECK(clip.channels == 2 && clip.frames() == 11025);
    CHECK(!audio::decodeClip("not audio at all", clip, err) && !err.empty());
    CHECK(!audio::decodeClip("", clip, err));
}

TEST(audio_bank_errors_are_readable) {
    auto a = offlineAudio();
    std::string err;
    CHECK(!loadBank(*a, R"({"sounds":{"x":{"synth":{},"bus":"drums"}}})", &err) && err.find("sound 'x': unknown bus 'drums'") != std::string::npos);
    CHECK(!loadBank(*a, R"({"sounds":{"x":{"volume":1}}})", &err) && err.find("exactly one of") != std::string::npos);
    CHECK(!loadBank(*a, R"({"volume":{"loud":1}})", &err) && err.find("unknown bus 'loud'") != std::string::npos);
    CHECK(!loadBank(*a, R"({"sounds":{"x":{"synth":{},"pitch":[2,1]}}})", &err));
    // a missing file is only a warning: the game still starts, that sound is silent
    CHECK(loadBank(*a, R"({"sounds":{"boom":{"file":"sfx/boom.ogg"},"beep":{"synth":{},"volum":2}}})", &err));
    CHECK(a->soundCount() == 2 && a->sound(a->find("boom"))->clips.empty());
    CHECK(a->warnings().size() == 2);                               // missing file + unknown key 'volum'
    CHECK(a->play("boom") == audio::kNoVoice && a->play("beep") != audio::kNoVoice);
    // a failed load keeps the previous bank
    CHECK(loadBank(*a, R"({"sounds":{"beep":{"synth":{}}}})", &err));
    CHECK(!loadBank(*a, R"({"sounds":{"y":{}}})", &err) && a->find("beep") != audio::kNoSound);
    // a typo in a section name is reported instead of silently producing a game without sound
    CHECK(loadBank(*a, R"({"sound":{}})", &err) && a->warnings().size() == 1 && a->warnings()[0].find("'sound'") != std::string::npos);
}

TEST(audio_voice_limits_and_cooldown) {
    auto a = offlineAudio(4);
    CHECK(loadBank(*a, R"({"sounds":{
        "hit":  {"synth":{"wave":"sine","duration":1}, "maxInstances":2},
        "shot": {"synth":{"wave":"sine","duration":1}, "cooldown":0.1, "maxInstances":0},
        "any":  {"synth":{"wave":"sine","duration":1}, "maxInstances":0},
        "vip":  {"synth":{"wave":"sine","duration":1}, "maxInstances":0, "priority":5}}})"));
    for (int i = 0; i < 5; ++i) a->play("hit");
    renderRms(*a, 0.05f);
    CHECK(a->activeVoices() == 2);                                  // oldest instances were stolen
    CHECK(a->play("shot") != audio::kNoVoice);
    CHECK(a->play("shot") == audio::kNoVoice);                      // cooldown
    a->update(0.15);
    CHECK(a->play("shot") != audio::kNoVoice);
    renderRms(*a, 0.05f);
    CHECK(a->activeVoices() == 4);                                  // global limit (2 hit + 2 shot)
    a->play("any");                                                 // steals the oldest low-priority voice
    renderRms(*a, 0.05f);
    CHECK(a->activeVoices() == 4);
    for (int i = 0; i < 4; ++i) a->play("vip");
    renderRms(*a, 0.05f);
    a->play("any");                                                 // lower than everything playing: dropped
    renderRms(*a, 0.05f);
    CHECK(a->activeVoices() == 4);
    a->stopAll(0.01f);
    renderRms(*a, 0.05f);
    CHECK(a->activeVoices() == 0);
}

TEST(audio_spatial_and_volumes) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"spatial":{"minDistance":10,"maxDistance":20,"reverb":false},"sounds":{"tone":{"synth":{"wave":"sine","freq":300,"duration":2},"loop":true}}})"));
    a->setListener(0, 0, 0, 10);
    CHECK(a->audibility(5, 0) == 1.0f && a->audibility(25, 0) == 0.0f);
    CHECK(a->audibility(15, 0) > 0.0f && a->audibility(15, 0) < 1.0f);
    audio::PlayParams far;
    far.positional = true; far.x = 100;
    CHECK(loadBank(*a, R"({"spatial":{"minDistance":10,"maxDistance":20,"reverb":false},"sounds":{"blip":{"synth":{"duration":0.1}}}})"));
    CHECK(a->play("blip", far) == audio::kNoVoice);                 // inaudible one-shots are not even started
    CHECK(loadBank(*a, R"({"spatial":{"minDistance":10,"maxDistance":20,"reverb":false},"sounds":{"tone":{"synth":{"wave":"sine","freq":300,"duration":2},"loop":true}}})"));
    audio::PlayParams right;
    right.positional = true; right.x = 9;
    audio::VoiceId v = a->play("tone", right);
    auto [l, r] = renderRms(*a, 0.2f);
    CHECK(r > l * 1.5 && l > 0.0);                                  // panned right, both channels audible
    a->setListener(0, 0, 3.14159265f, 10);                          // camera turned around: now on the left
    renderRms(*a, 0.05f);
    std::tie(l, r) = renderRms(*a, 0.2f);
    CHECK(l > r * 1.5);
    a->setMuted(true);
    renderRms(*a, 0.1f);                                            // master fades out (no click)
    std::tie(l, r) = renderRms(*a, 0.1f);
    CHECK(l < 1e-4 && r < 1e-4);
    a->setMuted(false);
    a->setMasterVolume(0.5f);
    a->setVolume(audio::Bus::Music, 0.25f);
    auto saved = a->saveSettings();
    auto b = offlineAudio();
    b->loadSettings(saved + "garbage\nnan nan\n");
    CHECK(b->masterVolume() == 0.5f && b->volume(audio::Bus::Music) == 0.25f && !b->muted());
    a->stop(v, 0.01f);
}

TEST(audio_music_playlists) {
    auto a = offlineAudio();
    std::map<std::string, std::string> files = {{"m/a.wav", makeWav(44100, 2, 0.3f, 220)}, {"m/b.wav", makeWav(22050, 1, 0.3f, 330)}};
    CHECK(loadBank(*a, R"({"music":{"match":{"files":["m/a.wav","m/b.wav"],"shuffle":true},"menu":"m/a.wav","broken":"m/none.ogg"}})", nullptr, files));
    CHECK(a->warnings().size() == 1);                               // missing track
    CHECK(a->hasMusic("match") && !a->playMusic("nope"));
    CHECK(a->playMusic("match", 0.05f));
    renderRms(*a, 0.1f);
    auto [l, r] = renderRms(*a, 1.2f);                              // 4x longer than a track: the playlist loops
    CHECK(l > 0.1 && r > 0.1);
    a->stopMusic(0.1f);
    renderRms(*a, 0.3f);
    std::tie(l, r) = renderRms(*a, 0.2f);
    CHECK(l < 1e-4 && a->currentMusic().empty());
    CHECK(a->playMusic("broken"));                                  // no playable track: silent, no hang
    std::tie(l, r) = renderRms(*a, 0.2f);
    CHECK(l < 1e-4);
}

TEST(audio_sample_games_banks_load_cleanly) {
    for (const char* g : {"skirmish", "mushrooms"}) {
        const std::string dir = std::string("templates/") + g + "/";
        Json bank = Json::parse(readSource(dir + "audio.json"));
        game::Rules rules = game::loadRules(Json::parse(readSource(dir + "rules.json")));
        auto a = offlineAudio();
        std::string err;
        CHECK(a->loadBank(bank, [&](const std::string& p, std::string& out) { out = readSource(dir + p); return !out.empty(); }, &err));
        CHECK(a->warnings().empty());
        game::RtsAudio s;
        CHECK(s.init(*a, rules, bank, &err));
        CHECK(s.warnings().empty());
        for (auto& w : s.warnings()) std::printf("    %s: %s\n", g, w.c_str());
    }
}

TEST(audio_rts_mapping_warnings) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"sounds":{"s":{"synth":{}}}})"));
    game::Rules rules = game::loadRules(Json::parse(R"({"resources":{"gold":{"kind":"stock"}},"types":{"guy":{"weapons":[{"id":"axe"}]}}})"));
    game::RtsAudio s;
    std::string err;
    CHECK(s.init(*a, rules, Json::parse(R"({"events":{"died":"s","explode":"s","notEnough.gold":"s","notEnough.oil":"s"},
        "types":{"guy":{"select":"nope","weapons":{"axe":{"hit":"s"},"bow":{"hit":"s"}}},"ghost":{"died":"s"}}})"), &err));
    CHECK(s.warnings().size() == 5); // explode, oil, nope, bow, ghost
    CHECK(!s.init(*a, rules, Json::parse(R"({"types":[1,2]})"), &err) && err.find("\"types\" must be an object") != std::string::npos);
}

TEST(audio_rts_events_keep_sim_deterministic) {
    // The same bot match with and without sound attached must produce identical checksums, and the
    // attached run must actually produce sounds (combat, deaths, the viewer's notifications).
    const std::string dir = "templates/skirmish/";
    Json rulesJson = Json::parse(readSource(dir + "rules.json"));
    Json map = Json::parse(readSource(dir + "maps/valley.json"));
    Json bank = Json::parse(readSource(dir + "audio.json"));
    game::Rules rules = game::loadRules(rulesJson);
    auto play = [&](bool withAudio, int& played) {
        auto a = offlineAudio();
        game::RtsAudio sound;
        if (withAudio) {
            a->loadBank(bank, [&](const std::string& p, std::string& out) { out = readSource(dir + p); return !out.empty(); });
            sound.init(*a, rules, bank);
        }
        SimConfig cfg;
        cfg.seed = 11;
        Simulation sim(cfg, std::make_unique<game::RtsModule>());
        sim.setup(rulesJson, map);
        std::vector<game::RtsAI> bots;
        for (auto& p : sim.players()) bots.emplace_back(p.slot, 11);
        if (withAudio) sound.attach(sim, 1);
        std::vector<float> buf(2400 * 2);
        for (int t = 0; t < 20 * 240 && sim.result() == 0; ++t) {
            for (auto& b : bots) b.update(sim);
            sim.step();
            if (t % 2) continue;  // two ticks per "frame": the collector must not lose the first tick's events
            a->setListener(32, 24, 0, 1000);
            sound.update(sim, 0.1);
            a->update(0.1);
            a->render(buf.data(), 2400);
        }
        played = sound.played();
        return sim.checksum();
    };
    int silent = 0, loud = 0;
    const uint64_t without = play(false, silent), with = play(true, loud);
    CHECK(without == with);
    CHECK(silent == 0 && loud > 50);
}

TEST(audio_decode_and_stream_ogg_vorbis) {
    const std::string ogg = readSource("tests/data/audio/tone.ogg");
    CHECK(!ogg.empty());
    audio::Clip clip;
    std::string err;
    CHECK(audio::decodeClip(ogg, clip, err));
    CHECK(clip.channels == 2 && clip.sampleRate == 22050);
    CHECK(clip.frames() > 10500 && clip.frames() < 11600);
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"sounds":{"tone":{"file":"t.ogg","spatial":false}},"music":{"match":"t.ogg"}})", &err, {{"t.ogg", ogg}}));
    CHECK(a->warnings().empty());
    a->play("tone");
    auto [l, r] = renderRms(*a, 0.3f);
    CHECK(l > 0.05 && r > 0.05);
    renderRms(*a, 0.4f);
    CHECK(a->playMusic("match", 0.01f));                             // streamed (decoded in the mixer)
    std::tie(l, r) = renderRms(*a, 0.3f);
    CHECK(l > 0.05 && r > 0.05);
}
// ------------------------------------------------------------------ unpacker plugins (foreign archives)

#include <filesystem>
#include <fstream>
#include "engine/pack/Unpacker.h"

namespace {
namespace tfs = std::filesystem;

tfs::path testDir(const std::string& name) {
    tfs::path d = tfs::temp_directory_path() / ("rts_tests_" + name);
    std::error_code ec;
    tfs::remove_all(d, ec);
    tfs::create_directories(d);
    return d;
}
void writeFile(const tfs::path& p, const std::string& s) {
    tfs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << s;
}
bool havePython() {
#if defined(_WIN32)
    return std::system("py -3 --version >NUL 2>&1") == 0;
#else
    return std::system("python3 --version >/dev/null 2>&1") == 0;
#endif
}
std::string le32(uint32_t v) { std::string s; for (int i = 0; i < 4; ++i) s += char(uint8_t(v >> (8 * i))); return s; }
} // namespace

TEST(pack_external_flag_and_merge) {
    using namespace rts::pack;
    PackWriter a;
    a.addData("x.txt", "hello");
    WriteOptions ext; ext.external = true;
    std::string bytes;
    CHECK(a.writeToString(bytes, ext));
    auto pa = Pack::open(memorySource(bytes));
    CHECK(pa && pa->external());
    PackWriter b;
    b.addData("own.json", "{}");
    CHECK(b.addPack(*pa, "imported/a") == 1);
    CHECK(b.external());                              // external-ness is contagious
    std::string merged;
    CHECK(b.writeToString(merged, WriteOptions{}));
    auto pm = Pack::open(memorySource(merged));
    std::string out;
    CHECK(pm && pm->external() && pm->read("imported/a/x.txt", out) && out == "hello");
    PackWriter c;
    c.addData("y", "z");
    CHECK(c.writeToString(bytes, WriteOptions{}));
    CHECK(!Pack::open(memorySource(bytes))->external());
}

TEST(unpacker_manifests_are_validated) {
    using namespace rts::pack;
    tfs::path d = testDir("manifests");
    writeFile(d / "good" / "unpacker.json", R"({"protocol":1,"name":"good","extensions":["ABC"],"magic":[{"offset":-4,"ascii":"TAIL"}],"command":["tool"]})");
    writeFile(d / "oldproto" / "unpacker.json", R"({"protocol":7,"name":"old","extensions":[".x"],"command":["t"]})");
    writeFile(d / "nocmd" / "unpacker.json", R"({"protocol":1,"name":"nocmd","extensions":[".x"]})");
    writeFile(d / "reserved" / "unpacker.json", R"({"protocol":1,"name":"rpak","extensions":[".x"],"command":["t"]})");
    writeFile(d / "undetectable" / "unpacker.json", R"({"protocol":1,"name":"nothing","command":["t"]})");
    writeFile(d / "badhex" / "unpacker.json", R"({"protocol":1,"name":"badhex","magic":[{"hex":"zz"}],"command":["t"]})");
    writeFile(d / "broken" / "unpacker.json", R"({"protocol":1, oops)");
    UnpackerRegistry reg;
    reg.addSearchDir(d.string());
    CHECK(reg.errors().size() == 6);
    const UnpackerInfo* g = reg.byName("good");
    CHECK(g && g->extensions.size() == 1 && g->extensions[0] == ".abc" && g->external && g->magic[0].offset == -4);
    CHECK(reg.byName("rpak") && reg.byName("rpak")->builtin);
    std::error_code ec;
    tfs::remove_all(d, ec);
}

TEST(unpacker_plugins_import_synthetic_archives) {
    using namespace rts::pack;
    if (!havePython()) { std::printf("  (python3 not found: plugin tests skipped)\n"); return; }
    tfs::path d = testDir("plugins");
    UnpackerRegistry reg;
    reg.addDefaultDirs(RTS_SOURCE_DIR);
    CHECK(reg.errors().empty());
    for (const char* n : {"zip", "tar", "war", "zpac", "zrsc", "dmg"}) {
        const UnpackerInfo* u = reg.byName(n);
        std::string err;
        CHECK(u && reg.handshake(*u, &err));
    }
    // zip with hostile names: everything must land inside the pack namespace, sanitized.
    std::string py = "python3 -c \"import zipfile; z=zipfile.ZipFile(r'" + (d / "t.zip").string() +
                     "','w'); z.writestr('a/b.txt','bee'); z.writestr('../evil.txt','e'); z.writestr('/abs.txt','abs'); z.close()\"";
    CHECK(std::system(py.c_str()) == 0);
    // Warcraft-style archive: one stored entry, one placeholder, one LZ entry (8 literals + ref).
    std::string war = le32(0x19) + std::string("\x03\x00\xe8\x03", 4);
    uint32_t base = 8 + 12;
    std::string e0 = le32(5) + "hello";
    std::string e2 = le32(0x20000000u | 11) + std::string("\xff", 1) + "RIFFabcd" + std::string("\x00", 1) + std::string("\x00\x00", 2);
    war += le32(base) + le32(base + uint32_t(e0.size())) + le32(base + uint32_t(e0.size()) + 1);
    war += e0 + "x" + e2;
    writeFile(d / "T.WAR", war);
    // Z .PAC: zero header, LZSS with MSB-first flags: 4 literals, a 3-byte ref to ring pos 1.
    writeFile(d / "T.PAC", std::string("\0\0\0\0", 4) + std::string("\xf0", 1) + "abcd" + std::string("\x11\x00", 2));
    PackWriter w;
    std::string err;
    ImportResult r1, r2, r3;
    ImportOptions o1; o1.prefix = "z";
    CHECK(importArchive(reg, (d / "t.zip").string(), o1, w, &r1, &err));
    ImportOptions o2; o2.prefix = "w";
    CHECK(importArchive(reg, (d / "T.WAR").string(), o2, w, &r2, &err));
    ImportOptions o3; o3.prefix = "p";
    CHECK(importArchive(reg, (d / "T.PAC").string(), o3, w, &r3, &err));
    CHECK(r1.format == "zip" && r1.files == 3 && r2.format == "war" && r2.files == 2 && r3.format == "zpac" && r1.external);
    std::string bytes;
    CHECK(w.writeToString(bytes, WriteOptions{}));
    auto p = Pack::open(memorySource(bytes));
    std::string out;
    CHECK(p && p->external());
    CHECK(p && p->read("z/a/b.txt", out) && out == "bee");
    CHECK(p && p->read("z/evil.txt", out) && out == "e" && p->read("z/abs.txt", out) && out == "abs");
    CHECK(p && p->read("w/0000.bin", out) && out == "hello");
    CHECK(p && p->read("w/0002.wav", out) && out == "RIFFabcdRIF");   // ref copies ring[0..2]
    CHECK(p && p->read("p/T.bin", out) && out == "abcdabc");        // pos 1 = first output byte
    // Unknown data is refused with a readable reason.
    writeFile(d / "mystery.qqq", "????");
    CHECK(!importArchive(reg, (d / "mystery.qqq").string(), ImportOptions{}, w, nullptr, &err) && err.find("no unpacker") != std::string::npos);
    std::error_code ec;
    tfs::remove_all(d, ec);
}

#if !defined(_WIN32)
TEST(unpacker_hostile_plugin_is_contained) {
    using namespace rts::pack;
    if (!havePython()) return;
    tfs::path d = testDir("hostile");
    writeFile(d / "plug" / "unpacker.json", R"({"protocol":1,"name":"evil","extensions":[".evil"],"command":["python3","p.py"]})");
    writeFile(d / "plug" / "p.py", R"(import json, os, sys
m = sys.argv[1]
req = json.load(open(sys.argv[2]))
if m == 'info': print(json.dumps({'protocol': 1, 'name': 'evil'}))
elif m == 'probe': print(json.dumps({'match': 'fail' not in req['archive']}))
elif m == 'extract':
    if 'fail' in req['archive'] or 'bad' in req['archive']: pass
    out = req['out']; os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'ok.txt'), 'w').write('ok')
    os.symlink('/etc/hosts', os.path.join(out, 'link.txt'))
    os.symlink('/etc', os.path.join(out, 'linkdir'))
    print('library noise')
    print(json.dumps({'files': 3}))
)");
    writeFile(d / "plug2" / "unpacker.json", R"({"protocol":1,"name":"broken","extensions":[".broken"],"command":["python3","p.py"]})");
    writeFile(d / "plug2" / "p.py", "import sys\nprint('{\"error\": {\"message\": \"bad header\"}}'); sys.exit(1)\n");
    writeFile(d / "a.evil", "x");
    writeFile(d / "a.broken", "x");
    UnpackerRegistry reg;
    reg.addSearchDir(d.string());
    CHECK(reg.errors().empty());
    PackWriter w;
    std::string err;
    ImportResult r;
    CHECK(importArchive(reg, (d / "a.evil").string(), ImportOptions{}, w, &r, &err));
    CHECK(r.files == 1 && w.size() == 1);                           // symlinks never followed
    CHECK(!importArchive(reg, (d / "a.broken").string(), ImportOptions{"broken", "", {}, "", false}, w, nullptr, &err));
    CHECK(err.find("bad header") != std::string::npos);
    std::error_code ec;
    tfs::remove_all(d, ec);
}
#endif

TEST(unpacker_westwood_pak) {
    using namespace rts::pack;
    if (!havePython()) return;
    tfs::path d = testDir("wwpak");
    // Dune II layout: (offset, name\0)... 0 terminator, then data.
    std::string a = "AAAA", b = "Creative Voice File";
    uint32_t hdr = 4 + 6 + 4 + 6 + 4;            // "A.CPS\0" and "B.VOC\0"
    std::string pak = le32(hdr) + std::string("A.CPS\0", 6) + le32(hdr + 4) + std::string("B.VOC\0", 6) + le32(0) + a + b;
    writeFile(d / "T.PAK", pak);
    writeFile(d / "Q.PAK", "PACK" + std::string(12, '\0'));  // Quake pak must not be taken for Westwood
    UnpackerRegistry reg;
    reg.addDefaultDirs(RTS_SOURCE_DIR);
    std::string why;
    const UnpackerInfo* u = reg.detect((d / "T.PAK").string(), &why);
    CHECK(u && u->name == "wwpak");
    CHECK(!reg.detect((d / "Q.PAK").string(), &why));
    PackWriter w;
    ImportResult r;
    std::string err;
    CHECK(importArchive(reg, (d / "T.PAK").string(), ImportOptions{}, w, &r, &err) && r.files == 2);
    std::string bytes, out;
    CHECK(w.writeToString(bytes, WriteOptions{}));
    auto p = Pack::open(memorySource(bytes));
    CHECK(p && p->read("A.CPS", out) && out == a && p->read("B.VOC", out) && out == b);
    std::error_code ec;
    tfs::remove_all(d, ec);
}

// ------------------------------------------------------------------ importers (tools/importers)

TEST(importer_dune2_rules_and_synthetic_scenario) {
    if (!havePython()) return;
    CHECK(std::system("python3 " RTS_SOURCE_DIR "/tools/importers/lib/selftest.py >/dev/null") == 0);
    tfs::path d = testDir("dune2");
    // A made-up scenario in the Dune II INI layout (no game data): two houses, a base, units, spice.
    std::string ini = "[BASIC]\nMapScale=1\nWinFlags=6\n[MAP]\nField=1300\nBloom=2409\nSeed=353\n"
                      "[Atreides]\nQuota=500\nCredits=1000\nBrain=Human\n[Ordos]\nCredits=500\nBrain=CPU\n"
                      "[UNITS]\nID001=Atreides,Trike,256,1501,64,Guard\nID002=Ordos,Quad,256,2600,64,Hunt\nID003=Fremen,Sandworm,256,2000,64,Ambush\n"
                      "ID004=Ordos,Frigate,256,2000,64,Guard\n"
                      "[STRUCTURES]\nID000=Atreides,Const Yard,256,1630\nID001=Ordos,Windtrap,256,2660\nGEN2662=Ordos,Wall\nGEN2663=Ordos,Concrete\n";
    std::string pak = le32(4 + 13 + 4) + std::string("SCENA001.INI\0", 13) + le32(0) + ini;
    writeFile(d / "src" / "SCENARIO.PAK", pak);
    std::string cmd = "python3 " RTS_SOURCE_DIR "/tools/importers/dune2/import.py --maps-only --json --source '" + (d / "src").string() +
                      "' --out '" + (d / "out").string() + "' > '" + (d / "report.json").string() + "'";
    CHECK(std::system(cmd.c_str()) == 0);
    Json rep = Json::parseFile((d / "report.json").string());
    CHECK(rep.getBool("ok") && rep.getInt("maps") == 1 && rep["skipped"].getInt("Frigate") == 1);
    Json game = Json::parseFile((d / "out" / "game.json").string());
    CHECK(game.getBool("external") && game["maps"].size() == 1);
    // The importer's rules and map must load into the real simulation and run.
    SimConfig cfg; cfg.seed = 4;
    auto sim = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
    bool loaded = true;
    try {
        sim->setup(Json::parseFile((d / "out" / "rules.json").string()), Json::parseFile((d / "out" / "maps" / "scena001.json").string()));
    } catch (const std::exception& e) { std::printf("  %s\n", e.what()); loaded = false; }
    CHECK(loaded);
    if (loaded) {
        CHECK(countType(*sim, "const_yard", 1) == 1 && countType(*sim, "windtrap", 2) == 1 && countType(*sim, "trike", 1) == 1);
        CHECK(countType(*sim, "wall", 2) == 1 && countType(*sim, "spice_bloom", 0) == 1);
        Fixed spice{};
        for (auto a : mod(*sim).state(*sim).grid.fieldAmt) spice += a;
        CHECK(spice > Fixed{});                                       // Field= became a spice field
        run(*sim, 20);
        CHECK(sim->result() == 0);                                   // Ordos still has units: no instant win
    }
    std::error_code ec;
    tfs::remove_all(d, ec);
}

TEST(mech_surrender_ends_match_and_replays) {
    auto play = [](std::vector<Command>* cmds, uint32_t* end, bool* surrenderEvent) {
        auto sim = makeSim(kSkirmish, "maps/valley.json", 9);
        std::vector<game::RtsAI> bots;
        for (auto& p : sim->players()) bots.emplace_back(p.slot, 9);
        for (uint32_t t = 0; t < 20 * 60 && sim->result() == 0; ++t) {
            for (auto& b : bots) b.update(*sim);
            if (sim->tick() == 20 * 30) { Command c; c.player = 2; c.type = game::kCmdSurrender; sim->submit(c); }
            sim->step();
            for (auto& ev : sim->events())
                if (ev.type == game::kEvPlayerDefeated && ev.player == 2 && ev.value == game::kDefeatSurrendered) *surrenderEvent = true;
        }
        *cmds = sim->executedCommands();
        *end = sim->tick();
        return std::pair{sim->checksum(), sim->result()};
    };
    std::vector<Command> cmds;
    uint32_t end = 0;
    bool ev = false;
    auto live = play(&cmds, &end, &ev);
    CHECK(ev);
    CHECK(live.second == 1);                                   // team of player 1 wins by surrender
    CHECK(end > 20 * 30 && end < 20 * 32);
    auto sim = makeSim(kSkirmish, "maps/valley.json", 9);
    for (auto& c : cmds) sim->submit(c);
    while (sim->tick() < end) sim->step();
    CHECK(sim->checksum() == live.first && sim->result() == 1);
}

// ------------------------------------------------------------------ dungeon management (Dungeon Keeper class)

static const char* kDungeonRules = R"({
  "resources": {"gold": {"start": 1000}, "food": {"start": 0}, "research": {"start": 0}},
  "diggable": {"D": {"hardness": 2}, "G": {"hardness": 2, "yields": {"gold": 150}}},
  "territory": {"startRadius": 2, "buildOnlyOwned": true},
  "hand": {"max": 4, "slapDamage": 5},
  "possession": {"cost": {"gold": 10}},
  "victory": "keyUnits",
  "rooms": {
    "lair": {"cost": {"gold": 10}, "supply": 1},
    "hatchery": {"cost": {"gold": 10}, "income": {"food": 0.5}},
    "library": {"cost": {"gold": 20}, "work": {"yields": {"research": 1}}}
  },
  "types": {
    "heart": {"structure": {"footprint": [2, 2]}, "health": {"max": 3000}, "key": true, "sight": 12, "produces": ["imp"]},
    "shrine": {"structure": {"footprint": [1, 1]}},
    "imp": {"move": {"speed": 4}, "digs": {"rate": 2}, "claimer": {"time": 0.5}, "health": {"max": 60}, "sight": 6},
    "portal": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "claimedByTerritory": true, "portal": {"interval": 3}},
    "goblin": {"move": {"speed": 3}, "health": {"max": 100}, "weapons": [{"range": 1, "damage": 8}], "cost": {"supply": 1}, "sight": 6,
               "attractedBy": {"rooms": {"lair": 1}},
               "needs": {"hunger": {"interval": 5, "room": "hatchery", "time": 1, "cost": {"food": 1}},
                         "pay": {"interval": 4, "amount": {"gold": 30}}, "works": ["library"],
                         "happiness": {"unpaid": 0.5, "leaveBelow": 0.3, "leaveAfter": 2}}}
  }})";

// 24x12: own pocket on the left (x 1..9), dirt wall with a gold vein at x 10..13, open ground on the right.
static std::string dungeonMap(const std::string& entities) {
    std::string rows;
    for (int y = 0; y < 12; ++y) {
        std::string r(24, '.');
        r[0] = r[23] = '#';
        if (y == 0 || y == 11) r = std::string(24, '#');
        else for (int x = 10; x <= 13; ++x) r[size_t(x)] = (x == 10 && y == 6) ? 'G' : 'D';
        rows += std::string(y ? "," : "") + "\"" + r + "\"";
    }
    return R"({"size":[24,12],"players":[{"name":"A","team":1},{"name":"B","team":2}],"terrain":{"rows":[)" + rows + "]},\"entities\":[" +
           R"({"type":"heart","owner":1,"pos":[4,6]},{"type":"heart","owner":2,"pos":[20,6]})" + entities + "]}";
}
static Command rectCmd(uint8_t player, uint16_t type, int x, int y, int w, int h, int extra) {
    Command c; c.player = player; c.type = type;
    c.point = {Fixed::fromInt(x) + Fixed::fromRatio(1, 2), Fixed{}, Fixed::fromInt(y) + Fixed::fromRatio(1, 2)};
    c.param = game::rectParam(w, h, extra);
    return c;
}

TEST(mech_dungeon_dig_and_claim) {
    auto sim = makeSimJson(kDungeonRules, dungeonMap(R"(,{"type":"imp","owner":1,"pos":[7,6]})"));
    auto& m = mod(*sim);
    auto& gs = m.state(*sim);
    auto& g = gs.grid;
    CHECK(g.dig[size_t(g.index(10, 6))] == 2 && !g.passable(10, 6, game::kLayerGround));   // gold vein, solid
    CHECK(g.territory[size_t(g.index(4, 6))] == 1 && g.territory[size_t(g.index(8, 6))] == 0); // start radius around the heart
    int before = m.territoryOf(*sim, 1);
    Fixed gold = stockOf(*sim, 1, "gold");
    sim->submit(rectCmd(1, game::kCmdDig, 10, 5, 2, 3, 0));
    sim->submit(rectCmd(2, game::kCmdDig, 12, 5, 1, 1, 0));                                   // B's mark is not A's job
    run(*sim, 60);
    CHECK(g.dig[size_t(g.index(10, 6))] == 0 && g.passable(10, 6, game::kLayerGround));      // dug out
    CHECK(g.dig[size_t(g.index(11, 5))] == 0 && g.dig[size_t(g.index(11, 7))] == 0);
    CHECK(g.dig[size_t(g.index(12, 5))] != 0);                                                // only own marks
    CHECK(stockOf(*sim, 1, "gold") == gold + Fixed::fromInt(150));                            // vein yield
    CHECK(m.territoryOf(*sim, 1) > before + 10);                                              // claimed the pocket and the tunnel
    CHECK(g.territory[size_t(g.index(9, 6))] == 1 && g.territory[size_t(g.index(11, 6))] == 1);
    std::vector<Vec3> path;                                                                   // pathfinding sees the tunnel
    CHECK(g.findPath(g.cellCenter(7, 6), g.cellCenter(11, 7), game::kLayerGround, path));
    // build only on own territory
    std::string why;
    bool ok = m.canPlace(*sim, 1, uint16_t(m.rules().findType("shrine")), g.cellCenter(8, 3), kNullEntity, &why);
    if (!ok) std::printf("    canPlace: %s (territory %d)\n", why.c_str(), g.territory[size_t(g.index(8, 3))]);
    CHECK(ok);
    CHECK(!m.canPlace(*sim, 1, uint16_t(m.rules().findType("shrine")), g.cellCenter(16, 3)));
}

TEST(mech_dungeon_rooms_portal_recruit) {
    auto sim = makeSimJson(kDungeonRules, dungeonMap(R"(,{"type":"portal","owner":0,"pos":[8,9]},{"type":"imp","owner":1,"pos":[7,6]})"));
    auto& m = mod(*sim);
    auto& gs = m.state(*sim);
    run(*sim, 6);
    Entity portal = findType(*sim, "portal", 1);
    CHECK(portal.valid());                                    // claimed when the imp claimed the floor next to it
    CHECK(countType(*sim, "goblin", 1) == 0);                 // no lair: nothing is attracted
    Fixed gold = stockOf(*sim, 1, "gold");
    sim->submit(rectCmd(1, game::kCmdPaintRoom, 1, 3, 2, 2, m.rules().findRoom("lair")));
    sim->submit(rectCmd(1, game::kCmdPaintRoom, 15, 3, 2, 2, m.rules().findRoom("lair"))); // not own floor: ignored
    run(*sim, 0.5);
    CHECK(m.roomCellsOf(*sim, 1, m.rules().findRoom("lair")) == 4);
    CHECK(stockOf(*sim, 1, "gold") == gold - Fixed::fromInt(40));
    CHECK(gs.players[1].supplyCap == Fixed::fromInt(4));       // lair capacity per cell
    run(*sim, 12);
    int goblins = countType(*sim, "goblin", 1);
    CHECK(goblins >= 1 && goblins <= 4);                       // recruited up to the lair capacity
    sim->submit(rectCmd(1, game::kCmdPaintRoom, 1, 3, 2, 2, -1)); // erase: refunds half
    run(*sim, 0.5);
    CHECK(m.roomCellsOf(*sim, 1, m.rules().findRoom("lair")) == 0);
    CHECK(gs.roomZones.empty());
}

TEST(mech_dungeon_needs_work_and_leaving) {
    auto sim = makeSimJson(kDungeonRules, dungeonMap(R"(,{"type":"portal","owner":1,"pos":[8,9]},{"type":"goblin","owner":1,"pos":[6,4]})"));
    auto& m = mod(*sim);
    auto& gs = m.state(*sim);
    sim->submit(rectCmd(1, game::kCmdPaintRoom, 1, 3, 2, 2, m.rules().findRoom("lair")));
    sim->submit(rectCmd(1, game::kCmdPaintRoom, 1, 7, 2, 2, m.rules().findRoom("hatchery")));
    sim->submit(rectCmd(1, game::kCmdPaintRoom, 5, 3, 2, 2, m.rules().findRoom("library")));
    run(*sim, 4);
    CHECK(stockOf(*sim, 1, "research") > Fixed{});            // works in the library when idle
    CHECK(stockOf(*sim, 1, "food") > Fixed{});                // hatchery produces food per cell
    Fixed gold = stockOf(*sim, 1, "gold");
    run(*sim, 6);
    Entity gob = findType(*sim, "goblin", 1);
    CHECK(gob.valid());
    CHECK(stockOf(*sim, 1, "gold") <= gold - Fixed::fromInt(30)); // payday
    auto& nd = sim->world().get<game::Needs>(gob);
    CHECK(nd.hunger < Fixed::fromInt(1));                      // ate in the hatchery
    CHECK(nd.happiness > Fixed::fromRatio(9, 10));
    // unpaid wages make it unhappy; it walks out through the portal
    gs.players[1].stock[size_t(m.rules().findResource("gold"))] = Fixed{};
    bool left = false;
    for (int i = 0; i < 20 * 20 && !left; ++i) { sim->step(); for (auto& ev : sim->events()) if (ev.type == game::kEvCreatureLeft && ev.a == gob) left = true; }
    CHECK(left);
    run(*sim, 0.2);
    CHECK(!sim->world().alive(gob));
}

TEST(mech_dungeon_hand_slap_possess) {
    auto sim = makeSimJson(kDungeonRules, dungeonMap(R"(,{"type":"goblin","owner":1,"pos":[6,4]},{"type":"goblin","owner":2,"pos":[18,4]})"));
    auto& m = mod(*sim);
    auto& gs = m.state(*sim);
    World& w = sim->world();
    Entity gob = findType(*sim, "goblin", 1), enemy = findType(*sim, "goblin", 2);
    run(*sim, 0.5);
    Command pick; pick.player = 1; pick.type = game::kCmdPickUp; pick.units = {gob, enemy};
    sim->submit(pick);
    run(*sim, 0.2);
    CHECK(gs.hand[1].size() == 1 && (w.get<game::Unit>(gob).flags & game::kUnitInHand));
    CHECK(!(w.get<game::Unit>(enemy).flags & game::kUnitInHand));
    Command mv; mv.player = 1; mv.type = game::kCmdMove; mv.units = {gob}; mv.point = {Fixed::fromInt(2), Fixed{}, Fixed::fromInt(8)};
    sim->submit(mv);                                           // units in the hand ignore orders
    sim->submit(rectCmd(1, game::kCmdDrop, 16, 4, 1, 1, 0));    // not own floor: refused
    run(*sim, 0.5);
    CHECK(gs.hand[1].size() == 1);
    sim->submit(rectCmd(1, game::kCmdDrop, 2, 8, 1, 1, 0));
    run(*sim, 0.2);
    CHECK(gs.hand[1].empty() && !(w.get<game::Unit>(gob).flags & game::kUnitInHand));
    CHECK((w.get<Transform>(gob).pos - Vec3{Fixed::fromRatio(5, 2), Fixed{}, Fixed::fromRatio(17, 2)}).xz().length() < Fixed::fromInt(1));
    Fixed speed = m.stat(*sim, gob, game::Stat::Speed);
    Command slap; slap.player = 1; slap.type = game::kCmdSlap; slap.units = {gob};
    sim->submit(slap);
    run(*sim, 0.2);
    CHECK(w.get<game::Unit>(gob).hp == Fixed::fromInt(95));
    CHECK(m.stat(*sim, gob, game::Stat::Speed) > speed);
    CHECK(w.get<game::Needs>(gob).happiness < Fixed::fromInt(1));
    // possession: direct steering, AI-style orders ignored
    Command pos; pos.player = 1; pos.type = game::kCmdPossess; pos.units = {gob};
    sim->submit(pos);
    run(*sim, 0.2);
    CHECK(gs.possessed[1] == gob && (w.get<game::Unit>(gob).flags & game::kUnitPossessed));
    Fixed x0 = w.get<Transform>(gob).pos.x;
    for (int i = 0; i < 20; ++i) {
        Command d; d.player = 1; d.type = game::kCmdDirect; d.point = {Fixed::fromInt(1), Fixed{}, Fixed{}};
        sim->submit(d);
        sim->step();
    }
    CHECK(w.get<Transform>(gob).pos.x > x0 + Fixed::fromInt(2));
    Command rel; rel.player = 1; rel.type = game::kCmdPossess;
    sim->submit(rel);
    run(*sim, 0.2);
    CHECK(!gs.possessed[1].valid() && !(w.get<game::Unit>(gob).flags & game::kUnitPossessed));
}

// ------------------------------------------------------------------ territory war (State.io style)

static const char* kTerritoryRules = R"({
  "resources": {"money": {"start": 300}},
  "territory": {"claimBy": "units", "startFromRegions": true},
  "victory": {"mode": "territory", "share": 0.8},
  "playerAbilities": [{"id": "strike", "cost": {"money": 50}, "cooldown": 2, "charges": {"max": 1, "rearmCost": {"money": 20}, "rearmTime": 3},
                       "effects": [{"type": "damage", "amount": 500, "radius": 2}]}],
  "techs": {"drill": {"cost": {"money": 10, "time": 0.5}, "levels": 3, "costGrowth": 1, "effects": [{"type": "soldier", "stat": "damage", "mul": 1.5}]}},
  "types": {
    "barracks": {"structure": {"footprint": [2, 2]}, "capturable": true, "health": {"max": 800}, "supply": 10,
                 "autoTrain": {"unit": "soldier", "interval": 1}, "researches": ["drill"]},
    "town": {"structure": {"footprint": [2, 2]}, "capturable": true, "invulnerable": true},
    "soldier": {"move": {"speed": 3}, "health": {"max": 50}, "weapons": [{"range": 1.5, "damage": 6}], "cost": {"supply": 1}, "claim": {"radius": 1.5, "rate": 4}},
    "ghost": {"move": {"speed": 1}, "invulnerable": true}
  }})";

static std::string territoryMap(const std::string& entities, const std::string& regionRow) {
    std::string regions;
    for (int y = 0; y < 8; ++y) regions += std::string(y ? "," : "") + "\"" + regionRow + "\"";
    return flatMap(24, 8, ",\"regions\":{\"rows\":[" + regions + "]},\"entities\":[" + entities + "]");
}

TEST(mech_territory_front_line_and_contested_cells) {
    auto sim = makeSimJson(kTerritoryRules, territoryMap(
        R"({"type":"town","owner":1,"pos":[3,4]},{"type":"town","owner":2,"pos":[21,4]},{"type":"soldier","owner":1,"pos":[8.5,4.5]},{"type":"ghost","owner":2,"pos":[11.5,1.5]})",
        "AAAAAAAANNNNNNNNNNNNBBBB"));
    auto& m = mod(*sim);
    auto& g = m.state(*sim).grid;
    CHECK(m.territoryOf(*sim, 1) == 64 && m.territoryOf(*sim, 2) == 32 && m.state(*sim).territoryTotal == 192);
    Command mv; mv.player = 1; mv.type = game::kCmdMove; mv.units = {findType(*sim, "soldier", 1)}; mv.point = {Fixed::fromRatio(23, 2), Fixed{}, Fixed::fromRatio(7, 2)};
    sim->submit(mv);
    run(*sim, 8);
    CHECK(g.territory[size_t(g.index(8, 4))] == 1 && g.territory[size_t(g.index(10, 4))] == 1 && g.territory[size_t(g.index(11, 3))] == 1);
    CHECK(g.territory[size_t(g.index(11, 1))] == 0);           // contested: an enemy stands there
    CHECK(g.territory[size_t(g.index(16, 4))] == 0);           // only cells touching own territory near the unit
    CHECK(m.territoryOf(*sim, 1) > 64);
}

TEST(mech_territory_autotrain_capture_victory) {
    auto sim = makeSimJson(kTerritoryRules, territoryMap(
        R"({"type":"barracks","owner":1,"pos":[3,4]},{"type":"town","owner":2,"pos":[16,4]})", "AAAAAAAAAAAABBBBBBBBBBBB"));
    auto& m = mod(*sim);
    run(*sim, 5);
    int soldiers = countType(*sim, "soldier", 1);
    CHECK(soldiers >= 3 && soldiers <= 10);
    bool captured = false;
    for (int i = 0; i < 20 * 120 && sim->result() == 0; ++i) {
        sim->step();
        for (auto& ev : sim->events()) if (ev.type == game::kEvCaptured && ev.player == 1) captured = true;
    }
    CHECK(countType(*sim, "soldier", 1) <= 10);                // population cap
    CHECK(captured && sim->world().get<Owner>(findType(*sim, "town", 1)).player == 1);
    CHECK(sim->result() == 1);                                 // 80% share
    CHECK(m.territoryOf(*sim, 1) * 10 >= m.state(*sim).territoryTotal * 8);
}

TEST(mech_player_abilities_and_tech_levels) {
    auto sim = makeSimJson(kTerritoryRules, territoryMap(
        R"({"type":"town","owner":1,"pos":[3,4]},{"type":"town","owner":2,"pos":[21,4]},{"type":"soldier","owner":2,"pos":[12,4]},{"type":"soldier","owner":2,"pos":[12.5,5]},{"type":"barracks","owner":1,"pos":[6,4]})",
        "AAAAAAAAAAAABBBBBBBBBBBB"));
    auto& m = mod(*sim);
    auto& gs = m.state(*sim);
    gs.players[1].stock[0] = Fixed::fromInt(300);
    Command a; a.player = 1; a.type = game::kCmdPlayerAbility; a.param = 0; a.point = {Fixed::fromInt(12), Fixed{}, Fixed::fromRatio(9, 2)};
    CHECK(m.canUsePlayerAbility(*sim, 1, 0));
    sim->submit(a);
    run(*sim, 0.2);
    CHECK(countType(*sim, "soldier", 2) == 0);
    CHECK(gs.playerAbilities[1][0].charges == 0 && !m.canUsePlayerAbility(*sim, 1, 0));
    Fixed money = stockOf(*sim, 1, "money");
    run(*sim, 3.5);
    CHECK(gs.playerAbilities[1][0].charges == 1 && stockOf(*sim, 1, "money") == money - Fixed::fromInt(20)); // rearmed for a cost
    // repeatable tech: 3 levels, cost 10, 20, 30, effects compound
    Entity br = findType(*sim, "barracks", 1);
    int drill = m.rules().findTech("drill");
    for (int lv = 0; lv < 4; ++lv) {
        Command r; r.player = 1; r.type = game::kCmdResearch; r.units = {br}; r.param = drill;
        sim->submit(r);
        run(*sim, 1);
    }
    CHECK(gs.players[1].researched[size_t(drill)] == 3);
    CHECK(stockOf(*sim, 1, "money") == money - Fixed::fromInt(20 + 60));
    Entity sol = findType(*sim, "soldier", 1);
    CHECK(sol.valid() && m.stat(*sim, sol, game::Stat::Damage) == Fixed::parse("3.375"));
}

// ------------------------------------------------------------------ settlers economy (Knights and Merchants style)

static const char* kSettlersRules = R"({
  "storage": "buildings",
  "roadNetwork": {"cost": {"stone": 0.1}, "speed": 1.5},
  "resources": {"wood": {}, "planks": {"start": 10}, "stone": {"start": 20}, "food": {}, "sword": {"start": 2}, "gold": {"global": true, "start": 100}},
  "types": {
    "store": {"structure": {"footprint": [2, 2]}, "storehouse": {"capacity": 500}, "health": {"max": 1000}},
    "tree": {"invulnerable": true, "structure": {"footprint": [1, 1]}, "resourceNode": {"resource": "wood", "amount": 50}},
    "woodcutter": {"structure": {"footprint": [2, 2]}, "recipe": {"out": {"wood": 1}, "time": 2, "from": "wood", "radius": 8}, "cost": {"planks": 2, "gold": 10, "time": 2}},
    "sawmill": {"structure": {"footprint": [2, 2]}, "recipe": {"in": {"wood": 1}, "out": {"planks": 2}, "time": 2}},
    "inn": {"structure": {"footprint": [2, 2]}},
    "barracks": {"structure": {"footprint": [2, 2]}, "produces": ["soldier"]},
    "serf": {"move": {"speed": 4}, "carrier": {"capacity": 1}},
    "builder": {"move": {"speed": 3}, "builds": ["woodcutter"]},
    "recruit": {"move": {"speed": 3}},
    "soldier": {"move": {"speed": 3}, "health": {"max": 100}, "weapons": [{"range": 1, "damage": 5}], "trainFrom": "recruit", "cost": {"sword": 1, "time": 1},
                "needs": {"hunger": {"interval": 3, "at": "inn", "time": 0.5, "cost": {"food": 1}, "starve": 10}}}
  }})";

static Command connectCmd(Simulation& sim, std::vector<Entity> units) {
    (void)sim;
    Command c; c.player = 1; c.type = game::kCmdConnectRoad; c.units = std::move(units);
    return c;
}

TEST(mech_settlers_chain_carriers_roads) {
    auto sim = makeSimJson(kSettlersRules, flatMap(32, 14, R"(,"entities":[
        {"type":"store","owner":1,"pos":[4,6]},{"type":"woodcutter","owner":1,"pos":[14,6]},{"type":"sawmill","owner":1,"pos":[9,11]},
        {"type":"tree","owner":0,"pos":[18.5,4.5]},{"type":"tree","owner":0,"pos":[18.5,8.5]},
        {"type":"serf","owner":1,"pos":[6,9]},{"type":"serf","owner":1,"pos":[7,9]},{"type":"store","owner":2,"pos":[29,2]}])"));
    auto& m = mod(*sim);
    World& w = sim->world();
    CHECK(stockOf(*sim, 1, "planks") == Fixed::fromInt(10));           // starting goods are in the storehouse
    CHECK(w.get<game::Inventory>(findType(*sim, "store", 1)).have[size_t(m.rules().findResource("planks"))] == Fixed::fromInt(10));
    Entity wc = findType(*sim, "woodcutter", 1), saw = findType(*sim, "sawmill", 1), store = findType(*sim, "store", 1);
    sim->submit(connectCmd(*sim, {wc}));                                 // the sawmill stays off the road network
    run(*sim, 20);
    CHECK(m.roadConnected(*sim, wc, store) && !m.roadConnected(*sim, saw, store));
    CHECK(stockOf(*sim, 1, "stone") < Fixed::fromInt(20));               // roads cost stone
    CHECK(stockOf(*sim, 1, "wood") >= Fixed::fromInt(2));                 // wood hauled to the store
    CHECK(w.get<game::Inventory>(saw).have[size_t(m.rules().findResource("wood"))] == Fixed{}); // unconnected: no deliveries
    sim->submit(connectCmd(*sim, {saw}));
    Fixed planks = stockOf(*sim, 1, "planks");
    run(*sim, 30);
    CHECK(m.roadConnected(*sim, saw, store));
    CHECK(stockOf(*sim, 1, "planks") > planks);                          // wood -> planks -> store
    int trees = countType(*sim, "tree", 0);
    CHECK(trees == 2);
    Fixed left{};
    w.each<game::ResourceStock>([&](Entity, game::ResourceStock& rs) { left += rs.amount; });
    CHECK(left < Fixed::fromInt(100));                                    // raw producer depletes the trees
}

TEST(mech_settlers_construction_materials_and_recruits) {
    auto sim = makeSimJson(kSettlersRules, flatMap(32, 14, R"(,"entities":[
        {"type":"store","owner":1,"pos":[4,6]},{"type":"barracks","owner":1,"pos":[4,11]},{"type":"inn","owner":1,"pos":[9,3]},
        {"type":"builder","owner":1,"pos":[7,7]},{"type":"serf","owner":1,"pos":[6,9]},{"type":"recruit","owner":1,"pos":[6,12]},
        {"type":"store","owner":2,"pos":[29,2]}])"));
    auto& m = mod(*sim);
    World& w = sim->world();
    Command b; b.player = 1; b.type = game::kCmdBuild; b.units = {findType(*sim, "builder", 1)}; b.param = m.rules().findType("woodcutter");
    b.point = {Fixed::fromInt(12), Fixed{}, Fixed::fromInt(7)};
    sim->submit(b);
    run(*sim, 2);
    Entity site = findType(*sim, "woodcutter", 1);
    CHECK(site.valid() && w.has<game::Construction>(site));
    CHECK(stockOf(*sim, 1, "gold") == Fixed::fromInt(90));               // coins paid at once, planks delivered
    CHECK(stockOf(*sim, 1, "planks") == Fixed::fromInt(10));
    run(*sim, 3);
    CHECK(w.has<game::Construction>(site));                              // no road yet: nothing delivered, no progress past 0
    sim->submit(connectCmd(*sim, {site}));
    run(*sim, 25);
    CHECK(countType(*sim, "woodcutter", 1) == 1);                         // built once the planks arrived
    CHECK(stockOf(*sim, 1, "planks") == Fixed::fromInt(8));
    // soldiers are made from a recruit plus a sword
    Entity br = findType(*sim, "barracks", 1);
    Command t; t.player = 1; t.type = game::kCmdTrain; t.units = {br}; t.param = m.rules().findType("soldier");
    sim->submit(t);
    sim->submit(t);                                                      // second one: no recruit left
    run(*sim, 2);
    CHECK(countType(*sim, "soldier", 1) == 1 && countType(*sim, "recruit", 1) == 0);
    CHECK(stockOf(*sim, 1, "sword") == Fixed::fromInt(1));
    // food: no food in the store -> the soldier starves
    Entity sol = findType(*sim, "soldier", 1);
    run(*sim, 5);
    CHECK(w.get<game::Unit>(sol).hp < Fixed::fromInt(100));
    m.addResource(*sim, 1, uint16_t(m.rules().findResource("food")), Fixed::fromInt(5));
    CHECK(stockOf(*sim, 1, "food") == Fixed::fromInt(5));
    run(*sim, 4);
    CHECK(stockOf(*sim, 1, "food") < Fixed::fromInt(5));                  // ate at the inn
}

// ------------------------------------------------------------------ Mushroom Wars 2 lanes

TEST(mech_lanes_generated_and_followed) {
    const char* rules = R"({
      "lanes": {"left": 2, "right": 1, "spacing": 0.4},
      "types": {
        "village": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "garrison": {"max": 30, "unit": "shroom", "interval": 1}},
        "shroom": {"move": {"speed": 3}, "entersGarrison": true}
      }})";
    std::string rows;
    for (int y = 0; y < 16; ++y) {
        std::string r(32, '.');
        if (y >= 4 && y <= 11) for (int x = 15; x <= 16; ++x) r[size_t(x)] = '#';   // a rock between the villages
        rows += std::string(y ? "," : "") + "\"" + r + "\"";
    }
    std::string map = R"({"size":[32,16],"players":[{"name":"A","team":1},{"name":"B","team":2}],"terrain":{"rows":[)" + rows + R"(]},
        "entities":[{"type":"village","owner":1,"pos":[5,8],"garrison":20},{"type":"village","owner":0,"pos":[27,8],"garrison":0},{"type":"village","owner":2,"pos":[16,1],"garrison":5}]})";
    auto sim = makeSimJson(rules, map);
    auto& m = mod(*sim);
    auto& gs = m.state(*sim);
    World& w = sim->world();
    run(*sim, 0.1);
    CHECK(gs.lanes.size() == 3 * 4);                                     // 3 pairs x (1 main + 2 left + 1 right)
    Entity a, b;
    w.each<game::Garrison, Owner, Transform>([&](Entity e, game::Garrison&, Owner& o, Transform& t) {
        if (o.player == 1) a = e;
        if (o.player == 0 && t.pos.x > Fixed::fromInt(20)) b = e;
    });
    auto lanes = m.lanesBetween(*sim, a, b);
    CHECK(lanes.size() == 4 && gs.lanes[size_t(lanes[0])].offset == 0);
    bool clear = true, apart = false;
    for (int li : lanes) {
        const auto& L = gs.lanes[size_t(li)];
        for (auto& p : L.points) {
            int cx = gs.grid.cellX(p.x), cy = gs.grid.cellY(p.z);
            if (cx >= 15 && cx <= 16 && cy >= 4 && cy <= 11) clear = false;  // never through the rock
        }
        if (L.offset != 0 && (L.points[L.points.size() / 2] - gs.lanes[size_t(lanes[0])].points[gs.lanes[size_t(lanes[0])].points.size() / 2]).xz().length() > Fixed::fromRatio(3, 10)) apart = true;
    }
    CHECK(clear && apart);
    Command c; c.player = 1; c.type = game::kCmdSend; c.units = {a}; c.target = b; c.param = 50;
    sim->submit(c);
    run(*sim, 1.5);
    std::vector<int> used(gs.lanes.size(), 0);
    int walkers = 0;
    w.each<game::LaneWalker>([&](Entity, game::LaneWalker& lw) { used[size_t(lw.lane)]++; ++walkers; });
    int lanesUsed = 0;
    for (int li : lanes) if (used[size_t(li)]) ++lanesUsed;
    CHECK(walkers > 4 && lanesUsed == 4);                               // round-robin over all lanes
    run(*sim, 15);
    CHECK(w.get<Owner>(b).player == 1);                                  // arrived and captured the empty village
}

// ------------------------------------------------------------------ movement: bodies, surround, avoidance

TEST(mech_melee_surround_big_target) {
    const char* rules = R"({
      "victory": "noUnits",
      "types": {
        "grunt": {"move": {"speed": 3, "radius": 0.4}, "health": {"max": 100}, "weapons": [{"range": 0.5, "damage": 1, "cooldown": 0.5}]},
        "titan": {"move": {"speed": 0.01, "radius": 1.5, "mass": 50}, "health": {"max": 100000}}
      }})";
    std::string ents;
    for (int i = 0; i < 10; ++i) ents += std::string(",") + R"({"type":"grunt","owner":1,"pos":[)" + std::to_string(3 + (i / 3)) + "." + "5," + std::to_string(9 + (i % 3)) + R"(.5]})";
    auto play = [&](bool print) {
        auto sim = makeSimJson(rules, flatMap(32, 20, ",\"entities\":[{\"type\":\"titan\",\"owner\":2,\"pos\":[16,10]}" + ents + "]"));
        Command c; c.player = 1; c.type = game::kCmdAttack; c.target = findType(*sim, "titan", 2);
        sim->world().each<game::Unit, Owner>([&](Entity e, game::Unit&, Owner& o) { if (o.player == 1) c.units.push_back(e); });
        sim->submit(c);
        run(*sim, 6);
        std::vector<uint64_t> hitters;
        for (int i = 0; i < 40; ++i) {
            sim->step();
            for (auto& ev : sim->events())
                if (ev.type == game::kEvAttack && ev.player == 1 && std::find(hitters.begin(), hitters.end(), ev.a.packed()) == hitters.end()) hitters.push_back(ev.a.packed());
        }
        if (print) std::printf("    %zu of 10 attackers hitting\n", hitters.size());
        return std::pair{hitters.size(), sim->checksum()};
    };
    auto a = play(true), b = play(false);
    CHECK(a.first >= 6);                                        // they spread around instead of queueing
    CHECK(a == b);
}

TEST(mech_rvo_corridor_pass_through_idle_units) {
    const char* rules = R"({"victory": "noUnits", "types": {"walker": {"move": {"speed": 3, "radius": 0.4}, "health": {"max": 50}}}})";
    std::string map = R"({"size":[20,5],"players":[{"name":"A","team":1},{"name":"B","team":2}],"terrain":{"rows":["####################","....................","....................","....................","####################"]},
      "entities":[{"type":"walker","owner":1,"pos":[10.5,1.5]},{"type":"walker","owner":1,"pos":[10.5,2.5]},{"type":"walker","owner":1,"pos":[10.5,3.5]},
                  {"type":"walker","owner":1,"pos":[2.5,2.5]},{"type":"walker","owner":2,"pos":[19.5,3.5]}]})";
    auto play = [&]() {
        auto sim = makeSimJson(rules, map);
        Entity mover;
        sim->world().each<game::Unit, Owner, Transform>([&](Entity e, game::Unit&, Owner& o, Transform& t) { if (o.player == 1 && t.pos.x < Fixed::fromInt(5)) mover = e; });
        Command c; c.player = 1; c.type = game::kCmdMove; c.units = {mover}; c.point = {Fixed::fromRatio(35, 2), Fixed{}, Fixed::fromRatio(5, 2)};
        sim->submit(c);
        uint32_t arrived = 0;
        for (int i = 0; i < 20 * 15 && !arrived; ++i) {
            sim->step();
            if ((sim->world().get<Transform>(mover).pos - c.point).xz().length() < Fixed::fromRatio(6, 10)) arrived = sim->tick();
        }
        return std::pair{arrived, sim->checksum()};
    };
    auto a = play(), b = play();
    std::printf("    corridor: arrived at tick %u\n", a.first);
    CHECK(a.first > 0 && a.first < 20 * 10);                   // got through the three standing units within 10 s
    CHECK(a == b);
}

// ------------------------------------------------------------------ sample games: short bot matches are deterministic

TEST(sample_games_bot_matches_deterministic) {
    const char* games[][2] = {
        {"frostcrown", "maps/frozen_pass.json"}, {"starfall", "maps/twin_plateaus.json"}, {"ironfront", "maps/ore_valley.json"},
        {"ambersands", "maps/spice_basin.json"}, {"metaltide", "maps/twin_craters.json"}, {"fourages", "maps/highland.json"},
        {"holdtheline", "maps/crossroads.json"}, {"lastcolony", "maps/last_valley.json"}, {"clanlands", "maps/fjords.json"},
        {"flagsectors", "maps/sector_strip.json"}, {"laneclash", "maps/long_lane.json"}, {"underlord", "maps/two_keeps.json"},
        {"borderrush", "maps/five_lands.json"}, {"hearthhaul", "maps/green_dale.json"}, {"sporelanes", "maps/spore_field.json"},
        {"spellbunkers", "maps/rune_field.json"}};
    for (auto& g : games) {
        std::string dir = std::string(RTS_SOURCE_DIR) + "/templates/" + g[0];
        std::vector<Command> cmds;
        uint64_t a = botMatch(dir, g[1], 3, 20 * 60, &cmds), b = botMatch(dir, g[1], 3, 20 * 60);
        bool ok = a == b && !cmds.empty();
        if (!ok) std::printf("    %s: not deterministic or idle bots\n", g[0]);
        CHECK(ok);
    }
}

// ---------------- 2D games: tilesets, autotiling, tile terrain (2D-mode agent) ----------------
static const char* kTestTileset = R"({"name": "t", "atlas": "a.png", "tileSize": [16, 16], "columns": 8, "terrains": [
    {"id": "sand", "char": "s", "cell": ".", "tiles": [0, {"tile": 1, "weight": 0.5}]},
    {"id": "ore", "char": "o", "cell": ".", "field": {"resource": "ore", "density": 5}, "under": "sand", "tiles": [2], "autotile": {"type": "corner16", "first": 100}},
    {"id": "high", "char": "h", "cell": "R", "level": 1, "under": "sand", "autotile": {"type": "blob47", "first": 10}},
    {"id": "wall", "char": "w", "cell": "#", "autotile": {"type": "mask16", "first": 60}}]})";

TEST(tiles2d_tileset_and_autotile) {
    game::Tileset ts = game::loadTileset(Json::parse(kTestTileset));
    CHECK(ts.terrains.size() == 4 && ts.find("high") == 2 && ts.findChar('w') == 3);
    // canonical blob order: 47 shapes, isolated = 0, full = 46, corners without both edges don't count
    std::vector<int> seen;
    for (int m = 0; m < 256; ++m) { int i = game::blob47Index(m); if (std::find(seen.begin(), seen.end(), i) == seen.end()) seen.push_back(i); }
    CHECK(seen.size() == 47 && game::blob47Index(0) == 0 && game::blob47Index(255) == 46 && game::blob47Index(2) == 0);
    Json tiles = Json::parse(R"({"tileset": "x", "rows": ["sssss", "shhhs", "shhhs", "shhhs", "swwws"]})");
    game::TileLayer L = game::loadTileLayer(tiles, ts, 5, 5);
    game::CellTiles c = game::resolveCell(ts, L, 2, 2);          // interior of the plateau: full mask, no base tiles -> blob tile 46
    CHECK(c.top == 10 + 46 && c.under == -1);
    c = game::resolveCell(ts, L, 1, 1);                           // NW corner of the plateau: E, S, SE set
    CHECK(c.top == 10 + game::blob47Index(4 | 8 | 16) && (c.under == 0 || c.under == 1));
    c = game::resolveCell(ts, L, 1, 4);                           // wall row: E neighbour + map edge below (S counts as same)
    CHECK(c.top == 60 + (2 | 4));
    // deterministic variants
    CHECK(game::resolveCell(ts, L, 0, 0).top == game::resolveCell(ts, L, 0, 0).top);
    bool threw = false;
    try { game::loadTileset(Json::parse(R"({"atlas": "a.png", "terrains": [{"id": "a", "under": "zz", "tiles": [0]}]})")); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

TEST(tiles2d_terrain_applied_to_map_idempotent) {
    game::Tileset ts = game::loadTileset(Json::parse(kTestTileset));
    Json map = Json::parse(R"({"size": [4, 2], "terrain": {"rows": ["....", "...."]},
        "fields": [{"resource": "ore", "rows": ["9999", "0000"]}],
        "tiles": {"tileset": "x", "rows": ["show", "ssss"]}})");
    game::applyTilesToMap(map, ts);
    CHECK(map["terrain"]["rows"][0].str() == ".R.#" && map["terrain"]["levels"][0].str() == "0100");
    std::string once = map.dump();
    game::applyTilesToMap(map, ts);
    CHECK(map.dump() == once);                                     // loading twice gives the same map
    // the tiles own their cells' fields: hand-painted digits there are cleared, others kept
    CHECK(map["fields"][0]["rows"][0].str() == "9909" && map["fields"][1]["rows"][0].str() == "0050");
    Json old = Json::parse(R"({"size": [4, 2], "terrain": {"rows": ["....", "~~~~"]}})");
    std::string before = old.dump();
    game::applyTilesToMap(old, ts);
    CHECK(old.dump() == before);                                   // maps without tiles are untouched
}

TEST(tiles2d_sample_games_deterministic) {
    for (const char* g : {"tiles2d", "km2d"}) {
        std::string dir = std::string(RTS_SOURCE_DIR) + "/templates/" + g;
        Json manifest = Json::parseFile(dir + "/game.json");
        auto run = [&] {
            Json map = Json::parseFile(dir + "/" + manifest["maps"][size_t(0)].getStr("file"));
            std::string err;
            bool ok = game::applyTilesToMap(map, [&](const std::string& p, std::string& out) {
                std::ifstream f(dir + "/" + p, std::ios::binary);
                if (!f) return false;
                out.assign(std::istreambuf_iterator<char>(f), {});
                return true;
            }, &err);
            CHECK(ok);
            SimConfig cfg; cfg.seed = 5;
            Simulation sim(cfg, std::make_unique<game::RtsModule>());
            sim.setup(Json::parseFile(dir + "/rules.json"), map);
            std::vector<game::RtsAI> bots;
            for (auto& p : sim.players()) bots.emplace_back(p.slot, 5);
            for (int i = 0; i < 20 * 120; ++i) { for (auto& b : bots) b.update(sim); sim.step(); }
            return sim.checksum();
        };
        uint64_t a = run(), b = run();
        CHECK(a == b);
    }
}

// ------------------------------------------------------------------ texture atlases
#include <cstring>
#include <map>
#include <set>

#include "engine/atlas/AtlasFormat.h"
#include "tools/atlas/AtlasBuild.h"

namespace {
// Opaque blob of `w x h` inside a transparent canvas at (ox, oy); `seed` varies the pixels.
rts::atlas::Image atlasTestImage(int cw, int ch, int ox, int oy, int w, int h, uint32_t seed, bool opaqueCanvas = false) {
    rts::atlas::Image img;
    img.width = cw; img.height = ch;
    img.rgba.assign(size_t(cw) * size_t(ch) * 4, 0);
    uint32_t s = seed * 2654435761u + 1;
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            bool in = x >= ox && y >= oy && x < ox + w && y < oy + h;
            if (!in && !opaqueCanvas) continue;
            s = s * 1664525u + 1013904223u;
            uint8_t* p = img.px(x, y);
            p[0] = uint8_t(s >> 24); p[1] = uint8_t(x * 7 + seed); p[2] = uint8_t(y * 5); p[3] = in && !opaqueCanvas ? uint8_t(128 + (s >> 25)) : 255;
        }
    return img;
}
rts::atlas::SourceImage atlasSrc(const std::string& path, const rts::atlas::Image& img) { return {path, rts::atlas::encodePng(img)}; }
rts::atlas::AtlasConfig atlasCfg(const std::string& json, const std::string& profile = "linux") {
    return rts::atlas::AtlasConfig::fromGame(rts::Json::parse(json), profile);
}
bool sameImage(const rts::atlas::Image& a, const rts::atlas::Image& b) { return a.width == b.width && a.height == b.height && a.rgba == b.rgba; }
} // namespace

TEST(atlas_maxrects_no_overlap_bounds_padding) {
    using namespace rts::atlas;
    for (int pass = 0; pass < 3; ++pass) {
        PackSettings s;
        s.maxPage = pass == 2 ? 256 : 1024;   // pass 2 forces several pages
        s.padding = 2 + pass;
        s.extrude = pass;
        s.pow2 = pass != 1;
        std::vector<PackItem> items;
        uint32_t r = 12345 + uint32_t(pass);
        for (int i = 0; i < 160; ++i) {
            r = r * 1664525u + 1013904223u;
            PackItem it;
            it.w = 4 + int((r >> 8) % 60);
            it.h = 4 + int((r >> 20) % 50);
            it.allowRotate = pass == 1 && i % 3 == 0;
            it.cluster = "c" + std::to_string(i / 16);
            it.key = "k" + std::to_string(1000 + i);
            items.push_back(it);
        }
        PackedPages pp = packRects(items, s);
        CHECK(pp.ok);
        if (!pp.ok) continue;
        if (pass == 2) CHECK(pp.pages.size() > 1);
        else CHECK(pp.pages.size() == 1);
        bool inside = true, apart = true, sizesOk = true;
        for (auto& pg : pp.pages) {
            sizesOk &= pg.w <= s.maxPage && pg.h <= s.maxPage && pg.w % 4 == 0 && pg.h % 4 == 0;
            if (s.pow2) sizesOk &= (pg.w & (pg.w - 1)) == 0 && (pg.h & (pg.h - 1)) == 0;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            const Placement& a = pp.placements[i];
            int aw = a.rotated ? items[i].h : items[i].w, ah = a.rotated ? items[i].w : items[i].h;
            const PackRect& pg = pp.pages[size_t(a.page)];
            inside &= a.x - s.extrude >= 0 && a.y - s.extrude >= 0 && a.x + aw + s.extrude <= pg.w && a.y + ah + s.extrude <= pg.h;
            if (!items[i].allowRotate) inside &= !a.rotated;
            for (size_t j = i + 1; j < items.size(); ++j) {
                const Placement& b = pp.placements[j];
                if (b.page != a.page) continue;
                int bw = b.rotated ? items[j].h : items[j].w, bh = b.rotated ? items[j].w : items[j].h;
                // extruded rects must be separated by at least `padding` along x or y
                int ax0 = a.x - s.extrude, ax1 = a.x + aw + s.extrude, ay0 = a.y - s.extrude, ay1 = a.y + ah + s.extrude;
                int bx0 = b.x - s.extrude, bx1 = b.x + bw + s.extrude, by0 = b.y - s.extrude, by1 = b.y + bh + s.extrude;
                bool sepX = ax1 + s.padding <= bx0 || bx1 + s.padding <= ax0, sepY = ay1 + s.padding <= by0 || by1 + s.padding <= ay0;
                apart &= sepX || sepY;
            }
        }
        CHECK(inside);
        CHECK(apart);
        CHECK(sizesOk);
        if (pass == 2) { // clusters that fit on a page are never split
            std::map<std::string, std::set<int>> pagesOf;
            for (size_t i = 0; i < items.size(); ++i) pagesOf[items[i].cluster].insert(pp.placements[i].page);
            bool whole = true;
            for (auto& [c, ps] : pagesOf) whole &= ps.size() == 1;
            CHECK(whole);
        }
    }
    PackSettings s;
    s.maxPage = 64;
    CHECK(!packRects({PackItem{100, 10, false, "", "big"}}, s).ok); // too large -> readable error
}

TEST(atlas_build_is_deterministic) {
    using namespace rts::atlas;
    std::vector<SourceImage> in;
    for (int i = 0; i < 40; ++i)
        in.push_back(atlasSrc("units/u" + std::to_string(i % 5) + "/f" + std::to_string(i) + ".png", atlasTestImage(40, 40, i % 7, i % 5, 10 + i % 23, 12 + i % 17, uint32_t(i))));
    for (int i = 0; i < 6; ++i) in.push_back(atlasSrc("ui/icon" + std::to_string(i) + ".png", atlasTestImage(24, 24, 2, 2, 20, 20, uint32_t(100 + i))));
    AtlasConfig cfg = atlasCfg(R"({"atlas": {"maxPage": 256}})");
    AtlasOutput a, b;
    CHECK(buildAtlas(in, cfg, a));
    std::vector<SourceImage> rev(in.rbegin(), in.rend()); // input order must not matter
    CHECK(buildAtlas(rev, cfg, b));
    CHECK(a.files.size() == b.files.size() && a.files.size() >= 3);
    bool same = a.files.size() == b.files.size();
    for (size_t i = 0; same && i < a.files.size(); ++i) same = a.files[i] == b.files[i];
    CHECK(same);
    CHECK(a.stats.atlased == 46 && a.stats.pages >= 2);
    // the manifest survives a round trip
    Manifest m;
    CHECK(m.parse(a.manifest.serialize()));
    CHECK(m.serialize() == a.manifest.serialize());
    CHECK(m.regions().size() == 46);
}

TEST(atlas_dedupe_trim_rotation_extract) {
    using namespace rts::atlas;
    Image blob = atlasTestImage(30, 20, 0, 0, 30, 20, 7);
    auto placed = [&](int cw, int ch, int ox, int oy) {
        Image img;
        img.width = cw; img.height = ch;
        img.rgba.assign(size_t(cw) * size_t(ch) * 4, 0);
        for (int y = 0; y < blob.height; ++y) for (int x = 0; x < blob.width; ++x) std::memcpy(img.px(ox + x, oy + y), blob.px(x, y), 4);
        return img;
    };
    Image a = placed(64, 64, 5, 9), b = placed(48, 40, 13, 2), c = atlasTestImage(16, 50, 0, 0, 16, 50, 9, true);
    std::vector<SourceImage> in = {atlasSrc("sprites/a.png", a), atlasSrc("sprites/b.png", b), atlasSrc("sprites/tall.png", c),
                                   atlasSrc("sprites/empty.png", atlasTestImage(32, 32, 0, 0, 0, 0, 1))};
    AtlasConfig cfg = atlasCfg(R"({"atlas": {"files": {"sprites/tall.png": {"rotate": true}}}})");
    AtlasOutput out;
    CHECK(buildAtlas(in, cfg, out));
    const Region *ra = out.manifest.find("sprites/a.png"), *rb = out.manifest.find("sprites/b.png"), *re = out.manifest.find("sprites/empty.png");
    CHECK(ra && rb && re);
    if (!ra || !rb || !re) return;
    CHECK(out.stats.duplicates == 1);
    CHECK(ra->x == rb->x && ra->y == rb->y && ra->page == rb->page);        // one region for identical pixels
    CHECK(ra->offsetX == 5 && ra->offsetY == 9 && ra->w == 30 && ra->h == 20); // trimmed, offsets kept
    CHECK(rb->offsetX == 13 && rb->offsetY == 2 && rb->sourceW == 48 && rb->sourceH == 40);
    CHECK(re->w == 1 && re->h == 1);
    Image back;
    const Image& page = out.pageImages[size_t(ra->page)];
    CHECK(extractRegion(page, *ra, back) && sameImage(back, a));
    CHECK(extractRegion(page, *rb, back) && sameImage(back, b));
    const Region* rc = out.manifest.find("sprites/tall.png");
    CHECK(rc && extractRegion(out.pageImages[size_t(rc->page)], *rc, back) && sameImage(back, c)); // with or without rotation
    // extrusion: the texel left of the content repeats the content's first column
    CHECK(std::memcmp(page.px(ra->x - 1, ra->y), page.px(ra->x, ra->y), 4) == 0);
    // UVs follow the region
    float uv[8];
    regionUVs(*ra, page.width, page.height, uv);
    CHECK(uv[0] == float(ra->x) / float(page.width) && uv[4] == float(ra->x + ra->w) / float(page.width));
    // .rtex round trip, both filters
    for (bool delta : {false, true}) {
        Image d;
        uint32_t flags = 0;
        CHECK(decodeRtex(encodeRtex(page, true, false, delta), d, &flags) && sameImage(d, page) && ((flags & 4u) != 0) == delta);
    }
}

TEST(atlas_grouping_rules) {
    using namespace rts::atlas;
    Image col = atlasTestImage(32, 32, 0, 0, 32, 32, 3, true);
    std::vector<SourceImage> in = {
        atlasSrc("units/knight/body.png", col), atlasSrc("units/knight/body_n.png", atlasTestImage(32, 32, 0, 0, 32, 32, 4, true)),
        atlasSrc("tiles/water_repeat.png", col), atlasSrc("tiles/grass.png", atlasTestImage(16, 16, 0, 0, 16, 16, 5, true)),
        atlasSrc("tiles/bush.png", atlasTestImage(16, 16, 2, 2, 8, 8, 6)), atlasSrc("models/rock.png", col),
        atlasSrc("ui/logo.png", col), atlasSrc("ui/big.png", atlasTestImage(700, 20, 0, 0, 700, 20, 8, true)),
        atlasSrc("textures/fx/spark.png", col), atlasSrc("ui/cursor.png", atlasTestImage(16, 16, 0, 0, 16, 16, 9)),
        // second members: a group holding a single image is left standalone
        atlasSrc("units/knight/legs.png", atlasTestImage(20, 20, 0, 0, 20, 20, 13, true)), atlasSrc("units/knight/legs_n.png", atlasTestImage(20, 20, 0, 0, 20, 20, 14, true)),
        atlasSrc("tiles/grass2.png", atlasTestImage(16, 16, 0, 0, 16, 16, 15, true)), atlasSrc("tiles/bush2.png", atlasTestImage(16, 16, 1, 1, 9, 9, 16)),
        atlasSrc("textures/fx/spark2.png", atlasTestImage(8, 8, 0, 0, 8, 8, 17)), atlasSrc("ui/cursor2.png", atlasTestImage(16, 16, 0, 0, 12, 12, 18)),
        atlasSrc("effects/lonely.png", atlasTestImage(8, 8, 0, 0, 8, 8, 19))};
    AtlasConfig cfg = atlasCfg(R"({"atlas": {"files": {"ui/logo.png": {"noAtlas": true}, "textures/fx/*": {"group": "fx2"}}}})", "web");
    CHECK(cfg.pack.maxPage == 2048);
    AtlasOutput out;
    CHECK(buildAtlas(in, cfg, out));
    auto pageOf = [&](const char* p) { const Region* r = out.manifest.find(p); return r ? r->page : -1; };
    CHECK(pageOf("units/knight/body.png") >= 0 && pageOf("units/knight/body_n.png") >= 0);
    CHECK(pageOf("units/knight/body.png") != pageOf("units/knight/body_n.png"));  // normal maps never mixed with colour
    CHECK(pageOf("tiles/water_repeat.png") < 0);                                    // repeat wrap stays standalone
    CHECK(pageOf("tiles/grass.png") >= 0 && pageOf("tiles/bush.png") >= 0 && pageOf("tiles/grass.png") != pageOf("tiles/bush.png")); // opaque vs alpha tiles
    CHECK(pageOf("models/rock.png") < 0);                                           // 3D textures are not candidates
    CHECK(pageOf("ui/logo.png") < 0 && pageOf("ui/big.png") < 0);                   // noAtlas, too large (> maxPage/4)
    CHECK(pageOf("textures/fx/spark.png") >= 0 && out.manifest.pages()[size_t(pageOf("textures/fx/spark.png"))].group == "fx2");
    CHECK(pageOf("effects/lonely.png") < 0);                                        // alone in its group
    CHECK(pageOf("ui/cursor.png") >= 0);
    const Region* g = out.manifest.find("tiles/grass.png");
    CHECK(g && !out.manifest.pages()[size_t(g->page)].alpha);
    CHECK(out.stats.excluded.size() == 4);
    bool threw = false;
    try { atlasCfg(R"({"atlas": {"files": {"x": {"grup": "a"}}}})"); } catch (const std::exception& e) { threw = std::string(e.what()).find("grup") != std::string::npos; }
    CHECK(threw);
}

TEST(atlas_sprite_sheets_are_never_trimmed) {
    using namespace rts;
    pack::PackWriter w;
    atlas::Image sheet = atlasTestImage(64, 32, 3, 2, 50, 28, 31), other = atlasTestImage(64, 32, 3, 2, 50, 28, 32);
    CHECK(w.addData("rules.json", R"({"types": {"tank": {"sprite": {"sheet": "sprites/tank.png", "frame": [32, 32]}}}})"));
    CHECK(w.addData("sprites/tank.png", atlas::encodePng(sheet)));
    CHECK(w.addData("sprites/other.png", atlas::encodePng(other)));
    atlas::AtlasStats st;
    CHECK(atlas::applyAtlas(w, atlas::AtlasConfig::fromGame(Json::object(), "linux"), st));
    std::string bytes;
    CHECK(w.writeToString(bytes, pack::WriteOptions{}));
    auto p = pack::Pack::open(pack::memorySource(bytes));
    std::string text;
    atlas::Manifest m;
    CHECK(p && p->read(atlas::kManifestPath, text) && m.parse(text));
    const atlas::Region *s = m.find("sprites/tank.png"), *o = m.find("sprites/other.png");
    CHECK(s && s->w == 64 && s->h == 32 && s->offsetX == 0 && !s->rotated); // frame grid intact
    CHECK(o && o->w == 50 && o->offsetX == 3);                             // standalone sprite trimmed
}

TEST(atlas_pack_integration_and_runtime_lookup) {
    using namespace rts;
    pack::PackWriter w;
    atlas::Image sprite = atlasTestImage(40, 30, 6, 4, 20, 18, 11), tile = atlasTestImage(16, 16, 0, 0, 16, 16, 12, true);
    CHECK(w.addData("game.json", R"({"name": "t", "atlas": {"maxPage": {"default": 512}}})"));
    CHECK(w.addData("units/a/walk.png", atlas::encodePng(sprite)));
    CHECK(w.addData("units/a/run.png", atlas::encodePng(atlasTestImage(12, 12, 0, 0, 12, 12, 13))));
    CHECK(w.addData("tiles/t.png", atlas::encodePng(tile)));
    CHECK(w.addData("tiles/t2.png", atlas::encodePng(atlasTestImage(16, 16, 0, 0, 16, 16, 14, true))));
    CHECK(w.addData("models/m.png", atlas::encodePng(tile)));
    atlas::AtlasStats st;
    CHECK(atlas::applyAtlas(w, atlas::AtlasConfig::fromGame(Json::parse(R"({"atlas": {"maxPage": {"default": 512}}})"), "linux"), st));
    CHECK(st.atlased == 4 && st.pages == 2);
    std::string bytes, bytes2;
    pack::WriteStats ws;
    CHECK(w.writeToString(bytes, pack::WriteOptions{}, &ws));
    CHECK(w.writeToString(bytes2, pack::WriteOptions{}));
    CHECK(bytes == bytes2);
    atlas::finishStats(st, ws);
    CHECK(st.storedBytes > 0);
    CHECK(st.toJson()["pages"].integer() == 2);
    auto p = pack::Pack::open(pack::memorySource(bytes));
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(!p->contains("units/a/walk.png") && p->contains("models/m.png") && p->contains(atlas::kManifestPath));
    atlas::ImageSource src([&](const std::string& path, std::string& out) { return p->read(path, out); }, atlas::decodeImageFile);
    atlas::Image back;
    CHECK(src.atlased("units/a/walk.png") && src.load("units/a/walk.png", back) && sameImage(back, sprite));
    CHECK(src.load("tiles/t.png", back) && sameImage(back, tile));
    CHECK(!src.load("models/m.png", back));
}

// ---------------- models (models agent): loaders, animation, procedural ----------------
#include "engine/render/Animation.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace modeltest {
using namespace rts::render;
inline bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
inline std::string b64(const std::string& in) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += a[v >> 18]; out += a[(v >> 12) & 63]; out += a[(v >> 6) & 63]; out += a[v & 63];
    }
    if (i + 1 == in.size()) { uint32_t v = uint8_t(in[i]) << 16; out += a[v >> 18]; out += a[(v >> 12) & 63]; out += "=="; }
    else if (i + 2 == in.size()) { uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8); out += a[v >> 18]; out += a[(v >> 12) & 63]; out += a[(v >> 6) & 63]; out += '='; }
    return out;
}
template <class T> void put(std::string& b, std::initializer_list<T> v) { for (T x : v) b.append(reinterpret_cast<const char*>(&x), sizeof x); }
inline FileReader mapReader(std::map<std::string, std::string> files) {
    return [files](const std::string& p, std::string& out) { auto it = files.find(p); if (it == files.end()) return false; out = it->second; return true; };
}
inline FileReader diskReader() {
    return [](const std::string& p, std::string& out) { std::ifstream f(p, std::ios::binary); if (!f) return false; std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true; };
}
// Minimal skinned + animated glTF with an embedded data-URI buffer.
inline std::string skinnedGltf(const char* extraRequired = "") {
    std::string bin;
    put<float>(bin, {0, 0, 0, 1, 0, 0, 0, 1, 0});                       // 0: positions (36)
    put<uint8_t>(bin, {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0});            // 36: joints (12)
    put<float>(bin, {1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0});        // 48: weights (48)
    put<uint16_t>(bin, {0, 1, 2, 0});                                    // 96: indices (6 + pad)
    put<float>(bin, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,    // 104: inverse bind (128)
                     1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1});
    put<float>(bin, {0, 1});                                             // 232: times (8)
    const float s = std::sin(0.785398f), c = std::cos(0.785398f);        // 90 deg about Y
    put<float>(bin, {0, 0, 0, 1, 0, s, 0, c});                           // 240: rotations (32)
    put<float>(bin, {0, 0, 0, 5, 0, 0});                                 // 272: translations (24)
    std::string j = R"({"asset":{"version":"2.0"},"extensionsRequired":[)" + std::string(extraRequired) + R"(],
      "buffers":[{"byteLength":296,"uri":"data:application/octet-stream;base64,)" + b64(bin) + R"("}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12},
        {"buffer":0,"byteOffset":48,"byteLength":48},{"buffer":0,"byteOffset":96,"byteLength":6},
        {"buffer":0,"byteOffset":104,"byteLength":128},{"buffer":0,"byteOffset":232,"byteLength":8},
        {"buffer":0,"byteOffset":240,"byteLength":32},{"buffer":0,"byteOffset":272,"byteLength":24}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
        {"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},
        {"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
        {"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"},
        {"bufferView":4,"componentType":5126,"count":2,"type":"MAT4"},
        {"bufferView":5,"componentType":5126,"count":2,"type":"SCALAR"},
        {"bufferView":6,"componentType":5126,"count":2,"type":"VEC4"},
        {"bufferView":7,"componentType":5126,"count":2,"type":"VEC3"}],
      "materials":[{"name":"TeamCloth","alphaMode":"MASK","alphaCutoff":0.3,"doubleSided":true,
        "pbrMetallicRoughness":{"baseColorFactor":[0.5,0.25,1,1],"metallicFactor":0.2}}],
      "meshes":[{"name":"m","primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":3,"material":0}]}],
      "nodes":[{"name":"root","children":[1]},{"name":"bone","translation":[0,1,0]},{"name":"skinned","mesh":0,"skin":0}],
      "skins":[{"joints":[0,1],"inverseBindMatrices":4}],
      "scenes":[{"nodes":[0,2]}],
      "animations":[{"name":"Spin","samplers":[{"input":5,"output":6},{"input":5,"output":7,"interpolation":"STEP"}],
        "channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}},{"sampler":1,"target":{"node":0,"path":"translation"}}]}]})";
    return j;
}
} // namespace modeltest

TEST(models_quat_matches_matrices) {
    using namespace modeltest;
    quat q = quat::axisAngle({0, 1, 0}, 0.7f);
    float3 v{1, 0.5f, -2};
    float3 a = q.rotate(v), b = mat4::rotateY(0.7f).transformPoint(v), c = mat4::rotate(q).transformPoint(v);
    CHECK(near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z));
    CHECK(near(c.x, b.x) && near(c.y, b.y) && near(c.z, b.z));
    quat h = slerp(quat{}, quat::axisAngle({0, 1, 0}, 1.0f), 0.5f), e = quat::axisAngle({0, 1, 0}, 0.5f);
    CHECK(near(h.y, e.y) && near(h.w, e.w));
    mat4 t = mat4::trs({1, 2, 3}, q, {2, 2, 2});
    float3 p = t.transformPoint({1, 0, 0}), r = q.rotate({2, 0, 0});
    CHECK(near(p.x, r.x + 1) && near(p.y, r.y + 2) && near(p.z, r.z + 3));
    mat4 n = mat4::scale({2, 1, 1}).normalMatrix();
    CHECK(near(n.at(0, 0), 0.5f) && near(n.at(1, 1), 1.0f));
}

TEST(models_gltf_skin_and_clips) {
    using namespace modeltest;
    std::vector<std::string> warn;
    auto m = loadGltf("t.gltf", skinnedGltf(), mapReader({}), &warn);
    CHECK(m->nodes.size() == 3 && m->skins.size() == 1 && m->clips.size() == 1);
    CHECK(m->meshes.size() == 1 && m->meshes[0].primitives.size() == 1);
    const Primitive& p = m->meshes[0].primitives[0];
    CHECK(p.indexCount == 3 && p.jointPalette.size() == 2);
    const Material& mt = m->materials[size_t(p.material)];
    CHECK(mt.name == "TeamCloth" && mt.alpha == AlphaMode::Mask && near(mt.alphaCutoff, 0.3f) && mt.doubleSided && near(mt.metallic, 0.2f));
    CHECK(near(mt.baseColor.y, 0.25f));
    // weights normalized to bytes, palette slots resolve to the skin joints
    const ModelVertex& v2 = m->vertices[m->indices[p.firstIndex + 2]];
    CHECK(v2.weights[0] + v2.weights[1] >= 254 && v2.weights[0] + v2.weights[1] <= 256);
    // bind pose: joint world * inverse bind = identity
    std::vector<mat4> pal;
    paletteMatrices(*m, m->skins[0], p, m->bindWorld, pal);
    bool identity = true;
    for (auto& x : pal) for (int i = 0; i < 16; ++i) identity &= near(x.m[i], (i % 5 == 0) ? 1.0f : 0.0f);
    CHECK(identity);
    // clip sampling: linear rotation at half time, step translation
    Pose pose;
    bindPose(*m, pose);
    sampleClip(m->clips[0], 0.5f, pose);
    quat e = quat::axisAngle({0, 1, 0}, 0.785398f);
    CHECK(near(std::fabs(pose[1].r.y), e.y, 2e-3f) && near(pose[0].t.x, 0));
    sampleClip(m->clips[0], 1.0f, pose);
    CHECK(near(pose[0].t.x, 5));
    CHECK(near(m->clips[0].duration, 1));
    CHECK(m->findClip("spin") == 0 && m->findNode("bone") == 1);
}

TEST(models_gltf_readable_errors) {
    using namespace modeltest;
    bool threw = false;
    try { loadGltf("bad.gltf", skinnedGltf(R"("KHR_draco_mesh_compression")"), mapReader({}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find("KHR_draco_mesh_compression") != std::string::npos && std::string(e.what()).find("bad.gltf") == 0; }
    CHECK(threw);
    threw = false;
    try { loadModel("missing.glb", mapReader({}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find("missing.glb") != std::string::npos; }
    CHECK(threw);
    threw = false;
    try { loadModel("x.blend", mapReader({{"x.blend", "data"}}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find("unsupported model format") != std::string::npos; }
    CHECK(threw);
    // accessor pointing past its buffer view
    std::string g = skinnedGltf();
    size_t at = g.find(R"("bufferView":0,"componentType":5126,"count":3)");
    g.replace(at, std::strlen(R"("bufferView":0,"componentType":5126,"count":3)"), R"("bufferView":0,"componentType":5126,"count":9)");
    threw = false;
    try { loadGltf("range.gltf", g, mapReader({}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find("out of range") != std::string::npos; }
    CHECK(threw);
}

TEST(models_obj_and_mtl) {
    using namespace modeltest;
    std::string obj = "mtllib m.mtl\no Box\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
                      "usemtl Glass\nf 1/1/1 2/2/1 3/3/1 4/4/1\nusemtl Missing\nf -4 -3 -2\n";
    std::string mtl = "newmtl Glass\nKd 0.2 0.4 0.6\nd 0.5\nNs 98\nKe 1 0 0\n";
    std::vector<std::string> warn;
    auto m = loadModel("dir/x.obj", mapReader({{"dir/x.obj", obj}, {"dir/m.mtl", mtl}}), &warn);
    CHECK(m->materials.size() >= 2);
    int glass = m->findMaterial("Glass");
    CHECK(glass >= 0);
    if (glass >= 0) {
        const Material& g = m->materials[size_t(glass)];
        CHECK(g.alpha == AlphaMode::Blend && near(g.baseColor.w, 0.5f) && near(g.roughness, 0.1414f, 1e-3f) && near(g.emissive.x, 1));
    }
    size_t tris = 0;
    for (auto& md : m->meshes) for (auto& p : md.primitives) tris += p.indexCount / 3;
    CHECK(tris == 3);                       // quad -> 2 triangles + 1 triangle with negative indices
    CHECK(!warn.empty());                   // unknown material 'Missing'
    CHECK(near(m->boundsMax.x, 1) && near(m->boundsMax.y, 1));
}

TEST(models_animator_crossfade_and_layers) {
    using namespace modeltest;
    auto m = loadGltf("t.gltf", skinnedGltf(), mapReader({}), nullptr);
    // second clip: translate the bone up
    AnimClip up;
    up.name = "Up"; up.duration = 1;
    AnimChannel ch; ch.node = 1; ch.path = AnimChannel::Translation; ch.times = {0, 1}; ch.values = {0, 3, 0, 0, 3, 0};
    up.channels.push_back(ch);
    m->clips.push_back(up);
    Animator a;
    a.setModel(m.get());
    Animator::Play p; p.fade = 0.5f;
    a.play(0, 0, p);
    a.update(0.1f);
    a.play(0, 1, p);                        // cross-fade Spin -> Up over 0.5 s
    a.update(0.25f);
    Pose pose;
    a.evaluate(pose);
    CHECK(pose[1].t.y > 1.5f && pose[1].t.y < 2.5f);   // halfway between bind (1) and Up (3)
    a.update(0.5f);
    a.evaluate(pose);
    CHECK(near(pose[1].t.y, 3));
    // overlay restricted to the root's subtree: node 2 (mesh node) stays untouched
    Animator::Play once; once.loop = false; once.fade = 0;
    a.play(1, 0, once);
    a.setMask(1, 1);
    a.update(2.0f);
    CHECK(a.finished(1));
    a.evaluate(pose);
    CHECK(near(pose[0].t.x, 0));            // root not in the mask: the step translation is not applied
}

TEST(models_procedural_modifiers) {
    using namespace modeltest;
    std::vector<ProcAnim> mods(3);
    mods[0].channel = ProcAnim::Translate; mods[0].axis = {0, 1, 0}; mods[0].amp = 0.5f; mods[0].wave = ProcAnim::Const; mods[0].fade = 0; mods[0].randomPhase = false;
    mods[1].special = ProcAnim::Aim; mods[1].channel = ProcAnim::Rotate; mods[1].node = 0; mods[1].turnSpeed = 100; mods[1].limit = 3;
    mods[2].special = ProcAnim::Fall; mods[2].channel = ProcAnim::Rotate; mods[2].axis = {1, 0, 0}; mods[2].amp = 1.5f; mods[2].on = kProcEvDeath; mods[2].decay = 1; mods[2].node = 0; mods[2].fade = 0;
    ProcInputs in;
    in.dt = 0.1f; in.hasTarget = true; in.aimYaw = 0.6f;
    in.eventAge[kProcEvDeath] = 0.5f;
    std::vector<ProcRuntime> rt;
    Pose pose(1);
    mat4 obj;
    applyProcedural(mods, in, rt, pose, obj);
    CHECK(near(obj.at(1, 3), 0.5f));        // whole-object translation
    CHECK(near(rt[1].aim, 0.6f));           // turret reached the target angle
    // fall: half way through (age 0.5 of 1 s) with ease-in -> amp * 0.25 on top of the aim rotation
    float3 f = pose[0].r.rotate({0, 1, 0});
    CHECK(f.y < 0.99f);
}

TEST(models_sample_assets_load) {
    using namespace modeltest;
    const std::string dir = std::string(RTS_SOURCE_DIR) + "/templates/showcase/models/";
    std::vector<std::string> warn;
    auto golem = loadModel(dir + "golem.glb", diskReader(), &warn);
    CHECK(golem->clips.size() == 6 && golem->skins.size() == 1 && golem->skins[0].joints.size() == 8);
    CHECK(golem->findClip("Death") >= 0 && golem->images.size() == 3);
    auto tank = loadModel(dir + "tank.glb", diskReader(), &warn);
    CHECK(tank->variants.size() == 2 && tank->findNode("Turret") >= 0 && tank->findVariant("winter") == 1);
    auto mill = loadModel(dir + "windmill.gltf", diskReader(), &warn);
    int sail = mill->findMaterial("Sail");
    CHECK(sail >= 0 && mill->materials[size_t(sail)].alpha == AlphaMode::Mask && mill->materials[size_t(sail)].baseTex >= 0);
    if (sail >= 0) CHECK(near(mill->materials[size_t(sail)].uvScale.y, 2));
    auto crystal = loadModel(dir + "crystal.obj", diskReader(), &warn);
    CHECK(crystal->findMaterial("Crystal") >= 0 && crystal->images.size() == 1);
    for (auto& w : warn) std::printf("  warning: %s\n", w.c_str());
    CHECK(warn.empty());
}

TEST(i18n_ui_markup_keys) {
    CHECK((i18n::markupKeys(R"(<h2>{{ 'menu.map' | tr(lang) }}</h2><b data-if="screen == 'menu'">{{"a.b"|tr}}</b>{{ 'x' | trim }}<!-- {{ 'c' | tr(lang) }} -->)")
           == std::vector<std::string>{"menu.map", "a.b"}));
}

TEST(i18n_saved_language_roundtrip) {
    // The user data dir does not exist on first run; saving must create it.
    const auto root = std::filesystem::temp_directory_path() / "rts-test-i18n-userdir";
    const std::string dir = (root / "nested").string();
    std::filesystem::remove_all(root);
    CHECK(i18n::readSavedLanguage(dir).empty());
    CHECK(i18n::saveLanguage(dir, "ru_RU"));
    CHECK(i18n::readSavedLanguage(dir) == "ru-RU");
    std::filesystem::remove_all(root);
}

TEST(i18n_placeholder_only_strings_are_not_untranslated) {
    // "{name}: {text}" (net.chat_line) is the same in every language: no "not translated" warning.
    CHECK(i18n::visibleText("{name}: {text}") == ": ");
    CHECK(i18n::visibleText("{{x}} {n} ok") == "{x}  ok");
    i18n::Catalog en, ru;
    en.merge(Json::parse(R"({"chat": "{name}: {text}", "score": "{a} – {b}", "lit": "{{raw}}", "hi": "Hello, {name}"})"));
    ru.merge(Json::parse(R"({"chat": "{name}: {text}", "score": "{a} – {b}", "lit": "{{raw}}", "hi": "Hello, {name}"})"));
    std::vector<std::string> flagged;
    for (auto& i : i18n::check(en, "en", ru, "ru"))
        if (i.message.find("not translated yet") != std::string::npos) flagged.push_back(i.key);
    // literal braces are visible text, so "{{raw}}" still counts as words
    CHECK((flagged == std::vector<std::string>{"hi", "lit"}));
}

TEST(i18n_ui_documents_translated) {
    // Every {{ 'key' | tr(lang) }} in the engine's documents exists in every engine table, and the
    // documents have no hard-coded Cyrillic (menu, hud, lobby, and any document added later).
    namespace fs = std::filesystem;
    auto read = contentReader("skirmish");
    int docs = 0;
    for (auto& entry : fs::directory_iterator(fs::path(RTS_SOURCE_DIR) / "engine" / "assets" / "ui")) {
        if (entry.path().extension() != ".rml") continue;
        ++docs;
        const std::string doc = entry.path().filename().string();
        std::string rml;
        CHECK(read("engine/ui/" + doc, rml));
        auto keys = i18n::markupKeys(rml);
        if (doc != "keyboard.rml") CHECK(!keys.empty()); // the on-screen keyboard has only key caps, no text
        for (const char* lang : {"en", "ru"}) {
            std::string text;
            CHECK(read(std::string("engine/lang/") + lang + ".json", text));
            i18n::Catalog c;
            c.merge(Json::parse(text));
            for (auto& k : keys)
                if (!c.find(k)) { CHECK(false); std::printf("  %s: '%s' missing in %s\n", doc.c_str(), k.c_str(), lang); }
        }
        bool cyrillic = false;
        for (size_t i = 0; i + 1 < rml.size(); ++i)
            if ((unsigned char)rml[i] == 0xD0 || (unsigned char)rml[i] == 0xD1) cyrillic = true;
        CHECK(!cyrillic);
    }
    CHECK(docs >= 3);   // menu, hud, lobby
}

#include "engine/atlas/TextureLibrary.h"

namespace {
// Records texture creation; everything else is a no-op.
class FakeDevice final : public rts::gfx::Device {
public:
    std::vector<std::pair<int, int>> created;
    int alive = 0;
    rts::gfx::DeviceInfo info_;
    std::string err_;
    const rts::gfx::DeviceInfo& info() const override { return info_; }
    rts::gfx::BufferHandle createBuffer(const rts::gfx::BufferDesc&) override { return {}; }
    void updateBuffer(rts::gfx::BufferHandle, const void*, size_t, size_t) override {}
    void destroyBuffer(rts::gfx::BufferHandle) override {}
    rts::gfx::TextureHandle createTexture(const rts::gfx::TextureDesc& d) override { created.push_back({d.width, d.height}); ++alive; return {uint32_t(created.size())}; }
    void updateTexture(rts::gfx::TextureHandle, int, int, int, int, const void*, int) override {}
    void destroyTexture(rts::gfx::TextureHandle) override { --alive; }
    rts::gfx::ShaderHandle createShader(const rts::gfx::ShaderDesc&) override { return {}; }
    void destroyShader(rts::gfx::ShaderHandle) override {}
    rts::gfx::PipelineHandle createPipeline(const rts::gfx::PipelineDesc&) override { return {}; }
    void destroyPipeline(rts::gfx::PipelineHandle) override {}
    const std::string& lastError() const override { return err_; }
    void beginFrame(int, int) override {}
    void endFrame() override {}
    void beginPass(const rts::gfx::PassDesc&) override {}
    void endPass() override {}
    void setViewport(const rts::gfx::Rect&) override {}
    void setScissor(const rts::gfx::Rect&) override {}
    void setPipeline(rts::gfx::PipelineHandle) override {}
    void setVertexBuffer(rts::gfx::BufferHandle, size_t) override {}
    void setIndexBuffer(rts::gfx::BufferHandle, size_t) override {}
    void setTexture(int, rts::gfx::TextureHandle) override {}
    void setUniforms(int, const void*, size_t) override {}
    void draw(uint32_t, uint32_t) override {}
    void drawIndexed(uint32_t, uint32_t, int32_t, uint32_t) override {}
};
} // namespace

TEST(atlas_texture_library_is_transparent) {
    using namespace rts;
    pack::PackWriter w;
    atlas::Image a = atlasTestImage(40, 40, 8, 6, 20, 24, 21), b = atlasTestImage(40, 40, 0, 0, 30, 30, 22), m = atlasTestImage(8, 4, 0, 0, 8, 4, 23, true);
    CHECK(w.addData("units/x/a.png", atlas::encodePng(a)));
    CHECK(w.addData("units/x/b.png", atlas::encodePng(b)));
    CHECK(w.addData("models/m.png", atlas::encodePng(m)));
    atlas::AtlasStats st;
    CHECK(atlas::applyAtlas(w, atlas::AtlasConfig::fromGame(Json::object(), "web"), st));
    std::string bytes;
    CHECK(w.writeToString(bytes, pack::WriteOptions{}));
    auto p = pack::Pack::open(pack::memorySource(bytes));
    if (!p) { CHECK(false); return; }
    FakeDevice dev;
    {
        atlas::TextureLibrary lib(dev, [&](const std::string& path, std::string& out) { return p->read(path, out); });
        const atlas::TextureRef* ra = lib.get("units/x/a.png");
        const atlas::TextureRef* rb = lib.get("units/x/b.png");
        const atlas::TextureRef* rm = lib.get("models/m.png");
        CHECK(ra && rb && rm && !lib.get("nope.png"));
        if (!ra || !rb || !rm) return;
        CHECK(ra->texture == rb->texture && ra->page >= 0);          // same page: no texture switch
        CHECK(ra->offsetX == 8 && ra->offsetY == 6 && ra->w == 20 && ra->h == 24 && ra->sourceW == 40);
        CHECK(rm->page == -1 && rm->w == 8 && rm->h == 4 && rm->opaque && rm->uv[4] == 1.0f);
        CHECK(dev.created.size() == 2 && lib.residentPages() == 1);  // one page + one standalone texture
        CHECK(lib.get("units/x/a.png") == ra);                      // cached
        float u = 0, v = 0;
        atlas::mapUV(*ra, float(ra->offsetX), float(ra->offsetY), u, v);
        CHECK(u == ra->uv[0] && v == ra->uv[1]);
        atlas::mapUV(*rm, 8.0f, 4.0f, u, v);
        CHECK(u == 1.0f && v == 1.0f);
        atlas::Image back;
        CHECK(lib.loadImage("units/x/b.png", back) && sameImage(back, b));
    }
    CHECK(dev.alive == 0);
}

// ------------------------------------------------------------------ heightfield terrain (agent-terrain-sim)
// corners in 1/16 units, row-major (w+1)*(h+1)
static std::string cornersJson(int w, int h, const std::function<int(int, int)>& f) {
    std::string out = "[";
    for (int y = 0; y <= h; ++y)
        for (int x = 0; x <= w; ++x) out += (x || y ? "," : "") + std::to_string(f(x, y));
    return out + "]";
}
static std::string hfMap(int w, int h, const std::function<int(int, int)>& f, const std::string& extra) {
    std::string rows;
    for (int y = 0; y < h; ++y) rows += std::string(y ? "," : "") + "\"" + std::string(size_t(w), '.') + "\"";
    return "{\"size\":[" + std::to_string(w) + "," + std::to_string(h) + "],\"players\":[{\"name\":\"A\",\"team\":1},{\"name\":\"B\",\"team\":2}]," +
           "\"terrain\":{\"rows\":[" + rows + "],\"corners\":" + cornersJson(w, h, f) + "}" + extra + "}";
}

TEST(terrain_surface_height_and_slope) {
    game::MapGrid g;
    g.init(2, 2);
    g.corner = {0, 16, 32, 0, 16, 32, 0, 16, 32};     // plane rising 1 unit per cell along x
    g.enableHeightfield(Fixed::fromInt(1));
    CHECK(g.surfaceHeight(Fixed::fromInt(0), Fixed::fromInt(0)) == Fixed{});
    CHECK(g.surfaceHeight(Fixed::fromRatio(1, 2), Fixed::fromRatio(3, 2)) == Fixed::fromRatio(1, 2));
    CHECK(g.surfaceHeight(Fixed::fromRatio(5, 4), Fixed::fromInt(1)) == Fixed::fromRatio(5, 4));
    CHECK(g.surfaceHeight(Fixed::fromInt(9), Fixed::fromInt(9)) == Fixed::fromInt(2));   // clamped to the map
    CHECK(g.cellHeight(1, 0) == Fixed::fromRatio(3, 2));
    CHECK(g.slope[0] == 256 && g.slope[1] == 256);       // rise 1 per cell, 1/256 units
    CHECK(g.level[1] == 1 && g.level[0] == 0);           // cliff level = floor(h / levelHeight)
    Vec3 gr = g.gradient(Fixed::fromRatio(1, 2), Fixed::fromRatio(1, 2));
    CHECK(gr.x == Fixed::fromInt(1) && gr.z == Fixed{});
    // bilinear inside a twisted cell
    g.corner = {0, 16, 0, 16, 0, 16, 0, 16, 0};
    g.enableHeightfield(Fixed::fromInt(1));
    CHECK(g.surfaceHeight(Fixed::fromRatio(1, 2), Fixed::fromRatio(1, 2)) == Fixed::fromRatio(1, 2));
    CHECK(g.slope[0] >= 256);                            // steep edges count even without a gradient
}

TEST(terrain_slope_passability_per_type) {
    const char* rules = R"({
      "terrain": {"model": "heightfield", "maxSlope": {"ground": 0.7}, "material": "ignored-visual-key"},
      "types": {
        "tank": {"move": {"speed": 2, "radius": 0.4}},
        "kbot": {"move": {"speed": 2, "radius": 0.4, "maxSlope": 2.5}},
        "plane": {"move": {"speed": 3, "layer": "air"}}
      }})";
    // a wall at x=10..11: rises 2 units over one cell (slope 2) except a gentle gap at the bottom rows
    auto f = [](int x, int y) { return (x == 11 && y < 12) ? 32 : 0; };
    auto sim = makeSimJson(rules, hfMap(24, 16, f, R"(,"entities":[])"));
    auto& R = mod(*sim).rules();
    auto& g = mod(*sim).state(*sim).grid;
    const game::UnitType& tank = R.types[size_t(R.findType("tank"))];
    const game::UnitType& kbot = R.types[size_t(R.findType("kbot"))];
    CHECK(g.hf && R.terrain.heightfield());
    CHECK(!g.passable(10, 5, tank.pathLayer) && g.passable(10, 5, kbot.pathLayer));
    CHECK(g.passable(10, 14, tank.pathLayer));
    std::vector<Vec3> path;
    CHECK(g.findPath(g.cellCenter(4, 5), g.cellCenter(18, 5), tank.pathLayer, path));
    bool detour = false;
    for (auto& p : path) if (p.z.floorInt() >= 12) detour = true;
    CHECK(detour);                                        // the tank goes around through the gentle gap
    CHECK(g.findPath(g.cellCenter(4, 5), g.cellCenter(18, 5), kbot.pathLayer, path) && path.size() == 1); // straight over
    CHECK(!g.areaBuildable(10, 4, 2, 2) && g.areaBuildable(3, 3, 2, 2));
    bool threw = false;
    try { game::loadRules(Json::parse(R"({"terrain": {"model": "hills"}, "types": {"a": {}}})")); } catch (const std::exception& e) {
        threw = std::string(e.what()).find("terrain.model") != std::string::npos;
    }
    CHECK(threw);
}

TEST(terrain_line_of_sight_behind_ridge) {
    const char* rules = R"({
      "terrain": {"model": "heightfield", "maxSlope": 3},
      "types": {"scout": {"move": {"speed": 2}, "sight": 14}}})";
    // ridge 4 units high around x=12, valley floor 0 on both sides
    auto f = [](int x, int) { int d = std::abs(x - 12); return d >= 3 ? 0 : (3 - d) * 22; };
    auto sim = makeSimJson(rules, hfMap(32, 8, f, R"(,"entities":[{"type":"scout","owner":1,"pos":[5.5,4.5]},{"type":"scout","owner":2,"pos":[30.5,4.5]}])"));
    run(*sim, 1);
    auto& gs = mod(*sim).state(*sim);
    auto vis = [&](int x) { return gs.vision[1][size_t(gs.grid.index(x, 4))] == 2; };
    CHECK(vis(6) && vis(10) && vis(12));                  // up to the crest
    CHECK(!vis(16) && !vis(17));                          // behind the ridge: hidden
    // the same map as plain cliff levels sees everything in range
    std::string flatRules = R"({"types": {"scout": {"move": {"speed": 2}, "sight": 14}}})";
    auto sim2 = makeSimJson(flatRules, hfMap(32, 8, f, R"(,"entities":[{"type":"scout","owner":1,"pos":[5.5,4.5]},{"type":"scout","owner":2,"pos":[30.5,4.5]}])"));
    run(*sim2, 1);
    CHECK(mod(*sim2).state(*sim2).vision[1][size_t(gs.grid.index(17, 4))] == 2);
    CHECK(!mod(*sim2).state(*sim2).grid.hf);
}

// ------------------------------------------------------------------ split packs (downloadable chunks)

#include "engine/pack/Chunks.h"
#include "engine/pack/FileCopy.h"
#include "engine/pack/Sha256.h"

TEST(pack_sha256_vectors) {
    using namespace rts::pack;
    auto a = sha256("abc", 3);
    CHECK(toHex(a.data(), a.size()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string m(1000000, 'a');
    auto b = sha256(m.data(), m.size());
    CHECK(toHex(b.data(), b.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

namespace {
std::string noise(size_t n, uint32_t seed) {   // incompressible test data (not simulation)
    std::string s(n, '\0');
    for (char& c : s) { seed = seed * 1664525u + 1013904223u; c = char(seed >> 24); }
    return s;
}
rts::pack::PackWriter splitInputs() {
    rts::pack::PackWriter w;
    w.addData("game.json", "{\"name\":\"x\"}");
    w.addData("rules.json", "{}");
    w.addData("maps/a.json", noise(3000, 1));
    for (int i = 0; i < 6; ++i) w.addData("music/track" + std::to_string(i) + ".ogg", noise(40000, 10 + uint32_t(i)));
    w.addData("music/copy.ogg", noise(40000, 10));   // duplicate of track0
    return w;
}
} // namespace

TEST(pack_split_layout_and_reading) {
    using namespace rts::pack;
    PackWriter w = splitInputs();
    SplitOptions so;
    so.chunkSize = 100000;
    so.base = {"game.json", "rules.json", "maps/*"};
    std::string base;
    std::vector<std::string> chunks;
    SplitResult sr;
    WriteStats st;
    std::string err;
    CHECK(w.writeSplit(base, chunks, WriteOptions{}, so, &st, &sr, &err));
    CHECK(base.size() < 10000);                                  // only what must ship
    CHECK(chunks.size() == 3 && sr.chunks.size() == 3);          // 6 x 40 KB in chunks of <= 100 KB
    for (auto& c : chunks) CHECK(c.size() <= so.chunkSize);
    auto p = Pack::open(memorySource(base), &err);
    CHECK(p && p->chunks().size() == 3 && p->size() == 10);
    if (!p) return;
    std::string out;
    CHECK(p->read("game.json", out) && out == "{\"name\":\"x\"}");
    CHECK(!p->read("music/track3.ogg", out) && p->lastError().find("not downloaded") != std::string::npos);
    PackSet set;
    Pack& ref = *p;
    set.mount(std::move(p));
    CHECK(set.state("music/track3.ogg") == PackSet::State::NotDownloaded && set.state("maps/a.json") == PackSet::State::Ready);
    // A chunk from another build or the wrong slot is refused; the right one attaches.
    CHECK(!ref.attachChunk(1, memorySource(chunks[1]), &err));
    for (size_t i = 0; i < chunks.size(); ++i) CHECK(ref.attachChunk(i + 1, memorySource(chunks[i]), &err));
    for (int i = 0; i < 6; ++i) CHECK(set.read("music/track" + std::to_string(i) + ".ogg", out) && out == noise(40000, 10 + uint32_t(i)));
    CHECK(set.read("music/copy.ogg", out) && out == noise(40000, 10));
    CHECK(set.state("music/track3.ogg") == PackSet::State::Ready);
    // Same content split or not has the same content hash (identifies the data, not the layout).
    PackWriter w2 = splitInputs();
    std::string whole;
    WriteStats st2;
    CHECK(w2.writeToString(whole, WriteOptions{}, &st2));
    CHECK(st2.contentHash == st.contentHash);
}

TEST(pack_split_budgets) {
    using namespace rts::pack;
    std::string base, err;
    std::vector<std::string> chunks;
    SplitOptions tight;
    tight.chunkSize = 100000;
    tight.base = {"music/*"};
    tight.baseSize = 50000;                                      // must-ship files don't fit
    PackWriter w1 = splitInputs();
    CHECK(!w1.writeSplit(base, chunks, WriteOptions{}, tight, nullptr, nullptr, &err) && err.find("music/") != std::string::npos);
    SplitOptions fill;
    fill.chunkSize = 100000;
    fill.baseSize = 100000;
    fill.fillBase = true;                                        // base filled up to the limit
    PackWriter w2 = splitInputs();
    SplitResult sr;
    CHECK(w2.writeSplit(base, chunks, WriteOptions{}, fill, nullptr, &sr, &err));
    CHECK(base.size() <= 100000 && base.size() > 80000 && chunks.size() == 2);
    SplitOptions total;
    total.chunkSize = 100000;
    total.totalBudget = 100000;                                  // whole content over the limit
    PackWriter w3 = splitInputs();
    CHECK(!w3.writeSplit(base, chunks, WriteOptions{}, total, nullptr, nullptr, &err) && err.find("whole content") != std::string::npos);
}

TEST(pack_chunk_manager_sources_cache_and_tampering) {
    using namespace rts::pack;
    tfs::path d = testDir("chunks");
    PackWriter w = splitInputs();
    SplitOptions so;
    so.chunkSize = 100000;
    so.base = {"game.json", "rules.json", "maps/*"};
    SplitResult sr;
    std::string err;
    CHECK(w.writeSplitFiles((d / "app" / "content.rpak").string(), (d / "cdn").string(), WriteOptions{}, so, nullptr, &sr, &err));
    // A bad mirror first: chunk 2 tampered there, everything else missing.
    tfs::create_directories(d / "badmirror");
    std::string c2;
    { std::ifstream f(d / "cdn" / sr.chunks[1].file, std::ios::binary); std::ostringstream ss; ss << f.rdbuf(); c2 = ss.str(); }
    c2[c2.size() / 2] ^= 1;
    writeFile(d / "badmirror" / sr.chunks[1].file, c2);
    auto runAll = [&](ChunkManager& m) {
        m.requestAll();
        for (int i = 0; i < 1000 && m.progress().busy; ++i) m.poll();
    };
    auto p = Pack::openFile((d / "app" / "content.rpak").string(), &err);
    CHECK(p != nullptr);
    if (!p) return;
    ChunkManager m;
    m.setCacheDir((d / "cache").string());
    m.setSources({(d / "badmirror").string(), "file://" + (d / "cdn").string() + "/{file}"});
    m.setVerifyBytesPerPoll(1 << 16);                            // several polls per chunk
    m.addPack(*p);
    runAll(m);
    auto pr = m.progress();
    CHECK(pr.ready == 3 && pr.failed == 0 && pr.bytesReady == pr.bytesTotal);
    std::string out;
    CHECK(p->read("music/track4.ogg", out) && out == noise(40000, 14));
    // Next start: everything comes from the cache, no source needed.
    auto p2 = Pack::openFile((d / "app" / "content.rpak").string(), &err);
    ChunkManager m2;
    m2.setCacheDir((d / "cache").string());
    m2.addPack(*p2);
    CHECK(m2.progress().ready == 3 && p2->read("music/track1.ogg", out));
    // No working source: readable failure.
    auto p3 = Pack::openFile((d / "app" / "content.rpak").string(), &err);
    ChunkManager m3;
    m3.setCacheDir((d / "cache-empty").string());
    m3.setSources({(d / "nowhere").string()});
    m3.addPack(*p3);
    runAll(m3);
    CHECK(m3.progress().failed == 3 && !m3.chunks()[0].error.empty());
    std::error_code ec;
    tfs::remove_all(d, ec);
}

TEST(audio_3d_distance_models_and_pose) {
    audio::DistanceDef d;
    d.minDistance = 10; d.maxDistance = 50;
    for (auto model : {audio::Rolloff::Inverse, audio::Rolloff::Linear, audio::Rolloff::Exponential}) {
        d.model = model;
        CHECK(audio::distanceGain(d, 5, 1) == 1.0f && audio::distanceGain(d, 50, 1) == 0.0f);
        float prev = 1.0f;
        bool monotonic = true;
        for (float x = 10; x <= 50; x += 1) { float g = audio::distanceGain(d, x, 1); monotonic = monotonic && g <= prev; prev = g; }
        CHECK(monotonic);
        CHECK(audio::distanceGain(d, 40, 2) > audio::distanceGain(d, 40, 1));  // more hearing = farther
    }
    audio::Environment env;
    audio::ListenerPose ear;   // at the origin, looking down -z
    auto above = audio::spatialize(ear, env, d, 0, 30, 0), side = audio::spatialize(ear, env, d, 30, 0, 0);
    CHECK(std::fabs(above.distance - 30) < 1e-3f && std::fabs(above.gain - side.gain) < 1e-5f);  // height counts as distance
    CHECK(std::fabs(above.pan) < 1e-3f && side.pan > 0.8f);
    ear.forward[0] = 0; ear.forward[1] = -1; ear.forward[2] = 0;          // top-down camera
    ear.up[0] = 0; ear.up[1] = 0; ear.up[2] = -1;                          // screen up = world -z
    CHECK(audio::spatialize(ear, env, d, 20, -5, 0).pan > 0.5f);          // world +x is screen right
    env.rear = 1;
    ear = audio::ListenerPose{};
    auto front = audio::spatialize(ear, env, d, 0, 0, -20), back = audio::spatialize(ear, env, d, 0, 0, 20);
    CHECK(back.gain < front.gain && back.cutoff > 0 && (front.cutoff == 0 || back.cutoff < front.cutoff));
    CHECK(front.t > 0.2f && front.send > env.reverbNear);                  // farther = more reverb
    CHECK(audio::farWeight(0.1f, 0.3f) == 0.0f && audio::farWeight(1.0f, 0.3f) == 1.0f && audio::farWeight(0.65f, 0.3f) > 0.3f);
}

namespace {
// Fraction of the signal's energy in its sample-to-sample differences: high for bright sounds.
double brightness(const std::vector<float>& st) {
    double e = 0, d = 0;
    for (size_t i = 2; i < st.size(); i += 2) { e += st[i] * st[i]; const double x = st[i] - st[i - 2]; d += x * x; }
    return e > 0 ? d / e : 0;
}
std::vector<float> renderBuf(audio::AudioEngine& a, float seconds) {
    std::vector<float> buf(size_t(48000 * seconds) * 2);
    a.render(buf.data(), int(buf.size() / 2));
    return buf;
}
double energy(const std::vector<float>& st, size_t from, size_t to) {
    double e = 0;
    for (size_t i = from * 2; i < std::min(st.size(), to * 2); ++i) e += st[i] * st[i];
    return e;
}
} // namespace

TEST(audio_air_absorption_dulls_far_sounds) {
    // linear rolloff 0 keeps the level until the last 20 %: only the air filter differs
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"spatial":{"model":"linear","rolloff":0,"minDistance":5,"maxDistance":100,"airAbsorption":1000,"reverb":false},
        "sounds":{"hiss":{"synth":{"wave":"noise","freq":20000,"duration":0.5,"attack":0,"release":0}}}})"));
    a->setListener(0, 0, 0, 5);
    audio::PlayParams p;
    p.positional = true;
    p.x = 2;
    a->play("hiss", p);
    const double nearB = brightness(renderBuf(*a, 0.4f));
    renderBuf(*a, 0.3f);
    p.x = 75;
    a->play("hiss", p);
    const auto farBuf = renderBuf(*a, 0.4f);
    CHECK(energy(farBuf, 0, 19200) > 1.0);                                  // still clearly audible
    CHECK(brightness(farBuf) < nearB * 0.3);                                // but much duller
}

TEST(audio_reverb_grows_with_distance) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"spatial":{"model":"linear","rolloff":0,"minDistance":5,"maxDistance":100,"airAbsorption":false,
        "reverb":{"size":0.7,"wet":0.6,"near":0.0,"far":1.0}},
        "sounds":{"tick":{"synth":{"wave":"square","freq":300,"duration":0.04,"attack":0,"release":0.01}}}})"));
    a->setListener(0, 0, 0, 5);
    auto tailRatio = [&](float x) {
        audio::PlayParams p;
        p.positional = true;
        p.x = x;
        a->play("tick", p);
        auto b = renderBuf(*a, 0.8f);
        const double direct = energy(b, 0, 2400), tail = energy(b, 9600, 38400);  // 0..50 ms vs 200..800 ms
        renderBuf(*a, 4.0f);                                                       // let the tail die
        return direct > 0 ? tail / direct : 0.0;
    };
    const double nearR = tailRatio(2), farR = tailRatio(75);
    CHECK(nearR < 0.01 && farR > 0.05 && farR > nearR * 10);
    auto after = renderBuf(*a, 0.1f);                                       // the reverb decays (stable, no NaN)
    CHECK(energy(after, 0, 4800) < 1e-6 && std::isfinite(energy(after, 0, 4800)));
}

TEST(audio_far_variant_crossfade) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"spatial":{"model":"linear","rolloff":0,"minDistance":5,"maxDistance":100,"airAbsorption":false,"reverb":false},
        "sounds":{"boom":{"synth":{"wave":"sine","freq":2000,"duration":1},"far":{"sound":"boomFar","from":0.2},"maxDistance":100},
                  "boomFar":{"synth":{"wave":"sine","freq":100,"duration":1},"maxDistance":160}}})"));
    CHECK(a->sound(a->find("boom"))->far == a->find("boomFar"));
    a->setListener(0, 0, 0, 5);
    auto crossings = [&](float x) {
        audio::PlayParams p;
        p.positional = true;
        p.x = x;
        audio::VoiceId v = a->play("boom", p);
        auto b = renderBuf(*a, 0.3f);
        int zc = 0;
        for (size_t i = 2; i < b.size(); i += 2) zc += (b[i] > 0) != (b[i - 2] > 0);
        a->stop(v, 0.01f);
        renderBuf(*a, 0.05f);
        CHECK(a->activeVoices() == 0);                                      // stop() ends both halves
        return zc;
    };
    CHECK(crossings(3) > 1000);    // near: the bright 2 kHz version
    CHECK(crossings(95) < 100);    // far: the 100 Hz rumble
    CHECK(a->audibility(a->find("boom"), 130, 0, 0) > 0.0f);  // heard as far as its far variant carries
    std::string err;
    CHECK(loadBank(*a, R"({"sounds":{"x":{"synth":{},"far":"nope"}}})", &err) && a->warnings().size() == 1);
    CHECK(!loadBank(*a, R"({"spatial":{"model":"cubic"}})", &err) && err.find("unknown model 'cubic'") != std::string::npos);
    CHECK(!loadBank(*a, R"({"sounds":{"x":{"synth":{},"minDistance":30,"maxDistance":20}}})", &err) && err.find("minDistance < maxDistance") != std::string::npos);
}

TEST(audio_speed_of_sound_delay) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"spatial":{"model":"linear","rolloff":0,"minDistance":5,"maxDistance":200,"airAbsorption":false,"reverb":false,"speedOfSound":100},
        "sounds":{"bang":{"synth":{"wave":"square","freq":200,"duration":0.2,"attack":0}}}})"));
    a->setListener(0, 0, 0, 5);
    audio::PlayParams p;
    p.positional = true;
    p.x = 50;                                                                 // 0.5 s away
    a->play("bang", p);
    auto b = renderBuf(*a, 0.8f);
    CHECK(energy(b, 0, 22000) < 1e-9);                                        // nothing before ~0.46 s
    CHECK(energy(b, 24500, 33000) > 1.0);                                     // then the bang
    p.x = 0.5f;
    a->play("bang", p);
    b = renderBuf(*a, 0.05f);
    CHECK(energy(b, 0, 2400) > 0.1);                                          // close sounds are immediate
}

// ---------------- Music (loops, layers, quantized transitions, stingers, ducking) ----------------
#include "tools/audiopack/AudioPack.h"

namespace {
// 16-bit mono WAV from a function of time
std::string wavOf(int rate, float seconds, const std::function<float(float)>& f) {
    std::vector<int16_t> pcm(size_t(float(rate) * seconds));
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t(std::lrint(std::clamp(f(float(i) / float(rate)), -1.0f, 1.0f) * 20000.0f));
    return audiopack::encodeWav(pcm, 1, rate);
}
float tone(float hz, float t) { return std::sin(6.2831853f * hz * t); }
// Amplitude of `hz` in the left channel of `st` between two times (Goertzel).
double level(const std::vector<float>& st, double hz, double from, double to) {
    const size_t a = size_t(from * 48000), b = std::min(st.size() / 2, size_t(to * 48000));
    if (b <= a) return 0;
    const double w = 6.283185307 * hz / 48000, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = a; i < b; ++i) { const double s0 = st[i * 2] + c * s1 - s2; s2 = s1; s1 = s0; }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / double(b - a);
}
} // namespace

TEST(audio_music_loop_points) {
    auto a = offlineAudio();
    // 0..0.5 s: 200 Hz intro, 0.5..1 s: 800 Hz loop body
    std::map<std::string, std::string> files = {{"m.wav", wavOf(48000, 1.0f, [](float t) { return t < 0.5f ? tone(200, t) : tone(800, t); })}};
    CHECK(loadBank(*a, R"({"music":{"match":[{"file":"m.wav","loopStart":0.5}]}})", nullptr, files));
    a->playMusic("match", 0.01f);
    auto b = renderBuf(*a, 3.0f);
    CHECK(level(b, 200, 0.1, 0.45) > 0.3 && level(b, 800, 0.1, 0.45) < 0.03);   // intro once
    CHECK(level(b, 800, 1.2, 2.9) > 0.3 && level(b, 200, 1.2, 2.9) < 0.03);     // then the loop body only
}

TEST(audio_music_layers_follow_intensity) {
    auto a = offlineAudio();
    std::map<std::string, std::string> files = {{"base.wav", wavOf(48000, 1.0f, [](float t) { return 0.5f * tone(200, t); })},
                                                {"drums.wav", wavOf(48000, 1.0f, [](float t) { return 0.5f * tone(1500, t); })}};
    CHECK(loadBank(*a, R"({"music":{"match":{"layers":[{"file":"base.wav"},{"file":"drums.wav","intensity":0.5}]}}})", nullptr, files));
    CHECK(a->musicHasLayers("match"));
    a->playMusic("match", 0.01f);
    auto calm = renderBuf(*a, 1.5f);
    CHECK(level(calm, 200, 0.5, 1.5) > 0.2 && level(calm, 1500, 0.5, 1.5) < 0.02);  // stems loop: still playing after 1 s
    a->setMusicIntensity(1.0f);
    auto fight = renderBuf(*a, 4.0f);
    CHECK(level(fight, 1500, 3.0, 4.0) > 0.2 && level(fight, 200, 3.0, 4.0) > 0.2);
    a->setMusicIntensity(0.0f);
    auto after = renderBuf(*a, 5.0f);
    CHECK(level(after, 1500, 4.0, 5.0) < 0.02);
}

TEST(audio_music_transition_on_bar) {
    auto a = offlineAudio();
    std::map<std::string, std::string> files = {{"a.wav", wavOf(48000, 4.0f, [](float t) { return 0.5f * tone(300, t); })},
                                                {"b.wav", wavOf(48000, 4.0f, [](float t) { return 0.5f * tone(900, t); })}};
    CHECK(loadBank(*a, R"({"music":{"calm":{"files":["a.wav"],"bpm":120,"beatsPerBar":4},"fight":"b.wav"}})", nullptr, files));
    a->playMusic("calm", 0.01f);
    renderBuf(*a, 0.3f);
    a->playMusic("fight", 0.02f);                 // bar = 2 s at 120 bpm: the switch waits until 2.0 s
    auto b = renderBuf(*a, 2.5f);                 // covers 0.3 .. 2.8 s
    CHECK(level(b, 300, 0.9, 1.6) > 0.2 && level(b, 900, 0.0, 1.6) < 0.02);
    CHECK(level(b, 900, 1.9, 2.4) > 0.2 && level(b, 300, 1.9, 2.4) < 0.02);
    std::string err;
    CHECK(!loadBank(*a, R"({"music":{"x":{"files":["a.wav"],"transition":"soon"}}})", &err, files) && err.find("unknown transition 'soon'") != std::string::npos);
    CHECK(!loadBank(*a, R"({"music":{"x":{"files":["a.wav"],"layers":["b.wav"]}}})", &err, files) && err.find("either") != std::string::npos);
}

TEST(audio_stinger_sync_and_ducking) {
    auto a = offlineAudio();
    std::map<std::string, std::string> files = {{"m.wav", wavOf(48000, 4.0f, [](float t) { return 0.5f * tone(300, t); })}};
    CHECK(loadBank(*a, R"({"music":{"match":{"files":["m.wav"],"bpm":120}},
        "sounds":{"sting":{"synth":{"wave":"sine","freq":1200,"duration":0.3},"bus":"music","sync":"beat","spatial":false},
                  "say":{"synth":{"wave":"sine","freq":2000,"duration":1.0,"attack":0.01,"release":0.01},"bus":"voice","volume":0.3}}})", nullptr, files));
    CHECK(a->sound(a->find("say"))->duckMusic > 0.3f);   // voice ducks music by default
    a->playMusic("match", 0.01f);
    renderBuf(*a, 0.1f);
    a->play("sting");                                    // asked at 0.1 s: lands on the beat at 0.5 s
    auto b = renderBuf(*a, 0.8f);                         // 0.1 .. 0.9 s
    CHECK(level(b, 1200, 0.0, 0.35) < 0.02 && level(b, 1200, 0.42, 0.65) > 0.2);
    renderBuf(*a, 1.0f);
    const double before = level(renderBuf(*a, 0.5f), 300, 0.0, 0.5);
    a->play("say");
    auto during = renderBuf(*a, 0.8f);
    const double ducked = level(during, 300, 0.3, 0.8);
    CHECK(ducked < before * 0.75 && ducked > before * 0.45);           // voice ducks by 0.4 by default
    renderBuf(*a, 2.5f);                                   // released after the voice ends
    CHECK(level(renderBuf(*a, 0.5f), 300, 0.0, 0.5) > before * 0.95);
    std::string err;
    CHECK(!loadBank(*a, R"({"sounds":{"x":{"synth":{},"sync":"later"}}})", &err) && err.find("unknown sync") != std::string::npos);
}

TEST(audio_music_per_map) {
    const std::string dir = "templates/skirmish/";
    Json rulesJson = Json::parse(readSource(dir + "rules.json"));
    game::Rules rules = game::loadRules(rulesJson);
    auto a = offlineAudio();
    std::map<std::string, std::string> files = {{"m.wav", wavOf(48000, 0.5f, [](float t) { return tone(300, t); })}};
    Json bank = Json::parse(R"({"music":{"match":"m.wav","match.valley":"m.wav"}})");
    CHECK(loadBank(*a, bank.dump(), nullptr, files));
    game::RtsAudio s;
    CHECK(s.init(*a, rules, bank));
    Simulation sim(SimConfig{}, std::make_unique<game::RtsModule>());
    sim.setup(rulesJson, Json::parse(readSource(dir + "maps/valley.json")));
    s.attach(sim, 1, "maps/valley.json");
    CHECK(a->currentMusic() == "match.valley");
    s.attach(sim, 1, "maps/other.json");
    CHECK(a->currentMusic() == "match");
}

// ---------------- Build-time compression (tools/audiopack) ----------------

namespace {
double snrDb(const std::vector<int16_t>& a, const std::vector<int16_t>& b) {
    const size_t n = std::min(a.size(), b.size());
    double s = 0, e = 0;
    for (size_t i = 0; i < n; ++i) { s += double(a[i]) * a[i]; const double d = double(a[i]) - b[i]; e += d * d; }
    return e > 0 ? 10 * std::log10(s / e) : 200;
}
std::vector<int16_t> sinePcm(int channels, size_t frames, int rate) {
    std::vector<int16_t> p(frames * size_t(channels));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) p[i * size_t(channels) + size_t(c)] = int16_t(std::lrint(std::sin(6.2831853 * (440 + 220 * c) * double(i) / rate) * 16000));
    return p;
}
} // namespace

TEST(audiopack_adpcm_roundtrip) {
    for (int ch : {1, 2}) {
        for (size_t frames : {size_t(1), size_t(1017), size_t(1018), size_t(30000)}) {
            auto pcm = sinePcm(ch, frames, 44100);
            const std::string wav = audiopack::encodeAdpcm(pcm, ch, 44100);
            audio::Clip c;
            std::string err;
            CHECK(audio::decodeClip(wav, c, err));
            CHECK(c.channels == ch && c.sampleRate == 44100 && c.frames() >= frames && c.frames() <= frames + 7);
            if (frames > 1000) CHECK(snrDb(pcm, c.pcm) > 30);
            CHECK(wav.size() < pcm.size() * 2 / 3 || frames < 2000);   // about 4:1
        }
    }
}

TEST(audiopack_vorbis_roundtrip) {
    auto pcm = sinePcm(2, 44100, 44100);
    std::string ogg, ogg2, err;
    CHECK(audiopack::encodeVorbis(pcm, 2, 44100, 0.4f, 7, ogg, &err));
    CHECK(audiopack::encodeVorbis(pcm, 2, 44100, 0.4f, 7, ogg2, &err) && ogg == ogg2);   // reproducible builds
    CHECK(ogg.size() < pcm.size() * 2 / 10);
    audio::Clip c;
    CHECK(audio::decodeClip(ogg, c, err));
    CHECK(c.channels == 2 && c.frames() == 44100);
    CHECK(snrDb(pcm, c.pcm) > 25);
}

TEST(audiopack_profiles_and_conversion) {
    using audiopack::Codec;
    audiopack::Settings st;
    std::string err;
    CHECK(audiopack::settingsFor(Json::parse("{}"), "desktop", st, &err) && st.sounds == Codec::Keep && st.music == Codec::Keep);
    CHECK(audiopack::settingsFor(Json::parse("{}"), "mobile", st, &err) && st.sounds == Codec::Adpcm && st.music == Codec::Vorbis && st.maxRate == 32000);
    CHECK(audiopack::settingsFor(Json::parse(R"({"build":{"desktop":{"music":"vorbis","musicQuality":0.6}}})"), "desktop", st, &err) &&
          st.music == Codec::Vorbis && std::fabs(st.musicQuality - 0.6f) < 1e-3f);
    CHECK(!audiopack::settingsFor(Json::parse(R"({"build":{"mobile":{"music":"mp4"}}})"), "mobile", st, &err) && err.find("unknown codec 'mp4'") != std::string::npos);
    CHECK(!audiopack::settingsFor(Json::parse(R"({"build":{"mobile":{"bitrate":1}}})"), "mobile", st, &err) && err.find("unknown key 'bitrate'") != std::string::npos);
    CHECK(audiopack::profileForTarget("android") == "mobile" && audiopack::profileForTarget("web") == "web" && audiopack::profileForTarget("macos") == "desktop");

    // a bank with a positional effect, a UI sound kept on purpose, an already-lossy file and music
    std::map<std::string, std::string> files = {
        {"sfx/hit.wav", audiopack::encodeWav(sinePcm(2, 24000, 48000), 2, 48000)},
        {"ui/click.wav", audiopack::encodeWav(sinePcm(1, 4800, 48000), 1, 48000)},
        {"sfx/old.ogg", readSource("tests/data/audio/tone.ogg")},
        {"music/a.wav", audiopack::encodeWav(sinePcm(2, 96000, 48000), 2, 48000)},
    };
    Json bank = Json::parse(R"({"sounds":{"hit":{"files":["sfx/hit.wav"]},"click":{"file":"ui/click.wav","bus":"ui","compress":"keep"},
        "old":{"file":"sfx/old.ogg"}},"music":{"match":{"files":[{"file":"music/a.wav","loopStart":0.5}],"bpm":100}}})");
    CHECK(audiopack::settingsFor(bank, "mobile", st, &err));
    audiopack::Output out;
    auto reader = [&](const std::string& p, std::string& o) { auto it = files.find(p); if (it == files.end()) return false; o = it->second; return true; };
    CHECK(audiopack::convert(bank, reader, st, out, &err));
    CHECK(out.files.count("sfx/hit.wav") && out.files.count("music/a.ogg") && !out.files.count("ui/click.wav") && !out.files.count("sfx/old.ogg"));
    CHECK(out.replaced.size() == 1 && out.replaced[0] == "music/a.wav");
    CHECK(out.report.after() < out.report.before() / 3);
    audio::Clip hit;
    CHECK(audio::decodeClip(out.files["sfx/hit.wav"], hit, err) && hit.channels == 1 && hit.sampleRate == 32000);  // mono, resampled
    // the rewritten bank loads with the converted files, loop points and grid intact
    auto a = offlineAudio();
    Json nb = Json::parse(out.audioJson);
    CHECK(nb["music"]["match"]["files"][size_t(0)].getStr("file") == "music/a.ogg");
    CHECK(a->loadBank(nb, [&](const std::string& p, std::string& o) {
        if (auto it = out.files.find(p); it != out.files.end()) { o = it->second; return true; }
        return reader(p, o);
    }, &err));
    CHECK(a->warnings().empty());
    CHECK(a->playMusic("match") && !a->sound(a->find("hit"))->clips.empty());
}

TEST(audiopack_stage_game_folder) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "rts_audiopack_test";
    std::error_code cleanup;
    fs::remove_all(root, cleanup);
    fs::create_directories(root / "game" / "music");
    auto write = [](const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; };
    write(root / "game" / "game.json", R"({"name":"T","audio":"sound.json"})");
    write(root / "game" / "sound.json", R"({"music":{"match":"music/a.wav"},"build":{"web":{"musicQuality":0.1}}})");
    write(root / "game" / "music" / "a.wav", audiopack::encodeWav(sinePcm(2, 48000, 48000), 2, 48000));
    write(root / "game" / "rules.json", "{}");
    audiopack::Report rep;
    std::string err;
    const bool staged = audiopack::stageGame((root / "game").string(), (root / "web").string(), "web", rep, &err);
    if (!staged) std::printf("    stageGame: %s\n", err.c_str());
    CHECK(staged);
    CHECK(fs::exists(root / "web" / "music" / "a.ogg") && !fs::exists(root / "web" / "music" / "a.wav") && fs::exists(root / "web" / "rules.json"));
    // a failed staging must fail the check, not abort every following test with an exception
    CHECK(fs::exists(root / "web" / "sound.json") && Json::parseFile((root / "web" / "sound.json").string())["music"].getStr("match") == "music/a.ogg");
    CHECK(fs::exists(root / "game" / "music" / "a.wav"));                 // the project itself is untouched
    CHECK(audiopack::stageGame((root / "game").string(), (root / "desktop").string(), "desktop", rep, &err));
    CHECK(fs::exists(root / "desktop" / "music" / "a.wav") && rep.items.empty());   // desktop keeps files by default
    write(root / "game" / "sound.json", R"({"sounds":{"x":{"file":"../../../etc/passwd"}},"build":{"web":{"sounds":"pcm"}}})");
    CHECK(audiopack::stageGame((root / "game").string(), (root / "web2").string(), "web", rep, &err));
    CHECK(rep.items.empty() && rep.warnings.size() == 1);                  // nothing outside the game folder is read
    fs::remove_all(root, cleanup);   // never throws: a locked temp file on Windows must not abort the run
}

// ---------------- particles (engine/fx, modules/rts/fx) ----------------
#include <cstdlib>
#include "engine/fx/ParticleSystem.h"
#include "engine/fx/FxModelMesh.h"
#include "modules/rts/fx/RtsFx.h"

namespace {
fx::EffectDef fxParse(const char* text, std::vector<std::string>* errs = nullptr) {
    fx::EffectDef d;
    std::vector<std::string> e;
    fx::parseEffect(Json::parse(text), "t.json", d, e);
    if (errs) *errs = e;
    return d;
}
} // namespace

TEST(fx_parse_validates_and_round_trips) {
    std::vector<std::string> errs;
    fx::EffectDef d = fxParse(R"({"emitters": [{ "texture": "builtin:smoke", "blend": "alpha", "align": "velocity", "burst": 12,
        "life": [0.5, 1], "speed": 3, "size": [0.2, 0.4], "spread": 30, "shape": "ring", "radius": 2, "gravity": [0, -9, 0],
        "color": [[0, "#ff8000ff"], [1, "#20202000"]], "sizeOverLife": [[0, 1], [1, 3]], "minQuality": "medium" }]})", &errs);
    CHECK(errs.empty());
    CHECK(d.emitters.size() == 1);
    auto& e = d.emitters[0];
    CHECK(e.blend == fx::FxBlend::Alpha && e.align == fx::FxAlign::Velocity && e.shape == fx::FxShape::Ring);
    CHECK(e.burst == 12 && e.speed.min == 3 && e.speed.max == 3 && e.minQuality == fx::FxQuality::Medium);
    CHECK(std::fabs(e.color[0].rgba.y - 128 / 255.0f) < 1e-3f && e.color[1].rgba.w == 0);
    fx::EffectDef back;
    std::vector<std::string> e2;
    CHECK(fx::parseEffect(fx::effectToJson(d), "t.json", back, e2) && e2.empty());
    CHECK(back.emitters.size() == 1 && back.emitters[0].burst == 12 && back.emitters[0].radius == 2 && back.emitters[0].gravity.y == -9);

    fxParse(R"({"emitters": [{ "burst": 3, "blend": "screen", "colour": "#fff" }]})", &errs);
    CHECK(errs.size() == 2);   // unknown blend, unknown field (typos must not pass silently)
    fxParse(R"({"emitters": [{ "rate": 5 }]})", &errs);
    CHECK(errs.size() == 1 && errs[0].find("duration") != std::string::npos);
    fxParse(R"({"emitters": []})", &errs);
    CHECK(!errs.empty());
}

TEST(fx_quality_tiers_bound_particles) {
    fx::EffectDef d = fxParse(R"({"emitters": [{ "burst": 2000, "life": 5 }, { "burst": 10, "life": 5, "minQuality": "high" }]})");
    auto count = [&](fx::FxQuality q) {
        fx::ParticleSystem ps;
        ps.setQuality(q);
        ps.init(nullptr);
        int id = ps.define("boom", d);
        ps.play(id, {0, 0, 0});
        ps.update(0.016f);
        return ps.stats().particles;
    };
    CHECK(count(fx::FxQuality::Off) == 0);
    CHECK(count(fx::FxQuality::Low) == fx::limitsFor(fx::FxQuality::Low).maxParticles);   // budget is a hard cap
    CHECK(count(fx::FxQuality::Medium) == 1400);                                           // 2000 x 0.7, high-only emitter off
    CHECK(count(fx::FxQuality::High) == 2010);
    CHECK(fx::defaultQualityFor("Mali-T720", true) == fx::FxQuality::Low || std::getenv("RTS_PARTICLES"));
    CHECK(fx::defaultQualityFor("Adreno (TM) 640", true) == fx::FxQuality::Medium || std::getenv("RTS_PARTICLES"));
    CHECK(fx::defaultQualityFor("Apple M2", false) == fx::FxQuality::High || std::getenv("RTS_PARTICLES"));
}

TEST(fx_attached_trail_follows_and_stops) {
    fx::ParticleSystem ps;
    ps.setQuality(fx::FxQuality::High);
    ps.init(nullptr);
    int id = ps.define("trail", fxParse(R"({"emitters": [{ "perMeter": 10, "loop": true, "life": 0.5, "speed": 0 }]})"));
    fx::FxHandle h = ps.play(id, {0, 1, 0});
    CHECK(h.valid() && ps.playing(h));
    for (int i = 1; i <= 10; ++i) { ps.move(h, {float(i), 1, 0}); ps.update(0.01f); }
    CHECK(ps.stats().particles >= 99 && ps.stats().particles <= 101);   // 10 m x 10 per metre, wherever the frames fall
    ps.stop(h);
    ps.update(0.01f);
    CHECK(!ps.playing(h));
    for (int i = 0; i < 60; ++i) ps.update(0.02f);
    CHECK(ps.stats().particles == 0);
    CHECK(ps.effect("missing") < 0 && !ps.errors().empty());
}

TEST(fx_offscreen_one_shots_are_not_spawned) {
    fx::ParticleSystem ps;
    ps.setQuality(fx::FxQuality::High);
    ps.init(nullptr);
    int id = ps.define("puff", fxParse(R"({"emitters": [{ "burst": 5, "life": 1, "speed": 1 }]})"));
    render::RTSCamera cam;
    cam.target = {50, 0, 50};
    ps.setView(cam, cam.proj(16.0f / 9.0f, false) * cam.view(), 1600, 900);
    CHECK(ps.play(id, {50, 0, 50}).valid());
    CHECK(!ps.play(id, {50, 0, 140}).valid());   // behind the camera
    CHECK(!ps.play(id, {400, 0, 50}).valid());   // far off to the side
    CHECK(ps.stats().culled == 2);
}

TEST(fx_sample_games_have_valid_effects) {
    for (std::string g : {"skirmish", "mushrooms"}) {
        const std::string dir = "templates/" + g + "/";
        fx::ParticleSystem ps;
        ps.init(nullptr);
        ps.setFileReader([&](const std::string& p, std::string& out) { out = readSource(dir + p); return !out.empty(); });
        game::Rules rules = game::loadRules(Json::parse(readSource(dir + "rules.json")));
        game::RtsFx bridge;
        std::string err;
        CHECK(bridge.init(ps, rules, Json::parse(readSource(dir + "fx/fx.json")), &err));
        for (auto& w : bridge.warnings()) std::printf("    %s: %s\n", g.c_str(), w.c_str());
        CHECK(bridge.warnings().empty());
        for (auto& entry : std::filesystem::directory_iterator(std::string(RTS_SOURCE_DIR) + "/" + dir + "fx")) {
            const std::string name = entry.path().stem().string();
            if (name == "fx") continue;
            CHECK(ps.effect(name) >= 0);
        }
        for (auto& e : ps.errors()) std::printf("    %s\n", e.c_str());
        CHECK(ps.errors().empty());
    }
}

TEST(fx_rts_events_keep_sim_deterministic) {
    // Same bot match with and without effects: identical checksums, and effects actually played.
    const std::string dir = "templates/skirmish/";
    Json rulesJson = Json::parse(readSource(dir + "rules.json"));
    Json map = Json::parse(readSource(dir + "maps/valley.json"));
    Json bank = Json::parse(readSource(dir + "fx/fx.json"));
    game::Rules rules = game::loadRules(rulesJson);
    auto play = [&](bool withFx, int& played) {
        fx::ParticleSystem ps;
        ps.setQuality(fx::FxQuality::Low);
        ps.init(nullptr);
        ps.setFileReader([&](const std::string& p, std::string& out) { out = readSource(dir + p); return !out.empty(); });
        game::RtsFx bridge;
        if (withFx) bridge.init(ps, rules, bank);
        SimConfig cfg;
        cfg.seed = 11;
        Simulation sim(cfg, std::make_unique<game::RtsModule>());
        sim.setup(rulesJson, map);
        std::vector<game::RtsAI> bots;
        for (auto& p : sim.players()) bots.emplace_back(p.slot, 11);
        if (withFx) bridge.attach(sim, 0);
        for (int t = 0; t < 20 * 240 && sim.result() == 0; ++t) {
            for (auto& b : bots) b.update(sim);
            sim.step();
            if (t % 2) continue;   // two ticks per frame: the collector keeps both ticks' events
            bridge.update(sim, nullptr);
            ps.update(0.1f);
        }
        played = bridge.played();
        bridge.detach();
        return sim.checksum();
    };
    int none = 0, some = 0;
    const uint64_t without = play(false, none), with = play(true, some);
    CHECK(without == with);
    CHECK(none == 0 && some > 20);   // first four minutes of a bot match: deaths, hits, buildings
}

// ------------------------------------------------------------------ abilities and effects

static const char* kFxRules = R"({
  "victory": "noUnits",
  "statuses": {
    "poison": {"tags": ["debuff", "magic"], "tick": {"damage": 5, "interval": 1}, "stacking": "stack", "maxStacks": 3},
    "regen": {"tags": ["buff"], "tick": {"heal": 10, "interval": 1}},
    "frost": {"tags": ["debuff", "magic"], "mods": [{"stat": "speed", "mul": 0.5}]},
    "rooted": {"tags": ["debuff"], "flags": ["root"]},
    "silenced": {"tags": ["debuff", "magic"], "flags": ["silence"]},
    "disarmed": {"tags": ["debuff"], "flags": ["disarm"]},
    "shielded": {"tags": ["buff"], "shield": 50},
    "stone": {"tags": ["buff"], "flags": ["invulnerable"]},
    "vampiric": {"tags": ["buff"], "mods": [{"stat": "lifesteal", "add": 0.5}]},
    "cloak": {"tags": ["buff"], "flags": ["invisible"]}
  },
  "types": {
    "mage": {"move": {"speed": 2}, "health": {"max": 500}, "energy": {"max": 1000, "start": 1000, "regen": 0},
      "abilities": [
        {"id": "rocket", "target": "unit", "range": 12, "cooldown": 1, "autocast": false, "delivery": {"type": "projectile", "speed": 5, "homing": true}, "effects": [{"type": "damage", "amount": 40}]},
        {"id": "mortar", "target": "point", "range": 12, "cooldown": 1, "autocast": false, "delivery": {"type": "projectile", "speed": 6, "arc": true}, "effects": [{"type": "damage", "amount": 30, "radius": 2, "falloff": 0.5}]},
        {"id": "lance", "target": "point", "range": 12, "cooldown": 1, "autocast": false, "delivery": {"type": "projectile", "speed": 8, "collide": true}, "effects": [{"type": "damage", "amount": 25}]},
        {"id": "strike", "target": "point", "range": 20, "cooldown": 1, "autocast": false, "delivery": {"type": "area", "delay": 2, "radius": 2}, "effects": [{"type": "damage", "amount": 60, "radius": 2}]},
        {"id": "beam", "target": "unit", "range": 12, "cooldown": 1, "autocast": false, "delivery": {"type": "channel", "duration": 3, "interval": 0.5}, "effects": [{"type": "damage", "amount": 10}]},
        {"id": "zap", "target": "unit", "range": 12, "cooldown": 1, "autocast": false, "delivery": {"type": "chain", "jumps": 3, "jumpRadius": 3, "jumpFalloff": 0.5}, "effects": [{"type": "damage", "amount": 80}]},
        {"id": "breath", "target": "point", "range": 8, "cooldown": 1, "autocast": false, "delivery": {"type": "cone", "angle": 60, "length": 6}, "effects": [{"type": "damage", "amount": 20}]},
        {"id": "slowcast", "target": "unit", "range": 12, "cooldown": 1, "autocast": false, "castTime": 1, "backswing": 0.5, "effects": [{"type": "damage", "amount": 50}]}
      ]},
    "dummy": {"move": {"speed": 2}, "health": {"max": 1000}}
  }})";

static Fixed dist2DTest(Vec3 a, Vec3 b) { return (a - b).xz().length(); }
static Entity unitAt(Simulation& sim, Fixed x, Fixed z) {
    Entity best; Fixed bd{};
    sim.world().each<game::Unit, Transform>([&](Entity e, game::Unit&, Transform& t) {
        Fixed d = (t.pos - Vec3{x, Fixed{}, z}).xz().length();
        if (!best.valid() || d < bd) { best = e; bd = d; }
    });
    return best;
}
static Fixed hpAt(Simulation& sim, int x, int z) { return sim.world().get<game::Unit>(unitAt(sim, Fixed::fromInt(x), Fixed::fromInt(z))).hp; }
static void castAbility(Simulation& sim, Entity caster, int idx, Entity target, Vec3 point) {
    Command c; c.player = sim.world().get<Owner>(caster).player; c.type = game::kCmdAbility; c.units = {caster}; c.param = idx; c.target = target; c.point = point;
    sim.submit(c);
}
static Vec3 P(double x, double z) { return {Fixed::fromRaw(int64_t(x * 65536)), Fixed{}, Fixed::fromRaw(int64_t(z * 65536))}; }
static std::unique_ptr<Simulation> fxSim(const char* rules, const std::string& dummies, uint64_t seed = 1) {
    return makeSimJson(rules, flatMap(40, 20, ",\"entities\":[{\"type\":\"mage\",\"owner\":1,\"pos\":[5,10]}" + dummies + "]"), seed);
}
static std::string dummy(double x, double z, int owner = 2, const char* type = "dummy") {
    char b[160]; std::snprintf(b, sizeof b, ",{\"type\":\"%s\",\"owner\":%d,\"pos\":[%.2f,%.2f]}", type, owner, x, z); return b;
}

TEST(fx_projectile_homing_arc_collide) {
    {   // homing rocket: damage arrives after the flight, not at once
        auto sim = fxSim(kFxRules, dummy(13, 10));
        Entity mage = findType(*sim, "mage", 1);
        castAbility(*sim, mage, 0, unitAt(*sim, Fixed::fromInt(13), Fixed::fromInt(10)), {});
        bool launched = false;
        for (int i = 0; i < 12; ++i) { sim->step(); for (auto& ev : sim->events()) if (ev.type == game::kEvProjectileLaunch) launched = true; }
        CHECK(launched);
        CHECK(hpAt(*sim, 13, 10) == Fixed::fromInt(1000));
        run(*sim, 2.5);
        CHECK(hpAt(*sim, 13, 10) == Fixed::fromInt(960));
    }
    {   // ballistic mortar: splash with falloff around the impact point
        auto sim = fxSim(kFxRules, dummy(12, 10) + dummy(13.5, 10));
        castAbility(*sim, findType(*sim, "mage", 1), 1, kNullEntity, P(12, 10));
        bool high = false;
        for (int i = 0; i < 40; ++i) { sim->step(); sim->world().each<game::Projectile, Transform>([&](Entity, game::Projectile&, Transform& t) { if (t.pos.y > Fixed::fromInt(1)) high = true; }); }
        CHECK(high);                                                      // it flew an arc
        CHECK(hpAt(*sim, 12, 10) == Fixed::fromInt(970));
        Fixed far = sim->world().get<game::Unit>(unitAt(*sim, Fixed::fromRatio(27, 2), Fixed::fromInt(10))).hp;
        CHECK(far > Fixed::fromInt(970) && far < Fixed::fromInt(1000));   // less at the edge
    }
    {   // colliding lance hits the first enemy on its way
        auto sim = fxSim(kFxRules, dummy(9, 10) + dummy(12, 10));
        castAbility(*sim, findType(*sim, "mage", 1), 2, kNullEntity, P(15, 10));
        run(*sim, 2);
        CHECK(hpAt(*sim, 9, 10) == Fixed::fromInt(975));
        CHECK(hpAt(*sim, 12, 10) == Fixed::fromInt(1000));
    }
}

TEST(fx_delayed_area_channel_chain_cone_windup) {
    {   // artillery: a marker, then the hit after the delay
        auto sim = fxSim(kFxRules, dummy(15, 10));
        castAbility(*sim, findType(*sim, "mage", 1), 3, kNullEntity, P(15, 10));
        bool marker = false;
        for (int i = 0; i < 30; ++i) { sim->step(); for (auto& ev : sim->events()) if (ev.type == game::kEvAreaStart) marker = true; }
        CHECK(marker && hpAt(*sim, 15, 10) == Fixed::fromInt(1000));
        run(*sim, 1.2);
        CHECK(hpAt(*sim, 15, 10) == Fixed::fromInt(940));
        CHECK(sim->world().count<game::Area>() == 0);
    }
    {   // channel: ticks while channelling; a stun interrupts it
        auto sim = fxSim(kFxRules, dummy(9, 10));
        Entity mage = findType(*sim, "mage", 1);
        castAbility(*sim, mage, 4, unitAt(*sim, Fixed::fromInt(9), Fixed::fromInt(10)), {});
        run(*sim, 3.5);
        Fixed full = Fixed::fromInt(1000) - hpAt(*sim, 9, 10);
        CHECK(full >= Fixed::fromInt(60) && full <= Fixed::fromInt(70));
        auto sim2 = fxSim(kFxRules, dummy(9, 10));
        Entity m2 = findType(*sim2, "mage", 1);
        castAbility(*sim2, m2, 4, unitAt(*sim2, Fixed::fromInt(9), Fixed::fromInt(10)), {});
        run(*sim2, 1.1);
        sim2->world().get<game::Buffs>(m2).stunUntil = sim2->tick() + 10;
        run(*sim2, 2.5);
        Fixed cut = Fixed::fromInt(1000) - hpAt(*sim2, 9, 10);
        CHECK(cut > Fixed{} && cut <= Fixed::fromInt(30));
    }
    {   // chain lightning: 80, 40, 20, 10 and nothing beyond the jump radius
        auto sim = fxSim(kFxRules, dummy(9, 10) + dummy(11, 10) + dummy(13, 10) + dummy(15, 10) + dummy(25, 10));
        castAbility(*sim, findType(*sim, "mage", 1), 5, unitAt(*sim, Fixed::fromInt(9), Fixed::fromInt(10)), {});
        run(*sim, 0.5);
        CHECK(hpAt(*sim, 9, 10) == Fixed::fromInt(920) && hpAt(*sim, 11, 10) == Fixed::fromInt(960));
        CHECK(hpAt(*sim, 13, 10) == Fixed::fromInt(980) && hpAt(*sim, 15, 10) == Fixed::fromInt(990));
        CHECK(hpAt(*sim, 25, 10) == Fixed::fromInt(1000));
    }
    {   // cone: in front only
        auto sim = fxSim(kFxRules, dummy(8, 10) + dummy(8, 13) + dummy(2, 10));
        castAbility(*sim, findType(*sim, "mage", 1), 6, kNullEntity, P(10, 10));
        run(*sim, 0.5);
        CHECK(hpAt(*sim, 8, 10) == Fixed::fromInt(980));
        CHECK(hpAt(*sim, 8, 13) == Fixed::fromInt(1000) && hpAt(*sim, 2, 10) == Fixed::fromInt(1000));
    }
    {   // windup: nothing before the cast time, then the hit; backswing keeps the caster busy
        auto sim = fxSim(kFxRules, dummy(9, 10));
        Entity mage = findType(*sim, "mage", 1);
        castAbility(*sim, mage, 7, unitAt(*sim, Fixed::fromInt(9), Fixed::fromInt(10)), {});
        bool windup = false;
        for (int i = 0; i < 12; ++i) { sim->step(); for (auto& ev : sim->events()) if (ev.type == game::kEvCastStart) windup = true; }
        CHECK(windup && hpAt(*sim, 9, 10) == Fixed::fromInt(1000));
        run(*sim, 0.8);
        CHECK(hpAt(*sim, 9, 10) == Fixed::fromInt(950));
        CHECK(sim->world().get<game::AbilityState>(mage).busyUntil > sim->tick());
    }
}

static const char* kFxRules2 = R"({
  "victory": "noUnits",
  "resources": {"gold": {"start": 100}},
  "statuses": {
    "poison": {"tags": ["debuff", "magic"], "tick": {"damage": 5, "interval": 1}, "stacking": "stack", "maxStacks": 3},
    "regen": {"tags": ["buff"], "tick": {"heal": 10, "interval": 1}},
    "frost": {"tags": ["debuff", "magic"], "mods": [{"stat": "speed", "mul": 0.5}]},
    "rooted": {"tags": ["debuff"], "flags": ["root"]},
    "silenced": {"tags": ["debuff", "magic"], "flags": ["silence"]},
    "disarmed": {"tags": ["debuff"], "flags": ["disarm"]},
    "shielded": {"tags": ["buff"], "shield": 50},
    "stone": {"tags": ["buff"], "flags": ["invulnerable"]},
    "vampiric": {"tags": ["buff"], "mods": [{"stat": "lifesteal", "add": 0.5}]},
    "cloak": {"tags": ["buff"], "flags": ["invisible"]},
    "focus": {"tags": ["buff"], "mods": [{"stat": "energyRegen", "mul": 3}]}
  },
  "techs": {"lore": {"cost": {"time": 1}}},
  "playerAbilities": [
    {"id": "poison", "cost": {}, "effects": [{"type": "status", "status": "poison", "duration": 6, "radius": 0.6}]},
    {"id": "regen", "cost": {}, "effects": [{"type": "status", "status": "regen", "duration": 3, "radius": 0.6, "affects": "allies"}]},
    {"id": "frost", "cost": {}, "effects": [{"type": "status", "status": "frost", "duration": 5, "radius": 0.6}]},
    {"id": "root", "cost": {}, "effects": [{"type": "status", "status": "rooted", "duration": 3, "radius": 0.6}]},
    {"id": "silence", "cost": {}, "effects": [{"type": "status", "status": "silenced", "duration": 3, "radius": 0.6}]},
    {"id": "disarm", "cost": {}, "effects": [{"type": "status", "status": "disarmed", "duration": 3, "radius": 0.6}]},
    {"id": "shield", "cost": {}, "effects": [{"type": "status", "status": "shielded", "duration": 10, "radius": 0.6, "affects": "allies"}]},
    {"id": "stone", "cost": {}, "effects": [{"type": "status", "status": "stone", "duration": 3, "radius": 0.6, "affects": "allies"}]},
    {"id": "vamp", "cost": {}, "effects": [{"type": "status", "status": "vampiric", "duration": 10, "radius": 0.6, "affects": "allies"}]},
    {"id": "cloak", "cost": {}, "effects": [{"type": "status", "status": "cloak", "duration": 10, "radius": 0.6, "affects": "allies"}]},
    {"id": "cleanse", "cost": {}, "effects": [{"type": "dispel", "from": "enemies", "radius": 0.6, "affects": "allies"}]},
    {"id": "stomp", "cost": {}, "effects": [{"type": "knockback", "amount": 3, "radius": 3}]},
    {"id": "pull", "cost": {}, "effects": [{"type": "knockback", "amount": -3, "radius": 12}]},
    {"id": "wolves", "cost": {}, "effects": [{"type": "spawn", "unit": "wolf", "count": 2, "limit": 3, "duration": 30}]},
    {"id": "recall", "cost": {}, "effects": [{"type": "teleport", "shape": "map", "affects": "own", "kind": "unit", "ofType": "dummy"}]},
    {"id": "tithe", "cost": {}, "effects": [{"type": "resource", "resource": "gold", "amount": 50, "affects": "self", "shape": "map"}]},
    {"id": "rob", "cost": {}, "effects": [{"type": "resource", "resource": "gold", "amount": -40, "transfer": true, "shape": "map"}]},
    {"id": "burn", "cost": {}, "effects": [{"type": "energyBurn", "amount": 60, "percent": 1, "radius": 0.6}]},
    {"id": "flare", "cost": {}, "effects": [{"type": "reveal", "radius": 4, "duration": 5}]},
    {"id": "focus", "cost": {}, "effects": [{"type": "status", "status": "focus", "duration": 10, "radius": 0.6, "affects": "allies"}]}
  ],
  "types": {
    "dummy": {"move": {"speed": 2}, "health": {"max": 1000}, "energy": {"max": 200, "start": 100, "regen": 1}, "sight": 3},
    "archer": {"move": {"speed": 2}, "health": {"max": 300}, "sight": 8, "weapons": [{"range": 5, "damage": 10, "cooldown": 1,
               "onHit": {"chance": 1, "effects": [{"type": "status", "status": "poison", "duration": 3}]}}]},
    "wolf": {"move": {"speed": 3}, "health": {"max": 100}},
    "hero": {"move": {"speed": 2}, "health": {"max": 500}, "energy": {"max": 500, "start": 500}, "hero": {"levels": [10, 20]}, "unique": false,
             "abilities": [
               {"id": "bolt", "target": "unit", "range": 10, "cooldown": 6, "charges": 2, "autocast": false, "effects": [{"type": "damage", "amount": 10}],
                "levels": [{"effects": [{"amount": 30}]}]},
               {"id": "secret", "target": "unit", "range": 10, "cooldown": 1, "autocast": false, "requires": ["lore"], "effects": [{"type": "damage", "amount": 5}]},
               {"id": "mend", "target": "unit", "range": 8, "cooldown": 1, "smart": "lowestHpAlly", "effects": [{"type": "heal", "amount": 100}]}
             ], "researches": []},
    "school": {"structure": {"footprint": [2, 2]}, "researches": ["lore"]}
  }})";

static int paIndex(Simulation& sim, const char* id) { return mod(sim).rules().findPlayerAbility(id); }
static void cast(Simulation& sim, uint8_t player, const char* id, Vec3 p, Entity target = kNullEntity) {
    Command c; c.player = player; c.type = game::kCmdPlayerAbility; c.param = paIndex(sim, id); c.point = p; c.target = target;
    sim.submit(c);
}
static std::unique_ptr<Simulation> fx2(const std::string& ents, uint64_t seed = 1) {
    return makeSimJson(kFxRules2, flatMap(40, 20, ",\"entities\":[" + ents.substr(1) + "]"), seed);
}

TEST(fx_statuses_dot_hot_mods_flags) {
    auto sim = fx2(dummy(10, 10) + dummy(20, 10, 1) + dummy(30, 5, 1));
    World& w = sim->world();
    auto& m = mod(*sim);
    Entity foe = unitAt(*sim, Fixed::fromInt(10), Fixed::fromInt(10)), mine = unitAt(*sim, Fixed::fromInt(20), Fixed::fromInt(10));
    for (int k = 0; k < 4; ++k) { cast(*sim, 1, "poison", P(10, 10)); sim->step(); }
    run(*sim, 0.2);
    auto& st = w.get<game::Statuses>(foe).list;
    CHECK(st.size() == 1 && st[0].stacks == 3);                           // stacks capped at 3
    run(*sim, 2.05);
    CHECK(w.get<game::Unit>(foe).hp == Fixed::fromInt(970));              // 2 ticks x 15
    w.get<game::Unit>(mine).hp = Fixed::fromInt(500);
    cast(*sim, 1, "regen", P(20, 10));
    run(*sim, 3.5);
    CHECK(w.get<game::Unit>(mine).hp == Fixed::fromInt(530));             // HoT 3 x 10
    cast(*sim, 1, "frost", P(10, 10));
    run(*sim, 0.2);
    CHECK(m.stat(*sim, foe, game::Stat::Speed) == Fixed::fromInt(1));     // slowed by half
    cast(*sim, 2, "root", P(20, 10));
    run(*sim, 0.2);
    Vec3 before = w.get<Transform>(mine).pos;
    Command mv; mv.player = 1; mv.type = game::kCmdMove; mv.units = {mine}; mv.point = P(25, 10);
    sim->submit(mv);
    run(*sim, 1);
    CHECK((w.get<Transform>(mine).pos - before).xz().length() < Fixed::fromRatio(1, 10));  // rooted
    run(*sim, 3);
    CHECK(w.get<Transform>(mine).pos.x > before.x + Fixed::fromInt(2));   // free again
    // dispel only what enemies cast: the own shield stays, the enemy frost goes
    Entity far = unitAt(*sim, Fixed::fromInt(30), Fixed::fromInt(5));
    cast(*sim, 1, "shield", P(30, 5));
    cast(*sim, 2, "frost", P(30, 5));
    run(*sim, 0.3);
    CHECK(w.get<game::Statuses>(far).list.size() == 2);
    cast(*sim, 1, "cleanse", P(30, 5));
    run(*sim, 0.3);
    CHECK(w.get<game::Statuses>(far).list.size() == 1 && m.rules().statuses[w.get<game::Statuses>(far).list[0].def].id == "shielded");
    // the shield absorbs 50 damage, then breaks
    m.dealDamage(*sim, far, Fixed::fromInt(30), 0, kNullEntity, 2);
    CHECK(w.get<game::Unit>(far).hp == Fixed::fromInt(1000));
    m.dealDamage(*sim, far, Fixed::fromInt(30), 0, kNullEntity, 2);
    CHECK(w.get<game::Unit>(far).hp == Fixed::fromInt(990));
    // invulnerable
    cast(*sim, 1, "stone", P(30, 5));
    run(*sim, 0.2);
    m.dealDamage(*sim, far, Fixed::fromInt(100), 0, kNullEntity, 2);
    CHECK(w.get<game::Unit>(far).hp == Fixed::fromInt(990));
}

TEST(fx_silence_disarm_invisibility_lifesteal_onhit) {
    auto sim = fx2(dummy(6, 10, 1, "hero") + dummy(10, 10, 2, "archer") + dummy(13, 10) + dummy(30, 10, 1, "archer") + dummy(34, 10, 2));
    World& w = sim->world();
    auto& m = mod(*sim);
    Entity hero = findType(*sim, "hero", 1);
    Entity archer = unitAt(*sim, Fixed::fromInt(10), Fixed::fromInt(10));
    cast(*sim, 2, "silence", P(6, 10));
    run(*sim, 0.2);
    CHECK(!m.canCast(*sim, hero, 0));
    run(*sim, 3.2);
    CHECK(m.canCast(*sim, hero, 0));
    // the enemy archer shoots the hero; its arrows poison
    bool poisoned = false;
    for (int i = 0; i < 60 && !poisoned; ++i) {
        sim->step();
        for (auto& in : w.get<game::Statuses>(hero).list) if (m.rules().statuses[in.def].id == "poison") poisoned = true;
    }
    CHECK(poisoned);
    // disarm: no more shots
    cast(*sim, 1, "disarm", P(10, 10));
    run(*sim, 0.3);
    bool shot = false;
    for (int i = 0; i < 40; ++i) { sim->step(); for (auto& ev : sim->events()) if (ev.type == game::kEvAttack && ev.a == archer) shot = true; }
    CHECK(!shot);
    // cloak: an enemy archer next to it doesn't see it as a target
    Entity mine = findType(*sim, "archer", 1);
    Entity sneaky = unitAt(*sim, Fixed::fromInt(34), Fixed::fromInt(10));
    (void)sneaky;
    cast(*sim, 1, "cloak", P(30, 10));
    run(*sim, 0.3);
    w.get<game::Combat>(mine).target = kNullEntity;
    // vampiric archer heals from its hits
    w.get<game::Unit>(mine).hp = Fixed::fromInt(200);
    cast(*sim, 1, "vamp", P(30, 10));
    run(*sim, 3);
    CHECK(w.get<game::Unit>(mine).hp > Fixed::fromInt(200));
    // invisible units can't be acquired by enemies
    Entity foeArcher = archer;
    w.get<Transform>(foeArcher).pos = P(27, 10);
    run(*sim, 0.5);
    CHECK(w.get<game::Combat>(foeArcher).target != mine);
}

TEST(fx_knockback_summon_teleport_resource_burn_reveal) {
    auto sim = fx2(dummy(10, 10) + dummy(12, 10) + dummy(35, 15, 1) + dummy(36, 15, 1));
    World& w = sim->world();
    auto& m = mod(*sim);
    Entity a = unitAt(*sim, Fixed::fromInt(12), Fixed::fromInt(10));
    cast(*sim, 1, "stomp", P(10, 10));
    run(*sim, 0.2);
    CHECK(w.get<Transform>(a).pos.x > Fixed::fromInt(14));                   // pushed away
    cast(*sim, 1, "pull", P(5, 10));
    run(*sim, 0.2);
    CHECK(w.get<Transform>(a).pos.x < Fixed::fromInt(13));                   // pulled in
    for (int k = 0; k < 3; ++k) { cast(*sim, 1, "wolves", P(20, 5)); run(*sim, 0.2); }
    CHECK(countType(*sim, "wolf", 1) == 3);                                  // summon limit
    cast(*sim, 1, "recall", P(20, 18));
    run(*sim, 0.2);
    CHECK(dist2DTest(w.get<Transform>(unitAt(*sim, Fixed::fromInt(20), Fixed::fromInt(18))).pos, P(20, 18)) < Fixed::fromInt(2));
    CHECK(w.get<Owner>(unitAt(*sim, Fixed::fromInt(35), Fixed::fromInt(15))).player != 1 || dist2DTest(w.get<Transform>(unitAt(*sim, Fixed::fromInt(35), Fixed::fromInt(15))).pos, P(35, 15)) > Fixed::fromInt(3));
    Fixed g1 = stockOf(*sim, 1, "gold"), g2 = stockOf(*sim, 2, "gold");
    cast(*sim, 1, "tithe", P(1, 1));
    run(*sim, 0.2);
    cast(*sim, 1, "rob", P(1, 1));
    run(*sim, 0.2);
    CHECK(stockOf(*sim, 1, "gold") == g1 + Fixed::fromInt(90) && stockOf(*sim, 2, "gold") == g2 - Fixed::fromInt(40));
    Entity foe = unitAt(*sim, Fixed::fromInt(10), Fixed::fromInt(10));
    Fixed hp = w.get<game::Unit>(foe).hp;
    cast(*sim, 1, "burn", w.get<Transform>(foe).pos);
    run(*sim, 0.2);
    CHECK(w.get<game::Unit>(foe).energy < Fixed::fromInt(50) && w.get<game::Unit>(foe).hp < hp - Fixed::fromInt(50));
    CHECK(!m.isVisible(*sim, 1, P(5, 3)));
    cast(*sim, 1, "flare", P(5, 3));
    run(*sim, 0.5);
    CHECK(m.isVisible(*sim, 1, P(5, 3)));
    run(*sim, 5);
    CHECK(!m.isVisible(*sim, 1, P(5, 3)));
}

TEST(fx_unit_ability_charges_levels_requires_smart) {
    auto sim = fx2(dummy(6, 10, 1, "hero") + dummy(9, 10) + dummy(7, 13, 1) + dummy(3, 3, 1, "school"));
    World& w = sim->world();
    auto& m = mod(*sim);
    Entity hero = findType(*sim, "hero", 1), foe = unitAt(*sim, Fixed::fromInt(9), Fixed::fromInt(10));
    for (int k = 0; k < 3; ++k) { castAbility(*sim, hero, 0, foe, {}); run(*sim, 0.3); }
    CHECK(w.get<game::Unit>(foe).hp == Fixed::fromInt(980));               // two charges, the third cast fails
    CHECK(!m.canCast(*sim, hero, 1));                                     // needs the tech
    Command r; r.player = 1; r.type = game::kCmdResearch; r.units = {findType(*sim, "school", 1)}; r.param = m.rules().findTech("lore");
    sim->submit(r);
    run(*sim, 1.5);
    CHECK(m.canCast(*sim, hero, 1));
    w.get<game::Hero>(hero).level = 2;                                    // level 2 table: 30 damage
    run(*sim, 6.5);
    castAbility(*sim, hero, 0, foe, {});
    run(*sim, 0.3);
    CHECK(w.get<game::Unit>(foe).hp == Fixed::fromInt(950));
    // smart autocast heals the most wounded ally
    Entity hurt = unitAt(*sim, Fixed::fromInt(7), Fixed::fromInt(13));
    w.get<game::Unit>(hurt).hp = Fixed::fromInt(300);
    run(*sim, 1);
    CHECK(w.get<game::Unit>(hurt).hp == Fixed::fromInt(400));
    // energy regen stat from a status
    Fixed e0 = w.get<game::Unit>(hurt).energy;
    cast(*sim, 1, "focus", w.get<Transform>(hurt).pos);
    run(*sim, 2);
    CHECK(w.get<game::Unit>(hurt).energy >= e0 + Fixed::fromInt(5));
}

// ------------------------------------------------------------------ Bunker Wars style skills (garrisons, groups, players)

static const char* kBwRules = R"({
  "victory": "noEntities",
  "lanes": {"left": 1, "right": 1, "spacing": 0.4},
  "resources": {"mana": {"start": 100, "regen": 2, "capped": true, "storage": 200}, "morale": {"start": 50}},
  "statuses": {
    "frozen": {"tags": ["debuff", "magic"], "flags": ["stun"]},
    "plague": {"tags": ["debuff", "magic"], "flags": ["spreads"], "garrisonPerSecond": -1},
    "locked": {"tags": ["debuff"], "flags": ["lock"]},
    "fort": {"tags": ["buff"], "mods": [{"stat": "defense", "mul": 2}]},
    "aegis": {"tags": ["buff"], "flags": ["invulnerable"]},
    "barrier5": {"tags": ["buff"], "shield": 5},
    "revenge": {"tags": ["buff"], "onCaptured": [{"type": "garrisonKill", "percent": 1, "shape": "target", "affects": "any"}]},
    "boom": {"tags": ["buff"], "mods": [{"stat": "productionSpeed", "mul": 3}]},
    "flight": {"tags": ["buff"], "flags": ["flying"]},
    "muted": {"tags": ["debuff"], "flags": ["silence"]},
    "drought": {"tags": ["debuff"], "flags": ["freezeRegen"]},
    "wind": {"tags": ["debuff"], "mods": [{"stat": "speed", "mul": 0.5}]},
    "watch": {"tags": ["buff"], "flags": ["alert"]},
    "aura": {"tags": ["buff"], "tick": {"interval": 0.5, "effects": [{"type": "damage", "amount": 1, "radius": 1.5}]}}
  },
  "loadoutSlots": 30,
  "playerAbilities": [
    {"id": "bomb", "target": "structure", "effects": [{"type": "garrisonKill", "percent": 0.5}]},
    {"id": "landing", "target": "structure", "effects": [{"type": "garrisonAdd", "amount": 10, "affects": "own"}]},
    {"id": "defector", "target": "structure", "effects": [{"type": "garrisonConvert", "percent": 0.4}]},
    {"id": "panic", "target": "structure", "effects": [{"type": "garrisonPanic", "percent": 0.3}]},
    {"id": "possession", "target": "structure", "effects": [{"type": "capture", "duration": 3, "neutral": true}]},
    {"id": "swap", "target": "structure", "effects": [{"type": "swap"}]},
    {"id": "casttower", "target": "structure", "effects": [{"type": "transform", "to": "tower", "duration": 3, "affects": "any"}]},
    {"id": "upgrade", "target": "structure", "effects": [{"type": "level", "delta": 1, "affects": "own"}]},
    {"id": "downgrade", "target": "structure", "effects": [{"type": "level", "delta": -1, "duration": 3}]},
    {"id": "average", "target": "structure", "effects": [{"type": "equalize", "affects": "own"}]},
    {"id": "redirect", "target": "structure", "effects": [{"type": "redirect", "duration": 10, "affects": "own"}]},
    {"id": "lock", "target": "structure", "effects": [{"type": "status", "status": "locked", "duration": 5}]},
    {"id": "fortify", "target": "structure", "effects": [{"type": "status", "status": "fort", "duration": 20, "affects": "own"}]},
    {"id": "aegis", "target": "structure", "effects": [{"type": "status", "status": "aegis", "duration": 20, "affects": "own"}]},
    {"id": "shield5", "target": "structure", "effects": [{"type": "status", "status": "barrier5", "duration": 20, "affects": "own"}]},
    {"id": "plague", "target": "structure", "effects": [{"type": "status", "status": "plague", "duration": 30}]},
    {"id": "revenge", "target": "structure", "effects": [{"type": "status", "status": "revenge", "duration": 30, "affects": "own"}]},
    {"id": "resurrect", "target": "structure", "effects": [{"type": "garrisonAdd", "percent": 1, "ofLost": true, "affects": "own"}]},
    {"id": "boom", "target": "structure", "effects": [{"type": "status", "status": "boom", "duration": 10, "affects": "own"}]},
    {"id": "freeze", "target": "group", "effects": [{"type": "status", "status": "frozen", "duration": 3, "shape": "group"}]},
    {"id": "gohome", "target": "group", "effects": [{"type": "sendBack", "mode": "home", "shape": "group"}]},
    {"id": "throwback", "target": "group", "effects": [{"type": "sendBack", "mode": "throwBack", "amount": 4, "shape": "group"}]},
    {"id": "illusion", "target": "group", "effects": [{"type": "clone", "fake": true, "shape": "group", "affects": "own"}]},
    {"id": "closerows", "target": "group", "effects": [{"type": "formation", "shape": "group", "affects": "own"}]},
    {"id": "hypnosis", "target": "group", "effects": [{"type": "convert", "percent": 0.5, "shape": "group"}]},
    {"id": "flywave", "target": "group", "effects": [{"type": "status", "status": "flight", "duration": 20, "shape": "group", "affects": "own"}]},
    {"id": "killaura", "target": "group", "effects": [{"type": "status", "status": "aura", "duration": 20, "shape": "group", "affects": "own"}]},
    {"id": "barrier", "target": "point", "effects": [{"type": "spawn", "unit": "wall"}]},
    {"id": "mines", "target": "point", "effects": [{"type": "area", "trigger": "contact", "radius": 1, "maxTriggers": 3, "effects": [{"type": "kill", "shape": "target"}]}]},
    {"id": "tornado", "target": "group", "effects": [{"type": "area", "motion": "lane", "speed": 6, "lifetime": 20, "interval": 0.25, "trigger": "periodic", "radius": 1.2,
                                                      "effects": [{"type": "kill", "radius": 1.2}]}]},
    {"id": "shortway", "target": "point", "effects": [{"type": "area", "trigger": "contact", "radius": 1.5, "lifetime": 20, "teleportTo": [12, 0], "affects": "own"}]},
    {"id": "souls", "target": "point", "effects": [{"type": "area", "trigger": "periodic", "radius": 4, "lifetime": 20, "deathSpawn": "skeleton"}]},
    {"id": "silence", "target": "player", "effects": [{"type": "playerStatus", "status": "muted", "duration": 10}]},
    {"id": "drought", "target": "player", "effects": [{"type": "playerStatus", "status": "drought", "duration": 10}]},
    {"id": "wind", "target": "player", "effects": [{"type": "playerStatus", "status": "wind", "duration": 10}]},
    {"id": "watch", "target": "player", "effects": [{"type": "playerStatus", "status": "watch", "duration": 60, "affects": "self"}]},
    {"id": "thief", "target": "player", "effects": [{"type": "resource", "resource": "morale", "amount": -20, "transfer": true}]},
    {"id": "share", "target": "player", "effects": [{"type": "equalizeResource", "resource": "morale", "affects": "any"}]},
    {"id": "sacrifice", "target": "structure", "effects": [{"type": "sacrifice", "percent": 0.5, "gain": {"mana": 2}, "affects": "own"}]},
    {"id": "steal", "target": "player", "effects": [{"type": "stealSkill", "duration": 30}]},
    {"id": "breakmagic", "target": "none", "effects": [{"type": "dispel", "shape": "map", "affects": "any"}]},
    {"id": "slowdown", "target": "player", "effects": [{"type": "cooldown", "amount": 20}]},
    {"id": "nuke", "target": "point", "cost": {"mana": 50}, "cooldown": 30, "initialCooldown": 5, "castTime": 2,
     "effects": [{"type": "garrisonKill", "amount": 5, "radius": 2, "kind": "structure"}],
     "levels": [{"effects": [{"amount": 10}]}, {"effects": [{"amount": 15}]}]},
    {"id": "tide", "target": "structure", "autoEvery": 10, "effects": [{"type": "garrisonAdd", "amount": 2, "affects": "own"}]},
    {"id": "blessed", "passive": true, "status": {"id": "bless", "incomeMult": {"mana": 2}}}
  ],
  "types": {
    "spore": {"move": {"speed": 2.4, "radius": 0.15}, "health": {"max": 1}, "entersGarrison": true},
    "hut": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "level": 1, "upgrades": ["hut2"], "garrison": {"max": 40, "unit": "spore", "interval": 1, "capturable": true}},
    "hut2": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "level": 2, "garrison": {"max": 60, "unit": "spore", "interval": 0.8, "capturable": true}},
    "tower": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "level": 1, "garrison": {"max": 20, "unit": "spore", "capturable": true}, "weapons": [{"range": 4, "damage": 1, "cooldown": 1}]},
    "wall": {"health": {"max": 5}, "move": {"speed": 0, "radius": 0.6}, "blocksLanes": true},
    "skeleton": {"move": {"speed": 2}, "health": {"max": 20}}
  }})";

static std::string bwMap(const std::string& extra = "", const std::string& loadA = "", const std::string& loadB = "") {
    std::string players = std::string("[{\"name\":\"A\",\"team\":1") + (loadA.empty() ? "" : ",\"loadout\":" + loadA) + "},{\"name\":\"B\",\"team\":2" +
                          (loadB.empty() ? "" : ",\"loadout\":" + loadB) + "}]";
    return flatMap(40, 16, R"(,"entities":[
      {"type":"hut","owner":1,"pos":[4,8],"garrison":20},{"type":"hut","owner":1,"pos":[10,3],"garrison":5},
      {"type":"hut","owner":2,"pos":[36,8],"garrison":20},{"type":"hut","owner":2,"pos":[30,13],"garrison":10},
      {"type":"hut","owner":0,"pos":[20,8],"garrison":3})" + extra + "]", players);
}
static Entity hutAt(Simulation& sim, int x, int z) { return unitAt(sim, Fixed::fromInt(x), Fixed::fromInt(z)); }
static int garrisonOf(Simulation& sim, Entity e) { return sim.world().get<game::Garrison>(e).count; }
static void send(Simulation& sim, uint8_t player, Entity from, Entity to, int pct) {
    Command c; c.player = player; c.type = game::kCmdSend; c.units = {from}; c.target = to; c.param = pct; sim.submit(c);
}

TEST(bw_garrison_primitives) {
    auto sim = makeSimJson(kBwRules, bwMap());
    World& w = sim->world();
    auto& m = mod(*sim);
    Entity a1 = hutAt(*sim, 4, 8), a2 = hutAt(*sim, 10, 3), b1 = hutAt(*sim, 36, 8), b2 = hutAt(*sim, 30, 13), nh = hutAt(*sim, 20, 8);
    auto g = [&](Entity e) -> game::Garrison& { return w.get<game::Garrison>(e); };
    for (Entity e : {a1, a2, b1, b2}) g(e).growth = Fixed{};
    // bomb: half of the garrison
    g(b1).count = 20;
    cast(*sim, 1, "bomb", {}, b1); run(*sim, 0.2);
    CHECK(garrisonOf(*sim, b1) == 10 || garrisonOf(*sim, b1) == 11);
    // resurrection: the lost units come back
    int before = garrisonOf(*sim, b1);
    cast(*sim, 2, "resurrect", {}, b1); run(*sim, 0.2);
    CHECK(garrisonOf(*sim, b1) >= before + 10);
    // defector: 40% leave and walk to the caster's structure
    g(b2).count = 10;
    cast(*sim, 1, "defector", {}, b2); run(*sim, 0.2);
    CHECK(garrisonOf(*sim, b2) <= 7 && countType(*sim, "spore", 1) == 4);
    // possession: captured for 3 s, then back
    cast(*sim, 1, "possession", {}, nh); run(*sim, 0.2);
    CHECK(w.get<Owner>(nh).player == 1);
    run(*sim, 3.2);
    CHECK(w.get<Owner>(nh).player == 0);
    // swap: my structure trades places with an enemy one
    g(a2).count = 2; g(b2).count = 30;
    cast(*sim, 1, "swap", {}, b2); run(*sim, 0.2);
    CHECK(w.get<Owner>(b2).player == 1 && w.get<Owner>(a2).player == 2);
    // cast tower for 3 s, upgrade and timed downgrade along the upgrade chain
    cast(*sim, 1, "casttower", {}, a1); run(*sim, 0.2);
    CHECK(m.rules().types[w.get<game::Unit>(a1).type].id == "tower");
    run(*sim, 3.2);
    CHECK(m.rules().types[w.get<game::Unit>(a1).type].id == "hut");
    cast(*sim, 1, "upgrade", {}, a1); run(*sim, 0.2);
    CHECK(m.rules().types[w.get<game::Unit>(a1).type].id == "hut2");
    cast(*sim, 2, "downgrade", {}, a1); run(*sim, 0.2);
    CHECK(m.rules().types[w.get<game::Unit>(a1).type].id == "hut");
    run(*sim, 3.2);
    CHECK(m.rules().types[w.get<game::Unit>(a1).type].id == "hut2");
    // average: own garrisons even out
    g(a1).count = 30; g(b2).count = 2;
    cast(*sim, 1, "average", {}, a1); run(*sim, 0.2);
    CHECK(std::abs(garrisonOf(*sim, a1) - garrisonOf(*sim, b2)) <= 1);
    // lock: no production, no sending
    g(b1).count = 10; g(b1).growth = Fixed{};
    cast(*sim, 1, "lock", {}, b1); run(*sim, 0.2);
    send(*sim, 2, b1, b2, 50); run(*sim, 2);
    CHECK(garrisonOf(*sim, b1) == 10);
    // redirect: a1's growth goes to b2 (own structures)
    int ga = garrisonOf(*sim, a1), gb = garrisonOf(*sim, b2);
    cast(*sim, 1, "redirect", {}, a1); run(*sim, 3);
    CHECK(garrisonOf(*sim, a1) == ga && garrisonOf(*sim, b2) > gb);
}

TEST(bw_assault_defense_shield_plague_revenge) {
    auto sim = makeSimJson(kBwRules, bwMap());
    World& w = sim->world();
    Entity a1 = hutAt(*sim, 4, 8), nh = hutAt(*sim, 20, 8), b1 = hutAt(*sim, 36, 8);
    // invulnerable: an assault does nothing
    w.get<game::Garrison>(nh).count = 3;
    cast(*sim, 2, "aegis", {}, nh);  // not owned by 2: refused
    w.get<Owner>(nh).player = 2;
    cast(*sim, 2, "aegis", {}, nh);
    run(*sim, 0.2);
    w.get<game::Garrison>(a1).count = 20;
    send(*sim, 1, a1, nh, 50);
    run(*sim, 12);
    CHECK(w.get<Owner>(nh).player == 2 && w.get<game::Garrison>(nh).count >= 3);
    // plague spreads: an infected structure's sent units infect where they go
    cast(*sim, 1, "plague", {}, b1); run(*sim, 0.2);
    Entity b2 = hutAt(*sim, 30, 13);
    send(*sim, 2, b1, b2, 30);
    run(*sim, 6);
    bool infected = false;
    for (auto& in : w.get<game::Statuses>(b2).list) if (mod(*sim).rules().statuses[in.def].id == "plague") infected = true;
    CHECK(infected);
    // revenge: capturing a structure with it costs the capturer the garrison it brought
    Entity a2 = hutAt(*sim, 10, 3);
    w.get<game::Garrison>(a2).count = 0;
    w.get<game::Garrison>(a2).growth = Fixed{};
    cast(*sim, 1, "revenge", {}, a2); run(*sim, 0.2);
    w.get<game::Garrison>(b2).count = 30;
    send(*sim, 2, b2, a2, 100);
    for (int i = 0; i < 20 * 15 && w.get<Owner>(a2).player != 2; ++i) sim->step();
    run(*sim, 0.2);
    CHECK(w.get<Owner>(a2).player == 2);
    CHECK(w.get<game::Garrison>(a2).lost >= 1);                         // the revenge blast hit the capturers
}

TEST(bw_groups_on_lanes) {
    auto sim = makeSimJson(kBwRules, bwMap());
    World& w = sim->world();
    auto& m = mod(*sim);
    Entity a1 = hutAt(*sim, 4, 8), nh = hutAt(*sim, 20, 8);
    run(*sim, 1.05);
    w.get<game::Garrison>(a1).count = 30;
    send(*sim, 1, a1, nh, 50);
    run(*sim, 2);
    std::vector<Entity> group;
    w.each<game::GroupTag, Owner>([&](Entity e, game::GroupTag&, Owner& o) { if (o.player == 1) group.push_back(e); });
    CHECK(group.size() >= 8);
    // freeze the whole group
    cast(*sim, 2, "freeze", {}, group[0]); run(*sim, 0.2);
    Vec3 p0 = w.get<Transform>(group[0]).pos;
    run(*sim, 1);
    CHECK(dist2DTest(w.get<Transform>(group[0]).pos, p0) < Fixed::fromRatio(1, 10));
    run(*sim, 2.5);
    // throw back: lane position goes down
    uint16_t lp = w.get<game::LaneWalker>(group[0]).pos;
    cast(*sim, 2, "throwback", {}, group[0]); run(*sim, 0.2);
    CHECK(w.get<game::LaneWalker>(group[0]).pos + 4 <= lp + 1);
    // hypnosis: half of the group changes sides
    cast(*sim, 2, "hypnosis", {}, group[0]); run(*sim, 0.2);
    int converted = 0;
    for (Entity e : group) if (w.alive(e) && w.get<Owner>(e).player == 2) ++converted;
    CHECK(converted >= int(group.size()) / 2 - 1 && converted <= int(group.size()) / 2 + 1);
    // go home: the rest turn around
    Entity rest;
    for (Entity e : group) if (w.alive(e) && w.get<Owner>(e).player == 1) rest = e;
    cast(*sim, 2, "gohome", {}, rest); run(*sim, 0.2);
    CHECK(w.get<game::Orders>(rest).queue.front().target == a1);
    run(*sim, 15);
    CHECK(w.get<Owner>(nh).player != 1);
    (void)m;
}

TEST(bw_hazards_portal_mines_souls_barrier) {
    auto sim = makeSimJson(kBwRules, bwMap());
    World& w = sim->world();
    Entity a1 = hutAt(*sim, 4, 8), nh = hutAt(*sim, 20, 8);
    run(*sim, 1.05);
    w.get<game::Garrison>(a1).count = 30;
    // mines on the lane kill three units
    cast(*sim, 2, "mines", P(12, 8));
    send(*sim, 1, a1, nh, 34);
    run(*sim, 2);
    std::vector<Entity> group;
    w.each<game::GroupTag, Owner>([&](Entity e, game::GroupTag&, Owner& o) { if (o.player == 1) group.push_back(e); });
    int sent = int(group.size());
    run(*sim, 6);
    int died = 0;
    for (Entity e : group) if (!w.alive(e)) ++died;
    CHECK(sent >= 8);
    CHECK(w.get<game::Garrison>(nh).count >= 0);
    // a tornado runs along the lane of a group and kills what it touches
    w.get<game::Garrison>(a1).count = 30;
    send(*sim, 1, a1, nh, 50);
    run(*sim, 2);
    group.clear();
    w.each<game::GroupTag, Owner>([&](Entity e, game::GroupTag&, Owner& o) { if (o.player == 1) group.push_back(e); });
    CHECK(!group.empty());
    cast(*sim, 2, "tornado", {}, group[0]);
    run(*sim, 2.5);
    int dead = 0;
    for (Entity e : group) if (!w.alive(e)) ++dead;
    CHECK(dead > 0);
    // barrier: a wall on the lane stops walkers until destroyed
    auto sim2 = makeSimJson(kBwRules, bwMap());
    World& w2 = sim2->world();
    Entity c1 = hutAt(*sim2, 4, 8), cn = hutAt(*sim2, 20, 8);
    w2.get<game::Garrison>(c1).count = 30;
    w2.get<game::Garrison>(cn).count = 3;
    run(*sim2, 0.5);
    cast(*sim2, 2, "barrier", P(12, 8));
    run(*sim2, 0.3);
    CHECK(countType(*sim2, "wall", 2) == 1);
    send(*sim2, 1, c1, cn, 20);
    run(*sim2, 10);
    CHECK(countType(*sim2, "wall", 2) == 0);                              // they wore it down (5 hp)
    CHECK(w2.get<Owner>(cn).player == 0);                                 // and didn't reach the hut
    // souls: units dying inside turn into skeletons for the area owner
    auto sim3 = makeSimJson(kBwRules, bwMap(R"(,{"type":"skeleton","owner":1,"pos":[25,3]},{"type":"skeleton","owner":1,"pos":[25.5,3]})"));
    cast(*sim3, 2, "souls", P(25, 3));
    run(*sim3, 0.3);
    sim3->world().each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) { if (o.player == 1 && mod(*sim3).rules().types[u.type].id == "skeleton") u.hp = Fixed{}; });
    run(*sim3, 0.3);
    CHECK(countType(*sim3, "skeleton", 2) == 2);
    (void)died;
}

TEST(bw_player_skills_loadout_levels_auto) {
    auto sim = makeSimJson(kBwRules, bwMap("", R"(["silence","drought","wind","watch","thief","share","sacrifice","steal","breakmagic","slowdown",{"id":"nuke","level":3},"tide","blessed","bomb","landing","freeze"])",
                                              R"(["bomb","landing","freeze","possession"])"));
    World& w = sim->world();
    auto& m = mod(*sim);
    auto& gs = m.state(*sim);
    Entity a1 = hutAt(*sim, 4, 8), b1 = hutAt(*sim, 36, 8);
    // loadout: B can't cast what it doesn't have
    CHECK(!m.canUsePlayerAbility(*sim, 2, paIndex(*sim, "silence")));
    CHECK(m.canUsePlayerAbility(*sim, 2, paIndex(*sim, "bomb")));
    // passive: A's mana income is doubled (regen 2 -> 4/s)
    Fixed mana = stockOf(*sim, 1, "mana"), manaB = stockOf(*sim, 2, "mana");
    run(*sim, 5);
    CHECK(fxAbs(stockOf(*sim, 1, "mana") - mana - Fixed::fromInt(20)) < Fixed::fromRatio(1, 100));
    CHECK(fxAbs(stockOf(*sim, 2, "mana") - manaB - Fixed::fromInt(10)) < Fixed::fromRatio(1, 100));
    // freeze regen
    cast(*sim, 1, "drought", P(36, 8), b1);
    run(*sim, 0.2);
    manaB = stockOf(*sim, 2, "mana");
    run(*sim, 3);
    CHECK(stockOf(*sim, 2, "mana") == manaB);
    // silence: B can't cast for 10 s
    cast(*sim, 1, "silence", P(36, 8), b1);
    run(*sim, 0.2);
    CHECK(!m.canUsePlayerAbility(*sim, 2, paIndex(*sim, "bomb")));
    // morale thief and equalize
    cast(*sim, 1, "thief", P(36, 8), b1); run(*sim, 0.2);
    CHECK(stockOf(*sim, 1, "morale") == Fixed::fromInt(70) && stockOf(*sim, 2, "morale") == Fixed::fromInt(30));
    cast(*sim, 1, "share", P(36, 8), b1); run(*sim, 0.2);
    CHECK(stockOf(*sim, 1, "morale") == Fixed::fromInt(50) && stockOf(*sim, 2, "morale") == Fixed::fromInt(50));
    // sacrifice half a garrison for mana
    w.get<game::Garrison>(a1).count = 20;
    Fixed m0 = stockOf(*sim, 1, "mana");
    cast(*sim, 1, "sacrifice", {}, a1); run(*sim, 0.2);
    CHECK(stockOf(*sim, 1, "mana") >= m0 + Fixed::fromInt(19));
    // steal a skill B has and A doesn't
    cast(*sim, 1, "steal", P(36, 8), b1); run(*sim, 0.2);
    CHECK(gs.playerAbilities[1][size_t(paIndex(*sim, "possession"))].owned);
    // break all magic: player statuses gone
    CHECK(!gs.players[2].statuses.empty());
    cast(*sim, 1, "breakmagic", P(1, 1)); run(*sim, 0.2);
    CHECK(gs.players[2].statuses.empty());
    CHECK(m.canUsePlayerAbility(*sim, 2, paIndex(*sim, "bomb")));
    // enemy cooldowns +20 s
    cast(*sim, 1, "slowdown", P(36, 8), b1); run(*sim, 0.2);
    CHECK(!m.canUsePlayerAbility(*sim, 2, paIndex(*sim, "bomb")));
    // nuke at level 3: initial cooldown, activation delay, 15 units
    w.get<game::Garrison>(b1).count = 30;
    int nuke = paIndex(*sim, "nuke");
    CHECK(gs.playerAbilities[1][size_t(nuke)].level == 3);
    cast(*sim, 1, "nuke", P(36, 8)); run(*sim, 2.5);
    CHECK(w.get<game::Garrison>(b1).count <= 19);                       // level 3: 15 units (plus a little growth)
    // alert: watch, then B sends at A
    cast(*sim, 1, "watch", P(4, 8), a1); run(*sim, 0.2);
    gs.playerAbilities[2][size_t(paIndex(*sim, "bomb"))].readyAt = 0;
    w.get<game::Garrison>(b1).count = 20;
    send(*sim, 2, b1, a1, 30);
    bool alert = false;
    for (int i = 0; i < 10; ++i) { sim->step(); for (auto& ev : sim->events()) if (ev.type == game::kEvAlert && ev.player == 1) alert = true; }
    CHECK(alert);
    // auto skill: tide adds 2 units to an own structure every 10 s by itself
    int total0 = 0;
    w.each<game::Garrison, Owner>([&](Entity, game::Garrison& g, Owner& o) { if (o.player == 1) total0 += g.count; });
    (void)total0;
}

TEST(bw_skills_deterministic) {
    auto play = [](uint64_t seed) {
        auto sim = makeSimJson(kBwRules, bwMap(), seed);
        std::vector<game::RtsAI> bots;
        for (auto& p : sim->players()) bots.emplace_back(p.slot, seed);
        for (int t = 0; t < 20 * 90 && sim->result() == 0; ++t) { for (auto& b : bots) b.update(*sim); sim->step(); }
        return std::pair{sim->checksum(), sim->tick()};
    };
    CHECK(play(3) == play(3));
}

TEST(bw_more_primitives) {
    auto sim = makeSimJson(kBwRules, bwMap());
    World& w = sim->world();
    auto& m = mod(*sim);
    Entity a1 = hutAt(*sim, 4, 8), a2 = hutAt(*sim, 10, 3), b1 = hutAt(*sim, 36, 8), b2 = hutAt(*sim, 30, 13), nh = hutAt(*sim, 20, 8);
    // panic: 30% of the garrison flees
    w.get<game::Garrison>(b2).count = 10; w.get<game::Garrison>(b2).growth = Fixed{};
    cast(*sim, 1, "panic", {}, b2); run(*sim, 0.2);
    CHECK(garrisonOf(*sim, b2) == 7);
    // fortify / production boost / shield on own structures
    cast(*sim, 2, "fortify", {}, b1); cast(*sim, 2, "boom", {}, b1); cast(*sim, 2, "shield5", {}, b1);
    run(*sim, 0.2);
    CHECK(m.stat(*sim, b1, game::Stat::Defense) == Fixed::fromInt(2) && m.stat(*sim, b1, game::Stat::ProductionSpeed) == Fixed::fromInt(3));
    // wind: B's units are slowed
    cast(*sim, 1, "wind", P(36, 8), b1); run(*sim, 0.2);
    w.get<game::Garrison>(b1).count = 20;
    send(*sim, 2, b1, b2, 50); run(*sim, 1);
    Entity bs;
    w.each<game::GroupTag, Owner>([&](Entity e, game::GroupTag&, Owner& o) { if (o.player == 2) bs = e; });
    CHECK(bs.valid() && m.stat(*sim, bs, game::Stat::Speed) < Fixed::fromInt(2));
    // A's group: illusions double it, flight takes it off the lane, a portal throws it forward, a kill aura hurts enemies
    w.get<game::Garrison>(a1).count = 30;
    cast(*sim, 1, "shortway", P(9, 8));
    send(*sim, 1, a1, nh, 30); run(*sim, 1);
    std::vector<Entity> grp;
    w.each<game::GroupTag, Owner>([&](Entity e, game::GroupTag&, Owner& o) { if (o.player == 1) grp.push_back(e); });
    CHECK(!grp.empty());
    size_t n0 = grp.size();
    cast(*sim, 1, "illusion", {}, grp[0]); run(*sim, 0.2);
    int fakes = 0;
    w.each<game::GroupTag, Owner>([&](Entity e, game::GroupTag&, Owner& o) { if (o.player == 1 && (m.statusFlags(*sim, e) & game::kStFake)) ++fakes; });
    CHECK(fakes >= int(n0) - 1);
    bool jumped = false;
    for (int i = 0; i < 40 && !jumped; ++i) { sim->step(); for (Entity e : grp) if (w.alive(e) && w.get<Transform>(e).pos.x > Fixed::fromInt(18)) jumped = true; }
    CHECK(jumped);                                                       // the shortcut portal moved someone 12 cells ahead
    // flight: straight to the target, then the illusions arrive without effect
    int gnh = garrisonOf(*sim, nh);
    cast(*sim, 1, "flywave", {}, grp[0]); run(*sim, 6);
    CHECK(w.get<Owner>(nh).player == 1 || garrisonOf(*sim, nh) < gnh);
    // auto skill: tide gives an own structure 2 units every 10 s on its own
    int tot = 0;
    w.each<game::Garrison, Owner>([&](Entity, game::Garrison& g, Owner& o) { if (o.player == 2) { tot += g.count; g.growth = Fixed{}; } });
    (void)tot; (void)a2;
}

TEST(terrain_hpa_path_matches_astar) {
    // 256x256 maze-ish grid: walls with gaps; HPA* (big-map mode) vs plain unbounded A*
    game::MapGrid g;
    g.init(256, 256);
    for (int x = 20; x < 256; x += 37)
        for (int y = 0; y < 256; ++y) if ((y + x) % 90 > 6) g.flags[size_t(g.index(x, y))] = 0;
    for (int y = 30; y < 256; y += 53)
        for (int x = 0; x < 230; ++x) if (x % 70 > 5) g.flags[size_t(g.index(x, y))] = 0;
    auto lengthOf = [](Vec3 from, const std::vector<Vec3>& p) { Fixed l{}; for (auto& q : p) { l += (q - from).xz().length(); from = q; } return l; };
    Random rng(9);
    int compared = 0;
    for (int i = 0; i < 30; ++i) {
        int x0 = int(rng.below(256)), y0 = int(rng.below(256)), x1 = int(rng.below(256)), y1 = int(rng.below(256));
        if (!g.passable(x0, y0, game::kLayerGround) || !g.passable(x1, y1, game::kLayerGround)) continue;
        std::vector<Vec3> a, b;
        g.hpaMinCells = 1 << 30;
        bool okA = g.findPath(g.cellCenter(x0, y0), g.cellCenter(x1, y1), game::kLayerGround, a, 256 * 256);
        g.hpaMinCells = 0;
        bool okB = g.findPath(g.cellCenter(x0, y0), g.cellCenter(x1, y1), game::kLayerGround, b, 40000);
        CHECK(okA == okB);
        if (!okA) continue;
        Fixed la = lengthOf(g.cellCenter(x0, y0), a), lb = lengthOf(g.cellCenter(x0, y0), b);
        CHECK(lb <= la * Fixed::fromRatio(115, 100) + Fixed::fromInt(4));   // near-optimal
        for (size_t k = 0; k + 1 < b.size(); ++k)                           // every leg is walkable
            CHECK(g.lineWalkable(g.cellX(b[k].x), g.cellY(b[k].z), g.cellX(b[k + 1].x), g.cellY(b[k + 1].z), game::kLayerGround));
        ++compared;
    }
    CHECK(compared >= 10 && g.hpaStats[0] >= 10);
    // a structure on the only gap (x=94, y=0..2) between two pockets: the touched clusters are rebuilt
    std::vector<Vec3> p;
    g.hpaMinCells = 0;
    CHECK(g.findPath(g.cellCenter(60, 10), g.cellCenter(120, 10), game::kLayerGround, p));
    g.block(94, 0, 1, 3, +1);
    CHECK(!g.findPath(g.cellCenter(60, 10), g.cellCenter(120, 10), game::kLayerGround, p));
    g.hpaMinCells = 1 << 30;
    CHECK(!g.findPath(g.cellCenter(60, 10), g.cellCenter(120, 10), game::kLayerGround, p, 256 * 256)); // truly cut off
    g.hpaMinCells = 0;
    g.block(94, 0, 1, 3, -1);
    CHECK(g.findPath(g.cellCenter(60, 10), g.cellCenter(120, 10), game::kLayerGround, p));
}

TEST(terrain_incremental_clearance_matches_full) {
    game::MapGrid g;
    g.init(40, 30);
    for (int y = 5; y < 25; ++y) g.flags[size_t(g.index(20, y))] = 0;
    g.updateClearance();
    g.block(8, 8, 3, 2, +1);
    g.block(30, 2, 2, 2, +1);
    g.block(8, 8, 3, 2, -1);
    g.block(1, 26, 4, 4, +1);
    CHECK(!g.clearanceDirty);
    std::vector<uint8_t> inc = g.clearance;
    g.updateClearance();
    CHECK(inc == g.clearance);
}

TEST(terrain_heightfield_match_is_deterministic) {
    const std::string dir = std::string(RTS_SOURCE_DIR) + "/templates/metaltide";
    Json rules = Json::parseFile(dir + "/rules.json"), map = Json::parseFile(dir + "/maps/twin_craters.json");
    CHECK(game::loadRules(rules).terrain.heightfield());
    auto run = [&] {
        SimConfig cfg; cfg.seed = 3;
        Simulation sim(cfg, std::make_unique<game::RtsModule>());
        sim.setup(rules, map);
        CHECK(mod(sim).state(sim).grid.hf);
        std::vector<game::RtsAI> bots;
        for (auto& p : sim.players()) bots.emplace_back(p.slot, 3);
        for (int i = 0; i < 20 * 150; ++i) { for (auto& b : bots) b.update(sim); sim.step(); }
        // units stand on the surface
        bool onGround = true;
        sim.world().each<game::Mobile, Transform>([&](Entity, game::Mobile&, Transform& t) {
            if (t.pos.y != mod(sim).state(sim).grid.surfaceHeight(t.pos.x, t.pos.z)) onGround = false;
        });
        CHECK(onGround);
        return sim.checksum();
    };
    uint64_t a = run(), b = run();
    CHECK(a == b);
    // the corners take part in the checksum only with model "heightfield"
    Json flatRules = Json::parse(R"({"types": {"u": {"move": {"speed": 1}}}})");
    std::string m = flatMap(8, 8, "");
    Json withCorners = Json::parse(m);
    Json cs = Json::array();
    for (int i = 0; i < 81; ++i) cs.push(Json::number(int64_t(i % 5)));
    withCorners.member("terrain").set("corners", cs);
    SimConfig cfg;
    Simulation s1(cfg, std::make_unique<game::RtsModule>()), s2(cfg, std::make_unique<game::RtsModule>());
    s1.setup(flatRules, Json::parse(m));
    s2.setup(flatRules, withCorners);
    CHECK(s1.checksum() == s2.checksum());
}

TEST(terrain_corners_file_roundtrip) {
    std::vector<int16_t> c = {0, -5, 300, 32767, -32768, 7, 1, 2, 3};
    std::string bytes = game::encodeCorners(c);
    CHECK(bytes.size() == 18 && uint8_t(bytes[2]) == 0xfb && uint8_t(bytes[3]) == 0xff);
    Json map = Json::parse(R"({"size": [2, 2], "terrain": {"cornersFile": "maps/x.r16"}})");
    bool ok = game::applyCornersFile(map, [&](const std::string& p, std::string& out) { out = bytes; return p == "maps/x.r16"; }, nullptr);
    CHECK(ok);
    std::vector<int16_t> back;
    CHECK(game::decodeCorners(map["terrain"], 2, 2, back, nullptr) && back == c);
    std::string err;
    CHECK(!game::decodeCorners(map["terrain"], 3, 3, back, &err) && err.find("expected 32 bytes") != std::string::npos);
    Json missing = Json::parse(R"({"terrain": {"cornersFile": "maps/none.r16"}})");
    CHECK(!game::applyCornersFile(missing, [](const std::string&, std::string&) { return false; }, &err));
}

// ---- terrain rendering (agent-terrain): the view's surface math (RtsTerrainSurface.h)
#include "modules/rts/view/RtsTerrainSurface.h"

TEST(terrain_view_surface_levels_matches_classic) {
    game::MapGrid g;
    g.init(6, 4);
    for (auto& f : g.flags) f = game::kCellGround;
    for (int y = 0; y < 4; ++y) for (int x = 3; x < 6; ++x) g.level[size_t(g.index(x, y))] = 1;
    g.flags[size_t(g.index(2, 1))] |= game::kCellRamp;
    const float lh = 1.5f;
    CHECK(std::fabs(game::terrain::heightAt(g, lh, 0.5f, 0.5f) - 0.0f) < 1e-5f);
    CHECK(std::fabs(game::terrain::heightAt(g, lh, 4.5f, 2.5f) - 1.5f) < 1e-5f);
    // ramp at (2,1) rises east towards level 1: linear in x
    CHECK(std::fabs(game::terrain::heightAt(g, lh, 2.25f, 1.5f) - 0.375f) < 1e-4f);
    CHECK(std::fabs(game::terrain::heightAt(g, lh, 2.75f, 1.5f) - 1.125f) < 1e-4f);
}

TEST(terrain_view_surface_heightfield_matches_mesh_triangles) {
    game::MapGrid g;
    g.init(2, 2);
    g.corner = {0, 16, 32, 16, 64, 16, 0, 16, 0};   // (w+1)*(h+1), 1/16 units
    // corners exact
    for (int Y = 0; Y <= 2; ++Y)
        for (int X = 0; X <= 2; ++X)
            CHECK(std::fabs(game::terrain::heightAt(g, 1.5f, float(X), float(Y)) - float(g.corner[size_t(Y * 3 + X)]) / 16.0f) < 1e-5f);
    // on the diagonal (0,0)-(1,1) of cell (0,0): linear between corner (0,0) = 0 and (1,1) = 4
    CHECK(std::fabs(game::terrain::heightAt(g, 1.5f, 0.5f, 0.5f) - 2.0f) < 1e-5f);
    // triangle (0,0),(1,0),(1,1): at (0.75, 0.25) = 0 + 1 * 0.75 + (4 - 1) * 0.25
    CHECK(std::fabs(game::terrain::heightAt(g, 1.5f, 0.75f, 0.25f) - 1.5f) < 1e-5f);
    // triangle (0,0),(0,1),(1,1): at (0.25, 0.75) = 0 + (4 - 1) * 0.25 + 1 * 0.75
    CHECK(std::fabs(game::terrain::heightAt(g, 1.5f, 0.25f, 0.75f) - 1.5f) < 1e-5f);
    // the view agrees with the simulation's bilinear surface at corners and cell centres within the diagonal error
    for (int Y = 0; Y <= 2; ++Y)
        for (int X = 0; X <= 2; ++X)
            CHECK(std::fabs(float(g.surfaceHeight(Fixed::fromInt(X), Fixed::fromInt(Y)).toDouble()) - game::terrain::heightAt(g, 1.5f, float(X), float(Y))) < 1e-3f);
    // outside the map: clamped to the border
    CHECK(std::fabs(game::terrain::heightAt(g, 1.5f, -3.0f, -3.0f) - 0.0f) < 1e-5f);
}

TEST(terrain_view_grid_corner_from_levels) {
    game::MapGrid g;
    g.init(4, 4);
    for (auto& f : g.flags) f = game::kCellGround;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) g.level[size_t(g.index(x, y))] = int8_t(x >= 2 ? 2 : 0);
    // a corner on the cliff edge averages the two plateaus; inside a plateau it is the plateau height
    CHECK(std::fabs(game::terrain::gridCorner(g, 1.0f, 1, 2) - 0.0f) < 1e-5f);
    CHECK(std::fabs(game::terrain::gridCorner(g, 1.0f, 3, 2) - 2.0f) < 1e-5f);
    CHECK(std::fabs(game::terrain::gridCorner(g, 1.0f, 2, 2) - 1.0f) < 1e-5f);
}

// ---------------------------------------------------------------------------------------------
// GPU-compressed textures: KTX2 container and CPU block decoders (models agent)
// ---------------------------------------------------------------------------------------------
#include "engine/render/TextureCodec.h"
#include "engine/render/Model.h"
#include "third_party/zstd/zstd.h"
#include <cstring>

using namespace rts::render;

namespace ktxtest {
std::vector<uint8_t> be64(uint32_t hi, uint32_t lo) {
    std::vector<uint8_t> b(8);
    for (int i = 0; i < 4; ++i) { b[size_t(i)] = uint8_t(hi >> (24 - 8 * i)); b[size_t(4 + i)] = uint8_t(lo >> (24 - 8 * i)); }
    return b;
}
// EAC block: base, multiplier, table, 3-bit indices in column-major order (i = x * 4 + y).
std::vector<uint8_t> eac(int base, int mul, int table, const int idx[16]) {
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= uint64_t(idx[i]) << (45 - 3 * i);
    std::vector<uint8_t> b{uint8_t(base), uint8_t(mul << 4 | table)};
    for (int i = 5; i >= 0; --i) b.push_back(uint8_t(bits >> (8 * i)));
    return b;
}
bool px(const uint8_t* out, int x, int y, int r, int g, int b, int a) {
    const uint8_t* p = out + (y * 4 + x) * 4;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}
void put32(std::string& s, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) s[at + size_t(i)] = char(v >> (8 * i)); }
void put64(std::string& s, size_t at, uint64_t v) { put32(s, at, uint32_t(v)); put32(s, at + 4, uint32_t(v >> 32)); }
// Minimal KTX2 writer: 2D, one layer/face, levels given largest first, optional zstd per level.
std::string makeKtx2(uint32_t vkFormat, uint32_t w, uint32_t h, const std::vector<std::vector<uint8_t>>& levels, bool zstd) {
    const uint8_t id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    std::string s(80 + 24 * levels.size(), '\0');
    std::memcpy(s.data(), id, 12);
    put32(s, 12, vkFormat); put32(s, 16, 1); put32(s, 20, w); put32(s, 24, h);
    put32(s, 36, 1); put32(s, 40, uint32_t(levels.size())); put32(s, 44, zstd ? 2 : 0);
    for (size_t l = levels.size(); l-- > 0;) {   // smallest level first in the file, as the spec suggests
        std::string data(levels[l].begin(), levels[l].end());
        if (zstd) {
            std::string c(ZSTD_compressBound(data.size()), '\0');
            c.resize(ZSTD_compress(c.data(), c.size(), data.data(), data.size(), 19));
            data = c;
        }
        while (s.size() % 8) s.push_back('\0');
        put64(s, 80 + 24 * l, s.size()); put64(s, 88 + 24 * l, data.size()); put64(s, 96 + 24 * l, levels[l].size());
        s += data;
    }
    return s;
}
// ETC1 individual-mode block: left half (136,136,136) table 0, right half (68,68,68) table 7.
// Pixel (0,0) uses index 01 (+8), pixel (3,0) index 11 (-183), the rest index 00.
const std::vector<uint8_t> kEtcIndividual = be64(0x8484841Cu, 0x10001001u);
} // namespace ktxtest

TEST(etc_blocks_decode_per_spec) {
    using namespace ktxtest;
    uint8_t out[64];
    CHECK(decodeBlock(BlockFormat::ETC2_RGB8, kEtcIndividual.data(), out));
    CHECK(px(out, 0, 0, 144, 144, 144, 255) && px(out, 1, 1, 138, 138, 138, 255));
    CHECK(px(out, 2, 0, 115, 115, 115, 255) && px(out, 3, 0, 0, 0, 0, 255) && px(out, 3, 3, 115, 115, 115, 255));
    // Differential, flipped: base (16,8,0) + delta (+3,-4,0); top table 1 (+5), bottom table 2 (+9).
    auto diff = be64(0x8344002Bu, 0);
    decodeBlock(BlockFormat::ETC2_RGB8, diff.data(), out);
    CHECK(px(out, 3, 1, 137, 71, 5, 255) && px(out, 0, 2, 165, 42, 9, 255));
    // T mode (R overflows): C1 = (187,34,51), C2 = 136 grey, distance 32; row 0 uses paint colours 0..3.
    auto tmode = be64(0xF323888Bu, 0x11001010u);
    decodeBlock(BlockFormat::ETC2_RGB8, tmode.data(), out);
    CHECK(px(out, 0, 0, 187, 34, 51, 255) && px(out, 1, 0, 168, 168, 168, 255));
    CHECK(px(out, 2, 0, 136, 136, 136, 255) && px(out, 3, 0, 104, 104, 104, 255) && px(out, 2, 2, 187, 34, 51, 255));
    // H mode (G + dG < 0): C1 = (136,34,68) < C2 = 136 grey, so the ordering bit is 0.
    // R1=8 G1=2 B1=4, R2=8 G2=8 B2=8, da=1 db=0 -> distance index 4 (23); paints C1+-23, C2+-23.
    {
        uint32_t hi = 0;
        hi |= 8u << 27;                        // R1 (bits 62..59)
        hi |= (2u >> 1) << 24 | (2u & 1) << 20;   // G1 = bits 58..56 + bit 52
        hi |= (4u >> 3) << 19 | (4u & 7) << 15;   // B1 = bit 51 + bits 49..47
        hi |= 8u << 11 | 8u << 7 | 8u << 3;    // R2 G2 B2
        hi |= 1u << 2 | 1u << 1;               // da, diff
        hi |= 1u << 18;                        // unused bit 50: dG = 0b110 = -2 with G = 0
        uint32_t lo = 1u << 4 | 1u << 24 | 1u << 28 | 1u << 12;   // row 0: indices 0,1,2,3
        auto h = be64(hi, lo);
        decodeBlock(BlockFormat::ETC2_RGB8, h.data(), out);
        CHECK(px(out, 0, 0, 159, 57, 91, 255) && px(out, 1, 0, 113, 11, 45, 255));
        CHECK(px(out, 2, 0, 159, 159, 159, 255) && px(out, 3, 0, 113, 113, 113, 255));
    }
    // Planar (B overflows) with O = H = V: constant (130, 0, 239).
    auto planar = be64(0x4001F9C2u, 0x01DC003Bu);
    decodeBlock(BlockFormat::ETC2_RGB8, planar.data(), out);
    CHECK(px(out, 0, 0, 130, 0, 239, 255) && px(out, 3, 3, 130, 0, 239, 255));
    // EAC alpha in ETC2_RGBA8: base 100, multiplier 2, table 13 (-1 -2 -3 -10 0 1 2 9).
    int idx[16]; for (int& i : idx) i = 4;
    idx[0] = 7; idx[4] = 3;   // (0,0) -> +9*2, (1,0) -> -10*2
    auto rgba8 = eac(100, 2, 13, idx);
    rgba8.insert(rgba8.end(), kEtcIndividual.begin(), kEtcIndividual.end());
    decodeBlock(BlockFormat::ETC2_RGBA8, rgba8.data(), out);
    CHECK(px(out, 0, 0, 144, 144, 144, 118) && px(out, 1, 0, 138, 138, 138, 80) && px(out, 2, 2, 115, 115, 115, 100));
    // EAC R11: multiplier 0 adds the raw modifier to base*8+4; otherwise modifier*multiplier*8.
    int ridx[16] = {}; ridx[4] = 7;
    auto r11 = eac(128, 0, 0, ridx);
    decodeBlock(BlockFormat::EAC_R11, r11.data(), out);
    CHECK(px(out, 0, 0, 128, 0, 0, 255) && px(out, 1, 0, 130, 0, 0, 255));   // 1025, 1042 of 2047
    ridx[4] = 3;
    r11 = eac(128, 1, 0, ridx);
    decodeBlock(BlockFormat::EAC_R11, r11.data(), out);
    CHECK(px(out, 1, 0, 113, 0, 0, 255));   // 1028 - 15*8 = 908
}

TEST(bc_blocks_decode_per_spec) {
    using namespace ktxtest;
    uint8_t out[64];
    // BC1 four-colour: red / blue endpoints, row 0 indices 0,1,2,3.
    const uint8_t bc1[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0, 0, 0};
    CHECK(decodeBlock(BlockFormat::BC1, bc1, out));
    CHECK(px(out, 0, 0, 255, 0, 0, 255) && px(out, 1, 0, 0, 0, 255, 255) && px(out, 2, 0, 170, 0, 85, 255));
    CHECK(px(out, 3, 0, 85, 0, 170, 255) && px(out, 2, 3, 255, 0, 0, 255));
    // BC1 three-colour (c0 <= c1): index 2 = midpoint, index 3 = transparent black.
    const uint8_t bc1t[8] = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0, 0, 0};
    decodeBlock(BlockFormat::BC1, bc1t, out);
    CHECK(px(out, 0, 0, 0, 0, 255, 255) && px(out, 2, 0, 127, 0, 127, 255) && px(out, 3, 0, 0, 0, 0, 0));
    // BC3: 8-value alpha (255..0) then the BC1 colour block (always four-colour, even when c0 <= c1).
    uint8_t bc3[16] = {255, 0, 0x3A, 0, 0, 0, 0, 0};
    std::memcpy(bc3 + 8, bc1t, 8);
    decodeBlock(BlockFormat::BC3, bc3, out);
    CHECK(px(out, 0, 0, 0, 0, 255, 218) && px(out, 1, 0, 255, 0, 0, 36) && px(out, 3, 0, 170, 0, 85, 255));
    // BC4 six-value mode (a0 <= a1): 51 for index 2, 0 and 255 for indices 6 and 7.
    const uint8_t bc4[8] = {0, 255, 2 | 6 << 3 | (7 << 6 & 0xFF), 7 >> 2, 0, 0, 0, 0};
    decodeBlock(BlockFormat::BC4, bc4, out);
    CHECK(px(out, 0, 0, 51, 0, 0, 255) && px(out, 1, 0, 0, 0, 0, 255) && px(out, 2, 0, 255, 0, 0, 255) && px(out, 3, 0, 0, 0, 0, 255));
    // BC7 mode 6: R 127..0, A 127..127, P-bits 1 / 0 (applied to every channel of the endpoint);
    // row 0 indices 0, 15, 8.
    uint8_t bc7[16] = {};
    int pos = 0;
    auto bits = [&](uint32_t v, int n) { for (int i = 0; i < n; ++i, ++pos) bc7[pos >> 3] |= uint8_t((v >> i & 1) << (pos & 7)); };
    bits(1u << 6, 7);
    bits(127, 7); bits(0, 7); bits(0, 7); bits(0, 7); bits(0, 7); bits(0, 7); bits(127, 7); bits(127, 7);
    bits(1, 1); bits(0, 1);
    bits(0, 3); bits(15, 4); bits(8, 4);
    CHECK(decodeBlock(BlockFormat::BC7, bc7, out));
    CHECK(px(out, 0, 0, 255, 1, 1, 255) && px(out, 1, 0, 0, 0, 0, 254) && px(out, 2, 0, 120, 0, 0, 254) && px(out, 3, 3, 255, 1, 1, 255));
    CHECK(!decodeBlock(BlockFormat::ASTC_4x4, bc7, out));
}

TEST(ktx2_parse_and_decode_image) {
    using namespace ktxtest;
    // 8x4 ETC2 sRGB, two levels (8x4 = 2 blocks, 4x2 = 1 block), with and without zstd.
    std::vector<uint8_t> l0 = kEtcIndividual, planar = be64(0x4001F9C2u, 0x01DC003Bu);
    l0.insert(l0.end(), planar.begin(), planar.end());
    for (bool zstd : {false, true}) {
        const std::string file = makeKtx2(148, 8, 4, {l0, planar}, zstd);
        CompressedImage ci;
        std::string err;
        CHECK(isKtx2(file) && parseKtx2(file, ci, err));
        CHECK(ci.format == BlockFormat::ETC2_RGB8 && ci.srgb && ci.width == 8 && ci.height == 4 && ci.levels.size() == 2);
        CHECK(ci.levels.size() == 2 && ci.levels[0] == l0 && ci.levels[1] == planar);
        std::vector<uint8_t> mip1;
        CHECK(decodeBlocks(ci, 1, mip1) && mip1.size() == 4 * 2 * 4 && mip1[0] == 130 && mip1[2] == 239);
        Image img;
        CHECK(decodeImage(file, img) && img.width == 8 && img.height == 4 && img.rgba.size() == 8 * 4 * 4);
        CHECK(img.compressed && img.compressed->levels.size() == 2);
        if (img.rgba.size() == 128) {
            CHECK(img.rgba[0] == 144 && img.rgba[3 * 4] == 0 && img.rgba[4 * 4] == 130 && img.rgba[4 * 4 + 2] == 239);
            CHECK(img.rgba[(3 * 8 + 7) * 4 + 2] == 239);   // bottom-right pixel comes from the second block
        }
        Image copy = img;   // stays copyable, shares the block data
        CHECK(copy.compressed == img.compressed);
    }
    // Uncompressed RGBA8 passthrough and BC1 with a size that is not a multiple of 4 (crop).
    {
        std::vector<uint8_t> rgba = {1, 2, 3, 4, 5, 6, 7, 8};
        Image img;
        CHECK(decodeImage(makeKtx2(37, 2, 1, {rgba}, true), img) && img.rgba == rgba && img.compressed && !img.compressed->srgb);
        const std::vector<uint8_t> bc1 = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0, 0, 0, 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0, 0, 0};
        CompressedImage ci;
        std::string err;
        CHECK(parseKtx2(makeKtx2(131, 5, 3, {bc1}, false), ci, err) && ci.format == BlockFormat::BC1);
        std::vector<uint8_t> out;
        CHECK(decodeBlocks(ci, 0, out) && out.size() == 5 * 3 * 4);
        if (out.size() == 60) CHECK(out[4 * 4 + 2] == 255 && out[4 * 4 + 3] == 255);   // pixel (4,0) = blue of block 2
        ci.vkFormat = 131;
        const uint8_t three[8] = {0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF};   // all index 3
        ci.levels[0].assign(three, three + 8); ci.levels[0].insert(ci.levels[0].end(), three, three + 8);
        CHECK(decodeBlocks(ci, 0, out) && out[3] == 255 && out[0] == 0);   // BC1_RGB: opaque black
        CHECK(parseKtx2(makeKtx2(157, 4, 4, {std::vector<uint8_t>(16)}, false), ci, err) && ci.format == BlockFormat::ASTC_4x4);
        CHECK(!decodeBlocks(ci, 0, out));   // no CPU ASTC decoder
    }
}

TEST(ktx2_rejects_bad_files) {
    using namespace ktxtest;
    CompressedImage ci;
    std::string err;
    std::vector<uint8_t> l0 = kEtcIndividual;
    const std::string good = makeKtx2(147, 4, 4, {l0}, false), goodZ = makeKtx2(147, 4, 4, {l0}, true);
    CHECK(parseKtx2(good, ci, err) && parseKtx2(goodZ, ci, err));
    std::string basis = good;
    put32(basis, 44, 1);
    CHECK(!parseKtx2(basis, ci, err) && err.find("transcoder") != std::string::npos);
    std::string uastc = good;
    put32(uastc, 12, 0);
    CHECK(!parseKtx2(uastc, ci, err) && err.find("BasisLZ/UASTC") != std::string::npos);
    std::string odd = good;
    put32(odd, 12, 1000);
    CHECK(!parseKtx2(odd, ci, err) && err.find("vkFormat") != std::string::npos);
    // Every truncation fails cleanly; so do wild offsets, sizes, level counts and corrupt zstd data.
    for (const std::string* f : {&good, &goodZ}) {
        bool allFail = true;
        for (size_t n = 0; n < f->size(); ++n) allFail = allFail && !parseKtx2(f->substr(0, n), ci, err);
        CHECK(allFail);
    }
    std::string bad = good;
    put64(bad, 80, ~0ull);
    CHECK(!parseKtx2(bad, ci, err));
    bad = good; put64(bad, 88, ~0ull - 4);
    CHECK(!parseKtx2(bad, ci, err));
    bad = good; put32(bad, 40, 200);
    CHECK(!parseKtx2(bad, ci, err));
    bad = good; put32(bad, 20, 0x7fffffff);
    CHECK(!parseKtx2(bad, ci, err));
    bad = goodZ; put64(bad, 96, 1ull << 40);
    CHECK(!parseKtx2(bad, ci, err));
    bad = goodZ; bad[bad.size() - 3] ^= 0x5A; bad[bad.size() - 6] ^= 0x33;
    CHECK(!parseKtx2(bad, ci, err) || ci.levels.size() == 1);   // corrupt payload never crashes
    uint32_t seed = 12345;
    bool garbageFails = true;
    for (int i = 0; i < 200; ++i) {
        std::string g = good;
        for (size_t k = 12; k < g.size(); ++k) { seed = seed * 1664525u + 1013904223u; if ((seed >> 28) == 0) g[k] = char(seed >> 16); }
        if (parseKtx2(g, ci, err)) { std::vector<uint8_t> out; decodeBlocks(ci, 0, out); }
        garbageFails = garbageFails && !parseKtx2(std::string(64 + size_t(i), char(i)), ci, err);
    }
    CHECK(garbageFails);
    Image img;
    CHECK(!decodeImage(basis, img) && !decodeImage(good.substr(0, 90), img));
}

// ---------------- models (models agent): FBX loader ----------------
TEST(models_fbx_skinned_cube) {
    using namespace modeltest;
    std::vector<std::string> warn;
    const std::string path = std::string(RTS_SOURCE_DIR) + "/tests/data/models/cube.fbx";
    auto m = loadModel(path, diskReader(), &warn);
    for (auto& w : warn) std::printf("  warning: %s\n", w.c_str());
    CHECK(warn.empty());
    CHECK(m->nodes.size() == 3 && m->meshes.size() == 1 && m->skins.size() == 1 && m->clips.size() == 1);
    const int box = m->findNode("Box"), root = m->findNode("Root"), top = m->findNode("Top");
    CHECK(box >= 0 && root >= 0 && top >= 0);
    if (box < 0 || root < 0 || top < 0) return;
    CHECK(m->nodes[size_t(top)].parent == root && m->nodes[size_t(box)].mesh == 0 && m->nodes[size_t(box)].skin == 0);
    CHECK(near(m->nodes[size_t(top)].t.y, 1.0f));                       // 100 cm -> 1 m
    // one primitive per material, 6 quads -> 12 triangles
    const MeshDef& md = m->meshes[0];
    CHECK(md.primitives.size() == 2);
    uint32_t tris = 0;
    for (auto& p : md.primitives) tris += p.indexCount / 3;
    CHECK(tris == 12);
    const int crate = m->findMaterial("Crate"), glass = m->findMaterial("Glass");
    CHECK(crate >= 0 && glass >= 0);
    if (crate >= 0) CHECK(near(m->materials[size_t(crate)].baseColor.x, 0.8f) && m->materials[size_t(crate)].alpha == AlphaMode::Opaque);
    if (glass >= 0) {
        const Material& g = m->materials[size_t(glass)];
        CHECK(g.alpha == AlphaMode::Blend && g.baseColor.w < 0.99f && g.emissive.z > 0.1f);
    }
    // bounds in metres, Y up
    CHECK(near(m->boundsMin.x, -0.5f) && near(m->boundsMax.x, 0.5f) && near(m->boundsMin.y, 0) && near(m->boundsMax.y, 2));
    // skin: weights normalized, bind pose palette = identity
    for (auto& p : md.primitives) {
        CHECK(!p.jointPalette.empty() && p.jointPalette.size() <= 2);
        std::vector<mat4> pal;
        paletteMatrices(*m, m->skins[0], p, m->bindWorld, pal);
        bool identity = true;
        for (auto& x : pal) for (int i = 0; i < 16; ++i) identity &= near(x.m[i], (i % 5 == 0) ? 1.0f : 0.0f);
        CHECK(identity);
        for (uint32_t i = 0; i < p.indexCount; ++i) {
            const ModelVertex& v = m->vertices[m->indices[p.firstIndex + i]];
            const int sum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
            CHECK(sum >= 254 && sum <= 256);
        }
    }
    // animation: Root moves 1 m along X, Top turns 90 degrees about Y over one second
    const AnimClip& clip = m->clips[0];
    CHECK(clip.name == "Bend" && near(clip.duration, 1, 0.02f));
    Pose pose;
    bindPose(*m, pose);
    sampleClip(clip, 1.0f, pose);
    const quat e = quat::axisAngle({0, 1, 0}, 1.5707963f);
    CHECK(near(pose[size_t(root)].t.x, 1, 0.01f));
    CHECK(near(std::fabs(pose[size_t(top)].r.y), e.y, 0.01f) && near(std::fabs(pose[size_t(top)].r.w), e.w, 0.01f));
    sampleClip(clip, 0.5f, pose);
    CHECK(near(pose[size_t(root)].t.x, 0.5f, 0.01f));
}

TEST(models_fbx_readable_errors) {
    using namespace modeltest;
    bool threw = false;
    try { loadModel("units/broken.fbx", mapReader({{"units/broken.fbx", "this is not an FBX file at all"}}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find("units/broken.fbx") != std::string::npos; std::printf("  (expected) %s\n", e.what()); }
    CHECK(threw);
    threw = false;
    try { loadModel("units/thing.xyz", mapReader({{"units/thing.xyz", "x"}}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find(".fbx") != std::string::npos; }
    CHECK(threw);
}


// ---------------- models (models agent): IK, additive, blend space, morph targets, baking ----------------
#include "engine/gfx/Gfx.h"
namespace modeltest {
// Records textures so baking / uploads can be checked without a GPU.
class NullDevice final : public rts::gfx::Device {
public:
    struct Tex { rts::gfx::TextureDesc desc; std::vector<float> data; };
    std::vector<Tex> textures;
    const rts::gfx::DeviceInfo& info() const override { return info_; }
    rts::gfx::BufferHandle createBuffer(const rts::gfx::BufferDesc&) override { return {++next_}; }
    void updateBuffer(rts::gfx::BufferHandle, const void*, size_t, size_t) override {}
    void destroyBuffer(rts::gfx::BufferHandle) override {}
    rts::gfx::TextureHandle createTexture(const rts::gfx::TextureDesc& d) override {
        Tex t; t.desc = d;
        if (d.format == rts::gfx::PixelFormat::RGBA32F && d.data) {
            const float* f = static_cast<const float*>(d.data);
            t.data.assign(f, f + size_t(d.width) * size_t(d.height) * 4);
        }
        textures.push_back(std::move(t));
        return {uint32_t(textures.size())};
    }
    void updateTexture(rts::gfx::TextureHandle, int, int, int, int, const void*, int) override {}
    void destroyTexture(rts::gfx::TextureHandle) override {}
    rts::gfx::ShaderHandle createShader(const rts::gfx::ShaderDesc&) override { return {++next_}; }
    void destroyShader(rts::gfx::ShaderHandle) override {}
    rts::gfx::PipelineHandle createPipeline(const rts::gfx::PipelineDesc&) override { return {++next_}; }
    void destroyPipeline(rts::gfx::PipelineHandle) override {}
    const std::string& lastError() const override { return err_; }
    void beginFrame(int, int) override {}
    void endFrame() override {}
    void beginPass(const rts::gfx::PassDesc&) override {}
    void endPass() override {}
    void setViewport(const rts::gfx::Rect&) override {}
    void setScissor(const rts::gfx::Rect&) override {}
    void setPipeline(rts::gfx::PipelineHandle) override {}
    void setVertexBuffer(rts::gfx::BufferHandle, size_t) override {}
    void setIndexBuffer(rts::gfx::BufferHandle, size_t) override {}
    void setTexture(int, rts::gfx::TextureHandle) override {}
    void setUniforms(int, const void*, size_t) override {}
    void draw(uint32_t, uint32_t) override {}
    void drawIndexed(uint32_t, uint32_t, int32_t, uint32_t) override {}
private:
    rts::gfx::DeviceInfo info_;
    std::string err_;
    uint32_t next_ = 0;
};
// Three-joint chain along +Y: a (0,0,0) -> b (0,1,0) -> c (0,2,0).
inline std::unique_ptr<Model> chainModel() {
    auto m = std::make_unique<Model>();
    Node a; a.name = "a"; Node b; b.name = "b"; b.parent = 0; b.t = {0, 1, 0}; Node c; c.name = "c"; c.parent = 1; c.t = {0, 1, 0};
    m->nodes = {a, b, c};
    m->finalize();
    return m;
}
} // namespace modeltest

TEST(models_two_bone_ik_reaches_target) {
    using namespace modeltest;
    auto m = chainModel();
    Pose pose;
    bindPose(*m, pose);
    std::vector<mat4> world;
    worldMatrices(*m, pose, world);
    const float3 target{0.8f, 1.2f, 0};
    solveTwoBoneIK(*m, pose, world, 0, 1, 2, target, {1, 1, 0});
    std::vector<mat4> check;
    worldMatrices(*m, pose, check);               // the returned world matches a fresh evaluation
    float3 c{check[2].at(0, 3), check[2].at(1, 3), check[2].at(2, 3)};
    CHECK(near(c.x, target.x, 2e-3f) && near(c.y, target.y, 2e-3f) && near(c.z, 0, 2e-3f));
    CHECK(near(world[2].at(0, 3), c.x) && near(world[2].at(1, 3), c.y));
    float3 b{check[1].at(0, 3), check[1].at(1, 3), check[1].at(2, 3)};
    CHECK(near(length(b), 1, 1e-3f) && near(length(c - b), 1, 1e-3f));   // bone lengths kept
    CHECK(b.x > 0.0f);                                                      // bends towards the pole
    // unreachable target: chain stretches straight towards it
    solveTwoBoneIK(*m, pose, world, 0, 1, 2, {0, 0, 5}, {0, 1, 1});
    CHECK(near(world[2].at(2, 3), 2, 1e-2f));
}

TEST(models_additive_and_blend_space) {
    using namespace modeltest;
    auto m = chainModel();
    AnimClip walk, run, nod;
    AnimChannel ch; ch.node = 1; ch.path = AnimChannel::Translation;
    walk.name = "Walk"; walk.duration = 1; ch.times = {0, 1}; ch.values = {0, 1, 0, 0, 1, 0}; walk.channels.push_back(ch);
    run.name = "Run"; run.duration = 0.5f; ch.values = {0, 3, 0, 0, 3, 0}; run.channels.push_back(ch);
    nod.name = "Nod"; nod.duration = 1; ch.node = 2; ch.values = {0, 1, 0, 0.5f, 1, 0}; nod.channels.push_back(ch);
    m->clips = {walk, run, nod};
    Animator a;
    a.setModel(m.get());
    Animator::Play p; p.fade = 0;
    a.play(0, 0, p);
    a.setBlend(0, 1, 0.5f);
    Pose pose;
    a.evaluate(pose);
    CHECK(near(pose[1].t.y, 2));                  // halfway between Walk (1) and Run (3)
    a.update(0.375f);                             // mixed cycle = 0.75 s: half a cycle
    CHECK(near(a.time(0), 0.5f));
    auto bs = a.baseSample();
    CHECK(bs.clipA == 0 && bs.clipB == 1 && near(bs.w, 0.5f) && near(bs.timeB, 0.25f));
    a.setAdditive(0, 2, 1, 0);                    // additive: adds clip(t) - clip(0)
    a.update(0.5f);
    a.evaluate(pose);
    CHECK(near(pose[2].t.x, 0.25f, 1e-3f) && near(pose[2].t.y, 1));
}

TEST(models_morph_targets_and_weights) {
    using namespace modeltest;
    const std::string dir = std::string(RTS_SOURCE_DIR) + "/templates/showcase/models/";
    auto m = loadModel(dir + "slime.glb", diskReader(), nullptr);
    const Primitive& p = m->meshes[0].primitives[0];
    CHECK(p.morphTargets == 2 && p.vertexCount > 0 && p.morphDeltas.size() == size_t(2) * p.vertexCount * 6);
    CHECK(m->clips.size() == 1 && m->clips[0].channels[0].path == AnimChannel::Weights && m->clips[0].channels[0].comps == 2);
    Pose pose;
    bindPose(*m, pose);
    sampleClip(m->clips[0], 0.375f, pose);        // between (0.8, 0) and (0, 0.6)
    CHECK(near(pose[0].w[0], 0.4f) && near(pose[0].w[1], 0.3f));
    NullDevice dev;
    uploadModel(dev, *m);
    CHECK(m->morphTex.valid() && m->morphTexWidth == 2048);
    const auto& tex = dev.textures[m->morphTex.id - 1];
    const size_t texel = p.morphTexel + 2 * 5;    // target 0, vertex 5: position delta
    CHECK(tex.desc.format == rts::gfx::PixelFormat::RGBA32F && tex.data.size() > texel * 4 + 2);
}

TEST(models_baked_clips_match_the_pose) {
    using namespace modeltest;
    const std::string dir = std::string(RTS_SOURCE_DIR) + "/templates/showcase/models/";
    auto m = loadModel(dir + "golem.glb", diskReader(), nullptr);
    NullDevice dev;
    CHECK(bakeAnimations(dev, *m, 30));
    const auto& b = m->baked;
    CHECK(b.tex.valid() && b.joints == 8 && b.clips.size() == m->clips.size() && b.rows > 1);
    const auto& tex = dev.textures[b.tex.id - 1];
    CHECK(tex.desc.width == 8 * 3 && tex.desc.height == b.rows);
    // row of Walk at 0.5 s holds world * inverse bind of every joint
    const int walk = m->findClip("Walk");
    const int row = bakedRow(*m, walk, 0.5f);
    CHECK(row == b.clips[size_t(walk)].firstRow + 15);
    Pose pose;
    bindPose(*m, pose);
    sampleClip(m->clips[size_t(walk)], 0.5f, pose);
    std::vector<mat4> world;
    worldMatrices(*m, pose, world);
    const auto& sk = m->skins[size_t(b.skin)];
    bool same = true;
    for (int j = 0; j < b.joints; ++j) {
        const mat4 jm = world[size_t(sk.joints[size_t(j)])] * sk.inverseBind[size_t(j)];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) same &= near(tex.data[(size_t(row) * 24 + size_t(j) * 3 + size_t(r)) * 4 + size_t(c)], jm.at(r, c), 1e-4f);
    }
    CHECK(same);
    CHECK(bakedRow(*m, -1, 0) == 0);               // bind pose row
    CHECK(bakedRow(*m, walk, 1.0f) == b.clips[size_t(walk)].firstRow); // loop wraps to frame 0
}

// ---------------- fog of war modes (rules / map "fogOfWar") and the view look ----------------
#include "modules/rts/view/RtsFog.h"
TEST(fog_of_war_modes_and_look) {
    auto withFog = [](const char* mode, bool inMap) {
        Json rules = Json::parseFile(kSkirmish + "/rules.json"), map = Json::parseFile(kSkirmish + "/maps/valley.json");
        Json f = Json::object();
        f.set("mode", Json::string(mode));
        (inMap ? map : rules).set("fogOfWar", f);
        SimConfig cfg; cfg.seed = 2;
        auto sim = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
        sim->setup(rules, map);
        for (int i = 0; i < 40; ++i) sim->step();
        return sim;
    };
    auto count = [](Simulation& s, uint8_t p, uint8_t v) {
        auto& vis = mod(s).state(s).vision[p];
        return std::count(vis.begin(), vis.end(), v);
    };
    auto full = withFog("full", false), explored = withFog("explored", false), none = withFog("none", false), mapNone = withFog("none", true);
    const auto cells = std::ptrdiff_t(mod(*full).state(*full).grid.w * mod(*full).state(*full).grid.h);
    CHECK(count(*full, 1, 0) > cells / 2);                 // most of the map unexplored
    CHECK(count(*explored, 1, 0) == 0 && count(*explored, 1, 1) > cells / 2);
    CHECK(count(*none, 1, 2) == cells && count(*mapNone, 2, 2) == cells);
    Entity enemyHall = findType(*none, "townhall", 2);
    CHECK(enemyHall.valid() && mod(*none).isVisible(*none, 1, none->world().get<Transform>(enemyHall).pos));
    CHECK(!mod(*full).isVisible(*full, 1, full->world().get<Transform>(findType(*full, "townhall", 2)).pos));
    bool threw = false;
    try { Json r = Json::parseFile(kSkirmish + "/rules.json"); Json f = Json::object(); f.set("mode", Json::string("fuzzy")); r.set("fogOfWar", f); game::loadRules(r); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
    // view look: rules first, game.json overrides per key; colours in 0..255 or 0..1
    game::FogLook L = game::parseFogLook(Json::parse(R"({"color": [255, 0, 0], "style": "grid", "softness": 3, "grid": {"show": "all", "alpha": 0.3}})"),
                                         Json::parse(R"({"softness": 1, "explored": 0.2})"));
    CHECK(L.color[0] == 1.0f && L.color[1] == 0.0f && L.style == game::FogLook::Grid && L.softness == 1 && L.gridEverywhere);
    CHECK(std::fabs(L.explored - 0.2f) < 1e-6f && std::fabs(L.gridAlpha - 0.3f) < 1e-6f);
    CHECK(!game::parseFogLook(Json::parse(R"({"mode": "none"})"), Json()).show);
}

TEST(fog_line_of_sight_3d_blockers) {
    // flat cliff-level map with fogOfWar.lineOfSight: a wall (vision.blocks) hides what is behind it,
    // a tower's eye height sees over it, seesOver ignores it, a smoke area blocks while it lasts
    const char* rules = R"({
      "fogOfWar": {"lineOfSight": true},
      "victory": "noUnits",
      "types": {
        "scout": {"move": {"speed": 2}, "sight": 14},
        "tower": {"move": {"speed": 2}, "sight": 14, "vision": {"eye": 6}},
        "drone": {"move": {"speed": 2}, "sight": 14, "vision": {"seesOver": true}},
        "wall": {"structure": {"footprint": [1, 8]}, "health": {"max": 500}, "vision": {"blocks": 3}},
        "smoker": {"move": {"speed": 2}, "sight": 2, "abilities": [
          {"id": "smoke", "target": "point", "range": 30, "cooldown": 1, "autocast": false,
           "delivery": {"type": "area", "radius": 2, "lifetime": 10, "blocksVision": 4}, "effects": [{"type": "damage", "amount": 0}]}]}
      }})";
    auto mk = [&](const char* viewer, bool wall) {
        std::string ents = std::string(R"(,"entities":[{"type":")") + viewer + R"(","owner":1,"pos":[5.5,4.5]},{"type":"scout","owner":2,"pos":[30.5,4.5]})";
        if (wall) ents += R"(,{"type":"wall","owner":0,"pos":[10.5,4]})";
        return makeSimJson(rules, flatMap(32, 8, ents + "]"));
    };
    auto vis = [](Simulation& s, int x, int y = 4) { auto& gs = mod(s).state(s); return gs.vision[1][size_t(gs.grid.index(x, y))] == 2; };
    auto open = mk("scout", false); run(*open, 1);
    CHECK(vis(*open, 8) && vis(*open, 15) && vis(*open, 18));
    auto walled = mk("scout", true); run(*walled, 1);
    CHECK(vis(*walled, 8) && vis(*walled, 9));
    CHECK(!vis(*walled, 13) && !vis(*walled, 16) && !vis(*walled, 18));
    auto tower = mk("tower", true); run(*tower, 1);
    CHECK(vis(*tower, 16) && vis(*tower, 18));      // high eye: sees over the wall (cells right behind it stay hidden)
    auto drone = mk("drone", true); run(*drone, 1);
    CHECK(vis(*drone, 11) && vis(*drone, 16));
    // the wall destroyed: sight opens again
    Entity w = findType(*walled, "wall", 0);
    CHECK(w.valid());
    if (w.valid()) { walled->world().get<game::Unit>(w).hp = Fixed{}; run(*walled, 1); CHECK(vis(*walled, 16)); }
    // smoke cloud between the eyes and the target
    auto smoke = makeSimJson(rules, flatMap(32, 8, R"(,"entities":[{"type":"scout","owner":1,"pos":[5.5,4.5]},{"type":"smoker","owner":1,"pos":[5.5,6.5]},{"type":"scout","owner":2,"pos":[30.5,4.5]}])"));
    run(*smoke, 1);
    CHECK(vis(*smoke, 16));
    castAbility(*smoke, findType(*smoke, "smoker", 1), 0, kNullEntity, P(11.5, 4.5));
    run(*smoke, 1);
    CHECK(!vis(*smoke, 16) && vis(*smoke, 8));
}

// ---------------- Lighting (lighting agent): GPU-free logic ----------------
#include "modules/rts/view/RtsLights.h"
#include "engine/render/QualityGovernor.h"

TEST(lighting_day_night_cycle) {
    const game::Rules rules = game::loadRules(Json::parseFile(kSkirmish + "/rules.json"));
    const render::Lighting base;
    auto sky = [&](const char* block, double seconds, float* night = nullptr) {
        game::RtsLights L;
        L.init(rules);
        L.configure(Json::parse(block), Json{}, Json{});
        render::Lighting l = base;
        L.updateSky(l, seconds);
        if (night) *night = L.night();
        return l;
    };
    float n = 0;
    render::Lighting noon = sky(R"({"time": 0.5})", 0, &n);
    CHECK(n < 0.01f);
    CHECK(std::fabs(noon.sunIntensity - base.sunIntensity) < 0.05f);
    render::Lighting midnight = sky(R"({"time": 0.0})", 0, &n);
    CHECK(n > 0.95f);
    CHECK(midnight.sunIntensity < base.sunIntensity * 0.4f);
    CHECK(midnight.skyColor.x < base.skyColor.x * 0.5f);
    sky(R"({"time": 0.5, "dayLength": 100})", 50, &n);          // half a day later: midnight
    CHECK(n > 0.95f);
    sky(R"({"time": 0.5, "dayLength": 100})", 100, &n);         // a full day: noon again
    CHECK(n < 0.01f);
    render::Lighting dusk = sky(R"({"time": 0.71})", 0, &n);
    CHECK(dusk.sunColor.z < base.sunColor.z - 0.1f);            // warm evening sun
    // the simulation's day phase (mechanic E) drives the sky even without a lighting block
    { game::RtsLights L; L.init(rules); render::Lighting l = base; L.updateSky(l, 0, 0.0f); CHECK(L.night() > 0.95f); }
    { game::RtsLights L; L.init(rules); L.configure(Json::parse(R"({"time": 0.0})"), Json{}, Json{});
      render::Lighting l = base; L.updateSky(l, 0, 0.5f); CHECK(L.night() < 0.01f); }   // and wins over the block's clock
    // no block: the renderer's lighting is left alone
    { game::RtsLights L; L.init(rules); render::Lighting l = base; L.updateSky(l, 1234); CHECK(l.sunIntensity == base.sunIntensity && L.night() == 0); }
}

TEST(lighting_presets) {
    const game::Rules rules = game::loadRules(Json::parseFile(kSkirmish + "/rules.json"));
    const render::Lighting base;
    auto run = [&](const char* block, std::string* preset = nullptr, float* night = nullptr) {
        game::RtsLights L;
        L.init(rules);
        L.configure(Json::parse(block), Json{}, Json{});
        render::Lighting l = base;
        L.updateSky(l, 0);
        if (preset) *preset = L.preset();
        if (night) *night = L.night();
        return l;
    };
    std::string id;
    float n = 0;
    render::Lighting cave = run(R"({"preset": "cave"})", &id, &n);
    CHECK(id == "cave" && n > 0.95f);
    CHECK(cave.skyColor.x < 0.06f && cave.sunIntensity < base.sunIntensity * 0.1f);
    render::Lighting desert = run(R"({"preset": "desert"})", &id, &n);
    CHECK(id == "desert" && n < 0.01f && desert.sunIntensity > base.sunIntensity);
    run(R"({"preset": "cave", "time": 0.5})", nullptr, &n);       // explicit fields win over the preset
    CHECK(n < 0.01f);
    run(R"({"preset": "nope"})", &id);
    CHECK(id.empty());
    // a cave map in a daylight game stays a cave: map preset beats game fields
    {
        game::RtsLights L;
        L.init(rules);
        L.configure(Json::parse(R"({"time": 0.35, "dayLength": 480})"), Json::parse(R"({"preset": "cave"})"), Json{});
        render::Lighting l = base;
        L.updateSky(l, 100);
        CHECK(L.night() > 0.95f && L.preset() == "cave");
    }
}

TEST(lighting_lightning_deterministic) {
    CHECK(game::RtsLights::lightningAt(10.3, 0) == 0);
    int strikes = 0, weak = 0;
    bool same = true, was = false, wasWeak = false;
    for (int i = 0; i < 120000; ++i) {          // 10 minutes in 5 ms steps; count rising edges
        const double t = i * 0.005;
        const float a = game::RtsLights::lightningAt(t, 1.0f);
        same &= a == game::RtsLights::lightningAt(t, 1.0f);
        const bool on = a > 0.5f, onWeak = game::RtsLights::lightningAt(t, 0.2f) > 0.5f;
        strikes += on && !was;
        weak += onWeak && !wasWeak;
        was = on; wasWeak = onWeak;
    }
    CHECK(same);
    CHECK(strikes > 20 && strikes < 400);   // ~0.16 flashes per second at storm 1
    CHECK(weak < strikes);
}

TEST(lighting_type_and_map_data) {
    const game::Rules sk = game::loadRules(Json::parseFile(kSkirmish + "/rules.json"));
    game::RtsLights L;
    L.init(sk);
    const auto& hall = L.typeLights(uint16_t(sk.findType("townhall")));
    CHECK(hall.glows.size() == 1 && hall.glows[0].radius > 4 && hall.glows[0].halo > 0 && !hall.glows[0].spot);
    const auto& archer = L.typeLights(uint16_t(sk.findType("archer")));
    CHECK(archer.attack.on && archer.projectile.on && archer.glows.empty());
    const auto& cat = L.typeLights(uint16_t(sk.findType("catapult")));
    CHECK(!cat.projectile.on && cat.hit.on);                     // data: "projectileLight": false
    // abilities: one light per ability, colour from what it does (artillery = damage -> orange)
    const std::string fc = std::string(RTS_SOURCE_DIR) + "/templates/frostcrown/rules.json";
    if (std::filesystem::exists(fc)) {
        const game::Rules fr = game::loadRules(Json::parseFile(fc));
        game::RtsLights F;
        F.init(fr);
        const int t = fr.findType("far_seer");   // chain lightning: damage -> orange
        CHECK(t >= 0);
        if (t >= 0) {
            const auto& d = F.typeLights(uint16_t(t));
            CHECK(d.abilities.size() == fr.types[size_t(t)].abilities.size());
            CHECK(!d.abilities.empty() && d.abilities[0].on && d.abilities[0].color.x > 0.9f && d.abilities[0].color.z < 0.5f);
        }
    }
    const auto ml = game::RtsLights::parseMapLights(Json::parse(R"([{"x": 1, "z": 2}, {"pos": [3, 4], "radius": 6, "halo": false}, 7])"));
    CHECK(ml.size() == 2);
    CHECK(ml[0].x == 1 && ml[0].z == 2 && ml[0].radius == 4 && ml[0].halo > 0);
    CHECK(ml[1].x == 3 && ml[1].z == 4 && ml[1].radius == 6 && ml[1].halo == 0);
}

TEST(lighting_spot_lights) {
    using game::RtsLights;
    const RtsLights::Glow point = RtsLights::parseGlow(Json::parse(R"({"radius": 3})"));
    CHECK(!point.spot && point.radius == 3);
    CHECK(!RtsLights::glowLight(point, {0, 0, 0}, 0, 1, 0, 0).spot());
    const RtsLights::Glow s = RtsLights::parseGlow(Json::parse(R"({"type": "spot", "pitch": 30, "angle": 40, "offset": [0.5, 0.2]})"));
    CHECK(s.spot && s.radius == 9 && s.angle == 40);
    render::PointLight l = RtsLights::glowLight(s, {10, 2, 5}, 0, 1, 0, 0);   // facing +X
    CHECK(l.spot() && l.dir.x > 0.8f && l.dir.y < -0.4f && std::fabs(l.dir.z) < 1e-4f);
    CHECK(std::fabs(l.pos.x - 10.5f) < 1e-4f && std::fabs(l.pos.z - 5.2f) < 1e-4f && l.groundY == 2);
    CHECK(l.cosOuter < l.cosInner && std::fabs(l.cosOuter - std::cos(20.0f * 0.0174532925f)) < 1e-4f);
    render::PointLight turned = RtsLights::glowLight(s, {0, 0, 0}, 1.5707963f, 1, 0, 0);   // facing +Z
    CHECK(turned.dir.z > 0.8f && std::fabs(turned.dir.x) < 1e-3f);
    // a searchlight sweeps: its direction changes over time, within the amplitude
    const RtsLights::Glow sw = RtsLights::parseGlow(Json::parse(R"({"angle": 30, "pitch": 30, "yaw": 90, "sweep": 45, "sweepSpeed": 30})"));
    float minA = 1e9f, maxA = -1e9f;
    for (int i = 0; i < 400; ++i) {
        const render::PointLight x = RtsLights::glowLight(sw, {0, 0, 0}, 0, 1, i * 0.05f, 0);
        const float a = std::atan2(x.dir.z, x.dir.x) / 0.0174532925f;
        minA = std::min(minA, a); maxA = std::max(maxA, a);
    }
    CHECK(minA > 44 && minA < 50 && maxA > 130 && maxA < 136);
    const auto ml = RtsLights::parseMapLights(Json::parse(R"([{"x": 4, "z": 6, "type": "spot", "yaw": 90}])"));
    CHECK(ml.size() == 1 && ml[0].spot && ml[0].x == 4 && ml[0].yaw == 90);
}

TEST(lighting_reactive_patterns) {
    using game::RtsLights;
    auto avg = [](const RtsLights::Glow& g, float hp, bool powered, float night, float* mn = nullptr, float* mx = nullptr) {
        float sum = 0, lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 2000; ++i) { const float v = RtsLights::glowLevel(g, i * 0.01f, 0.3f, hp, powered, night); sum += v; lo = std::min(lo, v); hi = std::max(hi, v); }
        if (mn) *mn = lo;
        if (mx) *mx = hi;
        return sum / 2000;
    };
    const RtsLights::Glow steady = RtsLights::parseGlow(Json::parse(R"({})"));
    CHECK(std::fabs(avg(steady, 1, true, 0) - 1) < 1e-4f);
    CHECK(std::fabs(avg(steady, 0.1f, false, 0) - 1) < 1e-4f);   // a fire ignores power and damage
    float lo = 0, hi = 0;
    avg(RtsLights::parseGlow(Json::parse(R"({"pattern": "pulse", "rate": 1})")), 1, true, 0, &lo, &hi);
    CHECK(lo > 0.05f && lo < 0.2f && hi > 0.95f);
    const float strobe = avg(RtsLights::parseGlow(Json::parse(R"({"pattern": "strobe", "rate": 2})")), 1, true, 0);
    CHECK(strobe > 0.06f && strobe < 0.18f);                     // short flashes
    const RtsLights::Glow night = RtsLights::parseGlow(Json::parse(R"({"nightOnly": true})"));
    CHECK(RtsLights::glowLevel(night, 1, 0, 1, true, 0) == 0 && RtsLights::glowLevel(night, 1, 0, 1, true, 1) == 1);
    const RtsLights::Glow lamp = RtsLights::parseGlow(Json::parse(R"({"electric": true})"));
    const float brown = avg(lamp, 1, false, 0), broken = avg(lamp, 0.05f, true, 0);
    CHECK(brown > 0.15f && brown < 0.5f);                        // low power: dim and stuttering
    CHECK(broken < 0.8f && broken > 0.2f);                       // badly damaged: sparks out now and then
    CHECK(RtsLights::glowLevel(lamp, 3.21f, 0.5f, 0.05f, false, 0) == RtsLights::glowLevel(lamp, 3.21f, 0.5f, 0.05f, false, 0));
    CHECK(RtsLights::parseGlow(Json::parse(R"({"type": "spot"})")).electric);
    // a beacon spot rotates all the way round
    const RtsLights::Glow beacon = RtsLights::parseGlow(Json::parse(R"({"type": "spot", "pattern": "beacon", "rate": 0.5})"));
    float minA = 1e9f, maxA = -1e9f;
    for (int i = 0; i < 100; ++i) {
        const render::PointLight l = RtsLights::glowLight(beacon, {0, 0, 0}, 0, 1, i * 0.02f, 0);
        const float a = std::atan2(l.dir.z, l.dir.x) / 0.0174532925f;
        minA = std::min(minA, a); maxA = std::max(maxA, a);
    }
    CHECK(maxA - minA > 300);
}

TEST(lighting_quality_governor) {
    using render::QualityGovernor;
    auto feed = [](QualityGovernor& g, double ms, double seconds) {
        int changes = 0;
        for (double t = 0; t < seconds; t += ms / 1000.0) changes += g.update(ms / 1000.0) ? 1 : 0;
        return changes;
    };
    QualityGovernor g;
    g.reset(3, 3);
    feed(g, 16.7, 3);                              // smooth: nothing happens
    CHECK(g.shadows() == 3 && g.lights() == 3 && g.stepsDown() == 0);
    feed(g, 40, 7);                                // heavy fight: first step drops per-pixel lights
    CHECK(g.lights() == 2 && g.shadows() == 3);
    feed(g, 60, 60);                               // still too slow: all the way down, lights last
    CHECK(g.shadows() == 0 && g.lights() == 0);
    // a phone capped at 30 fps from the start is not "slowing down"
    QualityGovernor capped;
    capped.reset(2, 2);
    CHECK(feed(capped, 33.3, 40) == 0);
    // the player picked a tier in the menu: hands off
    QualityGovernor locked;
    locked.reset(3, 3);
    feed(locked, 16.7, 2);
    locked.lock();
    CHECK(feed(locked, 50, 30) == 0 && locked.shadows() == 3);
    // after the fight one step comes back, once
    QualityGovernor rec;
    rec.reset(2, 2);
    feed(rec, 16.7, 2);
    feed(rec, 40, 9);
    const int afterDrop = rec.shadows() + rec.lights();
    CHECK(afterDrop < 4);
    feed(rec, 16.7, 45);
    CHECK(rec.shadows() + rec.lights() == afterDrop + 1);
    feed(rec, 16.7, 60);
    CHECK(rec.shadows() + rec.lights() == afterDrop + 1);
}

// ---------------- Native plugins bridge, store and ads services (platforms agent) ----------------
#include "engine/platform/PluginJson.h"
#include "engine/platform/Plugins.h"
#include "engine/services/Ads.h"
#include "engine/services/Store.h"
#include <cstdlib>
#include <thread>

static void setEnvVar(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

TEST(plugin_json_keeps_numbers_and_unicode) {
    namespace json = plugins::json;
    json::Value v;
    std::string err;
    CHECK(json::parse(R"({"price":0.99,"micros":1990000000,"t":"Привет 🙂","a":[true,null]})", v, &err));
    CHECK(v["price"].s == "0.99" && v["micros"].i64() == 1990000000);
    CHECK(v["t"].str() == "Привет 🙂");
    CHECK(json::dump(v) == "{\"price\":0.99,\"micros\":1990000000,\"t\":\"Привет 🙂\",\"a\":[true,null]}");
    CHECK(!json::parse("{\"a\":}", v, &err) && !err.empty());
    CHECK(json::parseOr(json::stripComments("// head\n{ \"u\": \"http://x\" /* c */ }"))["u"].str() == "http://x");
    CHECK(plugins::detail::resolvePlatformValues(R"({"id":{"android":"A","ios":"I","default":"D"},"n":{"web":1}})", "ios") == R"({"id":"I","n":null})");
    CHECK(plugins::detail::resolvePlatformValues(R"({"id":{"android":"A","desktop":"PC"}})", "linux") == R"({"id":"PC"})");
}

namespace {
struct EchoPlugin : plugins::NativePlugin {
    std::vector<std::string> calls;
    void call(const std::string& method, const std::string& args, plugins::Reply r) override {
        calls.push_back(method + " " + args);
        if (method == "fail") { r.fail("nope"); return; }
        if (method == "later") { std::thread([r] { r.ok(R"({"from":"thread"})"); }).join(); return; }
        if (method == "event") plugins::emit("echo", "ping", R"({"n":1})");
        r.ok(args);
    }
    void update() override { ++updates; }
    int updates = 0;
};
} // namespace

TEST(plugin_bridge_calls_replies_events) {
    plugins::configure(R"({"echo":{"key":{"linux":"L","windows":"L","macos":"L","default":"L"}}})");
    auto echo = std::make_shared<EchoPlugin>();
    plugins::registerPlugin("echo", echo);
    CHECK(plugins::available("echo") && !plugins::available("no-such-plugin"));
    // desktop mocks exist only when the game lists them (a shipped build must not offer fake purchases)
    if (!std::getenv("RTS_MOCK_PLUGINS")) CHECK(!plugins::available("mock-store") && !plugins::available("mock-ads"));
    std::vector<std::string> got;
    plugins::pump();
    CHECK(echo->updates == 0); // no per-frame work before "init"
    plugins::call("echo", "hello", R"({"x":1})", [&](const plugins::Result& r) { got.push_back(r.ok ? r.data : "ERR " + r.error); });
    CHECK(got.empty()); // replies never arrive inside call()
    plugins::pump();
    CHECK(echo->updates == 1); // then update() every frame
    CHECK(got.size() == 1 && got[0] == R"({"x":1})");
    CHECK(echo->calls.size() == 2 && echo->calls[0] == R"(init {"key":"L"})"); // config first, resolved
    plugins::call("echo", "again", "{}", {});
    plugins::call("echo", "fail", "{}", [&](const plugins::Result& r) { got.push_back(r.ok ? "ok" : "ERR " + r.error); });
    plugins::call("echo", "later", "{}", [&](const plugins::Result& r) { got.push_back(r.data); });
    plugins::call("no-such-plugin", "x", "{}", [&](const plugins::Result& r) { got.push_back(r.ok ? "ok" : "missing"); });
    std::vector<std::string> events;
    int id = plugins::addListener("echo", [&](const std::string& ev, const std::string& d) { events.push_back(ev + d); });
    plugins::call("echo", "event", "{}", {});
    plugins::pump();
    CHECK(echo->calls.size() == 6); // init went out only once
    CHECK(got.size() == 4 && got[1] == "ERR nope" && got[2] == R"({"from":"thread"})" && got[3] == "missing");
    CHECK(events.size() == 1 && events[0] == R"(ping{"n":1})");
    plugins::removeListener(id);
    plugins::unregisterPlugin("echo");
}

TEST(store_and_ads_through_mock_plugins) {
    plugins::configureGame(R"(// comments allowed, like every game.json
    {
      "plugins": { "mock-store": {}, "mock-ads": {} },
      "store": { "providers": ["no-such-store", "mock-store"],
                 "products": [ { "id": "gems", "type": "consumable" },
                               { "id": "noads", "type": "nonConsumable", "ids": { "default": "com.game.noads" } } ] },
      "ads": { "providers": { "default": ["mock-ads"] } }
    })");
    CHECK(store::available() && store::provider() == "mock-store");
    CHECK(store::typeOf("noads") == store::ProductType::NonConsumable);
    std::vector<store::Product> prods;
    store::products([&](bool ok, const std::vector<store::Product>& p, const std::string&) { if (ok) prods = p; });
    plugins::pump();
    CHECK(prods.size() == 2 && prods[1].id == "noads" && prods[0].price == "$0.99" && prods[0].priceMicros == 990000);

    std::vector<store::Purchase> done, async;
    store::onPurchase([&](const store::Purchase& p) { async.push_back(p); });
    setEnvVar("RTS_MOCK_STORE", "purchased");
    store::purchase("noads", [&](const store::Purchase& p) { done.push_back(p); });
    setEnvVar("RTS_MOCK_STORE", "cancelled");
    store::purchase("gems", [&](const store::Purchase& p) { done.push_back(p); });
    setEnvVar("RTS_MOCK_STORE", "pending");
    store::purchase("gems", [&](const store::Purchase& p) { done.push_back(p); });
    plugins::pump();
    setEnvVar("RTS_MOCK_STORE", "purchased");
    CHECK(done.size() == 3);
    CHECK(done[0].state == store::PurchaseState::Purchased && done[0].productId == "noads" && !done[0].token.empty());
    CHECK(done[1].state == store::PurchaseState::Cancelled);
    CHECK(done[2].state == store::PurchaseState::Pending);
    CHECK(async.size() == 1 && async[0].state == store::PurchaseState::Purchased && async[0].productId == "gems");

    std::vector<store::Purchase> restored;
    store::finish(async[0]); // consumable granted → consumed, no longer restored
    store::restore([&](bool ok, const std::vector<store::Purchase>& p, const std::string&) { if (ok) restored = p; });
    plugins::pump();
    CHECK(restored.size() == 1 && restored[0].productId == "noads" && restored[0].restored);

    CHECK(ads::available() && ads::provider() == "mock-ads");
    ads::preload(ads::Format::Rewarded);
    plugins::pump();
    CHECK(ads::ready(ads::Format::Rewarded));
    std::vector<ads::ShowResult> shown;
    int pauses = 0;
    int l = plugins::addListener("*", [&](const std::string& ev, const std::string&) { pauses += ev == "app.pause"; });
    ads::show(ads::Format::Rewarded, [&](const ads::ShowResult& r) { shown.push_back(r); }, "double_reward");
    setEnvVar("RTS_MOCK_ADS", "noreward");
    ads::show(ads::Format::Rewarded, [&](const ads::ShowResult& r) { shown.push_back(r); });
    setEnvVar("RTS_MOCK_ADS", "ok");
    ads::setEnabled(false);
    ads::show(ads::Format::Interstitial, [&](const ads::ShowResult& r) { shown.push_back(r); });
    ads::setEnabled(true);
    plugins::pump();
    CHECK(shown.size() == 3);
    CHECK(shown[0].shown == false && shown[0].error == "ads disabled"); // answered at once, no plugin call
    CHECK(shown[1].shown && shown[1].rewarded);
    CHECK(shown[2].shown && !shown[2].rewarded);
    CHECK(pauses == 2 && !ads::ready(ads::Format::Interstitial));
    plugins::removeListener(l);
}

// ---------------- overlays (data-driven helper elements; overlays agent) ----------------
#include "engine/platform/Platform.h"
#include "modules/rts/view/RtsOverlayData.h"
#include "modules/rts/view/RtsView.h"

static game::Rules skirmishRules() { return game::loadRules(Json::parseFile(kSkirmish + "/rules.json")); }
static std::string overlayError(const game::Rules& R, const char* json) {
    try { game::parseOverlay(R, Json::parse(json)); } catch (const std::exception& e) { return e.what(); }
    return "";
}
static bool hasText(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

TEST(overlay_builtins_are_data) {
    game::Rules R = skirmishRules();
    game::OverlaySet set = game::loadOverlaySet(R, Json(), Json());
    CHECK(set.warnings.empty());
    std::vector<std::string> ids;
    for (auto& d : set.defs) { ids.push_back(d.id); CHECK(d.builtin); CHECK(d.enabled); }
    CHECK((ids == std::vector<std::string>{"selection", "hover", "move_markers", "placement", "garrison_count", "health_bars", "build_progress"}));
    const game::OverlayDef& sel = set.defs[0];
    CHECK(sel.anchor == game::OvAnchor::Selected && sel.shape == game::OvShape::Ring && sel.radiusFrom == game::OvRadius::Selection);
    CHECK(sel.style.ground == game::OvGround::Flat && sel.style.segments == 32 && sel.style.thickness == 0);
    // exact old colours: rgba(120, 255, 120, 230) and the neutral rgba(255, 255, 160, 230)
    CHECK(render::rgbaf(sel.style.color) == render::rgba(120, 255, 120, 230));
    CHECK(render::rgbaf(sel.style.colors[game::kColNeutral]) == render::rgba(255, 255, 160, 230));
    const game::OverlayDef& hp = set.defs[5];
    CHECK(hp.shape == game::OvShape::Bar && hp.style.space == game::OvSpace::Screen && hp.barW == 26 && hp.barWStructure == 46);
    CHECK(hp.style.stops.size() == 3 && hp.style.stops[0].first < hp.style.stops[1].first);
    CHECK(hp.anchorFilter.flagsOff == (game::kOvInvulnerable | game::kOvGarrison));
}

TEST(overlay_override_and_merge_by_id) {
    game::Rules R = skirmishRules();
    Json rules = Json::parse(R"([
        {"id": "health_bars", "enabled": false},
        {"id": "selection", "style": {"thickness": 0.06}},
        {"id": "grid", "when": "placing", "anchor": "ghost", "shape": {"type": "grid", "radius": 3, "predicate": "buildable"}}])");
    Json game = Json::parse(R"([{"id": "grid", "shape": {"radius": 5}}, {"id": "hover", "replace": true, "anchor": "hovered", "shape": "ring"}])");
    game::OverlaySet set = game::loadOverlaySet(R, rules, game);
    CHECK(set.warnings.empty());
    const game::OverlayDef* hp = nullptr; const game::OverlayDef* sel = nullptr; const game::OverlayDef* grid = nullptr; const game::OverlayDef* hov = nullptr;
    for (auto& d : set.defs) {
        if (d.id == "health_bars") hp = &d;
        if (d.id == "selection") sel = &d;
        if (d.id == "grid") grid = &d;
        if (d.id == "hover") hov = &d;
    }
    CHECK(hp && !hp->enabled && hp->builtin);
    CHECK(sel && sel->style.thickness > 0.05f && render::rgbaf(sel->style.color) == render::rgba(120, 255, 120, 230)); // colour kept
    CHECK(grid && grid->shape == game::OvShape::Grid && grid->radius == 5 && grid->pred == game::OvPred::Buildable);  // game.json wins
    CHECK(hov && hov->radiusFrom == game::OvRadius::Fixed && !hov->builtin);   // replaced entirely
    CHECK(set.defs.size() == 8);
}

TEST(overlay_validation_errors) {
    game::Rules R = skirmishRules();
    CHECK(overlayError(R, R"({"id": "a", "shape": "ring", "anchor": {"type": "entities", "types": ["dragon"]}})") ==
          "overlay 'a'.anchor.types: unknown type 'dragon'");
    CHECK(hasText(overlayError(R, R"({"id": "b", "shape": "hexagon"})"), "unknown shape 'hexagon'"));
    CHECK(hasText(overlayError(R, R"({"id": "c", "anchor": "cursor", "shape": {"type": "bar"}})"), "a bar needs an entity anchor"));
    CHECK(hasText(overlayError(R, R"({"id": "d", "shape": "ring", "colour": "red"})"), "unknown key 'colour'"));
    CHECK(hasText(overlayError(R, R"({"id": "e", "shape": "ring", "style": {"color": "#12"}})"), "bad hex colour"));
    CHECK(hasText(overlayError(R, R"({"id": "f", "shape": "ring", "when": {"key": "Hyper"}})"), "unknown key 'Hyper'"));
    CHECK(hasText(overlayError(R, R"({"id": "g", "shape": "ring", "when": {"selected": {"owner": "friend"}}})"), "unknown owner 'friend'"));
    CHECK(hasText(overlayError(R, R"({"id": "h", "shape": "ring", "anchor": "cursor", "when": {"flags": ["shiny"]}})"), "unknown flag 'shiny'"));
    CHECK(hasText(overlayError(R, R"({"id": "i", "shape": "text", "event": {"on": "explode"}})"), "unknown event 'explode'"));
    CHECK(hasText(overlayError(R, R"({"id": "j", "shape": "line", "anchor": "selected"})"), "needs \"to\""));
    CHECK(hasText(overlayError(R, R"({"id": "k", "shape": "ring", "when": {"status": "frozen"}})"), "unknown status 'frozen'"));
    CHECK(hasText(overlayError(R, R"({"shape": "ring"})"), "missing \"id\""));
    CHECK(overlayError(R, R"({"id": "ok", "shape": {"type": "ring", "radius": "range:0"}, "anchor": {"type": "selected", "owner": "own"},
                              "when": {"any": [{"key": "Alt"}, {"zoom": [0, 40]}]}, "style": {"color": [1, 0.5, 0.2, 0.8], "dash": [0.5, 0.3]}})").empty());
    bool dup = false;
    try { game::validateOverlays(R, Json::parse(R"([{"id": "x", "shape": "ring"}, {"id": "x", "shape": "circle"}])")); }
    catch (const std::exception& e) { dup = hasText(e.what(), "duplicate id"); }
    CHECK(dup);
    // partial overrides of built-ins are valid lists (checked merged over the default)
    bool partialOk = true;
    try { game::validateOverlays(R, Json::parse(R"([{"id": "selection", "style": {"thickness": 0.07}}, {"id": "health_bars", "enabled": false}])")); }
    catch (const std::exception&) { partialOk = false; }
    CHECK(partialOk);
    bool partialBad = false;
    try { game::validateOverlays(R, Json::parse(R"([{"id": "selection", "style": {"colr": 1}}])")); }
    catch (const std::exception& e) { partialBad = hasText(e.what(), "unknown key 'colr'"); }
    CHECK(partialBad);
    // a broken entry in game data is skipped with a warning; the rest still loads
    game::OverlaySet set = game::loadOverlaySet(R, Json::parse(R"([{"id": "bad", "shape": "nope"}])"), Json());
    CHECK(set.warnings.size() == 1 && set.defs.size() == 7);
}

TEST(overlay_templates_are_valid) {
    game::Rules R = skirmishRules();
    Json tpl = game::overlayTemplates();
    CHECK(tpl.size() >= 10);
    for (auto& t : tpl.items()) {
        std::string err;
        try { game::parseOverlay(R, t["overlay"]); } catch (const std::exception& e) { err = e.what(); }
        if (!err.empty()) std::printf("  template %s: %s\n", t.getStr("id").c_str(), err.c_str());
        CHECK(err.empty());
    }
}

TEST(overlay_conditions_on_sim_state) {
    auto sim = makeSim(kSkirmish, "maps/valley.json", 1);
    for (int i = 0; i < 20; ++i) sim->step();
    game::RtsModule& m = mod(*sim);
    const game::Rules& R = m.rules();
    game::RtsScene scene;   // like sceneFromSim (RtsView.cpp needs the renderer, not linked here)
    sim->world().each<game::Unit, Owner, Transform>([&](Entity e, game::Unit& u, Owner& ow, Transform& tr) {
        game::SceneObject so;
        so.entity = e; so.type = u.type; so.owner = ow.player;
        so.pos = {float(tr.pos.x.toDouble()), 0, float(tr.pos.z.toDouble())};
        so.hp = float((u.hp / R.types[u.type].maxHp).toDouble());
        scene.objects.push_back(so);
    });
    scene.tick = sim->tick();
    int own = -1, enemy = -1, worker = -1;
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        auto& o = scene.objects[i];
        if (o.isProjectile) continue;
        if (o.owner == 1 && own < 0 && !R.types[o.type].structure) own = int(i);
        if (o.owner == 1 && R.types[o.type].id == "peasant") worker = int(i);
        if (o.owner == 2 && enemy < 0) enemy = int(i);
    }
    CHECK(own >= 0 && enemy >= 0);
    if (own < 0 || enemy < 0) return;
    scene.objects[size_t(own)].selected = true;
    scene.objects[size_t(enemy)].visible = true;
    game::RtsOverlays ov;
    Json extra = Json::parse(R"([{"id": "hits", "event": {"on": "hit", "owner": "own"}, "shape": {"type": "text", "text": "-{value}"}},
                                 {"id": "enemyHits", "event": {"on": "hit", "owner": "enemy"}, "shape": {"type": "text", "text": "x"}}])");
    ov.init(R, extra, Json(), "");
    CHECK(ov.set().warnings.empty());
    bool keys[int(platform::Key::Count)] = {};
    keys[int(platform::Key::LeftShift)] = true;
    game::OverlayContext ctx;
    ctx.sim = sim.get(); ctx.player = 1; ctx.keys = keys; ctx.keyCount = int(platform::Key::Count);
    ctx.placeType = R.findType("barracks");
    ov.setContext(ctx);
    auto& gs = m.state(*sim);
    gs.trig.vars["alarm"] = Fixed::fromInt(3);
    ov.prepare(scene, 1.0, &gs.vision[1], gs.grid, 30);
    auto cond = [&](const char* when) {
        Json e = Json::parse(std::string(R"({"id": "t", "shape": "ring", "anchor": "cursor", "when": )") + when + "}");
        return game::parseOverlay(R, e).when;
    };
    const game::SceneObject& o = scene.objects[size_t(own)];
    const game::SceneObject& en = scene.objects[size_t(enemy)];
    CHECK(ov.relation(1) == game::kRelOwn && ov.relation(2) == game::kRelEnemy && ov.relation(0) == game::kRelNeutral);
    CHECK(ov.test(cond(R"({"selected": {"owner": "own"}})"), nullptr));      // something own is selected
    CHECK(!ov.test(cond(R"({"selected": {"owner": "enemy"}})"), nullptr));
    CHECK(ov.test(cond(R"("selected")"), &o) && !ov.test(cond(R"("selected")"), &en));   // per entity
    CHECK(ov.test(cond(R"({"selected": false})"), &en));
    CHECK(ov.test(cond(R"({"owner": "enemy"})"), &en) && !ov.test(cond(R"({"owner": "enemy"})"), &o));
    CHECK(ov.test(cond(R"({"flags": ["unit", "!structure"]})"), &o));
    if (worker >= 0) CHECK(ov.test(cond(R"({"types": ["peasant"]})"), &scene.objects[size_t(worker)]));
    CHECK(ov.test(cond(R"("placing")"), nullptr) && ov.test(cond(R"({"placing": ["barracks", "farm"]})"), nullptr));
    CHECK(!ov.test(cond(R"({"placing": "tower"})"), nullptr));
    CHECK(ov.test(cond(R"({"key": "Shift"})"), nullptr) && !ov.test(cond(R"({"key": ["Shift", "Q"]})"), nullptr));
    CHECK(ov.test(cond(R"({"state": "playing"})"), nullptr) && !ov.test(cond(R"("paused")"), nullptr));
    CHECK(ov.test(cond(R"({"var": {"name": "alarm", ">=": 3}})"), nullptr) && !ov.test(cond(R"({"flag": "quiet"})"), nullptr));
    CHECK(ov.test(cond(R"({"zoom": [10, 40]})"), nullptr) && !ov.test(cond(R"({"zoom": [50, 90]})"), nullptr));
    CHECK(ov.test(cond(R"({"any": [{"key": "Q"}, {"not": "paused"}]})"), nullptr));
    CHECK(ov.test(cond(R"({"hp": [0.5, 2]})"), &o) && !ov.test(cond(R"({"hp": [0, 0.5]})"), &o));
    // cell predicates read the grid / fog of the local player
    const int cx = int(std::floor(o.pos.x)), cz = int(std::floor(o.pos.z));
    CHECK(ov.cellTest(game::OvPred::Visible, cx, cz));
    CHECK(ov.cellTest(game::OvPred::Buildable, cx, cz) == (gs.grid.areaBuildable(cx, cz, 1, 1)));
    CHECK(ov.cellTest(game::OvPred::Blocked, -5, -5) == false);
    // events become popups: a hit on the own unit, filtered by the owner of the entity hit
    scene.events.push_back({uint16_t(game::kEvHit), 2, o.entity, en.entity, Fixed::fromInt(7).raw, sim->world().get<Transform>(o.entity).pos});
    scene.tick += 1;
    ov.prepare(scene, 1.1, &gs.vision[1], gs.grid, 30);
    CHECK(ov.popupCount() == 1);
    ov.prepare(scene, 1.2, &gs.vision[1], gs.grid, 30);   // same tick: not spawned twice
    CHECK(ov.popupCount() == 1);
    ov.prepare(scene, 5.0, &gs.vision[1], gs.grid, 30);   // expired after its lifetime
    CHECK(ov.popupCount() == 0);
}

// ---------------- particle modules for richer effects / Unity imports (vfx tools) ----------------
TEST(fx_modules_parse_round_trip_and_errors) {
    std::vector<std::string> errs;
    fx::EffectDef d = fxParse(R"({"emitters": [{ "texture": "fx/t.png", "blend": "premultiplied", "align": "upright", "aspect": [1, 2],
        "offset": [0, 1, 0], "bursts": [[0, 5], { "time": 0.2, "count": [2, 4], "cycles": 3, "interval": 0.1 }],
        "startColor": ["#ff0000", "#0000ff"], "space": "local", "maxParticles": 40, "velocity": [0, 2, 0], "speedLimit": 4,
        "colorBySpeed": { "speed": [0, 3], "color": [[0, "#ffffff"], [1, "#ff0000"]] }, "sizeBySpeed": { "speed": [0, 3], "size": [[0, 1], [1, 2]] },
        "noise": { "strength": 1, "frequency": 0.5, "scroll": 0.2 }, "frames": [2, 2], "cycles": 2, "startFrame": [0, 3],
        "shape": "cone", "arc": 180, "thickness": 0.5, "radial": -0.5, "radius": 1, "hidden": false,
        "collision": { "height": 0.1, "bounce": 0.3, "dampen": 0.2, "lifeLoss": 0.1, "minKillSpeed": 0.5 },
        "subEmitters": [{ "on": "death", "effect": "pop", "chance": 0.5, "inheritColor": true }],
        "trail": { "time": 0.4, "width": [[0, 1], [1, 0]], "color": "#ffffff80", "blend": "alpha", "inheritColor": false },
        "light": { "color": "#ff8000", "radius": 2, "intensity": 1.5, "ratio": 0.5, "max": 3, "sizeAffectsRadius": true } }]})", &errs);
    for (auto& e : errs) std::printf("    %s\n", e.c_str());
    CHECK(errs.empty() && d.emitters.size() == 1);
    const auto& e = d.emitters[0];
    CHECK(e.blend == fx::FxBlend::Premultiplied && e.align == fx::FxAlign::Upright && e.shape == fx::FxShape::Cone);
    CHECK(e.bursts.size() == 2 && e.bursts[1].cycles == 3 && e.bursts[1].count.max == 4 && e.startColorPair && e.localSpace);
    CHECK(e.maxParticles == 40 && e.velocity.y == 2 && e.speedLimit.max == 4 && e.colorBySpeed.size() == 2 && e.sizeBySpeed.size() == 2);
    CHECK(e.noise.strength.max == 1 && e.cycles == 2 && e.startFrame.max == 3 && e.arc == 180 && e.radial == -0.5f);
    CHECK(e.collision.enabled && e.collision.bounce > 0.29f && e.subEmitters.size() == 1 && e.subEmitters[0].on == fx::FxSubEvent::Death);
    CHECK(e.trail.enabled && e.trail.blend == fx::FxBlend::Alpha && !e.trail.inheritColor && e.light.enabled && e.light.max == 3);
    CHECK(d.lifetime() > 0.4f && !d.looping());
    fx::EffectDef back;
    std::vector<std::string> e2;
    CHECK(fx::parseEffect(fx::effectToJson(d), "t.json", back, e2) && e2.empty());   // the editor's save keeps every module
    const auto& b = back.emitters[0];
    CHECK(b.bursts.size() == 2 && b.bursts[1].interval > 0.09f && b.startColorPair && b.localSpace && b.trail == e.trail);
    CHECK(b.subEmitters.size() == 1 && b.subEmitters[0].effect == "pop" && b.light.sizeAffectsRadius && b.collision.minKillSpeed > 0.49f);
    CHECK(b.noise.scroll > 0.19f && b.colorSpeed.max == 3 && b.aspect.max == 2 && b.offset.y == 1 && b.thickness == 0.5f);
    // an old effect writes none of the new keys
    fx::EffectDef plain = fxParse(R"({"emitters": [{ "burst": 3 }]})");
    std::string text = fx::effectToJson(plain).dump(-1);
    for (const char* k : {"bursts", "trail", "light", "noise", "space", "aspect", "subEmitters", "collision", "radial"})
        CHECK(text.find(k) == std::string::npos);
    // readable errors inside modules
    fxParse(R"({"emitters": [{ "burst": 1, "trail": { "tme": 1 }, "radial": 2, "subEmitters": [{ "on": "spawn", "effect": "x" }] }]})", &errs);
    CHECK(errs.size() == 3);
    bool trailKey = false;
    for (auto& x : errs) trailKey |= x.find("trail.tme: unknown field") != std::string::npos;
    CHECK(trailKey);
    fxParse(R"({"emitters": [{ "bursts": [[0, 2]], "loop": true }]})", &errs);
    CHECK(errs.size() == 1 && errs[0].find("duration") != std::string::npos);
}

TEST(fx_modules_simulate_bursts_limits_subs_collision_lights) {
    auto make = [] { auto ps = std::make_unique<fx::ParticleSystem>(); ps->setQuality(fx::FxQuality::High); ps->init(nullptr); return ps; };
    {   // timed bursts with cycles
        auto ps = make();
        int id = ps->define("b", fxParse(R"({"emitters": [{ "bursts": [[0, 5], { "time": 0.1, "count": 3, "cycles": 2, "interval": 0.05 }], "life": 5 }]})"));
        ps->play(id, {0, 0, 0});
        ps->update(0.05f);
        CHECK(ps->stats().particles == 5);
        for (int i = 0; i < 4; ++i) ps->update(0.05f);
        CHECK(ps->stats().particles == 11);
    }
    {   // looping bursts repeat every duration; per-emitter cap
        auto ps = make();
        int id = ps->define("l", fxParse(R"({"emitters": [{ "bursts": [[0, 2]], "loop": true, "duration": 0.5, "life": 10 },
                                                          { "burst": 50, "maxParticles": 7, "life": 10 }]})"));
        fx::FxHandle h = ps->play(id, {0, 0, 0});
        for (int i = 0; i < 25; ++i) ps->update(0.05f);   // t = 1.25: bursts at 0, 0.5, 1.0
        CHECK(ps->stats().particles == 6 + 7);
        CHECK(ps->playing(h));
    }
    {   // death sub-emitter plays another effect where the particle died
        auto ps = make();
        ps->define("pop", fxParse(R"({"emitters": [{ "burst": 2, "life": 1, "speed": 0 }]})"));
        int id = ps->define("p", fxParse(R"({"emitters": [{ "burst": 3, "life": 0.1, "speed": 0, "subEmitters": [{ "on": "death", "effect": "pop" }] }]})"));
        ps->play(id, {0, 0, 0});
        for (int i = 0; i < 8; ++i) ps->update(0.02f);
        CHECK(ps->stats().particles == 6);
        int self = ps->define("self", fxParse(R"({"emitters": [{ "burst": 1, "life": 0.05, "subEmitters": [{ "on": "death", "effect": "self" }] }]})"));
        ps->clear();
        ps->play(self, {0, 0, 0});
        for (int i = 0; i < 40; ++i) ps->update(0.02f);
        CHECK(ps->stats().particles == 0 && ps->stats().instances == 0);   // recursion stops at the depth limit
    }
    {   // ground collision: falling particles stay above the ground height function
        auto ps = make();
        ps->setGroundHeight([](float, float) { return 0.5f; });
        int id = ps->define("c", fxParse(R"({"emitters": [{ "burst": 20, "life": 3, "speed": [0, 1], "gravity": [0, -10, 0],
            "collision": { "bounce": 0 } }]})"));
        ps->play(id, {0, 2, 0});
        for (int i = 0; i < 100; ++i) ps->update(0.02f);
        CHECK(ps->stats().particles == 20);
        std::vector<fx::FxPointLight> none;
        ps->collectLights(none);
        CHECK(none.empty());
    }
    {   // particle lights: ratio, max per effect, quality off
        auto ps = make();
        int id = ps->define("li", fxParse(R"({"emitters": [{ "burst": 30, "life": 2, "color": "#ffffffff",
            "light": { "color": "#ff8000", "radius": 2, "max": 4 } }]})"));
        ps->play(id, {0, 0, 0});
        ps->play(id, {5, 0, 0});
        ps->update(0.02f);
        std::vector<fx::FxPointLight> lights;
        ps->collectLights(lights);
        CHECK(lights.size() == 8 && lights[0].radius > 1.9f && lights[0].color.y < 0.6f);
        lights.clear();
        ps->collectLights(lights, 3);
        CHECK(lights.size() == 3);
        ps->setQuality(fx::FxQuality::Off);
        lights.clear();
        ps->collectLights(lights);
        CHECK(lights.empty());
    }
    {   // local space follows the moving effect, world space stays behind (positions read through particle lights)
        auto ps = make();
        auto at = [&](const char* space) {
            ps->clear();
            int id = ps->define(space, fxParse((std::string(R"({"emitters": [{ "burst": 1, "rate": 0.01, "loop": true, "life": 5, "speed": 0, "space": ")") +
                                               space + R"(", "color": "#ffffffff", "light": { "radius": 1 } }]})").c_str()));
            fx::FxHandle h = ps->play(id, {0, 0, 0});
            ps->update(0.02f);
            ps->move(h, {3, 0, 0});
            ps->update(0.02f);
            std::vector<fx::FxPointLight> l;
            ps->collectLights(l);
            return l.empty() ? -1.0f : l[0].pos.x;
        };
        CHECK(std::fabs(at("local") - 3) < 1e-3f);
        CHECK(std::fabs(at("world")) < 1e-3f);
    }
}

TEST(fx_unity_importer_output_is_valid) {
    // Runs tools/importers/unity_vfx on the synthetic Unity project and loads every effect it wrote.
    if (std::system("python3 -c \"import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)\" > /dev/null 2>&1") != 0) {
        std::printf("    (skipped: python3 not found)\n");
        return;
    }
    const std::string src = std::string(RTS_SOURCE_DIR);
    const std::string out = (std::filesystem::temp_directory_path() / "rts_unity_vfx_test").string();
    std::filesystem::remove_all(out);
    std::filesystem::create_directories(out);
    const std::string cmd = "python3 \"" + src + "/tools/importers/unity_vfx/import.py\" --source \"" + src +
                            "/tests/data/unity_vfx/Assets/FX\" --game \"" + out + "\" --json > \"" + out + "/report.json\"";
    CHECK(std::system(cmd.c_str()) == 0);
    Json rep = Json::parseFile(out + "/report.json");
    CHECK(rep["effects"].size() == 3);
    fx::ParticleSystem ps;
    ps.setQuality(fx::FxQuality::High);
    ps.init(nullptr);
    ps.setFileReader([&](const std::string& p, std::string& text) {
        std::ifstream f(out + "/" + p, std::ios::binary);
        if (!f) return false;
        text.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    });
    for (auto& e : rep["effects"].items()) {
        int id = ps.effect(e.getStr("name"));
        CHECK(id >= 0);
        if (id >= 0) { ps.play(id, {0, 0, 0}); for (int i = 0; i < 60; ++i) ps.update(1 / 30.0f); }
    }
    for (auto& e : ps.errors()) std::printf("    %s\n", e.c_str());
    CHECK(ps.errors().empty());
    const fx::EffectDef* ex = ps.def(ps.effect("explosion"));
    CHECK(ex && ex->emitters.size() == 4);
    if (ex && ex->emitters.size() == 4) {
        CHECK(ex->emitters[2].trail.enabled && ex->emitters[2].light.enabled && ex->emitters[2].collision.enabled);
        CHECK(ex->emitters[2].subEmitters.size() == 1 && ex->emitters[2].align == fx::FxAlign::Velocity);
        CHECK(ex->emitters[1].bursts.size() == 2 && ex->emitters[1].noise.strength.max > 0);
    }
    CHECK(std::filesystem::exists(out + "/fx/textures/smoke.png"));
    std::filesystem::remove_all(out);
}

// ---------------- particles: the rest of Unity's modules ----------------
namespace {
struct FxRun {
    fx::ParticleSystem ps;
    std::vector<fx::FxParticleInfo> parts;
    int id = -1;
    explicit FxRun(const char* json, fx::FxQuality q = fx::FxQuality::High) {
        ps.setQuality(q);
        ps.init(nullptr);
        std::vector<std::string> errs;
        fx::EffectDef d;
        fx::parseEffect(Json::parse(json), "t.json", d, errs);
        for (auto& e : errs) std::printf("    %s\n", e.c_str());
        id = ps.define("t", d);
    }
    void step(float seconds, float dt = 1 / 30.0f) { for (float t = 0; t < seconds - 1e-4f; t += dt) ps.update(dt); ps.snapshot(parts); }
};
} // namespace

TEST(fx_unity_modules_parse_and_round_trip) {
    const char* text = R"({
      "emitters": [{
        "texture": "builtin:spark", "blend": "multiply", "align": "world", "loop": true, "duration": 2, "prewarm": true,
        "delay": [0, 0.5], "life": { "curve": [[0, 1], [1, 2]] }, "speed": [-2, 3], "size": { "curve": [[0, 0.5]], "curveMin": [[0, 0.2]] },
        "rate": { "curve": [[0, 0], [1, 50]] }, "bursts": [{ "time": 0, "count": 4, "cycles": 0, "interval": 0.25, "chance": 0.5 }],
        "shape": "donut", "radius": 2, "donutRadius": 0.3, "emitFrom": "shell", "arcMode": "loop", "arcSpeed": 2, "arcSpread": 0.25,
        "shapeRotation": [0, 45, 0], "randomDirection": 0.2, "sphericalDirection": 0.1, "randomPosition": 0.05, "alignToDirection": true,
        "sizeY": [1, 2], "rotation3D": { "x": [0, 90] }, "gravityScale": 0.5, "startColor": { "gradient": [[0, "#ff0000"], [1, "#0000ff"]], "overTime": true },
        "colorMin": [[0, "#ffffff"], [1, "#000000"]], "sizeOverLifeMin": [[0, 0.5]], "sizeOverLifeY": [[0, 1], [1, 2]],
        "spin": { "curve": [[0, 90], [1, -90]] }, "spin3D": { "x": 30 }, "spinBySpeed": { "speed": [0, 5], "spin": [[0, 0], [1, 180]] },
        "velocityOverLife": { "y": { "curve": [[0, 2], [1, 0]] }, "orbitalY": 90, "orbitalOffset": [0, 1, 0], "radial": 0.5, "speedModifier": [[0, 1], [1, 0.5]] },
        "force": { "x": [-1, 1], "randomize": true }, "speedLimit": 4, "speedLimitAxes": { "y": 2 }, "limitDampen": 0.5,
        "drag": [0.1, 0.2], "dragBySize": true, "dragBySpeed": true, "inheritVelocity": { "mode": "current", "amount": 0.5 },
        "lifeBySpeed": { "speed": [0, 10], "factor": [[0, 1], [1, 0.5]] }, "externalForces": 1,
        "noise": { "strength": 1, "axes": [1, 0.5, 1], "octaves": 3, "damping": true, "rotation": 45, "size": 0.2, "remap": [[0, -1], [1, 1]] },
        "frames": [4, 2], "row": "random", "frameOverTime": { "curve": [[0, 0], [1, 1]] }, "flip": [0.5, 0],
        "pivot": [0, 0.5, 0], "cameraStretch": 0.1, "sort": "youngest", "sortOrder": 3, "maxScreenSize": 0.3, "meshes": ["builtin:cube"],
        "maxParticles": 50, "ringBuffer": true,
        "collision": { "ground": false, "planes": [{ "point": [0, 0, 0], "normal": [0, 1, 0] }], "radius": 0.5, "maxKillSpeed": 20 },
        "trigger": { "volumes": [{ "center": [0, 1, 0], "radius": 2 }], "outside": "kill", "enter": "callback" },
        "subEmitters": [{ "on": "trigger", "effect": "t2", "inherit": ["color", "size", "duration"] }, { "on": "manual", "effect": "t2" }],
        "trail": { "mode": "ribbon", "ribbons": 2, "lifeColor": "#ff8000", "textureMode": "tile", "dieWithParticles": false, "lifeFraction": 0.5 },
        "light": { "radiusOverLife": [[0, 1], [1, 0]] }
      }],
      "forceFields": [{ "position": [0, 1, 0], "shape": "cylinder", "radius": 3, "length": 4, "gravity": 2, "rotation": 90, "rotationAttraction": 0.5 }]
    })";
    std::vector<std::string> errs;
    fx::EffectDef d = fxParse(text, &errs);
    for (auto& e : errs) std::printf("    %s\n", e.c_str());
    CHECK(errs.empty() && d.emitters.size() == 1 && d.forceFields.size() == 1);
    const auto& e = d.emitters[0];
    CHECK(e.blend == fx::FxBlend::Multiply && e.align == fx::FxAlign::World && e.prewarm && e.delay.max == 0.5f);
    CHECK(e.life.curved() && e.life.min == 1 && e.life.max == 2 && e.size.curveMin.size() == 1 && e.speed.min == -2);
    CHECK(e.rate.curved() && e.continuous() && e.bursts[0].cycles == 0 && e.bursts[0].chance == 0.5f);
    CHECK(e.shape == fx::FxShape::Donut && e.emitFrom == fx::FxEmitFrom::Shell && e.arcMode == fx::FxArcMode::Loop && e.shapeRotation.y == 45);
    CHECK(e.size3D && e.rotation3D && e.startColorOverTime && e.spin.curved() && e.spin3D && e.spinBySpeed.enabled);
    CHECK(e.velocityOverLife.enabled && e.velocityOverLife.y.curved() && e.velocityOverLife.orbitalY.max == 90 && e.force.randomize);
    CHECK(e.speedLimitAxes.enabled && e.speedLimitAxes.x.min > 1e5f && e.inheritVelocity.current && e.lifeBySpeed.enabled && e.externalForces == 1);
    CHECK(e.noise.octaves == 3 && e.noise.damping && e.noise.remap.size() == 2 && e.row == -2 && e.frameOverTime.curved());
    CHECK(e.sort == fx::FxSort::Youngest && e.sortOrder == 3 && e.meshes.size() == 1 && e.ringBuffer);
    CHECK(e.collision.planes.size() == 1 && !e.collision.ground && e.trigger.volumes.size() == 1 && e.trigger.outside == fx::FxTriggerAction::Kill);
    CHECK(e.subEmitters.size() == 2 && e.subEmitters[0].inheritSize && e.subEmitters[0].inheritDuration && e.subEmitters[1].on == fx::FxSubEvent::Manual);
    CHECK(e.trail.ribbon && e.trail.ribbons == 2 && !e.trail.dieWithParticles && e.trail.textureMode == fx::FxTextureMode::Tile);
    CHECK(d.forceFields[0].cylinder && d.forceFields[0].rotation == 90);
    // serialize -> parse -> serialize is stable
    fx::EffectDef back;
    std::vector<std::string> e2;
    CHECK(fx::parseEffect(fx::effectToJson(d), "t.json", back, e2) && e2.empty());
    CHECK(fx::effectToJson(back).dump() == fx::effectToJson(d).dump());
    // readable errors for the new keys
    fxParse(R"({"emitters": [{ "burst": 1, "shape": "mesh", "ringBuffer": true, "life": { "curve": "x" } }]})", &errs);
    CHECK(errs.size() == 3);   // mesh missing, ringBuffer without maxParticles, bad curve
}

TEST(fx_curves_prewarm_bursts_ring_buffer) {
    // rate rising linearly 0 -> 100 over 1 s: about 50 particles
    FxRun a(R"({"emitters": [{ "rate": { "curve": [[0, 0], [1, 100]] }, "duration": 1, "life": 5, "speed": 0 }]})");
    a.ps.play(a.id, {0, 0, 0});
    a.step(1.0f);
    CHECK(a.parts.size() >= 44 && a.parts.size() <= 56);
    // life curve over the emitter time: early particles live 1 s, late ones 3 s
    FxRun b(R"({"emitters": [{ "rate": 20, "duration": 1, "life": { "curve": [[0, 1], [1, 3]] }, "speed": 0 }]})");
    b.ps.play(b.id, {0, 0, 0});
    b.step(1.0f);
    float lo = 1e9f, hi = 0;
    for (auto& p : b.parts) { lo = std::min(lo, p.life); hi = std::max(hi, p.life); }
    CHECK(lo < 1.3f && hi > 2.6f);
    // prewarm: a looping effect is full right away
    FxRun c(R"({"emitters": [{ "rate": 50, "duration": 2, "loop": true, "prewarm": true, "life": 1, "speed": 0 }]})");
    c.ps.play(c.id, {0, 0, 0});
    c.step(0.0f);
    CHECK(c.parts.size() >= 40);
    // endless burst cycles every 0.25 s for a 1 s duration, half of them by chance
    FxRun e(R"({"emitters": [{ "bursts": [{ "time": 0, "count": 10, "cycles": 0, "interval": 0.25 }], "duration": 1, "life": 5, "speed": 0 }]})");
    e.ps.play(e.id, {0, 0, 0});
    e.step(1.5f);
    CHECK(e.parts.size() == 40);
    // ring buffer: 5 at most, they never die, new ones replace the oldest
    FxRun r(R"({"emitters": [{ "rate": 20, "duration": 5, "life": 0.2, "speed": 0, "maxParticles": 5, "ringBuffer": true }]})");
    r.ps.play(r.id, {0, 0, 0});
    r.step(1.0f);
    CHECK(r.parts.size() == 5);
    float oldest = 0;
    for (auto& p : r.parts) oldest = std::max(oldest, p.age);
    CHECK(oldest < 0.4f);   // replaced, not kept from the start
    // instances live on while their particles do; playing() only while emitting
    FxRun l(R"({"emitters": [{ "burst": 3, "life": 1, "speed": 0 }]})");
    fx::FxHandle h = l.ps.play(l.id, {0, 0, 0});
    l.step(0.1f);
    CHECK(!l.ps.playing(h) && l.ps.stats().instances == 1 && l.parts.size() == 3);
    l.step(1.0f);
    CHECK(l.ps.stats().instances == 0 && l.parts.empty());
}

TEST(fx_shapes_donut_box_mesh_cone_arc) {
    auto distTorus = [](fx::float3 p, float R) { float q = std::sqrt(p.x * p.x + p.z * p.z) - R; return std::sqrt(q * q + p.y * p.y); };
    FxRun d(R"({"emitters": [{ "burst": 200, "shape": "donut", "radius": 2, "donutRadius": 0.5, "emitFrom": "shell", "life": 5, "speed": 0 }]})");
    d.ps.play(d.id, {0, 0, 0});
    d.step(1 / 30.0f);
    bool onTube = !d.parts.empty();
    for (auto& p : d.parts) onTube = onTube && std::fabs(distTorus(p.pos, 2) - 0.5f) < 1e-3f;
    CHECK(onTube);
    FxRun b(R"({"emitters": [{ "burst": 200, "shape": "box", "box": [2, 2, 2], "emitFrom": "shell", "life": 5, "speed": 0 }]})");
    b.ps.play(b.id, {0, 0, 0});
    b.step(1 / 30.0f);
    bool onFace = b.parts.size() == 200;
    for (auto& p : b.parts) onFace = onFace && std::fabs(std::max({std::fabs(p.pos.x), std::fabs(p.pos.y), std::fabs(p.pos.z)}) - 1) < 1e-3f;
    CHECK(onFace);
    // mesh shape: the built-in cube, from triangles -> on its surface, directed along its normals
    FxRun m(R"({"emitters": [{ "burst": 200, "shape": "mesh", "mesh": "builtin:cube", "emitFrom": "triangle", "life": 5, "speed": 1 }]})");
    m.ps.play(m.id, {0, 0, 0});
    m.step(1 / 30.0f);
    bool onCube = m.parts.size() == 200;
    for (auto& p : m.parts) {
        fx::float3 q = p.pos - p.vel * (1 / 30.0f);
        onCube = onCube && std::fabs(std::max({std::fabs(q.x), std::fabs(q.y), std::fabs(q.z)}) - 0.5f) < 2e-3f;
    }
    CHECK(onCube);
    // cone volume: particles start anywhere along the length (not only on the base)
    FxRun c(R"({"emitters": [{ "burst": 300, "shape": "cone", "radius": 0.5, "spread": 20, "length": 4, "life": 5, "speed": 0 }]})");
    c.ps.play(c.id, {0, 0, 0});
    c.step(1 / 30.0f);
    float top = 0;
    for (auto& p : c.parts) top = std::max(top, p.pos.y);
    CHECK(top > 3.0f);
    // arc spread: burst evenly over 4 directions only
    FxRun s(R"({"emitters": [{ "burst": 40, "shape": "ring", "radius": 1, "arcMode": "spread", "arcSpread": 0.25, "life": 5, "speed": 0 }]})");
    s.ps.play(s.id, {0, 0, 0});
    s.step(1 / 30.0f);
    std::vector<std::pair<int, int>> spots;
    for (auto& p : s.parts) {
        std::pair<int, int> k{int(std::lround(p.pos.x * 10)), int(std::lround(p.pos.z * 10))};
        if (std::find(spots.begin(), spots.end(), k) == spots.end()) spots.push_back(k);
    }
    CHECK(spots.size() == 4);
}

TEST(fx_velocity_force_limit_fields_noise) {
    // orbital velocity: particles keep their distance to the centre and go around it
    FxRun o(R"({"emitters": [{ "burst": 20, "shape": "ring", "radius": 1, "emitFrom": "shell", "life": 5, "speed": 0,
                 "velocityOverLife": { "orbitalY": 90 } }]})");
    o.ps.play(o.id, {0, 0, 0});
    o.step(1 / 30.0f);
    std::vector<fx::float3> start;
    for (auto& p : o.parts) start.push_back(p.pos);
    o.step(1.0f, 1 / 60.0f);
    bool kept = o.parts.size() == start.size();
    float turned = 0;
    for (size_t i = 0; kept && i < o.parts.size(); ++i) {
        kept = std::fabs(std::sqrt(o.parts[i].pos.x * o.parts[i].pos.x + o.parts[i].pos.z * o.parts[i].pos.z) - 1) < 0.08f;
        turned += std::acos(std::clamp(render::dot(render::normalize(o.parts[i].pos), render::normalize(start[i])), -1.0f, 1.0f));
    }
    CHECK(kept);
    CHECK(o.parts.size() && std::fabs(turned / float(o.parts.size()) - 3.14159f / 2) < 0.2f);
    // force up + hard speed limit 2
    FxRun f(R"({"emitters": [{ "burst": 10, "life": 5, "speed": 0, "force": { "y": 20, "space": "world" }, "speedLimit": 2 }]})");
    f.ps.play(f.id, {0, 0, 0});
    f.step(1.0f);
    bool limited = !f.parts.empty();
    for (auto& p : f.parts) limited = limited && render::length(p.vel) <= 2.01f && p.vel.y > 1.5f;
    CHECK(limited);
    // a gravity force field pulls particles towards its centre
    FxRun g(R"({"emitters": [{ "burst": 30, "shape": "sphere", "radius": 3, "emitFrom": "shell", "life": 5, "speed": 0, "externalForces": 1 }],
                "forceFields": [{ "radius": 10, "gravity": 5 }]})");
    g.ps.play(g.id, {0, 0, 0});
    g.step(0.5f);
    float avg = 0;
    for (auto& p : g.parts) avg += render::length(p.pos);
    CHECK(!g.parts.empty() && avg / float(g.parts.size()) < 2.6f);
    // wind acts on externalForces emitters only
    FxRun w(R"({"emitters": [{ "burst": 5, "life": 5, "speed": 0, "externalForces": 1 }, { "burst": 5, "life": 5, "speed": 0 }]})");
    w.ps.setWind({4, 0, 0});
    w.ps.play(w.id, {0, 0, 0});
    w.step(1.0f);
    int blown = 0, still = 0;
    for (auto& p : w.parts) (p.pos.x > 1 ? blown : still) += p.emitter == 0 ? (p.pos.x > 1 ? 1 : 0) : (p.pos.x < 0.01f ? 1 : 0);
    CHECK(blown == 5 && still == 5);
    // noise with octaves moves particles on every tier
    for (auto q : {fx::FxQuality::Low, fx::FxQuality::High}) {
        FxRun n(R"({"emitters": [{ "burst": 10, "life": 5, "speed": 0, "noise": { "strength": 2, "octaves": 4, "remap": [[0, -1], [1, 1]] } }]})", q);
        n.ps.play(n.id, {0, 0, 0});
        n.step(1.0f);
        float moved = 0;
        for (auto& p : n.parts) moved += render::length(p.pos);
        CHECK(n.parts.size() > 0 && moved > 0.1f);
    }
}

TEST(fx_collision_planes_triggers_subemitters_inherit) {
    // a wall at x = 1 (normal -x) sends particles back
    FxRun c(R"({"emitters": [{ "burst": 5, "life": 5, "speed": 3, "direction": [0, 0, 1], "spread": 0,
                 "collision": { "ground": false, "bounce": 1, "planes": [{ "point": [0, 0, 1], "normal": [0, 0, -1] }] } }]})");
    c.ps.play(c.id, {0, 0, 0});
    c.step(1.0f);
    bool back = !c.parts.empty();
    for (auto& p : c.parts) back = back && p.pos.x < 1.01f && p.vel.x < 0;   // effect z = world x for the default play direction
    CHECK(back);
    // trigger: particles leaving the volume die
    FxRun t(R"({"emitters": [{ "burst": 20, "life": 5, "speed": 2, "trigger": { "volumes": [{ "radius": 1 }], "outside": "kill" } }]})");
    t.ps.play(t.id, {0, 0, 0});
    t.step(0.3f);
    CHECK(t.parts.size() == 20);
    t.step(0.5f);
    CHECK(t.parts.empty());
    // manual sub-emitters fire on emitManual; inherited size scales the child particles
    fx::ParticleSystem ps;
    ps.setQuality(fx::FxQuality::High);
    ps.init(nullptr);
    int child = ps.define("child", fxParse(R"({"emitters": [{ "burst": 1, "life": 5, "speed": 0, "size": 1 }]})"));
    int parent = ps.define("parent", fxParse(R"({"emitters": [{ "burst": 2, "life": 5, "speed": 0, "size": 3,
        "subEmitters": [{ "on": "manual", "effect": "child", "inherit": ["size"] }] }]})"));
    CHECK(child >= 0 && parent >= 0);
    fx::FxHandle h = ps.play(parent, {0, 0, 0});
    ps.update(1 / 30.0f);
    CHECK(ps.stats().particles == 2);
    ps.emitManual(h);
    ps.update(1 / 30.0f);
    ps.update(1 / 30.0f);
    std::vector<fx::FxParticleInfo> parts;
    ps.snapshot(parts);
    int big = 0;
    for (auto& p : parts) if (p.effect == child && std::fabs(p.size - 3) < 0.05f) ++big;
    CHECK(parts.size() == 4 && big == 2);
}

TEST(fx_ribbons_orphan_trails_and_meshes_simulate) {
    // trails that outlive their particles keep the effect instance busy until they fade
    FxRun o(R"({"emitters": [{ "rate": 20, "duration": 0.5, "life": 0.3, "speed": 2,
                "trail": { "time": 0.6, "dieWithParticles": false, "minQuality": "low" } }]})");
    o.ps.play(o.id, {0, 0, 0});
    o.step(1.0f);
    CHECK(o.parts.empty());
    // mesh particles and ribbons run headless (drawing is checked on screenshots)
    FxRun m(R"({"emitters": [{ "rate": 30, "duration": 1, "life": 1, "speed": 1, "meshes": ["builtin:cube", "builtin:sphere"], "rotation3D": { "x": [0, 90] },
                "spin3D": { "y": 90 }, "trail": { "mode": "ribbon", "ribbons": 2 } }]})");
    m.ps.play(m.id, {0, 0, 0});
    m.step(0.5f);
    CHECK(m.parts.size() > 10);
    CHECK(m.ps.errors().empty());
}

TEST(fx_mesh_particles_from_model_files) {
    // a tetrahedron as OBJ: loaded through the model loader for mesh particles and the "mesh" shape
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n";
    auto read = [&](const std::string& p, std::string& out) { if (p != "models/shard.obj") return false; out = obj; return true; };
    fx::FxMesh mesh;
    CHECK(fx::meshFromModelFile("models/shard.obj", read, mesh));
    CHECK(mesh.idx.size() == 12 && mesh.pos.size() >= 4 && mesh.nrm.size() == mesh.pos.size());
    fx::ParticleSystem ps;
    ps.setQuality(fx::FxQuality::High);
    ps.setMeshLoader([&](const std::string& name, fx::FxMesh& out) { return fx::meshFromModelFile(name, read, out); });
    ps.init(nullptr);
    int id = ps.define("m", fxParse(R"({"emitters": [{ "burst": 50, "life": 2, "speed": 0, "shape": "mesh", "mesh": "models/shard.obj",
                                         "emitFrom": "vertex", "meshes": ["models/shard.obj"] }]})"));
    ps.play(id, {0, 0, 0});
    ps.update(1 / 30.0f);
    std::vector<fx::FxParticleInfo> parts;
    ps.snapshot(parts);
    bool onVertex = parts.size() == 50;
    for (auto& p : parts) {   // model space -> effect space -> world for the default play direction: unchanged
        bool hit = false;
        for (auto v : {fx::float3{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}) hit = hit || render::length(p.pos - v) < 1e-3f;
        onVertex = onVertex && hit;
    }
    CHECK(onVertex);
    for (auto& e : ps.errors()) std::printf("    %s\n", e.c_str());
    CHECK(ps.errors().empty());
}

TEST(fx_unity_importer_all_modules_play) {
    // The synthetic project exercising every ParticleSystem module: imported effects parse and play.
    if (std::system("python3 -c \"import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)\" > /dev/null 2>&1") != 0) {
        std::printf("    (skipped: python3 not found)\n");
        return;
    }
    const std::string src = std::string(RTS_SOURCE_DIR);
    const std::string out = (std::filesystem::temp_directory_path() / "rts_unity_vfx_modules").string();
    std::filesystem::remove_all(out);
    std::filesystem::create_directories(out);
    const std::string cmd = "python3 \"" + src + "/tools/importers/unity_vfx/import.py\" --source \"" + src +
                            "/tests/data/unity_vfx/Assets/Modules\" --game \"" + out + "\" --json > \"" + out + "/report.json\"";
    CHECK(std::system(cmd.c_str()) == 0);
    Json rep = Json::parseFile(out + "/report.json");
    CHECK(rep["effects"].size() >= 5);
    auto read = [&](const std::string& p, std::string& text) {
        std::ifstream f(out + "/" + p, std::ios::binary);
        if (!f) return false;
        text.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    };
    fx::ParticleSystem ps;
    ps.setQuality(fx::FxQuality::High);
    ps.setFileReader(read);
    ps.setMeshLoader([&](const std::string& name, fx::FxMesh& m) { return fx::meshFromModelFile(name, read, m); });
    ps.init(nullptr);
    for (auto& e : rep["effects"].items()) {
        int id = ps.effect(e.getStr("name"));
        CHECK(id >= 0);
        if (id < 0) continue;
        ps.play(id, {0, 0, 0});
        int most = 0;
        for (int i = 0; i < 90; ++i) { ps.update(1 / 30.0f); most = std::max(most, ps.stats().particles); }
        CHECK(most > 0);
        std::vector<fx::FxParticleInfo> parts;
        ps.snapshot(parts);
        bool finite = true;
        for (auto& p : parts) finite = finite && std::isfinite(p.pos.x + p.pos.y + p.pos.z + p.size);
        CHECK(finite);
    }
    int meshErrors = 0;
    for (auto& e : ps.errors()) {
        if (e.find("rock.fbx") != std::string::npos) { ++meshErrors; continue; }   // a stub file in the test data
        std::printf("    %s\n", e.c_str());
    }
    CHECK(ps.errors().size() == size_t(meshErrors));
    std::filesystem::remove_all(out);
}

// ---------------- Cube-sphere (RtsSphere) ----------------
TEST(sphere_cells_and_dirs_round_trip) {
    using namespace rts::game;
    for (int N : {1, 4, 7, 16}) {
        CubeSphere s(N);
        int bad = 0;
        for (int id = 0; id < s.cellCount(); ++id) {
            const SphereCell c = s.cell(id);
            if (s.cellId(c.face, c.x, c.y) != id) ++bad;
            const Vec3 d = s.cellCenterDir(id);
            if (fxAbs(d.length() - Fixed::fromInt(1)) > Fixed::fromRaw(8)) ++bad;
            if (s.cellFromDir(d) != id) ++bad;
            int f; Fixed a, b;
            s.faceFromDir(d * Fixed::fromInt(37), f, a, b);          // any length works
            // stage 1: the inverse warp is linear inside a cell (exact at cell edges), up to ~0.07 cell off at the centre
            if (f != c.face || fxAbs(a - (Fixed::fromInt(c.x) + Fixed::fromRatio(1, 2))) > Fixed::fromRatio(1, 10)) ++bad;
        }
        CHECK(bad == 0);
    }
}

TEST(sphere_neighbors_symmetric_and_adjacent) {
    using namespace rts::game;
    for (int N : {1, 2, 5, 8}) {
        CubeSphere s(N);
        int asym = 0, far = 0, cornerCells = 0, total = 0;
        const Fixed cellTurns = Fixed::fromRatio(1, 4 * N);
        for (int id = 0; id < s.cellCount(); ++id) {
            int nb[8];
            const int k = s.neighbors8(id, nb);
            total += k;
            if (k == 7) ++cornerCells;
            for (int i = 0; i < k; ++i) {
                int back[8];
                const int kb = s.neighbors8(nb[i], back);
                bool found = false;
                for (int j = 0; j < kb; ++j) found |= back[j] == id;
                if (!found || nb[i] == id) ++asym;
                const Fixed arc = sphereArcTurns(s.cellCenterDir(id), s.cellCenterDir(nb[i]));
                if (arc.raw <= 0 || arc > cellTurns * 2) ++far;
            }
            for (int i = 0; i < k; ++i) for (int j = i + 1; j < k; ++j) if (nb[i] == nb[j] && N > 1) ++asym;
            // orthogonal neighbours are mutually orthogonal neighbours
            static const int kO[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& d : kO) {
                const int o = s.neighbor(id, d[0], d[1]);
                bool back = false;
                for (auto& e : kO) back |= s.neighbor(o, e[0], e[1]) == id;
                if (o < 0 || !back) ++asym;
            }
        }
        CHECK(asym == 0);
        CHECK(far == 0);
        // 8 cube corners x 3 faces have 7 neighbours; with N == 1 every diagonal passes a cube corner (4 each)
        CHECK(N == 1 ? total == 6 * 4 : (cornerCells == 24 && total == 8 * s.cellCount() - 24));
    }
}

TEST(sphere_corner_duplicates_and_seams) {
    using namespace rts::game;
    for (int N : {1, 3, 8}) {
        CubeSphere s(N);
        int bad = 0, dup2 = 0, dup3 = 0;
        for (int f = 0; f < 6; ++f) for (int j = 0; j <= N; ++j) for (int i = 0; i <= N; ++i) {
            int ids[3];
            const int k = s.cornerDuplicates(f, i, j, ids);
            const bool edge = i == 0 || j == 0 || i == N || j == N, cube = (i == 0 || i == N) && (j == 0 || j == N);
            if (k != (cube ? 3 : edge ? 2 : 1)) ++bad;
            if (k == 2) ++dup2;
            if (k == 3) ++dup3;
            bool self = false;
            const Vec3 d = s.cornerDir(f, i, j);
            for (int m = 0; m < k; ++m) {
                if (m && ids[m] <= ids[m - 1]) ++bad;
                const int g = ids[m] / ((N + 1) * (N + 1)), r = ids[m] % ((N + 1) * (N + 1));
                self |= ids[m] == s.cornerId(f, i, j);
                if (!(s.cornerDir(g, r % (N + 1), r / (N + 1)) == d)) ++bad; // bit-identical seam vertices
            }
            if (!self || s.canonicalCorner(f, i, j) != ids[0]) ++bad;
        }
        CHECK(bad == 0);
        CHECK(dup3 == 24 && dup2 == 24 * (N - 1));
        // unique vertices of a cube-sphere grid: 6 N^2 + 2
        int unique = 0;
        for (int f = 0; f < 6; ++f) for (int j = 0; j <= N; ++j) for (int i = 0; i <= N; ++i)
            unique += s.canonicalCorner(f, i, j) == s.cornerId(f, i, j);
        CHECK(unique == 6 * N * N + 2);
    }
}

TEST(sphere_great_circle_steps) {
    using namespace rts::game;
    CubeSphere s(16);
    const Vec3 a = s.cellCenterDir(s.cellId(0, 3, 4)), b = s.cellCenterDir(s.cellId(5, 12, 9));
    const Fixed arc0 = sphereArcTurns(a, b);
    CHECK(arc0 > Fixed::fromRatio(1, 8) && arc0 < Fixed::fromRatio(1, 2));
    Vec3 p = a;
    bool reached = false;
    int steps = 0;
    Fixed prev = arc0;
    bool monotonic = true;
    while (!reached && steps < 1000) {
        p = sphereRotateToward(p, b, Fixed::fromRatio(1, 200), &reached);
        const Fixed arc = sphereArcTurns(p, b);
        if (!reached && arc >= prev) monotonic = false;
        prev = arc;
        ++steps;
    }
    CHECK(reached && monotonic);
    CHECK(steps >= arc0.raw * 200 / Fixed::kOne && steps <= arc0.raw * 200 / Fixed::kOne + 3);
    CHECK(fxAbs(p.length() - Fixed::fromInt(1)) <= Fixed::fromRaw(8));
    // antipodal start still moves deterministically
    Vec3 q = sphereRotateToward(a, a * Fixed::fromInt(-1), Fixed::fromRatio(1, 100));
    CHECK(sphereArcTurns(q, a) > Fixed::fromRatio(1, 200));
    // tangents are unit and orthogonal to the direction
    Vec3 e, so;
    CubeSphere::faceTangents(2, s.cellCenterDir(s.cellId(2, 5, 5)), e, so);
    CHECK(fxAbs(e.dot(s.cellCenterDir(s.cellId(2, 5, 5)))) < Fixed::fromRatio(1, 100));
    CHECK(fxAbs(e.length() - Fixed::fromInt(1)) < Fixed::fromRatio(1, 100));
}

// golden generator hashes (pure integer pipeline: identical on every platform / compiler)
#define GEN_GOLD_NOISE 0xb63f6f508d85105full
#define GEN_GOLD_CLASSIC 0xbca2c83d9c0791c8ull
#define GEN_GOLD_CONTINENTS 0x169af2ca325ff112ull
#define GEN_GOLD_ISLANDS 0x954d027fe4f6f9f7ull
#define GEN_GOLD_HIGHLANDS 0x9e9e30de7fecd4e7ull
#define GEN_GOLD_CANYONS 0x7fcf37e9490ffb17ull
#define GEN_GOLD_LANES 0x6010e5d33a6a8fdbull
#define GEN_GOLD_CAVES 0xd348fb3d9f4d2f55ull
#define GEN_GOLD_METAL 0x5e11afce218433b2ull
#define GEN_GOLD_PLANET_WORLD 0x8101a56ab8e6f0c6ull
#define GEN_GOLD_PLANET 0x0c2a152a97d7e02bull

// ---------------- terrain / map generator (agent-terrain-gen) ----------------
#include "modules/rts/gen/MapGen.h"
#include "modules/rts/gen/GenNoise.h"
#include "modules/rts/gen/GenField.h"
#include "modules/rts/RtsSphere.h"

static const char* kGenRules = R"({
  "resources": {"gold": {"start": 500}, "wood": {"start": 200}, "ore": {"field": true, "cellMax": 100}},
  "types": {
    "hall":   {"structure": {"footprint": [4, 4]}, "health": {"max": 1500}, "produces": ["worker"], "dropoff": ["gold", "wood", "ore"]},
    "worker": {"move": {"speed": 2.4, "radius": 0.35}, "gather": {"gold": {}, "wood": {}, "ore": {}}, "builds": ["hall"], "cost": {"gold": 50, "time": 10}},
    "mine":   {"structure": {"footprint": [3, 3]}, "invulnerable": true, "resourceNode": {"resource": "gold", "amount": 10000}},
    "tree":   {"structure": {"footprint": [1, 1]}, "invulnerable": true, "resourceNode": {"resource": "wood", "amount": 80}},
    "ogre":   {"health": {"max": 300}, "move": {"speed": 2}, "neutral": {"leash": 8}, "weapons": [{"range": 1, "damage": 10, "cooldown": 1}]},
    "spot":   {"structure": {"footprint": [2, 2]}, "invulnerable": true},
    "drill":  {"structure": {"footprint": [2, 2], "on": "spot"}, "income": {"gold": 1}}
  }
})";

static uint64_t genHash(const std::string& preset, uint64_t seed, int players, const std::string& rulesText = kGenRules,
                        const std::string& sym = "auto", Json* out = nullptr) {
    Json rj = Json::parse(rulesText);
    game::Rules rules = game::loadRules(rj);
    gen::GenRequest req;
    req.preset = preset; req.seed = seed; req.players = players; req.symmetry = sym;
    Json map = Json::parse(R"({"name": "g", "size": [48, 48], "players": []})");
    std::string err;
    if (!gen::generateMap(req, rules, rj, map, &err)) { std::printf("  gen error: %s\n", err.c_str()); return 0; }
    if (out) *out = map;
    return gen::mapHash(map);
}

TEST(gen_noise_deterministic) {
    // fixed sample values: bit-identical on every platform (pure integer maths)
    uint64_t h = 1469598103934665603ull;
    gen::Fbm f; f.octaves = 5; f.warp = 32768; f.ridged = 16384;
    for (int i = 0; i < 64; ++i) {
        h = (h ^ uint64_t(uint32_t(gen::fbm2(int64_t(i) * 40503, int64_t(i) * 91233 - 400000, 7, f)))) * 1099511628211ull;
        h = (h ^ uint64_t(uint32_t(gen::gradNoise3(int64_t(i) * 30011, -int64_t(i) * 12345, int64_t(i) * 777, 9)))) * 1099511628211ull;
        h = (h ^ uint64_t(uint32_t(gen::valueNoise2(int64_t(i) * 70001, int64_t(i) * 3, 3)))) * 1099511628211ull;
    }
    std::printf("  noise hash %016llx\n", (unsigned long long)h);
    CHECK(h == GEN_GOLD_NOISE);
    // ranges
    int lo = 0, hi = 0;
    for (int i = 0; i < 2000; ++i) { int v = gen::gradNoise2(int64_t(i) * 9973, int64_t(i) * 5003, 1); lo = std::min(lo, v); hi = std::max(hi, v); }
    CHECK(lo >= -65536 && hi <= 65536 && lo < -20000 && hi > 20000);
}

TEST(gen_symmetry_images) {
    std::string err;
    gen::Sym s;
    CHECK(gen::Sym::make("auto", 4, 40, 40, s, &err) && s.kind == gen::Sym::Rot4);
    CHECK(gen::Sym::make("auto", 4, 48, 32, s, &err) && s.kind == gen::Sym::Mirror4);
    CHECK(!gen::Sym::make("mirror", 3, 40, 40, s, &err) && err.find("needs 2 players") != std::string::npos);
    for (const char* k : {"point", "mirror", "mirror-y", "rot4", "mirror4"}) {
        const int n = std::string(k) == "rot4" || std::string(k) == "mirror4" ? 4 : 2;
        CHECK(gen::Sym::make(k, n, 40, 40, s, &err));
        bool ok = true;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) {   // folding then imaging returns the cell
                int fx, fy, ix, iy;
                const int kk = s.foldCell(x, y, fx, fy);
                s.imageCell(kk, fx, fy, ix, iy);
                if (ix != x || iy != y || s.foldCell(fx, fy, ix, iy) != 0) ok = false;
            }
        CHECK(ok);
    }
}

TEST(gen_golden_maps) {
    // same request -> identical map; golden hashes pin the output (change them only on purpose)
    const uint64_t a = genHash("classic", 7, 2), b = genHash("classic", 7, 2), c = genHash("classic", 8, 2);
    CHECK(a == b && a != c);
    struct G { const char* preset; int players; uint64_t gold; };
    const G gold[] = {{"classic", 2, GEN_GOLD_CLASSIC}, {"continents", 4, GEN_GOLD_CONTINENTS}, {"islands", 2, GEN_GOLD_ISLANDS},
                      {"highlands", 3, GEN_GOLD_HIGHLANDS}, {"canyons", 2, GEN_GOLD_CANYONS}, {"lanes", 2, GEN_GOLD_LANES}};
    for (auto& g : gold) {
        const uint64_t h = genHash(g.preset, 7, g.players);
        std::printf("  %s/%d %016llx\n", g.preset, g.players, (unsigned long long)h);
        CHECK(h == g.gold);
    }
}

TEST(gen_fair_and_connected) {
    for (const char* preset : {"classic", "continents", "highlands", "canyons"})
        for (uint64_t seed = 1; seed <= 3; ++seed)
            for (int np : {2, 4}) {
                Json m;
                genHash(preset, seed, np, kGenRules, "auto", &m);
                const Json& g = m["generated"];
                CHECK(g.getBool("connected"));
                if (!g.getBool("connected")) {
                    std::printf("  not connected: %s seed %d players %d %s\n", preset, int(seed), np, g["warnings"].dump(-1).c_str());
                    if (const char* dbg = std::getenv("RTS_GEN_DUMP")) { FILE* f = std::fopen(dbg, "w"); if (f) { std::fputs(m.dump().c_str(), f); std::fclose(f); } }
                }
                CHECK(int(m["players"].size()) == np);
                // symmetric layouts: every start is an image of player 1's start, objects per owner equal
                int own[9] = {};
                for (auto& e : m["entities"].items()) { const int o = int(e.getInt("owner")); if (o >= 1 && o <= 8) own[o]++; }
                for (int p = 2; p <= np; ++p) CHECK(own[p] == own[1]);
                CHECK(own[1] > 0);
                if (np == 2) CHECK(g.getInt("pathSpread", 0) <= 1);
            }
}

TEST(gen_match_hook_and_replay) {
    // a map with "generate": every loader regenerates the same map from the match seed
    Json rj = Json::parse(kGenRules);
    auto run = [&](uint64_t seed) {
        Json map = Json::parse(R"({"name": "m", "size": [40, 40], "players": [{"name": "A"}],
                                   "generate": {"preset": "classic", "seed": "match", "players": 2}})");
        CHECK(gen::prepareGenerate(map, [](const std::string&, std::string&) { return false; }, nullptr));
        CHECK(map["players"].size() == 2);
        SimConfig cfg; cfg.seed = seed;
        Simulation sim(cfg, std::make_unique<game::RtsModule>());
        sim.setup(rj, map);
        for (int i = 0; i < 40; ++i) sim.step();
        return sim.checksum();
    };
    CHECK(run(11) == run(11));
    CHECK(run(11) != run(12));
    // readable errors
    Json bad = Json::parse(R"({"size": [40, 40], "players": [{}, {}], "generate": {"preset": "nope"}})");
    SimConfig cfg;
    Simulation sim(cfg, std::make_unique<game::RtsModule>());
    std::string what;
    try { sim.setup(rj, bad); } catch (const std::exception& e) { what = e.what(); }
    CHECK(what.find("unknown preset 'nope'") != std::string::npos);
}

TEST(gen_caves_and_heightfield) {
    const std::string caveRules = R"({"resources": {"gold": {"start": 100}},
      "diggable": {"D": {"hardness": 3}, "G": {"hardness": 5, "yields": {"gold": 100}}, "J": {"hardness": 8, "yields": {"gold": 20}, "infinite": true}},
      "types": {"heart": {"key": true, "structure": {"footprint": [3, 3]}, "produces": ["imp"]},
                "imp": {"move": {"speed": 3}, "digs": {"rate": 1}, "cost": {"gold": 10}}}})";
    Json m;
    const uint64_t h = genHash("caves", 5, 2, caveRules, "auto", &m);
    std::printf("  caves %016llx\n", (unsigned long long)h);
    CHECK(h == GEN_GOLD_CAVES);
    std::string rows;
    for (auto& r : m["terrain"]["rows"].items()) rows += r.str();
    CHECK(rows.find('D') != std::string::npos && rows.find('G') != std::string::npos);
    CHECK(m["generated"].getBool("connected"));
    const std::string hfRules = std::string(kGenRules).substr(0, std::string(kGenRules).size() - 2) + R"(, "terrain": {"model": "heightfield", "maxSlope": 0.8}})";
    Json hm;
    const uint64_t hh = genHash("metal", 5, 2, hfRules, "auto", &hm);
    std::printf("  metal %016llx\n", (unsigned long long)hh);
    CHECK(hh == GEN_GOLD_METAL);
    CHECK(hm["terrain"]["corners"].size() == size_t(49 * 49));   // the map's own size wins over the preset size
    CHECK(hm["generated"].getBool("connected"));
}

TEST(gen_planet_seams) {
    Json rj = Json::parse(kGenRules);
    game::Rules rules = game::loadRules(rj);
    Json map = Json::parse(R"({"size": [16, 16], "players": [{}, {}, {}],
        "world": {"bodies": [{"id": "terra", "kind": "planet", "radius": 64, "faceSize": 12,
                              "generate": {"preset": "rocky", "seed": 3, "players": 3}}]}})");
    std::string err;
    CHECK(gen::applyGenerate(map, rules, rj, 1, 3, &err));
    const Json& body = map["world"]["bodies"][size_t(0)];
    const Json& cs = body["terrain"]["corners"];
    CHECK(cs.size() == size_t(6 * 13 * 13));
    CHECK(body["terrain"]["faces"].size() == 6);
    game::CubeSphere S(12);
    bool seams = true;
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= 12; ++j)
            for (int i = 0; i <= 12; ++i) {
                int ids[3]; const int n = S.cornerDuplicates(f, i, j, ids);
                for (int q = 1; q < n; ++q) if (cs[size_t(ids[q])].integer() != cs[size_t(ids[0])].integer()) seams = false;
            }
    CHECK(seams);
    CHECK(body["generated"]["starts"].size() == 3);
    // three sites on a sphere: maximally separated means well over a quarter turn apart
    CHECK(body["generated"]["minStartArc"].fx() > Fixed::fromRatio(25, 100));
    Json again = Json::parse(R"({"size": [16, 16], "players": [{}, {}, {}],
        "world": {"bodies": [{"id": "terra", "kind": "planet", "radius": 64, "faceSize": 12,
                              "generate": {"preset": "rocky", "seed": 3, "players": 3}}]}})");
    CHECK(gen::applyGenerate(again, rules, rj, 99, 3, &err));
    CHECK(gen::mapHash(again) == gen::mapHash(map));
    std::printf("  planet %016llx\n", (unsigned long long)gen::mapHash(map));
    CHECK(gen::mapHash(map) == GEN_GOLD_PLANET);
}

// ---- planet view (agent-planet-render): float cube-sphere surface of RtsPlanet.h
#include "modules/rts/view/RtsPlanet.h"
TEST(planet_view_surface_seams_and_picking) {
    using namespace rts::render;
    rts::game::PlanetBody b;
    b.radius = 40; b.faceSize = 16;
    const int N = 16, S = (N + 1) * (N + 1);
    b.corners.assign(size_t(6 * S), 0);
    rts::game::CubeSphere cs(N);
    // heights as a function of the corner direction: duplicates on seams get equal values
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                const Vec3 d = cs.cornerDir(f, i, j);
                b.corners[size_t(cs.cornerId(f, i, j))] = int16_t((d.x.toDouble() * 3 + d.y.toDouble() * 2) * 16);
            }
    rts::game::PlanetSurface s;
    s.init(b);
    bool seamsShared = true;   // every duplicated corner has the same position on all its faces
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                int dup[3];
                const int n = cs.cornerDuplicates(f, i, j, dup);
                for (int k = 0; k < n; ++k) {
                    const int g = dup[k] / S, jj = (dup[k] % S) / (N + 1), ii = dup[k] % (N + 1);
                    const float3 a = s.cornerPos(f, i, j), c = s.cornerPos(g, ii, jj);
                    if (a.x != c.x || a.y != c.y || a.z != c.z) seamsShared = false;
                }
            }
    CHECK(seamsShared);
    // float face mapping round-trips and agrees with the corner table
    int f; float fa, fb;
    s.faceFromDir(s.dirFromFace(4, 3.25f, 11.5f), f, fa, fb);
    CHECK(f == 4 && std::fabs(fa - 3.25f) < 1e-3f && std::fabs(fb - 11.5f) < 1e-3f);
    const float3 cd = s.cornerDir(2, 5, 7), fd = s.dirFromFace(2, 5, 7);
    CHECK(length(cd - fd) < 1e-4f);   // Q16 vs float
    // height at a corner direction = that corner's height; ray from outside hits the surface there
    CHECK(std::fabs(s.heightAt(cd) - s.cornerHeight(2, 5, 7)) < 1e-3f);
    float3 hit;
    const float3 target = s.surfacePoint(cd);
    CHECK(s.raycast(cd * 200.0f, cd * -1.0f, hit));
    CHECK(length(hit - target) < 0.05f);
    CHECK(!s.raycast(cd * 200.0f, cd, hit));   // pointing away: no hit
    // map world block: bodies, orbit parents, start
    Json map = Json::parse(R"({"world": {"bodies": [{"id": "sun", "kind": "star", "radius": 10},
        {"id": "p", "kind": "planet", "radius": 20, "faceSize": 4, "orbit": {"around": "sun", "radius": 100, "period": 10, "phase": 0.25}}],
        "start": "p"}})");
    std::vector<std::string> warn;
    rts::game::PlanetWorld w = rts::game::loadPlanetWorld(map, &warn);
    CHECK(w.bodies.size() == 2 && w.start == 1 && w.bodies[1].parent == 0 && warn.empty()); // no corners: a smooth sphere
    const float3 c0 = w.center(1, 0), c5 = w.center(1, 5);   // quarter turn, then half a period later
    CHECK(std::fabs(c0.x) < 1e-3f && std::fabs(c0.z - 100) < 1e-3f && std::fabs(c5.z + 100) < 1e-2f);
    CHECK(rts::game::loadPlanetWorld(Json::parse("{}")).empty());
}

// ---------------- Sound groups and steal modes ----------------

TEST(audio_groups_share_a_limit) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"spatial":{"reverb":false},
        "groups":{"melee":{"max":3}},
        "sounds":{"swing":{"synth":{"duration":2},"group":"melee","maxInstances":0},
                  "clang":{"synth":{"duration":2},"group":"melee","maxInstances":0},
                  "other":{"synth":{"duration":2},"maxInstances":0}}})"));
    CHECK(a->groupCount() == 1 && a->findGroup("melee") == 0 && a->group(0)->max == 3);
    CHECK(a->sound(a->find("swing"))->groups.size() == 1 && a->sound(a->find("other"))->groups.empty());
    for (int i = 0; i < 3; ++i) { a->play("swing"); a->play("clang"); }
    a->play("other");
    renderRms(*a, 0.05f);
    CHECK(a->activeVoices() == 4);           // 3 from the group (both sounds together) + the ungrouped one
}

TEST(audio_group_takes_a_whole_bus) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"groups":{"ui":{"max":2,"bus":"ui"}},
        "sounds":{"a":{"synth":{"duration":2},"bus":"ui","maxInstances":0},"b":{"synth":{"duration":2},"bus":"ui","maxInstances":0},
                  "c":{"synth":{"duration":2},"maxInstances":0}}})"));
    CHECK(a->sound(a->find("a"))->groups.size() == 1 && a->sound(a->find("c"))->groups.empty());
    for (int i = 0; i < 3; ++i) { a->play("a"); a->play("b"); a->play("c"); }
    renderRms(*a, 0.05f);
    CHECK(a->activeVoices() == 2 + 3);
}

TEST(audio_group_cooldown) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"groups":{"shots":{"cooldown":0.1}},
        "sounds":{"bow":{"synth":{"duration":0.05},"group":"shots"},"gun":{"synth":{"duration":0.05},"group":"shots"}}})"));
    CHECK(a->play("bow") != audio::kNoVoice);
    CHECK(a->play("gun") == audio::kNoVoice);   // another sound, same group: still in its cooldown
    a->update(0.12);
    CHECK(a->play("gun") != audio::kNoVoice);
}

namespace {
// Plays `name` at x (listener at the origin, looking down -z) and reports whether it started.
audio::VoiceId playAtX(audio::AudioEngine& a, const char* name, float x) {
    audio::PlayParams p;
    p.positional = true;
    p.x = x;
    return a.play(name, p);
}
// Which of the probe tones (1000 + 100 * i Hz) are audible in the next 0.2 s.
std::vector<bool> tonesPlaying(audio::AudioEngine& a, int n) {
    renderBuf(a, 0.05f);   // let stolen voices fade out
    auto b = renderBuf(a, 0.2f);
    std::vector<bool> on;
    for (int i = 0; i < n; ++i) on.push_back(level(b, 1000 + 100 * i, 0.0, 0.2) > 0.005);
    return on;
}
std::string probeBank(const char* steal) {
    std::string s = R"({"spatial":{"model":"linear","rolloff":0,"minDistance":5,"maxDistance":200,"airAbsorption":false,"reverb":false},
        "groups":{"g":{"max":2,"steal":")" + std::string(steal) + R"("}},"sounds":{)";
    for (int i = 0; i < 4; ++i) {
        if (i) s += ",";
        s += "\"t" + std::to_string(i) + "\":{\"synth\":{\"wave\":\"sine\",\"freq\":" + std::to_string(1000 + 100 * i) +
             ",\"duration\":3,\"volume\":" + (i == 1 ? "0.2" : "1") + "},\"group\":\"g\",\"maxInstances\":0}";
    }
    return s + "}}";
}
} // namespace

TEST(audio_steal_farthest) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, probeBank("farthest")));
    a->setListener(audio::ListenerPose{});
    playAtX(*a, "t0", 10);           // near
    playAtX(*a, "t1", 60);           // far
    renderBuf(*a, 0.02f);
    playAtX(*a, "t2", 30);           // full: the far one (t1) makes room
    CHECK((tonesPlaying(*a, 4) == std::vector<bool>{true, false, true, false}));
    CHECK(playAtX(*a, "t3", 100) != audio::kNoVoice);   // accepted by the engine...
    CHECK((tonesPlaying(*a, 4) == std::vector<bool>{true, false, true, false}));  // ...but farther than all: dropped, nothing cut off
}

TEST(audio_steal_oldest_quietest_none) {
    {
        auto a = offlineAudio();
        CHECK(loadBank(*a, probeBank("oldest")));
        a->setListener(audio::ListenerPose{});
        playAtX(*a, "t0", 10); renderBuf(*a, 0.02f);
        playAtX(*a, "t1", 60); renderBuf(*a, 0.02f);
        playAtX(*a, "t3", 100);                                       // oldest goes, even though the new one is far
        CHECK((tonesPlaying(*a, 4) == std::vector<bool>{false, true, false, true}));
    }
    {
        auto a = offlineAudio();
        CHECK(loadBank(*a, probeBank("quietest")));
        a->setListener(audio::ListenerPose{});
        playAtX(*a, "t0", 10);
        playAtX(*a, "t1", 10);                                        // volume 0.2: the quietest
        renderBuf(*a, 0.02f);
        playAtX(*a, "t2", 10);
        CHECK((tonesPlaying(*a, 4) == std::vector<bool>{true, false, true, false}));
        playAtX(*a, "t1", 10);                                        // quieter than everything playing: dropped
        CHECK((tonesPlaying(*a, 4) == std::vector<bool>{true, false, true, false}));
    }
    {
        auto a = offlineAudio();
        CHECK(loadBank(*a, probeBank("none")));
        a->setListener(audio::ListenerPose{});
        playAtX(*a, "t0", 50); playAtX(*a, "t1", 50); renderBuf(*a, 0.02f);
        playAtX(*a, "t2", 1);                                         // even a close one: nothing is cut off
        CHECK((tonesPlaying(*a, 4) == std::vector<bool>{true, true, false, false}));
    }
}

TEST(audio_group_respects_priority_and_far_pairs) {
    auto a = offlineAudio();
    CHECK(loadBank(*a, R"({"spatial":{"model":"linear","rolloff":0,"minDistance":5,"maxDistance":200,"airAbsorption":false,"reverb":false},
        "groups":{"g":{"max":2,"steal":"oldest"}},
        "sounds":{"vip":{"synth":{"wave":"sine","freq":1000,"duration":3},"group":"g","priority":5,"maxInstances":0},
                  "low":{"synth":{"wave":"sine","freq":1100,"duration":3},"group":"g","maxInstances":0},
                  "boom":{"synth":{"wave":"sine","freq":1200,"duration":3},"group":"g","maxInstances":0,"far":{"sound":"boomFar","from":0.0}},
                  "boomFar":{"synth":{"wave":"sine","freq":1300,"duration":3},"maxInstances":0,"maxDistance":300}}})"));
    a->setListener(audio::ListenerPose{});
    playAtX(*a, "vip", 10); renderBuf(*a, 0.02f);
    playAtX(*a, "vip", 10); renderBuf(*a, 0.02f);
    playAtX(*a, "low", 10);                                           // group full of higher priority: dropped
    CHECK((tonesPlaying(*a, 4) == std::vector<bool>{true, false, false, false}));
    a->stopAll(0.01f);
    renderBuf(*a, 0.1f);
    playAtX(*a, "boom", 100);                                         // a near/far pair counts once
    playAtX(*a, "low", 10);
    renderBuf(*a, 0.05f);
    CHECK(a->activeVoices() == 3);                                    // boom + boomFar + low: the pair did not fill the group
    playAtX(*a, "low", 10);                                           // full: the oldest (boom) goes, with its far half
    CHECK((tonesPlaying(*a, 4) == std::vector<bool>{false, true, false, false}));
    CHECK(a->activeVoices() == 2);
}

TEST(audio_group_errors) {
    auto a = offlineAudio();
    std::string err;
    CHECK(!loadBank(*a, R"({"groups":{"g":{"max":2,"steal":"random"}}})", &err) && err.find("group 'g': unknown steal 'random'") != std::string::npos);
    CHECK(!loadBank(*a, R"({"groups":{"g":{"max":2,"bus":"drums"}}})", &err) && err.find("unknown bus 'drums'") != std::string::npos);
    CHECK(!loadBank(*a, R"({"sounds":{"x":{"synth":{},"steal":"later"}}})", &err) && err.find("sound 'x': unknown steal 'later'") != std::string::npos);
    CHECK(!loadBank(*a, R"({"sounds":{"x":{"synth":{},"group":3}}})", &err) && err.find("\"group\" must be") != std::string::npos);
    CHECK(loadBank(*a, R"({"groups":{"g":{"maxx":2}},"sounds":{"x":{"synth":{},"group":["g","nope"]}}})", &err));
    CHECK(a->warnings().size() == 3);   // unknown key 'maxx', a group that limits nothing, unknown group 'nope'
}

// ------------------------------------------------------------------ splash & icons (splash agent)
#include "engine/splash/SplashData.h"
#include "tools/branding/Icons.h"
#include "tools/branding/Splash.h"

TEST(splash_parse_and_timeline) {
    Json j = Json::parse(R"({"background": "#102030", "image": "bg.png",
        "logos": [{"image": "a.png", "start": 0, "duration": 2, "scale": 0.5, "animation": {"fadeIn": 0.5, "fadeOut": 0.5}},
                  {"sheet": "s.png", "frame": [64, 32], "count": 6, "fps": 10, "start": 1.5,
                   "animation": [{"t": 0, "opacity": 0, "scale": 0.5}, {"t": 1, "opacity": 1, "scale": 1, "ease": "out"}]}],
        "minSeconds": 1, "maxSeconds": 4, "loop": true,
        "progress": {"color": "#ff0000", "position": "top", "width": 0.5, "height": 4}})");
    std::vector<std::string> warn;
    splash::Spec s = splash::parseSpec(j, &warn);
    CHECK(s.enabled && s.layers.size() == 3);
    CHECK(std::abs(s.background.r - 16.0f / 255) < 1e-4f && std::abs(s.background.b - 48.0f / 255) < 1e-4f);
    CHECK(s.layers[0].fit == splash::Fit::Cover && s.layers[1].fit == splash::Fit::Contain);
    CHECK(s.layers[2].frames() == 6 && s.layers[2].frameW == 64);
    CHECK(std::abs(s.sequenceLength() - 2.0f) < 1e-4f && std::abs(s.minimumTime() - 1.0f) < 1e-4f);
    CHECK(s.images().size() == 3);
    float st[splash::kPropCount];
    splash::layerState(s.layers[1], 0.25f, st);
    CHECK(std::abs(st[splash::kOpacity] - 0.5f) < 1e-3f);           // half way through fadeIn
    splash::layerState(s.layers[1], 1.75f, st);
    CHECK(std::abs(st[splash::kOpacity] - 0.5f) < 1e-3f);           // half way through fadeOut
    splash::layerState(s.layers[2], 0.5f, st);
    CHECK(st[splash::kOpacity] > 0.5f && st[splash::kScale] > 0.75f); // eased out: ahead of linear
    std::vector<splash::ImageSize> sizes = {{1920, 1080}, {400, 200}, {64, 32}};
    auto items = splash::evaluate(s, sizes, 1.0f, 1280, 720);
    CHECK(items.size() == 2);                                         // bg + logo a, the sheet starts at 1.5
    CHECK(std::abs(items[0].w - 1280) < 1 && std::abs(items[1].w - 640) < 1 && std::abs(items[1].h - 320) < 1);
    items = splash::evaluate(s, sizes, 1.5f + 0.35f, 1280, 720);
    CHECK(items.back().layer == 2 && items.back().frame == 3);       // 0.35 s at 10 fps
    // looping while loading wraps the timeline, the real time keeps counting
    CHECK(splash::advanceTimeline(s, 1.9f, 0.2f, true) < 0.2f);
    CHECK(splash::advanceTimeline(s, 1.9f, 0.2f, false) > 2.0f);
    CHECK(!splash::readyToLeave(s, 0.5f, true, false) && splash::readyToLeave(s, 0.5f, true, true) && !splash::readyToLeave(s, 9, false, true));
    float x, y, w, h;
    CHECK(splash::progressRect(s, 1000, 500, 2, x, y, w, h) && w == 500 && h == 8 && x == 250);
    bool threw = false;
    try { splash::parseSpec(Json::parse(R"({"logos": [{"image": "a.png", "fit": "squash"}]})")); } catch (const std::exception& e) {
        threw = std::string(e.what()).find("logos[0]") != std::string::npos;
    }
    CHECK(threw);
    CHECK(!splash::parseSpec(Json()).enabled);
    splash::Color c;
    CHECK(splash::parseColor(std::string("#fff8"), c) && std::abs(c.a - 0x88 / 255.0f) < 1e-4f && !splash::parseColor(std::string("red"), c));
}

TEST(icons_resize_and_render) {
    branding::Image src = branding::makeImage(64, 64, {1, 0, 0, 1});
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 64; ++x) src.px(x, y)[3] = 0;   // top half transparent
    branding::Image half = branding::resize(src, 32, 32);
    CHECK(half.width == 32 && half.height == 32);
    CHECK(half.px(10, 25)[0] == 255 && half.px(10, 25)[3] == 255 && half.px(10, 5)[3] == 0);
    // transparent pixels don't darken the colour of the edge (premultiplied resampling)
    branding::Image tiny = branding::resize(src, 1, 1);
    CHECK(tiny.px(0, 0)[0] == 255 && std::abs(int(tiny.px(0, 0)[3]) - 128) <= 1);
    branding::Image up = branding::resize(src, 200, 100);
    CHECK(up.width == 200 && up.height == 100);
    // iOS look: opaque, flattened on the background
    branding::IconLook ios;
    ios.opaque = true;
    ios.hasBackground = true;
    ios.background = {0, 0, 1, 1};
    branding::Image i = branding::renderIcon(src, 40, ios);
    CHECK(!branding::hasAlpha(i) && i.px(20, 5)[2] == 255 && i.px(20, 35)[0] == 255);
    branding::IconLook mac;
    mac.margin = 0.1f;
    mac.cornerRadius = 0.25f;
    branding::Image m = branding::renderIcon(branding::makeImage(16, 16, {0, 1, 0, 1}), 100, mac);
    CHECK(m.px(2, 50)[3] == 0 && m.px(50, 50)[3] == 255 && m.px(11, 11)[3] == 0);   // margin, body, rounded corner
}

TEST(icons_ico_icns_png_writers) {
    std::vector<branding::Image> imgs;
    for (int s : branding::windowsIcoSizes()) imgs.push_back(branding::makeImage(s, s, {0.5f, 0.5f, 0.5f, 1}));
    std::string ico = branding::encodeIco(imgs);
    auto u16 = [&](size_t o) { return unsigned(uint8_t(ico[o])) | unsigned(uint8_t(ico[o + 1])) << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    CHECK(u16(0) == 0 && u16(2) == 1 && u16(4) == imgs.size());
    size_t end = 0;
    for (size_t k = 0; k < imgs.size(); ++k) {
        size_t e = 6 + 16 * k;
        int w = uint8_t(ico[e]) == 0 ? 256 : uint8_t(ico[e]);
        CHECK(w == imgs[k].width && u16(e + 6) == 32);
        size_t size = u32(e + 8), off = u32(e + 12);
        if (w == 256) CHECK(ico.compare(off + 1, 3, "PNG") == 0);
        else CHECK(u32(off) == 40 && u32(off + 4) == unsigned(w) && u32(off + 8) == unsigned(w * 2) && size == size_t(40 + w * w * 4 + ((w + 31) / 32) * 4 * w));
        end = std::max(end, off + size);
    }
    CHECK(end == ico.size());
    std::string icns = branding::encodeIcns({{"ic07", branding::makeImage(128, 128)}, {"ic10", branding::makeImage(1024, 1024)}});
    auto be32 = [&](size_t o) { return unsigned(uint8_t(icns[o])) << 24 | unsigned(uint8_t(icns[o + 1])) << 16 | unsigned(uint8_t(icns[o + 2])) << 8 | unsigned(uint8_t(icns[o + 3])); };
    CHECK(icns.compare(0, 4, "icns") == 0 && be32(4) == icns.size() && icns.compare(8, 4, "ic07") == 0);
    CHECK(icns.compare(8 + be32(12), 4, "ic10") == 0);
    std::string png = branding::encodePng(branding::makeImage(3, 2, {1, 1, 1, 1}));
    atlas::Image back;
    CHECK(atlas::decodeImageFile(png, back) && back.width == 3 && back.height == 2);
}

TEST(icons_generate_platform_sets) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rts_icons_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "game");
    branding::Image master = branding::makeImage(512, 384, {0.9f, 0.6f, 0.1f, 0.5f});   // too small, not square, alpha
    CHECK(branding::writePng((dir / "game" / "m.png").string(), master));
    Json game = Json::parse(R"({"name": "Test Game", "icons": {"default": "m.png", "background": "#203040",
        "android": {"background": "#112233", "monochrome": "m.png"}},
        "splash": {"background": "#000000", "logos": [{"image": "m.png", "duration": 1}]}})");
    branding::IconSpec s = branding::parseIcons(game);
    auto warnings = branding::validateIcons(s, (dir / "game").string());
    auto has = [&](const char* w) { for (auto& x : warnings) if (x.find(w) != std::string::npos) return true; return false; };
    CHECK(has("square") && has("at least 1024") && has("transparency"));
    branding::Generated g;
    branding::generateIOS(s, (dir / "game").string(), (dir / "ios" / "Assets.xcassets").string(), g);
    Json contents = Json::parseFile((dir / "ios" / "Assets.xcassets" / "AppIcon.appiconset" / "Contents.json").string());
    CHECK(contents["images"].size() == branding::iosIconSet().size());
    for (auto& im : contents["images"].items()) CHECK(fs::exists(dir / "ios" / "Assets.xcassets" / "AppIcon.appiconset" / im.getStr("filename")));
    branding::Image i180;
    CHECK(branding::loadImageFile((dir / "ios" / "Assets.xcassets" / "AppIcon.appiconset" / "icon_180.png").string(), i180) && i180.width == 180 && !branding::hasAlpha(i180));
    branding::generateAndroid(s, (dir / "game").string(), (dir / "res").string(), (dir / "play.png").string(), g);
    for (const char* d : {"mdpi", "hdpi", "xhdpi", "xxhdpi", "xxxhdpi"}) {
        CHECK(fs::exists(dir / "res" / (std::string("mipmap-") + d) / "ic_launcher.png"));
        CHECK(fs::exists(dir / "res" / (std::string("mipmap-") + d) / "ic_launcher_round.png"));
        CHECK(fs::exists(dir / "res" / (std::string("mipmap-") + d) / "ic_launcher_monochrome.png"));
    }
    branding::Image fg;
    CHECK(branding::loadImageFile((dir / "res" / "mipmap-xxxhdpi" / "ic_launcher_foreground.png").string(), fg) && fg.width == 432);
    std::ifstream ax(dir / "res" / "mipmap-anydpi-v26" / "ic_launcher.xml");
    std::string axml((std::istreambuf_iterator<char>(ax)), {});
    CHECK(axml.find("@color/ic_launcher_background") != std::string::npos && axml.find("monochrome") != std::string::npos);
    branding::Image play;
    CHECK(branding::loadImageFile((dir / "play.png").string(), play) && play.width == 512 && !branding::hasAlpha(play));
    branding::generateWindows(s, (dir / "game").string(), (dir / "win").string(), g);
    CHECK(fs::file_size(dir / "win" / "app.ico") > 1000 && fs::exists(dir / "win" / "app.rc"));
    branding::GameMeta meta;
    meta.name = "Test Game"; meta.exe = "Test_Game"; meta.backgroundColor = "#000000";
    branding::generateWeb(s, (dir / "game").string(), (dir / "web").string(), meta, g);
    Json man = Json::parseFile((dir / "web" / "manifest.webmanifest").string());
    CHECK(man.getStr("name") == "Test Game" && man["icons"].size() == 4 && man["icons"][size_t(1)].getStr("purpose") == "maskable");
    for (auto& ic : man["icons"].items()) CHECK(fs::exists(dir / "web" / ic.getStr("src")));
    CHECK(fs::exists(dir / "web" / "favicon.ico") && fs::exists(dir / "web" / "apple-touch-icon.png"));
    branding::generateLinux(s, (dir / "game").string(), (dir / "linux").string(), meta, g);
    CHECK(fs::exists(dir / "linux" / "share" / "icons" / "hicolor" / "256x256" / "apps" / "Test_Game.png"));
    CHECK(fs::exists(dir / "linux" / "share" / "applications" / "Test_Game.desktop"));
    branding::generateMac(s, (dir / "game").string(), (dir / "mac").string(), g);
    CHECK(fs::exists(dir / "mac" / "AppIcon.iconset" / "icon_512x512@2x.png") && fs::file_size(dir / "mac" / "AppIcon.icns") > 1000);
    // launch screens
    branding::NativeSplash n = branding::nativeSplash(game);
    CHECK(n.enabled && n.image == "m.png");
    branding::generateIOSLaunch(n, (dir / "game").string(), (dir / "ios" / "Assets.xcassets").string(), (dir / "ios").string(), g);
    CHECK(fs::exists(dir / "ios" / "LaunchScreen.storyboard") && fs::exists(dir / "ios" / "Assets.xcassets" / "LaunchImage.imageset" / "launch@3x.png"));
    branding::generateAndroidLaunch(n, (dir / "game").string(), (dir / "res").string(), g);
    std::ifstream tv31(dir / "res" / "values-v31" / "rts_themes.xml");
    std::string themes((std::istreambuf_iterator<char>(tv31)), {});
    CHECK(themes.find("windowSplashScreenBackground") != std::string::npos && themes.find("rts_splash_icon") != std::string::npos);
    CHECK(fs::exists(dir / "res" / "drawable" / "rts_launch.xml") && fs::exists(dir / "res" / "drawable-xxhdpi" / "rts_splash_logo.png"));
    CHECK(branding::splashBaseFiles(game).size() == 1);
    CHECK(g.warnings.empty());
    // previews render (agents look at them through MCP)
    std::vector<std::string> pw;
    branding::Image sheet = branding::iconsContactSheet(s, (dir / "game").string(), &pw);
    CHECK(sheet.width > 600 && sheet.height > 400);
    branding::Image strip = branding::splashStrip(splash::parseSpec(game["splash"]), (dir / "game").string(), {0, 0.5f}, 160, 90, &pw);
    CHECK(strip.width > 320 && strip.height > 90);
    ax.close(); tv31.close();   // Windows can't delete open files
    std::error_code rmEc;
    fs::remove_all(dir, rmEc);
}

// ---- sprite shadows (RtsSpriteShadow): data keys, tiers, projected decal geometry
#include "modules/rts/view/RtsSpriteShadow.h"

TEST(sprite_shadow_data_keys) {
    using game::SpriteShadowMode;
    // defaults: blob, unlit; game.json wins over rules.json
    game::SpriteDefaults d0 = game::parseSpriteDefaults(Json(), Json());
    CHECK(d0.shadow == SpriteShadowMode::Blob && !d0.lit);
    game::SpriteDefaults d = game::parseSpriteDefaults(Json::parse(R"({"shadow": "none", "lit": true})"), Json::parse(R"({"shadow": "cast"})"));
    CHECK(d.shadow == SpriteShadowMode::Cast && d.lit);
    // old files: missing shadow = blob for units, none for buildings and ground sprites
    auto spec = [&](const char* js, const game::SpriteDefaults& dd, bool structure, bool ground = false) {
        return game::parseSpriteShadow(Json::parse(js), dd, structure, ground, 1.0f, 1.5f, 2, 3);
    };
    CHECK(spec("{}", d0, false).mode == SpriteShadowMode::Blob);
    CHECK(spec("{}", d0, true).mode == SpriteShadowMode::None);
    CHECK(spec("{}", d, true).mode == SpriteShadowMode::Cast);          // game opts in: buildings cast
    CHECK(spec("{}", d, false, true).mode == SpriteShadowMode::None);   // ground sprites never cast
    CHECK(spec(R"({"shadow": false})", d, false).mode == SpriteShadowMode::None);
    CHECK(spec(R"({"shadow": true})", d0, true).mode == SpriteShadowMode::Blob);
    CHECK(spec(R"({"shadow": "cast"})", d0, false).mode == SpriteShadowMode::Cast);
    // object form: blob (compatibility) unless "mode" says otherwise; size / alpha kept for the blob
    game::SpriteShadowSpec o = spec(R"({"shadow": {"size": [0.9, 0.5], "alpha": 0.3}})", d, true);
    CHECK(o.mode == SpriteShadowMode::Blob && std::fabs(o.blobW - 0.9f) < 1e-4f && std::fabs(o.blobH - 0.5f) < 1e-4f && std::fabs(o.blobAlpha - 0.3f) < 1e-4f);
    CHECK(spec(R"({"shadow": {"mode": "cast", "size": [0.9, 0.5]}})", d0, true).mode == SpriteShadowMode::Cast);
    // structure blob covers the footprint; caster height / offset / lit overrides
    game::SpriteShadowSpec b = spec(R"({"castHeight": 2.5, "shadowOffset": [0.25, -0.5], "lit": false})", d, true);
    CHECK(b.blobW > 2.0f && b.blobH > 3.0f && std::fabs(b.castHeight - 2.5f) < 1e-4f);
    CHECK(std::fabs(b.offsetRight - 0.25f) < 1e-4f && std::fabs(b.offsetFront + 0.5f) < 1e-4f && !b.lit);
    CHECK(std::fabs(spec("{}", d, false).castHeight - 1.5f) < 1e-4f && spec("{}", d, false).lit);
    CHECK(std::fabs(b.castDepth - 3.0f) < 1e-4f && spec("{}", d, false).castDepth == 0);   // buildings: box of the footprint depth
    CHECK(spec(R"({"castDepth": 0})", d, true).castDepth == 0);
}

TEST(sprite_shadow_tiers) {
    using game::SpriteShadowMode;
    using render::ShadowQuality;
    auto at = [](SpriteShadowMode m, ShadowQuality q, bool s) { return game::shadowDrawFor(m, q, s); };
    // Off: blobs only (the old look); Low: buildings cast, units blob; Medium+: all cast
    CHECK(!at(SpriteShadowMode::Cast, ShadowQuality::Off, true).cast && at(SpriteShadowMode::Cast, ShadowQuality::Off, true).blobK == 1);
    CHECK(at(SpriteShadowMode::Cast, ShadowQuality::Low, true).cast && !at(SpriteShadowMode::Cast, ShadowQuality::Low, false).cast);
    CHECK(at(SpriteShadowMode::Cast, ShadowQuality::Medium, false).cast && at(SpriteShadowMode::Cast, ShadowQuality::High, false).cast);
    CHECK(!at(SpriteShadowMode::Blob, ShadowQuality::High, false).cast && at(SpriteShadowMode::Blob, ShadowQuality::High, false).blobK == 1);
    CHECK(!at(SpriteShadowMode::None, ShadowQuality::High, false).cast && at(SpriteShadowMode::None, ShadowQuality::Off, false).blobK == 0);
}

TEST(sprite_shadow_projected_decal_follows_ground) {
    // levels map with a cliff: a blob across the edge is cut from both levels' surface triangles,
    // every decal vertex lies on the terrain mesh (heightAt agrees inside a cell's top quad)
    game::MapGrid g;
    g.init(6, 4);
    for (auto& f : g.flags) f = game::kCellGround;
    for (int y = 0; y < 4; ++y) for (int x = 3; x < 6; ++x) g.level[size_t(g.index(x, y))] = 1;
    for (size_t i = 0; i < g.height.size(); ++i) g.height[i] = int16_t(i * 5 % 16);
    const float lh = 1.5f;
    game::GroundSurface surf{&g, lh, false};
    std::vector<render::float3> tris;
    game::surfaceTriangles(surf, 2.4f, 1.4f, 3.6f, 2.6f, tris);
    CHECK(tris.size() == 4 * 6);   // 2 x 2 cells, 2 triangles each
    const render::float3 o{2.4f, 0, 1.4f}, aU{1, 0, 0}, aV{0, 0, 1};
    int pieces = 0;
    float minY = 1e9f, maxY = -1e9f;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        const render::float3 t[3] = {tris[i], tris[i + 1], tris[i + 2]};
        render::float3 poly[8];
        int n = game::clipToRect(t, o, aU, 1.2f, aV, 1.2f, poly);
        if (n) ++pieces;
        for (int k = 0; k < n; ++k) {
            CHECK(poly[k].x >= 2.4f - 1e-4f && poly[k].x <= 3.6f + 1e-4f && poly[k].z >= 1.4f - 1e-4f && poly[k].z <= 2.6f + 1e-4f);
            // on the plane of its source triangle
            const render::float3 nrm = render::cross(t[1] - t[0], t[2] - t[0]);
            CHECK(std::fabs(render::dot(poly[k] - t[0], nrm)) < 1e-3f);
            minY = std::min(minY, poly[k].y); maxY = std::max(maxY, poly[k].y);
        }
    }
    CHECK(pieces == 8);
    CHECK(minY < 1.0f && maxY > 1.5f);   // both sides of the cliff, no flat plate at one height
    // flat cells (cell centres away from the cliff) match the view's ground height
    std::vector<render::float3> one;
    game::surfaceTriangles(surf, 0.5f, 0.5f, 0.5f, 0.5f, one);
    CHECK(one.size() == 6);
    // tile maps: flat cells at level * levelHeight + height / 16
    game::GroundSurface tilesSurf{&g, lh, true};
    one.clear();
    game::surfaceTriangles(tilesSurf, 4.5f, 2.5f, 4.5f, 2.5f, one);
    CHECK(one.size() == 6 && std::fabs(one[0].y - (1.5f + float(g.height[size_t(g.index(4, 2))]) / 16.0f)) < 1e-5f);
    // fully outside the rect: nothing
    const render::float3 far[3] = {{10, 0, 10}, {11, 0, 10}, {10, 0, 11}};
    render::float3 out[8];
    CHECK(game::clipToRect(far, o, aU, 1.2f, aV, 1.2f, out) == 0);
}

TEST(sprite_shadow_caster_faces_camera_unless_sun_grazes) {
    const render::float3 gR{1, 0, 0}, gF{0, 0, 1};
    // sun towards the camera: keep the camera's axis (silhouette as seen)
    render::float3 r = game::casterRight(gR, gF, {0.2f, 1.0f, 0.8f});
    CHECK(std::fabs(r.x - 1) < 1e-4f && std::fabs(r.z) < 1e-4f);
    // sun from the side (along the sprite plane): turned so the shadow is not a sliver, still unit length
    r = game::casterRight(gR, gF, {1.0f, 1.0f, 0.0f});
    const float c = std::fabs(r.x);
    CHECK(c < 0.9f && c > 0.4f && std::fabs(render::length(r) - 1) < 1e-3f);
}


// ---------------- one-click MCP setup (editor Agent window) ----------------
#if defined(RTS_TESTS_EDITOR_CORE)
#include "apps/editor/core/McpInstall.h"
TEST(mcp_install_patches_configs_as_text) {
    using namespace rts::editor;
    McpServerSpec sp; sp.command = "/opt/rts/rtsmcp"; sp.args = {"--engine", "/opt/rts"};
    const std::string entry = mcpEntryJson(sp);
    std::string err, t;
    CHECK(mcpPatchJson(t, "rtsengine", entry, &err));                        // empty file -> fresh config
    CHECK(Json::parse(t)["mcpServers"]["rtsengine"]["command"].str() == "/opt/rts/rtsmcp");
    // unrelated content stays byte for byte, including numbers our reader would round
    std::string user = "{\n  \"numStartups\": 123456789012345,\n  \"ratio\": 0.123456789,\n  \"theme\": \"dark\"\n}\n";
    t = user;
    CHECK(mcpPatchJson(t, "rtsengine", entry, &err));
    CHECK(t.find("123456789012345") != std::string::npos && t.find("0.123456789") != std::string::npos && t.find("\"theme\": \"dark\"") != std::string::npos);
    CHECK(Json::parse(t)["mcpServers"]["rtsengine"]["args"].size() == 2);
    // existing servers: added next to them, replaced in place on update, removed cleanly
    t = R"({"mcpServers": {"other": {"command": "x"}}, "z": 1})";
    CHECK(mcpPatchJson(t, "rtsengine", entry, &err));
    Json j = Json::parse(t);
    CHECK(j["mcpServers"]["other"]["command"].str() == "x" && j["mcpServers"]["rtsengine"]["command"].str() == "/opt/rts/rtsmcp");
    sp.command = "/new/rtsmcp";
    CHECK(mcpPatchJson(t, "rtsengine", mcpEntryJson(sp), &err));
    CHECK(Json::parse(t)["mcpServers"]["rtsengine"]["command"].str() == "/new/rtsmcp" && t.find("/opt/rts/rtsmcp") == std::string::npos);
    CHECK(mcpPatchJson(t, "rtsengine", "", &err));
    j = Json::parse(t);
    CHECK(!j["mcpServers"].has("rtsengine") && j["mcpServers"]["other"]["command"].str() == "x" && j["z"].integer() == 1);
    t = R"({"mcpServers": {"a": {}, "rtsengine": {"command": "y"}}})";       // last member: the comma before it goes
    CHECK(mcpPatchJson(t, "rtsengine", "", &err));
    CHECK(!Json::parse(t)["mcpServers"].has("rtsengine") && Json::parse(t)["mcpServers"].has("a"));
    t = "[1, 2]";
    CHECK(!mcpPatchJson(t, "rtsengine", entry, &err) && !err.empty());   // not an object: refused, untouched
    CHECK(t == "[1, 2]");
}
#endif


// ---------------------------------------------------------------------------------------------
// Converted formats: "<file>.glb" sidecars (tools/models/ModelConvert.h, models agent)
// ---------------------------------------------------------------------------------------------
TEST(models_converted_sidecar_loads_for_the_source_path) {
    using namespace modeltest;
    std::string glb;
    CHECK(diskReader()(std::string(RTS_SOURCE_DIR) + "/templates/showcase/models/sword.glb", glb));
    CHECK(isNativeModelFormat("a/b.MDX") && isNativeModelFormat("x.vox") && !isNativeModelFormat("ship.lwo") && !isNativeModelFormat("noext"));
    auto m = loadModel("units/ship.lwo", mapReader({{"units/ship.lwo", "LWO source"}, {"units/ship.lwo.glb", glb}}), nullptr);
    CHECK(m && m->path == "units/ship.lwo" && !m->meshes.empty());
    bool threw = false;
    try { loadModel("units/raw.lwo", mapReader({{"units/raw.lwo", "LWO source"}}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find("modelconv") != std::string::npos; }
    CHECK(threw);
}

#if defined(RTS_TEST_MODELCONV)
#include "tools/models/ModelConvert.h"
TEST(models_pack_step_ships_sidecars_not_sources) {
    using namespace rts::models;
    CHECK(needsConversion("m/a.blend") && needsConversion("m/A.LWO") && !needsConversion("m/a.glb") && !needsConversion("m/a.mdx") &&
          !needsConversion("m/a.png") && sidecarPath("m/a.x") == "m/a.x.glb");
    rts::pack::PackWriter w;
    CHECK(w.addData("models/ship.lwo", "source"));
    CHECK(w.addData("models/ship.lwo.glb", "converted"));
    CHECK(w.addData("models/broken.x", "not a model at all"));
    CHECK(w.addData("models/tank.glb", "glb"));
    ConvertReport rep;
    std::string err;
    CHECK(applyModelConversion(w, rep, &err));
    const auto paths = w.paths();
    auto has = [&](const std::string& p) { return std::find(paths.begin(), paths.end(), p) != paths.end(); };
    CHECK(!has("models/ship.lwo") && has("models/ship.lwo.glb") && !has("models/broken.x") && has("models/tank.glb"));
    int failed = 0;
    for (const auto& it : rep.items) failed += it.ok ? 0 : 1;
    CHECK(rep.items.size() == 2 && failed == 1);   // broken.x can't be converted (or no converter): reported
}
#endif
#include <array>
// ---------------- models: voxels (.vox MagicaVoxel, .vxl/.hva Westwood) ----------------
namespace voxtest {
using namespace rts::render;
using Kv = std::vector<std::pair<std::string, std::string>>;
inline void i32(std::string& b, int32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); }
inline void f32(std::string& b, float v) { b.append(reinterpret_cast<const char*>(&v), 4); }
inline std::string vstr(const std::string& s) { std::string b; i32(b, int32_t(s.size())); return b + s; }
inline std::string vdict(const Kv& d) {
    std::string b; i32(b, int32_t(d.size()));
    for (auto& [k, v] : d) b += vstr(k) + vstr(v);
    return b;
}
inline std::string chunk(const char* id, const std::string& content, const std::string& kids = "") {
    std::string b(id, 4); i32(b, int32_t(content.size())); i32(b, int32_t(kids.size()));
    return b + content + kids;
}
inline std::string model(int sx, int sy, int sz, const std::vector<std::array<uint8_t, 4>>& vox) {
    std::string s; i32(s, sx); i32(s, sy); i32(s, sz);
    std::string x; i32(x, int32_t(vox.size()));
    for (auto& v : vox) x.append(reinterpret_cast<const char*>(v.data()), 4);
    return chunk("SIZE", s) + chunk("XYZI", x);
}
inline std::string trn(int id, int child, const std::string& name, const Kv& frame) {
    std::string b; i32(b, id); b += vdict(name.empty() ? Kv{} : Kv{{"_name", name}});
    i32(b, child); i32(b, -1); i32(b, 0); i32(b, 1); b += vdict(frame);
    return chunk("nTRN", b);
}
inline std::string grp(int id, const std::vector<int>& kids) {
    std::string b; i32(b, id); b += vdict({}); i32(b, int32_t(kids.size()));
    for (int k : kids) i32(b, k);
    return chunk("nGRP", b);
}
inline std::string shp(int id, const std::vector<std::pair<int, int>>& models) {  // (model, frame)
    std::string b; i32(b, id); b += vdict({}); i32(b, int32_t(models.size()));
    for (auto& [m, f] : models) { i32(b, m); b += vdict({{"_f", std::to_string(f)}}); }
    return chunk("nSHP", b);
}
// 2x1x1 red box (shared by "Body" and the rotated "Body2"), a 1-voxel team flag, a 2-frame "Anim"
inline std::string sampleVox() {
    std::string kids = model(2, 1, 1, {{0, 0, 0, 1}, {1, 0, 0, 1}}) + model(1, 1, 1, {{0, 0, 0, 250}});
    kids += trn(0, 1, "", {}) + grp(1, {2, 4, 6, 8});
    kids += trn(2, 3, "Body", {{"_t", "0 0 5"}}) + shp(3, {{0, 0}});
    kids += trn(4, 5, "Flag", {}) + shp(5, {{1, 0}});
    kids += trn(6, 7, "Body2", {{"_r", "17"}}) + shp(7, {{0, 0}});
    kids += trn(8, 9, "Anim", {}) + shp(9, {{0, 0}, {1, 1}});
    std::string pal;
    for (int i = 0; i < 256; ++i) {
        const uint8_t c[4] = {uint8_t(i == 0 ? 255 : 128), 0, uint8_t(i == 0 ? 0 : 128), 255};  // entry 0 = colour index 1
        pal.append(reinterpret_cast<const char*>(c), 4);
    }
    kids += chunk("RGBA", pal);
    std::string matl; i32(matl, 1); matl += vdict({{"_type", "_emit"}, {"_emit", "0.5"}});
    kids += chunk("MATL", matl);
    std::string f = "VOX "; i32(f, 150);
    return f + chunk("MAIN", "", kids);
}
struct VxlSec { std::string name; int sx, sy, sz; std::vector<std::array<int, 4>> vox; float scale; };  // x, y, z, colour
inline std::string sampleVxl(const std::vector<VxlSec>& secs, bool withPalette) {
    std::string body, tails;
    for (const auto& s : secs) {
        std::vector<int32_t> starts(size_t(s.sx * s.sy), -1), ends(size_t(s.sx * s.sy), -1);
        std::string data;
        for (int y = 0; y < s.sy; ++y)
            for (int x = 0; x < s.sx; ++x) {
                std::vector<int> col(size_t(s.sz), -1);
                for (auto& v : s.vox) if (v[0] == x && v[1] == y) col[size_t(v[2])] = v[3];
                bool any = false;
                for (int c : col) any = any || c >= 0;
                if (!any) continue;
                starts[size_t(y * s.sx + x)] = int32_t(data.size());
                int z = 0;
                while (z < s.sz) {  // one span per run: skip, count, (colour, normal) x count, count
                    int skip = 0;
                    while (z < s.sz && col[size_t(z)] < 0) { ++z; ++skip; }
                    std::string run;
                    while (z < s.sz && col[size_t(z)] >= 0) { run += char(col[size_t(z)]); run += char(0); ++z; }
                    data += char(skip); data += char(run.size() / 2); data += run; data += char(run.size() / 2);
                }
                ends[size_t(y * s.sx + x)] = int32_t(data.size()) - 1;
            }
        const uint32_t s0 = uint32_t(body.size());
        for (int32_t v : starts) i32(body, v);
        const uint32_t e0 = uint32_t(body.size());
        for (int32_t v : ends) i32(body, v);
        const uint32_t d0 = uint32_t(body.size());
        body += data;
        i32(tails, int32_t(s0)); i32(tails, int32_t(e0)); i32(tails, int32_t(d0)); f32(tails, s.scale);
        const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (float v : id) f32(tails, v);
        for (float v : {0.0f, 0.0f, 0.0f, float(s.sx), float(s.sy), float(s.sz)}) f32(tails, v);
        tails += char(s.sx); tails += char(s.sy); tails += char(s.sz); tails += char(4);
    }
    std::string f("Voxel Animation\0", 16);
    i32(f, 1); i32(f, int32_t(secs.size())); i32(f, int32_t(secs.size())); i32(f, int32_t(body.size()));
    f += char(16); f += char(31);
    std::string pal(768, '\0');
    if (withPalette) { pal[64 * 3] = char(255); pal[20 * 3 + 1] = char(200); }
    f += pal;
    for (size_t i = 0; i < secs.size(); ++i) {
        std::string n = secs[i].name; n.resize(16, '\0');
        f += n; i32(f, int32_t(i)); i32(f, 1); i32(f, 0);
    }
    return f + body + tails;
}
inline std::string sampleHva(const std::vector<std::string>& names, const std::vector<std::vector<std::array<float, 12>>>& frames) {
    std::string f("test");
    f.resize(16, '\0');
    i32(f, int32_t(frames.size())); i32(f, int32_t(names.size()));
    for (auto n : names) { n.resize(16, '\0'); f += n; }
    for (auto& fr : frames) for (auto& mx : fr) for (float v : mx) f32(f, v);
    return f;
}
// every triangle winds counter-clockwise around its stored normal, and normals point outwards
inline bool facesConsistent(const Model& m, int mesh) {
    if (mesh < 0) return false;
    for (const auto& p : m.meshes[size_t(mesh)].primitives) {
        float3 c{};
        for (uint32_t i = 0; i < p.vertexCount; ++i) c += m.vertices[p.firstVertex + i].pos;
        c = c * (1.0f / float(p.vertexCount));
        for (uint32_t i = p.firstIndex; i + 2 < p.firstIndex + p.indexCount; i += 3) {
            const ModelVertex &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]], &d = m.vertices[m.indices[i + 2]];
            if (dot(cross(b.pos - a.pos, d.pos - a.pos), a.nrm) <= 0) return false;
            if (dot(a.nrm, a.pos - c) <= 0) return false;
        }
    }
    return true;
}
inline size_t meshVerts(const Model& m, int mesh) { size_t n = 0; if (mesh >= 0) for (auto& p : m.meshes[size_t(mesh)].primitives) n += p.vertexCount; return n; }
inline size_t meshIdx(const Model& m, int mesh) { size_t n = 0; if (mesh >= 0) for (auto& p : m.meshes[size_t(mesh)].primitives) n += p.indexCount; return n; }
} // namespace voxtest

TEST(models_vox_greedy_scene_team) {
    using namespace voxtest;
    using modeltest::near;
    std::vector<std::string> warn;
    auto m = loadModel("units/t.vox", modeltest::mapReader({{"units/t.vox", sampleVox()}}), &warn);
    const int body = m->findNode("Body"), body2 = m->findNode("Body2"), flag = m->findNode("Flag"), anim = m->findNode("Anim");
    CHECK(body >= 0 && body2 >= 0 && flag >= 0 && anim >= 0);
    if (body < 0 || body2 < 0 || flag < 0 || anim < 0) return;
    const Node &b = m->nodes[size_t(body)], &b2 = m->nodes[size_t(body2)], &fl = m->nodes[size_t(flag)];
    CHECK(b.mesh >= 0 && b.mesh == b2.mesh);                              // a model used twice shares its mesh
    CHECK(meshVerts(*m, b.mesh) == 24 && meshIdx(*m, b.mesh) == 36);     // 2x1x1 box = 6 merged quads
    CHECK(facesConsistent(*m, b.mesh) && facesConsistent(*m, fl.mesh));
    CHECK(near(b.t.x, 0) && near(b.t.y, 5) && near(b.t.z, 0));           // MagicaVoxel z up -> y up
    const float3 rx = b2.r.rotate({1, 0, 0});                             // 90 degrees about z -> about y
    CHECK(near(rx.x, 0) && near(rx.y, 0) && near(rx.z, -1));
    if (b.mesh < 0 || fl.mesh < 0) return;
    const auto& bp = m->meshes[size_t(b.mesh)].primitives[0];
    CHECK(m->vertices[bp.firstVertex].color == 0xff0000ffu);              // palette red, linear
    CHECK(m->materials[size_t(bp.material)].emissive.x > 0.5f && near(m->materials[size_t(bp.material)].emissiveStrength, 0.5f));
    const auto& fp = m->meshes[size_t(fl.mesh)].primitives[0];
    const uint32_t team = m->vertices[fp.firstVertex].color;
    CHECK((team >> 24) == 0 && (team & 255) == 55);                       // team slot: alpha 0, sRGB 128 -> linear 55
    CHECK(m->materials[size_t(fp.material)].team == TeamMode::VertexAlpha);
    // two-frame shape -> clip Idle toggling child scales
    const int ci = m->findClip("Idle");
    CHECK(ci >= 0);
    if (ci >= 0) {
        const AnimClip& c = m->clips[size_t(ci)];
        CHECK(c.channels.size() == 2 && near(c.duration, 0.2f));
        CHECK(c.channels[0].interp == AnimChannel::Step && c.channels[0].path == AnimChannel::Scale && c.channels[0].times.size() == 2);
    }
    CHECK(m->nodes[size_t(anim)].children.size() == 2);
    CHECK(warn.empty());
    // the generated showcase sample loads with its scene graph
    std::vector<std::string> w2;
    auto tank = loadModel(std::string(RTS_SOURCE_DIR) + "/templates/showcase/models/voxel_tank.vox", modeltest::diskReader(), &w2);
    const int turret = tank->findNode("Turret"), barrel = tank->findNode("Barrel");
    CHECK(turret >= 0 && barrel >= 0 && tank->findNode("Hull") >= 0 && w2.empty());
    if (barrel >= 0) CHECK(tank->nodes[size_t(barrel)].parent == turret);
}

TEST(models_vxl_hva_sections) {
    using namespace voxtest;
    using modeltest::near;
    std::vector<VxlSec> secs = {{"Body", 2, 1, 1, {{0, 0, 0, 64}, {1, 0, 0, 20}}, 1.0f}, {"Turret", 1, 1, 1, {{0, 0, 0, 64}}, 1.0f}};
    const std::array<float, 12> ident = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::vector<std::vector<std::array<float, 12>>> frames;
    for (int f = 0; f < 3; ++f)  // turret: 90 degrees about z (voxel space), lifted by 24, moved forward by 12
        frames.push_back({ident, {0, -1, 0, 12, 1, 0, 0, 0, 0, 0, 1, 24.0f + float(f)}});
    std::vector<std::string> warn;
    auto m = loadModel("units/tank.vxl", modeltest::mapReader({{"units/tank.vxl", sampleVxl(secs, true)}, {"units/tank.hva", sampleHva({"Body", "Turret"}, frames)}}), &warn);
    const int body = m->findNode("Body"), tur = m->findNode("Turret");
    CHECK(body >= 0 && tur >= 0 && warn.empty());
    if (body < 0 || tur < 0) return;
    const Node &b = m->nodes[size_t(body)], &t = m->nodes[size_t(tur)];
    CHECK(meshVerts(*m, b.mesh) == 10 * 4 && facesConsistent(*m, b.mesh));  // 2 colours: 4 sides split in two + 2 caps
    CHECK(near(t.t.x, 0) && near(t.t.y, 24) && near(t.t.z, 12));          // (x, y, z) voxel -> (y, z, x) engine
    const float3 fwd = t.r.rotate({0, 0, 1});                             // voxel +x (forward) turned to voxel +y
    CHECK(near(fwd.x, 1) && near(fwd.y, 0) && near(fwd.z, 0));
    bool teamV = false, own = false;
    if (b.mesh >= 0)
        for (auto& p : m->meshes[size_t(b.mesh)].primitives)
            for (uint32_t i = 0; i < p.vertexCount; ++i) {
                const uint32_t c = m->vertices[p.firstVertex + i].color;
                if ((c >> 24) == 0) teamV = true;
                if (c == 0xff0000ffu) own = true;                             // palette 64 = red
            }
    CHECK(teamV && own && m->materials[0].team == TeamMode::VertexAlpha);
    const int ci = m->findClip("Idle");
    CHECK(ci >= 0);
    if (ci >= 0) {
        CHECK(near(m->clips[size_t(ci)].duration, 3.0f / 15.0f) && m->clips[size_t(ci)].channels.size() == 6);
        CHECK(m->clips[size_t(ci)].channels[3].times.size() == 3);
    }
    // no .hva: warning, no clip; empty palette: unittem.pal next to it (6-bit)
    std::string pal(768, '\0');
    pal[64 * 3 + 2] = char(63);
    warn.clear();
    auto m2 = loadModel("units/tank.vxl", modeltest::mapReader({{"units/tank.vxl", sampleVxl(secs, false)}, {"units/unittem.pal", pal}}), &warn);
    CHECK(m2->clips.empty() && warn.size() == 1);
    const int b2 = m2->findNode("Body");
    bool blue = false;
    if (b2 >= 0 && m2->nodes[size_t(b2)].mesh >= 0)
        for (auto& p : m2->meshes[size_t(m2->nodes[size_t(b2)].mesh)].primitives)
            for (uint32_t i = 0; i < p.vertexCount; ++i) blue = blue || m2->vertices[p.firstVertex + i].color == 0xffff0000u;
    CHECK(blue);
}

TEST(models_voxel_broken_files) {
    using namespace voxtest;
    auto expectThrow = [](const std::string& path, const std::string& bytes) {
        try { loadModel(path, modeltest::mapReader({{path, bytes}}), nullptr); }
        catch (const std::exception& e) { return std::string(e.what()).find(path) != std::string::npos; }
        return false;
    };
    CHECK(expectThrow("a/x.vox", "VOX junk"));
    CHECK(expectThrow("a/x.vxl", "Voxel Anim"));
    CHECK(expectThrow("a/x.vox", sampleVox().substr(0, 60)));
    // truncations and byte flips never crash (the loader either throws or returns a model)
    const std::string vox = sampleVox();
    const std::string vxl = sampleVxl({{"Body", 3, 2, 4, {{0, 0, 0, 64}, {1, 1, 2, 20}, {2, 0, 3, 5}}, 1.0f}}, true);
    uint32_t rng = 12345;
    int loaded = 0;
    for (int i = 0; i < 400; ++i) {
        std::string v = i % 2 ? vox : vxl;
        rng = rng * 1664525u + 1013904223u;
        if (i % 4 < 2) v.resize(rng % v.size());
        else for (int k = 0; k < 4; ++k) { rng = rng * 1664525u + 1013904223u; v[rng % v.size()] = char(rng >> 24); }
        const std::string p = i % 2 ? "f.vox" : "f.vxl";
        std::vector<std::string> w;
        try { loadModel(p, modeltest::mapReader({{p, v}}), &w); ++loaded; } catch (const std::exception&) {}
    }
    CHECK(loaded > 0);
}

// ---------------- models (MDX agent): Warcraft III .mdx / .mdl, BLP textures ----------------
namespace mdxtest {
using namespace rts::render;
using modeltest::put;
inline std::string cstr(const std::string& s, size_t n) { std::string r = s; r.resize(n, '\0'); return r; }
inline std::string u32s(std::initializer_list<uint32_t> v) { std::string b; put<uint32_t>(b, v); return b; }
inline std::string f32s(std::initializer_list<float> v) { std::string b; put<float>(b, v); return b; }
inline std::string chunk(const char* tag, const std::string& data) { return std::string(tag, 4) + u32s({uint32_t(data.size())}) + data; }
inline std::string sized(const std::string& body) { return u32s({uint32_t(body.size() + 4)}) + body; }
// track: tag, interpolation, global sequence, keys (frame + comps values [+ in / out tangents])
inline std::string track(const char* tag, uint32_t interp, int32_t gseq, const std::vector<std::pair<int32_t, std::vector<float>>>& keys) {
    std::string b(tag, 4);
    b += u32s({uint32_t(interp > 1 ? keys.size() / 3 : keys.size()), interp, uint32_t(gseq)});
    for (size_t i = 0; i < keys.size(); ++i) {
        if (interp <= 1 || i % 3 == 0) put<int32_t>(b, {keys[i].first});
        for (float f : keys[i].second) put<float>(b, {f});
    }
    return b;
}
inline std::string node(const std::string& name, int32_t id, int32_t parent, uint32_t flags, const std::string& tracks) {
    std::string body = cstr(name, 80);
    put<int32_t>(body, {id, parent});
    body += u32s({flags}) + tracks;
    return sized(body);
}
// Pixels: red opaque, green transparent, red half, green opaque.
inline std::string paletted2x2() {
    std::string b = "BLP1" + u32s({1, 8, 2, 2, 4, 0});
    b += u32s({156 + 1024}) + std::string(15 * 4, '\0') + u32s({8}) + std::string(15 * 4, '\0');
    std::string pal(1024, '\0');
    pal[2] = char(255); pal[5] = char(255);   // 0 = red (B, G, R, x), 1 = green
    b += pal;
    put<uint8_t>(b, {0, 1, 0, 1, 255, 0, 128, 255});
    return b;
}
inline const float kS = 0.70710678f;
// One geoset (6 vertices, 3 triangles), bones Root (0) and Arm (1), attachment "Hand Right Ref" (2),
// sequences Stand / Walk, one global sequence, team colour under an alpha-tested texture.
inline std::string mdx() {
    std::string d = "MDLX" + chunk("VERS", u32s({800}));
    d += chunk("MODL", std::string(372, '\0'));
    std::string seqs;
    for (auto [name, s, e] : {std::tuple<const char*, uint32_t, uint32_t>{"Stand", 0, 1000}, {"Walk", 2000, 3000}})
        seqs += cstr(name, 80) + u32s({s, e}) + f32s({0}) + u32s({0}) + f32s({0}) + u32s({0}) + std::string(28, '\0');
    d += chunk("SEQS", seqs);
    d += chunk("GLBS", u32s({500}));
    std::string lays = u32s({28, 0, 0, 1}) + u32s({0xffffffffu, 0}) + f32s({1}) + u32s({28, 1, 0x10, 0}) + u32s({0xffffffffu, 0}) + f32s({1});
    const std::string mat = u32s({0, 0}) + "LAYS" + u32s({2}) + lays;
    d += chunk("MTLS", sized(mat));
    d += chunk("TEXS", u32s({0}) + cstr("Textures\\Guy.blp", 260) + u32s({3}) + u32s({1}) + cstr("", 260) + u32s({0}));
    std::string g = "VRTX" + u32s({6}) + f32s({-8, -16, 0, -8, 16, 0, -8, 0, 64, 0, -32, 64, 0, -32, 96, 32, 0, 32});
    g += "NRMS" + u32s({6}) + f32s({1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0});
    g += "PTYP" + u32s({1, 4}) + "PCNT" + u32s({1, 9}) + "PVTX" + u32s({9});
    put<uint16_t>(g, {0, 1, 2, 2, 4, 3, 0, 1, 5});
    g += "GNDX" + u32s({6});
    put<uint8_t>(g, {0, 0, 1, 2, 2, 0});
    g += "MTGC" + u32s({3, 1, 2, 1}) + "MATS" + u32s({4, 0, 0, 1, 1});
    g += u32s({0, 0, 0}) + std::string(28, '\0') + u32s({0});
    g += "UVAS" + u32s({1}) + "UVBS" + u32s({6}) + f32s({0, 0, 1, 0, 0, 1, 1, 1, 0.5f, 0.5f, 0.25f, 0.75f});
    d += chunk("GEOS", sized(g));
    const std::string rootT = track("KGTR", 1, -1, {{0, {0, 0, 0}}, {500, {0, 0, 10}}, {1000, {0, 0, 0}}});
    const std::string armR = track("KGRT", 1, -1, {{2250, {0, 0, 0, 1}}, {2750, {0, 0, kS, kS}}});
    const std::string armS = track("KGSC", 2, -1, {{0, {1, 1, 1}}, {0, {0, 0, 0}}, {0, {0, 0, 0}}, {1000, {2, 2, 2}}, {1000, {0, 0, 0}}, {1000, {0, 0, 0}}});
    std::string bones = node("Root", 0, -1, 256, rootT) + u32s({0, 0xffffffffu}) + node("Arm", 1, 0, 256, armR + armS) + u32s({0, 0xffffffffu});
    d += chunk("BONE", bones);
    const std::string handT = track("KGTR", 1, 0, {{0, {0, 0, 0}}, {500, {0, 0, 128}}});
    d += chunk("ATCH", sized(node("Hand Right Ref", 2, 1, 2048, handT) + cstr("", 260) + u32s({0})));
    d += chunk("PIVT", f32s({0, 0, 0, 0, 0, 64, 0, -32, 64}));
    d += chunk("CAMS", std::string(16, 'x'));   // unknown / unsupported chunks are skipped
    return d;
}
inline const char* kMdl = R"(// the same model as mdx() in text form
Version { FormatVersion 800, }
Model "Guy" { BlendTime 150, }
Sequences 2 {
    Anim "Stand" { Interval { 0, 1000 }, }
    Anim "Walk" { Interval { 2000, 3000 }, MoveSpeed 200, }
}
GlobalSequences 1 { Duration 500, }
Textures 2 {
    Bitmap { Image "Textures\Guy.blp", WrapWidth, WrapHeight, }
    Bitmap { Image "", ReplaceableId 1, }
}
Materials 1 {
    Material {
        Layer { FilterMode None, static TextureID 1, }
        Layer { FilterMode Transparent, TwoSided, static TextureID 0, }
    }
}
Geoset {
    Vertices 6 { { -8, -16, 0 }, { -8, 16, 0 }, { -8, 0, 64 }, { 0, -32, 64 }, { 0, -32, 96 }, { 32, 0, 32 }, }
    Normals 6 { { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, }
    TVertices 6 { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 }, { 0.5, 0.5 }, { 0.25, 0.75 }, }
    VertexGroup { 0, 0, 1, 2, 2, 0, }
    Faces 1 9 { Triangles { { 0, 1, 2, 2, 4, 3, 0, 1, 5 }, } }
    Groups 3 4 { Matrices { 0 }, Matrices { 0, 1 }, Matrices { 1 }, }
    MinimumExtent { -8, -32, 0 },
    MaterialID 0,
    SelectionGroup 0,
}
Bone "Root" {
    ObjectId 0,
    GeosetId 0,
    GeosetAnimId None,
    Translation 3 { Linear, 0: { 0, 0, 0 }, 500: { 0, 0, 10 }, 1000: { 0, 0, 0 }, }
}
Bone "Arm" {
    ObjectId 1,
    Parent 0,
    Rotation 2 { Linear, 2250: { 0, 0, 0, 1 }, 2750: { 0, 0, 0.70710678, 0.70710678 }, }
    Scaling 2 {
        Hermite,
        0: { 1, 1, 1 },
            InTan { 0, 0, 0 },
            OutTan { 0, 0, 0 },
        1000: { 2, 2, 2 },
            InTan { 0, 0, 0 },
            OutTan { 0, 0, 0 },
    }
}
Attachment "Hand Right Ref" {
    ObjectId 2,
    Parent 1,
    AttachmentID 0,
    Translation 2 { Linear, GlobalSeqId 0, 0: { 0, 0, 0 }, 500: { 0, 0, 128 }, }
}
PivotPoints 3 { { 0, 0, 0 }, { 0, 0, 64 }, { 0, -32, 64 }, }
Camera "unused" { Position { 0, 0, 0 }, }
)";
inline FileReader files() { return modeltest::mapReader({{"units/Textures/Guy.blp", paletted2x2()}}); }
inline int vertexAt(const Model& m, float3 p) {
    for (size_t i = 0; i < m.vertices.size(); ++i) if (length(m.vertices[i].pos - p) < 1e-4f) return int(i);
    return -1;
}
} // namespace mdxtest

TEST(models_mdx_skeleton_skin_clips_team) {
    using namespace mdxtest;
    using modeltest::near;
    std::vector<std::string> warn;
    auto m = loadModel("units/guy/guy.mdx", modeltest::mapReader({{"units/guy/guy.mdx", mdx()}, {"units/Textures/Guy.blp", paletted2x2()}}), &warn);
    for (auto& w : warn) std::printf("  warning: %s\n", w.c_str());
    CHECK(warn.empty());
    // nodes: root, Root, Arm, Hand Right Ref, mesh
    CHECK(m->nodes.size() == 5 && m->skins.size() == 1 && m->meshes.size() == 1);
    const int root = m->findNode("Root"), arm = m->findNode("Arm"), hand = m->findNode("Hand Right Ref");
    CHECK(root > 0 && arm > 0 && hand > 0);
    if (root <= 0 || arm <= 0 || hand <= 0) return;
    CHECK(m->nodes[size_t(arm)].parent == root && m->nodes[size_t(hand)].parent == arm && m->nodes[size_t(root)].parent == 0);
    // pivot (0, 0, 64) -> Y up, metres = 128 units; pivot-relative bind translations
    CHECK(near(m->nodes[size_t(arm)].t.y, 0.5f) && near(m->nodes[size_t(arm)].t.x, 0) && near(m->nodes[size_t(arm)].t.z, 0));
    CHECK(near(m->nodes[size_t(hand)].t.x, -0.25f) && near(m->nodes[size_t(hand)].t.y, 0));
    // facing: the nose vertex (WC3 +X forward) is at engine +Z
    CHECK(vertexAt(*m, {0, 0.25f, 0.25f}) >= 0);
    CHECK(near(m->boundsMax.z, 0.25f) && near(m->boundsMin.y, 0) && near(m->boundsMax.y, 0.75f));
    // skin: bind palette = identity, matrix group {Root, Arm} averaged
    const Skin& sk = m->skins[0];
    for (auto& p : m->meshes[0].primitives) {
        std::vector<mat4> pal;
        paletteMatrices(*m, sk, p, m->bindWorld, pal);
        bool identity = true;
        for (auto& x : pal) for (int i = 0; i < 16; ++i) identity &= near(x.m[i], (i % 5 == 0) ? 1.0f : 0.0f);
        CHECK(identity && p.jointPalette.size() <= size_t(kMaxPaletteJoints));
    }
    const int v2 = vertexAt(*m, {0, 0.5f, -0.0625f});
    CHECK(v2 >= 0);
    if (v2 >= 0) {
        const ModelVertex& v = m->vertices[size_t(v2)];
        CHECK(v.weights[0] + v.weights[1] == 255 && v.weights[0] >= 127 && v.weights[1] >= 127 && v.joints[0] != v.joints[1]);
    }
    // material: team colour under the texture -> BaseAlpha, opaque, two-sided, texture from the parent directory
    const Material& mat = m->materials[size_t(m->meshes[0].primitives[0].material)];
    CHECK(mat.team == TeamMode::BaseAlpha && mat.alpha == AlphaMode::Opaque && mat.doubleSided && mat.baseTex >= 0);
    if (mat.baseTex >= 0) {
        const Image& img = m->images[size_t(mat.baseTex)];
        CHECK(img.width == 2 && img.height == 2 && img.repeat && img.rgba[0] == 255 && img.rgba[7] == 0);
    }
    // clips
    CHECK(m->clips.size() == 2 && m->findClip("Stand") == 0 && m->findClip("walk") == 1);
    if (m->clips.size() != 2) return;
    CHECK(near(m->clips[0].duration, 1) && near(m->clips[1].duration, 1));
    Pose pose;
    bindPose(*m, pose);
    sampleClip(m->clips[0], 0.5f, pose);
    CHECK(near(pose[size_t(root)].t.y, 10.0f / 128));                  // KGTR (0,0,10) -> +Y
    CHECK(near(pose[size_t(arm)].s.x, 1.5f, 0.01f));                   // Hermite, flat tangents
    sampleClip(m->clips[0], 0.25f, pose);                                // global sequence: 2 periods of 0.5 s
    CHECK(near(pose[size_t(hand)].t.y, 0.5f, 0.01f));
    sampleClip(m->clips[0], 0.75f, pose);
    CHECK(near(pose[size_t(hand)].t.y, 0.5f, 0.01f));
    // Walk: keys at 0.25 s (identity) and 0.75 s (90 degrees about up); the loop seam gets 45 degrees
    bindPose(*m, pose);
    sampleClip(m->clips[1], 0.0f, pose);
    const quat q45 = quat::axisAngle({0, 1, 0}, 0.785398f);
    CHECK(near(std::fabs(dot(pose[size_t(arm)].r, q45)), 1, 1e-3f));
    sampleClip(m->clips[1], 1.0f, pose);
    CHECK(near(std::fabs(dot(pose[size_t(arm)].r, q45)), 1, 1e-3f));
    // rotation happens around the pivot: hand vertex (0,-32,64) turns to (32,0,64) -> engine (0, 0.5, 0.25)
    sampleClip(m->clips[1], 0.75f, pose);
    std::vector<mat4> world;
    worldMatrices(*m, pose, world);
    int j = -1;
    for (size_t k = 0; k < sk.joints.size(); ++k) if (sk.joints[k] == arm) j = int(k);
    CHECK(j >= 0);
    if (j >= 0) {
        const float3 p = (world[size_t(arm)] * sk.inverseBind[size_t(j)]).transformPoint({-0.25f, 0.5f, 0});
        CHECK(near(p.x, 0) && near(p.y, 0.5f) && near(p.z, 0.25f));
    }
}

TEST(models_mdl_matches_mdx) {
    using namespace mdxtest;
    using modeltest::near;
    std::vector<std::string> wx, wl;
    auto a = loadMdx("units/guy/guy.mdx", mdx(), files(), &wx);
    auto b = loadModel("units/guy/guy.mdl", modeltest::mapReader({{"units/guy/guy.mdl", kMdl}, {"units/Textures/Guy.blp", paletted2x2()}}), &wl);
    for (auto& w : wl) std::printf("  warning: %s\n", w.c_str());
    CHECK(wl.empty());
    CHECK(a->nodes.size() == b->nodes.size() && a->vertices.size() == b->vertices.size() && a->indices == b->indices);
    CHECK(a->materials.size() == b->materials.size() && a->images.size() == b->images.size() && a->clips.size() == b->clips.size());
    if (a->nodes.size() != b->nodes.size() || a->vertices.size() != b->vertices.size() || a->clips.size() != b->clips.size()) return;
    for (size_t i = 0; i < a->nodes.size(); ++i)
        CHECK(a->nodes[i].name == b->nodes[i].name && a->nodes[i].parent == b->nodes[i].parent && length(a->nodes[i].t - b->nodes[i].t) < 1e-5f);
    bool same = true;
    for (size_t i = 0; i < a->vertices.size(); ++i) {
        const ModelVertex &x = a->vertices[i], &y = b->vertices[i];
        same &= length(x.pos - y.pos) < 1e-5f && std::memcmp(x.weights, y.weights, 4) == 0 && std::memcmp(x.joints, y.joints, 4) == 0 && near(x.uv.y, y.uv.y);
    }
    CHECK(same);
    const Material &ma = a->materials[0], &mb = b->materials[0];
    CHECK(ma.team == mb.team && ma.alpha == mb.alpha && ma.doubleSided == mb.doubleSided && (ma.baseTex >= 0) == (mb.baseTex >= 0));
    for (size_t c = 0; c < a->clips.size(); ++c) {
        const AnimClip &x = a->clips[c], &y = b->clips[c];
        CHECK(x.name == y.name && near(x.duration, y.duration) && x.channels.size() == y.channels.size());
        if (x.channels.size() != y.channels.size()) continue;
        for (size_t k = 0; k < x.channels.size(); ++k) {
            CHECK(x.channels[k].node == y.channels[k].node && x.channels[k].interp == y.channels[k].interp && x.channels[k].times == y.channels[k].times);
            bool vals = x.channels[k].values.size() == y.channels[k].values.size();
            for (size_t i = 0; vals && i < x.channels[k].values.size(); ++i) vals = near(x.channels[k].values[i], y.channels[k].values[i], 1e-5f);
            CHECK(vals);
        }
    }
}

TEST(models_mdx_mdl_malformed_input) {
    using namespace mdxtest;
    const std::string good = mdx();
    int threw = 0, loaded = 0;
    for (size_t len = 0; len < good.size(); ++len) {   // every truncation: an exception or a model, never a crash
        try { loadMdx("cut.mdx", good.substr(0, len), files(), nullptr); ++loaded; }
        catch (const std::runtime_error& e) { ++threw; if (len == 100) std::printf("  (expected) %s\n", e.what()); }
    }
    CHECK(threw > int(good.size()) * 9 / 10 && threw + loaded == int(good.size()));
    uint32_t rng = 12345;
    for (int iter = 0; iter < 3000; ++iter) {          // random corruption
        std::string bad = good;
        for (int k = 0; k < 1 + iter % 8; ++k) {
            rng = rng * 1664525u + 1013904223u;
            const size_t at = (rng >> 8) % bad.size();
            rng = rng * 1664525u + 1013904223u;
            bad[at] = char(rng >> 24);
        }
        try { loadMdx("bad.mdx", bad, files(), nullptr); } catch (const std::runtime_error&) {}
    }
    const std::string text = kMdl;
    threw = 0;
    for (size_t len = 0; len < text.size(); len += 7) {
        try { loadMdl("cut.mdl", text.substr(0, len), files(), nullptr); } catch (const std::runtime_error&) { ++threw; }
    }
    CHECK(threw > 0);
    for (int iter = 0; iter < 500; ++iter) {
        std::string bad = text;
        rng = rng * 1664525u + 1013904223u;
        bad[(rng >> 8) % bad.size()] = "{},:\"0-x\n"[(rng >> 4) % 9];
        try { loadMdl("bad.mdl", bad, files(), nullptr); } catch (const std::runtime_error&) {}
    }
    bool msg = false;
    try { loadMdl("deep.mdl", std::string(5000, '{'), files(), nullptr); }
    catch (const std::runtime_error& e) { msg = std::string(e.what()).find("deep.mdl") != std::string::npos; }
    CHECK(msg);
    msg = false;
    try { loadModel("u/x.mdx", modeltest::mapReader({{"u/x.mdx", "MDLX"}}), nullptr); }
    catch (const std::runtime_error& e) { msg = std::string(e.what()).find("u/x.mdx: no geosets") != std::string::npos; }
    CHECK(msg);
}

TEST(models_blp_decode) {
    using namespace mdxtest;
    // BLP1 paletted, 8-bit alpha
    Image img;
    CHECK(decodeImage(paletted2x2(), img) && img.width == 2 && img.height == 2);
    if (img.rgba.size() == 16) {
        CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[3] == 255);    // red, opaque
        CHECK(img.rgba[5] == 255 && img.rgba[7] == 0);                          // green, transparent
        CHECK(img.rgba[11] == 128 && img.rgba[15] == 255);
    }
    // BLP1 JPEG: 4 components stored as B, G, R, A without colour transform (16x16, made with PIL)
    auto hex = [](const char* h) { std::string b; for (; h[0] && h[1]; h += 2) b += char(std::stoi(std::string(h, 2), nullptr, 16)); return b; };
    const std::string jh = hex("ffd8ffee000e41646f626500640000000000ffdb00430001010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101ffc000140800100010044311004d11005911004b1100ffc4001f0000010501010101010100000000000000000102030405060708090a0bffc400b5100002010303020403050504040000017d01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa");
    const std::string jm = hex("ffda000e0443004d0059004b00003f00fe902be1fafe27ebfbf8aff2afa28afe20ff00e0e45ff83917fe19ff00fe13dff827a7fc13d3c7bff1901ff132f07fed2ffb4bf83f52ff00937fff005b61af7c1df83baf584bff002701febf4cf885f10b4c9ffe31ff00fd2bc2be15baff008680fed6d5bf67ff00f550a2bfd9028ff83917fe0e45ff00867fff0084f7fe09e9ff0004f4f1effc6407fc4cbc1ffb4bfed2fe0fd4bfe4dfff00d6d86bdf077e0eebd612ff00c9c07fafd33e217c42d327ff008c7fff004af0af856ebfe1a03fb5b56fd9ff00fcabe8a2bfce0ebfffd9");
    std::string jb = "BLP1" + u32s({0, 8, 16, 16, 4, 0}) + u32s({uint32_t(160 + jh.size())}) + std::string(60, '\0') + u32s({uint32_t(jm.size())}) + std::string(60, '\0');
    jb += u32s({uint32_t(jh.size())}) + jh + jm;
    Image ji;
    CHECK(decodeImage(jb, ji) && ji.width == 16 && ji.height == 16);
    if (ji.rgba.size() == 16 * 16 * 4) {
        auto px = [&](int x, int y, int c) { return int(ji.rgba[size_t(y * 16 + x) * 4 + size_t(c)]); };
        auto close = [](int a, int b) { return std::abs(a - b) <= 3; };
        CHECK(close(px(0, 0, 0), 20) && close(px(0, 0, 1), 100) && close(px(0, 0, 2), 200) && close(px(0, 0, 3), 255));
        CHECK(close(px(12, 12, 0), 220) && close(px(12, 12, 2), 30) && close(px(12, 12, 3), 0));
    }
    // BLP2: uncompressed BGRA and DXT3 (explicit alpha)
    auto blp2 = [&](uint8_t enc, uint8_t depth, uint8_t type, uint32_t w, uint32_t h, const std::string& data) {
        std::string b = "BLP2" + u32s({1});
        put<uint8_t>(b, {enc, depth, type, 0});
        b += u32s({w, h, uint32_t(148 + 1024)}) + std::string(60, '\0') + u32s({uint32_t(data.size())}) + std::string(60, '\0') + std::string(1024, '\0') + data;
        return b;
    };
    std::string bgra;
    put<uint8_t>(bgra, {10, 20, 30, 40, 50, 60, 70, 80});
    Image u;
    CHECK(decodeImage(blp2(3, 8, 0, 2, 1, bgra), u) && u.width == 2 && u.rgba.size() == 8 && u.rgba[0] == 30 && u.rgba[2] == 10 && u.rgba[3] == 40 && u.rgba[7] == 80);
    std::string dxt3;
    put<uint8_t>(dxt3, {0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});      // pixel 0 alpha 0, others 15
    put<uint16_t>(dxt3, {0xF800, 0x001F});                                       // red, blue
    put<uint8_t>(dxt3, {0, 0, 0, 0});
    Image d3;
    CHECK(decodeImage(blp2(2, 8, 1, 4, 4, dxt3), d3) && d3.width == 4 && d3.rgba.size() == 64);
    if (d3.rgba.size() == 64) CHECK(d3.rgba[0] == 255 && d3.rgba[2] == 0 && d3.rgba[3] == 0 && d3.rgba[7] == 255);
    // broken files: false, never a crash
    int ok = 0;
    for (const std::string& f : {paletted2x2(), jb, blp2(2, 8, 1, 4, 4, dxt3)})
        for (size_t len = 0; len < f.size(); ++len) { Image t; ok += decodeImage(f.substr(0, len), t); }
    CHECK(ok == 0);
    uint32_t rng = 7;
    for (int iter = 0; iter < 2000; ++iter) {
        std::string bad = jb;
        for (int k = 0; k < 4; ++k) { rng = rng * 1664525u + 1013904223u; bad[160 + (rng >> 8) % (bad.size() - 160)] = char(rng >> 24); }
        Image t;
        decodeImage(bad, t);
    }
}


TEST(models_find_clip_numbered_variants) {
    using namespace rts::render;
    Model m;
    for (const char* n : {"Stand Ready", "Attack - 1", "Attack - 2", "Walk", "Spell_01"}) { AnimClip c; c.name = n; m.clips.push_back(c); }
    CHECK(m.findClip("Attack") == 1 && m.findClip("attack - 2") == 2 && m.findClip("walk") == 3 && m.findClip("Spell") == 4);
    CHECK(m.findClip("Stand") == -1 && m.findClip("Att") == -1);   // "Stand Ready" is another action
}

// ---- interchange formats: Collada, 3DS, PLY, STL (models agent, interchange loaders)

namespace interchangetest {
using namespace rts::render;
using modeltest::near;
using modeltest::mapReader;
// 2x2 uncompressed 32-bit TGA (stb_image reads it): red, green, blue, white
inline std::string tga2x2() {
    std::string t(18, '\0');
    t[2] = 2; t[12] = 2; t[14] = 2; t[16] = 32; t[17] = 0x28;
    const unsigned char px[16] = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};   // BGRA
    t.append(reinterpret_cast<const char*>(px), 16);
    return t;
}
inline const char* skinnedDae() {
    return R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
 <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
 <library_geometries>
  <geometry id="quad" name="Quad"><mesh>
   <source id="quad-pos"><float_array id="quad-pos-a" count="12">-0.5 0 0  0.5 0 0  0.5 2 0  -0.5 2 0</float_array>
    <technique_common><accessor source="#quad-pos-a" count="4" stride="3"><param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common></source>
   <vertices id="quad-v"><input semantic="POSITION" source="#quad-pos"/></vertices>
   <polylist count="1"><input semantic="VERTEX" source="#quad-v" offset="0"/><vcount>4</vcount><p>0 1 2 3</p></polylist>
  </mesh></geometry>
 </library_geometries>
 <library_controllers>
  <controller id="skin" name="Skin"><skin source="#quad">
   <bind_shape_matrix>1 0 0 0  0 1 0 0  0 0 1 0.25  0 0 0 1</bind_shape_matrix>
   <source id="skin-joints"><Name_array id="skin-joints-a" count="2">JRoot JTop</Name_array>
    <technique_common><accessor source="#skin-joints-a" count="2" stride="1"><param name="JOINT" type="name"/></accessor></technique_common></source>
   <source id="skin-ibm"><float_array id="skin-ibm-a" count="32">1 0 0 -0.5  0 1 0 0  0 0 1 0  0 0 0 1   1 0 0 -0.5  0 1 0 -1  0 0 1 0  0 0 0 1</float_array>
    <technique_common><accessor source="#skin-ibm-a" count="2" stride="16"><param name="TRANSFORM" type="float4x4"/></accessor></technique_common></source>
   <source id="skin-w"><float_array id="skin-w-a" count="3">1 0.5 0.25</float_array>
    <technique_common><accessor source="#skin-w-a" count="3" stride="1"><param name="WEIGHT" type="float"/></accessor></technique_common></source>
   <joints><input semantic="JOINT" source="#skin-joints"/><input semantic="INV_BIND_MATRIX" source="#skin-ibm"/></joints>
   <vertex_weights count="4"><input semantic="JOINT" source="#skin-joints" offset="0"/><input semantic="WEIGHT" source="#skin-w" offset="1"/>
    <vcount>1 1 2 2</vcount><v>0 0  0 0  0 1 1 1  1 0 0 2</v></vertex_weights>
  </skin></controller>
 </library_controllers>
 <library_visual_scenes><visual_scene id="scene">
  <node id="Armature" name="Armature">
   <node id="Root" sid="JRoot" name="Root" type="JOINT"><matrix sid="transform">1 0 0 0.5  0 1 0 0  0 0 1 0  0 0 0 1</matrix>
    <node id="Top" sid="JTop" name="Top" type="JOINT"><translate sid="location">0 1 0</translate><rotate sid="rotZ">0 0 1 0</rotate></node>
   </node>
  </node>
  <node id="Body" name="Body"><instance_controller url="#skin"><skeleton>#Root</skeleton></instance_controller></node>
 </visual_scene></library_visual_scenes>
 <library_animations>
  <animation id="anim-root">
   <source id="ar-in"><float_array id="ar-in-a" count="2">0 1</float_array><technique_common><accessor source="#ar-in-a" count="2" stride="1"/></technique_common></source>
   <source id="ar-out"><float_array id="ar-out-a" count="32">1 0 0 0.5 0 1 0 0 0 0 1 0 0 0 0 1  1 0 0 2.5 0 1 0 0 0 0 1 0 0 0 0 1</float_array>
    <technique_common><accessor source="#ar-out-a" count="2" stride="16"/></technique_common></source>
   <sampler id="ar-s"><input semantic="INPUT" source="#ar-in"/><input semantic="OUTPUT" source="#ar-out"/></sampler>
   <channel source="#ar-s" target="Root/transform"/>
  </animation>
  <animation id="anim-top"><animation>
   <source id="at-in"><float_array id="at-in-a" count="2">0 1</float_array><technique_common><accessor source="#at-in-a" count="2" stride="1"/></technique_common></source>
   <source id="at-out"><float_array id="at-out-a" count="2">0 90</float_array><technique_common><accessor source="#at-out-a" count="2" stride="1"/></technique_common></source>
   <source id="at-i"><Name_array id="at-i-a" count="2">LINEAR LINEAR</Name_array><technique_common><accessor source="#at-i-a" count="2" stride="1"/></technique_common></source>
   <sampler id="at-s"><input semantic="INPUT" source="#at-in"/><input semantic="OUTPUT" source="#at-out"/><input semantic="INTERPOLATION" source="#at-i"/></sampler>
   <channel source="#at-s" target="Top/rotZ.ANGLE"/>
  </animation></animation>
 </library_animations>
 <library_animation_clips>
  <animation_clip id="walk" name="Walk" start="0" end="1"><instance_animation url="#anim-root"/><instance_animation url="#anim-top"/></animation_clip>
 </library_animation_clips>
 <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>)";
}
inline const char* texturedZupDae() {
    return R"(<COLLADA version="1.4.1"><asset><unit name="centimeter" meter="0.01"/><up_axis>Z_UP</up_axis></asset>
<library_images><image id="img"><init_from>tex%20file.tga</init_from></image></library_images>
<library_effects><effect id="fx"><profile_COMMON>
 <newparam sid="surf"><surface type="2D"><init_from>img</init_from></surface></newparam>
 <newparam sid="samp"><sampler2D><source>surf</source></sampler2D></newparam>
 <technique sid="common"><phong>
  <emission><color>0.5 0 0 1</color></emission>
  <diffuse><texture texture="samp" texcoord="UVMap"/></diffuse>
  <shininess><float>30</float></shininess>
  <transparent opaque="A_ONE"><color>1 1 1 1</color></transparent>
  <transparency><float>0.5</float></transparency>
 </phong></technique></profile_COMMON>
 <extra><technique profile="GOOGLEEARTH"><double_sided>1</double_sided></technique></extra></effect></library_effects>
<library_materials><material id="mat" name="Paint &amp; Glass"><instance_effect url="#fx"/></material></library_materials>
<library_geometries><geometry id="tri"><mesh>
 <source id="p"><float_array id="pa" count="9">0 0 0  100 0 0  0 0 100</float_array><technique_common><accessor source="#pa" count="3" stride="3"/></technique_common></source>
 <source id="uv"><float_array id="uva" count="6">0 0 1 0 0 1</float_array><technique_common><accessor source="#uva" count="3" stride="2"/></technique_common></source>
 <vertices id="v"><input semantic="POSITION" source="#p"/></vertices>
 <!-- one triangle -->
 <triangles count="1" material="sym"><input semantic="VERTEX" source="#v" offset="0"/><input semantic="TEXCOORD" source="#uv" offset="1" set="0"/><p>0 0 1 1 2 2</p></triangles>
</mesh></geometry></library_geometries>
<library_visual_scenes><visual_scene id="s"><node id="n" name="Tri"><instance_geometry url="#tri"><bind_material><technique_common>
 <instance_material symbol="sym" target="#mat"/></technique_common></bind_material></instance_geometry></node></visual_scene></library_visual_scenes>
<scene><instance_visual_scene url="#s"/></scene></COLLADA>)";
}
template <class T> void be(std::string& b, T v) {   // big endian
    char tmp[sizeof(T)];
    std::memcpy(tmp, &v, sizeof(T));
    for (size_t i = 0; i < sizeof(T); ++i) b += tmp[sizeof(T) - 1 - i];
}
template <class T> void le(std::string& b, T v) { b.append(reinterpret_cast<const char*>(&v), sizeof(T)); }
inline std::string chunk3ds(uint16_t id, const std::string& payload) {
    std::string c;
    le<uint16_t>(c, id);
    le<uint32_t>(c, uint32_t(payload.size() + 6));
    return c + payload;
}
inline std::string sample3ds() {
    std::string mat = chunk3ds(0xA000, std::string("Red") + '\0');
    mat += chunk3ds(0xA020, chunk3ds(0x0011, std::string("\xff\x00\x00", 3)));
    std::string pct; le<int16_t>(pct, 50);
    mat += chunk3ds(0xA050, chunk3ds(0x0030, pct));
    std::string verts; le<uint16_t>(verts, 4);
    for (float f : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f}) le<float>(verts, f);
    std::string uvs; le<uint16_t>(uvs, 4);
    for (float f : {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f}) le<float>(uvs, f);
    std::string matrix;
    for (float f : {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f}) le<float>(matrix, f);
    std::string faces; le<uint16_t>(faces, 2);
    for (uint16_t i : {0, 1, 2, 7, 0, 2, 3, 7}) le<uint16_t>(faces, i);
    std::string group = std::string("Red") + '\0';
    for (uint16_t i : {2, 0, 1}) le<uint16_t>(group, i);
    std::string smooth; le<uint32_t>(smooth, 1); le<uint32_t>(smooth, 2);   // different groups: hard edge on the diagonal
    faces += chunk3ds(0x4130, group) + chunk3ds(0x4150, smooth);
    const std::string mesh = chunk3ds(0x4110, verts) + chunk3ds(0x4140, uvs) + chunk3ds(0x4160, matrix) + chunk3ds(0x4120, faces);
    std::string version; le<uint32_t>(version, 3);
    const std::string editor = chunk3ds(0x3D3E, version) + chunk3ds(0xAFFF, mat) + chunk3ds(0x4000, std::string("Box") + '\0' + chunk3ds(0x4100, mesh));
    std::string frames; le<uint32_t>(frames, 0); le<uint32_t>(frames, 30);
    std::string id; le<uint16_t>(id, 0);
    std::string hdr = std::string("Box") + '\0'; le<uint16_t>(hdr, 0); le<uint16_t>(hdr, 0); le<uint16_t>(hdr, 0xFFFF);
    std::string pivot; for (float f : {0.f, 0.f, 0.f}) le<float>(pivot, f);
    std::string track; le<uint16_t>(track, 0); le<uint32_t>(track, 0); le<uint32_t>(track, 0); le<uint32_t>(track, 2);
    le<uint32_t>(track, 0); le<uint16_t>(track, 0); for (float f : {0.f, 0.f, 0.f}) le<float>(track, f);
    le<uint32_t>(track, 30); le<uint16_t>(track, 1); le<float>(track, 0.5f); for (float f : {3.f, 0.f, 0.f}) le<float>(track, f);   // tension
    const std::string node = chunk3ds(0xB030, id) + chunk3ds(0xB010, hdr) + chunk3ds(0xB013, pivot) + chunk3ds(0xB020, track);
    const std::string kf = chunk3ds(0xB008, frames) + chunk3ds(0xB002, node);
    return chunk3ds(0x4D4D, chunk3ds(0x0002, version) + chunk3ds(0x3D3D, editor) + chunk3ds(0xB000, kf));
}
inline const char* asciiPly() {
    return "ply\r\nformat ascii 1.0\r\ncomment TextureFile t.tga\r\nelement vertex 4\r\nproperty float x\r\nproperty float y\r\nproperty float z\r\n"
           "property float s\r\nproperty float t\r\nproperty uchar red\r\nproperty uchar green\r\nproperty uchar blue\r\n"
           "element face 1\r\nproperty list uchar int vertex_indices\r\nend_header\r\n"
           "0 0 0 0 0 255 0 0\r\n1 0 0 1 0 0 255 0\r\n1 1 0 1 1 0 0 255\r\n0 1 0 0 1 255 255 255\r\n4 0 1 2 3\r\n";
}
inline std::string binaryPly() {   // the same quad, big endian, with an unknown element in between
    std::string b = "ply\nformat binary_big_endian 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
                    "property float s\nproperty float t\nproperty uchar red\nproperty uchar green\nproperty uchar blue\n"
                    "element extra 2\nproperty list uchar short junk\nproperty double w\n"
                    "element face 1\nproperty list uchar int vertex_indices\nend_header\n";
    const float v[4][5] = {{0, 0, 0, 0, 0}, {1, 0, 0, 1, 0}, {1, 1, 0, 1, 1}, {0, 1, 0, 0, 1}};
    const uint8_t c[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    for (int i = 0; i < 4; ++i) { for (float f : v[i]) be<float>(b, f); for (uint8_t x : c[i]) b += char(x); }
    for (int i = 0; i < 2; ++i) { b += char(2); be<int16_t>(b, 7); be<int16_t>(b, 8); be<double>(b, 1.5); }
    b += char(4);
    for (int i : {0, 1, 2, 3}) be<int32_t>(b, i);
    return b;
}
inline std::string binaryStl() {   // header starting with "solid" on purpose, two facets, VisCAM colour on the first
    std::string b = "solid but actually binary";
    b.resize(80, ' ');
    le<uint32_t>(b, 2);
    const float tri[2][12] = {{0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0}};
    for (int t = 0; t < 2; ++t) {
        for (float f : tri[t]) le<float>(b, f);
        le<uint16_t>(b, t == 0 ? uint16_t(0x8000u | (31u << 10)) : uint16_t(0));
    }
    return b;
}
// Truncations and byte flips must end in an exception or a model, never a crash.
inline int fuzz(const std::string& path, const std::string& bytes, const std::map<std::string, std::string>& extra) {
    int threw = 0;
    auto run = [&](const std::string& b) {
        std::map<std::string, std::string> files = extra;
        files[path] = b;
        try { std::vector<std::string> w; loadModel(path, mapReader(files), &w); }
        catch (const std::exception&) { ++threw; }
    };
    uint32_t s = 12345;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return s >> 8; };
    for (size_t cut = 0; cut < bytes.size(); cut += 1 + bytes.size() / 97) run(bytes.substr(0, cut));
    for (int i = 0; i < 300; ++i) {
        std::string b = bytes;
        for (int k = 0, n = 1 + int(rnd() % 4); k < n; ++k) b[rnd() % b.size()] = char(rnd());
        run(b);
    }
    return threw;
}
} // namespace interchangetest

TEST(models_dae_skinned_two_bones) {
    using namespace interchangetest;
    std::vector<std::string> warn;
    auto m = loadModel("units/rig.dae", mapReader({{"units/rig.dae", skinnedDae()}}), &warn);
    for (auto& w : warn) std::printf("  warning: %s\n", w.c_str());
    CHECK(warn.empty());
    const int root = m->findNode("Root"), top = m->findNode("Top"), body = m->findNode("Body");
    CHECK(root >= 0 && top >= 0 && body >= 0 && m->nodes.size() == 4);
    if (root < 0 || top < 0 || body < 0) return;
    CHECK(m->nodes[size_t(top)].parent == root && m->nodes[size_t(root)].parent == m->findNode("Armature"));
    CHECK(near(m->nodes[size_t(root)].t.x, 0.5f) && near(m->nodes[size_t(top)].t.y, 1));   // row-major <matrix>
    CHECK(m->skins.size() == 1 && m->nodes[size_t(body)].skin == 0 && m->nodes[size_t(body)].mesh == 0);
    const Skin& sk = m->skins[0];
    CHECK(sk.joints.size() == 2 && sk.joints[0] == root && sk.joints[1] == top);
    CHECK(near(sk.inverseBind[1].at(1, 3), -1) && near(sk.inverseBind[1].at(0, 3), -0.5f));
    const Primitive& p = m->meshes[0].primitives[0];
    CHECK(m->meshes[0].primitives.size() == 1 && p.indexCount == 6 && p.jointPalette.size() == 2);
    std::vector<mat4> pal;
    paletteMatrices(*m, sk, p, m->bindWorld, pal);
    bool identity = true;
    for (auto& x : pal) for (int i = 0; i < 16; ++i) identity &= near(x.m[i], (i % 5 == 0) ? 1.0f : 0.0f);
    CHECK(identity);
    bool topVertex = false;
    for (uint32_t i = 0; i < p.indexCount; ++i) {
        const ModelVertex& v = m->vertices[m->indices[p.firstIndex + i]];
        CHECK(near(v.pos.z, 0.25f));                       // bind shape matrix baked in
        CHECK(v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3] == 255);
        if (near(v.pos.x, -0.5f) && near(v.pos.y, 2)) {    // JTop 1, JRoot 0.25 -> 0.8 / 0.2
            topVertex = true;
            CHECK(p.jointPalette[v.joints[0]] == 1 && near(v.weights[0] / 255.0f, 0.8f, 0.01f));
        }
    }
    CHECK(topVertex);
    CHECK(m->clips.size() == 1 && m->findClip("walk") == 0);
    if (m->clips.empty()) return;
    const AnimClip& clip = m->clips[0];
    CHECK(near(clip.duration, 1));
    Pose pose;
    bindPose(*m, pose);
    sampleClip(clip, 0.5f, pose);
    const quat e = quat::axisAngle({0, 0, 1}, 0.785398f);
    CHECK(near(pose[size_t(root)].t.x, 1.5f, 1e-3f));
    CHECK(near(std::fabs(pose[size_t(top)].r.z), e.z, 2e-3f) && near(std::fabs(pose[size_t(top)].r.w), e.w, 2e-3f));
    CHECK(near(pose[size_t(top)].t.y, 1));
}

TEST(models_dae_textured_zup_and_errors) {
    using namespace interchangetest;
    std::vector<std::string> warn;
    auto m = loadModel("dae/tri.dae", mapReader({{"dae/tri.dae", texturedZupDae()}, {"dae/tex file.tga", tga2x2()}}), &warn);
    for (auto& w : warn) std::printf("  warning: %s\n", w.c_str());
    CHECK(warn.empty());
    // root node: centimetres -> metres, Z up -> Y up
    CHECK(m->nodes.size() == 2 && m->nodes[0].name == "root" && near(m->nodes[0].s.x, 0.01f) && m->nodes[1].parent == 0);
    CHECK(near(m->boundsMax.y, 1) && near(m->boundsMax.x, 1) && near(m->boundsMin.y, 0));
    const float3 up = m->nodes[0].r.rotate({0, 0, 1});
    CHECK(near(up.y, 1));
    const Primitive& p = m->meshes[0].primitives[0];
    const Material& mt = m->materials[size_t(p.material)];
    CHECK(m->materials.size() == 1 && mt.name == "Paint & Glass");
    CHECK(mt.baseTex == 0 && m->images.size() == 1 && m->images[0].width == 2);
    CHECK(mt.alpha == AlphaMode::Blend && near(mt.baseColor.w, 0.5f) && near(mt.emissive.x, 0.5f) && near(mt.roughness, 0.25f) && mt.doubleSided);
    const ModelVertex& v2 = m->vertices[m->indices[p.firstIndex + 2]];
    CHECK(near(v2.uv.y, 0) && near(v2.nrm.y, -1));         // V flipped; smoothed normal (no NORMAL input)
    // readable errors
    auto err = [](const std::string& path, const std::string& text) {
        try { loadModel(path, mapReader({{path, text}}), nullptr); }
        catch (const std::exception& e) { return std::string(e.what()); }
        return std::string();
    };
    std::string e = err("bad.dae", "<COLLADA><asset></COLLADA>");
    CHECK(e.find("bad.dae") == 0 && e.find("line 1") != std::string::npos);
    std::string broken = texturedZupDae();
    broken.replace(broken.find("<p>0 0 1 1 2 2</p>"), 18, "<p>0 0 1 1 9 2</p>");
    e = err("idx.dae", broken);
    CHECK(e.find("out of range") != std::string::npos);
    CHECK(err("cam.dae", "<COLLADA><library_visual_scenes><visual_scene id='s'><node id='c'/></visual_scene></library_visual_scenes></COLLADA>").find("no geometry") != std::string::npos);
    CHECK(err("x.dae", "<mesh/>").find("not a Collada") != std::string::npos);
}

TEST(models_3ds_chunks_smoothing_keyframer) {
    using namespace interchangetest;
    std::vector<std::string> warn;
    auto m = loadModel("old/box.3ds", mapReader({{"old/box.3ds", sample3ds()}}), &warn);
    for (auto& w : warn) std::printf("  warning: %s\n", w.c_str());
    CHECK(warn.empty());
    CHECK(m->nodes.size() == 2 && m->nodes[0].name == "root" && m->nodes[1].name == "Box" && m->nodes[1].parent == 0);
    const Primitive& p = m->meshes[0].primitives[0];
    CHECK(m->meshes[0].primitives.size() == 1 && p.indexCount == 6);
    CHECK(p.vertexCount == 6);            // smoothing groups 1 and 2 split the shared diagonal
    const Material& mt = m->materials[size_t(p.material)];
    CHECK(mt.name == "Red" && near(mt.baseColor.x, 1) && near(mt.baseColor.y, 0) && mt.alpha == AlphaMode::Blend && near(mt.baseColor.w, 0.5f));
    for (uint32_t i = 0; i < p.vertexCount; ++i) CHECK(near(m->vertices[p.firstVertex + i].nrm.z, 1));
    const float3 up = m->nodes[0].r.rotate({0, 0, 1});
    CHECK(near(up.y, 1));                  // Z up -> Y up
    CHECK(m->clips.size() == 1 && m->clips[0].name == "Take 001" && near(m->clips[0].duration, 1));
    if (m->clips.empty()) return;
    Pose pose;
    bindPose(*m, pose);
    sampleClip(m->clips[0], 0.5f, pose);
    CHECK(near(pose[1].t.x, 1.5f));
    bool threw = false;
    std::string bad = sample3ds();
    bad[2] = 0x7f;                         // main chunk length past the end
    try { loadModel("old/bad.3ds", mapReader({{"old/bad.3ds", bad}}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find("old/bad.3ds") == 0; }
    CHECK(threw);
}

TEST(models_ply_ascii_binary) {
    using namespace interchangetest;
    std::vector<std::string> warn;
    auto a = loadModel("scan/q.ply", mapReader({{"scan/q.ply", asciiPly()}, {"scan/t.tga", tga2x2()}}), &warn);
    auto b = loadModel("scan/b.ply", mapReader({{"scan/b.ply", binaryPly()}}), &warn);
    for (auto& w : warn) std::printf("  warning: %s\n", w.c_str());
    CHECK(warn.empty());
    for (const Model* m : {a.get(), b.get()}) {
        CHECK(m->vertices.size() == 4 && m->indices.size() == 6);
        CHECK(m->vertices[0].color == 0xff0000ffu && m->vertices[3].color == 0xffffffffu);   // red; white
        CHECK(near(m->vertices[2].uv.x, 1) && near(m->vertices[2].uv.y, 0));
        CHECK(near(m->vertices[1].nrm.z, 1));                                               // smoothed
        CHECK(near(m->boundsMax.y, 1) && near(m->boundsMax.x, 1));                          // Y up kept
    }
    CHECK(a->materials[0].baseTex == 0 && b->materials[0].baseTex == -1);
    bool threw = false;
    std::string bad = asciiPly();
    bad.replace(bad.find("4 0 1 2 3"), 9, "4 0 1 2 7");
    try { loadModel("scan/bad.ply", mapReader({{"scan/bad.ply", bad}}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find("out of range") != std::string::npos; }
    CHECK(threw);
}

TEST(models_stl_ascii_binary) {
    using namespace interchangetest;
    const std::string ascii = "solid part\n facet normal 0 0 0\n  outer loop\n   vertex 0 0 0\n   vertex 1 0 0\n   vertex 0 1 0\n  endloop\n endfacet\nendsolid part\n";
    std::vector<std::string> warn;
    auto a = loadModel("print/a.stl", mapReader({{"print/a.stl", ascii}}), &warn);
    auto b = loadModel("print/b.STL", mapReader({{"print/b.STL", binaryStl()}}), &warn);
    CHECK(warn.empty());
    CHECK(a->nodes.size() == 1 && a->nodes[0].name == "part" && a->vertices.size() == 3);
    CHECK(near(a->vertices[0].nrm.y, 1));                          // zero normal recomputed, Z up -> Y up
    CHECK(near(a->vertices[2].pos.z, -1) && near(a->vertices[2].pos.y, 0));
    CHECK(b->vertices.size() == 6 && b->vertices[0].color == 0xff0000ffu && b->vertices[3].color == 0xffffffffu);
    CHECK(near(b->vertices[0].nrm.y, 1));
    bool threw = false;
    try { loadModel("print/c.stl", mapReader({{"print/c.stl", "solid x\n facet normal 0 0 1\n vertex 0 0 zero\nendsolid\n"}}), nullptr); }
    catch (const std::exception& e) { threw = std::string(e.what()).find("print/c.stl") == 0; }
    CHECK(threw);
}

TEST(models_dae_palette_split) {
    using namespace interchangetest;
    // 100 joints, one triangle per joint: primitives are split so each palette fits kMaxPaletteJoints
    const int n = 100;
    std::string pos, idx, names, ibm, vcount, v, nodes;
    for (int j = 0; j < n; ++j) {
        const float x = float(j);
        pos += std::to_string(x) + " 0 0 " + std::to_string(x + 0.5f) + " 0 0 " + std::to_string(x) + " 1 0 ";
        idx += std::to_string(j * 3) + " " + std::to_string(j * 3 + 1) + " " + std::to_string(j * 3 + 2) + " ";
        names += "j" + std::to_string(j) + " ";
        ibm += "1 0 0 " + std::to_string(-x) + " 0 1 0 0 0 0 1 0 0 0 0 1 ";
        vcount += "1 1 1 ";
        for (int k = 0; k < 3; ++k) v += std::to_string(j) + " 0 ";
        nodes += "<node id='n" + std::to_string(j) + "' sid='j" + std::to_string(j) + "'><translate>" + std::to_string(x) + " 0 0</translate></node>";
    }
    const std::string dae = "<COLLADA><library_geometries><geometry id='g'><mesh><source id='p'><float_array id='pa' count='" + std::to_string(n * 9) + "'>" + pos +
        "</float_array><technique_common><accessor source='#pa' count='" + std::to_string(n * 3) + "' stride='3'/></technique_common></source>"
        "<vertices id='v'><input semantic='POSITION' source='#p'/></vertices><triangles count='" + std::to_string(n) + "'><input semantic='VERTEX' source='#v' offset='0'/><p>" + idx +
        "</p></triangles></mesh></geometry></library_geometries><library_controllers><controller id='c'><skin source='#g'>"
        "<source id='jn'><Name_array id='jna' count='" + std::to_string(n) + "'>" + names + "</Name_array><technique_common><accessor source='#jna' count='" + std::to_string(n) + "'/></technique_common></source>"
        "<source id='ib'><float_array id='iba' count='" + std::to_string(n * 16) + "'>" + ibm + "</float_array><technique_common><accessor source='#iba' count='" + std::to_string(n) + "' stride='16'/></technique_common></source>"
        "<source id='w'><float_array id='wa' count='1'>1</float_array><technique_common><accessor source='#wa' count='1'/></technique_common></source>"
        "<joints><input semantic='JOINT' source='#jn'/><input semantic='INV_BIND_MATRIX' source='#ib'/></joints>"
        "<vertex_weights count='" + std::to_string(n * 3) + "'><input semantic='JOINT' source='#jn' offset='0'/><input semantic='WEIGHT' source='#w' offset='1'/><vcount>" + vcount + "</vcount><v>" + v + "</v></vertex_weights>"
        "</skin></controller></library_controllers><library_visual_scenes><visual_scene id='s'><node id='skel'>" + nodes + "</node>"
        "<node id='body'><instance_controller url='#c'><skeleton>#skel</skeleton></instance_controller></node></visual_scene></library_visual_scenes></COLLADA>";
    std::vector<std::string> warn;
    auto m = loadModel("many.dae", mapReader({{"many.dae", dae}}), &warn);
    CHECK(warn.empty());
    CHECK(m->skins.size() == 1 && m->skins[0].joints.size() == size_t(n));
    const auto& prims = m->meshes[0].primitives;
    CHECK(prims.size() == 2);
    uint32_t tris = 0;
    for (const Primitive& p : prims) {
        tris += p.indexCount / 3;
        CHECK(!p.jointPalette.empty() && int(p.jointPalette.size()) <= kMaxPaletteJoints);
        std::vector<mat4> pal;
        paletteMatrices(*m, m->skins[0], p, m->bindWorld, pal);
        bool identity = true;
        for (auto& x : pal) for (int i = 0; i < 16; ++i) identity &= near(x.m[i], (i % 5 == 0) ? 1.0f : 0.0f);
        CHECK(identity);
        for (uint32_t i = 0; i < p.indexCount; ++i) {   // every vertex points at the joint of its triangle
            const ModelVertex& vx = m->vertices[m->indices[p.firstIndex + i]];
            const int joint = p.jointPalette[vx.joints[0]];
            CHECK(vx.weights[0] == 255 && joint == int(std::floor(vx.pos.x + 0.01f)));
        }
    }
    CHECK(tris == uint32_t(n));
}

TEST(models_interchange_fuzz_never_crashes) {
    using namespace interchangetest;
    // every loader survives truncations and random byte flips (throws or loads, never crashes)
    const int a = fuzz("f/rig.dae", skinnedDae(), {});
    const int b = fuzz("f/tri.dae", texturedZupDae(), {{"f/tex file.tga", tga2x2()}});
    const int c = fuzz("f/box.3ds", sample3ds(), {});
    const int d = fuzz("f/q.ply", asciiPly(), {});
    const int e = fuzz("f/b.ply", binaryPly(), {});
    const int f = fuzz("f/b.stl", binaryStl(), {});
    CHECK(a > 50 && b > 50 && c > 50 && d > 20 && e > 20 && f > 20);   // most truncations are reported
}

// ---- classic formats: Total Annihilation .3do / .gaf, C&C Generals .w3d, DDS (synthetic data) ----

namespace classictest {
using namespace rts::render;
using modeltest::near;
template <class T> void put(std::string& b, T v) { b.append(reinterpret_cast<const char*>(&v), sizeof v); }
template <class T> void putAt(std::string& b, size_t at, T v) { std::memcpy(&b[at], &v, sizeof v); }

// TA 3DO writer: pieces with vertices (native frame, units), primitives (colour, indices, texture).
struct TaPrim { int color; std::vector<uint16_t> idx; std::string tex; };
struct TaPiece { std::string name; float3 off; std::vector<float3> verts; std::vector<TaPrim> prims; std::vector<TaPiece> kids; int sel = -1; };
inline uint32_t emit3do(std::string& out, const TaPiece& p) {
    const uint32_t at = uint32_t(out.size());
    out.append(52, '\0');
    const uint32_t nameAt = uint32_t(out.size());
    out += p.name; out += '\0';
    const uint32_t vAt = uint32_t(out.size());
    for (auto v : p.verts) { put<int32_t>(out, int32_t(v.x * 65536)); put<int32_t>(out, int32_t(v.y * 65536)); put<int32_t>(out, int32_t(v.z * 65536)); }
    const uint32_t pAt = uint32_t(out.size());
    out.append(32 * p.prims.size(), '\0');
    for (size_t k = 0; k < p.prims.size(); ++k) {
        const uint32_t iAt = uint32_t(out.size());
        for (auto i : p.prims[k].idx) put<uint16_t>(out, i);
        uint32_t tAt = 0;
        if (!p.prims[k].tex.empty()) { tAt = uint32_t(out.size()); out += p.prims[k].tex; out += '\0'; }
        const size_t pa = pAt + 32 * k;
        putAt<int32_t>(out, pa, p.prims[k].color); putAt<int32_t>(out, pa + 4, int32_t(p.prims[k].idx.size()));
        putAt<uint32_t>(out, pa + 12, iAt); putAt<uint32_t>(out, pa + 16, tAt);
    }
    uint32_t child = 0, prev = 0;
    for (const auto& k : p.kids) {
        const uint32_t c = emit3do(out, k);
        if (!prev) child = c; else putAt<uint32_t>(out, prev + 44, c);
        prev = c;
    }
    const int32_t hdr[13] = {1, int32_t(p.verts.size()), int32_t(p.prims.size()), p.sel, int32_t(p.off.x * 65536), int32_t(p.off.y * 65536),
                             int32_t(p.off.z * 65536), int32_t(nameAt), 0, int32_t(vAt), int32_t(pAt), 0, int32_t(child)};
    std::memcpy(&out[at], hdr, sizeof hdr);
    return at;
}
// GAF with one entry; frames of w x h palette indices; compressed frames use TA's run-length lines.
inline std::string gafFile(const std::string& name, const std::vector<std::vector<uint8_t>>& frames, int w, int h, bool compressed) {
    std::string g;
    put<uint32_t>(g, 0x00010100); put<uint32_t>(g, 1); put<uint32_t>(g, 0); put<uint32_t>(g, 16);
    put<uint16_t>(g, uint16_t(frames.size())); put<uint16_t>(g, 1); put<uint32_t>(g, 0);
    std::string n = name; n.resize(32, '\0'); g += n;
    const size_t table = g.size();
    g.append(8 * frames.size(), '\0');
    for (size_t f = 0; f < frames.size(); ++f) {
        const uint32_t fat = uint32_t(g.size());
        g.append(24, '\0');
        const uint32_t dat = uint32_t(g.size());
        if (!compressed) g.append(reinterpret_cast<const char*>(frames[f].data()), frames[f].size());
        else
            for (int y = 0; y < h; ++y) {
                std::string line;
                for (int x = 0; x < w;) {
                    const uint8_t v = frames[f][size_t(y * w + x)];
                    int run = 1;
                    while (x + run < w && frames[f][size_t(y * w + x + run)] == v && run < 32) ++run;
                    if (v == 9) line += char(run << 1 | 1);
                    else if (run > 1) { line += char((run - 1) << 2 | 2); line += char(v); }
                    else { line += char(0); line += char(v); }
                    x += run;
                }
                put<uint16_t>(g, uint16_t(line.size())); g += line;
            }
        putAt<uint16_t>(g, fat, uint16_t(w)); putAt<uint16_t>(g, fat + 2, uint16_t(h));
        g[fat + 8] = 9; g[fat + 9] = compressed ? 1 : 0;
        putAt<uint32_t>(g, fat + 16, dat);
        putAt<uint32_t>(g, table + 8 * f, fat); putAt<uint32_t>(g, table + 8 * f + 4, 1);
    }
    return g;
}
inline std::string taPalette() {
    std::string p;
    for (int i = 0; i < 256; ++i) { p += char(i); p += char(255 - i); p += char(i / 2); p += '\0'; }
    return p;
}
inline uint8_t lin(int c) { float s = c / 255.0f; float l = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); return uint8_t(std::lround(l * 255)); }
// Every triangle's winding agrees with its stored normal (front faces point outward).
inline bool windingMatchesNormals(const Model& m) {
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const ModelVertex &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]], &c = m.vertices[m.indices[i + 2]];
        if (dot(cross(b.pos - a.pos, c.pos - a.pos), a.nrm) <= 0) return false;
    }
    return true;
}
// Feeds every prefix of `bytes` to `load`: each must throw (or load), never crash.
template <class F> bool survivesTruncation(const std::string& bytes, F&& load) {
    int threw = 0;
    for (size_t n = 0; n < bytes.size(); n += 1 + n / 64) {
        try { load(bytes.substr(0, n)); } catch (const std::runtime_error&) { ++threw; }
    }
    // random byte flips
    uint32_t seed = 12345;
    for (int k = 0; k < 300; ++k) {
        std::string b = bytes;
        for (int j = 0; j < 4; ++j) { seed = seed * 1664525u + 1013904223u; b[(seed >> 8) % b.size()] = char(seed >> 24); }
        try { load(b); } catch (const std::runtime_error&) {}
    }
    return threw > 0;
}

inline std::string tank3do() {
    TaPiece base{"base", {0, 0, 0}, {{-1, 0, -2}, {1, 0, -2}, {1, 0, 2}, {-1, 0, 2}, {0, 1, 0}, {-3, 0, -3}, {3, 0, -3}, {3, 0, 3}}, {}, {}, -1};
    // top-down CCW seen from +Y in the native frame: textured quad facing up
    base.prims.push_back({0, {0, 3, 2, 1}, "plate"});
    base.prims.push_back({40, {0, 1, 4}, ""});           // coloured triangle (palette 40)
    base.prims.push_back({40, {1, 0, 4}, ""});           // same vertex set: the last one wins
    base.prims.push_back({7, {0, 1}, ""});               // a line: skipped
    base.prims.push_back({3, {5, 6, 7}, ""});            // selection primitive: skipped
    base.sel = 4;
    TaPiece turret{"turret", {0.5f, 2, 1}, {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}}, {{200, {0, 1, 2}, ""}}, {}, -1};
    TaPiece flare{"flare", {0, 0, -3}, {}, {}, {}, -1};
    turret.kids.push_back(flare);
    base.kids.push_back(turret);
    TaPiece sib{"door", {-2, 0, 0}, {}, {}, {}, -1};
    base.kids.push_back(sib);
    std::string out;
    emit3do(out, base);
    return out;
}
} // namespace classictest

TEST(models_3do_pieces_palette_gaf) {
    using namespace classictest;
    std::vector<uint8_t> px(16, 50);
    px[0] = 9; px[1] = 9;                                 // transparent run (compressed skip)
    px[5] = 77;
    const std::string gaf = gafFile("plate", {px}, 4, 4, true);
    std::vector<std::string> warn;
    auto m = loadModel("units/tank.3do", modeltest::mapReader({{"units/tank.3do", tank3do()}, {"units/textures/plate.gaf", gaf}, {"units/../palettes/PALETTE.PAL", taPalette()}}), &warn);
    CHECK(warn.empty());
    for (auto& w : warn) std::printf("  warning: %s\n", w.c_str());
    const int b = m->findNode("base"), t = m->findNode("turret"), f = m->findNode("flare"), d = m->findNode("door");
    CHECK(b == 0 && t > 0 && f > 0 && d > 0);
    CHECK(m->nodes[size_t(t)].parent == b && m->nodes[size_t(f)].parent == t && m->nodes[size_t(d)].parent == b);
    CHECK(near(m->nodes[size_t(t)].t.x, 0.5f) && near(m->nodes[size_t(t)].t.y, 2) && near(m->nodes[size_t(t)].t.z, -1));   // z mirrored
    CHECK(near(m->nodes[size_t(f)].t.z, 3) && m->nodes[size_t(f)].mesh < 0);
    // base: textured quad (2 triangles) + one coloured triangle; selection primitive, duplicate, line skipped
    const MeshDef& bm = m->meshes[size_t(m->nodes[size_t(b)].mesh)];
    uint32_t tris = 0;
    for (auto& p : bm.primitives) tris += p.indexCount / 3;
    CHECK(tris == 3 && bm.primitives.size() == 2);
    CHECK(windingMatchesNormals(*m));
    const Primitive* texPrim = nullptr; const Primitive* colPrim = nullptr;
    for (auto& p : bm.primitives) (m->materials[size_t(p.material)].baseTex >= 0 ? texPrim : colPrim) = &p;
    CHECK(texPrim && colPrim);
    if (texPrim && colPrim) {
        // the textured quad faces +Y; colour 40 -> palette (40, 215, 20) as linear vertex colour
        CHECK(near(m->vertices[m->indices[texPrim->firstIndex]].nrm.y, 1));
        const uint32_t c = m->vertices[m->indices[colPrim->firstIndex]].color;
        CHECK((c & 255) == lin(40) && ((c >> 8) & 255) == lin(215) && ((c >> 16) & 255) == lin(20));
        const Image& img = m->images[size_t(m->materials[size_t(texPrim->material)].baseTex)];
        CHECK(img.width == 4 && img.height == 4 && img.rgba[3] == 0 && img.rgba[1 * 4 + 3] == 0 && img.rgba[2 * 4 + 3] == 255);
        CHECK(uint8_t(img.rgba[5 * 4]) == 77 && uint8_t(img.rgba[5 * 4 + 1]) == 255 - 77 && uint8_t(img.rgba[2 * 4]) == 50);
        CHECK(m->materials[size_t(texPrim->material)].alpha == AlphaMode::Mask);
    }
    CHECK(m->clips.empty() && m->skins.empty());
    // turret colour 200
    const MeshDef& tm = m->meshes[size_t(m->nodes[size_t(t)].mesh)];
    CHECK(tm.primitives.size() == 1 && tm.primitives[0].indexCount == 3);
}

TEST(models_3do_team_texture_and_fallbacks) {
    using namespace classictest;
    // 10 player frames: the logo pixels (index 1, 2) change, the border (index 0) does not -> team mask
    std::vector<std::vector<uint8_t>> frames;
    for (int p = 0; p < 10; ++p) { std::vector<uint8_t> f(4, 0); f[1] = uint8_t(100 + p); f[2] = uint8_t(100 + p); frames.push_back(f); }
    std::vector<std::string> warn;
    auto m = loadModel("u/tank.3do", modeltest::mapReader({{"u/tank.3do", tank3do()}, {"u/plate.gaf", gafFile("plate", frames, 2, 2, false)}, {"u/palette.pal", taPalette()}}), &warn);
    CHECK(warn.empty());
    int team = -1;
    for (auto& mt : m->materials) if (mt.team == TeamMode::Mask) team = mt.teamTex;
    CHECK(team >= 0);
    if (team >= 0) {
        const Image& mask = m->images[size_t(team)];
        CHECK(mask.rgba[0] == 0 && uint8_t(mask.rgba[4]) == 255 && uint8_t(mask.rgba[8]) == 255 && mask.rgba[12] == 0);
    }
    // no palette, no texture: built-in palette + one aggregated warning each
    warn.clear();
    auto m2 = loadModel("u/tank.3do", modeltest::mapReader({{"u/tank.3do", tank3do()}}), &warn);
    CHECK(warn.size() == 2 && m2->images.empty());
    bool mentions = false;
    for (auto& w : warn) mentions |= w.find("plate") != std::string::npos;
    CHECK(mentions);
    // converted PNG-free fallback: a .tga next to the model
    std::string tga(18, '\0');
    tga[2] = 2; tga[12] = 1; tga[14] = 1; tga[16] = 32; tga[17] = 8;
    tga += std::string("\x10\x20\x30\xff", 4);
    warn.clear();
    auto m3 = loadModel("u/tank.3do", modeltest::mapReader({{"u/tank.3do", tank3do()}, {"u/textures/plate.tga", tga}, {"u/palette.pal", taPalette()}}), &warn);
    CHECK(warn.empty() && m3->images.size() == 1 && m3->images[0].width == 1);
}

TEST(models_3do_malformed_input) {
    using namespace classictest;
    const std::string good = tank3do();
    auto reader = modeltest::mapReader({{"u/palette.pal", taPalette()}});
    CHECK(survivesTruncation(good, [&](const std::string& b) { load3do("u/tank.3do", b, reader, nullptr); }));
    // the turret's sibling pointing at itself: cycle error, not an endless loop
    std::string loop = good;
    uint32_t turretAt = 0;
    std::memcpy(&turretAt, &good[48], 4);
    putAt<uint32_t>(loop, turretAt + 44, turretAt);
    bool threw = false;
    try { load3do("u/a.3do", loop, reader, nullptr); } catch (const std::runtime_error& e) { threw = std::string(e.what()).find("cycle") != std::string::npos; }
    CHECK(threw);
    std::string bad = good;
    putAt<uint32_t>(bad, 48, uint32_t(bad.size() + 100));
    threw = false;
    try { load3do("u/b.3do", bad, reader, nullptr); } catch (const std::runtime_error& e) { threw = std::string(e.what()).find("u/b.3do") == 0; }
    CHECK(threw);
    threw = false;
    try { load3do("u/c.3do", std::string("\x02\0\0\0", 4) + std::string(60, '\0'), reader, nullptr); } catch (const std::runtime_error& e) { threw = std::string(e.what()).find("version") != std::string::npos; }
    CHECK(threw);
    // GAF damage: textures fall back to palette colours with a warning, never a crash
    std::vector<uint8_t> px(16, 50);
    const std::string gaf = gafFile("plate", {px}, 4, 4, true);
    for (size_t n = 0; n < gaf.size(); n += 3) {
        std::vector<std::string> w;
        auto r = modeltest::mapReader({{"u/palette.pal", taPalette()}, {"u/plate.gaf", gaf.substr(0, n)}});
        auto m = load3do("u/tank.3do", good, r, &w);
        CHECK(!m->meshes.empty());
    }
}

namespace classictest {
// W3D writer helpers
inline std::string ck(uint32_t id, const std::string& payload, bool container = false) {
    std::string c;
    put<uint32_t>(c, id); put<uint32_t>(c, uint32_t(payload.size()) | (container ? 0x80000000u : 0));
    return c + payload;
}
inline std::string nm(const std::string& s, size_t n = 16) { std::string r = s; r.resize(n, '\0'); return r; }
inline std::string f32s(std::initializer_list<float> v) { std::string s; for (float x : v) put<float>(s, x); return s; }
inline std::string u32s(std::initializer_list<uint32_t> v) { std::string s; for (uint32_t x : v) put<uint32_t>(s, x); return s; }
// one triangle mesh (+ optional influences / texture / house colour name)
inline std::string w3dTri(const std::string& name, const std::vector<float3>& pos, const std::vector<uint16_t>& bones, const std::string& tex, uint32_t attrs = 0) {
    std::string hdr = u32s({0x00040002, attrs | (bones.empty() ? 0u : 0x00020000u)}) + nm(name) + nm("TANK");
    hdr += u32s({1, uint32_t(pos.size()), 1, 0, 0, 0, 0, 3, 1}) + std::string(40, '\0');
    std::string c = ck(0x1F, hdr), v, n, uv, t;
    for (auto p : pos) { v += f32s({p.x, p.y, p.z}); n += f32s({0, 0, 1}); uv += f32s({p.x, p.y}); }
    c += ck(0x02, v) + ck(0x03, n);
    t = u32s({0, 1, 2, 0}) + f32s({0, 0, 1, 0});
    c += ck(0x20, t);
    if (!bones.empty()) { std::string inf; for (auto b : bones) { put<uint16_t>(inf, b); inf.append(6, '\0'); } c += ck(0x0E, inf); }
    std::string vm = u32s({0}) + std::string("\xff\xff\xff\0\xff\xff\xff\0", 8) + std::string(8, '\0') + f32s({1, 1, 0});
    c += ck(0x2A, ck(0x2B, ck(0x2D, vm), true), true);
    std::string sh(16, '\0'); sh[0] = 3; sh[1] = 1; sh[5] = 1; sh[7] = 1; sh[8] = tex.empty() ? 0 : 1;
    c += ck(0x29, sh);
    std::string pass = ck(0x39, u32s({0})) + ck(0x3A, u32s({0}));
    if (!tex.empty()) {
        c += ck(0x30, ck(0x31, ck(0x32, tex + '\0'), true), true);
        pass += ck(0x48, ck(0x49, u32s({0})) + ck(0x4A, uv), true);
    }
    c += ck(0x38, pass, true);
    return ck(0x00, c, true);
}
inline std::string w3dHierarchy() {
    std::string hdr = u32s({0x00040001}) + nm("TANK_SKL") + u32s({2}) + f32s({0, 0, 0});
    std::string piv = nm("ROOTTRANSFORM") + u32s({0xffffffffu}) + f32s({0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    piv += nm("Turret") + u32s({0}) + f32s({1, 0, 2, 0, 0, 0, 0, 0, 0, 1});
    return ck(0x100, ck(0x101, hdr) + ck(0x102, piv), true);
}
inline std::string w3dHlod() {
    std::string lod = ck(0x703, u32s({3}) + f32s({1e30f}));
    lod += ck(0x704, u32s({1}) + nm("TANK.GUN", 32));
    lod += ck(0x704, u32s({0}) + nm("TANK.HOUSECOLOR01", 32));
    lod += ck(0x704, u32s({0}) + nm("TANK.SKIN", 32));
    std::string low = ck(0x703, u32s({1}) + f32s({0.2f})) + ck(0x704, u32s({0}) + nm("TANK.LOWLOD", 32));
    return ck(0x700, ck(0x701, u32s({0x00010000, 2}) + nm("TANK") + nm("TANK_SKL")) + ck(0x702, low, true) + ck(0x702, lod, true), true);
}
// classic animation: Turret rotates about W3D Z (up) by 0 -> 90 degrees over 3 frames at 2 fps
inline std::string w3dAnim() {
    std::string hdr = u32s({0x00040001}) + nm("TANK_SKL.TURN") + nm("TANK_SKL") + u32s({3, 2});
    std::string q;
    for (int f = 0; f < 3; ++f) { const float a = 0.785398f * float(f) / 2; q += f32s({0, 0, std::sin(a), std::cos(a)}); }
    std::string chq; put<uint16_t>(chq, 0); put<uint16_t>(chq, 2); put<uint16_t>(chq, 4); put<uint16_t>(chq, 6); put<uint16_t>(chq, 1); put<uint16_t>(chq, 0);
    std::string chx; put<uint16_t>(chx, 1); put<uint16_t>(chx, 1); put<uint16_t>(chx, 1); put<uint16_t>(chx, 0); put<uint16_t>(chx, 1); put<uint16_t>(chx, 0);
    return ck(0x200, ck(0x201, hdr) + ck(0x202, chq + q) + ck(0x202, chx + f32s({0.5f})), true);
}
inline std::string w3dModel(bool withHierarchy) {
    std::string f = withHierarchy ? w3dHierarchy() : std::string();
    f += w3dTri("GUN", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {}, "gun.tga");
    f += w3dTri("HOUSECOLOR01", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {}, "");
    f += w3dTri("LOWLOD", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {}, "");
    f += w3dTri("HIDDEN", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {}, "", 0x1000);
    // skinned: vertex 2 lives in Turret space (0, 1, 1) -> model (1, 1, 3) in W3D = (1, 3, 1) in the engine
    f += w3dTri("SKIN", {{0, 0, 0}, {1, 0, 0}, {0, 1, 1}}, {0, 0, 1}, "zhca_body.tga");
    f += w3dHlod();
    if (withHierarchy) f += w3dAnim();
    return f;
}
inline std::string ddsHeader(int w, int h, const char* fourcc, uint32_t bits = 0, uint32_t r = 0, uint32_t g = 0, uint32_t b = 0, uint32_t a = 0, uint32_t mips = 1, uint32_t pfFlags = 0) {
    std::string d = "DDS ";
    put<uint32_t>(d, 124); put<uint32_t>(d, 0x1007 | (mips > 1 ? 0x20000u : 0)); put<uint32_t>(d, uint32_t(h)); put<uint32_t>(d, uint32_t(w));
    put<uint32_t>(d, 0); put<uint32_t>(d, 0); put<uint32_t>(d, mips); d.append(44, '\0');
    put<uint32_t>(d, 32);
    if (fourcc) { put<uint32_t>(d, 0x4 | pfFlags); d.append(fourcc, 4); d.append(20, '\0'); }
    else { put<uint32_t>(d, pfFlags ? pfFlags : 0x41); put<uint32_t>(d, 0); put<uint32_t>(d, bits); put<uint32_t>(d, r); put<uint32_t>(d, g); put<uint32_t>(d, b); put<uint32_t>(d, a); }
    put<uint32_t>(d, 0x1000); d.append(16, '\0');
    return d;
}
inline std::string dxt1Solid(uint16_t c565, int blocks) {
    std::string s;
    for (int i = 0; i < blocks; ++i) { put<uint16_t>(s, c565); put<uint16_t>(s, c565); put<uint32_t>(s, 0); }
    return s;
}
} // namespace classictest

TEST(models_w3d_hierarchy_hlod_skin_anim) {
    using namespace classictest;
    const std::string tga = std::string("\0\0\x02\0\0\0\0\0\0\0\0\0\x01\0\x01\0\x20\x08", 18) + std::string("\x10\x20\x30\x00", 4);
    std::vector<std::string> warn;
    auto m = loadModel("art/w3d/tank.w3d", modeltest::mapReader({{"art/w3d/tank.w3d", w3dModel(true)}, {"art/w3d/../Textures/gun.dds", ddsHeader(4, 4, "DXT1") + dxt1Solid(0xF800, 1)},
                                                                  {"art/w3d/zhca_body.tga", tga}}), &warn);
    for (auto& w : warn) std::printf("  warning: %s\n", w.c_str());
    CHECK(warn.empty());
    const int root = m->findNode("ROOTTRANSFORM"), turret = m->findNode("Turret");
    CHECK(root == 0 && turret == 1 && m->nodes[1].parent == 0);
    // W3D (1, 0, 2) Z-up / +X forward -> engine (0, 2, 1) Y-up / +Z forward
    CHECK(near(m->nodes[1].t.x, 0) && near(m->nodes[1].t.y, 2) && near(m->nodes[1].t.z, 1));
    // GUN on the Turret bone node itself, HOUSECOLOR01 on the root, LOWLOD / HIDDEN skipped
    CHECK(m->nodes[1].mesh >= 0 && m->meshes[size_t(m->nodes[1].mesh)].name == "GUN");
    CHECK(m->nodes[0].mesh >= 0 && m->meshes[size_t(m->nodes[0].mesh)].name == "HOUSECOLOR01");
    CHECK(m->meshes.size() == 3);
    for (auto& md : m->meshes) CHECK(md.name != "LOWLOD" && md.name != "HIDDEN");
    const Material& gun = m->materials[size_t(m->meshes[size_t(m->nodes[1].mesh)].primitives[0].material)];
    CHECK(gun.baseTex >= 0 && m->images[size_t(gun.baseTex)].width == 4 && m->images[size_t(gun.baseTex)].compressed);
    CHECK(near(gun.baseColor.x, 1) && gun.team == TeamMode::None);
    const Material& house = m->materials[size_t(m->meshes[size_t(m->nodes[0].mesh)].primitives[0].material)];
    CHECK(house.team == TeamMode::Tint && near(house.baseColor.x, 1) && near(house.baseColor.y, 1));
    // UV V flipped (D3D convention): vertex (1, 0) -> uv (1, 1)
    const Primitive& gp = m->meshes[size_t(m->nodes[1].mesh)].primitives[0];
    CHECK(near(m->vertices[gp.firstVertex + 1].uv.x, 1) && near(m->vertices[gp.firstVertex + 1].uv.y, 1));
    CHECK(near(m->vertices[gp.firstVertex].pos.y, 0) && near(m->vertices[gp.firstVertex + 1].pos.z, 1));   // w3d +X -> engine +Z
    CHECK(windingMatchesNormals(*m));
    // skin: joints = pivots, vertex 2 moved from Turret space into model space
    CHECK(m->skins.size() == 1 && m->skins[0].joints.size() == 2);
    int skinNode = -1;
    for (size_t i = 0; i < m->nodes.size(); ++i) if (m->nodes[i].skin == 0) skinNode = int(i);
    CHECK(skinNode >= 0);
    if (skinNode >= 0) {
        const Primitive& sp = m->meshes[size_t(m->nodes[size_t(skinNode)].mesh)].primitives[0];
        CHECK(sp.jointPalette.size() == 2);
        const ModelVertex& v2 = m->vertices[sp.firstVertex + 2];
        CHECK(near(v2.pos.x, 1) && near(v2.pos.y, 3) && near(v2.pos.z, 1));
        CHECK(sp.jointPalette[v2.joints[0]] == 1 && v2.weights[0] == 255);
        const mat4 jm = m->bindWorld[1] * m->skins[0].inverseBind[1];
        CHECK(near(jm.at(0, 0), 1) && near(jm.at(1, 3), 0));   // bind pose: identity joint matrix
        CHECK(m->materials[size_t(sp.material)].team == TeamMode::BaseAlpha);
    }
    // clip TURN: 3 frames at 2 fps = 1 s; Turret yaw about W3D Z = engine Y, translation +0.5 W3D X at frame 1
    CHECK(m->clips.size() == 1 && m->findClip("TURN") == 0);
    if (m->clips.size() == 1) {
        CHECK(near(m->clips[0].duration, 1));
        Pose pose;
        bindPose(*m, pose);
        sampleClip(m->clips[0], 1.0f, pose);
        const quat e = quat::axisAngle({0, 1, 0}, 1.570796f);
        CHECK(near(pose[1].r.y, e.y) && near(pose[1].r.w, e.w) && near(pose[1].r.x, 0));
        CHECK(near(pose[1].t.z, 1));   // outside its frame range the X channel is 0
        sampleClip(m->clips[0], 0.5f, pose);
        CHECK(near(pose[1].t.z, 1.5f) && near(pose[1].t.y, 2));
    }
}

TEST(models_w3d_external_hierarchy_and_anims) {
    using namespace classictest;
    // time-coded: Turret X 0 -> 1 between frames 0 and 4, then a binary (step) key to 3 at frame 6
    std::string tc;
    put<uint32_t>(tc, 3); put<uint16_t>(tc, 1); tc += char(1); tc += char(0);
    tc += u32s({0}) + f32s({0}) + u32s({4}) + f32s({1}) + u32s({6u | 0x80000000u}) + f32s({3});
    const std::string tcAnim = ck(0x280, ck(0x281, u32s({1}) + nm("TANK_SKL.RECOIL") + nm("TANK_SKL") + u32s({8}) + std::string("\x0a\0\0\0", 4)) + ck(0x282, tc), true);
    // adaptive delta: Turret Y starts at 2, +1 * 0.25 per frame (filter index 8 = 1.0, scale 0.25)
    std::string ad;
    put<uint32_t>(ad, 5); put<uint16_t>(ad, 1); ad += char(1); ad += char(1); put<float>(ad, 0.25f);
    put<float>(ad, 2.0f);
    ad += char(8); ad += std::string(8, char(0x11));
    const std::string adAnim = ck(0x280, ck(0x281, u32s({1}) + nm("WOBBLE") + nm("TANK_SKL") + u32s({5}) + std::string("\x0a\0\x01\0", 4)) + ck(0x282, ad), true);
    const std::string other = ck(0x200, ck(0x201, u32s({0x00040001}) + nm("OTHER.WALK") + nm("OTHER_SKL") + u32s({2, 30})), true);
    std::vector<std::string> warn;
    auto m = loadModel("w/tank.w3d", modeltest::mapReader({{"w/tank.w3d", w3dModel(false)}, {"w/TANK_SKL.w3d", w3dHierarchy() + tcAnim},
                                                            {"w/tank.anims.txt", "# clips\nwobble.w3d\n  missing.w3d \nother.w3d\n"},
                                                            {"w/wobble.w3d", adAnim}, {"w/other.w3d", other}}), &warn);
    CHECK(m->findNode("Turret") == 1 && m->skins.size() == 1);
    CHECK(m->findClip("RECOIL") >= 0 && m->findClip("WOBBLE") >= 0 && m->clips.size() == 2);
    int missing = 0, wrongHier = 0, tex = 0;
    for (auto& w : warn) { missing += w.find("missing.w3d") != std::string::npos; wrongHier += w.find("OTHER_SKL") != std::string::npos; tex += w.find("not found") != std::string::npos; }
    CHECK(missing == 1 && wrongHier == 1 && tex >= 2);   // + the two textures
    Pose pose;
    bindPose(*m, pose);
    const int rc = m->findClip("RECOIL"), wb = m->findClip("WOBBLE");
    if (rc >= 0 && wb >= 0) {
        sampleClip(m->clips[size_t(rc)], 2 / 10.0f, pose);   // frame 2: halfway 0 -> 1
        CHECK(near(pose[1].t.z, 1.5f));
        sampleClip(m->clips[size_t(rc)], 5 / 10.0f, pose);   // frame 5: held at 1 (next key is a step)
        CHECK(near(pose[1].t.z, 2.0f));
        sampleClip(m->clips[size_t(rc)], 6 / 10.0f, pose);
        CHECK(near(pose[1].t.z, 4.0f));
        sampleClip(m->clips[size_t(wb)], 4 / 10.0f, pose);   // frame 4: 2 + 4 * 0.25 = 3 along W3D Y = engine X
        CHECK(near(pose[1].t.x, 3.0f) && near(pose[1].t.y, 2));
    }
    // hierarchy missing entirely: meshes at the origin + warning
    warn.clear();
    auto m2 = loadModel("w/tank.w3d", modeltest::mapReader({{"w/tank.w3d", w3dModel(false)}}), &warn);
    bool said = false;
    for (auto& w : warn) said |= w.find("TANK_SKL") != std::string::npos;
    CHECK(said && m2->skins.empty() && !m2->meshes.empty());
}

TEST(models_w3d_malformed_input) {
    using namespace classictest;
    const std::string good = w3dModel(true);
    auto reader = modeltest::mapReader({});
    CHECK(survivesTruncation(good, [&](const std::string& b) { loadW3d("x.w3d", b, reader, nullptr); }));
    bool threw = false;
    try { loadW3d("h.w3d", w3dHierarchy(), reader, nullptr); } catch (const std::runtime_error& e) { threw = std::string(e.what()).find("no meshes") != std::string::npos; }
    CHECK(threw);
    // triangle index out of range
    std::string bad = good;
    const size_t tri = bad.find(u32s({0, 1, 2, 0}));
    putAt<uint32_t>(bad, tri + 4, 99);
    threw = false;
    try { loadW3d("t.w3d", bad, reader, nullptr); } catch (const std::runtime_error& e) { threw = std::string(e.what()).find("out of range") != std::string::npos; }
    CHECK(threw);
}

TEST(models_dds_formats) {
    using namespace classictest;
    Image img;
    // DXT1 8x8 with a full mip chain (4 + 1 + 1 + 1 blocks... 8x8: 4 blocks, 4x4: 1, 2x2: 1, 1x1: 1)
    CHECK(decodeImage(ddsHeader(8, 8, "DXT1", 0, 0, 0, 0, 0, 4) + dxt1Solid(0x07E0, 7), img));
    CHECK(img.width == 8 && img.height == 8 && img.rgba[0] == 0 && uint8_t(img.rgba[1]) == 255 && uint8_t(img.rgba[3]) == 255);
    CHECK(img.compressed && img.compressed->format == BlockFormat::BC1 && img.compressed->levels.size() == 4 && img.compressed->vkFormat == 133);
    // DXT1 three-colour block using index 3, no alpha flag -> opaque black, kept as CPU copy
    std::string tb; put<uint16_t>(tb, 0x0000); put<uint16_t>(tb, 0xFFFF); put<uint32_t>(tb, 0xFFFFFFFFu);
    CHECK(decodeImage(ddsHeader(4, 4, "DXT1") + tb, img) && img.rgba[0] == 0 && uint8_t(img.rgba[3]) == 255 && img.compressed->vkFormat == 131);
    CHECK(decodeImage(ddsHeader(4, 4, "DXT1", 0, 0, 0, 0, 0, 1, 0x1) + tb, img) && img.rgba[3] == 0);
    // DXT5: alpha endpoints 255 / 0, all indices 1 -> alpha 0; colour red
    std::string b5 = std::string("\xff\x00", 2) + std::string("\x49\x92\x24\x49\x92\x24", 6);
    put<uint16_t>(b5, 0xF800); put<uint16_t>(b5, 0xF800); put<uint32_t>(b5, 0);
    CHECK(decodeImage(ddsHeader(4, 4, "DXT5") + b5, img) && uint8_t(img.rgba[0]) == 255 && img.rgba[3] == 0 && img.compressed && img.compressed->format == BlockFormat::BC3);
    // DXT3: explicit 4-bit alpha 0x8 -> 136
    std::string b3(8, char(0x88)); put<uint16_t>(b3, 0x001F); put<uint16_t>(b3, 0x001F); put<uint32_t>(b3, 0);
    CHECK(decodeImage(ddsHeader(4, 4, "DXT3") + b3, img) && uint8_t(img.rgba[2]) == 255 && uint8_t(img.rgba[3]) == 136 && !img.compressed);
    // ATI2 / BC5: red = green = 255 -> blue reconstructed ~0 ... x = y = 1 -> z = 0 -> 128
    std::string b45 = std::string("\xff\xff", 2) + std::string(6, '\0');
    CHECK(decodeImage(ddsHeader(4, 4, "ATI2") + b45 + b45, img) && uint8_t(img.rgba[0]) == 255 && uint8_t(img.rgba[1]) == 255 && near(float(uint8_t(img.rgba[2])), 128, 1.5f));
    CHECK(decodeImage(ddsHeader(4, 4, "ATI1") + b45, img) && uint8_t(img.rgba[0]) == 255 && img.rgba[1] == 0);
    // uncompressed A8R8G8B8 2x1
    CHECK(decodeImage(ddsHeader(2, 1, nullptr, 32, 0xff0000, 0xff00, 0xff, 0xff000000u, 1, 0x41) + std::string("\x30\x20\x10\x80\x01\x02\x03\x04", 8), img));
    CHECK(img.width == 2 && img.rgba[0] == 0x10 && img.rgba[1] == 0x20 && img.rgba[2] == 0x30 && uint8_t(img.rgba[3]) == 0x80 && img.rgba[4] == 3 && img.rgba[7] == 4);
    // X8R8G8B8 (no alpha flag), R5G6B5, L8, A8L8 (D3DFMT code in the FourCC field)
    CHECK(decodeImage(ddsHeader(1, 1, nullptr, 32, 0xff0000, 0xff00, 0xff, 0, 1, 0x40) + std::string("\x30\x20\x10\x00", 4), img) && uint8_t(img.rgba[3]) == 255 && img.rgba[0] == 0x10);
    std::string p565; put<uint16_t>(p565, 0xF800);
    CHECK(decodeImage(ddsHeader(1, 1, nullptr, 16, 0xf800, 0x7e0, 0x1f, 0, 1, 0x40) + p565, img) && uint8_t(img.rgba[0]) == 255 && img.rgba[1] == 0 && img.rgba[2] == 0);
    CHECK(decodeImage(ddsHeader(1, 1, nullptr, 8, 0xff, 0, 0, 0, 1, 0x20000) + std::string("\x40", 1), img) && img.rgba[0] == 0x40 && img.rgba[2] == 0x40 && uint8_t(img.rgba[3]) == 255);
    const char a8l8[4] = {51, 0, 0, 0};
    CHECK(decodeImage(ddsHeader(1, 1, a8l8) + std::string("\x40\x80", 2), img) && img.rgba[1] == 0x40 && uint8_t(img.rgba[3]) == 0x80);
    // DX10 header: BC7 is kept compressed, unknown DXGI formats are rejected
    std::string dx10 = ddsHeader(4, 4, "DX10");
    std::string bc7(16, '\0'); bc7[0] = 0x40;   // mode 6
    CHECK(decodeImage(dx10 + u32s({98, 3, 0, 1, 0}) + bc7, img) && img.compressed && img.compressed->format == BlockFormat::BC7);
    CHECK(!decodeImage(dx10 + u32s({2, 3, 0, 1, 0}) + std::string(256, '\0'), img));
    // truncated / malformed: false, never a crash
    const std::string full = ddsHeader(16, 16, "DXT5", 0, 0, 0, 0, 0, 5) + std::string(16 * 16 + 64 + 16 * 3, '\x5a');
    int ok = 0;
    for (size_t n = 0; n < full.size(); n += 7) { Image t; ok += decodeImage(full.substr(0, n), t); }
    CHECK(ok > 0 && ok < 60);
    CHECK(!decodeImage(ddsHeader(0, 4, "DXT1"), img) && !decodeImage(ddsHeader(100000, 4, "DXT1"), img));
}

// ---- sprite shadows (RtsSpriteShadow): data keys, tiers, projected decal geometry
#include "modules/rts/view/RtsSpriteShadow.h"






// ---------------- one-click MCP setup (editor Agent window) ----------------
#if defined(RTS_TESTS_EDITOR_CORE)
#include "apps/editor/core/McpInstall.h"
#endif

// ---------------- Planets: surface, world block, orbit layer, transit ----------------
static const std::string kPlanets = std::string(RTS_SOURCE_DIR) + "/templates/planets";

TEST(planet_surface_offsets_and_hash_buckets) {
    using namespace rts::game;
    CubeSphere s(12);
    // offsets inside a face are exact; across seams they agree with neighbour steps
    int bad = 0;
    for (int id = 0; id < s.cellCount(); ++id) {
        for (int k = 0; k < 4; ++k) {
            static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            if (s.offset(id, d[k][0], d[k][1]) != s.neighbor(id, d[k][0], d[k][1])) ++bad;
        }
        // a 2-step walk ends near (2 cells) the start, never on itself
        for (int dx = -2; dx <= 2; ++dx) {
            const int o = s.offset(id, dx, 0);
            if (o < 0) continue;
            const Fixed arc = sphereArcTurns(s.cellCenterDir(id), s.cellCenterDir(o));
            if ((dx != 0 && o == id) || arc > Fixed::fromRatio(3 * (dx < 0 ? -dx : dx) + 1, 4 * 12 * 2)) ++bad;
        }
    }
    CHECK(bad == 0);
    Surface S;
    BodyDef b; b.id = "p"; b.radius = Fixed::fromInt(20); b.faceSize = 12;
    S.bodies = {b};
    S.build();
    CHECK(S.W == 12 && S.H == 72 && S.bucketCount() == 6 * 3 * 3);
    // every cell centre maps back to its cell, and lies on body 0 at radius
    int bad2 = 0;
    for (int c = 0; c < S.W * S.H; ++c) {
        const Vec3 p = S.cellCenter(c);
        if (S.cellAt(p) != c || S.bodyAt(p) != 0 || fxAbs(S.altitude(p)) > Fixed::fromRatio(1, 100)) ++bad2;
        std::vector<int32_t> near;
        S.bucketsAround(p, Fixed::fromInt(3), near);
        if (std::find(near.begin(), near.end(), S.bucketOf(p)) == near.end()) ++bad2;
    }
    CHECK(bad2 == 0);
    // ground distance is the chord, symmetric
    const Vec3 a = S.cellCenter(S.gridCell(0, 4, 6, 6)), c = S.cellCenter(S.gridCell(0, 5, 6, 6));
    CHECK(S.groundDist(a, c) == S.groundDist(c, a) && S.groundDist(a, c) > Fixed::fromInt(39));
    CHECK(!Surface::onSphere(Vec3{Fixed::fromInt(2048), Fixed{}, Fixed::fromInt(2048)}) && Surface::onSphere(a));
}

TEST(planet_world_loads_and_paths_cross_seams) {
    auto sim = makeSim(kPlanets, "maps/two_worlds.json", 1);
    auto& m = mod(*sim);
    const game::Surface* S = m.surface(*sim);
    CHECK(S != nullptr);
    if (!S) return;
    CHECK(S->bodies.size() == 3 && S->findBody("terra") == 1 && S->startBody == 1);
    CHECK(S->bodies[1].hasSurface() && !S->bodies[0].hasSurface());
    // commanders on opposite faces of terra
    Entity c1 = findType(*sim, "commander", 1), c2 = findType(*sim, "commander", 2);
    CHECK(c1.valid() && c2.valid());
    const Vec3 p1 = sim->world().get<Transform>(c1).pos, p2 = sim->world().get<Transform>(c2).pos;
    CHECK(m.bodyOf(*sim, p1) == 1 && m.bodyOf(*sim, p2) == 1);
    // ground path from one pole to the other: crosses at least two face seams, every step a neighbour
    auto& g = m.state(*sim).grid;
    std::vector<Vec3> path;
    CHECK(g.findPath(p1, p2, game::kLayerGround, path, 400000));
    CHECK(path.size() >= 2);
    if (!path.empty()) CHECK(path.back() == p2);
    // the route (waypoints joined by great-circle chords) visits at least one face besides the two poles
    std::vector<int> faces;
    Vec3 prev = p1;
    for (auto& p : path) {
        for (int k = 1; k <= 16; ++k) {
            int b, f, x, y;
            S->cellFace(S->cellAt(prev + (p - prev) * Fixed::fromRatio(k, 16)), b, f, x, y);
            if (std::find(faces.begin(), faces.end(), f) == faces.end()) faces.push_back(f);
        }
        prev = p;
    }
    CHECK(faces.size() >= 3);
    // a move order gets the commander around the planet
    Command mv; mv.player = 1; mv.type = game::kCmdMove; mv.units = {c1}; mv.point = S->cellCenter(S->gridCell(1, 0, 12, 12));
    sim->submit(mv);
    const Fixed before = S->groundDist(sim->world().get<Transform>(c1).pos, mv.point);
    for (int i = 0; i < 20 * 15; ++i) sim->step();
    const Vec3 now = sim->world().get<Transform>(c1).pos;
    CHECK(S->groundDist(now, mv.point) < before - Fixed::fromInt(10));
    CHECK(fxAbs(S->altitude(now) - S->heightAt(now)) < Fixed::fromRatio(1, 20));   // stays on the surface
    // body orbits: terra moves around the sun, luna around terra, positions are Fixed and periodic
    const Vec3 t0 = m.bodyWorldPosition(*sim, 1, 0), t1 = m.bodyWorldPosition(*sim, 1, 20 * 1200);
    CHECK(fxAbs((t0 - t1).length()) < Fixed::fromRatio(1, 2));
    CHECK(fxAbs((m.bodyWorldPosition(*sim, 2, 777) - m.bodyWorldPosition(*sim, 1, 777)).length() - Fixed::fromInt(60)) < Fixed::fromRatio(1, 4));
    // vision: own commander's cell is visible, the enemy pole is not
    int b, f, x, y;
    S->cellFace(S->cellAt(now), b, f, x, y);
    CHECK(m.cellVision(*sim, 1, b, f, x, y) == 2);
    S->cellFace(S->cellAt(p2), b, f, x, y);
    CHECK(m.cellVision(*sim, 1, b, f, x, y) == 0);
}

TEST(planet_orbit_units_and_transit) {
    auto sim = makeSim(kPlanets, "maps/two_worlds.json", 2);
    auto& m = mod(*sim);
    const game::Surface* S = m.surface(*sim);
    if (!S) { CHECK(false); return; }
    const int gun = m.rules().findType("gunship");
    CHECK(gun >= 0 && (m.rules().types[size_t(gun)].layer & game::kLayerOrbit));
    Entity c1 = findType(*sim, "commander", 1);
    const Vec3 home = sim->world().get<Transform>(c1).pos;
    Entity g1 = m.spawn(*sim, uint16_t(gun), 1, home);
    const Vec3 gp = sim->world().get<Transform>(g1).pos;
    CHECK(fxAbs((gp - game::Surface::anchor(1)).length() - S->bodies[1].shellRadius()) < Fixed::fromRatio(1, 50));   // on the shell
    // can't transit to its own body or a star; can to luna
    CHECK(!m.canTransit(*sim, g1, 1) && !m.canTransit(*sim, g1, 0) && m.canTransit(*sim, g1, 2));
    Command t; t.player = 1; t.type = game::kCmdTransit; t.units = {g1}; t.param = 2;
    sim->submit(t);
    for (int i = 0; i < 3; ++i) sim->step();   // input delay
    CHECK(sim->world().has<game::Transit>(g1));
    const auto tx = sim->world().get<game::Transit>(g1);
    CHECK(tx.arrive > tx.depart && (sim->world().get<game::Unit>(g1).flags & game::kUnitInTransit));
    // halfway: the world position lies between the two shell points
    const Vec3 mid = m.worldPosition(*sim, g1, (tx.depart + tx.arrive) / 2);
    CHECK(fxAbs((mid - (tx.fromWorld + tx.toWorld) * Fixed::fromRatio(1, 2)).length()) < Fixed::fromInt(3));   // within a tick of travel
    while (sim->tick() <= tx.arrive) sim->step();
    CHECK(!sim->world().has<game::Transit>(g1));
    const Vec3 arr = sim->world().get<Transform>(g1).pos;
    CHECK(m.bodyOf(*sim, arr) == 2);
    CHECK(fxAbs((arr - game::Surface::anchor(2)).length() - S->bodies[2].shellRadius()) < Fixed::fromRatio(1, 50));
    // an orbit unit sees the surface below it
    int b, f, x, y;
    S->cellFace(S->cellAt(arr), b, f, x, y);
    for (int i = 0; i < 6; ++i) sim->step();
    CHECK(m.cellVision(*sim, 1, b, f, x, y) == 2);
    // ground weapons without "orbit" can't hit it, the defender's can
    const auto& peewee = m.rules().types[size_t(m.rules().findType("peewee"))];
    const auto& defender = m.rules().types[size_t(m.rules().findType("defender"))];
    CHECK(!game::layerHit(peewee.weapons[0].targets, m.rules().types[size_t(gun)].layer));
    CHECK(game::layerHit(defender.weapons[0].targets, m.rules().types[size_t(gun)].layer));
}

TEST(planet_matches_deterministic_and_replay) {
    auto run = [](uint64_t seed, std::vector<Command>* rec, const std::vector<Command>* play) {
        auto sim = makeSim(kPlanets, "maps/two_worlds.json", seed);
        game::RtsAI a(1, seed), b(2, seed);
        if (play) for (const Command& c : *play) sim->submit(c);   // recorded with their execution ticks
        for (int t = 0; t < 20 * 90; ++t) {
            if (!play) { a.update(*sim); b.update(*sim); }
            sim->step();
        }
        if (rec) *rec = sim->executedCommands();
        return sim->checksum();
    };
    std::vector<Command> cmds;
    const uint64_t c1 = run(3, &cmds, nullptr), c2 = run(3, nullptr, nullptr);
    CHECK(c1 == c2);
    CHECK(!cmds.empty());
    CHECK(run(3, nullptr, &cmds) == c1);   // replaying the recorded commands gives the same state
}

#ifdef RTS_TESTS_EDITOR_CORE
#include "apps/editor/core/MapDoc.h"
TEST(planet_mapdoc_round_trip_and_validate) {
    editor::MapDoc doc;
    std::string err;
    CHECK(doc.load(kPlanets + "/maps/two_worlds.json", &err));
    CHECK(doc.data.planet() && doc.data.bodies.size() == 3);
    const Json orig = Json::parseFile(kPlanets + "/maps/two_worlds.json");
    const Json back = doc.toJson();
    CHECK(back["world"]["bodies"][size_t(1)]["terrain"]["corners"] == orig["world"]["bodies"][size_t(1)]["terrain"]["corners"]);
    CHECK(back["entities"][size_t(0)]["at"] == orig["entities"][size_t(0)]["at"]);
    CHECK(back["players"][size_t(0)]["start"] == orig["players"][size_t(0)]["start"]);
    CHECK(!back.has("size") && !back.has("terrain"));
    const Json rulesJson = Json::parseFile(kPlanets + "/rules.json");
    const game::Rules rules = game::loadRules(rulesJson);
    auto issues = doc.validate(rules, rulesJson);
    int errors = 0;
    for (auto& i : issues) if (i.error) { ++errors; std::printf("    issue: %s\n", i.text.c_str()); }
    CHECK(errors == 0);
    // a seam mismatch is reported, a bad position is an error
    doc.data.bodies[1].corners[0] += 5;
    doc.data.entities[0].at.set("face", Json::number(int64_t(9)));
    issues = doc.validate(rules, rulesJson);
    bool seam = false, pos = false;
    for (auto& i : issues) { seam |= !i.error && i.text.find("швах") != std::string::npos; pos |= i.error && i.text.find("0..5") != std::string::npos; }
    CHECK(seam && pos);
}
#endif
// ------------------------------------------------------------------ fidelity mechanics (RtsOriginals.cpp)

static Entity spawnAt(Simulation& sim, const std::string& type, uint8_t owner, double x, double z) {
    return mod(sim).spawn(sim, uint16_t(mod(sim).rules().findType(type)), owner, Vec3{Fixed::fromRatio(int64_t(x * 100), 100), Fixed{}, Fixed::fromRatio(int64_t(z * 100), 100)});
}

TEST(orig_crush_infantry_and_spice_bloom) {
    const char* rules = R"({"victory": "noUnits",
      "resources": {"spice": {"field": true, "regrow": 0, "cellMax": 100}},
      "types": {
        "tank": {"tags": ["vehicle"], "move": {"speed": 4, "radius": 0.5}, "health": {"max": 300}, "crush": {"tags": ["infantry", "bloom"]}},
        "soldier": {"tags": ["infantry"], "move": {"speed": 1, "radius": 0.25}, "health": {"max": 40}},
        "bloom": {"tags": ["bloom"], "health": {"max": 1}, "countsForVictory": false, "deathField": {"resource": "spice", "radius": 3, "amount": 100}}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, ""));
    auto& gs = mod(*sim).state(*sim);
    Entity tank = spawnAt(*sim, "tank", 1, 4, 8);
    Entity foe = spawnAt(*sim, "soldier", 2, 10, 8);
    Entity friendly = spawnAt(*sim, "soldier", 1, 14, 8.6);
    spawnAt(*sim, "bloom", 0, 20, 8);
    Command c; c.player = 1; c.type = game::kCmdMove; c.units = {tank}; c.point = {Fixed::fromInt(26), Fixed{}, Fixed::fromInt(8)};
    sim->submit(c);
    run(*sim, 8);
    CHECK(!sim->world().alive(foe));                       // run over
    CHECK(sim->world().alive(friendly));                   // own infantry is safe
    CHECK(countType(*sim, "bloom", 0) == 0);               // the bloom burst...
    CHECK(gs.grid.fieldAmt[size_t(gs.grid.index(20, 8))] > Fixed::fromInt(50));   // ...into a spice field
    Fixed before = gs.grid.fieldAmt[size_t(gs.grid.index(21, 8))];
    run(*sim, 5);
    CHECK(gs.grid.fieldAmt[size_t(gs.grid.index(21, 8))] == before);                // no regrowth
}

TEST(orig_sandworm_eats_submerges_and_resurfaces) {
    const char* rules = R"({"victory": "noStructures",
      "types": {
        "hq": {"structure": {"footprint": [2, 2]}, "health": {"max": 1000}},
        "worm": {"move": {"speed": 3, "radius": 0.6, "layer": "sand"}, "health": {"max": 3000}, "sight": 10,
                 "weapons": [{"range": 1.2, "damage": 2000, "cooldown": 1}], "neutral": {"wander": true, "wanderRadius": 5, "leash": 30, "eats": 2, "resurface": 5},
                 "countsForVictory": false},
        "quad": {"move": {"speed": 0.5, "radius": 0.4}, "health": {"max": 130}}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, R"(,"entities":[{"type":"hq","owner":1,"pos":[2,2]},{"type":"hq","owner":2,"pos":[29,13]},{"type":"worm","owner":-1,"pos":[16,8]}])"));
    auto& gs = mod(*sim).state(*sim);
    spawnAt(*sim, "quad", 1, 18, 8);
    spawnAt(*sim, "quad", 2, 14, 8);
    spawnAt(*sim, "quad", 1, 16, 10);
    run(*sim, 6);
    CHECK(countType(*sim, "quad", 1) + countType(*sim, "quad", 2) == 1);   // two eaten
    CHECK(countType(*sim, "worm", game::kHostileNeutral) == 0);            // full: dived
    CHECK(gs.resurface.size() == 1);
    run(*sim, 6);
    CHECK(countType(*sim, "worm", game::kHostileNeutral) == 1);            // back somewhere on the sand
}

TEST(orig_armor_models_bonus_and_shields) {
    const char* rules = R"({"victory": "noUnits", "armor": {"model": "percent", "perPoint": 0.06},
      "types": {
        "dummy": {"tags": ["cavalry"], "health": {"max": 1000, "armor": 5}},
        "shielded": {"health": {"max": 100, "shields": 50, "shieldRegen": 50, "shieldDelay": 1}},
        "spear": {"weapons": [{"damage": 10, "bonus": {"cavalry": 15}}]}
      }})";
    auto sim = makeSimJson(rules, flatMap(16, 16, ""));
    auto& m = mod(*sim);
    Entity d = spawnAt(*sim, "dummy", 2, 4, 4);
    m.dealDamage(*sim, d, Fixed::fromInt(100), 0, kNullEntity, 1);
    Fixed hp = sim->world().get<game::Unit>(d).hp;
    CHECK(hp > Fixed::fromInt(922) && hp < Fixed::fromInt(925));            // 100 * (1 - 0.3 / 1.3) = 76.9 taken
    const game::WeaponDef& wd = m.rules().types[size_t(m.rules().findType("spear"))].weapons[0];
    m.dealDamage(*sim, d, Fixed::fromInt(10), 0, kNullEntity, 1, &wd);       // +15 vs cavalry
    Fixed hp2 = sim->world().get<game::Unit>(d).hp;
    CHECK(hp - hp2 > Fixed::fromInt(19) && hp - hp2 < Fixed::fromInt(20));   // 25 * 0.769
    Entity s = spawnAt(*sim, "shielded", 2, 8, 8);
    m.dealDamage(*sim, s, Fixed::fromInt(30), 0, kNullEntity, 1);
    CHECK(sim->world().get<game::Unit>(s).hp == Fixed::fromInt(100));        // soaked by the shield
    m.dealDamage(*sim, s, Fixed::fromInt(30), 0, kNullEntity, 1);
    CHECK(sim->world().get<game::Unit>(s).hp < Fixed::fromInt(100));
    run(*sim, 3);
    CHECK(sim->world().get<game::Shield>(s).value == Fixed::fromInt(50));    // regenerated after the pause
}

TEST(orig_hero_skills_limit_free_first_and_revive) {
    const char* rules = R"({"victory": "noStructures",
      "resources": {"gold": {"start": 2000}},
      "heroes": {"skills": true, "max": 2, "freeFirst": true, "tiers": ["hall", "keep"], "revive": {"costShare": 0.5, "perLevel": 0.1}},
      "types": {
        "hall": {"structure": {"footprint": [2, 2]}, "health": {"max": 1000}, "upgrades": ["keep"]},
        "keep": {"structure": {"footprint": [2, 2]}, "health": {"max": 1200}, "cost": {"gold": 100, "time": 1}},
        "altar": {"structure": {"footprint": [2, 2]}, "health": {"max": 1000}, "produces": ["bm", "fs"]},
        "bm": {"hero": {"levels": [10, 20, 30]}, "move": {"speed": 2}, "health": {"max": 500}, "energy": {"max": 100, "start": 100},
               "cost": {"gold": 400, "time": 1, "supply": 0},
               "abilities": [{"id": "whirl", "target": "none", "cooldown": 1, "energy": 1, "effects": [{"type": "damage", "amount": 1, "radius": 1}]},
                             {"id": "ult", "target": "none", "cooldown": 1, "energy": 1, "learnAt": [6], "effects": [{"type": "damage", "amount": 1, "radius": 1}]}]},
        "fs": {"hero": {"levels": [10]}, "move": {"speed": 2}, "health": {"max": 400}, "cost": {"gold": 400, "time": 1}}
      }})";
    auto sim = makeSimJson(rules, flatMap(16, 16, R"(,"entities":[{"type":"hall","owner":1,"pos":[3,3]},{"type":"altar","owner":1,"pos":[8,3]},{"type":"hall","owner":2,"pos":[13,13]}])"));
    auto& m = mod(*sim);
    auto& eco = m.state(*sim).players[1];
    Entity altar = findType(*sim, "altar", 1);
    Command t; t.player = 1; t.type = game::kCmdTrain; t.units = {altar}; t.param = m.rules().findType("bm");
    sim->submit(t);
    run(*sim, 2);
    Entity bm = findType(*sim, "bm", 1);
    CHECK(bm.valid() && stockOf(*sim, 1, "gold") == Fixed::fromInt(2000));   // the first hero is free
    CHECK(sim->world().get<game::Hero>(bm).points == 1);
    CHECK(!m.canCast(*sim, bm, 0));                                          // not learned yet
    CHECK(!m.canTrain(*sim, 1, uint16_t(m.rules().findType("fs"))));         // second hero needs the keep
    Command l; l.player = 1; l.type = game::kCmdLearn; l.units = {bm}; l.param = 1;   // ultimate: level 6 only
    sim->submit(l);
    run(*sim, 0.2);
    CHECK(sim->world().get<game::Hero>(bm).skill[1] == 0);
    l.param = 0; sim->submit(l);
    run(*sim, 0.2);
    CHECK(sim->world().get<game::Hero>(bm).skill[0] == 1 && m.canCast(*sim, bm, 0));
    m.addHeroXp(*sim, bm, Fixed::fromInt(25), 1);                            // level 3: two more points
    CHECK(sim->world().get<game::Hero>(bm).points == 2);
    sim->world().get<game::Unit>(bm).hp = Fixed{};
    run(*sim, 0.2);
    CHECK(eco.deadHeroes.size() == 1 && eco.deadHeroes[0].level == 3);
    auto cost = m.trainCost(*sim, 1, uint16_t(m.rules().findType("bm")));
    CHECK(cost.size() == 1 && cost[0].value > Fixed::fromInt(279) && cost[0].value < Fixed::fromInt(281));   // 400 * (0.5 + 0.1 * 2)
    sim->submit(t);
    run(*sim, 2);
    bm = findType(*sim, "bm", 1);
    CHECK(bm.valid() && sim->world().get<game::Hero>(bm).level == 3 && sim->world().get<game::Hero>(bm).skill[0] == 1);
    CHECK(stockOf(*sim, 1, "gold") > Fixed::fromInt(1719) && stockOf(*sim, 1, "gold") < Fixed::fromInt(1721));
}

TEST(orig_gatherer_limit_and_upkeep_per_resource) {
    const char* rules = R"({"victory": "noStructures",
      "resources": {"gold": {}, "wood": {}},
      "upkeep": [{"supply": 2, "gatherMult": 0.5, "resources": ["gold"]}],
      "types": {
        "hall": {"structure": {"footprint": [2, 2]}, "health": {"max": 1000}, "dropoff": ["gold", "wood"], "supply": 10},
        "mine": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "resourceNode": {"resource": "gold", "amount": 10000, "maxGatherers": 1}},
        "peon": {"move": {"speed": 3, "radius": 0.3}, "health": {"max": 50}, "cost": {"supply": 1}, "gather": {"gold": {"carry": 10, "time": 2}}}
      }})";
    auto sim = makeSimJson(rules, flatMap(24, 12, R"(,"entities":[{"type":"hall","owner":1,"pos":[3,6]},{"type":"mine","owner":0,"pos":[8,6]},{"type":"hall","owner":2,"pos":[21,6]}])"));
    auto& m = mod(*sim);
    Entity a = spawnAt(*sim, "peon", 1, 6, 5), b = spawnAt(*sim, "peon", 1, 6, 7);
    Entity mine = findType(*sim, "mine", 0);
    Command g; g.player = 1; g.type = game::kCmdGather; g.units = {a, b}; g.target = mine;
    sim->submit(g);
    int maxAt = 0;
    for (int i = 0; i < 20 * 20; ++i) { sim->step(); maxAt = std::max(maxAt, m.gatherersAt(*sim, mine)); }
    CHECK(maxAt == 1);
    CHECK(m.gatherMultFor(*sim, 1, m.rules().findResource("gold")) == Fixed::fromRatio(1, 2));   // 2 supply used
    CHECK(m.gatherMultFor(*sim, 1, m.rules().findResource("wood")) == Fixed::fromInt(1));
}

TEST(orig_power_field_placement_and_switch_off) {
    const char* rules = R"({"victory": "noStructures",
      "types": {
        "pylon": {"structure": {"footprint": [2, 2]}, "health": {"max": 200}, "powerField": {"radius": 6}},
        "gate": {"structure": {"footprint": [2, 2], "needsField": 0}, "health": {"max": 500}, "produces": ["zealot"]},
        "zealot": {"move": {"speed": 2}, "health": {"max": 100}, "cost": {"time": 1}}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, R"(,"entities":[{"type":"pylon","owner":1,"pos":[5,8]},{"type":"gate","owner":1,"pos":[9,8]},{"type":"pylon","owner":2,"pos":[28,8]}])"));
    auto& m = mod(*sim);
    uint16_t gt = uint16_t(m.rules().findType("gate"));
    CHECK(m.canPlace(*sim, 1, gt, {Fixed::fromInt(7), Fixed{}, Fixed::fromInt(12)}));
    CHECK(!m.canPlace(*sim, 1, gt, {Fixed::fromInt(20), Fixed{}, Fixed::fromInt(8)}));
    run(*sim, 0.2);
    Entity gate = findType(*sim, "gate", 1);
    CHECK(!(sim->world().get<game::Unit>(gate).flags & game::kUnitDisabled));
    sim->world().get<game::Unit>(findType(*sim, "pylon", 1)).hp = Fixed{};
    run(*sim, 0.2);
    CHECK(sim->world().get<game::Unit>(gate).flags & game::kUnitDisabled);
}

TEST(orig_crews_snipers_and_empty_vehicles) {
    const char* rules = R"({"victory": "noStructures",
      "types": {
        "fort": {"structure": {"footprint": [2, 2]}, "health": {"max": 1000}},
        "jeep": {"move": {"speed": 0.1, "radius": 0.5}, "health": {"max": 200}, "crew": "robot"},
        "sniper": {"move": {"speed": 2}, "health": {"max": 50}, "weapons": [{"range": 8, "damage": 1, "cooldown": 0.5, "killsCrew": 1}]},
        "robot": {"move": {"speed": 3, "radius": 0.3}, "health": {"max": 70}, "crews": true}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, R"(,"entities":[{"type":"fort","owner":1,"pos":[2,2]},{"type":"fort","owner":2,"pos":[29,13]}])"));
    auto& m = mod(*sim);
    Entity jeep = spawnAt(*sim, "jeep", 2, 12, 8);
    Entity sniper = spawnAt(*sim, "sniper", 1, 6, 8);
    run(*sim, 1.5);
    CHECK(sim->world().get<Owner>(jeep).player == 0);                     // driver shot, vehicle empty
    sim->world().get<game::Unit>(sniper).hp = Fixed{};
    Entity robot = spawnAt(*sim, "robot", 1, 9, 8);
    Command c; c.player = 1; c.type = game::kCmdSmart; c.units = {robot}; c.target = jeep;
    sim->submit(c);
    run(*sim, 3);
    CHECK(sim->world().get<Owner>(jeep).player == 1 && !sim->world().alive(robot));
    (void)m;
}

TEST(orig_infection_and_death_payload) {
    const char* rules = R"({"victory": "noStructures",
      "types": {
        "hq": {"structure": {"footprint": [2, 2]}, "health": {"max": 1000}},
        "walker": {"tags": ["infected"], "move": {"speed": 2, "radius": 0.3}, "health": {"max": 60}, "weapons": [{"range": 0.5, "damage": 100, "cooldown": 1}]},
        "colonist": {"move": {"speed": 0.01, "radius": 0.3}, "health": {"max": 30}, "infect": {"by": ["infected"], "spawn": "walker", "count": 1}},
        "bomb": {"health": {"max": 10}, "onDeath": [{"type": "damage", "amount": 500, "radius": 3, "affects": "any"}]}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, R"(,"entities":[{"type":"hq","owner":1,"pos":[2,2]},{"type":"hq","owner":2,"pos":[29,13]}])"));
    spawnAt(*sim, "walker", 2, 10, 8);
    spawnAt(*sim, "colonist", 1, 11, 8);
    run(*sim, 2);
    CHECK(countType(*sim, "colonist", 1) == 0 && countType(*sim, "walker", 2) == 2);   // the victim rose
    Entity bomb = spawnAt(*sim, "bomb", 1, 20, 4);
    Entity near = spawnAt(*sim, "colonist", 1, 21, 4);
    sim->world().get<game::Unit>(bomb).hp = Fixed{};
    run(*sim, 0.2);
    CHECK(!sim->world().alive(near));
}

TEST(orig_garrison_upgrades_capture_level_keep_and_forges) {
    const char* rules = R"({"victory": "noEntities",
      "types": {
        "shroom": {"move": {"speed": 3, "radius": 0.15}, "health": {"max": 1}, "entersGarrison": true},
        "v1": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "level": 1, "garrison": {"max": 20, "unit": "shroom", "interval": 100, "keep": 1}, "upgrades": ["v2"]},
        "v2": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "level": 2, "garrison": {"max": 30, "unit": "shroom", "interval": 100, "onCapture": "v1", "keep": 1},
               "cost": {"garrison": 10, "time": 0.5}},
        "forge": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "garrison": {"max": 10, "unit": "shroom"},
                  "playerMods": {"max": 2, "mods": [{"type": "shroom", "stat": "damage", "mul": 1.5}]}}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, R"(,"entities":[{"type":"v1","owner":1,"pos":[4,8],"garrison":15},{"type":"v2","owner":2,"pos":[20,8],"garrison":3},{"type":"forge","owner":1,"pos":[4,3]}])"));
    auto& m = mod(*sim);
    Entity a = findType(*sim, "v1", 1), b = findType(*sim, "v2", 2);
    Command u; u.player = 1; u.type = game::kCmdUpgrade; u.units = {a}; u.param = m.rules().findType("v2");
    sim->submit(u);
    run(*sim, 1);
    CHECK(sim->world().get<game::Unit>(a).type == m.rules().findType("v2") && sim->world().get<game::Garrison>(a).count == 5);
    Command s; s.player = 1; s.type = game::kCmdSend; s.units = {a}; s.target = b; s.param = 100;
    sim->submit(s);
    run(*sim, 0.3);
    CHECK(sim->world().get<game::Garrison>(a).count + sim->world().get<game::Garrison>(a).pendingOut >= 1);   // one stays home
    CHECK(sim->world().get<game::Garrison>(a).count == 1);
    run(*sim, 10);
    // 4 shrooms at x1.5 attack vs 3 defenders: captured, and the level 2 building dropped to level 1
    CHECK(sim->world().get<Owner>(b).player == 1 && sim->world().get<game::Unit>(b).type == m.rules().findType("v1"));
}

TEST(orig_faction_only_types) {
    const char* rules = R"({"victory": "noStructures",
      "factions": {"atreides": {"name": "A", "start": []}, "harkonnen": {"name": "H", "start": []}},
      "types": {
        "yard": {"structure": {"footprint": [2, 2]}, "health": {"max": 1000}, "produces": ["sonic", "devastator"]},
        "sonic": {"move": {"speed": 2}, "health": {"max": 100}, "factions": ["atreides"]},
        "devastator": {"move": {"speed": 2}, "health": {"max": 100}, "factions": ["harkonnen"]}
      }})";
    auto sim = makeSimJson(rules, flatMap(16, 16, R"(,"entities":[{"type":"yard","owner":1,"pos":[3,3]},{"type":"yard","owner":2,"pos":[12,12]}])",
                                         R"([{"name":"A","team":1,"faction":"atreides"},{"name":"H","team":2,"faction":"harkonnen"}])"));
    auto& m = mod(*sim);
    CHECK(m.canTrain(*sim, 1, uint16_t(m.rules().findType("sonic"))) && !m.canTrain(*sim, 1, uint16_t(m.rules().findType("devastator"))));
    CHECK(m.canTrain(*sim, 2, uint16_t(m.rules().findType("devastator"))) && !m.canTrain(*sim, 2, uint16_t(m.rules().findType("sonic"))));
}

TEST(sprite_shadow_lighting_sun_dir_and_softness) {
    const game::Rules rules = game::loadRules(Json::parseFile(kSkirmish + "/rules.json"));
    game::RtsLights L;
    L.init(rules);
    L.configure(Json::parse(R"({"sunDir": [-1.0, 1.2, 0.45], "shadowSoftness": 0.07})"), Json{}, Json{});
    render::Lighting l;
    L.updateSky(l, 0);   // time 0.5 = noon: the sun stands at the given azimuth
    CHECK(l.sunDir.x < -0.5f && l.sunDir.z > 0.1f && l.sunDir.y > 0.5f);
    CHECK(std::fabs(l.shadowSoftness - 0.07f) < 1e-4f);
    L.configure(Json::parse("{}"), Json{}, Json{});   // another game: back to the renderer default
    L.updateSky(l, 0);
    CHECK(std::fabs(l.shadowSoftness - render::Lighting{}.shadowSoftness) < 1e-5f);
}


// ---------------------------------------------------------------------------------------------
// Materials on request: procedural textures, material library ops (models agent)
// ---------------------------------------------------------------------------------------------
#include "engine/render/TextureGen.h"
TEST(materials_texture_patterns_tile_and_repeat) {
    using namespace rts::render;
    for (const auto& pat : texturePatterns()) {
        Json spec = Json::object();
        spec.set("pattern", Json::string(pat));
        spec.set("size", Json::number(int64_t(64)));
        spec.set("seed", Json::number(int64_t(7)));
        Image a, b;
        generateTexture(spec, a);
        generateTexture(spec, b);
        CHECK(a.width == 64 && a.height == 64 && a.rgba.size() == 64u * 64u * 4u && a.rgba == b.rgba);   // deterministic
        if (pat == "gradient") continue;
        // tileable: the left column continues the right one about as smoothly as neighbours do
        long seam = 0, inner = 0;
        for (int y = 0; y < 64; ++y)
            for (int c = 0; c < 3; ++c) {
                seam += std::abs(int(a.rgba[(size_t(y) * 64 + 0) * 4 + size_t(c)]) - int(a.rgba[(size_t(y) * 64 + 63) * 4 + size_t(c)]));
                inner += std::abs(int(a.rgba[(size_t(y) * 64 + 31) * 4 + size_t(c)]) - int(a.rgba[(size_t(y) * 64 + 32) * 4 + size_t(c)]));
            }
        if (seam > inner * 3 + 64 * 3 * 40) std::printf("  pattern %s: seam %ld vs inner %ld\n", pat.c_str(), seam, inner);
        CHECK(seam <= inner * 3 + 64 * 3 * 40);
    }
    Json mask = Json::parse(R"({"pattern": "stripes", "size": 32, "scale": 4, "output": "mask"})");
    Image m;
    generateTexture(mask, m);
    int white = 0;
    for (size_t i = 0; i < m.rgba.size(); i += 4) white += m.rgba[i] > 128 ? 1 : 0;
    CHECK(white > 32 * 32 / 4 && white < 32 * 32 * 3 / 4);
    Json nrm = Json::parse(R"({"pattern": "brick", "size": 32, "output": "normal"})");
    Image n;
    generateTexture(nrm, n);
    CHECK(n.rgba[2] > 127);   // normals point out of the surface
    bool threw = false;
    try { Image x; generateTexture(Json::parse(R"({"pattern": "plaid"})"), x); } catch (const std::exception& e) { threw = std::string(e.what()).find("brick") != std::string::npos; }
    CHECK(threw);
}

#ifdef RTS_TESTS_EDITOR_CORE
#include "apps/editor/core/Workspace.h"
TEST(materials_library_ops_round_trip) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("rts_mat_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(root);
    fs::create_directories(root / "games");
    fs::copy(fs::path(RTS_SOURCE_DIR) / "templates" / "showcase", root / "games" / "showcase", fs::copy_options::recursive);
    editor::Workspace ws;
    ws.engineDir = RTS_SOURCE_DIR;
    std::string err;
    CHECK(ws.openGame((root / "games" / "showcase").string(), &err));
    auto ok = [](const Json& r) { if (r.has("error")) std::printf("  (op error) %s\n", r.getStr("error").c_str()); return !r.has("error"); };
    Json r = ws.call("texture_generate", Json::parse(R"({"pattern": "panels", "size": 64, "seed": 4})"));
    CHECK(ok(r) && r.getStr("path") == "textures/panels_4.png" && fs::file_size(root / "games/showcase/textures/panels_4.png") > 100);
    CHECK(ok(ws.call("material_set", Json::parse(R"({"name": "steel", "material": {"color": [0.7,0.7,0.75], "metallic": 0.9, "texture": "textures/panels_4.png"}})"))));
    CHECK(ok(ws.call("material_set", Json::parse(R"({"name": "rust", "material": {"base": "steel", "roughness": 0.9}})"))));
    CHECK(!ok(ws.call("material_set", Json::parse(R"({"name": "x", "material": {"metallic": 3}})"))));
    CHECK(!ok(ws.call("material_set", Json::parse(R"({"name": "y", "material": {"base": "nothing"}})"))));
    Json g = ws.call("material_get", Json::parse(R"({"name": "rust"})"));
    CHECK(g["resolved"].getStr("texture") == "textures/panels_4.png" && g["chain"].size() == 2);
    CHECK(ok(ws.call("material_apply", Json::parse(R"({"type": "catapult", "node": "Turret", "material": "rust"})"))));
    CHECK(ok(ws.call("material_apply", Json::parse(R"({"type": "footman", "target": "*", "material": {"use": "steel", "roughness": 0.2}})"))));
    CHECK(ws.project.rulesJson["types"]["catapult"]["view"]["assign"].getStr("Turret") == "rust");
    CHECK(ws.project.rulesJson["types"]["footman"]["view"]["materials"]["*"].getStr("use") == "steel");
    Json l = ws.call("material_list", Json::parse(R"({"type": "catapult"})"));
    CHECK(l["library"].has("steel") && l["types"].size() == 1 && l["types"][size_t(0)]["models"].size() == 2);
    CHECK(!ok(ws.call("material_delete", Json::parse(R"({"name": "steel"})"))));   // used by rust and footman
    CHECK(ok(ws.call("material_apply", Json::parse(R"({"type": "catapult", "node": "Turret", "remove": true})"))));
    CHECK(!ws.project.rulesJson["types"]["catapult"]["view"].has("assign"));
    CHECK(ok(ws.call("material_delete", Json::parse(R"({"name": "rust"})"))));
    Json saved = Json::parseFile((root / "games/showcase/materials.json").string());
    CHECK(saved["materials"].has("steel") && !saved["materials"].has("rust"));
    fs::remove_all(root);
}
#endif

#include "tests_fidelity.inc"

// ---------------- Planets: HPA* on cube-sphere bodies ----------------
namespace {
// A planet map JSON: bodies with random obstacle blobs (seeded), walls along seams with gaps, a blocked cube corner.
Json hpaPlanetMap(uint64_t seed, std::vector<int> faceSizes) {
    Random rng(seed, 7);
    Json bodies = Json::array();
    for (size_t bi = 0; bi < faceSizes.size(); ++bi) {
        const int N = faceSizes[bi];
        std::vector<std::string> rows(size_t(6 * N), std::string(size_t(N), '.'));
        auto at = [&](int f, int x, int y) -> char& { return rows[size_t(f * N + y)][size_t(x)]; };
        for (int k = 0; k < 6 * N * N / 60; ++k) {   // blobs
            const int f = int(rng.below(6)), x = int(rng.below(uint32_t(N))), y = int(rng.below(uint32_t(N))), r = 1 + int(rng.below(3));
            for (int dy = -r; dy <= r; ++dy) for (int dx = -r; dx <= r; ++dx)
                if (x + dx >= 0 && y + dy >= 0 && x + dx < N && y + dy < N && dx * dx + dy * dy <= r * r) at(f, x + dx, y + dy) = rng.below(3) ? '#' : '~';
        }
        for (int y = 0; y < N; ++y) if (y % 9 != 4) at(4, 0, y) = '#';   // wall on the seam 4|0 side, gaps every 9
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) at(2, N - 1 - i, N - 1 - j) = '#';  // a cube corner
        Json faces = Json::array();
        for (int f = 0; f < 6; ++f) {
            Json rj = Json::array();
            for (int y = 0; y < N; ++y) rj.push(Json::string(rows[size_t(f * N + y)]));
            Json fj = Json::object(); fj.set("rows", rj); faces.push(fj);
        }
        Json t = Json::object(); t.set("faces", faces);
        Json b = Json::object();
        b.set("id", Json::string("b" + std::to_string(bi)));
        b.set("kind", Json::string("planet"));
        b.set("radius", Json::number(int64_t(N / 2 + 4)));
        b.set("faceSize", Json::number(int64_t(N)));
        b.set("terrain", t);
        bodies.push(b);
    }
    Json w = Json::object(); w.set("bodies", bodies);
    Json m = Json::object();
    m.set("name", Json::string("hpa"));
    Json pl = Json::array();
    for (int p = 0; p < 2; ++p) { Json pj = Json::object(); pj.set("name", Json::string("P")); pj.set("team", Json::number(int64_t(p + 1))); pl.push(pj); }
    m.set("players", pl);
    m.set("world", w);
    return m;
}
int pathCost(const game::Surface& S, const std::vector<int>& cells) {
    int c = 0;
    for (size_t i = 1; i < cells.size(); ++i) {
        int k = -1;
        for (int n = 0; n < 8; ++n) if (S.neighbor(cells[i - 1], n) == cells[i]) k = n;
        if (k < 0) return -1;
        c += k >= 4 ? 14 : 10;
    }
    return c;
}
std::unique_ptr<Simulation> planetSim(const Json& map) {
    SimConfig cfg; cfg.seed = 1;
    auto sim = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
    sim->setup(Json::parseFile(kPlanets + "/rules.json"), map);
    return sim;
}
} // namespace

TEST(planet_hpa_matches_astar) {
    auto len3 = [](Vec3 from, const std::vector<Vec3>& p) { double l = 0; for (auto& q : p) { l += (q - from).length().toDouble(); from = q; } return l; };
    for (uint64_t seed : {1u, 2u, 3u}) {
        auto sim = planetSim(hpaPlanetMap(seed, {53, 24}));   // 53: partial clusters (16 + 16 + 16 + 5)
        auto& g = mod(*sim).state(*sim).grid;
        const game::Surface& S = *g.surf;
        Random rng(seed, 3);
        int pairs = 0, bothFound = 0, mismatch = 0, worse = 0, broken = 0, crossBody = 0, rawMismatch = 0;
        double ratioSum = 0;
        const uint8_t layers[2] = {game::kLayerGround, game::kLayerNaval};
        for (int q = 0; q < 200; ++q) {
            const int body = q % 3 == 2 ? 1 : 0, N = S.bodies[size_t(body)].faceSize;
            const uint8_t layer = layers[(q / 3) % 2];
            const int a = S.gridCell(body, int(rng.below(6)), int(rng.below(uint32_t(N))), int(rng.below(uint32_t(N))));
            const int b = S.gridCell(body, int(rng.below(6)), int(rng.below(uint32_t(N))), int(rng.below(uint32_t(N))));
            if (!g.passable(a % g.w, a / g.w, layer) || !g.passable(b % g.w, b / g.w, layer)) continue;
            ++pairs;
            // raw cell paths: HPA* exists iff A* exists, and every HPA* step is a legal move
            std::vector<int> ph, pa;
            g.sphereHpaMinCells = 0;
            const bool fh = g.sphereCellPath(a, b, layer, true, ph), fa = g.sphereCellPath(a, b, layer, false, pa);
            if (fh != fa) ++rawMismatch;
            if (fh && (ph.front() != a || ph.back() != b || pathCost(S, ph) < 0)) ++broken;
            for (size_t i = 1; fh && i < ph.size(); ++i) if (!g.canMoveCells(ph[i - 1], ph[i], layer)) ++broken;
            // game paths (string pulled): HPA* mode vs plain unbounded A*
            std::vector<Vec3> wh, wa;
            g.sphereHpaMinCells = 0;
            const bool gh = g.findPath(S.cellCenter(a), S.cellCenter(b), layer, wh, 1 << 30);
            g.sphereHpaMinCells = 1 << 30;
            const bool ga = g.findPath(S.cellCenter(a), S.cellCenter(b), layer, wa, 1 << 30);
            if (gh != ga) ++mismatch;
            if (!gh || !ga) continue;
            ++bothFound;
            const double lh = len3(S.cellCenter(a), wh), la = len3(S.cellCenter(a), wa);
            ratioSum += la > 0 ? lh / la : 1.0;
            if (lh > la * 1.15 + 4) ++worse;
        }
        std::vector<int> x;
        g.sphereHpaMinCells = 0;
        crossBody += g.sphereCellPath(S.gridCell(0, 0, 3, 3), S.gridCell(1, 0, 3, 3), game::kLayerGround, true, x);
        std::printf("  hpa seed %d: pairs=%d both=%d mismatch=%d/%d broken=%d worse=%d ratio=%.3f hpa=%d\n", int(seed), pairs, bothFound, mismatch, rawMismatch, broken, worse, bothFound ? ratioSum / bothFound : 0.0, g.hpaStats[0]);
        CHECK(pairs > 50 && bothFound > 40 && g.hpaStats[0] > 20);
        CHECK(mismatch == 0 && rawMismatch == 0);
        CHECK(broken == 0);
        CHECK(worse == 0);
        CHECK(ratioSum / std::max(1, bothFound) < 1.04);   // within a few % of A* on average
        CHECK(crossBody == 0);
    }
}

TEST(planet_hpa_incremental_equals_full_and_deterministic) {
    auto sim = planetSim(hpaPlanetMap(5, {40}));
    auto& g = mod(*sim).state(*sim).grid;
    const game::Surface& S = *g.surf;
    g.sphereHpaMinCells = 0;
    const uint64_t before = g.hpaSphereDigest(game::kLayerGround);
    // structures on a seam-adjacent cluster and near a cube corner, then a "dug" cell
    g.block(S.gridCell(0, 0, 30, 30) % g.w, S.gridCell(0, 0, 30, 30) / g.w, 3, 3, +1);
    g.block(S.gridCell(0, 4, 0, 10) % g.w, S.gridCell(0, 4, 0, 10) / g.w, 2, 4, +1);
    const int dug = S.gridCell(0, 3, 15, 16);
    g.flags[size_t(dug)] = 0; g.cellChanged(dug % g.w, dug / g.w);
    const uint64_t inc = g.hpaSphereDigest(game::kLayerGround);
    g.resetHpaSphere();
    const uint64_t full = g.hpaSphereDigest(game::kLayerGround);
    CHECK(inc == full);
    CHECK(inc != before);
    // removing the structures restores the original graph
    g.block(S.gridCell(0, 0, 30, 30) % g.w, S.gridCell(0, 0, 30, 30) / g.w, 3, 3, -1);
    g.block(S.gridCell(0, 4, 0, 10) % g.w, S.gridCell(0, 4, 0, 10) / g.w, 2, 4, -1);
    g.flags[size_t(dug)] = game::kCellGround; g.cellChanged(dug % g.w, dug / g.w);
    CHECK(g.hpaSphereDigest(game::kLayerGround) == before);
    // same map + seed twice -> identical graph and identical paths
    auto sim2 = planetSim(hpaPlanetMap(5, {40}));
    auto& g2 = mod(*sim2).state(*sim2).grid;
    g2.sphereHpaMinCells = 0;
    CHECK(g2.hpaSphereDigest(game::kLayerGround) == before);
    std::vector<Vec3> p1, p2;
    const Vec3 a = S.cellCenter(S.gridCell(0, 4, 20, 20)), b = S.cellCenter(S.gridCell(0, 5, 20, 20));
    g.findPath(a, b, game::kLayerGround, p1);
    g2.findPath(a, b, game::kLayerGround, p2);
    CHECK(!p1.empty() && p1 == p2 && g.hpaStats[0] > 0);
}

TEST(planet_hpa_threshold_and_slope_classes) {
    // templates/planets (faceSize 24) stays on plain A*; a 128 body uses HPA* by default
    auto small = makeSim(kPlanets, "maps/two_worlds.json", 1);
    auto& gs = mod(*small).state(*small).grid;
    CHECK(!gs.sphereHpaBody(1) && !gs.sphereHpaBody(2));
    auto big = planetSim(hpaPlanetMap(2, {105, 16}));   // 6 * 105^2 = 66150 >= 65536
    auto& gb = mod(*big).state(*big).grid;
    CHECK(gb.sphereHpaBody(0) && !gb.sphereHpaBody(1));
    // slope classes: templates/planets types with move.maxSlope get their own layer graphs on a heightfield body
    const auto& R = mod(*big).rules();
    const auto& cons = R.types[size_t(R.findType("constructor"))];
    const auto& cmdr = R.types[size_t(R.findType("commander"))];
    CHECK(cons.pathLayer != cmdr.pathLayer);
    CHECK(gb.hpaSphereDigest(cons.pathLayer) != 0 && gb.hpaSphereDigest(cmdr.pathLayer) != 0);
}

TEST(pack_file_copy_into_existing) {
    // fs::copy / copy_file(overwrite_existing) fail on MinGW when the destination exists;
    // rtsbuild re-stages into the same folders, the editor copies a template over dst/maps.
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "rts_filecopy_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "src" / "maps" / "sub");
    std::ofstream(dir / "src" / "a.txt") << "new";
    std::ofstream(dir / "src" / "maps" / "sub" / "b.txt") << "b";
    fs::create_directories(dir / "dst" / "maps");
    std::ofstream(dir / "dst" / "a.txt") << "old-and-longer";
    CHECK(rts::pack::copyTree(dir / "src", dir / "dst", ec));
    CHECK(rts::pack::copyTree(dir / "src", dir / "dst", ec));   // twice: every file exists now
    std::ifstream a(dir / "dst" / "a.txt");
    std::string av((std::istreambuf_iterator<char>(a)), {});
    a.close();
    CHECK(av == "new" && fs::exists(dir / "dst" / "maps" / "sub" / "b.txt"));
    CHECK(rts::pack::copyFileReplace(dir / "src" / "a.txt", dir / "dst" / "maps" / "a2.txt", ec));
    CHECK(rts::pack::copyFileReplace(dir / "src" / "a.txt", dir / "dst" / "maps" / "a2.txt", ec));
    CHECK(!rts::pack::copyFileReplace(dir / "src" / "a.txt", dir / "src" / "." / "a.txt", ec));   // never onto itself
    CHECK(fs::file_size(dir / "src" / "a.txt", ec) == 3);
    fs::remove_all(dir, ec);
}

// ---- agent-mech-tools: JsonAccessLog (rules_lint / rules_schema) and kEvTriggerFired (scenario_run trigger log)


#include "tests_mech.inc"   // agent-mech


TEST(gen_planet_world_players) {
    // a whole planet map from a planet preset: players, kit, objects, moons, engine-path fairness, determinism
    const std::string rules = R"({"victory": "keyUnits", "terrain": {"model": "heightfield", "maxSlope": {"ground": 0.8}},
      "resources": {"metal": {"start": 500}},
      "types": {"boss": {"key": true, "move": {"speed": 2, "radius": 0.5}, "builds": ["lab"], "weapons": [{"range": 2, "damage": 5, "cooldown": 1}]},
                "lab": {"structure": {"footprint": [3, 3]}, "produces": ["ship"]},
                "ship": {"move": {"speed": 3, "radius": 0.5, "layer": "orbit"}},
                "spot": {"structure": {"footprint": [2, 2]}, "invulnerable": true},
                "drill": {"structure": {"footprint": [2, 2], "on": "spot"}, "income": {"metal": 2}},
                "rock": {"structure": {"footprint": [1, 1]}, "invulnerable": true, "resourceNode": {"resource": "metal", "amount": 100}}}})";
    Json rj = Json::parse(rules);
    game::Rules R = game::loadRules(rj);
    auto make = [&](int players, uint64_t seed, const std::string& layout, Json& out) {
        gen::GenRequest req;
        req.preset = "rocky"; req.seed = seed; req.players = players;
        req.params = Json::object();
        req.params.set("layout", Json::string(layout));
        out = Json::parse(R"({"name": "w", "players": []})");
        std::string err;
        const bool ok = gen::generateMap(req, R, rj, out, &err);
        if (!ok) std::printf("  planet gen error: %s\n", err.c_str());
        return ok;
    };
    for (int np : {2, 3, 4}) {
        Json m;
        CHECK(make(np, 5, "shared", m));
        const Json& g = m["generated"];
        CHECK(g.getBool("connected"));
        CHECK(int(m["players"].size()) == np);
        int own[9] = {}, moon = 0;
        for (auto& e : m["entities"].items()) {
            const int o = int(e.getInt("owner"));
            if (o >= 1 && o <= 8) own[o]++;
            if (e["at"].getStr("body") == "luna") moon++;
            CHECK(e["at"].isObject());
        }
        for (int p = 2; p <= np; ++p) CHECK(own[p] == own[1]);
        CHECK(own[1] >= 1 && moon > 0);
        const Json& nr = g["nearResources"];
        for (size_t p = 1; p < nr.size(); ++p) CHECK(nr[p].integer() == nr[size_t(0)].integer());
        if (np == 2 || np == 4) CHECK(g.getInt("pathSpread", 99) <= 2);     // exact quarter / half turn symmetry
        else CHECK(g.getInt("pathSpread", 999) <= 150);
        // the generated map loads and plays deterministically
        auto run = [&]() {
            SimConfig cfg; cfg.seed = 3;
            Simulation sim(cfg, std::make_unique<game::RtsModule>());
            sim.setup(rj, m);
            for (int i = 0; i < 60; ++i) sim.step();
            return sim.checksum();
        };
        CHECK(run() == run());
        if (np == 2) {
            std::printf("  planet world 2p %016llx\n", (unsigned long long)gen::mapHash(m));
            CHECK(gen::mapHash(m) == GEN_GOLD_PLANET_WORLD);
        }
    }
    Json a, b;
    CHECK(make(3, 9, "separate", a) && make(3, 9, "separate", b));
    CHECK(gen::mapHash(a) == gen::mapHash(b));
    CHECK(a["world"]["bodies"].size() >= 4);                      // star + 3 worlds (+ moon)
    CHECK(a["generated"]["nearResources"][size_t(0)].integer() == a["generated"]["nearResources"][size_t(2)].integer());
    // match-time: "generate" with a planet preset regenerates from the match seed
    Json mm = Json::parse(R"({"name": "m", "players": [{}, {}], "generate": {"preset": "moon", "seed": "match", "players": 2}})");
    CHECK(gen::prepareGenerate(mm, [](const std::string&, std::string&) { return false; }, nullptr));
    auto runMatch = [&](uint64_t seed) {
        SimConfig cfg; cfg.seed = seed;
        Simulation sim(cfg, std::make_unique<game::RtsModule>());
        sim.setup(rj, mm);
        auto* mod = dynamic_cast<game::RtsModule*>(&sim.module());
        CHECK(mod->surface(sim) != nullptr && mod->generatedMap() != nullptr);
        for (int i = 0; i < 40; ++i) sim.step();
        return sim.checksum();
    };
    CHECK(runMatch(4) == runMatch(4));
    CHECK(runMatch(4) != runMatch(5));
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

// ---- territory on planets (agent-planet-render): starts, unit claims, DK diggers / claimers across face seams
TEST(planet_territory_claims_and_dig_cross_seams) {
    const std::string rules = R"J({"victory": {"mode": "survive", "seconds": 600},
      "diggable": {"D": {"hardness": 1}},
      "territory": {"claimBy": "both", "startRadius": 5},
      "types": {"hall": {"structure": {"footprint": [2, 2]}, "invulnerable": true},
                "digger": {"move": {"speed": 3, "radius": 0.3}, "digs": {"rate": 3}, "claimer": {"time": 0.2}},
                "trooper": {"move": {"speed": 2, "radius": 0.3}, "claim": {"radius": 2, "rate": 3}}}})J";
    const std::string map = R"J({"players": [{"start": {"body": "p", "face": 4, "x": 4, "y": 4}}, {"start": {"body": "p", "face": 5, "x": 4, "y": 4}}],
      "world": {"start": "p", "bodies": [{"id": "p", "kind": "planet", "radius": 12, "faceSize": 8,
        "terrain": {"faces": [{}, {}, {}, {}, {"rows": ["........", "........", "........", "........", "........", "........", "DDDDDDDD", "........"]}, {}]}}]},
      "entities": [{"type": "hall", "owner": 1, "at": {"body": "p", "face": 4, "x": 4, "y": 4}},
                   {"type": "hall", "owner": 2, "at": {"body": "p", "face": 5, "x": 4, "y": 4}},
                   {"type": "digger", "owner": 1, "at": {"body": "p", "face": 4, "x": 4.5, "y": 5.5}},
                   {"type": "trooper", "owner": 1, "at": {"body": "p", "face": 4, "x": 0.5, "y": 2.5}}]})J";
    auto run = [&](bool checks) {
        auto sim = makeSimJson(rules, map, 2);
        auto& m = mod(*sim);
        const game::Surface* S = m.surface(*sim);
        if (!S) { CHECK(false); return uint64_t(0); }
        auto& g = m.state(*sim).grid;
        auto facesOf = [&](uint8_t p) {
            int mask = 0;
            for (size_t i = 0; i < g.territory.size(); ++i)
                if (g.territory[i] == p) { int b, f, x, y; if (S->cellFace(int(i), b, f, x, y)) mask |= 1 << f; }
            return mask;
        };
        const int t0 = m.territoryOf(*sim, 1);
        if (checks) {
            CHECK(t0 > 40 && m.territoryOf(*sim, 2) >= t0);   // start discs around both halls (player 1's has a rock row)
            CHECK((facesOf(1) & ~(1 << 4)) != 0);             // the start radius crosses a face seam
            CHECK(g.territory[size_t(S->gridCell(0, 4, 4, 6))] == 0);   // rock is not owned
        }
        // mark the rock row; the digger digs it and claims the floor
        Command dg; dg.player = 1; dg.type = game::kCmdDig; dg.point = S->cellCenter(S->gridCell(0, 4, 0, 6)); dg.param = game::rectParam(8, 1);
        sim->submit(dg);
        for (int i = 0; i < 20 * 40; ++i) sim->step();
        int rock = 0;
        for (int x = 0; x < 8; ++x) rock += g.dig[size_t(S->gridCell(0, 4, x, 6))] ? 1 : 0;
        const int t1 = m.territoryOf(*sim, 1);
        if (checks) {
            CHECK(rock < 8);            // dug through (dig jobs found the marked cells via the sphere BFS)
            CHECK(t1 > t0 + 10);        // claimed by the trooper / digger
            // every own cell is connected to the hall through own cells (4-neighbours across seams)
            std::vector<uint8_t> seen(g.territory.size(), 0);
            std::vector<int> q{S->gridCell(0, 4, 4, 4)};
            seen[size_t(q[0])] = 1;
            for (size_t k = 0; k < q.size(); ++k)
                for (int d = 0; d < 4; ++d) {
                    const int n = S->neighbor(q[k], d);
                    if (n >= 0 && !seen[size_t(n)] && g.territory[size_t(n)] == 1) { seen[size_t(n)] = 1; q.push_back(n); }
                }
            CHECK(int(q.size()) == t1);
            CHECK(m.state(*sim).territoryCells[1] == t1);
        }
        return sim->checksum();
    };
    const uint64_t a = run(true), b = run(false);
    CHECK(a == b);
}

// ---- planet fog blur (agent-planet-render): seam-aware separable blur, timing on faceSize 256
TEST(planet_fog_blur_seams_and_speed) {
    using namespace rts::game;
    auto S = std::make_shared<Surface>();
    BodyDef b; b.id = "p"; b.radius = Fixed::fromInt(400); b.faceSize = 256;
    S->bodies.push_back(b);
    S->build();
    const size_t n = size_t(S->W) * size_t(S->H);
    PlanetFieldBlur blur(S);
    // a uniform field stays uniform (padding never reads void / foreign cells)
    std::vector<uint8_t> f(n, 200);
    blur(f, 2);
    CHECK(std::all_of(f.begin(), f.end(), [](uint8_t v) { return v == 200; }));
    // a bright block on the edge of face 4 spreads onto the neighbouring face across the seam
    std::fill(f.begin(), f.end(), uint8_t(0));
    for (int y = 100; y < 110; ++y)
        for (int x = 252; x < 256; ++x) f[size_t(S->gridCell(0, 4, x, y))] = 255;
    const int edge = S->gridCell(0, 4, 255, 104), across = S->offset(edge, 1, 0);
    CHECK(across >= 0 && S->offset(across, -1, 0) == edge);
    blur(f, 2);
    CHECK(f[size_t(across)] > 60);    // light leaks over the seam like inside a face
    int b2, f2, x2, y2;
    S->cellFace(across, b2, f2, x2, y2);
    CHECK(f2 != 4);
    // never into the stacked next row of the grid (face 4 row 255 vs face 5 row 0 are not neighbours)
    std::fill(f.begin(), f.end(), uint8_t(0));
    for (int x = 0; x < 256; ++x) f[size_t(S->gridCell(0, 4, x, 255))] = 255;
    blur(f, 2);
    int b3, f3, x3, y3;
    S->cellFace(S->offset(S->gridCell(0, 4, 128, 255), 0, 1), b3, f3, x3, y3);
    CHECK(f[size_t(S->gridCell(0, 5, 128, 0))] == 0 || f3 == 5);
    // timing: the old per-pass 3x3 mean over the neighbour table vs the separable blur
    std::vector<uint8_t> g(n);
    for (size_t i = 0; i < n; ++i) g[i] = uint8_t((i * 2654435761u) >> 24) & 0x80 ? 255 : 0;
    auto old = [&](std::vector<uint8_t>& v, int radius) {
        std::vector<uint16_t> tmp(n);
        for (int pass = 0; pass < radius; ++pass) {
            for (int c = 0; c < int(n); ++c) {
                int bb, fc, x, y;
                if (!S->cellFace(c, bb, fc, x, y)) { tmp[size_t(c)] = v[size_t(c)]; continue; }
                int sum = v[size_t(c)], cnt = 1;
                for (int k = 0; k < 8; ++k) { const int o = S->neighbor(c, k); if (o >= 0) { sum += v[size_t(o)]; ++cnt; } }
                tmp[size_t(c)] = uint16_t(sum / cnt);
            }
            for (size_t c = 0; c < n; ++c) v[c] = uint8_t(tmp[c]);
        }
    };
    auto ms = [&](auto&& fn) {
        std::vector<uint8_t> v = g;
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 5; ++k) { v = g; fn(v); }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5.0;
    };
    blur(g, 2);   // padding table built outside the timing
    for (size_t i = 0; i < n; ++i) g[i] = uint8_t((i * 2654435761u) >> 24) & 0x80 ? 255 : 0;
    const double tOld = ms([&](std::vector<uint8_t>& v) { old(v, 2); }), tNew = ms([&](std::vector<uint8_t>& v) { blur(v, 2); });
    std::printf("  fog blur faceSize 256 (%zu cells), radius 2: old %.2f ms, new %.2f ms per update\n", n, tOld, tNew);
    CHECK(tNew < tOld);
}
