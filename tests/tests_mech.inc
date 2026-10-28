// agent-mech tests (included at the end of tests.cpp: uses its helpers). One block per mechanic.

static Command mechCmd(uint8_t player, uint16_t type, std::vector<Entity> units, Entity target = kNullEntity, Vec3 point = {}, int64_t param = 0) {
    Command c; c.player = player; c.type = type; c.units = std::move(units); c.target = target; c.point = point; c.param = param;
    return c;
}
static Vec3 at(int x, int z) { return {Fixed::fromInt(x), Fixed{}, Fixed::fromInt(z)}; }
static Fixed hpOf(Simulation& sim, Entity e) { return sim.world().alive(e) ? sim.world().get<game::Unit>(e).hp : Fixed{}; }
static Vec3 posOf(Simulation& sim, Entity e) { return sim.world().get<Transform>(e).pos; }

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
