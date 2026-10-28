// Dependency-free unit tests. Run: ctest  (or ./rts_tests)
#include <filesystem>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "engine/core/ECS.h"
#include "engine/core/Fixed.h"
#include "engine/core/Random.h"
#include "engine/data/Json.h"
#include "engine/sim/Replay.h"
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"
#include "modules/rts/RtsTiles.h"
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
static const std::string kSkirmish = std::string(RTS_SOURCE_DIR) + "/games/skirmish";
static const std::string kMushrooms = std::string(RTS_SOURCE_DIR) + "/games/mushrooms";

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
                                                         : std::string(RTS_SOURCE_DIR) + "/games/" + game + "/" + rel;
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
        const std::string dir = std::string("games/") + g + "/";
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
    const std::string dir = "games/skirmish/";
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
        {"borderrush", "maps/five_lands.json"}, {"hearthhaul", "maps/green_dale.json"}, {"sporelanes", "maps/spore_field.json"}};
    for (auto& g : games) {
        std::string dir = std::string(RTS_SOURCE_DIR) + "/games/" + g[0];
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
        std::string dir = std::string(RTS_SOURCE_DIR) + "/games/" + g;
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
    const std::string dir = std::string(RTS_SOURCE_DIR) + "/games/showcase/models/";
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
    const std::string dir = "games/skirmish/";
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
    fs::remove_all(root);
    fs::create_directories(root / "game" / "music");
    auto write = [](const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; };
    write(root / "game" / "game.json", R"({"name":"T","audio":"sound.json"})");
    write(root / "game" / "sound.json", R"({"music":{"match":"music/a.wav"},"build":{"web":{"musicQuality":0.1}}})");
    write(root / "game" / "music" / "a.wav", audiopack::encodeWav(sinePcm(2, 48000, 48000), 2, 48000));
    write(root / "game" / "rules.json", "{}");
    audiopack::Report rep;
    std::string err;
    CHECK(audiopack::stageGame((root / "game").string(), (root / "web").string(), "web", rep, &err));
    CHECK(fs::exists(root / "web" / "music" / "a.ogg") && !fs::exists(root / "web" / "music" / "a.wav") && fs::exists(root / "web" / "rules.json"));
    CHECK(Json::parseFile((root / "web" / "sound.json").string())["music"].getStr("match") == "music/a.ogg");
    CHECK(fs::exists(root / "game" / "music" / "a.wav"));                 // the project itself is untouched
    CHECK(audiopack::stageGame((root / "game").string(), (root / "desktop").string(), "desktop", rep, &err));
    CHECK(fs::exists(root / "desktop" / "music" / "a.wav") && rep.items.empty());   // desktop keeps files by default
    write(root / "game" / "sound.json", R"({"sounds":{"x":{"file":"../../../etc/passwd"}},"build":{"web":{"sounds":"pcm"}}})");
    CHECK(audiopack::stageGame((root / "game").string(), (root / "web2").string(), "web", rep, &err));
    CHECK(rep.items.empty() && rep.warnings.size() == 1);                  // nothing outside the game folder is read
    fs::remove_all(root);
}

// ---------------- particles (engine/fx, modules/rts/fx) ----------------
#include <cstdlib>
#include "engine/fx/ParticleSystem.h"
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
        const std::string dir = "games/" + g + "/";
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
    const std::string dir = "games/skirmish/";
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
    const std::string dir = std::string(RTS_SOURCE_DIR) + "/games/showcase/models/";
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
    const std::string dir = std::string(RTS_SOURCE_DIR) + "/games/showcase/models/";
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

int main() {
    for (auto& [name, fn] : registry()) {
        int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
