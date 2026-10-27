// Generic, data-driven RTS gameplay module. One implementation for every game built on the engine.
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
    kCmdStop,         // units
    kCmdHold,         // units
    kCmdPatrol,       // units, point
    kCmdGather,       // units, target resource node (or dropoff to return cargo)
    kCmdBuild,        // units (builders), param = type, point = footprint centre
    kCmdTrain,        // units (producers), param = type
    kCmdCancelTrain,  // units (producers), param = queue slot (-1 = last)
    kCmdRally,        // units (producers), point or target
    kCmdSmart,        // right click: resolved by target (attack / gather / return / move / enter)
    kCmdSend,         // garrison structures, target structure, param = percent (Mushroom Wars)
};

// ---------------------------------------------------------------- events (presentation)
enum GameEvent : uint16_t {
    kEvSpawned = 1,      // a = entity, value = type
    kEvDied,             // a = entity, b = killer, value = type
    kEvAttack,           // a = attacker, b = target, value = weapon index
    kEvHit,              // a = target, value = damage (fixed raw)
    kEvTrainStarted,     // a = producer, value = type
    kEvTrainDone,        // a = new unit, value = type
    kEvBuildPlaced,      // a = structure, value = type
    kEvBuildDone,        // a = structure
    kEvGathered,         // a = worker, value = resource
    kEvCaptured,         // a = structure, player = new owner, value = old owner
    kEvPlayerDefeated,   // player
    kEvNotEnough,        // player, value = resource index (or 255 = supply)
};

// ---------------------------------------------------------------- components
struct Unit {
    uint16_t type = 0;
    Fixed hp{};
    uint8_t flags = 0;          // kUnitHold
    Vec3 facing{Fixed::fromInt(1), Fixed{}, Fixed{}};
};
enum UnitFlags : uint8_t { kUnitHold = 1, kUnitUnderConstruction = 2 };

enum class OrderType : uint8_t { Move, AttackMove, Attack, Patrol, Gather, Return, Build, Enter };

struct Order {
    OrderType type = OrderType::Move;
    Entity target = kNullEntity;
    Vec3 point{};
    Vec3 point2{};              // patrol origin
    int32_t param = 0;          // build type
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
    std::vector<uint16_t> roadPath;  // remaining road nodes (road maps)
};

struct Orders { std::vector<Order> queue; };

struct Combat {
    Fixed cooldown[4] = {};
    Entity target = kNullEntity;
    uint8_t acquireTimer = 0;
    bool autoTarget = false;    // target chosen automatically (may be replaced)
};

struct Cargo { int16_t res = -1; Fixed amount{}; Entity lastNode = kNullEntity; Vec3 lastNodePos{}; };

struct Production {
    std::vector<uint16_t> queue;
    Fixed progress{};
    Vec3 rally{};
    bool hasRally = false;
    Entity rallyTarget = kNullEntity;
};

struct Construction { Fixed progress{}; Entity builder = kNullEntity; };
struct ResourceStock { Fixed amount{}; };
struct Footprint { int x = 0, y = 0, w = 0, h = 0; };

struct Garrison {
    int count = 0;
    Fixed growth{};
    Fixed damageAcc{};
    // units waiting to leave the building (Mushroom Wars send)
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

// ---------------------------------------------------------------- per-player state (resource)
struct PlayerEconomy {
    std::vector<Fixed> stock;     // per resource
    std::vector<Fixed> cap;       // Flow storage caps (0 = unlimited)
    std::vector<Fixed> income;    // last computed net income per second (UI)
    Fixed supplyUsed{}, supplyCap{};
    std::vector<int> typeCount;   // completed objects per type (tech requirements)
};

struct GameState {
    std::vector<PlayerEconomy> players; // index = slot (0 = neutral)
    MapGrid grid;
    RoadGraph roads;
    SpatialHash hash;
    bool roadsOnly = false;
    // fog of war: per player, per cell: 0 unexplored, 1 explored, 2 visible
    std::vector<std::vector<uint8_t>> vision;
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

    // Helpers shared with AI, client and editor.
    Entity spawn(Simulation& sim, uint16_t type, uint8_t owner, Vec3 pos, bool underConstruction = false);
    bool canPlace(Simulation& sim, uint16_t type, Vec3 centre, Entity ignore = kNullEntity) const;
    static void footprintAt(const UnitType& t, Vec3 centre, int& x0, int& y0);
    bool requirementsMet(Simulation& sim, uint8_t player, uint16_t type) const;
    bool canAfford(Simulation& sim, uint8_t player, uint16_t type, int* missingRes = nullptr) const;
    bool isVisible(Simulation& sim, uint8_t player, Vec3 p) const;
    bool hostile(Simulation& sim, uint8_t a, uint8_t b) const;

private:
    // systems
    void sysEconomy(Simulation& sim);
    void sysProduction(Simulation& sim);
    void sysGarrison(Simulation& sim);
    void sysOrders(Simulation& sim);
    void sysTargeting(Simulation& sim);
    void sysMovement(Simulation& sim);
    void sysWeapons(Simulation& sim);
    void sysProjectiles(Simulation& sim);
    void sysDeath(Simulation& sim);
    void sysVision(Simulation& sim);
    void sysVictory(Simulation& sim);

    // behaviour helpers
    void issue(Simulation& sim, Entity e, const Order& o, bool queue);
    void moveTo(Simulation& sim, Entity e, Vec3 goal, Fixed radius);
    void stopMoving(Simulation& sim, Entity e);
    bool runOrder(Simulation& sim, Entity e, Order& o); // returns true when finished
    void dealDamage(Simulation& sim, Entity target, Fixed dmg, uint16_t dmgType, Entity source);
    Entity findNearestDropoff(Simulation& sim, uint8_t owner, Vec3 p, int res);
    Entity findNearestResource(Simulation& sim, Vec3 p, int res, Fixed maxDist);
    bool pay(Simulation& sim, uint8_t player, uint16_t type);
    void refund(Simulation& sim, uint8_t player, uint16_t type);
    Vec3 freeSpotNear(Simulation& sim, Entity structure, uint8_t layer);
    Fixed contactDistance(Simulation& sim, Entity a, Entity b);

    Rules rules_;
};

} // namespace rts::game
