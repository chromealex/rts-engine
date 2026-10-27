// Data model of the generic RTS layer. A game is described entirely by JSON:
//   rules.json  - resources, damage tables, unit/structure types, techs, factions, game-wide rules
//   maps/*.json - terrain (types, cliff levels, ramps, resource fields, regions), players,
//                 placed objects, roads, waves, victory overrides
//
// A type is a bag of optional blocks; the blocks present decide what an object can do. This lets
// the same code run Warcraft, StarCraft, Red Alert, Dune, Total Annihilation, Age of Empires,
// Company of Heroes, They Are Billions, Northgard or Mushroom Wars style games.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../engine/core/Fixed.h"
#include "../../engine/data/Json.h"

namespace rts::game {

constexpr uint16_t kNoType = UINT16_MAX;
constexpr uint8_t kHostileNeutral = 255;   // owner of creeps/monsters: hostile to every player

// ------------------------------------------------------------------ resources
enum class ResourceKind : uint8_t {
    Stock,     // gathered/earned and spent (gold, wood, ore, spice)
    Flow,      // produced per second, spent per second, stored up to a cap (TA metal/energy)
    Capacity,  // provided vs consumed, never stored (power in Red Alert / Dune)
};

struct ResourceDef {
    std::string id, name;
    ResourceKind kind = ResourceKind::Stock;
    Fixed start{};
    Fixed storage{};          // base storage cap (Flow always capped; Stock only when `capped`)
    bool capped = false;      // Stock: limited by storage (Dune silos)
    bool field = false;       // gathered from map cells (Red Alert ore, Dune spice)
    Fixed fieldRegrow{};      // per cell per second, up to fieldCellMax
    Fixed fieldCellMax = Fixed::fromInt(100);
    // Capacity deficit (used > provided)
    Fixed deficitProductionMult = Fixed::fromRatio(1, 2);
    uint64_t deficitDisableTags = 0; // objects with any of these tags stop working
};

// ------------------------------------------------------------------ stats / modifiers
enum class Stat : uint8_t { MaxHp, Armor, Damage, Range, Speed, Sight, GatherRate, AttackSpeed, ProductionSpeed, Count };
Stat parseStat(const std::string& s, bool* ok = nullptr);

struct StatMod { Stat stat = Stat::Damage; Fixed add{}; Fixed mul = Fixed::fromInt(1); };

// What a modifier applies to: types with any of `tags`, or one `type`, or everything.
struct ModTarget { uint64_t tags = 0; uint16_t type = kNoType; bool all = false; };

// ------------------------------------------------------------------ combat
enum Layer : uint8_t { kLayerGround = 1, kLayerAir = 2, kLayerNaval = 4, kLayerSandOnly = 8 };

struct WeaponDef {
    std::string id;
    Fixed range = Fixed::fromInt(1);
    Fixed minRange{};
    Fixed damage = Fixed::fromInt(5);
    Fixed cooldown = Fixed::fromInt(1);
    uint8_t targets = kLayerGround;
    Fixed projectileSpeed{};
    Fixed splash{};
    uint16_t damageType = 0;
};

// ------------------------------------------------------------------ abilities
enum class TargetKind : uint8_t { None, Self, Unit, Point };
enum class EffectKind : uint8_t { Damage, Heal, Stun, Buff, Spawn, Blink };

struct EffectDef {
    EffectKind kind = EffectKind::Damage;
    Fixed amount{};            // damage / heal
    Fixed radius{};            // 0 = single target
    Fixed duration{};          // stun / buff / summon lifetime
    uint16_t damageType = 0;
    StatMod mod;               // buff
    uint16_t spawnType = kNoType;
    int count = 1;
    bool enemies = true;       // area filters
    bool allies = false;
};

struct AbilityDef {
    std::string id, name;
    TargetKind target = TargetKind::None;
    Fixed range = Fixed::fromInt(6);
    Fixed cooldown = Fixed::fromInt(10);
    Fixed energyCost{};
    int requiredLevel = 1;     // heroes
    bool autocast = true;      // used automatically (AI and idle units)
    std::vector<EffectDef> effects;
    // passive aura (applied continuously around the owner)
    bool aura = false;
    Fixed auraRadius{};
};

// ------------------------------------------------------------------ economy blocks
struct Amount { uint16_t res = 0; Fixed value{}; };
struct GatherDef { uint16_t res = 0; Fixed carry = Fixed::fromInt(10); Fixed time = Fixed::fromInt(2); };

enum class BuildMode : uint8_t {
    Walk,   // builder stays and builds (Warcraft, AoE, TA; several builders add up)
    Warp,   // builder starts the structure and leaves (Protoss)
    Morph,  // builder becomes the structure (Zerg)
};

enum class TerrainReq : uint8_t { Any, Rock, Creep };

struct Requirement { bool tech = false; uint16_t index = 0; };

// ------------------------------------------------------------------ dungeon / territory blocks
// Creature needs (Dungeon Keeper): hunger/sleep are satisfied in rooms, wages are paid from a stock.
struct NeedDef {
    bool enabled = false;
    Fixed interval = Fixed::fromInt(60);   // seconds until the need becomes active
    int16_t room = -1;                     // room type that satisfies it
    Fixed time = Fixed::fromInt(4);        // seconds spent in the room
    std::vector<Amount> cost;              // paid when satisfied (food)
};
struct NeedsDef {
    NeedDef hunger, sleep;
    Fixed payInterval{};                   // >0: payday every N seconds
    std::vector<Amount> pay;
    std::vector<uint16_t> works;           // room types the creature works in when idle
    Fixed workTime = Fixed::fromInt(10);
    Fixed happyStart = Fixed::fromInt(1);
    Fixed unmetRate = Fixed::fromRatio(1, 100);    // happiness lost per second per unmet need
    Fixed recovery = Fixed::fromRatio(1, 200);     // gained per second when content
    Fixed slapLoss = Fixed::fromRatio(1, 10);
    Fixed unpaidLoss = Fixed::fromRatio(3, 10);
    Fixed leaveBelow = Fixed::fromRatio(1, 5);
    Fixed leaveAfter = Fixed::fromInt(30);
};
// Portal recruitment requirements: rooms of a minimum size and resources in stock.
struct AttractDef {
    std::vector<std::pair<uint16_t, int>> rooms;   // room type, min cells (player total)
    std::vector<Amount> resources;                 // min stock
    Fixed weight = Fixed::fromInt(1);
};

// ------------------------------------------------------------------ type
struct UnitType {
    std::string id, name;
    uint64_t tags = 0;
    bool structure = false;
    // health
    Fixed maxHp = Fixed::fromInt(100);
    Fixed armor{};
    uint16_t armorType = 0;
    Fixed regen{};
    bool invulnerable = false;
    // body / movement
    Fixed radius = Fixed::fromRatio(1, 2);
    uint8_t layer = kLayerGround;
    Fixed speed{};
    int footprintW = 0, footprintH = 0;
    Fixed sight = Fixed::fromInt(8);
    std::vector<WeaponDef> weapons;
    // energy & abilities
    Fixed maxEnergy{}, energyRegen{}, startEnergy{};
    std::vector<AbilityDef> abilities;
    // hero
    bool hero = false;
    std::vector<int> xpLevels;                 // xp needed for level 2, 3, ...
    std::vector<StatMod> perLevel;             // added per level above 1
    Fixed xpValue{};                           // xp granted to enemy heroes when killed
    std::vector<Amount> bounty;                // resources granted to the killer
    bool unique = false;                       // at most one per player (heroes)
    // squad (Company of Heroes): hp pool split over members, damage scales with members alive
    int members = 1;
    std::vector<Amount> reinforceCost;
    Fixed reinforceTime = Fixed::fromInt(4);
    // economy
    std::vector<Amount> cost;
    Fixed buildTime = Fixed::fromInt(10);
    Fixed supplyUsed{}, supplyProvided{};
    std::vector<Amount> income;                // per second (Flow / Stock), can be negative
    std::vector<Amount> storage;               // raises caps
    std::vector<Amount> provides, consumes;    // Capacity resources (power)
    std::vector<uint16_t> produces;            // trains types (structures here = sidebar build)
    std::vector<uint16_t> researches;          // techs
    std::vector<uint16_t> builds;              // constructs these structures
    BuildMode buildMode = BuildMode::Walk;
    Fixed buildPower = Fixed::fromInt(1);
    std::vector<GatherDef> gathers;
    int16_t resourceNode = -1;
    Fixed resourceAmount{};                    // <0 = infinite
    bool ownerOnly = false;                    // only the owner gathers (farms, refineries)
    uint32_t dropoffMask = 0;
    std::vector<Requirement> requires_;
    // larva-style parallel production
    int chargesMax = 0;
    Fixed chargeInterval{};
    // placement
    uint16_t placeOn = kNoType;                // must be built on top of this type (geyser, metal spot)
    bool takesOverNode = false;                // inherits the node's resource (refinery)
    TerrainReq terrainReq = TerrainReq::Any;
    bool extendsBase = true;                   // counts for build radius
    int creepRadius = 0;                       // spreads creep
    Fixed fieldSpawnRate{};                    // grows a resource field around it (ore mine)
    int16_t fieldSpawnRes = -1;
    int fieldSpawnRadius = 3;
    uint16_t deploysTo = kNoType;              // unit -> structure (MCV)
    std::vector<Amount> wreck;                 // leaves a reclaimable wreck
    bool sellable = true;
    // garrison (Mushroom Wars)
    int garrisonMax = 0;
    uint16_t spawnUnit = kNoType;
    Fixed spawnInterval{};
    bool capturable = false;
    Fixed garrisonDefense = Fixed::fromInt(1);
    Fixed garrisonDecay = Fixed::fromInt(1);
    bool entersGarrison = false;
    // control point (Company of Heroes)
    Fixed captureTime{};                       // >0: captured by standing next to it
    Fixed captureRadius = Fixed::fromInt(3);
    bool victoryPoint = false;
    bool capturesRegion = false;               // owning the point owns its region and the structures in it (Z flags)
    // neutral behaviour
    Fixed leash = Fixed::fromInt(10);          // creeps return home beyond this distance
    bool wanders = false;                      // roams around its home (sandworm)
    Fixed wanderRadius = Fixed::fromInt(20);
    // classification
    bool worker = false;
    bool countsForVictory = true;
    bool key = false;                          // losing all key units loses the game (commander)
    // lane battler: produced automatically and released together on the global wave clock
    uint16_t waveUnit = kNoType;
    int waveCount = 1;
    std::vector<uint16_t> upgrades;            // in-place upgrades (structure levels)
    Fixed score = Fixed::fromInt(1);           // points to the killer's team (score victory)
    // dungeon / territory
    Fixed digRate{};                           // >0: digs marked diggable cells (hardness per second)
    Fixed claimerTime{};                       // >0: walks to frontier cells and claims them (seconds per cell)
    Fixed claimRadius{}, claimRate{};          // area claim around the unit (cells per second)
    bool claimedByTerritory = false;           // neutral structure joins whoever owns the territory next to it (portal)
    bool hasNeeds = false;
    NeedsDef needs;
    bool attractable = false;
    AttractDef attract;
    Fixed portalInterval{};                    // >0: portal: recruits attracted creatures
    std::vector<uint16_t> portalPool;          // empty: every attractable type
    uint16_t autoTrainUnit = kNoType;          // continuous production into the field
    Fixed autoTrainInterval = Fixed::fromInt(5);
    Json view;
};

// ------------------------------------------------------------------ techs / factions / rules
struct TechDef {
    std::string id, name;
    std::vector<Amount> cost;
    int levels = 1;                            // repeatable: each level re-applies the effects
    Fixed costGrowth{};                        // cost * (1 + growth * levels already researched)
    Fixed time = Fixed::fromInt(30);
    std::vector<Requirement> requires_;
    std::vector<std::pair<ModTarget, StatMod>> effects;
};

struct FactionStart { uint16_t type = 0; Fixed dx{}, dz{}; int count = 1; };
struct FactionDef { std::string id, name; std::vector<FactionStart> start; };

struct UpkeepTier { Fixed supply{}; Fixed gatherMult = Fixed::fromInt(1); };

enum class VictoryMode : uint8_t { NoStructures, NoUnits, NoEntities, KeyUnits, Tickets, Survive, Score, Territory };

struct VictoryRules {
    VictoryMode mode = VictoryMode::NoStructures;
    Fixed tickets = Fixed::fromInt(500);
    Fixed ticketDrain = Fixed::fromInt(1);     // per second per point of difference
    Fixed surviveSeconds = Fixed::fromInt(600);  // survive / score: match length
    bool keyLoses = true;                        // score: losing all key structures ends the match at once
    Fixed share = Fixed::fromRatio(9, 10);       // territory: share of claimable cells that wins
};

struct HeightRules {
    bool blockVision = true;                  // lower ground can't see higher ground
    Fixed missUphill{};                       // chance (0..1) to miss a target standing higher
    Fixed rangePerLevel{};                    // attacker standing higher gets more range
    Fixed damagePerLevel{};                   // multiplier added per level of advantage
};

struct RegionRules {
    bool enabled = false;
    std::vector<Amount> claimCost;
    Fixed claimCostGrowth{};                  // +x * owned regions
    bool buildOnlyOwned = true;
    Fixed productionPerRegion{};              // production speed bonus per owned region beyond the first (Z)
};

// Diggable terrain: map character -> hardness and what digging it yields (gold veins, gems).
struct DiggableDef { char ch = 'D'; Fixed hardness = Fixed::fromInt(3); std::vector<Amount> yields; bool infinite = false; };

// Painted rooms: effects scale with the number of cells the player owns of that room type.
struct RoomDef {
    std::string id, name;
    std::vector<Amount> cost;                  // per cell
    Fixed supply{};                            // per cell: creature capacity (lair)
    std::vector<Amount> income, storage, provides; // per cell
    std::vector<Amount> workYield;             // per working creature per second (library research)
    std::vector<Amount> workCost;              // per working creature per second (training)
    Fixed workXp{};                            // xp per second for working heroes/levelled creatures
    Json view;
};

enum class ClaimBy : uint8_t { Both, Units, Workers };
struct TerritoryRules {
    bool enabled = false;
    ClaimBy claimBy = ClaimBy::Both;
    bool startFromRegions = false;             // owned regions start as owned territory
    int startRadius = 0;                       // cells around starting structures / start points
    bool buildOnlyOwned = false;
};

struct HandRules {
    int max = 0;                               // 0: no hand
    bool ownTerritoryOnly = true;              // pick up / drop only on own territory (when territory is enabled)
    Fixed slapDamage = Fixed::fromInt(5);
    Fixed slapDuration = Fixed::fromInt(10);
    Fixed slapSpeed = Fixed::fromRatio(3, 2);  // speed / work multipliers while slapped
    Fixed slapWork = Fixed::fromRatio(3, 2);
};

struct PossessionRules { bool enabled = false; std::vector<Amount> cost, drain; };

// Player-level abilities (not tied to a unit): nukes, air strikes, reinforcements.
struct PlayerAbilityDef {
    std::string id, name;
    std::vector<Amount> cost;
    Fixed cooldown{};
    int chargesMax = 0;                        // 0: unlimited (cooldown only)
    std::vector<Amount> rearmCost;
    Fixed rearmTime = Fixed::fromInt(30);
    TargetKind target = TargetKind::Point;
    std::vector<EffectDef> effects;
};

enum class CostModel : uint8_t { Upfront, Progressive };

struct Rules {
    std::vector<ResourceDef> resources;
    std::vector<std::string> tagNames;
    std::vector<std::string> armorTypes, damageTypes;
    std::vector<std::vector<Fixed>> damageTable;
    std::vector<UnitType> types;
    std::vector<TechDef> techs;
    std::vector<FactionDef> factions;
    std::vector<UpkeepTier> upkeep;
    Fixed supplyCap = Fixed::fromInt(200);
    int maxQueue = 5;
    CostModel costModel = CostModel::Upfront;
    Fixed sellRefund = Fixed::fromRatio(1, 2);
    int buildRadius = 0;                      // >0: structures only near own structures
    Fixed levelHeight = Fixed::fromRatio(3, 2);
    Fixed waveClock{};                        // >0: wave spawners release units every N seconds
    std::vector<uint16_t> autoBuildPool;      // "BUILD" button: structures picked and placed automatically
    std::vector<Amount> autoBuildCost;        // cost of one auto build (empty: the type's own cost)
    Fixed autoBuildGrowth{};                  // cost grows by x per structure already auto-built
    VictoryRules victory;
    HeightRules heights;
    RegionRules regions;
    std::vector<DiggableDef> diggables;
    std::vector<RoomDef> rooms;
    TerritoryRules territory;
    HandRules hand;
    PossessionRules possession;
    std::vector<PlayerAbilityDef> playerAbilities;
    bool dungeon = false;                      // any dungeon / territory mechanic present (systems run)

    int findRoom(const std::string& id) const;
    int findDiggable(char c) const;
    int findType(const std::string& id) const;
    int findResource(const std::string& id) const;
    int findTech(const std::string& id) const;
    int findFaction(const std::string& id) const;
    uint64_t tagMask(const std::string& name) const; // 0 if unknown
    Fixed damageMult(uint16_t dmgType, uint16_t armorType) const;
    bool matches(const ModTarget& t, uint16_t type) const;
};

Rules loadRules(const Json& j);        // throws std::runtime_error with a readable message
VictoryRules loadVictory(const Json& j, const VictoryRules& def);

} // namespace rts::game
