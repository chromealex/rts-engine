// Abilities, effects, statuses and their delivery: the data side of RtsEffects.cpp / RtsSkills.cpp.
// Included from RtsData.h (needs Amount, StatMod, Stat, kNoType).
//
// Everything a skill does is a *payload*: a list of effects in Rules::payloads. Effects may carry
// their own payload (a projectile delivers one on impact, an area applies one on its trigger, a
// status ticks one, a weapon applies one on hit), so complex skills are compositions of primitives.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../engine/core/Fixed.h"
#include "../../engine/data/Json.h"

namespace rts::game {

enum class TargetKind : uint8_t {
    None,       // no target (around the caster / whole map for player skills)
    Self,
    Unit,       // a unit or structure
    Point,
    Structure,  // a structure (garrison building)
    Group,      // a group of units in transit (sent together)
    Player,     // a player: effects use the filter's enemies/allies/self over players
    PointPair,  // two points (player abilities): from where to where (Chronosphere)
};

// Status flags: a status may carry several. Unit, structure or player statuses.
enum StatusFlag : uint32_t {
    kStStun = 1u << 0,         // can't move, attack or cast
    kStSilence = 1u << 1,      // can't cast (players: no player abilities)
    kStDisarm = 1u << 2,       // weapons don't fire (towers disabled)
    kStRoot = 1u << 3,         // can't move
    kStInvulnerable = 1u << 4, // no damage; garrisons can't be assaulted
    kStInvisible = 1u << 5,    // enemies can't target it; counts hidden from enemies (view)
    kStLock = 1u << 6,         // structure: no sending, no production
    kStBlind = 1u << 7,        // weapons can't acquire targets
    kStHidden = 1u << 8,       // garrison count hidden from enemies (view)
    kStFlying = 1u << 9,       // leaves its lane / terrain and moves straight to the goal
    kStFake = 1u << 10,        // illusion: does nothing on arrival, dies on any hit
    kStIgnoreDefense = 1u << 11, // assaults ignore the garrison's defense
    kStSpreads = 1u << 12,     // units sent from a structure carry the status and give it to the one they enter
    kStFreezeRegen = 1u << 13, // player: resource regeneration stops
    kStAlert = 1u << 14,       // player: alerts about groups sent at own structures
    kStRevealed = 1u << 15,    // visible to everyone (cancels invisibility)
};

enum class Stacking : uint8_t { Refresh, Stack, Independent };

struct StatusDef {
    std::string id, name;
    uint64_t tags = 0;                  // dispel by tag ("buff", "debuff", "magic", any custom tag)
    uint32_t flags = 0;
    std::vector<StatMod> mods;          // per stack (stats of units / structures; players: their objects)
    Fixed tickAmount{};                 // per tick and stack: > 0 damage, < 0 heal (DoT / HoT)
    uint16_t tickDamageType = 0;
    Fixed tickInterval = Fixed::fromInt(1);
    int16_t tickPayload = -1;           // applied around the holder every tick (kill aura, plague)
    Fixed shield{};                     // absorbs damage (units) or assaulting units (garrisons)
    Stacking stacking = Stacking::Refresh;
    int maxStacks = 1;
    int16_t onExpire = -1;              // payload when it runs out (two-stage skills)
    int16_t onCaptured = -1;            // payload when the structure gets captured while active (revenge)
    int16_t onHit = -1;                 // the holder's weapon hits apply this payload
    std::vector<Amount> incomeMult;     // player: multiplier per resource on income / regen
    Fixed garrisonPerSecond{};          // structure: +/- garrison units per second (plague, reinforcements)
    Json view;
};

enum class Shape : uint8_t { Target, Circle, Line, Cone, Ring, Map, Group };
enum class SortBy : uint8_t { None, Nearest, Farthest, LowestHp, HighestHp, LevelLow, LevelHigh, GarrisonLow, GarrisonHigh, Random };

// Who an effect touches.
struct TargetFilter {
    Shape shape = Shape::Target;
    Fixed width = Fixed::fromInt(1);    // line width
    Fixed angle = Fixed::fromRatio(1, 2); // cone: cos of the half angle
    Fixed ring{};                       // ring width (Ring: radius - ring .. radius)
    bool enemies = true, allies = false, self = false, neutral = false;
    uint8_t layers = 0xFF;              // ground / air / naval / sand
    bool units = true, structures = true;
    bool garrisons = false;             // only structures with a garrison
    bool inTransit = false;             // only units walking to a structure (lanes, sends)
    uint64_t tags = 0;
    uint16_t type = 0xFFFF;
    int minLevel = 0, maxLevel = 1 << 20;
    int maxTargets = 0;                 // 0: all
    SortBy sort = SortBy::None;
    bool ownFirst = false;
    std::string area;                   // map area name (triggers' "areas")
    bool explicitShape = false;
};

enum class AreaTrigger : uint8_t { Once, Periodic, Contact };
enum class AreaMotion : uint8_t { Static, Toward, Lane, Wander, Follow };

// How an effect's payload travels (projectiles, areas, chains).
struct DeliveryDef {
    Fixed speed = Fixed::fromInt(12);   // projectile / moving area (cells per second)
    bool homing = false, arc = false, collide = false;
    Fixed arcHeight = Fixed::fromInt(2);
    Fixed scatter{};                    // random offset of the impact point (blizzard)
    Fixed falloff{};                    // splash: damage at the edge = 1 - falloff
    // areas
    Fixed delay{};                      // first trigger after this many seconds (artillery marker)
    Fixed lifetime{};                   // area life (0: until its first trigger)
    Fixed interval = Fixed::fromInt(1); // periodic trigger interval
    AreaTrigger trigger = AreaTrigger::Once;
    AreaMotion motion = AreaMotion::Static;
    int maxTriggers = 0;                // contact areas: uses (mines)
    Fixed sight{};                      // reveals this radius to the owner
    Fixed blocksVision{};               // smoke: a sight blocker of this height over the area (fogOfWar.lineOfSight)
    bool teleport = false;              // portal: units entering are moved to the second point
    Vec3 teleportOffset{};
    uint16_t deathSpawn = 0xFFFF;       // units dying inside spawn this type for the caster (souls)
    // chains
    int jumps = 0;
    Fixed jumpRadius = Fixed::fromInt(4);
    Fixed jumpFalloff = Fixed::fromInt(1); // amount multiplier per jump
};

enum class EffectKind : uint8_t {
    // original kinds (unchanged)
    Damage, Heal, Stun, Buff, Spawn, Blink,
    // statuses and combat
    Status, Dispel, Knockback, Teleport, Resource, EnergyBurn, Reveal, Kill,
    // delivery
    Area, Projectile, Chain, Cast,
    // garrisons / structures
    GarrisonKill, GarrisonAdd, GarrisonConvert, GarrisonPanic, Capture, Swap, Transform, Level, Equalize, Link, Redirect,
    // groups in transit
    SendBack, Clone, Formation, Convert,
    // players
    PlayerStatus, EqualizeResource, Sacrifice, StealSkill, Cooldown,
    // RtsFidelity.cpp
    AddCharges,     // structure with larva charges: + amount, up to `limit` (inject larvae)
    TeleportGroup,  // units around one end moved to the other, keeping formation; may come back after `duration`
    Script,     // {"type": "script", "fn": "name", ...}: a Lua function (RtsScript.h), runs in the tick's scripts phase
    Field,      // {"type": "field", "field": id, "add" | "set" | "max": n, "radius": r}: grid field cells (RtsGridFields.h)
};

struct EffectDef {
    EffectKind kind = EffectKind::Damage;
    Fixed amount{};            // damage / heal / units / resource / distance
    Fixed amountMax{};         // random value in [amount, amountMax] (sim RNG) when `randomAmount`
    bool randomAmount = false;
    Fixed percent{};           // share (0..1) for garrison / group / resource effects
    Fixed radius{};            // 0 = single target
    Fixed duration{};          // stun / buff / summon lifetime / status / temporary capture
    Fixed chance = Fixed::fromInt(1);
    uint16_t damageType = 0;
    StatMod mod;               // buff
    uint16_t spawnType = 0xFFFF;
    int count = 1;
    int limit = 0;             // summons: at most this many alive per caster type
    bool enemies = true;       // area filters (legacy; mirrored in filter)
    bool allies = false;
    TargetFilter filter;
    int16_t status = -1;       // Status / PlayerStatus: status definition
    int16_t payload = -1;      // Area / Projectile / Chain / Cast: what is delivered
    DeliveryDef delivery;
    int16_t resource = -1;     // Resource / EnergyBurn / Sacrifice
    bool transfer = false;     // Resource drain goes to the caster's owner (steal)
    std::vector<Amount> gain;  // Sacrifice: per unit
    uint16_t toType = 0xFFFF;  // Transform target type
    Fixed keepChance{};        // Transform / Capture: chance the change stays after the duration
    int levelDelta = 0;        // Level: +n upgrades / -n downgrades
    uint64_t dispelTags = 0;   // Dispel: statuses with any of these tags (0: all)
    uint8_t dispelFrom = 0;    // Dispel: 0 any source, 1 hostile sources (cleanse), 2 friendly sources (purge)
    int mode = 0;              // SendBack: 0 home, 1 reverse, 2 throw back `amount` cells; Teleport: 0 targets, 1 caster
    bool fake = false;         // Clone: illusions
    bool ofLost = false;       // GarrisonAdd: percent of units lost (resurrection)
    bool fromCaster = false;   // Knockback / Area origin: from the caster instead of the point
    uint64_t killTags = 0;     // TeleportGroup: units with these tags die instead (Chronosphere infantry)
    uint8_t tpFrom = 0, tpTo = 0; // TeleportGroup "origin" / "destination": 0 point, 1 caster, 2 second point (pointPair)
    std::string scriptFn;      // Script: global function of the simulation scripts
    Json scriptArgs;           // Script: the effect's JSON (ctx.args)
    // Field: grid field cells around the point (radius 0: one cell); per-player fields write the owner's layer
    std::string fieldName;
    int16_t field = -1;        // index into Rules::gridFields (resolved by loadGridFieldRules)
    uint8_t fieldOp = 0;       // 0 add, 1 set, 2 max
    Fixed fieldAmount{};
    int32_t fieldValue = 0;    // fieldAmount in the field's raw units
};

// What autocast / the AI aims a skill at.
enum class SmartRule : uint8_t { Auto, LowestHpAlly, DensestEnemy, StrongestEnemyStructure, WeakestOwnStructure,
                                 LargestEnemyGroup, OwnStructureUnderAttack, Self, EnemyPlayer, DebuffedOwn,
                                 OwnProducer };   // own structure with larva charges below the cap, without the payload's status

struct AbilityDef {
    std::string id, name;
    TargetKind target = TargetKind::None;
    Fixed range = Fixed::fromInt(6);
    Fixed cooldown = Fixed::fromInt(10);
    Fixed initialCooldown{};
    Fixed energyCost{};
    std::vector<Amount> cost;  // resources (unit abilities may cost resources too)
    int requiredLevel = 1;     // heroes
    std::vector<int> requires_; // packed Requirement (tech bit 15 | index), granted by techs / structures
    bool autocast = true;      // used automatically (AI and idle units)
    std::vector<EffectDef> effects;
    int16_t payload = -1;      // effects (wrapped by the delivery) in Rules::payloads
    // passive aura (applied continuously around the owner)
    bool aura = false;
    Fixed auraRadius{};
    // casting
    Fixed castTime{};          // windup before the effect
    Fixed backswing{};         // the caster stays busy after the effect
    bool channel = false;      // payload repeats every channelInterval while channelling
    Fixed channelTime{}, channelInterval = Fixed::fromRatio(1, 2);
    int chargesMax = 0;        // > 0: charges regenerate one per `cooldown`
    SmartRule smart = SmartRule::Auto;
    std::vector<AbilityDef> levels; // level 2.. (hero level / skill level); each a full copy
};

// Player-level abilities (not tied to a unit): nukes, air strikes, Bunker Wars style skills.
struct PlayerAbilityDef {
    std::string id, name;
    std::vector<Amount> cost;
    Fixed cooldown{};
    Fixed initialCooldown{};
    int chargesMax = 0;                        // 0: unlimited (cooldown only)
    std::vector<Amount> rearmCost;
    Fixed rearmTime = Fixed::fromInt(30);
    TargetKind target = TargetKind::Point;
    std::vector<EffectDef> effects;
    int16_t payload = -1;
    Fixed castTime{};                          // activation delay
    std::vector<int> requires_;                // packed Requirement
    bool passive = false;                      // always on: `status` applied to the player
    int16_t status = -1;
    Fixed autoEvery{};                         // > 0: cast automatically every N seconds (map / timer skills)
    SmartRule smart = SmartRule::Auto;
    std::vector<PlayerAbilityDef> levels;      // level 2..
};

} // namespace rts::game
