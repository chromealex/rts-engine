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
struct Shield { Fixed hp{}; uint32_t lastHit = 0; };

// ---- stage 2
struct Formation { uint32_t group = 0; uint8_t shape = 0; Vec3 offset{}; };       // offset in formation space (x right, z back)
struct BridgeLaid { bool laid = false; std::vector<int32_t> cells; };   // gates and bridges: footprint opened; bridge: cells made ground
struct Ammo { int left[4] = {0, 0, 0, 0}; bool init = false; };
struct Merging { Entity partner = kNullEntity; };
struct Trader { Entity home = kNullEntity, dest = kNullEntity; bool loaded = false; };
struct ItemBag { std::vector<int16_t> items, charges; };                        // per slot (-1 empty)
struct GroundItem { Vec3 pos{}; uint16_t item = 0; int charges = 0; };

// ---- stage 3
struct Suppression { Fixed value{}; };                    // 0..100: suppressed / pinned thresholds in rules
struct Turret { Vec3 dir[4] = {}; bool init = false; };   // per weapon aim direction (turnRate)
struct Abandoned { uint8_t formerOwner = 0; };            // crew killed: neutral until infantry re-crews it
struct GrowState { Fixed timer{}; };
struct Bounty { Vec3 pos{}; Entity target = kNullEntity; uint8_t player = 0; int16_t res = -1; Fixed reward{}; };
struct RoundUnit { uint8_t owner = 0; uint16_t type = 0; Vec3 pos{}; };

// ---- stage 4: battle state of a soldier (Total War): morale, fatigue, charge run-up, supply
enum class MoraleMode : uint8_t { Steady = 0, Wavering = 1, Routing = 2 };
struct Morale {
    Fixed value{}, fatigue{}, run{};
    Vec3 lastPos{};
    uint32_t chargeTick = 0, lastCharge = 0, lastFire = 0;
    uint8_t state = 0;                 // MoraleMode
    bool init = false, outOfSupply = false;
};
// Logistics: a conveyor piece's direction (1 east +x, 2 south +z, 3 west, 4 north) and a producer / crafter.
struct Belt { uint8_t dir = 1; };
struct Crafter { std::vector<Fixed> in, out; Fixed timer{}, outTimer{}; };
// Turn-based mode: what a unit may still do this turn.
struct TurnBudget { Fixed move{}; int shots = 0; Vec3 lastPos{}; uint32_t turn = 0; };
// Free 3D flight: the altitude the unit climbs / dives to.
struct Altitude { Fixed target{}, y{}; };

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
    // stage 3
    std::vector<uint8_t> cover;                    // per cell cover level (map "cover", craters)
    std::vector<Bounty> bounties;
    uint8_t roundPhase = 0;                        // 0 prepare, 1 fight, 2 over
    int roundIndex = 0;
    uint32_t phaseEnd = 0;
    std::vector<RoundUnit> roundSnapshot;
    std::vector<std::pair<uint32_t, Fixed>> groupSpeed;   // derived each tick (not hashed)
    // stage 4 logistics: one packet per belt cell
    std::vector<int16_t> beltRes;                  // per cell: resource of the packet (-1 none)
    std::vector<Fixed> beltAmt, beltProg;          // amount, progress along the cell 0..1
    std::vector<std::pair<int32_t, uint8_t>> beltPlan;   // cell -> direction for belts ordered by build_line
    // turn-based mode
    uint8_t turnPlayer = 0;                        // whose turn (0 before the first)
    uint32_t turnIndex = 0, turnStart = 0;
};

} // namespace rts::game
