// Data-driven triggers (event -> conditions -> actions), the escape hatch for mechanics that the
// fixed type blocks don't cover: campaign scripts, bonuses, custom win conditions, map events.
// Declared in rules.json ("triggers") and in maps ("triggers", "areas"). Deterministic: evaluated
// inside the simulation tick with fixed-point math and the shared RNG, so they replay exactly.
//
//   {"id": "farm-bonus", "once": false, "forEachPlayer": true,
//    "event": {"type": "built", "unit": "farm"},
//    "conditions": [{"type": "count", "unit": "farm", ">=": 5}],
//    "actions": [{"type": "give", "resource": "gold", "amount": 200},
//                {"type": "message", "text": "Урожайный год! +200 золота"}]}
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "../../engine/core/Fixed.h"
#include "../../engine/data/Json.h"
#include "RtsData.h"

namespace rts::game {

enum class TrigEvent : uint8_t {
    Start, Time, Every, EnterArea, LeaveArea, UnitDied, Built, Trained, Researched, Captured,
    Resource, Variable, Wave, RegionClaimed, PlayerDefeated, Ability, LevelUp,
};

enum class TrigCmp : uint8_t { Ge, Gt, Le, Lt, Eq, Ne };

// Who an action/condition is about.
//   "triggering" (default): the player of the event; "owner": owner of the triggering unit;
//   "all": every player; "enemies"/"allies": relative to the triggering player; or a slot number.
struct TrigPlayer { int8_t slot = -1; uint8_t rel = 0; }; // rel: 0 triggering, 1 all, 2 enemies, 3 allies, 4 fixed slot, 5 neutral-hostile

struct TrigArea { Fixed x{}, z{}, w{}, h{}; bool contains(Vec3 p) const { return p.x >= x && p.z >= z && p.x < x + w && p.z < z + h; } };

struct TrigFilter {                // unit filter
    uint16_t type = kNoType;
    uint64_t tags = 0;
    bool structures = false, units = false; // both false = anything
    bool matches(const Rules& r, uint16_t t) const;
};

struct TrigCondition {
    enum Kind : uint8_t { Count, Resource, Variable, Researched, Random, Time, Alive, RegionsOwned,
                          Script /* {"type": "script", "fn": ...}: a Lua function's result (RtsScript.h) */ } kind = Count;
    TrigPlayer player;
    TrigFilter filter;
    int area = -1;
    int16_t index = -1;            // resource / tech
    std::string name;              // variable / script function
    TrigCmp cmp = TrigCmp::Ge;
    Fixed value{};
    bool negate = false;
    Json args;                     // script: the condition's JSON (ctx.args)
};

struct TrigAction {
    enum Kind : uint8_t {
        Spawn, Give, Take, SetOwner, Kill, Remove, Damage, Heal, Buff, Message, SetVariable, AddVariable,
        Research, EnableType, DisableType, Victory, Defeat, EnableTrigger, DisableTrigger, AddScore,
        SetRegionOwner, AttackMove, MoveUnits, SetTickets,
        CastAbility,       // a player ability (map / timer skills): "ability", "at" / "area" (free: "free": true)
        Script,            // {"type": "script", "fn": "name", ...}: calls a Lua function of the game's scripts
    } kind = Message;
    TrigPlayer player;
    TrigFilter filter;
    int area = -1;                 // target area (units inside / spawn position)
    bool triggeringUnit = false;   // target the event's unit
    uint16_t type = kNoType;
    int count = 1;
    int16_t index = -1;            // resource / tech / region
    Fixed amount{};
    Fixed duration{};
    StatMod mod;
    Vec3 at{};
    bool hasAt = false;
    bool attackMove = false;
    std::string text, name;        // message / variable / trigger id / script function
    Json args;                     // script: the action's JSON (ctx.args)
};

struct TriggerDef {
    std::string id;
    bool once = true;
    bool enabled = true;
    bool forEachPlayer = false;    // conditions/actions evaluated per player as "triggering"
    TrigEvent event = TrigEvent::Start;
    Fixed seconds{};               // Time / Every
    int area = -1;
    TrigFilter filter;
    TrigPlayer player;             // event filter (whose units/events)
    int16_t index = -1;            // resource / tech
    std::string name;              // variable
    TrigCmp cmp = TrigCmp::Ge;
    Fixed value{};
    std::vector<TrigCondition> conditions;
    std::vector<TrigAction> actions;
};

struct TriggerSet {
    std::vector<TriggerDef> defs;
    std::vector<std::string> areaNames;
    std::vector<TrigArea> areas;
    int findArea(const std::string& n) const;
    int findTrigger(const std::string& id) const;
};

// Parse rules-level and map-level triggers (map areas are shared). Throws std::runtime_error.
void loadTriggers(const Rules& rules, const Json& rulesJson, const Json& mapJson, TriggerSet& out);

// Runtime state (lives in GameState so it is hashed and part of the simulation).
struct TriggerState {
    std::vector<uint8_t> enabled;               // per def
    std::vector<std::vector<uint8_t>> fired;    // per def per player slot (once-triggers)
    std::vector<std::vector<uint64_t>> inside;  // per def: entities currently inside its area (sorted)
    std::vector<std::vector<uint8_t>> edge;     // per def per player: last condition value for edge events
    std::map<std::string, Fixed> vars;          // global variables (deterministic ordering)
    std::vector<std::string> messages;          // text table for kEvMessage (value = index)
    bool started = false;
};

} // namespace rts::game
