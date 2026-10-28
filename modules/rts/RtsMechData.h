// Included by RtsData.h after Stat / StatMod / layers.
// Data of the mechanics added for the "top-100 strategy games" plan (agent-mech): stances, retreat,
// transports and garrisoned infantry, ... Parsed in RtsMechData.cpp from optional JSON blocks; a game
// that uses none of them loads, plays and hashes exactly as before.
#pragma once
#include <cstdint>
#include <string>
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

struct ItemDef {
    std::string id, name;
    std::vector<Amount> cost;
    std::vector<StatMod> mods;             // while carried
    Fixed heal{}, energy{};                // on use
    int16_t status = -1;                   // on use: status on the user
    Fixed statusDuration = Fixed::fromInt(10);
    int charges = 0;                       // uses (0: passive only)
    bool consumable = true;                // used up at 0 charges
};
struct ItemDrop { uint16_t item = 0; Fixed chance = Fixed::fromInt(1); };

// A repeating cycle (day/night, seasons): each phase applies a player status to every player.
struct CyclePhase { std::string id; Fixed until{}; int16_t status = -1; };
struct CycleDef { std::string id; Fixed length = Fixed::fromInt(600); Fixed offset{}; std::vector<CyclePhase> phases; };
// Hazard: objects on a resource field (tiberium) or in a map area get a status / take damage.
struct HazardDef { int16_t field = -1; std::string area; int16_t status = -1; Fixed damage{}; uint64_t tags = 0; uint8_t layers = 1; };

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
    // stage 2
    bool wall = false;                     // built in lines (kCmdBuildLine)
    uint8_t gate = 0;                      // 1: lets its owner through, 2: owner and allies
    bool bridge = false;                   // over water: its footprint is walkable ground while it stands
    int16_t tunnel = -1;                   // tunnel network id (nydus): its transport cargo is shared by the network
    std::vector<int> ammo;                 // per weapon: shots before rearming (0 = unlimited)
    int dockSlots = 0;                     // airfield / helipad: aircraft rearm here
    Fixed rearmTime = Fixed::fromInt(1);   // dock: seconds per shot restored
    uint16_t mergeWith = 0xFFFF, mergeResult = 0xFFFF;   // archon: two of `mergeWith` (or this + it) -> result
    uint16_t splitType = 0xFFFF; int splitCount = 0;      // splits into N units
    bool market = false;                   // trades resources / is a trade destination
    Fixed tradeRate{};                     // trader: currency per cell of distance between markets per trip
    int inventorySlots = 0;
    std::vector<uint16_t> shopItems;       // shop: sells these items to heroes within shopRadius
    Fixed shopRadius = Fixed::fromInt(4);
    std::vector<ItemDrop> drops;           // creeps: items left on death
    // stage 3
    uint8_t givesCover = 0;                // structure: cover level for units next to it
    bool arcs = false; Fixed arcSide = Fixed::fromInt(1), arcRear = Fixed::fromInt(1);   // directional armor (damage multipliers)
    std::vector<Fixed> suppress, turretRate, killsCrew, crater;   // per weapon
    std::vector<uint16_t> infect;          // per weapon: kills rise as this type (0xFFFF none)
    uint16_t ruins = 0xFFFF;               // leaves this type when destroyed
    Fixed crushDamage{}; uint64_t crushTags = 0;
    int staff = 0;                         // workers inside (transport) needed to run at full speed
    uint16_t growUnit = 0xFFFF; Fixed growInterval{}; std::vector<Amount> growCost;   // population growth
    bool autonomous = false;               // acts on its own (Majesty heroes): bounties, enemies, exploring
    Fixed powerField{};                    // pylon: powers own structures within this radius
    bool needsPower = false;               // works and can be built only in a power field
    bool crew = false; uint64_t crewTags = 0;   // vehicle with a crew; crewless it is neutral until re-crewed
    uint16_t infectSpawn = 0xFFFF; int infectCount = 0;   // structure overrun by an infecting attacker spawns these
    int upgradeGarrison = 0;               // upgrading INTO this type costs garrison units (Mushroom Wars 2)
    // stage 4
    Fixed moraleMax{};                     // 0: rules morale.max
    Fixed chargeBonus{}, chargeRun{}, chargeShock{}, chargeCooldown{};   // charge: run-up (cells), first-hit damage multiplier
    Fixed supplyRadius{};                  // depot: supplies own units within this radius
    // logistics (Mindustry, Factorio): belts, producers, crafters, belt inputs
    Fixed beltSpeed{};                     // conveyor piece: cells per second
    uint64_t beltAccepts = 0;              // resources taken from belts into the owner's stock (bit per resource)
    int16_t outRes = -1; Fixed outAmount{}, outInterval{};   // producer: puts packets on adjacent belts
    std::vector<Amount> craftIn, craftOut; Fixed craftTime{};  // crafter: inputs from belts -> outputs to belts
    // free 3D flight (Homeworld): altitude band, climb rate, start altitude; ranges are measured in 3D
    Fixed altMin{}, altMax{}, altClimb{}, altStart{};
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
    // stage 2
    bool walls = false, gates = false, bridges = false, tunnels = false, ammo = false, merge = false;
    std::vector<std::string> tunnelNets;   // tunnel network names (MechType::tunnel indexes this)
    // market (Age of Empires): buy / sell resources for `currency` at prices that move with every trade
    bool market = false;
    int16_t currency = -1;
    std::vector<Fixed> basePrice;          // per resource (0: not traded)
    Fixed marketFee = Fixed::fromRatio(3, 10), priceStep = Fixed::fromInt(3), lot = Fixed::fromInt(100);
    Fixed tributeFee = Fixed::fromRatio(3, 10);
    bool tribute = false;
    bool diplomacy = false;                // players may change alliances (kCmdDiplomacy)
    bool traders = false;
    std::vector<ItemDef> items;
    bool hasItems = false;
    std::vector<CycleDef> cycles;
    std::vector<HazardDef> hazards;
    // card deck (Clash Royale): player abilities are cards; only the `hand` first cards of a player's deck can be cast
    bool deck = false;
    int hand = 4;
    Fixed deployNear{};                    // >0: point player abilities only within this distance of own structures
    bool deployOwnHalf = false;            // ... or on the own half of the map (left/right by start position)
    // stage 3
    std::vector<Fixed> coverLevels;        // damage multiplier per cover level (index 0 = none)
    uint64_t coverTags = 0;
    bool suppression = false;
    Fixed supDecay = Fixed::fromInt(10), supSuppressed = Fixed::fromInt(30), supPinned = Fixed::fromInt(80);
    Fixed supSpeed = Fixed::fromRatio(1, 2), supAttack = Fixed::fromRatio(3, 4);
    uint64_t supTags = 0;
    bool combat3 = false;                  // arcs / turrets / crew / infect / craters / crush present
    bool staffing = false, growth = false, autonomy = false, power = false, crews = false, infection = false, ruinsAny = false;
    bool rounds = false;
    Fixed prepareTime = Fixed::fromInt(30), fightTime = Fixed::fromInt(60);
    int roundCount = 10;
    bool roundReset = true;
    int16_t livesRes = -1;
    Fixed damagePerUnit = Fixed::fromInt(1);
    std::vector<Amount> roundIncome;
    // stage 4: morale and rout (Total War), fatigue, charge, supply lines (Rise of Nations, Hearts of Iron)
    bool morale = false;
    Fixed moraleMax = Fixed::fromInt(100), moraleRegen = Fixed::fromInt(2), hitLoss = Fixed::fromInt(100);
    Fixed allyDeath = Fixed::fromInt(6), allyRadius = Fixed::fromInt(6), flankSide = Fixed::fromRatio(3, 2), flankRear = Fixed::fromInt(2);
    Fixed waverAt = Fixed::fromInt(40), routAt = Fixed::fromInt(20), rallyAt = Fixed::fromInt(60), routSpeed = Fixed::fromRatio(5, 4);
    Fixed waverAttack = Fixed::fromRatio(17, 20), generalRegen = Fixed::fromInt(6), generalRadius = Fixed::fromInt(10), generalDeath = Fixed::fromInt(30);
    uint64_t moraleTags = 0, generalTags = 0;
    bool fatigue = false;
    Fixed fatRun = Fixed::fromInt(2), fatFight = Fixed::fromInt(4), fatRecover = Fixed::fromInt(5);
    Fixed tiredAt = Fixed::fromInt(50), exhaustedAt = Fixed::fromInt(85), tiredMul = Fixed::fromRatio(17, 20), exhaustedMul = Fixed::fromRatio(3, 5);
    uint64_t fatigueTags = 0;
    bool charge = false;
    bool supply = false;
    uint64_t supplyTags = 0;
    Fixed supplyAttrition = Fixed::fromRatio(1, 100), supplySpeed = Fixed::fromRatio(4, 5), supplyAttack = Fixed::fromRatio(4, 5);
    bool battle() const { return morale || fatigue || charge || supply; }
    bool flowFields = false; int flowMinCells = 8, flowCache = 16;
    bool conveyors = false;
    // turn-based mode (TB): players act in turns on the same simulation (Heroes / XCOM style battles)
    bool altitude = false;   // some type flies in 3D (SP)
    bool turns = false;
    Fixed turnMoveSeconds = Fixed::fromInt(4);   // move budget per turn = type speed x this
    int turnAttacks = 1;                         // shots per weapon per turn
    Fixed turnTimeLimit{};                       // >0: the turn ends by itself after this many seconds   // BA: big armies share flow fields
    int findItem(const std::string& id) const { for (size_t i = 0; i < items.size(); ++i) if (items[i].id == id) return int(i); return -1; }
};

} // namespace rts::game
