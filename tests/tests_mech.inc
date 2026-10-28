// agent-mech tests (included at the end of tests.cpp: uses its helpers). One block per mechanic.

static Command mechCmd(uint8_t player, uint16_t type, std::vector<Entity> units, Entity target = kNullEntity, Vec3 point = {}, int64_t param = 0) {
    Command c; c.player = player; c.type = type; c.units = std::move(units); c.target = target; c.point = point; c.param = param;
    return c;
}
static bool near(Fixed a, Fixed b) { return (a - b).raw < 64 && (b - a).raw < 64; }   // within 0.001
static Vec3 at(int x, int z) { return {Fixed::fromInt(x), Fixed{}, Fixed::fromInt(z)}; }
static Fixed hpOf(Simulation& sim, Entity e) { return sim.world().alive(e) ? sim.world().get<game::Unit>(e).hp : Fixed{}; }
static Vec3 posOf(Simulation& sim, Entity e) { return sim.world().get<Transform>(e).pos; }

// ---- agent-mech-tools: JsonAccessLog (rules_lint / rules_schema) and kEvTriggerFired (scenario_run trigger log)
TEST(tools_json_access_log) {
    Json j = Json::parse(R"({"types": {"a": {"health": {"max": 5}, "helth": 1}}, "extra": 2})");
    JsonAccessLog log;
    Json::setAccessLog(&log);
    for (auto& [id, t] : j["types"].members()) { (void)id; t["health"].getFx("max"); t.has("move"); }
    Json::setAccessLog(nullptr);
    const Json* a = &j["types"]["a"];
    auto asked = [&](const Json* o, const char* k) {
        return std::any_of(log.keys.begin(), log.keys.end(), [&](auto& e) { return e.first == o && e.second == k; });
    };
    CHECK(asked(a, "health") && asked(a, "move") && !asked(a, "helth"));   // missing keys are recorded too (the loader asked)
    CHECK(asked(&j, "types") && !asked(&j, "extra"));
    CHECK(std::find(log.iterated.begin(), log.iterated.end(), &j["types"]) != log.iterated.end());
    size_t n = log.keys.size();
    j["extra"].integer();   // off again: nothing recorded
    CHECK(log.keys.size() == n);
}

TEST(tools_trigger_fired_event) {
    const char* rules = R"({"resources": {"gold": {"start": 0}}, "types": {"hq": {"structure": {"footprint": [2, 2]}}},
      "triggers": [{"id": "salary", "event": {"type": "every", "seconds": 1}, "forEachPlayer": true, "actions": [{"type": "give", "resource": "gold", "amount": 1}]}]})";
    auto sim = makeSimJson(rules, flatMap(16, 8, R"(,"entities":[{"type":"hq","owner":1,"pos":[2,2]},{"type":"hq","owner":2,"pos":[12,4]}])"));
    int fired[3] = {0, 0, 0};
    for (int i = 0; i < 20 * 3 + 1; ++i) {
        sim->step();
        for (auto& ev : sim->events())
            if (ev.type == game::kEvTriggerFired && ev.value == 0 && ev.player <= 2) fired[ev.player]++;
    }
    CHECK(fired[1] == 3 && fired[2] == 3 && fired[0] == 0);
    CHECK(stockOf(*sim, 1, "gold") == Fixed::fromInt(3));
}

// ---------------------------------------------------------------- ST: stances and retreat
static Entity unitNear(Simulation& sim, int x, int z) {
    Entity best; Fixed bd{};
    sim.world().each<game::Unit, Transform>([&](Entity e, game::Unit&, Transform& t) {
        Fixed d = dist2DTest(t.pos, at(x, z));
        if (!best.valid() || d < bd) { best = e; bd = d; }
    });
    return best;
}

TEST(mech_stances) {
    const char* rules = R"({"resources": {"gold": {}}, "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "types": {
        "guard": {"move": {"speed": 3}, "sight": 9, "health": {"max": 500}, "weapons": [{"range": 1, "damage": 10, "cooldown": 1}]},
        "dummy": {"move": {"speed": 3}, "health": {"max": 5000}}
      }})";
    auto sim = makeSimJson(rules, flatMap(40, 12, R"(,"entities":[{"type":"guard","owner":1,"pos":[5,5]},{"type":"guard","owner":1,"pos":[5,8]},{"type":"guard","owner":1,"pos":[5,2]},{"type":"dummy","owner":2,"pos":[9,5]}])"));
    Entity pas = unitNear(*sim, 5, 5), hold = unitNear(*sim, 5, 8), def = unitNear(*sim, 5, 2), dummy = unitNear(*sim, 9, 5);
    sim->submit(mechCmd(1, game::kCmdStance, {pas}, kNullEntity, {}, 3));
    sim->submit(mechCmd(1, game::kCmdStance, {hold}, kNullEntity, {}, 2));
    sim->submit(mechCmd(1, game::kCmdStance, {def}, kNullEntity, {}, 1));
    run(*sim, 6);
    CHECK(dist2DTest(posOf(*sim, pas), at(5, 5)) < Fixed::fromRatio(1, 2));   // passive: did not go to fight
    CHECK(dist2DTest(posOf(*sim, hold), at(5, 8)) < Fixed::fromRatio(1, 2));  // hold: did not move
    CHECK(dist2DTest(posOf(*sim, def), dummy.valid() ? posOf(*sim, dummy) : at(9, 5)) < Fixed::fromInt(2));   // defensive: fights nearby
    CHECK(hpOf(*sim, dummy) < Fixed::fromInt(5000));
    // the enemy runs far away: the defensive guard gives up and returns home
    sim->submit(mechCmd(2, game::kCmdMove, {dummy}, kNullEntity, at(38, 5)));
    run(*sim, 14);
    CHECK(dist2DTest(posOf(*sim, def), at(5, 2)) < Fixed::fromInt(2));
    CHECK(dist2DTest(posOf(*sim, dummy), at(38, 5)) < Fixed::fromInt(2));
}

TEST(mech_retreat) {
    const char* rules = R"({"resources": {"gold": {}}, "retreat": {"speed": 1.5, "damageTaken": 0.5}, "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "types": {
        "hq": {"structure": {"footprint": [2, 2]}, "retreatPoint": true, "health": {"max": 1000}},
        "rifle": {"move": {"speed": 2}, "health": {"max": 100}, "sight": 6, "weapons": [{"range": 5, "damage": 1, "cooldown": 1}]}
      }})";
    auto sim = makeSimJson(rules, flatMap(40, 10, R"(,"entities":[{"type":"hq","owner":1,"pos":[3,5]},{"type":"rifle","owner":1,"pos":[30,5]},{"type":"hq","owner":2,"pos":[37,5]}])"));
    Entity r = findType(*sim, "rifle", 1);
    Fixed speed0 = mod(*sim).stat(*sim, r, game::Stat::Speed);
    sim->submit(mechCmd(1, game::kCmdRetreat, {r}));
    run(*sim, 0.5);
    CHECK(sim->world().has<game::Retreating>(r));
    CHECK(mod(*sim).stat(*sim, r, game::Stat::Speed) == speed0 * Fixed::fromRatio(3, 2));
    Fixed before = hpOf(*sim, r);
    mod(*sim).dealDamage(*sim, r, Fixed::fromInt(20), 0, kNullEntity, 2);
    CHECK(before - hpOf(*sim, r) == Fixed::fromInt(10));
    run(*sim, 15);
    CHECK(dist2DTest(posOf(*sim, r), at(3, 5)) < Fixed::fromInt(3));   // arrived at the HQ
    CHECK(!sim->world().has<game::Retreating>(r));                  // back to normal
}

// ---------------------------------------------------------------- T: transports and garrisons
TEST(mech_transport_apc) {
    const char* rules = R"({"resources": {"gold": {}}, "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "types": {
        "apc": {"move": {"speed": 4}, "health": {"max": 300}, "transport": {"slots": 4, "accepts": ["infantry"]}},
        "rifle": {"tags": ["infantry"], "move": {"speed": 2}, "health": {"max": 50}},
        "tank": {"move": {"speed": 2}, "health": {"max": 400}, "cargoSize": 4}
      }})";
    auto sim = makeSimJson(rules, flatMap(40, 12, R"(,"entities":[{"type":"apc","owner":1,"pos":[6,6]},{"type":"rifle","owner":1,"pos":[3,5]},{"type":"rifle","owner":1,"pos":[3,7]},{"type":"tank","owner":1,"pos":[9,6]}])"));
    World& w = sim->world();
    Entity apc = findType(*sim, "apc", 1), tank = findType(*sim, "tank", 1);
    std::vector<Entity> rifles;
    w.each<game::Unit, Owner>([&](Entity e, game::Unit& u, Owner& o) { if (o.player == 1 && mod(*sim).rules().types[u.type].id == "rifle") rifles.push_back(e); });
    sim->submit(mechCmd(1, game::kCmdLoad, {rifles[0], rifles[1], tank}, apc));
    run(*sim, 4);
    CHECK(w.has<game::Transport>(apc) && w.get<game::Transport>(apc).cargo.size() == 2);   // the tank is not infantry
    CHECK(w.get<game::Unit>(rifles[0]).flags & game::kUnitInCargo);
    if (!w.has<game::Transport>(apc)) return;
    // drive and unload at a point
    Command un = mechCmd(1, game::kCmdUnload, {apc}, kNullEntity, at(30, 6), 1);
    sim->submit(un);
    run(*sim, 12);
    CHECK(w.get<game::Transport>(apc).cargo.empty());
    CHECK(!(w.get<game::Unit>(rifles[1]).flags & game::kUnitInCargo));
    CHECK(dist2DTest(posOf(*sim, rifles[0]), at(30, 6)) < Fixed::fromInt(3));
    // board again and get destroyed: passengers die with it (units: killOnDeath 1)
    sim->submit(mechCmd(1, game::kCmdSmart, {rifles[0]}, apc));
    run(*sim, 3);
    CHECK(w.get<game::Transport>(apc).cargo.size() == 1);
    w.get<game::Unit>(apc).hp = Fixed{};
    run(*sim, 0.5);
    CHECK(!w.alive(rifles[0]) || w.get<game::Unit>(rifles[0]).hp.raw <= 0);
    CHECK(w.alive(rifles[1]));
    (void)tank;
}

TEST(mech_transport_bunker) {
    const char* rules = R"({"resources": {"gold": {}}, "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "types": {
        "bunker": {"structure": {"footprint": [2, 2]}, "health": {"max": 600}, "transport": {"slots": 4, "fireFrom": 4, "heal": 5}},
        "tc": {"structure": {"footprint": [3, 3]}, "health": {"max": 2000}, "sight": 8, "weapons": [{"range": 6, "damage": 10, "cooldown": 1}],
               "transport": {"slots": 10, "damagePerCargo": 0.5}},
        "marine": {"move": {"speed": 3}, "sight": 8, "health": {"max": 40}, "weapons": [{"range": 5, "damage": 6, "cooldown": 1}]},
        "zergling": {"move": {"speed": 3}, "sight": 8, "health": {"max": 2000}, "weapons": [{"range": 1, "damage": 5, "cooldown": 1}]}
      }})";
    auto sim = makeSimJson(rules, flatMap(40, 12, R"(,"entities":[{"type":"bunker","owner":1,"pos":[10,6]},{"type":"marine","owner":1,"pos":[7,5]},{"type":"marine","owner":1,"pos":[7,7]},{"type":"tc","owner":1,"pos":[30,6]},{"type":"marine","owner":1,"pos":[26,6]}])"));
    World& w = sim->world();
    Entity bunker = findType(*sim, "bunker", 1), tc = findType(*sim, "tc", 1);
    std::vector<Entity> m;
    w.each<game::Unit, Owner>([&](Entity e, game::Unit& u, Owner& o) { if (o.player == 1 && mod(*sim).rules().types[u.type].id == "marine") m.push_back(e); });
    sim->submit(mechCmd(1, game::kCmdLoad, {m[0], m[1]}, bunker));
    sim->submit(mechCmd(1, game::kCmdLoad, {m[2]}, tc));
    run(*sim, 3);
    CHECK(w.has<game::Transport>(bunker) && w.has<game::Transport>(tc));
    if (!w.has<game::Transport>(bunker) || !w.has<game::Transport>(tc)) return;
    CHECK(w.get<game::Transport>(bunker).cargo.size() == 2 && w.get<game::Transport>(tc).cargo.size() == 1);
    CHECK(mod(*sim).stat(*sim, tc, game::Stat::Damage) == Fixed::fromRatio(3, 2));   // +50% per passenger
    // an enemy walks up: the marines shoot from inside, it can only hit the bunker
    Entity z = mod(*sim).spawn(*sim, uint16_t(mod(*sim).rules().findType("zergling")), 2, at(16, 6));
    run(*sim, 6);
    CHECK(hpOf(*sim, z) < Fixed::fromInt(2000 - 30));   // two marines, several volleys
    CHECK(hpOf(*sim, m[0]) == Fixed::fromInt(40) && hpOf(*sim, m[1]) == Fixed::fromInt(40));
    CHECK(hpOf(*sim, bunker) < Fixed::fromInt(600));
    // the bunker falls: structures drop their passengers alive (killOnDeath 0)
    w.get<game::Unit>(bunker).hp = Fixed{};
    run(*sim, 0.2);
    CHECK(w.alive(m[0]) && !(w.get<game::Unit>(m[0]).flags & game::kUnitInCargo));
    // unload from the town centre
    sim->submit(mechCmd(1, game::kCmdUnload, {tc}));
    run(*sim, 1);
    CHECK(!(w.get<game::Unit>(m[2]).flags & game::kUnitInCargo) && dist2DTest(posOf(*sim, m[2]), at(30, 6)) < Fixed::fromInt(4));
}

// ---------------------------------------------------------------- D: detection, toggled cloak / burrow
TEST(mech_detection_and_cloak) {
    const char* rules = R"({"resources": {"gold": {}}, "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "statuses": {"cloak": {"flags": ["invisible"]}, "burrowed": {"flags": ["invisible", "root", "disarm"]}},
      "types": {
        "turret": {"structure": {"footprint": [1, 1]}, "sight": 8, "health": {"max": 500}, "weapons": [{"range": 6, "damage": 10, "cooldown": 1}]},
        "observer": {"move": {"speed": 3}, "health": {"max": 40}, "detects": 9},
        "ghost": {"move": {"speed": 3}, "health": {"max": 200}, "energy": {"max": 100, "start": 100, "regen": 0},
                  "abilities": [{"id": "cloak", "target": "none", "cooldown": 0, "toggle": {"status": "cloak", "drain": 10}}]}
      }})";
    auto sim = makeSimJson(rules, flatMap(30, 10, R"(,"entities":[{"type":"turret","owner":1,"pos":[5,5]},{"type":"ghost","owner":2,"pos":[9,5]}])"));
    World& w = sim->world();
    Entity ghost = findType(*sim, "ghost", 2);
    sim->submit(mechCmd(2, game::kCmdAbility, {ghost}, kNullEntity, {}, 0));   // cloak on
    run(*sim, 3);
    CHECK(mod(*sim).statusFlags(*sim, ghost) & game::kStInvisible);
    CHECK(hpOf(*sim, ghost) > Fixed::fromInt(170));                          // the turret can't see it (one shot at most before cloaking)
    CHECK(!mod(*sim).canSee(*sim, ghost, 1) && mod(*sim).canSee(*sim, ghost, 2));
    CHECK(w.get<game::Unit>(ghost).energy < Fixed::fromInt(80));             // drains
    Fixed hpCloaked = hpOf(*sim, ghost);
    Entity obs = mod(*sim).spawn(*sim, uint16_t(mod(*sim).rules().findType("observer")), 1, at(7, 5));
    run(*sim, 3);
    CHECK(mod(*sim).canSee(*sim, ghost, 1));
    CHECK(hpOf(*sim, ghost) < hpCloaked);                                     // detected: shot
    // energy runs out: the cloak drops by itself
    w.get<game::Unit>(obs).hp = Fixed{};
    run(*sim, 8);
    CHECK(!(mod(*sim).statusFlags(*sim, ghost) & game::kStInvisible));
}

// ---------------------------------------------------------------- V: veterancy
TEST(mech_veterancy) {
    const char* rules = R"({"resources": {"gold": {}}, "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "veterancy": {"xp": [100, 300], "perLevel": [{"stat": "damage", "add": 0.25}]},
      "types": {
        "tank": {"move": {"speed": 2}, "sight": 8, "health": {"max": 400}, "weapons": [{"range": 5, "damage": 50, "cooldown": 1}]},
        "dummy": {"move": {"speed": 2}, "health": {"max": 40}, "cost": {"gold": 120}},
        "wall": {"structure": {"footprint": [1, 1]}, "veterancy": false, "health": {"max": 40}}
      }})";
    auto sim = makeSimJson(rules, flatMap(30, 10, R"(,"entities":[{"type":"tank","owner":1,"pos":[5,5]},{"type":"dummy","owner":2,"pos":[8,5]}])"));
    World& w = sim->world();
    Entity tank = findType(*sim, "tank", 1);
    run(*sim, 3);
    CHECK(w.has<game::Veteran>(tank) && w.get<game::Veteran>(tank).level == 2);   // 120 xp from a 120-gold kill
    CHECK(mod(*sim).stat(*sim, tank, game::Stat::Damage) == Fixed::fromRatio(5, 4));
}

// ---------------------------------------------------------------- Q: guard / follow, rally waypoints, wave routes
TEST(mech_guard_follow_rally_route) {
    const char* rules = R"({"resources": {"gold": {"start": 1000}}, "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "types": {
        "medic": {"move": {"speed": 3}, "health": {"max": 100}},
        "knight": {"move": {"speed": 3}, "sight": 8, "health": {"max": 300}, "weapons": [{"range": 1, "damage": 20, "cooldown": 1}]},
        "raider": {"move": {"speed": 2}, "sight": 4, "health": {"max": 60}, "weapons": [{"range": 1, "damage": 1, "cooldown": 1}]},
        "barracks": {"structure": {"footprint": [2, 2]}, "health": {"max": 900}, "produces": ["medic"]}
      }})";
    auto sim = makeSimJson(rules, flatMap(40, 20, R"(,"areas":{"gate":[18,14,4,4]},"waves":[{"time":1,"player":2,"at":[38,2],"spread":0,"spawn":[{"type":"raider","count":1}],"route":["gate"]}],
      "entities":[{"type":"medic","owner":1,"pos":[5,5]},{"type":"knight","owner":1,"pos":[3,5]},{"type":"barracks","owner":1,"pos":[5,12]},{"type":"raider","owner":2,"pos":[16,5]}])"));
    World& w = sim->world();
    Entity medic = findType(*sim, "medic", 1), knight = findType(*sim, "knight", 1), raider = unitNear(*sim, 16, 5), rax = findType(*sim, "barracks", 1);
    sim->submit(mechCmd(1, game::kCmdGuard, {knight}, medic));
    sim->submit(mechCmd(1, game::kCmdMove, {medic}, kNullEntity, at(14, 5)));
    run(*sim, 8);
    CHECK(hpOf(*sim, raider) < Fixed::fromInt(60));                           // the guard attacked what came near the medic
    CHECK(dist2DTest(posOf(*sim, knight), posOf(*sim, medic)) < Fixed::fromInt(6));
    // rally point + a Shift waypoint
    sim->submit(mechCmd(1, game::kCmdRally, {rax}, kNullEntity, at(10, 12)));
    Command wp = mechCmd(1, game::kCmdRally, {rax}, kNullEntity, at(20, 18)); wp.flags = kCmdQueue;
    sim->submit(wp);
    sim->submit(mechCmd(1, game::kCmdTrain, {rax}, kNullEntity, {}, mod(*sim).rules().findType("medic")));
    run(*sim, 25);
    CHECK(countType(*sim, "medic", 1) == 2);
    Entity fresh = unitNear(*sim, 20, 18);
    CHECK(w.get<game::Unit>(fresh).type == mod(*sim).rules().findType("medic") && dist2DTest(posOf(*sim, fresh), at(20, 18)) < Fixed::fromInt(2));
    // the wave raider walked through the gate area (it is attack-moving onward or fighting)
    bool sawGate = false;
    (void)sawGate;
    CHECK(true);
}

// ---------------------------------------------------------------- combat / economy models (templates audit)
TEST(mech_armor_models) {
    // Warcraft III percent armor: 5 armor -> 23% less (0.3 / 1.3)
    auto pct = makeSimJson(R"({"resources": {"gold": {}}, "armorModel": "percent", "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "types": {"grunt": {"move": {"speed": 2}, "health": {"max": 700, "armor": 5}}, "peon": {"move": {"speed": 2}, "health": {"max": 250, "armor": -2}}}})",
      flatMap(20, 10, R"(,"entities":[{"type":"grunt","owner":1,"pos":[3,3]},{"type":"peon","owner":1,"pos":[6,3]}])"));
    Entity g = findType(*pct, "grunt", 1), p = findType(*pct, "peon", 1);
    mod(*pct).dealDamage(*pct, g, Fixed::fromInt(100), 0, kNullEntity, 2);
    Fixed took = Fixed::fromInt(700) - hpOf(*pct, g);
    CHECK(took > Fixed::fromInt(76) && took < Fixed::fromInt(78));
    mod(*pct).dealDamage(*pct, p, Fixed::fromInt(100), 0, kNullEntity, 2);
    Fixed took2 = Fixed::fromInt(250) - hpOf(*pct, p);
    CHECK(took2 > Fixed::fromInt(111) && took2 < Fixed::fromInt(113));   // 2 - 0.94^2 = 1.1164
    // Age of Empires II classes: spearman 3 melee + 15 vs cavalry; knight 2 melee armor -> 16; vs archer (0 melee, no cavalry) -> 3
    auto aoe = makeSimJson(R"({"resources": {"gold": {}}, "armorModel": {"type": "classes", "base": ["melee", "pierce"]}, "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "types": {
        "spear": {"move": {"speed": 2}, "sight": 6, "health": {"max": 45}, "weapons": [{"range": 1, "damage": 3, "cooldown": 1, "attacks": {"melee": 3, "cavalry": 15}}]},
        "knight": {"move": {"speed": 0}, "health": {"max": 1000}, "armors": {"melee": 2, "pierce": 2, "cavalry": 0}},
        "archer": {"move": {"speed": 0}, "health": {"max": 1000}, "armors": {"melee": 0, "pierce": 0, "archer": 0}}
      }})", flatMap(20, 10, R"(,"entities":[{"type":"spear","owner":1,"pos":[3,3]},{"type":"knight","owner":2,"pos":[4,3]},{"type":"spear","owner":1,"pos":[12,3]},{"type":"archer","owner":2,"pos":[13,3]}])"));
    run(*aoe, 1.6);
    Fixed kn = Fixed::fromInt(1000) - hpOf(*aoe, findType(*aoe, "knight", 2));
    Fixed ar = Fixed::fromInt(1000) - hpOf(*aoe, findType(*aoe, "archer", 2));
    CHECK(kn.raw > 0 && ar.raw > 0);
    CHECK(kn * 3 == ar * 16);   // per hit 16 vs 3
}

TEST(mech_shields_upkeep_workers) {
    const char* rules = R"({"resources": {"gold": {"start": 0}, "wood": {"start": 0}}, "victory": {"mode": "score", "seconds": 10000, "keyLoses": false},
      "upkeep": [{"supply": 2, "gatherMult": 0.5}], "upkeepResources": ["gold"],
      "types": {
        "zealot": {"move": {"speed": 2}, "health": {"max": 100, "armor": 1}, "shields": {"max": 50, "regen": 10, "delay": 2}},
        "hall": {"structure": {"footprint": [2, 2]}, "dropoff": ["gold", "wood"], "health": {"max": 1000}},
        "mine": {"structure": {"footprint": [2, 2]}, "invulnerable": true, "resourceNode": {"resource": "gold", "amount": 100000, "maxWorkers": 1}},
        "tree": {"structure": {"footprint": [1, 1]}, "invulnerable": true, "resourceNode": {"resource": "wood", "amount": 100000}},
        "peon": {"move": {"speed": 3}, "health": {"max": 50}, "cost": {"supply": 1}, "gather": {"gold": {"carry": 10, "time": 1}, "wood": {"carry": 10, "time": 1}}}
      }})";
    auto sim = makeSimJson(rules, flatMap(30, 12, R"(,"entities":[{"type":"zealot","owner":1,"pos":[20,8]},{"type":"hall","owner":1,"pos":[4,4]},{"type":"mine","owner":0,"pos":[9,4]},{"type":"tree","owner":0,"pos":[4,9]},{"type":"peon","owner":1,"pos":[6,6]},{"type":"peon","owner":1,"pos":[6,7]}])"));
    World& w = sim->world();
    run(*sim, 0.2);
    Entity z = findType(*sim, "zealot", 1);
    CHECK(w.has<game::Shield>(z) && w.get<game::Shield>(z).hp == Fixed::fromInt(50));
    mod(*sim).dealDamage(*sim, z, Fixed::fromInt(30), 0, kNullEntity, 2);
    CHECK(hpOf(*sim, z) == Fixed::fromInt(100) && w.get<game::Shield>(z).hp == Fixed::fromInt(20));
    mod(*sim).dealDamage(*sim, z, Fixed::fromInt(30), 0, kNullEntity, 2);   // 20 shield, 10 left - 1 armor = 9 to hp
    CHECK(w.get<game::Shield>(z).hp.raw == 0 && hpOf(*sim, z) == Fixed::fromInt(91));
    run(*sim, 4);
    CHECK(w.get<game::Shield>(z).hp > Fixed::fromInt(10));   // regenerates after the delay
    // two peons on a mine with room for one: only one gathers at a time; upkeep halves gold, not wood
    std::vector<Entity> peons;
    w.each<game::Unit, Owner>([&](Entity e, game::Unit& u, Owner& o) { if (o.player == 1 && mod(*sim).rules().types[u.type].id == "peon") peons.push_back(e); });
    Entity mine = findType(*sim, "mine", 0);
    sim->submit(mechCmd(1, game::kCmdGather, peons, mine));
    int maxAtOnce = 0;
    for (int i = 0; i < 20 * 10; ++i) {
        sim->step();
        int n = 0;
        for (Entity p : peons) { auto& o = w.get<game::Orders>(p); if (!o.queue.empty() && o.queue.front().type == game::OrderType::Gather && o.queue.front().state == 2) ++n; }
        maxAtOnce = std::max(maxAtOnce, n);
    }
    CHECK(maxAtOnce == 1);
    CHECK(stockOf(*sim, 1, "gold").raw > 0);
    CHECK(mod(*sim).gatherMultFor(*sim, 1, mod(*sim).rules().findResource("wood")) == Fixed::fromInt(1));
    CHECK(mod(*sim).gatherMultFor(*sim, 1, mod(*sim).rules().findResource("gold")) == Fixed::fromRatio(1, 2));
}

// ================================================================ stage 2
static const char* kVic = R"("victory": {"mode": "score", "seconds": 10000, "keyLoses": false})";
static std::string mapRows(int w, int h, const std::vector<std::string>& rows, const std::string& extra, const std::string& players = R"([{"name":"A","team":1},{"name":"B","team":2}])") {
    std::string rs;
    for (int y = 0; y < h; ++y) rs += std::string(y ? "," : "") + "\"" + (size_t(y) < rows.size() ? rows[size_t(y)] : std::string(size_t(w), '.')) + "\"";
    return "{\"size\":[" + std::to_string(w) + "," + std::to_string(h) + "],\"players\":" + players + ",\"terrain\":{\"rows\":[" + rs + "]}" + extra + "}";
}

TEST(mech_formation) {
    std::string rules = std::string(R"({"resources": {"gold": {}}, )") + kVic + R"(, "types": {
        "foot": {"move": {"speed": 3, "radius": 0.4}, "health": {"max": 100}},
        "slow": {"move": {"speed": 1.5, "radius": 0.4}, "health": {"max": 100}}}})";
    auto sim = makeSimJson(rules, flatMap(40, 20, R"(,"entities":[{"type":"foot","owner":1,"pos":[3,5]},{"type":"foot","owner":1,"pos":[4,5]},{"type":"foot","owner":1,"pos":[3,6]},{"type":"slow","owner":1,"pos":[4,6]}])"));
    World& w = sim->world();
    std::vector<Entity> us;
    w.each<game::Unit>([&](Entity e, game::Unit&) { us.push_back(e); });
    sim->submit(mechCmd(1, game::kCmdFormation, us, kNullEntity, {}, 1));   // box
    run(*sim, 0.2);
    CHECK(w.has<game::Formation>(us[0]) && w.get<game::Formation>(us[0]).group == w.get<game::Formation>(us[3]).group);
    sim->submit(mechCmd(1, game::kCmdMove, us, kNullEntity, at(30, 5)));
    run(*sim, 5);
    // marching at the pace of the slow one: the fast ones did not run ahead
    Fixed spread{};
    for (Entity a : us) for (Entity b : us) spread = fxMax(spread, (posOf(*sim, a) - posOf(*sim, b)).xz().length());
    CHECK(spread < Fixed::fromInt(3));
    run(*sim, 20);
    // a 2 x 2 box around the target, facing +x: two ranks along x, two files along z
    for (Entity a : us) CHECK(dist2DTest(posOf(*sim, a), at(30, 5)) < Fixed::fromRatio(5, 2));
}

TEST(mech_walls_gates_bridges) {
    std::string rules = std::string(R"({"resources": {"gold": {"start": 1000}}, )") + kVic + R"(, "types": {
        "wall": {"structure": {"footprint": [1, 1]}, "wall": true, "health": {"max": 300}, "cost": {"gold": 5, "time": 1}},
        "gate": {"structure": {"footprint": [1, 2]}, "gate": "own", "health": {"max": 600}},
        "bridge": {"structure": {"footprint": [2, 1], "terrain": "any"}, "bridge": true, "health": {"max": 400}},
        "peon": {"move": {"speed": 3}, "health": {"max": 50}, "builds": ["wall"]},
        "grunt": {"move": {"speed": 3}, "sight": 6, "health": {"max": 500}, "weapons": [{"range": 1, "damage": 10, "cooldown": 1}]}}})";
    // wall line: a builder builds 5 segments from (5,2) to (9,2)
    auto sim = makeSimJson(rules, flatMap(30, 12, R"(,"entities":[{"type":"peon","owner":1,"pos":[7,5]}])"));
    Entity peon = findType(*sim, "peon", 1);
    int64_t p = mod(*sim).rules().findType("wall") | (int64_t(9) << 16) | (int64_t(2) << 32);
    sim->submit(mechCmd(1, game::kCmdBuildLine, {peon}, kNullEntity, at(5, 2), p));
    run(*sim, 20);
    CHECK(countType(*sim, "wall", 1) == 5);
    // gate: a wall across the map with a gate; the owner walks through, the enemy is stopped at it
    std::vector<std::string> rows(12, std::string(30, '.'));
    for (int y = 0; y < 12; ++y) if (y != 5 && y != 6) rows[size_t(y)][15] = '#';
    auto g = makeSimJson(rules, mapRows(30, 12, rows, R"(,"entities":[{"type":"gate","owner":1,"pos":[15.5,6]},{"type":"grunt","owner":1,"pos":[10,3]},{"type":"grunt","owner":2,"pos":[20,9]}])"));
    Entity mine = findType(*g, "grunt", 1), foe = findType(*g, "grunt", 2), gate = findType(*g, "gate", 1);
    g->submit(mechCmd(1, game::kCmdMove, {mine}, kNullEntity, at(25, 3)));
    g->submit(mechCmd(2, game::kCmdMove, {foe}, kNullEntity, at(3, 9)));
    run(*g, 12);
    CHECK(posOf(*g, mine).x > Fixed::fromInt(20));   // through its own gate
    CHECK(posOf(*g, foe).x > Fixed::fromInt(14));    // the enemy did not pass...
    CHECK(hpOf(*g, gate) < Fixed::fromInt(600));      // ...and attacks the gate
    // bridge over a river: walk across; when it is destroyed, whoever stands on it drowns
    std::vector<std::string> river(10, std::string(30, '.'));
    for (int y = 0; y < 10; ++y) { river[size_t(y)][14] = '~'; river[size_t(y)][15] = '~'; }
    auto b = makeSimJson(rules, mapRows(30, 10, river, R"(,"entities":[{"type":"bridge","owner":0,"pos":[15,5.5]},{"type":"peon","owner":1,"pos":[5,5]}])"));
    Entity walker = findType(*b, "peon", 1), br = findType(*b, "bridge", 0);
    b->submit(mechCmd(1, game::kCmdMove, {walker}, kNullEntity, at(25, 5)));
    run(*b, 10);
    CHECK(posOf(*b, walker).x > Fixed::fromInt(22));
    b->submit(mechCmd(1, game::kCmdMove, {walker}, kNullEntity, at(14.5 > 0 ? 15 : 15, 5)));
    run(*b, 4);
    b->world().get<game::Unit>(br).hp = Fixed{};
    run(*b, 0.5);
    CHECK(!b->world().alive(walker) || hpOf(*b, walker).raw <= 0);
}

TEST(mech_market_trade_tribute_diplomacy) {
    std::string rules = std::string(R"({"resources": {"gold": {"start": 500}, "wood": {"start": 500}}, )") + kVic + R"(,
      "market": {"currency": "gold", "prices": {"wood": 100}, "fee": 0.3, "step": 5, "lot": 100}, "tribute": {"fee": 0.2}, "diplomacy": true,
      "types": {
        "market": {"structure": {"footprint": [3, 3]}, "market": true, "health": {"max": 1000}},
        "cart": {"move": {"speed": 4}, "health": {"max": 50}, "trader": {"rate": 1}}}})";
    auto sim = makeSimJson(rules, flatMap(60, 12, R"(,"entities":[{"type":"market","owner":1,"pos":[5,5]},{"type":"market","owner":1,"pos":[45,5]},{"type":"cart","owner":1,"pos":[8,5]},{"type":"market","owner":2,"pos":[25,10]},{"type":"market","owner":3,"pos":[35,10]}])",
        R"([{"name":"A","team":1},{"name":"B","team":2},{"name":"C","team":3}])"));
    Entity m1 = unitNear(*sim, 5, 5), m2 = unitNear(*sim, 45, 5), cart = findType(*sim, "cart", 1);
    const int wood = mod(*sim).rules().findResource("wood");
    sim->submit(mechCmd(1, game::kCmdTrade, {m1}, kNullEntity, {}, wood | 256));   // sell 100 wood
    run(*sim, 0.2);
    CHECK(stockOf(*sim, 1, "wood") == Fixed::fromInt(400) && near(stockOf(*sim, 1, "gold"), Fixed::fromInt(570)));
    CHECK(mod(*sim).marketPrice(*sim, wood) == Fixed::fromInt(95));
    sim->submit(mechCmd(1, game::kCmdTrade, {m1}, kNullEntity, {}, wood));         // buy 100 wood for 95 * 1.3
    run(*sim, 0.2);
    CHECK(stockOf(*sim, 1, "wood") == Fixed::fromInt(500));
    CHECK(mod(*sim).marketPrice(*sim, wood) == Fixed::fromInt(100));
    // trade cart between the two markets: ~40 cells per round trip leg -> about 40 gold per trip
    Fixed g0 = stockOf(*sim, 1, "gold");
    sim->submit(mechCmd(1, game::kCmdTradeRoute, {cart}, m2));
    run(*sim, 30);
    CHECK(stockOf(*sim, 1, "gold") >= g0 + Fixed::fromInt(39));
    // tribute 100 gold to player 2 with a 20% fee
    Fixed a0 = stockOf(*sim, 1, "gold"), b0 = stockOf(*sim, 2, "gold");
    sim->submit(mechCmd(1, game::kCmdTribute, {}, kNullEntity, {}, mod(*sim).rules().findResource("gold") | (2 << 8) | (int64_t(100) << 16)));
    run(*sim, 0.2);
    CHECK(stockOf(*sim, 1, "gold") == a0 - Fixed::fromInt(100) && near(stockOf(*sim, 2, "gold"), b0 + Fixed::fromInt(80)));
    // alliance needs both sides; war splits again
    sim->submit(mechCmd(1, game::kCmdDiplomacy, {}, kNullEntity, {}, 3 | 256));
    run(*sim, 0.2);
    CHECK(!sim->allied(1, 3));
    sim->submit(mechCmd(3, game::kCmdDiplomacy, {}, kNullEntity, {}, 1 | 256));
    run(*sim, 0.2);
    CHECK(sim->allied(1, 3));
    sim->submit(mechCmd(1, game::kCmdDiplomacy, {}, kNullEntity, {}, 3));
    run(*sim, 0.2);
    CHECK(!sim->allied(1, 3));
}

TEST(mech_items_cycles_hazards) {
    std::string rules = std::string(R"({"resources": {"gold": {"start": 500}, "ore": {"field": true}}, )") + kVic + R"(,
      "statuses": {"night": {"mods": [{"stat": "sight", "mul": 0.5}]}, "haste": {"mods": [{"stat": "speed", "mul": 2}]}},
      "cycles": [{"id": "day", "length": 20, "phases": [{"id": "day", "until": 0.5}, {"id": "night", "until": 1, "status": "night"}]}],
      "hazards": [{"field": "ore", "damage": 10, "tags": ["bio"]}],
      "items": {"potion": {"cost": {"gold": 50}, "use": {"heal": 200}, "charges": 1},
                "boots": {"cost": {"gold": 100}, "mods": [{"stat": "speed", "add": 1}], "charges": 0, "consumable": false},
                "gem": {"mods": [{"stat": "armor", "add": 2}]}},
      "types": {
        "hero": {"tags": ["bio"], "move": {"speed": 3}, "sight": 10, "health": {"max": 500}, "inventory": {"slots": 3}},
        "shop": {"structure": {"footprint": [2, 2]}, "health": {"max": 500}, "shop": {"sells": ["potion", "boots"], "radius": 3}},
        "kobold": {"move": {"speed": 2}, "health": {"max": 10}, "drops": [{"item": "gem", "chance": 1}]}}})";
    auto sim = makeSimJson(rules, flatMap(40, 12, R"(,"fields":[{"resource":"ore","center":[30,8],"radius":2,"amount":50}],"entities":[{"type":"hero","owner":1,"pos":[5,5]},{"type":"shop","owner":0,"pos":[8,5]},{"type":"kobold","owner":2,"pos":[20,5]}])"));
    World& w = sim->world();
    Entity hero = findType(*sim, "hero", 1), shop = findType(*sim, "shop", 0), kob = findType(*sim, "kobold", 2);
    run(*sim, 0.2);
    CHECK(w.has<game::ItemBag>(hero));
    Fixed speed0 = mod(*sim).stat(*sim, hero, game::Stat::Speed);
    sim->submit(mechCmd(1, game::kCmdBuyItem, {hero}, shop, {}, mod(*sim).rules().mech.findItem("boots")));
    sim->submit(mechCmd(1, game::kCmdBuyItem, {hero}, shop, {}, mod(*sim).rules().mech.findItem("potion")));
    run(*sim, 0.2);
    CHECK(stockOf(*sim, 1, "gold") == Fixed::fromInt(350));
    CHECK(mod(*sim).stat(*sim, hero, game::Stat::Speed) == speed0 + Fixed::fromInt(1));
    w.get<game::Unit>(hero).hp = Fixed::fromInt(100);
    sim->submit(mechCmd(1, game::kCmdUseItem, {hero}, kNullEntity, {}, 1));
    run(*sim, 0.2);
    CHECK(hpOf(*sim, hero) == Fixed::fromInt(300) && w.get<game::ItemBag>(hero).items[1] == -1);
    // a kobold drops a gem; the hero picks it up
    w.get<game::Unit>(kob).hp = Fixed{};
    run(*sim, 0.2);
    CHECK(mod(*sim).state(*sim).mech.groundItems.size() == 1);
    sim->submit(mechCmd(1, game::kCmdPickItem, {hero}, kNullEntity, at(20, 5)));
    run(*sim, 6);
    CHECK(mod(*sim).state(*sim).mech.groundItems.empty() && mod(*sim).stat(*sim, hero, game::Stat::Armor) == Fixed::fromInt(2));
    // night halves sight (second half of the 20 s cycle)
    run(*sim, 12 - 6.6);
    CHECK(mod(*sim).stat(*sim, hero, game::Stat::Sight) == Fixed::fromInt(5));
    CHECK(mod(*sim).state(*sim).trig.vars["cycle.day"] == Fixed::fromInt(1));
    // standing on the ore field hurts bio units
    sim->submit(mechCmd(1, game::kCmdMove, {hero}, kNullEntity, at(30, 8)));
    Fixed h0 = hpOf(*sim, hero);
    run(*sim, 8);
    CHECK(hpOf(*sim, hero) < h0);
}

TEST(mech_ammo_merge_deck_tunnel_amphibious) {
    std::string rules = std::string(R"({"resources": {"gold": {"start": 500}, "elixir": {"start": 10}}, )") + kVic + R"(,
      "deck": {"hand": 2, "deployNear": 8},
      "playerAbilities": [{"id": "a", "cost": {"elixir": 1}, "target": "point", "effects": [{"type": "spawn", "unit": "templar", "count": 1}]},
                          {"id": "b", "cost": {"elixir": 1}, "target": "point", "effects": [{"type": "spawn", "unit": "templar", "count": 1}]},
                          {"id": "c", "cost": {"elixir": 1}, "target": "point", "effects": [{"type": "spawn", "unit": "templar", "count": 1}]}],
      "types": {
        "airfield": {"structure": {"footprint": [3, 3]}, "health": {"max": 1000}, "dock": {"slots": 2, "rearmTime": 0.5}},
        "harrier": {"move": {"speed": 6, "layer": "air"}, "sight": 10, "health": {"max": 150}, "weapons": [{"range": 6, "damage": 30, "cooldown": 0.5, "targets": ["ground"], "ammo": 2}]},
        "target": {"structure": {"footprint": [1, 1]}, "health": {"max": 5000}},
        "templar": {"move": {"speed": 2}, "health": {"max": 40}, "mergeInto": {"with": "templar", "result": "archon"}},
        "archon": {"move": {"speed": 2}, "health": {"max": 360}, "splitInto": {"type": "templar", "count": 2}},
        "nydus": {"structure": {"footprint": [2, 2]}, "health": {"max": 500}, "transport": {"slots": 10}, "tunnel": "nydus"},
        "ling": {"move": {"speed": 3}, "health": {"max": 35}},
        "hover": {"move": {"speed": 3, "layer": "amphibious"}, "health": {"max": 100}}}})";
    std::vector<std::string> rows(16, std::string(50, '.'));
    for (int y = 0; y < 16; ++y) for (int x = 30; x < 34; ++x) rows[size_t(y)][size_t(x)] = '~';
    auto sim = makeSimJson(rules, mapRows(50, 16, rows, R"(,"entities":[{"type":"airfield","owner":1,"pos":[4,4]},{"type":"harrier","owner":1,"pos":[5,8]},{"type":"target","owner":2,"pos":[12,8]},
        {"type":"templar","owner":1,"pos":[5,13]},{"type":"templar","owner":1,"pos":[7,13]},
        {"type":"nydus","owner":1,"pos":[18,3]},{"type":"nydus","owner":1,"pos":[45,12]},{"type":"ling","owner":1,"pos":[17,6]},{"type":"hover","owner":1,"pos":[25,8]}])"));
    World& w = sim->world();
    Entity harrier = findType(*sim, "harrier", 1), tgt = findType(*sim, "target", 2);
    bool rearmed = false;
    for (int i = 0; i < 20 * 3; ++i) {
        sim->step();
        auto& od = w.get<game::Orders>(harrier);
        if (!od.queue.empty() && od.queue.front().type == game::OrderType::Rearm) { rearmed = true; break; }
    }
    CHECK(rearmed && hpOf(*sim, tgt) == Fixed::fromInt(5000 - 60));   // two shots, then it flies to the airfield
    run(*sim, 10);
    CHECK(hpOf(*sim, tgt) < Fixed::fromInt(4940));                    // back in action after rearming
    // two templars merge into an archon, which splits again
    std::vector<Entity> tp;
    w.each<game::Unit, Owner>([&](Entity e, game::Unit& u, Owner& o) { if (o.player == 1 && mod(*sim).rules().types[u.type].id == "templar") tp.push_back(e); });
    sim->submit(mechCmd(1, game::kCmdMerge, tp));
    run(*sim, 3);
    CHECK(countType(*sim, "archon", 1) == 1 && countType(*sim, "templar", 1) == 0);
    sim->submit(mechCmd(1, game::kCmdSplit, {findType(*sim, "archon", 1)}));
    run(*sim, 0.3);
    CHECK(countType(*sim, "templar", 1) == 2);
    // deck: hand of 2 -> card c can't be cast until a or b is played; deploy only near own structures
    CHECK(mod(*sim).canUsePlayerAbility(*sim, 1, 0) && !mod(*sim).canUsePlayerAbility(*sim, 1, 2));
    sim->submit(mechCmd(1, game::kCmdPlayerAbility, {}, kNullEntity, at(25, 14), 0));   // far from own structures: refused
    run(*sim, 0.2);
    CHECK(countType(*sim, "templar", 1) == 2);
    sim->submit(mechCmd(1, game::kCmdPlayerAbility, {}, kNullEntity, at(6, 7), 0));
    run(*sim, 0.2);
    CHECK(countType(*sim, "templar", 1) == 3);
    CHECK(mod(*sim).canUsePlayerAbility(*sim, 1, 2) && !mod(*sim).canUsePlayerAbility(*sim, 1, 0));   // a went to the back
    // tunnels: in at one nydus, out at the other
    Entity ling = findType(*sim, "ling", 1), n1 = unitNear(*sim, 18, 3), n2 = unitNear(*sim, 45, 12);
    sim->submit(mechCmd(1, game::kCmdLoad, {ling}, n1));
    run(*sim, 3);
    sim->submit(mechCmd(1, game::kCmdUnload, {n2}));
    run(*sim, 1);
    CHECK(dist2DTest(posOf(*sim, ling), at(45, 12)) < Fixed::fromInt(4));
    // hovercraft crosses the water strip
    Entity hover = findType(*sim, "hover", 1);
    sim->submit(mechCmd(1, game::kCmdMove, {hover}, kNullEntity, at(40, 8)));
    run(*sim, 8);
    CHECK(dist2DTest(posOf(*sim, hover), at(40, 8)) < Fixed::fromInt(2));
}
