// Data model of the generic RTS layer. A game is described entirely by JSON:
//   rules.json  - resources, armor/damage tables, unit & structure types, victory rules
//   maps/*.json - terrain grid, players, placed entities, optional road graph
//
// A unit/structure type is a bag of optional components. Which components are present decides
// what the object can do, so the same code runs Warcraft-, Red-Alert-, Total-Annihilation- and
// Mushroom-Wars-style games:
//   health, mover, weapons, vision, cost, producer, builder, gatherer, resourceNode, dropoff,
//   income (flow economy), supply, structure footprint, requirements (tech), garrison/spawner
//   (Mushroom Wars buildings), capturable.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../engine/core/Fixed.h"
#include "../../engine/data/Json.h"

namespace rts::game {

constexpr uint16_t kNoType = UINT16_MAX;

enum class ResourceKind : uint8_t {
    Stock,   // gathered / spent (gold, wood, ore)
    Flow,    // produced per second by buildings, stored up to a cap (TA metal/energy)
};

struct ResourceDef {
    std::string id, name;
    ResourceKind kind = ResourceKind::Stock;
    Fixed start{};       // starting amount (map may override per player)
    Fixed storage{};     // 0 = unlimited (Flow resources: base storage)
};

enum Layer : uint8_t { kLayerGround = 1, kLayerAir = 2, kLayerNaval = 4 };

struct WeaponDef {
    std::string id;
    Fixed range = Fixed::fromInt(1);
    Fixed minRange{};
    Fixed damage = Fixed::fromInt(5);
    Fixed cooldown = Fixed::fromInt(1);     // seconds
    uint8_t targets = kLayerGround;         // which layers it can hit
    Fixed projectileSpeed{};                 // 0 = instant hit
    Fixed splash{};                          // splash radius (full damage inside)
    uint16_t damageType = 0;
};

struct Amount { uint16_t res = 0; Fixed value{}; };

struct GatherDef { uint16_t res = 0; Fixed carry = Fixed::fromInt(10); Fixed time = Fixed::fromInt(2); };

struct UnitType {
    std::string id, name;
    bool structure = false;
    // health
    Fixed maxHp = Fixed::fromInt(100);
    Fixed armor{};
    uint16_t armorType = 0;
    Fixed regen{};
    bool invulnerable = false;               // resources, decorations
    // body / movement
    Fixed radius = Fixed::fromRatio(1, 2);
    uint8_t layer = kLayerGround;
    Fixed speed{};                           // 0 = cannot move
    int footprintW = 0, footprintH = 0;      // structures: cells blocked on the grid
    Fixed sight = Fixed::fromInt(8);
    std::vector<WeaponDef> weapons;
    // economy
    std::vector<Amount> cost;
    Fixed buildTime = Fixed::fromInt(10);
    Fixed supplyUsed{};
    Fixed supplyProvided{};
    std::vector<Amount> income;              // per second, may be negative (upkeep)
    std::vector<Amount> storage;             // raises Flow storage caps
    std::vector<uint16_t> produces;          // trains these types
    std::vector<uint16_t> builds;            // can construct these structures
    std::vector<GatherDef> gathers;
    int16_t resourceNode = -1;               // resource id this object contains
    Fixed resourceAmount{};
    uint32_t dropoffMask = 0;                // bit per resource accepted
    std::vector<uint16_t> requires_;         // all of these types must exist for the owner
    // garrison (Mushroom Wars-style buildings)
    int garrisonMax = 0;                     // >0: has a unit counter inside
    uint16_t spawnUnit = kNoType;            // unit type that is sent out / grows inside
    Fixed spawnInterval{};                   // seconds per unit, 0 = no growth
    bool capturable = false;                 // owner flips when garrison is taken
    Fixed garrisonDefense = Fixed::fromInt(1); // attackers needed per defender
    Fixed garrisonDecay = Fixed::fromInt(1);   // units/s lost above max
    bool entersGarrison = false;             // unit merges into / assaults garrisons on arrival
    // classification
    bool worker = false;
    bool countsForVictory = true;
    // presentation hints (opaque to the simulation)
    Json view;
};

struct Rules {
    std::vector<ResourceDef> resources;
    std::vector<std::string> armorTypes;     // index = armorType
    std::vector<std::string> damageTypes;
    std::vector<std::vector<Fixed>> damageTable; // [damageType][armorType] multiplier
    std::vector<UnitType> types;
    Fixed supplyCap = Fixed::fromInt(200);
    std::string victory = "noStructures";    // noStructures | noUnits | noEntities | ownAllCapturable
    int maxQueue = 5;

    int findType(const std::string& id) const;
    int findResource(const std::string& id) const;
    Fixed damageMult(uint16_t dmgType, uint16_t armorType) const;
};

// Throws std::runtime_error with a readable message on bad data.
Rules loadRules(const Json& j);

} // namespace rts::game
