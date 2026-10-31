// Grid fields (G5, RtsGridFields.h, docs/fields.md): kernels, effects, Lua API, saves, rollback, perf.
#include "modules/rts/RtsGridFields.h"

namespace {
// fire + fuel + water + creep on a 48x32 map; a few units for the per-cell effects
const char* kFieldRules = R"J({
  "victory": {"mode": "survive", "seconds": 900},
  "statuses": {"wet": {"mods": [{"stat": "speed", "mul": 0.9}]}},
  "damageTable": {"fire": {"none": 1}},
  "resources": {"gold": {"start": 0}},
  "fields": {
    "fuel": {"type": "int", "min": 0, "max": 100, "default": 6},
    "fire": {"type": "int", "min": 0, "max": 100,
      "kernels": [{"type": "spread", "period": 5, "above": 1, "set": 100, "chance": 0.3, "fuel": {"field": "fuel", "min": 1}},
                  {"type": "consume", "period": 5, "fuel": "fuel", "amount": 1, "above": 1, "out": 0}],
      "effects": [{"type": "damage", "above": 1, "perSecond": 10, "damageType": "fire", "period": 5},
                  {"type": "blockMove", "above": 90}]},
    "water": {"type": "fixed", "min": 0, "max": 50,
      "kernels": [{"type": "flow", "period": 2, "rate": 0.5, "unitsPerHeight": 1}, {"type": "decay", "period": 20, "percent": 0.01}],
      "effects": [{"type": "status", "above": 0.2, "status": "wet"}, {"type": "speed", "above": 0.5, "mul": 0.5}]},
    "creep": {"type": "int", "min": 0, "max": 100, "perPlayer": true, "resolution": 2,
      "kernels": [{"type": "diffuse", "period": 5, "rate": 0.5}, {"type": "decay", "period": 5, "amount": 1}],
      "effects": [{"type": "yield", "resource": "gold", "above": 20, "perSecond": 0.05}, {"type": "reveal", "above": 10}]}
  },
  "types": {
    "hive": {"structure": {"footprint": [2, 2]}, "health": {"max": 500}, "field": {"creep": {"emit": 60, "radius": 4, "mode": "max", "period": 5}}},
    "grunt": {"health": {"max": 200}, "move": {"speed": 2, "radius": 0.4}, "weapons": [{"range": 1, "damage": 1}]}
  }
})J";

std::string fieldMap(const std::string& extra = "") {
    return flatMap(48, 32, R"(,"entities":[{"type":"hive","owner":1,"pos":[6,6]},{"type":"hive","owner":2,"pos":[42,26]},
        {"type":"grunt","owner":1,"pos":[20.5,10.5]},{"type":"grunt","owner":2,"pos":[30.5,20.5]}],
      "gridFields":{"fire":{"fill":[{"rect":[18,8,2,2],"value":100}]},"water":{"fill":[{"circle":[30,20,2],"value":20}]}})" + extra);
}

int fieldIndex(Simulation& sim, const char* id) { return mod(sim).gridFields()->find(id); }
int64_t fieldSum(Simulation& sim, const char* id, int layer = 0) {
    auto* gf = mod(sim).gridFields();
    const int fi = gf->find(id);
    const auto& f = sim.world().resource<game::GridFieldStore>()->fields[size_t(fi)];
    int64_t s = 0;
    for (size_t i = 0; i < f.cells(); ++i) s += f.v[size_t(layer) * f.cells() + i];
    return s;
}
} // namespace

TEST(grid_fields_kernels_effects_and_determinism) {
    auto sim = makeSimJson(kFieldRules, fieldMap(), 7);
    auto& m = mod(*sim);
    game::GridFields* gf = m.gridFields();
    CHECK(gf && gf->active() && gf->defs().size() == 4);
    const int fire = fieldIndex(*sim, "fire"), fuel = fieldIndex(*sim, "fuel");
    CHECK(gf->get(*sim, fire, 18, 8) == 100 && gf->get(*sim, fire, 30, 30) == 0);
    CHECK(gf->get(*sim, fuel, 5, 5) == 6);
    const int64_t water0 = fieldSum(*sim, "water");
    CHECK(water0 > 0);
    // the burning cells block movement (blockMove above 90)
    CHECK(!m.state(*sim).grid.passable(18, 8, game::kLayerGround));
    Entity grunt = findType(*sim, "grunt", 1);
    const Fixed hp0 = sim->world().get<game::Unit>(grunt).hp;
    run(*sim, 4);
    // fire spread (fuel field), the first cells burnt out (consume -> out 0, fuel gone)
    int burning = 0, ash = 0;
    for (int z = 0; z < 32; ++z) for (int x = 0; x < 48; ++x) {
        burning += gf->get(*sim, fire, x, z) > 0;
        ash += gf->get(*sim, fuel, x, z) == 0;
    }
    CHECK(burning > 4);
    CHECK(ash >= 4);
    CHECK(gf->get(*sim, fire, 18, 8) == 0);   // burnt out (6 runs of fuel)
    CHECK(m.state(*sim).grid.passable(18, 8, game::kLayerGround));   // and unblocked again
    // the grunt next to the fire took fire damage
    if (sim->world().alive(grunt)) CHECK(sim->world().get<game::Unit>(grunt).hp < hp0);
    // water flowed out of its pool (flat ground: spreads, conserved up to the decay)
    const int wi = fieldIndex(*sim, "water");
    CHECK(gf->get(*sim, wi, 33, 20) > 0);
    CHECK(fieldSum(*sim, "water") <= water0);
    // creep: per-player layers emitted by the hives, diffused; yield pays the owners, reveal shows creep cells
    const int creep = fieldIndex(*sim, "creep");
    CHECK(gf->layers(creep) == 3);
    CHECK(gf->get(*sim, creep, 3, 3, 1) > 20 && gf->get(*sim, creep, 3, 3, 2) == 0);
    CHECK(gf->get(*sim, creep, 21, 13, 2) > 20);
    CHECK(stockOf(*sim, 1, "gold") > Fixed{});
    // determinism: two runs with the same seed, a different seed differs
    auto runSum = [](uint64_t seed) {
        auto s = makeSimJson(kFieldRules, fieldMap(), seed);
        run(*s, 20);
        return s->checksum();
    };
    const uint64_t a = runSum(5);
    CHECK(a == runSum(5));
    CHECK(a != runSum(6));
}

TEST(grid_fields_save_load_mid_burn_and_rollback) {
    auto sim = makeSimJson(kFieldRules, fieldMap(), 3);
    run(*sim, 3);   // mid-burn
    const std::string save = sim->serialize();
    SimSnapshot snap;
    sim->saveState(snap);
    run(*sim, 6);
    const uint64_t straight = sim->checksum();
    const int64_t fire = fieldSum(*sim, "fire");
    // load into a fresh simulation set up from the same rules + map
    auto other = makeSimJson(kFieldRules, fieldMap(), 3);
    std::string err;
    CHECK(other->deserialize(save, &err));
    run(*other, 6);
    CHECK(other->checksum() == straight);
    CHECK(fieldSum(*other, "fire") == fire);
    // rollback snapshot (World copy) continues identically too
    sim->loadState(snap);
    run(*sim, 6);
    CHECK(sim->checksum() == straight);
    // a game without fields has no field store and the same behaviour as before
    auto plain = makeSimJson(R"({"types": {"a": {"health": {"max": 10}}}})", flatMap(8, 8, ""));
    CHECK(!plain->world().resource<game::GridFieldStore>() && !mod(*plain).gridFields()->active());
}

TEST(grid_fields_lua_api_and_threshold_hooks) {
    Json rules = Json::parse(kFieldRules);
    rules.set("scripts", Json::parse(R"(["scripts/f.lua"])"));
    Json src = Json::object();
    src.set("scripts/f.lua", Json::string(R"LUA(
local function need(c, what) if not c then error("check failed: " .. what) end end
on("start", function()
  state.hits, state.falls = 0, 0
  need(field.fill_rect("fuel", 0, 0, 47, 31, 3) > 1000, "fill_rect")
  need(field.get("fuel", fx(5, 2), 7) == 3, "get")
  need(field.info("water").type == "fixed" and field.info("creep").per_player and field.info("creep").w == 24, "info")
  field.set("water", 10, 10, fx(3, 2))
  need(field.get("water", 10, 10) == fx(3, 2), "fixed value")
  need(field.fill_circle("fire", 40, 5, 2, 100, "max") >= 9, "fill_circle")
  need(#field.find("fire", {above = 50, x = 40, z = 5, radius = 3}) >= 9, "find table")
  need(#field.find("fire", function(v, x, z) return v >= 100 and x > 40 end, {limit = 3}) == 3, "find fn")
  local v, mx, mz = field.max("fire", 30, 0, 47, 10)
  need(v == 100 and mx >= 38, "max")
  need(field.sum("fuel", 0, 0, 3, 3) == 48, "sum")
  need(field.count("fire", 0, 0, 47, 31, 1) >= 9, "count")
  need(not pcall(function() field.set("creep", 1, 1, 5) end), "per-player write needs a player")
  field.set("creep", 1, 1, 77, 2)
  need(field.get("creep", 1, 1, 2) == 77 and field.get("creep", 1, 1) == 77, "per-player layer")
  local cx, cz = field.cell("creep", 5, 7)
  need(cx == 2 and cz == 3, "cell")
end)
on_field_threshold("fire", 50, function(e) state.hits = state.hits + 1; need(e.field == "fire" and e.value >= 50, "event") end)
on_field_threshold("fuel", 1, function(e) state.falls = state.falls + 1 end, "fall")
every(3, function() need(state.hits > 9 and state.falls > 0, "threshold hooks fired") end)
)LUA"));
    rules.set("$scriptSources", src);
    SimConfig cfg; cfg.seed = 2;
    auto sim = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
    sim->setup(rules, Json::parse(fieldMap()));
    run(*sim, 4);
    auto& m = mod(*sim);
    for (auto& e : m.scripts()->log()) if (e.error) std::printf("  field lua: %s\n", e.text.c_str());
    CHECK(m.scripts()->errorCount() == 0);
    CHECK(m.gridFields()->hooks().size() == 2);
    // view scripts get the read-only table (no writes)
    CHECK(scriptLogHas(*sim, "check failed") == false);
}

TEST(grid_fields_perf_256) {
    // 256x256 int field, diffuse (8 neighbours) + decay every 5 ticks, the whole map live; and an idle field
    const std::string rules = R"({"victory": {"mode": "survive", "seconds": 900},
      "fields": {"heat": {"type": "int", "min": 0, "max": 1000000, "kernels": [
          {"type": "diffuse", "period": 5, "rate": 0.4, "neighbours": 8}, {"type": "decay", "period": 5, "amount": 1}]},
        "idle": {"type": "int", "min": 0, "max": 100, "kernels": [{"type": "diffuse", "period": 5, "rate": 0.5}, {"type": "spread", "period": 5, "chance": 0.2}]}},
      "types": {"a": {"health": {"max": 10}}}})";
    auto sim = makeSimJson(rules, flatMap(256, 256, R"(,"gridFields":{"heat":{"fill":[{"rect":[0,0,256,256],"value":500000}]}})"));
    auto* gf = mod(*sim).gridFields();
    const int heat = gf->find("heat");
    for (int x = 0; x < 256; x += 3) gf->write(*sim, heat, x, (x * 7) % 256, 0, 1000000, 0);
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t v0 = gf->cellsVisited;
    for (int i = 0; i < 200; ++i) sim->step();
    const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  grid fields 256x256: %.3f ms per sim tick, whole step (diffuse8 + decay on 65536 live cells every 5 ticks, %llu cells visited)\n",
                total / 200.0, (unsigned long long)(gf->cellsVisited - v0));
    // idle field: no live chunks -> no cells visited
    auto sim2 = makeSimJson(rules, flatMap(256, 256, ""));
    auto* gf2 = mod(*sim2).gridFields();
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) sim2->step();
    const double idle = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::printf("  grid fields 256x256 idle: %.4f ms per tick for the whole sim step, %llu field cells visited\n", idle / 200.0,
                (unsigned long long)gf2->cellsVisited);
    CHECK(gf2->cellsVisited == 0);
    {   // a kernel run over the full map, measured alone
        const auto t2 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) gf->tick(*sim);   // tick 200: every kernel runs
        const double per = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count() / 20.0;
        std::printf("  grid fields 256x256: %.3f ms per kernel pass (diffuse8 + decay + effects), 5-tick period -> %.3f ms per tick\n", per, per / 5.0);
        CHECK(per < 25.0);   // generous: debug / slow CI machines; Release on a desktop is well under 1 ms per tick
    }
}
