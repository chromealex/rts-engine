// ------------------------------------------------------------------ foundations, traps, group teleports, charges (RtsFidelity.cpp)
// Included by tests.cpp after the fidelity helpers (spawnAt).

static int pavedCells(Simulation& sim, uint8_t owner) {
    auto& g = mod(sim).state(sim).grid;
    int n = 0;
    for (auto f : g.foundation) if (f == owner) ++n;
    return n;
}

TEST(fid_concrete_slabs_and_weathering) {
    const char* rules = R"({"victory": "noUnits", "foundation": {"startHp": 0.75, "decay": 6, "floor": 0.5},
      "types": {
        "slab": {"structure": {"footprint": [2, 2]}, "health": {"max": 20}, "foundation": true},
        "yard": {"structure": {"footprint": [2, 2]}, "health": {"max": 400}},
        "wall": {"structure": {"footprint": [1, 1]}, "health": {"max": 100}, "weathers": false},
        "tank": {"move": {"speed": 2}, "health": {"max": 100}}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, R"(,"entities":[{"type":"yard","owner":1,"pos":[28,12]},{"type":"tank","owner":2,"pos":[30,2]}])"));
    auto& m = mod(*sim);
    auto& w = sim->world();
    CHECK(m.rules().fidelity && m.rules().foundation.enabled);
    CHECK(pavedCells(*sim, 1) == 4);                         // the starting base stands on concrete
    run(*sim, 0.2);
    spawnAt(*sim, "slab", 1, 4, 4);
    run(*sim, 0.2);
    CHECK(pavedCells(*sim, 1) == 8);                         // the slab paved its 2x2...
    CHECK(countType(*sim, "slab", 1) == 0);                  // ...and went away
    Entity onPad = spawnAt(*sim, "yard", 1, 4, 4);
    CHECK(!w.has<game::Weathering>(onPad) && w.get<game::Unit>(onPad).hp == Fixed::fromInt(400));
    Entity bare = spawnAt(*sim, "yard", 1, 10, 4);
    CHECK(w.has<game::Weathering>(bare) && w.get<game::Unit>(bare).hp == Fixed::fromInt(300));   // starts at 75%
    spawnAt(*sim, "slab", 1, 16, 4);
    run(*sim, 0.1);
    Entity half = spawnAt(*sim, "yard", 1, 17, 4);           // two of its four cells on concrete
    CHECK(w.get<game::Unit>(half).hp == Fixed::fromInt(350));
    Entity wall = spawnAt(*sim, "wall", 1, 22.5, 4.5);
    run(*sim, 2);
    Fixed hb = w.get<game::Unit>(bare).hp;
    CHECK(hb < Fixed::fromInt(260) && hb > Fixed::fromInt(200));   // weathering: 10% of max hp per second when fully bare
    run(*sim, 5);
    CHECK(w.get<game::Unit>(bare).hp == Fixed::fromInt(200));     // down to the floor and no further
    CHECK(w.get<game::Unit>(half).hp == Fixed::fromInt(300));     // half bare: floor at 75%
    CHECK(w.get<game::Unit>(wall).hp == Fixed::fromInt(100) && w.get<game::Unit>(onPad).hp == Fixed::fromInt(400));
    // a destroyed structure takes its concrete with it
    const int before = pavedCells(*sim, 1);
    w.get<game::Unit>(onPad).hp = Fixed{};
    run(*sim, 0.1);
    CHECK(pavedCells(*sim, 1) == before - 4);
}

TEST(fid_ai_paves_before_building) {
    const char* rules = R"({"victory": "noStructures", "foundation": true,
      "resources": {"credits": {"start": 3000}},
      "types": {
        "cy": {"structure": {"footprint": [2, 2]}, "health": {"max": 1000}, "produces": ["slab", "plant"]},
        "slab": {"structure": {"footprint": [2, 2]}, "health": {"max": 20}, "foundation": true, "cost": {"credits": 20, "time": 1}},
        "plant": {"structure": {"footprint": [2, 2]}, "health": {"max": 400}, "cost": {"credits": 300, "time": 2}}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 32, R"(,"entities":[{"type":"cy","owner":1,"pos":[6,6]},{"type":"cy","owner":2,"pos":[26,26]}])"));
    auto& m = mod(*sim);
    const int base = pavedCells(*sim, 1);
    // the AI keeps a few cells paved and puts its buildings on them
    game::RtsAI ai(1, 7);
    for (int i = 0; i < 25 * 40; ++i) { ai.update(*sim); sim->step(); }
    CHECK(pavedCells(*sim, 1) > base);
    Vec3 p;
    CHECK(m.findAutoSpot(*sim, 1, uint16_t(m.rules().findType("plant")), p));
    int x0, y0; game::RtsModule::footprintAt(m.rules().types[size_t(m.rules().findType("plant"))], p, x0, y0);
    CHECK(m.bareCells(*sim, 1, x0, y0, 2, 2) < 4);           // the next building lands on concrete
}

TEST(fid_traps_hidden_charges_rearm) {
    const char* rules = R"({"victory": "noUnits",
      "types": {
        "spikes": {"tags": ["trap"], "structure": {"footprint": [1, 1]}, "health": {"max": 50},
                   "trap": {"radius": 1.2, "charges": 2, "rearm": 1, "revealFor": 0.5, "effects": [{"type": "damage", "amount": 30}]}},
        "imp": {"tags": ["infantry"], "move": {"speed": 1, "radius": 0.3}, "health": {"max": 100}},
        "tower": {"structure": {"footprint": [1, 1]}, "health": {"max": 500}, "sight": 9, "weapons": [{"range": 6, "damage": 5, "cooldown": 1}]}
      }})";
    auto sim = makeSimJson(rules, flatMap(32, 16, ""));
    auto& m = mod(*sim);
    auto& w = sim->world();
    run(*sim, 0.1);
    Entity trap = spawnAt(*sim, "spikes", 1, 10.5, 8.5);
    Entity mine = spawnAt(*sim, "imp", 1, 10.5, 9.6);          // own units never set it off
    Entity archer = spawnAt(*sim, "tower", 2, 10.5, 2.5);     // the trap is in range, but it can't see it
    CHECK((m.statusFlags(*sim, trap) & game::kStInvisible) != 0);
    run(*sim, 1);
    CHECK(w.get<game::Unit>(mine).hp == Fixed::fromInt(100));
    CHECK(w.get<game::Unit>(trap).hp == Fixed::fromInt(50));  // hidden: not attacked
    Entity foe = spawnAt(*sim, "imp", 2, 11.3, 8.5);
    run(*sim, 0.2);
    CHECK(w.get<game::Unit>(foe).hp == Fixed::fromInt(70));   // fired
    CHECK(w.get<game::TrapState>(trap).charges == 1);
    CHECK((m.statusFlags(*sim, trap) & game::kStRevealed) != 0); // exposed for a moment
    run(*sim, 0.5);
    CHECK(w.get<game::Unit>(foe).hp == Fixed::fromInt(70));   // rearming
    run(*sim, 0.6);
    CHECK(w.get<game::Unit>(foe).hp == Fixed::fromInt(40));   // second charge
    run(*sim, 0.2);
    CHECK(!w.alive(trap));                                     // used up
    (void)archer;
}

TEST(fid_inject_larvae_over_the_cap) {
    const char* rules = R"({"victory": "noUnits",
      "statuses": {"injected": {"onExpire": [{"type": "addCharges", "amount": 3, "limit": 19, "affects": "own"}]}},
      "types": {
        "hatch": {"structure": {"footprint": [3, 3]}, "health": {"max": 1500}, "produces": ["ling"], "charges": {"max": 3, "interval": 100}},
        "ling": {"move": {"speed": 3}, "health": {"max": 35}},
        "queen": {"move": {"speed": 1}, "health": {"max": 175}, "energy": {"max": 200, "regen": 0, "start": 30},
                  "abilities": [{"id": "inject", "target": "unit", "range": 4, "cooldown": 1, "energy": 25, "smart": "ownProducer",
                                 "effects": [{"type": "status", "status": "injected", "duration": 2, "affects": "own"}]}]}
      }})";
    auto sim = makeSimJson(rules, flatMap(24, 16, R"(,"entities":[{"type":"hatch","owner":1,"pos":[6,8]},{"type":"queen","owner":1,"pos":[9,8]},{"type":"ling","owner":2,"pos":[22,2]}])"));
    auto& w = sim->world();
    Entity hatch;
    w.each<game::Production, game::Unit>([&](Entity e, game::Production&, game::Unit&) { hatch = e; });
    CHECK(w.get<game::Production>(hatch).charges == 3);
    run(*sim, 1);
    CHECK(w.get<game::Production>(hatch).charges == 3);      // injected, larvae not out yet
    run(*sim, 2);
    CHECK(w.get<game::Production>(hatch).charges == 6);      // three more, over the natural cap of 3
    Entity queen;
    w.each<game::AbilityState, game::Unit>([&](Entity e, game::AbilityState&, game::Unit&) { queen = e; });
    CHECK(w.get<game::Unit>(queen).energy == Fixed::fromInt(5));   // one cast only (not while injected / no energy)
}

TEST(fid_group_teleport_chronosphere_and_recall) {
    const char* rules = R"({"victory": "noStructures",
      "playerAbilities": [{"id": "chrono", "target": "pointPair", "cooldown": 1,
                           "effects": [{"type": "teleportGroup", "radius": 2, "killTags": ["infantry"], "duration": 3}]}],
      "types": {
        "tank": {"tags": ["vehicle"], "move": {"speed": 1, "radius": 0.4}, "health": {"max": 300}, "weapons": [{"range": 1, "damage": 1, "cooldown": 9}]},
        "rifle": {"tags": ["infantry"], "move": {"speed": 1, "radius": 0.3}, "health": {"max": 50}},
        "hq": {"structure": {"footprint": [2, 2]}, "health": {"max": 1000}},
        "ship": {"move": {"speed": 1, "radius": 0.5}, "health": {"max": 500},
                 "abilities": [{"id": "recall", "target": "point", "range": 40, "cooldown": 5, "autocast": false,
                                "effects": [{"type": "teleportGroup", "radius": 2, "origin": "point", "destination": "caster"}]}]}
      }})";
    auto sim = makeSimJson(rules, flatMap(40, 24, R"(,"entities":[{"type":"hq","owner":1,"pos":[2,22]},{"type":"hq","owner":2,"pos":[36,20]}])"));
    auto& m = mod(*sim);
    auto& w = sim->world();
    run(*sim, 0.1);
    Entity t1 = spawnAt(*sim, "tank", 1, 5.5, 5.5), t2 = spawnAt(*sim, "tank", 1, 6.5, 5.5);
    Entity inf = spawnAt(*sim, "rifle", 1, 5.5, 6.5);
    Entity foe = spawnAt(*sim, "tank", 2, 5.5, 4.5);
    Command c; c.player = 1; c.type = game::kCmdPlayerAbility;
    c.param = 0 | game::RtsModule::packPoint2({Fixed::fromInt(20), Fixed{}, Fixed::fromInt(12)});
    c.point = {Fixed::fromInt(6), Fixed{}, Fixed::fromRatio(55, 10)};
    sim->submit(c);
    run(*sim, 0.5);
    auto near = [&](Entity e, int x, int z, int r) { auto p = w.get<Transform>(e).pos; return (p - Vec3{Fixed::fromInt(x), Fixed{}, Fixed::fromInt(z)}).xz().length() <= Fixed::fromInt(r); };
    CHECK(near(t1, 20, 12, 2) && near(t2, 20, 12, 2));        // moved, formation kept
    CHECK(!w.alive(inf) || w.get<game::Unit>(inf).hp.raw <= 0); // infantry doesn't survive the trip
    CHECK(near(foe, 5, 4, 1));                                 // enemies stay
    run(*sim, 3);
    CHECK(near(t1, 6, 6, 1) && near(t2, 6, 6, 2));            // back home after the duration
    // AI target: the own group to a spot just short of the enemy base
    Vec3 from, to;
    CHECK(m.pairTarget(*sim, 1, m.rules().playerAbilities[0].payload, from, to));
    CHECK(near(t1, int(from.x.toInt()), int(from.z.toInt()), 2) && (to - Vec3{Fixed::fromInt(36), Fixed{}, Fixed::fromInt(20)}).xz().length() < Fixed::fromInt(7));
    // a unit ability: units around a point come to the caster
    Entity ship = spawnAt(*sim, "ship", 1, 30.5, 4.5);
    m.castAbility(*sim, ship, 0, kNullEntity, Vec3{Fixed::fromInt(6), Fixed{}, Fixed::fromInt(6)});
    run(*sim, 0.5);
    CHECK(near(t1, 30, 4, 3) && near(t2, 30, 4, 3));
}
