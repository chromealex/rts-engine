// Components of the agent-mech mechanics (RtsMech*.cpp). Optional: an object carries them only when
// its game uses the mechanic, and they are hashed only when present.
#pragma once
#include <string>
#include <vector>
#include "../../engine/core/ECS.h"
#include "../../engine/core/Fixed.h"

namespace rts::game {

// Stance (Age of Empires, Warcraft): how an idle unit reacts to enemies.
struct Stance { uint8_t mode = 0; Vec3 home{}; bool homeSet = false; };
// Retreating to the nearest own retreat point: faster, takes less damage, does not fight.
struct Retreating { Entity to = kNullEntity; Vec3 point{}; };
// A transport (or garrisoned structure) and its passengers, in boarding order.
struct Transport { std::vector<Entity> cargo; int used = 0; Vec3 unloadAt{}; bool unloading = false, unloadAtPoint = false; uint32_t nextUnload = 0; };
// A passenger inside a transport (flag kUnitInCargo): off the map, moves with it.
struct Boarded { Entity carrier = kNullEntity; bool fires = false; };
// Veterancy of an ordinary unit: rank (1 = recruit) from kills of the last unit that hit the victim.
struct Veteran { int level = 1; Fixed xp{}; Entity lastHit = kNullEntity; };
// Extra rally waypoints of a producer (Shift + rally): new units walk them in order after the rally point.
struct RallyPath { std::vector<Vec3> points; };
// StarCraft shields: absorb damage first, regenerate after `delay` seconds without being hit.
struct MechShield { Fixed hp{}; uint32_t lastHit = 0; };   // agent-mech shields (type mech.shieldMax); originals has its own Shield

// ---- stage 2
struct Formation { uint32_t group = 0; uint8_t shape = 0; Vec3 offset{}; };       // offset in formation space (x right, z back)
struct BridgeLaid { bool laid = false; std::vector<int32_t> cells; };   // gates and bridges: footprint opened; bridge: cells made ground
struct Ammo { int left[4] = {0, 0, 0, 0}; bool init = false; };
struct Merging { Entity partner = kNullEntity; };
struct Trader { Entity home = kNullEntity, dest = kNullEntity; bool loaded = false; };
struct ItemBag { std::vector<int16_t> items, charges; };                        // per slot (-1 empty)
struct GroundItem { Vec3 pos{}; uint16_t item = 0; int charges = 0; };

// Global mech state (inside GameState; hashed when used).
struct MechState {
    std::vector<Fixed> prices;                     // market price per resource (in the currency), shared
    std::vector<GroundItem> groundItems;
    std::vector<std::vector<int>> decks;           // per player slot: player ability indices, hand = first N
    std::vector<std::vector<uint8_t>> allyRequest; // [a][b]: a asked b for an alliance
    std::vector<int> cyclePhase;                   // per cycle: current phase (-1 before the first tick)
    uint32_t nextGroup = 1;                        // formation groups
    // campaigns (trigger actions objective / dialog / endMission)
    struct Objective { std::string id, text; uint8_t state = 0, player = 0; bool primary = true; };   // state 0 active, 1 done, 2 failed
    struct Dialog { std::string speaker, portrait, voice, text; Fixed duration{}; };
    std::vector<Objective> objectives;
    std::vector<Dialog> dialogs;                   // every dialog line shown so far (kEvDialog value = index)
    std::string nextMission;                       // endMission "next"
    int8_t missionResult = 0;                      // 1 victory, -1 defeat (endMission), 0 running
    std::vector<std::pair<uint32_t, Fixed>> groupSpeed;   // derived each tick (not hashed)
};

} // namespace rts::game
