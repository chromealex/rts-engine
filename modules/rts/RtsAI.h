// Generic computer player for the data-driven RTS module. Like a human it only reads the state
// and submits Commands, so it never desyncs and replays do not re-run it.
// It adapts to the data: economy-and-army play when the game has workers/producers,
// garrison "send" play (Mushroom Wars) when the player owns growing garrisons.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../engine/core/Random.h"
#include "RtsModule.h"

namespace rts::game {

struct AIConfig {
    uint32_t thinkTicks = 10;     // reaction time
    int workersPerBase = 14;
    int firstAttackArmy = 8;
    int armyGrowth = 4;           // each wave needs this many more units
    int maxAttackArmy = 16;       // ...up to this many (supply caps would otherwise stall the attacks)
    Fixed aggression = Fixed::fromInt(1);
    // agent-ai-terrain knobs (rules "ai" block overrides the fields it names, see docs/game-format.md)
    bool terrain = true;          // terrain analysis: high-ground rally / siege spots, routes, watch posts, chokepoint towers
    int climbWeight = 24;         // route cost per level climbed (x10 = one cell)
    int exposureWeight = 12;      // route cost per level of higher ground overlooking a cell
    int towers = 1;               // defensive structures at the chokepoint toward the enemy once raided (0 = none)
    int scouts = 1;               // units holding watch posts on high ground
    uint32_t escalateSeconds = 300; // no enemy structure destroyed for this long: escalate (bigger waves, siege, economy targets)
    int invasionSize = 6;         // planets: orbit units gathered before an invasion of another body
};
struct AITerrain;

class RtsAI {
public:
    RtsAI(uint8_t player, uint64_t seed, AIConfig cfg = {}) : player_(player), rng_(seed, player), cfg_(cfg) {}
    void update(Simulation& sim);
    uint8_t player() const { return player_; }
    // Bot state for save states (next to Simulation::serialize): a bot restored from it plays on exactly like
    // the one that was saved; the terrain analysis is rebuilt from the map on the first think.
    std::string saveState() const;
    bool loadState(const std::string& bytes);

private:
    void economy(Simulation& sim, RtsModule& m);
    void military(Simulation& sim, RtsModule& m);
    void garrisonPlay(Simulation& sim, RtsModule& m);
    void special(Simulation& sim, RtsModule& m);   // deploy, sidebar placement, claims, auto-build, upgrades, research
    void dungeon(Simulation& sim, RtsModule& m);   // mark digs toward objectives, paint rooms
    void settlers(Simulation& sim, RtsModule& m);  // roads to buildings, production chains
    void mech(Simulation& sim, RtsModule& m);      // agent-mech (RtsMechAI.cpp): retreat the wounded, man bunkers, ...
    void mechEconomy(Simulation& sim, RtsModule& m);   // market, trade carts, hero items, merges
    void originals(Simulation& sim, RtsModule& m); // RtsAIOriginals.cpp: hero skills, crews, garrison upgrades
    void fidelity(Simulation& sim, RtsModule& m);  // RtsAIFidelity.cpp: concrete slabs, traps
    // agent-ai-terrain (RtsAITactics.cpp): terrain-aware army control, build spots, stuck builders, planets
    void tactics(Simulation& sim, RtsModule& m, std::vector<Entity>& idleArmy, std::vector<Entity>& army, bool defend);
    void applyRulesConfig(RtsModule& m);
    void ensureTerrain(Simulation& sim, RtsModule& m);
    bool findSpot(Simulation& sim, RtsModule& m, uint16_t type, Vec3 near, int radius, Vec3& out, bool defensive);
    void watchBuilders(Simulation& sim, RtsModule& m);
    bool buildTowers(Simulation& sim, RtsModule& m);
    int pickUnit(Simulation& sim, RtsModule& m, const std::vector<uint16_t>& opts);
    bool planetOps(Simulation& sim, RtsModule& m, std::vector<Entity>& idleArmy);
    void refreshBases(Simulation& sim, RtsModule& m);
    bool expand(Simulation& sim, RtsModule& m, const std::vector<int>& count);   // a dropoff at the next resource when the near ones are gone
    bool build(Simulation& sim, RtsModule& m, uint16_t type, int depth = 0, const Vec3* at = nullptr);
    bool train(Simulation& sim, RtsModule& m, uint16_t type); // from any idle producer
    void send(Simulation& sim, Command c);
    void stateIo(class rts::Archive& a);

    uint8_t player_;
    Random rng_;
    AIConfig cfg_;
    uint32_t next_ = 0;
    int wave_ = 0;
    uint32_t lastBuildTick_ = 0;
    uint32_t lastTechTick_ = 0;
    bool saveForWorkers_ = false;
    uint32_t lastDungeonTick_ = 0;
    uint32_t lastSkillTick_ = 0;
    int savingFor_ = -1;          // player skill the bot saves resources for
    uint32_t savingUntil_ = 0;
    uint32_t lastSettlersTick_ = 0;
    uint32_t lastMechTick_ = 0;
    bool mechTurnAI(Simulation& sim, RtsModule& m);
    uint32_t turnActed_ = 0;   // turn-based mode: false when it is not our turn; ends our turns   // few workers left and one can be trained: don't spend on the army
    // ---- agent-ai-terrain
    bool configured_ = false;
    int retreatPct_ = 35, holdSec_ = 30, crowdFree_ = 5, crowdCost_ = 6;
    uint32_t rallyTick_ = 0, lastSiteTick_ = 0;
    uint32_t allInTick_ = 0, spotRetryTick_ = 0, expandTick_ = 0;
    bool raided_ = false;            // enemies came to our base at least once
    std::shared_ptr<const AITerrain> terr_;   // immutable once built (copies of the bot share it)
    Vec3 home_{}, enemyHome_{};
    bool hasHome_ = false, hasEnemy_ = false;
    int homeCell_ = -1, enemyCell_ = -1;
    std::vector<int> mainRoute_, altRoute_;   // terrain-aware routes home -> enemy (cells)
    uint32_t routeTick_ = 0;
    int routeEnemyCell_ = -1;
    Vec3 rally_{};
    bool hasRally_ = false;
    int chokeCell_ = -1;                      // narrowest route cell on our side
    int watchCell_ = -1;                      // high-ground post for a scout
    std::vector<Entity> wave_units_, scouts_;
    std::vector<Vec3> waypoints_;
    size_t wp_ = 0;
    uint32_t wpTick_ = 0, waveTick_ = 0, holdUntil_ = 0;
    int waveStart_ = 0, waveEnemyStructs_ = 0;
    Entity waveTarget_;
    int escalation_ = 0, failedWaves_ = 0;
    uint32_t lastKillTick_ = 0, lastEscalateTick_ = 0, scoutTick_ = 0;
    int enemyStructsSeen_ = -1;
    struct StuckBuild { uint64_t entity = 0; uint32_t since = 0; Vec3 point{}; Fixed progress{}; };
    std::vector<StuckBuild> buildWatch_;
    std::vector<std::pair<int, uint32_t>> badSpots_;   // (cell, until tick)
    // planets
    std::vector<Entity> invaders_;
    int invadeBody_ = -1, invasions_ = 0;
    uint32_t invadeTick_ = 0;
    bool enemyOffBody_ = false;              // no enemy structure shares our body: invasion is the only way
};

} // namespace rts::game
