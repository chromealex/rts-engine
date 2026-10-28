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
#include "RtsTriggers.h"

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
    kCmdSurrender,    // no units: the issuing player is defeated (kEvPlayerDefeated with value kDefeatSurrendered)
    // dungeon management / territory (rect commands: point = any point in the first cell, param = rectParam(w, h, extra))
    kCmdDig,          // no units; mark (extra 0) / unmark (extra 1) diggable cells in a rect
    kCmdPaintRoom,    // no units; extra = room index (-1 = erase, refunds rules.sellRefund)
    kCmdPickUp,       // units: own units into the player's hand
    kCmdDrop,         // point: drop units from the hand (units = which ones, empty = the last picked; param = count, <=0: 1)
    kCmdSlap,         // units: own units
    kCmdPossess,      // units[0] = unit to possess; empty units = release
    kCmdDirect,       // possessed unit: point = direction (x, z), param bit 0 = fire, target = optional attack target
    kCmdPlayerAbility,// no units; param = index into rules.playerAbilities, point
    kCmdBuildRoad,    // no units; rect (extra 0 build, 1 remove), cost per cell from rules.roadNetwork
    kCmdConnectRoad,  // units: own structures; the sim lays the shortest road from each to the own road network
    kCmdLearn,        // heroes (rules heroes.skills): param = ability index to learn / raise with a skill point
};

// Rect parameter of kCmdDig / kCmdPaintRoom: width and height in cells (1..64) and a signed 24-bit extra value.
inline int64_t rectParam(int w, int h, int extra = 0) {
    return (int64_t(h & 0xFFFF) << 40) | (int64_t(w & 0xFFFF) << 24) | int64_t(extra & 0xFFFFFF);
}
inline void unpackRect(int64_t p, int& w, int& h, int& extra) {
    w = int((p >> 24) & 0xFFFF); h = int((p >> 40) & 0xFFFF);
    extra = int(p & 0xFFFFFF);
    if (extra & 0x800000) extra -= 0x1000000;
}

// ---------------------------------------------------------------- events (presentation)
enum DefeatReason : int64_t { kDefeatEliminated = 0, kDefeatSurrendered = 1 }; // kEvPlayerDefeated value

enum GameEvent : uint16_t {
    kEvSpawned = 1, kEvDied, kEvAttack, kEvHit, kEvTrainStarted, kEvTrainDone, kEvBuildPlaced, kEvBuildDone,
    kEvGathered, kEvCaptured, kEvPlayerDefeated, kEvNotEnough, kEvResearched, kEvLevelUp, kEvAbility, kEvMiss,
    kEvReadyToPlace, kEvSold, kEvRegionClaimed, kEvWave, kEvLowPower, kEvUpgraded,
    kEvMessage,       // trigger message: value = index into GameState::trig.messages, player 0 = everyone
    kEvDug,           // value = cell index
    kEvTerritory,     // a cell changed owner: value = cell index, player = new owner
    kEvRoomPainted,   // value = room type (-1 erased), player
    kEvRecruited,     // a = creature, b = portal
    kEvCreatureLeft,  // unhappy creature left the dungeon
    kEvPickedUp, kEvDropped, kEvSlapped,
    kEvPossessed,     // a = unit (kNullEntity: released)
    kEvPayday,        // value = 1 paid, 0 unpaid
    kEvPlayerAbility, // value = ability index
    kEvProduced,      // a = workplace, value = resource of the first output
    kEvDelivered,     // a = carrier, b = destination, value = resource
    kEvRoadBuilt,     // value = cell index (removed: player 0)
    kEvLanesChanged,  // lane geometry regenerated (GameState::lanes)
    // abilities and effects (statuses: value = status index in Rules::statuses)
    kEvStatusStart,   // a = holder (kNullEntity: player status, player = holder slot), b = source
    kEvStatusEnd,     // a = holder; value = status
    kEvAreaStart,     // a = area entity (Transform + Area), value = payload
    kEvAreaTrigger,   // a = area, pos
    kEvAreaEnd,       // a = area
    kEvProjectileLaunch, // a = projectile, b = caster
    kEvImpact,        // payload delivered: a = target (may be null), b = caster, value = payload, pos
    kEvChainJump,     // a = from, b = to
    kEvCastStart,     // windup started: a = caster, value = ability index
    kEvChannelStart, kEvChannelEnd, // a = caster
    kEvAlert,         // player = alerted player, a = first unit of the group, b = target structure
    kEvGarrisonChanged, // a = structure, value = units delta (effects)
    kEvLearned,       // a = hero, value = ability index | level << 8
    kEvCrewKilled,    // a = vehicle (now empty), b = shooter
    kEvCrewed,        // a = vehicle, player = new owner
    kEvInfected,      // a = victim, value = spawned type
    kEvSubmerged,     // a = eater (sandworm) leaving; kEvSpawned marks its return
};

// ---------------------------------------------------------------- components
enum UnitFlags : uint8_t { kUnitHold = 1, kUnitUnderConstruction = 2, kUnitDisabled = 4 /*power deficit*/, kUnitSilent = 8 /*removed without death effects*/,
                          kUnitInHand = 16 /*picked up: off the map*/, kUnitPossessed = 32 /*directly controlled*/,
                          kUnitInfected = 64 /*killed by an infecting unit: rises as the infect type*/ };

struct Unit {
    uint16_t type = 0;
    Fixed hp{};
    uint8_t flags = 0;
    uint8_t lastAttacker = 0;   // owner slot of the last damage source (xp / bounty)
    Vec3 facing{Fixed::fromInt(1), Fixed{}, Fixed{}};
    Fixed energy{};
};

enum class OrderType : uint8_t { Move, AttackMove, Attack, Patrol, Gather, Return, Build, Repair, Enter, Cast,
                                 Dig,        // point2 = cell to dig, point = cell to stand on
                                 ClaimCell,  // point = cell to claim (claimers)
                                 Need,       // param = NeedKind | room << 8, point = room cell (or target = structure)
                                 Haul };     // carrier job, see Carrier

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
    Vec3 vel{};                  // last velocity (local avoidance)
    Vec3 yieldHome{};            // stepped aside for a passing unit; walks back here
    uint32_t yieldUntil = 0;
    bool yielded = false;
};

struct Orders { std::vector<Order> queue; };

struct Combat {
    Fixed cooldown[4] = {};
    Entity target = kNullEntity;
    uint8_t acquireTimer = 0;
    bool autoTarget = false;
    Entity chargeTarget = kNullEntity;   // charging shots: aiming at this target since chargeStart
    uint32_t chargeStart = 0;
    // melee surround: the attack slot (ring around the target) this unit is heading for
    Entity slotTarget = kNullEntity;
    int8_t slot = -1;
    uint32_t slotTick = 0;
    Vec3 slotPos{};
};

struct Cargo { int16_t res = -1; Fixed amount{}; Entity lastNode = kNullEntity; Vec3 lastPos{}; bool field = false; };

struct ProdItem { int32_t item = 0; Fixed progress{}; Fixed paid{}; bool upgrade = false; Fixed timeMult = Fixed::fromInt(1); }; // item >= 0: type; < 0: tech ~item; timeMult: hero revive

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
    int lost = 0;                    // units lost to assaults and effects (resurrection)
    uint32_t pendingGroup = 0;       // group id of the units being sent
    Entity pendingTarget = kNullEntity;
};

struct Projectile {
    Entity target = kNullEntity, source = kNullEntity;
    Vec3 aim{};
    Fixed speed{}, damage{}, splash{};
    uint16_t damageType = 0;
    uint8_t owner = 0, layers = 0;
    // ability projectiles: deliver a payload on impact (effect = Rules::payloads[effList][effIndex])
    int16_t effList = -1, effIndex = -1, onHit = -1;
    Fixed onHitChance{};
    uint16_t sourceType = 0xFFFF;
    Vec3 start{};
    Fixed travel{}, total{};  // arc progress
    Fixed scale = Fixed::fromInt(1);
    int8_t weapon = -1;       // weapon index of sourceType (bonus damage, armor class, crew kills)
};

struct Hero { int level = 1; Fixed xp{}; uint8_t skill[8] = {}; uint8_t points = 0; };   // skill / points: rules heroes.skills
struct Shield { Fixed value{}; uint32_t lastHit = 0; };       // StarCraft shields (health.shields)
struct Eater { int kills = 0; };                              // neutral eats (sandworm)
struct Squad { Fixed reinforceTimer{}; bool reinforcing = false; };

struct Buff { Stat stat = Stat::Damage; Fixed add{}; Fixed mul = Fixed::fromInt(1); uint32_t until = 0; uint16_t sourceType = kNoType; };
struct Buffs { std::vector<Buff> list; uint32_t stunUntil = 0; };
struct AbilityState { uint32_t readyAt[8] = {}; uint8_t charges[8] = {}; uint32_t chargeAt[8] = {}; bool init = false; uint32_t busyUntil = 0; };
// A running status effect (unit, structure or player).
struct StatusInst {
    uint16_t def = 0;
    uint8_t owner = 0;              // who applied it
    Entity source = kNullEntity;
    uint16_t sourceType = 0xFFFF;
    uint32_t until = 0, nextTick = 0;
    uint16_t stacks = 1;
    Fixed shield{}, acc{};
    Entity link = kNullEntity;      // Link / Redirect partner
    uint16_t revertType = 0xFFFF;   // Transform / Level: type to go back to
    uint8_t revertOwner = 0;        // Capture: owner to give it back to
    bool revertsOwner = false;
    uint8_t capturedBy = 0;
};
struct Statuses { std::vector<StatusInst> list; uint32_t flags = 0; };   // flags: union of active StatusFlags
// Ground effect: delayed strike, persistent field, moving hazard, mine, portal, reveal, soul trap.
struct Area {
    Entity caster = kNullEntity;
    uint16_t sourceType = 0xFFFF;
    int16_t effList = -1, effIndex = -1;  // the Area effect in Rules::payloads
    uint32_t fireAt = 0, nextAt = 0, endAt = 0;
    Fixed radius{};
    int triggers = 0;
    std::vector<uint64_t> touched;        // contact areas: entities already hit
    Vec3 dir{}, dest{};
    int lane = -1; uint16_t lanePos = 0; bool laneReverse = false;
    Entity follow = kNullEntity;
    Fixed scale = Fixed::fromInt(1);
};
struct Channel { int16_t payload = -1; Entity target = kNullEntity; Vec3 point{}, startPos{}; uint32_t nextAt = 0, endAt = 0; int ability = 0; Fixed interval{}; };
struct GroupTag { uint32_t id = 0; Entity from = kNullEntity, to = kNullEntity; };
struct Capture { Fixed progress{}; uint8_t team = 0; };
struct Guard { Vec3 home{}; Fixed leash{}; bool returning = false; uint32_t nextWander = 0; };
struct Summon { uint32_t expire = 0; uint16_t summonerType = 0xFFFF; Entity summoner = kNullEntity; };

enum NeedKind : int32_t { kNeedHunger = 0, kNeedSleep = 1, kNeedWork = 2 };
struct Needs {
    Fixed hunger{}, sleep{};          // 0..1+: active at >= 1
    Fixed happiness = Fixed::fromInt(1);
    Fixed payTimer{}, unhappyTime{};
    bool leaving = false;
};
struct Possessed { Vec3 dir{}; Entity target = kNullEntity; bool fire = false; uint32_t lastTick = 0; };
struct PortalState { Fixed timer{}; };
struct AutoTrainState { Fixed timer{}; };
struct ClaimState { Fixed acc{}; };
struct Advancer { uint8_t unused = 0; };     // keeps pushing to the nearest front when idle (autoTrain units)

// Settlers economy
struct Inventory { std::vector<Fixed> have, incoming, reserved; };   // per resource
struct Workshop { Fixed progress{}; bool running = false; };
struct Carrier { Entity from = kNullEntity, to = kNullEntity; int16_t res = -1; Fixed amount{}, loaded{}; };

// Mushroom Wars 2 lanes
struct Lane { Entity a = kNullEntity, b = kNullEntity; int offset = 0; std::vector<Vec3> points; }; // offset: <0 left, >0 right
struct LaneWalker { int lane = -1; uint16_t pos = 0; bool reverse = false; };

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
    std::vector<uint8_t> typeDisabled;         // per type: switched off by triggers
    std::vector<StatusInst> statuses;          // player statuses (silence, freeze regen, passives, wind)
    uint32_t statusFlags = 0;
    // fidelity mechanics (RtsOriginals.cpp)
    std::vector<Fixed> gatherMultRes;          // upkeep per resource (empty: gatherMult for all)
    int heroesMade = 0;                        // heroes trained so far (free first hero)
    struct DeadHero { uint16_t type; int level; Fixed xp; uint8_t skill[8]; };
    std::vector<DeadHero> deadHeroes;          // revive keeps the level and skills
    std::vector<uint8_t> fieldCells;           // derived: cells covered by own power fields (needsField)
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

struct RoomZone { uint16_t type = 0; uint8_t owner = 0; std::vector<int32_t> cells; Vec3 center{}; };
struct PlayerAbilityState {
    uint32_t readyAt = 0; int charges = 0; Fixed rearm{};
    bool owned = true;                 // in the loadout (or stolen)
    uint8_t level = 1;
    uint32_t stolenUntil = 0;          // > 0: a stolen skill, lost at this tick
    uint32_t pendingAt = 0;            // activation delay: fires at this tick
    Vec3 pendingPoint{}; Entity pendingTarget = kNullEntity;
    uint32_t nextAuto = 0;
};

// Who / where an effect payload is applied (see RtsEffects.cpp).
struct EffectCtx {
    Entity caster = kNullEntity;
    uint8_t owner = 0;
    uint16_t sourceType = 0xFFFF;
    Entity target = kNullEntity;
    Vec3 point{}, origin{};
    Fixed scale = Fixed::fromInt(1);   // chain / splash falloff
    int depth = 0;
    uint32_t group = 0;
};

struct GameState {
    std::vector<PlayerEconomy> players;   // index = slot (0 = neutral)
    MapGrid grid;
    RoadGraph roads;
    SpatialHash hash;
    bool roadsOnly = false;
    std::vector<std::vector<uint8_t>> vision; // per player per cell: 0 unexplored, 1 explored, 2 visible
    std::vector<std::vector<int32_t>> visibleCells; // per player: cells at 2 (derived, so fog reset is O(visible))
    uint8_t fogMode = 0;                            // FogMode of this match (rules, map override)
    std::vector<int64_t> visionBase, visionOcc;     // line of sight: ground / occluder heights (1/64 units, derived)
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
    int winnerTeam = -1;                 // set by timed modes / triggers
    TriggerState trig;
    // dungeon / territory
    std::vector<int> territoryCells;     // per player slot: owned cells (HUD share = territoryCells[p] / territoryTotal)
    int territoryTotal = 0;              // claimable cells (ground or diggable)
    bool territoryDirty = true;          // presentation: territory / dig / room layers changed
    std::vector<RoomZone> roomZones;     // connected painted areas
    std::vector<std::vector<int>> roomCells; // [player][room type] cells owned
    bool roomsDirty = true;
    std::vector<std::vector<Entity>> hand;   // per player: units picked up (order = pick order)
    std::vector<Entity> possessed;           // per player
    std::vector<std::vector<PlayerAbilityState>> playerAbilities; // [player][ability]
    uint32_t nextGroupId = 1;                // ids of groups of units sent together
    // settlers economy
    std::vector<int32_t> roadComp;           // per cell: road network component (-1 none)
    bool roadsDirty = true;
    // lanes (Mushroom Wars 2): every lane from a to b (offset lanes around the main spline), for the view
    std::vector<Lane> lanes;
    bool lanesDirty = true;
    uint32_t laneSignature = 0;              // garrisons/obstacles the lanes were built for
    std::vector<uint32_t> laneRoundRobin;    // per garrison entity index: next lane slot
    // fidelity mechanics: eaters (sandworms) that submerged and come back later
    struct Resurface { uint16_t type; uint8_t owner; uint32_t at; };
    std::vector<Resurface> resurface;
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
    const TriggerSet& triggers() const { return triggers_; }
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
    // Heightfield terrain (rules terrain.model "heightfield"): ground height under p (0 otherwise), and the
    // height advantage of a over b in cliff levels - whole levels on cliff maps, fractional on heightfields.
    Fixed groundHeight(Simulation& sim, Vec3 p) const;
    Fixed heightAdvantage(Simulation& sim, Vec3 a, Vec3 b) const;
    Fixed slopeSpeed(Simulation& sim, Vec3 p, Vec2 dir) const;   // uphill / downhill speed factor (1 off heightfields)
    int regionAt(Simulation& sim, Vec3 p) const;
    bool canClaim(Simulation& sim, uint8_t player, int region, std::vector<Amount>* cost = nullptr) const;
    Entity findNearestFieldCell(Simulation& sim, Vec3 p, int res, Vec3& out, int maxRadius = 24) const;
    uint8_t teamOf(Simulation& sim, uint8_t player) const;

    // ---- internals (used across implementation files)
    void issue(Simulation& sim, Entity e, const Order& o, bool queue);
    void moveTo(Simulation& sim, Entity e, Vec3 goal, Fixed radius);
    void stopMoving(Simulation& sim, Entity e);
    void dealDamage(Simulation& sim, Entity target, Fixed dmg, uint16_t dmgType, Entity source, uint8_t sourceOwner, const WeaponDef* wd = nullptr);
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
    // ---- dungeon / territory
    void setTerritory(Simulation& sim, int cell, uint8_t owner);
    int territoryOf(Simulation& sim, uint8_t player) const;
    std::vector<Amount> techCost(Simulation& sim, uint8_t player, uint16_t tech) const; // grows with levels
    bool canUsePlayerAbility(Simulation& sim, uint8_t player, int index) const;
    int roomCellsOf(Simulation& sim, uint8_t player, int room) const;
    void applyEffects(Simulation& sim, Entity caster, uint8_t owner, uint16_t sourceType, const std::vector<EffectDef>& effects, Entity target, Vec3 point);
    void addHeroXp(Simulation& sim, Entity hero, Fixed xp, uint8_t player);
    // ---- abilities and effects (RtsEffects.cpp, RtsSkills.cpp)
    void applyPayload(Simulation& sim, int payload, const EffectCtx& ctx);
    const AbilityDef& abilityFor(Simulation& sim, Entity e, int index) const;   // level variant of the unit's ability
    const PlayerAbilityDef& playerAbilityFor(Simulation& sim, uint8_t player, int index) const;
    uint32_t statusFlags(Simulation& sim, Entity e) const;                       // union of active status flags
    bool isStunned(Simulation& sim, Entity e) const;
    void addStatus(Simulation& sim, Entity target, int def, Fixed duration, const EffectCtx& ctx);
    void addPlayerStatus(Simulation& sim, uint8_t player, int def, Fixed duration, const EffectCtx& ctx);
    int dispel(Simulation& sim, Entity target, uint64_t tags, uint8_t from);    // removed count
    bool castPlayerAbility(Simulation& sim, uint8_t player, int index, Vec3 point, Entity target);
    Fixed playerStat(Simulation& sim, uint8_t player, Stat s, Fixed base) const; // player statuses
    std::vector<Entity> groupMembers(Simulation& sim, uint32_t group) const;
    // Where to aim a skill (autocast, auto skills, AI): fills point / target; false when there is nothing worth it.
    bool smartTarget(Simulation& sim, uint8_t player, int payload, TargetKind kind, SmartRule rule, Vec3& point, Entity& target,
                     Vec3 from = {}, Fixed range = {});
    // ---- settlers economy
    bool isGood(uint16_t res) const;       // stored in buildings (rules.storage == "buildings")
    bool roadConnected(Simulation& sim, Entity a, Entity b);
    std::vector<Amount> upfrontCost(const std::vector<Amount>& cost) const; // the part paid at once (goods are delivered)
    // lanes between garrison buildings a -> b (main lane first), empty when none
    std::vector<int> lanesBetween(Simulation& sim, Entity a, Entity b) const;
    // ---- fidelity mechanics (RtsOriginals.cpp)
    std::vector<Amount> trainCost(Simulation& sim, uint8_t player, uint16_t type, Fixed* timeMult = nullptr) const; // growth, free hero, revive
    bool heroAllowed(Simulation& sim, uint8_t player, uint16_t type) const;   // limit and tiers
    bool skillLearned(Simulation& sim, Entity e, int index) const;
    bool canLearn(Simulation& sim, Entity e, int index) const;
    bool inField(Simulation& sim, uint8_t player, int16_t field, Vec3 centre, const UnitType& t) const;
    Fixed gatherMultFor(Simulation& sim, uint8_t player, int res) const;
    int gatherersAt(Simulation& sim, Entity node) const;
    bool canCrew(Simulation& sim, Entity unit, Entity vehicle) const;

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
    void sysTriggers(Simulation& sim);
    void sysDungeon(Simulation& sim);     // RtsDungeon.cpp: jobs, needs, portals, territory, possession
    void setupDungeon(Simulation& sim, const Json& map);
    void applyDungeonCommand(Simulation& sim, const Command& cmd);
    bool runDig(Simulation& sim, Entity e, Order& o);
    bool runClaimCell(Simulation& sim, Entity e, Order& o);
    bool runNeed(Simulation& sim, Entity e, Order& o);
    void movePossessed(Simulation& sim);
    void moveAgents(Simulation& sim);     // RtsAvoidance.cpp: path following with ORCA avoidance and yielding
    void recomputeRooms(Simulation& sim);
    void roomEconomy(Simulation& sim);
    void hashDungeon(Simulation& sim, Hasher& h);
    void applyEffect(Simulation& sim, const EffectDef& e, int list, int index, EffectCtx ctx);
    std::vector<Entity> effectTargets(Simulation& sim, const EffectDef& e, const EffectCtx& ctx);
    std::vector<uint8_t> effectPlayers(Simulation& sim, const EffectDef& e, const EffectCtx& ctx);
    bool filterOk(Simulation& sim, const TargetFilter& f, Entity e, uint8_t owner, Entity caster) const;
    void effectGarrison(Simulation& sim, const EffectDef& e, EffectCtx& ctx, const std::vector<Entity>& targets);
    void effectGroup(Simulation& sim, const EffectDef& e, EffectCtx& ctx, const std::vector<Entity>& targets);
    void effectPlayer(Simulation& sim, const EffectDef& e, EffectCtx& ctx);
    void sysEffects(Simulation& sim);     // statuses, areas, channels, player skills
    void tickAreas(Simulation& sim);
    void tickStatuses(Simulation& sim);
    void tickPlayerSkills(Simulation& sim);
    void impactProjectile(Simulation& sim, Entity p);
    void setupSkills(Simulation& sim, const Json& map);
    void hashEffects(Simulation& sim, Hasher& h);
    void enterGarrison(Simulation& sim, Entity unit, Entity structure);   // assault / reinforce with statuses (advanced)
    Fixed rollAmount(Simulation& sim, const EffectDef& e);
    void sysSettlers(Simulation& sim);    // RtsSettlers.cpp: workshops, carriers, storehouse stock, roads
    void applySettlersCommand(Simulation& sim, const Command& cmd);
    bool runHaul(Simulation& sim, Entity e, Order& o);
    void recomputeRoads(Simulation& sim);
    void depositGoods(Simulation& sim, uint8_t player, uint16_t res, Fixed amount);
    void withdrawGoods(Simulation& sim, uint8_t player, uint16_t res, Fixed amount);
    void syncStorehouses(Simulation& sim);
    void hashSettlers(Simulation& sim, Hasher& h);
    void sysLanes(Simulation& sim);       // RtsLanes.cpp
    void rebuildLanes(Simulation& sim);
    bool runLaneWalk(Simulation& sim, Entity e, Order& o);
    bool attackSlot(Simulation& sim, Entity e, Entity target, Fixed range, Vec3& out); // melee surround
    void moveToVia(Simulation& sim, Entity e, Vec3 goal, Fixed radius, bool aroundAllies);
    void runTriggerActions(Simulation& sim, size_t def, uint8_t player, Entity unit, Vec3 where);
    bool triggerConditions(Simulation& sim, const TriggerDef& d, uint8_t player);

    // fidelity mechanics hooks (RtsOriginals.cpp)
    void sysOriginals(Simulation& sim);
    void hashOriginals(Simulation& sim, Hasher& h);
    void economyOriginals(Simulation& sim);                         // fields, upkeep per resource, territory income
    Fixed mitigate(Simulation& sim, Entity target, Fixed raw, const WeaponDef* wd, Entity source);
    void afterHit(Simulation& sim, Entity target, Entity source, const WeaponDef* wd, bool killed);
    void onDeathOriginals(Simulation& sim, Entity e, uint8_t owner, Vec3 pos, bool silent);
    void onHeroSpawn(Simulation& sim, Entity e, uint8_t owner);
    void gainOn(Simulation& sim, uint8_t player, Fixed ResourceDef::*field, Fixed count);
    bool crewEnter(Simulation& sim, Entity unit, Entity vehicle);
    void applyLearn(Simulation& sim, Entity e, int index);
    Fixed playerModsStat(Simulation& sim, uint8_t player, uint16_t type, Stat s, Fixed& mul) const;
    bool runOrder(Simulation& sim, Entity e, Order& o);
    bool runGather(Simulation& sim, Entity e, Order& o);
    bool runBuild(Simulation& sim, Entity e, Order& o);
    Entity findNearestDropoff(Simulation& sim, uint8_t owner, Vec3 p, int res);
    Entity findNearestResource(Simulation& sim, uint8_t owner, Vec3 p, int res, Fixed maxDist);
    void grantKill(Simulation& sim, Entity victim, uint8_t killer);

    Rules rules_;
    TriggerSet triggers_;
    struct SlotClaim { uint64_t target = 0; int slot = 0; uint64_t claimer = 0; };
    std::vector<SlotClaim> slotClaims_;   // rebuilt every tick from Combat slots (derived, not state)
    struct DeathSpawn { uint16_t type; uint8_t owner; Vec3 pos; int count; };
    std::vector<DeathSpawn> deathSpawns_; // filled and emptied inside sysDeath (infection)
    mutable std::vector<uint16_t> gatherCount_;   // derived per tick: workers harvesting at each node (by entity index)
    mutable uint32_t gatherCountTick_ = UINT32_MAX;
    void assaultOriginals(Simulation& sim, Entity unit, Entity structure);   // gains for units lost in an assault
    void captureOriginals(Simulation& sim, Entity structure, uint8_t from, uint8_t to);
    void flushDeathSpawns(Simulation& sim);
};

} // namespace rts::game
