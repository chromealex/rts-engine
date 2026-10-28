// Included by RtsData.h after Stat / StatMod / layers.
// Data of the mechanics added for the "top-100 strategy games" plan (agent-mech): stances, retreat,
// transports and garrisoned infantry, ... Parsed in RtsMechData.cpp from optional JSON blocks; a game
// that uses none of them loads, plays and hashes exactly as before.
#pragma once
#include <cstdint>
#include <vector>
#include "../../engine/core/Fixed.h"

namespace rts::game {

enum class StanceMode : uint8_t { Aggressive = 0, Defensive = 1, Hold = 2, Passive = 3 };

// "transport": units (or a structure) carrying other units: dropships, APCs, bunkers, AoE town centres.
struct TransportDef {
    int slots = 0;                         // >0: is a transport; passengers take `cargoSize` slots each
    uint64_t acceptTags = 0;               // 0: any tag
    uint8_t acceptLayers = 1;              // move layers it takes (ground by default)
    int fireFrom = 0;                      // passengers that shoot from inside (bunker); 0 = none
    Fixed loadRange = Fixed::fromRatio(3, 2);   // contact distance to board
    Fixed unloadInterval = Fixed::fromRatio(1, 2); // seconds between two passengers leaving
    Fixed killOnDeath = Fixed::fromInt(1); // share of passengers dying with it (default: all for units, none for structures)
    bool killOnDeathSet = false;
    Fixed heal{};                          // hp per second healed inside
    Fixed damagePerCargo{};                // its own weapons: +x damage multiplier per passenger (AoE arrows)
    bool weaponFromCargo = false;          // shoots with the first passenger's weapons (IFV)
};

// "toggle" on an ability: casting switches a status on and off (cloak, burrow, siege mode), draining energy.
struct ToggleDef { int ability = -1; int16_t status = -1; Fixed drain{}; };

struct VeterancyTable {
    std::vector<Fixed> xp;                 // xp needed for rank 2, 3, ...
    std::vector<StatMod> perLevel;         // per rank above 1
    bool set = false;
};

struct MechType {
    StanceMode stance = StanceMode::Aggressive;
    bool stanceSet = false;
    bool retreatPoint = false;             // retreating units run to the nearest own one
    int cargoSize = 1;                     // slots a passenger takes
    TransportDef transport;
    Fixed detects{};                       // >0: reveals invisible enemies within this radius (detector)
    std::vector<ToggleDef> toggles;
    int veterancy = -1;                    // -1: rules table, 0: none, 1: own table below
    VeterancyTable vetTable;
    Fixed killXp{};                        // xp its killer gets: explicit "xp", else its cost x veterancy.fromCost
};

struct MechRules {
    bool any = false;                      // any agent-mech block present: the mech system runs
    // stances
    Fixed defendRadius = Fixed::fromInt(6);    // defensive stance: chase at most this far from home
    // retreat (Company of Heroes / Dawn of War II)
    bool retreat = false;
    Fixed retreatSpeed = Fixed::fromRatio(5, 4);
    Fixed retreatDamageTaken = Fixed::fromRatio(3, 4);
    bool transports = false;
    bool detection = false;                // any type detects
    Fixed maxDetect{};
    bool toggles = false;
    bool veterancy = false;
    VeterancyTable vet;                    // rules "veterancy"
    bool vetHeal = true;                   // rank up heals fully
    Fixed vetFromCost = Fixed::fromInt(1); // xp for a kill without "xp": victim cost x this
    bool follow = false;                   // guard / follow orders given (derived at runtime, not data)
};

} // namespace rts::game
