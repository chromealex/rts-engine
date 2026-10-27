// Dependency-free unit tests. Run: ctest  (or ./rts_tests)
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
    const char* path = "test_roundtrip.rpl";
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

// ---------------- Audio (engine/audio, modules/rts/audio) ----------------
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include "engine/audio/Audio.h"
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
    CHECK(!loadBank(*a, R"({"sounds":{"y":{}}})", &err) && a->find("beep") != audio::kNoSound);
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
    CHECK(loadBank(*a, R"({"sounds":{"tone":{"synth":{"wave":"sine","freq":300,"duration":2},"loop":true}}})"));
    a->setListener(0, 0, 0, 10);
    CHECK(a->audibility(5, 0) == 1.0f && a->audibility(25, 0) == 0.0f);
    CHECK(a->audibility(15, 0) > 0.0f && a->audibility(15, 0) < 1.0f);
    audio::PlayParams far;
    far.positional = true; far.x = 100;
    CHECK(loadBank(*a, R"({"sounds":{"blip":{"synth":{"duration":0.1}}}})"));
    CHECK(a->play("blip", far) == audio::kNoVoice);                 // inaudible one-shots are not even started
    CHECK(loadBank(*a, R"({"sounds":{"tone":{"synth":{"wave":"sine","freq":300,"duration":2},"loop":true}}})"));
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

int main() {
    for (auto& [name, fn] : registry()) {
        int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
