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

enum class ArmorModel : uint8_t { Subtract = 0, Percent = 1, Classes = 2 };
constexpr uint16_t kDamagePure = 0xFFFF;   // damage type: armor already applied (class model), no table / armor

struct ClassAttack { uint16_t cls = 0; Fixed amount{}; };

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
    // combat models
    std::vector<std::vector<ClassAttack>> attacks;   // per weapon: class attacks (AoE: melee 6, cavalry +15)
    std::vector<Fixed> armors;             // per armor class (class model); size = classes, missing = 0
    uint64_t armorClassMask = 0;           // classes it has (bonus classes apply only when present)
    Fixed shieldMax{}, shieldRegen{}, shieldDelay = Fixed::fromInt(10), shieldArmor{};   // StarCraft shields
    int maxWorkers = 0;                    // resource node: at most N gathering at once (WC3 mine, SC saturation)
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
    // combat / economy models (templates audit)
    ArmorModel armorModel = ArmorModel::Subtract;
    Fixed minDamageFraction = Fixed::fromRatio(1, 5);   // subtract: at least this share of the raw damage
    Fixed minDamage{};                     // ... and at least this much (AoE: 1)
    Fixed percentFactor = Fixed::fromRatio(6, 100);    // percent: reduction = a*f / (1 + a*f) (Warcraft III)
    std::vector<std::string> armorClasses; // class model: names; the first `baseClasses` always apply
    int baseClasses = 0;
    bool shields = false;
    uint32_t upkeepMask = 0;               // resources upkeep applies to (0 = all gathering)
    bool workerCaps = false;
};

} // namespace rts::game
