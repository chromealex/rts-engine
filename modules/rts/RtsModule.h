// Generic, data-driven RTS gameplay module. One implementation for every game built on the engine.
// Implementation is split over several files:
//   RtsModule.cpp    setup, commands, queries, hashing
//   RtsEconomy.cpp   resources, capacity/power, fields, production, research, construction, garrisons
//   RtsUnits.cpp     orders, movement, gathering, building behaviour
//   RtsCombat.cpp    targeting, weapons, projectiles, abilities, buffs, heroes, squads, death
//   RtsWorld.cpp     vision (with heights), creep, control points, regions, waves, neutrals, victory
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../engine/sim/CommonComponents.h"
#include "../../engine/sim/Simulation.h"
#include "RtsData.h"
#include "RtsMap.h"

namespace rts::game {

// ---------------------------------------------------------------- commands
enum CommandType : uint16_t {
    kCmdMove = 1,     // units, point
    kCmdAttack,       // units, target entity OR point (attack-move)
    kCmdStop,
    kCmdHold,
    kCmdPatrol,       // units, point
    kCmdGather,       // units, target node / dropoff, or point on a resource field
    kCmdBuild,        // units (builders), param = type, point = centre
    kCmdTrain,        // units (producers), param = type
    kCmdCancelTrain,  // units (producers or constructions), param = queue slot (-1 = last)
    kCmdRally,        // units (producers), point or target
    kCmdSmart,        // right click
    kCmdSend,         // garrison structures, target structure, param = percent
    kCmdResearch,     // units (researchers), param = tech
    kCmdPlace,        // producer with a finished structure (sidebar), point
    kCmdSell,         // structures
    kCmdDeploy,       // units with deploysTo
    kCmdAbility,      // units, param = ability index, target or point
    kCmdReinforce,    // squads
    kCmdClaim,        // point inside a region (Northgard)
    kCmdAutoBuild,    // no units; param = type or -1 (random from rules.autoBuild pool); spot chosen by the sim
    kCmdUpgrade,      // structures, param = target type from `upgrades`
};

// ---------------------------------------------------------------- events (presentation)
enum GameEvent : uint16_t {
    kEvSpawned = 1, kEvDied, kEvAttack, kEvHit, kEvTrainStarted, kEvTrainDone, kEvBuildPlaced, kEvBuildDone,
    kEvGathered, kEvCaptured, kEvPlayerDefeated, kEvNotEnough, kEvResearched, kEvLevelUp, kEvAbility, kEvMiss,
    kEvReadyToPlace, kEvSold, kEvRegionClaimed, kEvWave, kEvLowPower, kEvUpgraded,
};

// ---------------------------------------------------------------- components
enum UnitFlags : uint8_t { kUnitHold = 1, kUnitUnderConstruction = 2, kUnitDisabled = 4 /*power deficit*/, kUnitSilent = 8 /*removed without death effects*/ };

struct Unit {
    uint16_t type = 0;
    Fixed hp{};
    uint8_t flags = 0;
    uint8_t lastAttacker = 0;   // owner slot of the last damage source (xp / bounty)
    Vec3 facing{Fixed::fromInt(1), Fixed{}, Fixed{}};
    Fixed energy{};
};

enum class OrderType : uint8_t { Move, AttackMove, Attack, Patrol, Gather, Return, Build, Repair, Enter, Cast };

struct Order {
    OrderType type = OrderType::Move;
    Entity target = kNullEntity;
    Vec3 point{};
    Vec3 point2{};
    int32_t param = 0;
    uint8_t state = 0;
    Fixed timer{};
};

struct Mobile {
    std::vector<Vec3> path;
    uint16_t pathPos = 0;
    Vec3 goal{};
    bool moving = false;
    Fixed goalRadius{};
    Fixed stuck{};
    std::vector<uint16_t> roadPath;
};

struct Orders { std::vector<Order> queue; };

struct Combat {
    Fixed cooldown[4] = {};
    Entity target = kNullEntity;
    uint8_t acquireTimer = 0;
    bool autoTarget = false;
};

struct Cargo { int16_t res = -1; Fixed amount{}; Entity lastNode = kNullEntity; Vec3 lastPos{}; bool field = false; };

struct ProdItem { int32_t item = 0; Fixed progress{}; Fixed paid{}; bool upgrade = false; }; // item >= 0: type; < 0: tech ~item

struct Production {
    std::vector<ProdItem> queue;
    Vec3 rally{};
    bool hasRally = false;
    Entity rallyTarget = kNullEntity;
    uint16_t ready = kNoType;    // finished structure waiting to be placed (sidebar)
    int charges = 0;             // larva
    Fixed chargeTimer{};
};

struct Construction { Fixed progress{}; Fixed paid{}; Entity builder = kNullEntity; bool selfBuild = false; };
struct ResourceStock { int16_t res = -1; Fixed amount{}; bool infinite = false; };
struct Extractor { uint16_t nodeType = 0; Fixed remaining{}; };
struct Footprint { int x = 0, y = 0, w = 0, h = 0; };

struct Garrison {
    int count = 0;
    Fixed growth{};
    Fixed damageAcc{};
    int pendingOut = 0;
    Entity pendingTarget = kNullEntity;
};

struct Projectile {
    Entity target = kNullEntity, source = kNullEntity;
    Vec3 aim{};
    Fixed speed{}, damage{}, splash{};
    uint16_t damageType = 0;
    uint8_t owner = 0, layers = 0;
};

struct Hero { int level = 1; Fixed xp{}; };
struct Squad { Fixed reinforceTimer{}; bool reinforcing = false; };

struct Buff { Stat stat = Stat::Damage; Fixed add{}; Fixed mul = Fixed::fromInt(1); uint32_t until = 0; uint16_t sourceType = kNoType; };
struct Buffs { std::vector<Buff> list; uint32_t stunUntil = 0; };
struct AbilityState { uint32_t readyAt[8] = {}; };
struct Capture { Fixed progress{}; uint8_t team = 0; };
struct Guard { Vec3 home{}; Fixed leash{}; bool returning = false; uint32_t nextWander = 0; };
struct Summon { uint32_t expire = 0; };

// ---------------------------------------------------------------- per-player state
struct TypeMods { Fixed add[size_t(Stat::Count)]; Fixed mul[size_t(Stat::Count)]; };

struct PlayerEconomy {
    std::vector<Fixed> stock, cap, income;
    std::vector<Fixed> capProvided, capUsed;  // Capacity resources
    uint64_t disabledTags = 0;                 // from capacity deficits
    Fixed productionMult = Fixed::fromInt(1);
    Fixed supplyUsed{}, supplyCap{};
    std::vector<int> typeCount;                // completed
    std::vector<int> typeCountAll;             // incl. constructing and queued (unique)
    std::vector<uint8_t> researched, researching;
    std::vector<TypeMods> mods;                // per type
    Fixed gatherMult = Fixed::fromInt(1);      // upkeep
    int regions = 0;
    std::string faction;
};

struct WaveDef {
    uint32_t tick = 0;
    uint8_t player = 0;
    std::vector<std::pair<uint16_t, int>> spawn;
    Vec3 at{};
    Fixed spread = Fixed::fromInt(3);
    uint32_t every = 0;
    Fixed growth = Fixed::fromInt(1);
    int repeats = 0;
    int done = 0;
};

struct GameState {
    std::vector<PlayerEconomy> players;   // index = slot (0 = neutral)
    MapGrid grid;
    RoadGraph roads;
    SpatialHash hash;
    bool roadsOnly = false;
    std::vector<std::vector<uint8_t>> vision; // per player per cell: 0 unexplored, 1 explored, 2 visible
    // regions
    int regionCount = 0;
    std::vector<uint8_t> regionOwner;
    std::vector<std::vector<uint16_t>> regionAdj;
    std::vector<std::string> regionNames;
    std::vector<Vec3> regionCenter;
    // victory
    VictoryRules victory;
    std::vector<Fixed> tickets;          // per team (index = team id)
    std::vector<uint8_t> wavePlayers;    // slots controlled by scripted waves (excluded from defeat)
    std::vector<WaveDef> waves;
    bool fieldsDirty = true;             // presentation: resource fields changed
    // lane battler
    uint32_t nextWaveTick = 0;
    std::vector<Fixed> score;            // per team
    std::vector<int> autoBuilt;          // per player
    int winnerTeam = -1;                 // set by timed modes
};

// ---------------------------------------------------------------- module
class RtsModule final : public GameModule {
public:
    std::string name() const override { return "rts"; }
    void setup(Simulation& sim, const Json& rules, const Json& map) override;
    void applyCommand(Simulation& sim, const Command& cmd) override;
    void hashState(Simulation& sim, Hasher& h) override;
    int result(Simulation& sim) override;

    const Rules& rules() const { return rules_; }
    GameState& state(Simulation& sim) { return *sim.world().resource<GameState>(); }

    // ---- queries shared with AI, client and editor
    Entity spawn(Simulation& sim, uint16_t type, uint8_t owner, Vec3 pos, bool underConstruction = false);
    // Full placement check for `player` (terrain, blocking, radius, regions, creep, placeOn...).
    bool canPlace(Simulation& sim, uint8_t player, uint16_t type, Vec3 centre, Entity ignore = kNullEntity, std::string* why = nullptr);
    static void footprintAt(const UnitType& t, Vec3 centre, int& x0, int& y0);
    static Vec3 snapToFootprint(const UnitType& t, Vec3 p);
    bool requirementsMet(Simulation& sim, uint8_t player, const std::vector<Requirement>& req) const;
    bool canTrain(Simulation& sim, uint8_t player, uint16_t type) const; // requirements + unique
    bool canAfford(Simulation& sim, uint8_t player, const std::vector<Amount>& cost, Fixed supply = {}, int* missing = nullptr) const;
    bool isVisible(Simulation& sim, uint8_t player, Vec3 p) const;
    bool hostile(Simulation& sim, uint8_t a, uint8_t b) const;
    Fixed stat(Simulation& sim, Entity e, Stat s) const;          // effective stat with techs/levels/buffs
    Fixed typeStat(Simulation& sim, uint8_t player, uint16_t type, Stat s, Fixed base) const;
    int membersAlive(Simulation& sim, Entity e) const;
    int levelAt(Simulation& sim, Vec3 p) const;
    int regionAt(Simulation& sim, Vec3 p) const;
    bool canClaim(Simulation& sim, uint8_t player, int region, std::vector<Amount>* cost = nullptr) const;
    Entity findNearestFieldCell(Simulation& sim, Vec3 p, int res, Vec3& out, int maxRadius = 24) const;
    uint8_t teamOf(Simulation& sim, uint8_t player) const;

    // ---- internals (used across implementation files)
    void issue(Simulation& sim, Entity e, const Order& o, bool queue);
    void moveTo(Simulation& sim, Entity e, Vec3 goal, Fixed radius);
    void stopMoving(Simulation& sim, Entity e);
    void dealDamage(Simulation& sim, Entity target, Fixed dmg, uint16_t dmgType, Entity source, uint8_t sourceOwner);
    Fixed contactDistance(Simulation& sim, Entity a, Entity b);
    bool pay(Simulation& sim, uint8_t player, const std::vector<Amount>& cost, Fixed supply, bool emitFail = true);
    void refund(Simulation& sim, uint8_t player, const std::vector<Amount>& cost, Fixed fraction);
    Vec3 freeSpotNear(Simulation& sim, Entity structure, uint8_t layer);
    void recomputeMods(Simulation& sim, uint8_t player);
    void castAbility(Simulation& sim, Entity caster, int index, Entity target, Vec3 point);
    bool canCast(Simulation& sim, Entity caster, int index) const;
    Entity placeStructure(Simulation& sim, uint8_t player, uint16_t type, Vec3 centre, Entity builder, bool complete);
    void addResource(Simulation& sim, uint8_t player, uint16_t res, Fixed amount);
    // Deterministic spot for an auto-placed structure (nearest valid to the player's key/first structure).
    bool findAutoSpot(Simulation& sim, uint8_t player, uint16_t type, Vec3& out);
    std::vector<Amount> autoBuildCost(Simulation& sim, uint8_t player, uint16_t type) const;
    uint32_t ticksUntilWave(Simulation& sim) const;

private:
    void setupMap(Simulation& sim, const Json& map);
    // systems
    void sysEconomy(Simulation& sim);
    void sysFields(Simulation& sim);
    void sysProduction(Simulation& sim);
    void sysConstruction(Simulation& sim);
    void sysGarrison(Simulation& sim);
    void sysOrders(Simulation& sim);
    void sysNeutrals(Simulation& sim);
    void sysTargeting(Simulation& sim);
    void sysMovement(Simulation& sim);
    void sysWeapons(Simulation& sim);
    void sysProjectiles(Simulation& sim);
    void sysAbilities(Simulation& sim);
    void sysDeath(Simulation& sim);
    void sysCapture(Simulation& sim);
    void sysCreep(Simulation& sim);
    void sysVision(Simulation& sim);
    void sysWaves(Simulation& sim);
    void sysVictory(Simulation& sim);

    bool runOrder(Simulation& sim, Entity e, Order& o);
    bool runGather(Simulation& sim, Entity e, Order& o);
    bool runBuild(Simulation& sim, Entity e, Order& o);
    Entity findNearestDropoff(Simulation& sim, uint8_t owner, Vec3 p, int res);
    Entity findNearestResource(Simulation& sim, uint8_t owner, Vec3 p, int res, Fixed maxDist);
    void grantKill(Simulation& sim, Entity victim, uint8_t killer);

    Rules rules_;
};

} // namespace rts::game
