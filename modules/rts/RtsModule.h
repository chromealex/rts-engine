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
#include "RtsMech.h"
#include <memory>

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
    kCmdTransit,      // planets: orbit units, param = target body index; timed transfer to that body's orbit shell
    // agent-mech (RtsMech*.cpp)
    kCmdStance,       // units; param = StanceMode (0 aggressive, 1 defensive, 2 hold, 3 passive)
    kCmdRetreat,      // units: run to the nearest own retreatPoint (rules "retreat")
    kCmdLoad,         // units = passengers, target = transport (or units = transports, target = a passenger)
    kCmdUnload,       // units = transports; point: move there first (param 1), else unload where they stand
    kCmdGuard,        // units follow `target` and fight what attacks near it
    kCmdFollow,       // units follow `target`
    kCmdFormation,    // units; param = shape (0 line, 1 box, 2 wedge, 3 loose, -1 break up)
    kCmdBuildLine,    // builders; point = start, param = type | end x << 16 | end z << 32 (cells): walls in a line
    kCmdMerge,        // units: pair up into their mergeInto result (archons)
    kCmdSplit,        // units with splitInto
    kCmdTrade,        // units[0] = own market; param = resource | (1 << 8 when selling)
    kCmdTradeRoute,   // traders; target = destination market (own or allied), home = nearest own market
    kCmdTribute,      // param = resource | to player << 8 | amount << 16
    kCmdDiplomacy,    // param = other player | 1 << 8 ally (0: enemy)
    kCmdBuyItem,      // units[0] = buyer (inventory), target = shop, param = item index
    kCmdUseItem,      // units[0]; param = slot; target / point for the effect (self by default)
    kCmdDropItem,     // units[0]; param = slot
    kCmdPickItem,     // units[0]; point = where the ground item lies
    kCmdLearn,        // heroes (rules heroes.skills): param = ability index to learn / raise with a skill point
    kCmdBounty,       // point or target; param = resource | reward << 8 (autonomous heroes go for it)
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
    kEvTransitStart,  // planets: a = unit, value = target body, pos = departure point
    kEvTransitEnd,    // planets: a = unit, value = body arrived at, pos = arrival point
    kEvTriggerFired,    // tools/debug: value = trigger index (Rules triggers first, then map), player = triggering slot, a = unit
    kEvLoaded,          // agent-mech: a = passenger, b = transport, value = passengers aboard
    kEvUnloaded,        // a = passenger, b = transport, value = passengers left
    kEvCyclePhase,      // agent-mech: value = cycle index << 8 | phase index (day/night, seasons)
    kEvTraded,          // player, value = resource | 1 << 8 when sold (market); trader income: a = trader
    kEvDiplomacy,       // player = who, value = other | 1 << 8 when allied
    kEvItem,            // a = unit, value = item index; b: shop (bought) / null (used, picked, dropped)
    kEvMerged,          // a = result
    kEvObjective,       // campaigns: value = objective index | state << 16 (0 added, 1 done, 2 failed), player
    kEvDialog,          // value = index into MechState::dialogs, a = speaking unit (if any), player 0 = everyone
    kEvCamera,          // pos = where to look, value = duration in ms | 1 << 31 when the camera is locked, player
    kEvMissionEnd,      // value 1 victory / 0 defeat, player
    kEvLearned,       // a = hero, value = ability index | level << 8
    kEvCrewKilled,    // a = vehicle (now empty), b = shooter
    kEvCrewed,        // a = vehicle, player = new owner
    kEvInfected,      // a = victim, value = spawned type
    kEvSubmerged,     // a = eater (sandworm) leaving; kEvSpawned marks its return
    kEvTrapFired,     // a = trap, b = victim, pos = victim
    kEvGroupTeleport, // a = unit moved, b = caster (may be null), pos = arrival, value = 1 when it is a return trip
    kEvPaved,         // a = slab (removed), value = cells paved, pos = slab centre
    kEvMorale,          // a = unit, value = 0 steady / 1 wavering / 2 routing (stage 4)
};

// ---------------------------------------------------------------- components
enum UnitFlags : uint16_t { kUnitHold = 1, kUnitUnderConstruction = 2, kUnitDisabled = 4 /*power deficit*/, kUnitSilent = 8 /*removed without death effects*/,
                          kUnitInHand = 16 /*picked up: off the map*/, kUnitPossessed = 32 /*directly controlled*/,
                          kUnitInTransit = 64 /*between bodies (set together with kUnitInHand: off every surface)*/,
                          kUnitInCargo = 128 /*inside a transport: off the map (agent-mech)*/,
                          kUnitInfected = 256 /*killed by an infecting unit: rises as the infect type (originals)*/ };
constexpr uint16_t kUnitOffMap = kUnitInHand | kUnitInCargo;

struct Unit {
    uint16_t type = 0;
    Fixed hp{};
    uint16_t flags = 0;         // UnitFlags (hashed as u32)
    uint8_t lastAttacker = 0;   // owner slot of the last damage source (xp / bounty)
    Vec3 facing{Fixed::fromInt(1), Fixed{}, Fixed{}};
    Fixed energy{};
};

enum class OrderType : uint8_t { Move, AttackMove, Attack, Patrol, Gather, Return, Build, Repair, Enter, Cast,
                                 Dig,        // point2 = cell to dig, point = cell to stand on
                                 ClaimCell,  // point = cell to claim (claimers)
                                 Need,       // param = NeedKind | room << 8, point = room cell (or target = structure)
                                 Haul,       // carrier job, see Carrier
                                 Board,      // agent-mech: walk to the transport `target` and get in
                                 Unload,     // agent-mech (transports): go to `point` (state 0) then unload
                                 Follow,     // agent-mech: stay near `target`; param 1 = guard it
                                 Rearm,      // go to the dock `target` and reload ammo
                                 Trade,      // trader loop between its markets (Trader component)
                                 PickItem,   // walk to `point` and pick up the ground item there
                                 Merge };    // walk to `target` and merge with it

struct Order {
    OrderType type = OrderType::Move;
    Entity target = kNullEntity;
    Vec3 point{};
    Vec3 point2{};
    int32_t param = 0;
    uint8_t state = 0;
    Fixed timer{};
    bool flow = false;           // move along a shared flow field (rules movement.flowField)
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
    // flow field following (RtsFlowField.cpp): the field is keyed by (goal cell, radius, path layer, clearance)
    bool flow = false;
    uint8_t flowRadius = 0;
    uint8_t flowStuck = 0;       // stuck events while on the field: the first re-samples it, the second falls back to A*
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
struct Weathering { Fixed bare{}; };                          // share of the footprint off the owner's foundation (Dune II)
struct TrapState { int charges = 0; uint32_t readyAt = 0; };  // traps (RtsFidelity.cpp)
struct TeleportBack { Vec3 home{}; uint32_t at = 0; Entity caster = kNullEntity; }; // group teleport with a return trip
// Interplanetary transfer (kCmdTransit): leaves the shell of `from` at depart, arrives on the shell of `to`.
// from/to world positions are the shell points at departure (body positions at that tick), for the view.
struct Transit { int16_t from = 0, to = 0; uint32_t depart = 0, arrive = 0; Vec3 fromWorld{}, toWorld{}; Vec3 arrival{}; };
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
    Vec3 point2{};                             // second point of the last pointPair player ability (RtsFidelity.cpp)
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
    std::vector<Vec3> route;   // agent-mech: waypoints (map areas / points) the wave walks before its objective
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
    MechState mech;                           // agent-mech global state (RtsMech.h)
    bool mechActive = false;                  // agent-mech: a mech component exists (stance given in any game); derived
    std::vector<PlayerEconomy> players;   // index = slot (0 = neutral)
    MapGrid grid;
    RoadGraph roads;
    SpatialHash hash;
    bool roadsOnly = false;
    std::vector<std::vector<uint8_t>> vision; // per player per cell: 0 unexplored, 1 explored, 2 visible
    std::vector<std::vector<int32_t>> visibleCells; // per player: cells at 2 (derived, so fog reset is O(visible))
    uint8_t fogMode = 0;                            // FogMode of this match (rules, map override)
    std::vector<int64_t> visionBase, visionOcc;     // line of sight: ground / occluder heights (1/64 units, derived)
    uint64_t visionToken = 0;                       // vision cache that last updated this state (RtsVision.cpp; not saved / hashed)
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
    // planets (map `world` block): bodies, cube-sphere layout of the grid rows (RtsSurface.h); null on flat maps
    std::shared_ptr<const Surface> surface;
    // fidelity mechanics: eaters (sandworms) that submerged and come back later
    struct Resurface { uint16_t type; uint8_t owner; uint32_t at; };
    std::vector<Resurface> resurface;
};

// ---------------------------------------------------------------- module
struct VisionCache;   // RtsVision.cpp

class RtsModule final : public GameModule {
public:
    std::string name() const override { return "rts"; }
    void setup(Simulation& sim, const Json& rules, const Json& map) override;
    void applyCommand(Simulation& sim, const Command& cmd) override;
    void hashState(Simulation& sim, Hasher& h) override;
    bool serializeState(Simulation& sim, Archive& a) override;   // RtsSave.cpp
    int result(Simulation& sim) override;

    const Rules& rules() const { return rules_; }
    const TriggerSet& triggers() const { return triggers_; }
    // The map the simulation was set up from when the map had a "generate" block (nullptr otherwise):
    // presentation reads terrainMaterial / heights / regions from it (agent-terrain-gen).
    const Json* generatedMap() const { return generated_.isObject() ? &generated_ : nullptr; }
    GameState& state(Simulation& sim) { return *sim.world().resource<GameState>(); }

    // ---- queries shared with AI, client and editor
    Entity spawn(Simulation& sim, uint16_t type, uint8_t owner, Vec3 pos, bool underConstruction = false);
    // Full placement check for `player` (terrain, blocking, radius, regions, creep, placeOn...).
    bool canPlace(Simulation& sim, uint8_t player, uint16_t type, Vec3 centre, Entity ignore = kNullEntity, std::string* why = nullptr);
    // Footprint cells of a structure centred near `centre` (grid coordinates). On planets (surface given) the
    // footprint lies inside one face; one that would cross a face seam gets x0 = y0 = -1 (never buildable).
    static void footprintAt(const UnitType& t, Vec3 centre, int& x0, int& y0, const Surface* surface = nullptr);
    static Vec3 snapToFootprint(const UnitType& t, Vec3 p, const Surface* surface = nullptr);
    bool requirementsMet(Simulation& sim, uint8_t player, const std::vector<Requirement>& req) const;
    bool canTrain(Simulation& sim, uint8_t player, uint16_t type) const; // requirements + unique
    bool canAfford(Simulation& sim, uint8_t player, const std::vector<Amount>& cost, Fixed supply = {}, int* missing = nullptr) const;
    bool isVisible(Simulation& sim, uint8_t player, Vec3 p) const;
    // Tests / tools: run the brute-force reference vision instead of the cached one (results are identical;
    // RTS_VISION=ref does the same, RTS_VISION=check runs both and aborts on a difference).
    void setVisionReference(bool on);
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
    // p lifted to `h` above the ground (flat: y = ground height + h; planets: along the local up)
    Vec3 lift(Simulation& sim, Vec3 p, Fixed h) const;
    // ---- planets: what the view / editor / AI call (all no-ops / flat answers on flat maps)
    const Surface* surface(Simulation& sim) const;                // null on flat maps
    int bodyOf(Simulation& sim, Vec3 p) const;                     // 0 on flat maps
    // Body centre in world space at a sim tick (circular orbits, Fixed). View: float(world) at tick + alpha.
    Vec3 bodyWorldPosition(Simulation& sim, int body, uint32_t tick) const;
    // World-space position of an entity at a tick: body centre + body-local position; units in transit are
    // interpolated between the departure and arrival shell points.
    Vec3 worldPosition(Simulation& sim, Entity e, uint32_t tick) const;
    // Visibility of a planet cell (body, face, x, y) for a player: 0 unexplored, 1 explored, 2 visible.
    uint8_t cellVision(Simulation& sim, uint8_t player, int body, int face, int x, int y) const;
    bool canTransit(Simulation& sim, Entity e, int body) const;
    int regionAt(Simulation& sim, Vec3 p) const;
    bool canClaim(Simulation& sim, uint8_t player, int region, std::vector<Amount>* cost = nullptr) const;
    Entity findNearestFieldCell(Simulation& sim, Vec3 p, int res, Vec3& out, int maxRadius = 24) const;
    uint8_t teamOf(Simulation& sim, uint8_t player) const;

    // ---- internals (used across implementation files)
    void issue(Simulation& sim, Entity e, const Order& o, bool queue);
    void moveTo(Simulation& sim, Entity e, Vec3 goal, Fixed radius);
    // Shared flow field toward a goal area of `radius` world units (falls back to moveTo when no field applies).
    void moveToFlow(Simulation& sim, Entity e, Vec3 goal, Fixed radius);
    void sysFlowFields(Simulation& sim);   // refreshes the waypoints of units following fields (start of movement)
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

    // ---- agent-mech mechanics (RtsMech*.cpp)
    bool mechOn(Simulation& sim) const { return rules_.mech.any || sim.world().resource<GameState>()->mechActive; }
    bool applyMechCommand(Simulation& sim, const Command& cmd);   // true: handled (kCmdStance...; smart click on a transport)
    void sysMech(Simulation& sim);                                 // stances, retreat, transports
    void hashMech(Simulation& sim, Hasher& h, Entity e);           // per object, only components that exist
    void mechStatMod(Simulation& sim, Entity e, Stat s, Fixed& add, Fixed& mul) const;   // retreat speed, cargo arrows, veterancy
    Fixed mechDamageTaken(Simulation& sim, Entity e, Entity source) const;   // retreat, cover, armor arcs
    bool mechCanAcquire(Simulation& sim, Entity e) const;          // auto-targeting allowed (passive stance, cargo)
    bool mechMayChase(Simulation& sim, Entity e, Vec3 targetPos);  // defensive stance leash
    bool runBoard(Simulation& sim, Entity e, Order& o);
    bool runUnload(Simulation& sim, Entity e, Order& o);
    void mechOnDeath(Simulation& sim, Entity e);                   // a dying transport drops / kills its passengers
    bool board(Simulation& sim, Entity passenger, Entity carrier); // false: no room / not accepted
    void unloadOne(Simulation& sim, Entity carrier, size_t index);
    bool accepts(Simulation& sim, Entity carrier, Entity passenger) const;
    Entity nearestRetreatPoint(Simulation& sim, uint8_t owner, Vec3 from) const;
    bool detected(Simulation& sim, Entity e, uint8_t viewer) const;      // an allied detector reveals it
    bool canSee(Simulation& sim, Entity e, uint8_t viewer) const;        // not invisible to viewer (statuses + detectors)
    bool mechToggle(Simulation& sim, Entity caster, int ability);        // true: a toggle ability, handled
    void mechOnHit(Simulation& sim, Entity target, Entity source, Fixed dmg);       // veterancy: who hit last
    void mechOnSpawn(Simulation& sim, Entity e, Entity producer);        // rally waypoints
    bool runFollow(Simulation& sim, Entity e, Order& o);
    bool applyMechCommand2(Simulation& sim, const Command& cmd);    // guard, follow, rally waypoints
    void mechTick(Simulation& sim);                                  // veterans, toggles
    void mechVeteranKill(Simulation& sim, Entity victim);
    // combat / economy models (RtsMechCombat.cpp)
    void mechClassDamage(Simulation& sim, Entity attacker, size_t weapon, Entity target, Fixed& dmg, uint16_t& dmgType) const;
    Fixed mechArmorDamage(Simulation& sim, Entity target, Fixed raw, uint16_t dmgType) const;   // armor model
    Fixed mechShieldAbsorb(Simulation& sim, Entity target, Fixed dmg);                          // what is left for hp
    bool mechNodeHasRoom(Simulation& sim, Entity node, Entity worker) const;                    // maxWorkers
    void mechTickCombat(Simulation& sim);                                                       // shields
    // stage 2 (RtsMechWorld.cpp, RtsMechEconomy.cpp, RtsMechGroups.cpp)
    bool applyMechCommand3(Simulation& sim, const Command& cmd);
    void mechTickWorld(Simulation& sim);          // gates, bridges, hazards, cycles, docks
    void mechTickEconomy(Simulation& sim);        // traders, prices, deck setup
    void mechTickGroups(Simulation& sim);         // formation speeds, merges
    bool mechUseAmmo(Simulation& sim, Entity e, size_t weapon);
    bool runRearm(Simulation& sim, Entity e, Order& o);
    bool runTrade(Simulation& sim, Entity e, Order& o);
    bool runPickItem(Simulation& sim, Entity e, Order& o);
    bool runMerge(Simulation& sim, Entity e, Order& o);
    bool mechDeckAllows(Simulation& sim, uint8_t player, int ability, Vec3 point) const;
    bool mechInHand(Simulation& sim, uint8_t player, int ability) const;
    void mechDeckPlayed(Simulation& sim, uint8_t player, int ability);
    void mechDropItems(Simulation& sim, Entity victim);
    void mechBridgeDied(Simulation& sim, Entity e);
    void hashMechGlobal(Simulation& sim, Hasher& h);
    Fixed marketPrice(Simulation& sim, int res) const;
    void mechSetup(Simulation& sim, const Json& map);   // decks, prices, alliances (after setupMap)
    void mechTunnelGather(Simulation& sim, Entity tunnel);   // pull the network's passengers into this exit
    bool applyMechEconomyCommand(Simulation& sim, const Command& cmd);   // market, trade, tribute, diplomacy, items
    void campaignAction(Simulation& sim, const TrigAction& a, const std::vector<uint8_t>& players, Entity unit, Vec3 at);
    // stage 3 (RtsMechTactics.cpp, RtsMechSociety.cpp)
    bool mechOnFire(Simulation& sim, Entity e, size_t weapon, Entity target);   // turret aim, ammo, suppression, crews, craters
    void mechTickTactics(Simulation& sim);     // suppression decay, turrets, crush
    void mechTickSociety(Simulation& sim);     // growth, autonomy, rounds
    void mechTacticsDeath(Simulation& sim, Entity e);   // ruins, infection
    bool mechRecrew(Simulation& sim, const Command& cmd);
    bool mechSwitchedOff(Simulation& sim, Entity e) const;   // unpowered / unstaffed structures
    Fixed mechIncomeMult(Simulation& sim, Entity e) const;
    bool mechPowered(Simulation& sim, uint8_t player, Vec3 at) const;
    Fixed staffFraction(Simulation& sim, Entity e) const;
    bool applyMechSocietyCommand(Simulation& sim, const Command& cmd);   // bounty, garrison-paid upgrades
    bool inMechUpgrade_ = false;
    // stage 4 (RtsMechBattle.cpp): morale, rout, fatigue, charge, supply
    void mechTickBattle(Simulation& sim);
    void mechMoraleHit(Simulation& sim, Entity target, Entity source, Fixed dmg);
    void mechMoraleDeath(Simulation& sim, Entity victim);
    bool mechBattleFire(Simulation& sim, Entity e);      // false while routing; starts a charge
    void mechBattleStat(Simulation& sim, Entity e, Stat s, Fixed& mul) const;
    bool mechRouting(Simulation& sim, Entity e) const;
    Fixed moraleMaxOf(uint16_t type) const;
    void mechWaveRoute(Simulation& sim, Entity e, const std::vector<Vec3>& route);

private:
    Json generated_;                 // generated map (see generatedMap())
    void setupMap(Simulation& sim, const Json& map);
    // ---- planets (RtsPlanets.cpp)
    void setupWorld(Simulation& sim, const Json& map);               // world block -> Surface + stacked grid
    Vec3 mapPos(Simulation& sim, const Json& p) const;               // [x, z] / [x, y, z] or {"body", "face", "x", "y"}
    void moveSphere(Simulation& sim);                                // movement + separation on planet surfaces / shells
    // vision (RtsVision.cpp): fast cached passes and the brute-force reference they must match exactly
    void visionFlatFast(Simulation& sim);
    void visionSphereFast(Simulation& sim);
    void visionFlatRef(Simulation& sim);
    void visionSphereRef(Simulation& sim);
    std::shared_ptr<VisionCache> vcache_;   // derived, never state (see RtsVision.cpp)
    bool visionRef_ = false;
    void sysTransit(Simulation& sim);
    void applyTransit(Simulation& sim, const Command& cmd);
    void hashWorld(Simulation& sim, Hasher& h);
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
    void rebuildLanesSphere(Simulation& sim, const std::vector<Entity>& nodes);   // planets: same-body pairs, on the surface
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
    // foundations, traps, group teleports, charge effects (RtsFidelity.cpp)
    void sysFidelity(Simulation& sim);
    void hashFidelity(Simulation& sim, Hasher& h);
    void spawnFidelity(Simulation& sim, Entity e, bool underConstruction);   // slabs pave, weathering, hidden traps
    void builtFidelity(Simulation& sim, Entity e);                           // construction finished
    void deathFidelity(Simulation& sim, Entity e);                           // foundation under a destroyed structure
    void effectFidelity(Simulation& sim, const EffectDef& e, EffectCtx& ctx);
    void takePoint2(Simulation& sim, uint8_t player, int64_t param);
public:
    // pointPair player abilities: kCmdPlayerAbility param = index | packPoint2(second point)
    static int64_t packPoint2(Vec3 p);
    bool pairTarget(Simulation& sim, uint8_t player, int payload, Vec3& from, Vec3& to);   // AI: own group -> enemy base
    Fixed spotScoreFidelity(Simulation& sim, uint8_t player, uint16_t type, int x0, int y0, Vec2 d, Vec2 away);   // findAutoSpot: slabs, pads, traps (>= 1<<19: skip)
    int bareCells(Simulation& sim, uint8_t player, int x0, int y0, int w, int h) const;
};

} // namespace rts::game
