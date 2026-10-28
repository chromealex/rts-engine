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

// ------------------------------------------------------------------ stats / modifiers
enum class Stat : uint8_t {
    MaxHp, Armor, Damage, Range, Speed, Sight, GatherRate, AttackSpeed, ProductionSpeed,
    EnergyRegen,    // multiplier on energy regeneration
    Defense,        // multiplier on garrison defense (units lost per assaulting unit)
    GarrisonCap,    // added to / multiplies the garrison maximum
    CooldownRate,   // multiplier on how fast ability cooldowns run (2 = twice as fast)
    UpgradeCost,    // multiplier on upgrade cost and time
    Lifesteal,      // share of dealt damage healed (added)
    DamageTaken,    // multiplier on damage received
    Scale,          // presentation only (rage makes units bigger)
    Armor2,         // second armor class ("pierceArmor": Age of Empires melee / pierce armor)
    Count };
Stat parseStat(const std::string& s, bool* ok = nullptr);

struct StatMod { Stat stat = Stat::Damage; Fixed add{}; Fixed mul = Fixed::fromInt(1); };

// What a modifier applies to: types with any of `tags`, or one `type`, or everything.
struct ModTarget { uint64_t tags = 0; uint16_t type = kNoType; bool all = false; };

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
    bool global = false;      // with rules.storage == "buildings": stays a player-wide stock (coins), not a good
    int16_t convertTo = -1;   // credited as another resource when delivered (ore -> credits)
    Fixed regen{};            // per second for every player (mana, morale)
    Fixed convertRate = Fixed::fromInt(1);
    // fidelity mechanics (RtsOriginals.cpp): gains on match events and player stat mods per whole unit held
    Fixed gainKill{}, gainCapture{}, gainCaptured{}, gainUpgrade{}, gainLost{}, gainDefended{};
    Fixed min{};              // floor (negative regen = decay)
    std::vector<std::pair<uint64_t, StatMod>> perUnitMods; // tags (0 = all) -> mod * floor(stock) (morale suns)
};

// ------------------------------------------------------------------ combat
enum Layer : uint8_t { kLayerGround = 1, kLayerAir = 2, kLayerNaval = 4, kLayerSandOnly = 8,
                      kLayerOrbit = 16 /* planets: orbit shell; units also carry kLayerAir (free movement, see over) */ };
// Can a weapon with these target layers hit a unit of `layer`? Orbit units only by weapons listing "orbit".
inline bool layerHit(uint8_t targets, uint8_t layer) { return (layer & kLayerOrbit) ? (targets & kLayerOrbit) != 0 : (targets & layer) != 0; }

// Armor model (rules "armor" / alias "armorModel"): one damage pipeline for every game (RtsOriginals.cpp mitigate).
enum class ArmorModel : uint8_t { Subtract = 0, Percent = 1, Classes = 2 };
constexpr uint16_t kDamagePure = 0xFFFF;   // damage type: no damage table; in the classes model armor is already applied
struct ClassAttack { uint16_t cls = 0; Fixed amount{}; };

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
    int16_t onHit = -1;                 // payload applied to the target on hit (poison, frost)
    Fixed onHitChance = Fixed::fromInt(1);
    Fixed chargeTime{};                 // >0: the weapon has to stay on one target this long before each shot
    uint8_t armorClass = 0;             // 0: armor, 1: armor2 (pierce)
    std::vector<std::pair<uint64_t, Fixed>> bonus; // extra damage vs targets with any of these tags (AoE / SC2 bonuses)
    Fixed killsCrew{};                  // chance per hit to kill the crew of a crewed target (Z snipers)
    std::vector<ClassAttack> attacks;   // classes model: class attacks (AoE II: melee 6, cavalry +15)
};

// ------------------------------------------------------------------ abilities and effects
struct Amount { uint16_t res = 0; Fixed value{}; };
} // namespace rts::game
#include "RtsAbilityData.h"
namespace rts::game {
// ------------------------------------------------------------------ economy blocks
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
    uint16_t structure = kNoType;          // ...or an own structure of this type (inn)
    Fixed starve{};                        // hp lost per second while the need is active
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
// Production chain step (Knights and Merchants / Settlers): inputs from the building's inventory,
// outputs into it; raw producers have no inputs and may deplete a resource node/field nearby.
struct RecipeDef {
    bool enabled = false;
    std::vector<Amount> in, out;
    Fixed time = Fixed::fromInt(8);
    int16_t fromRes = -1;                  // depletes nodes / field cells of this resource within radius
    Fixed radius = Fixed::fromInt(8);
    int buffer = 2;                        // inputs kept / outputs held, in cycles
};
struct AttractDef {
    std::vector<std::pair<uint16_t, int>> rooms;   // room type, min cells (player total)
    std::vector<Amount> resources;                 // min stock
    Fixed weight = Fixed::fromInt(1);
};

}   // namespace rts::game
#include "RtsMechData.h"   // agent-mech data (needs StatMod, Amount, layers)
namespace rts::game {

// ------------------------------------------------------------------ type
struct UnitType {
    MechType mech;                             // agent-mech blocks (RtsMechData.h)
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
    Fixed radius = Fixed::fromRatio(1, 2);     // body radius: collisions, pathing clearance, edge-to-edge ranges
    Fixed visionBlock{};                       // "vision": {"blocks": h} — sight blocker of that height over its cells (trees, walls)
    Fixed eyeHeight{};                         // "vision": {"eye": h} — extra eye height (watch towers see further over obstacles)
    bool seesOver = false;                     // "vision": {"seesOver": true} — ignores obstacles (air units always do)
    Fixed mass = Fixed::fromInt(1);            // push resistance (default 4 * radius^2)
    uint8_t layer = kLayerGround;
    uint8_t pathLayer = kLayerGround;          // layer | slope class << 4: what pathing / passability get
    Fixed maxSlope = Fixed::fromInt(-1);       // move.maxSlope (heightfield terrain); <0: the layer's default
    Fixed speed{};
    Fixed transitSpeed{};                      // move.transitSpeed: interplanetary transfer speed (orbit units; 0 = 10 x speed)
    int footprintW = 0, footprintH = 0;
    Fixed sight = Fixed::fromInt(8);
    std::vector<WeaponDef> weapons;
    // energy & abilities
    Fixed maxEnergy{}, energyRegen{}, startEnergy{};
    std::vector<AbilityDef> abilities;
    std::vector<int16_t> statuses;             // permanent statuses (passives) every object of this type has
    int level = 1;                             // structure level (filters, level effects)
    bool blocksLanes = false;                  // lane walkers can't pass it while it stands (barrier)
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
    // settlers economy
    Fixed storeCapacity{};                     // >0: storehouse (general goods buffer)
    RecipeDef recipe;
    Fixed carrierCapacity{};                   // >0: carrier hauling goods between buildings
    uint16_t trainFrom = kNoType;              // training consumes an idle own unit of this type (recruit)
    uint16_t autoTrainUnit = kNoType;          // continuous production into the field
    Fixed autoTrainInterval = Fixed::fromInt(5);
    // ---- fidelity mechanics (RtsOriginals.cpp)
    Fixed armor2{};                            // second armor class (pierce)
    std::vector<Fixed> classArmors;            // classes model: armor per class (missing = 0)
    uint64_t classMask = 0;                    // classes it has (bonus classes apply only when present)
    Fixed shieldMax{}, shieldRegen{}, shieldDelay = Fixed::fromInt(10), shieldArmor{}; // StarCraft shields
    int maxGatherers = 0;                      // resource node: workers harvesting at once (WC3 mine 5, SC patch 1)
    int16_t fieldId = -1;                      // provides a power field (pylon) of this id
    Fixed fieldRadius{};
    int16_t needsField = -1;                   // structure works / may be placed only inside an own field
    int queueSlots = 1;                        // items produced in parallel (reactor)
    uint16_t crew = kNoType;                   // vehicle / gun manned by this unit type (Z drivers)
    bool crews = false;                        // this unit can man empty crewed vehicles
    uint64_t infectBy = 0;                     // killed by a unit with any of these tags: becomes `infectSpawn`
    uint16_t infectSpawn = kNoType;
    int infectCount = 1;
    int eats = 0;                              // neutral: submerges after this many kills (sandworm)
    Fixed resurface = Fixed::fromInt(60);
    int16_t onDeath = -1;                      // payload applied where it dies (commander explosion)
    int upgradeGarrison = 0;                   // upgrading into this type costs garrison units (Mushroom Wars 2)
    uint16_t captureAs = kNoType;              // turns into this type when captured (loses a level)
    int garrisonKeep = 0;                      // units that always stay home when sending (State.io)
    bool clash = false;                        // units in transit fight hostile units they meet
    std::vector<std::pair<ModTarget, StatMod>> playerMods; // every completed one modifies the owner's types
    int playerModsMax = 0;                     // count at most this many (0 = all)
    std::vector<std::vector<int>> learnAt;     // per ability: hero levels for skill levels 1, 2, ... (WC3)
    Fixed costGrowth{};                        // cost * (1 + growth * owned) (Dungeon Keeper imps)
    uint64_t crushTags = 0;                    // moving over units with these tags kills them (Dune / C&C tanks)
    uint32_t factionMask = 0;                  // "factions": only these factions may train / build it (0 = all)
    bool autoTrainAdvance = true;              // autoTrain.advance: false = the new units stay home (Northgard births)
    int requiresRegions = 0;                   // trainable only with this many own regions (Z factory tech levels)
    bool ownsRegion = false;                   // its region (and the territory cells of it) follow its owner (State.io states)
    int16_t deathFieldRes = -1;                // dying spreads a resource field (Dune spice bloom)
    Fixed deathFieldRadius = Fixed::fromInt(3), deathFieldAmount = Fixed::fromInt(100);
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
    int requiresAnyCount = 0;                  // "requiresAny": {"count": 2, "of": [...]}: that many different types of the list (AoE ages)
    std::vector<uint16_t> requiresAny;
};

struct FactionStart { uint16_t type = 0; Fixed dx{}, dz{}; int count = 1; };
struct FactionDef { std::string id, name; std::vector<FactionStart> start; };

struct UpkeepTier {
    Fixed supply{}; Fixed gatherMult = Fixed::fromInt(1);
    uint32_t resMask = 0xFFFFFFFFu;            // resources affected (WC3: gold only)
    Fixed incomeMult = Fixed::fromInt(1);      // multiplier on per-second income (CoH manpower upkeep)
};

// Damage model (rules "armor", alias "armorModel"): subtract (StarCraft), percent (Warcraft III) or classes (AoE II).
struct ArmorRules {
    ArmorModel model = ArmorModel::Subtract;
    bool negativeExp = false;                  // percent: negative armor amplifies by 2 - (1 - k)^-a (exact WC3) instead of 1 + min(-a*k, 0.71)
    bool plainFloor = false;                   // no armor rules at all: the original floor raw / 5 (no clamp at minDamage)
    std::vector<std::string> classes;          // classes model: class names; the first `baseClasses` always apply
    int baseClasses = 0;
    Fixed perPoint = Fixed::fromRatio(6, 100); // percent: reduction = a*k / (1 + a*k); negative armor amplifies
    Fixed minDamage{};                         // subtract: at least this much
    Fixed minShare = Fixed::fromRatio(1, 5);   // subtract: at least this share of the raw damage
};

// Warcraft III heroes (rules "heroes").
struct HeroRules {
    bool skills = false;                       // abilities are learned with skill points (one per level + one at start)
    int max = 0;                               // heroes per player (0 = no limit)
    bool freeFirst = false;                    // the first hero costs no resources
    std::vector<uint16_t> tiers;               // hero n needs a structure from tiers[n-1..]
    bool revive = false;                       // training a dead hero again revives it with its level
    Fixed reviveShare = Fixed::fromRatio(1, 2), revivePerLevel = Fixed::fromRatio(1, 10);
    Fixed reviveTimeShare = Fixed::fromInt(1);
    Fixed reviveMaxTime{};                     // 0: no cap (seconds)
    std::vector<Amount> reviveMaxCost;
};

enum class VictoryMode : uint8_t { NoStructures, NoUnits, NoEntities, KeyUnits, Tickets, Survive, Score, Territory };

struct VictoryRules {
    VictoryMode mode = VictoryMode::NoStructures;
    Fixed tickets = Fixed::fromInt(500);
    Fixed ticketDrain = Fixed::fromInt(1);     // per second per point of difference
    Fixed surviveSeconds = Fixed::fromInt(600);  // survive / score: match length
    bool keyLoses = true;                        // score: losing all key structures ends the match at once
    Fixed share = Fixed::fromRatio(9, 10);       // territory: share of claimable cells that wins
};

// Fog of war (rules "fogOfWar"; a map may override "mode"). Visual keys of the block (colour,
// style, softness, grid ...) belong to the view and are ignored here.
//   full (default): unexplored black, explored dim, enemies only in sight
//   explored: the whole map starts explored (terrain known), enemies still only in sight
//   none: no fog at all, everyone sees everything
enum class FogMode : uint8_t { Full, Explored, None };
struct FogRules {
    FogMode mode = FogMode::Full;
    bool shareAllies = true;                  // allies see what each other sees
    // 3D line of sight on any terrain model: ground heights (cliff levels or heightfield) and
    // dynamic blockers (types with vision.blocks, smoke areas, undug rock) hide what lies behind.
    // Heightfield maps with heights.blockVision use line of sight anyway.
    bool lineOfSight = false;
    Fixed rockHeight = Fixed::fromInt(2);     // undug diggable rock as a blocker (0 = transparent)
};

struct HeightRules {
    bool blockVision = true;                  // lower ground can't see higher ground
    Fixed missUphill{};                       // chance (0..1) to miss a target standing higher
    Fixed rangePerLevel{};                    // attacker standing higher gets more range
    Fixed damagePerLevel{};                   // multiplier added per level of advantage
};

// Terrain model (rules "terrain" block; visual keys there belong to the view and are ignored here).
//   levels (default): cliff levels + ramps, exactly the classic behaviour; flat: levels ignored (all 0);
//   heightfield: map corners drive surface height, slope passability, uphill speed, line of sight.
enum class TerrainModel : uint8_t { Levels, Flat, Heightfield };
struct TerrainRules {
    TerrainModel model = TerrainModel::Levels;
    Fixed maxSlopeGround = Fixed::fromInt(1);  // rise per cell a ground unit can climb (1 = 45 degrees)
    Fixed maxSlopeSand = Fixed::fromInt(-1);   // <0: same as ground
    Fixed buildSlope = Fixed::fromRatio(35, 100); // steepest cell a structure may stand on
    Fixed uphillSpeed = Fixed::fromRatio(1, 2);   // speed *= 1 - uphillSpeed * climb (climb = rise per cell along the move)
    Fixed downhillSpeed = Fixed::fromRatio(1, 5); // speed *= 1 + downhillSpeed * descent
    Fixed eyeHeight = Fixed::fromInt(1);       // line of sight starts this high above the ground
    Fixed targetHeight = Fixed::fromRatio(1, 2); // a cell counts as seen when this point above it is in sight
    std::vector<Fixed> slopeClasses;           // per-type move.maxSlope values (class 1..15; 0 = layer default)
    bool heightfield() const { return model == TerrainModel::Heightfield; }
};

struct RegionRules {
    bool enabled = false;
    std::vector<Amount> claimCost;
    Fixed claimCostGrowth{};                  // +x * owned regions
    bool buildOnlyOwned = true;
    Fixed productionPerRegion{};              // production speed bonus per owned region beyond the first (Z)
    bool requireExplored = false;             // a region must have been seen before it can be claimed (Northgard)
    bool supplyLines = false;                 // income of points counts only in sectors linked to an own key structure (CoH)
    bool clearFirst = false;                  // a region with hostile creatures in it can't be claimed (Northgard)
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
    std::vector<Amount> incomePerCell;         // per owned cell per second (Dungeon Keeper mana)
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

struct RoadNetworkRules {
    bool enabled = false;
    std::vector<Amount> cost;                  // per road cell
    Fixed speed = Fixed::fromRatio(3, 2);      // movement multiplier on road cells
    bool required = true;                      // deliveries only between buildings on one road network
};

// Local avoidance between moving units.
struct MovementRules { bool rvo = true; int neighbors = 8; Fixed timeHorizon = Fixed::fromRatio(3, 2);
                       int planetHpaCells = 256 * 256; };   // planets: HPA* on bodies with >= this many cells (0 = all)

// Mushroom Wars 2 style lanes: generated spline paths between garrison buildings.
struct LaneRules { bool enabled = false; int left = 0, right = 0; Fixed spacing = Fixed::fromRatio(35, 100); };

enum class CostModel : uint8_t { Upfront, Progressive };

struct Rules {
    MechRules mech;                           // agent-mech rules (RtsMechData.h)
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
    FogRules fog;
    TerrainRules terrain;
    RegionRules regions;
    std::vector<DiggableDef> diggables;
    std::vector<RoomDef> rooms;
    TerritoryRules territory;
    HandRules hand;
    PossessionRules possession;
    std::vector<PlayerAbilityDef> playerAbilities;
    std::vector<std::vector<EffectDef>> payloads;   // every effect list (abilities, projectiles, areas, statuses...)
    std::vector<StatusDef> statuses;
    std::vector<std::string> statusTagNames;   // status tags (dispel), separate from type tags
    // built-in statuses used by effects that undo themselves (temporary capture, transform, links, illusions)
    int16_t stTemp = -1, stLink = -1, stRedirect = -1, stFake = -1;
    size_t userStatuses = 0;                   // statuses defined by the game (built-ins come after)
    int loadoutSlots = 0;                      // >0: players pick at most this many player abilities (map "loadout")
    int findStatus(const std::string& id) const;
    int findPlayerAbility(const std::string& id) const;
    bool buildingStorage = false;              // "storage": "buildings": goods live in building inventories
    RoadNetworkRules roadNetwork;
    LaneRules lanes;
    MovementRules movement;
    bool settlers = false;                     // any settlers-economy mechanic present
    bool advancedEffects = false;              // statuses / delivery / skills beyond the original instant effects
    bool dungeon = false;                      // any dungeon / territory mechanic present (systems run)
    ArmorRules armor;
    HeroRules heroes;
    bool originals = false;                    // any fidelity mechanic (RtsOriginals.cpp) present: its system runs, its state is hashed
    std::vector<uint16_t> playerModTypes;      // types with playerMods (forges)
    std::vector<uint16_t> perUnitResources;    // resources with perUnit mods (morale)

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
void loadMechRules(Rules& r, const Json& j);   // RtsMechData.cpp, called by loadRules after types are parsed
// Unit designs (UD): rules "designs": {"chassis": {...}, "parts": {...}} and types with "design": {"chassis", "parts"}
// expand into plain type blocks. Returns a copy without "designs"; loadRules calls it when the block is present.
Json expandDesigns(const Json& j);
VictoryRules loadVictory(const Json& j, const VictoryRules& def);

} // namespace rts::game
