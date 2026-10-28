// Generic computer player for the data-driven RTS module. Like a human it only reads the state
// and submits Commands, so it never desyncs and replays do not re-run it.
// It adapts to the data: economy-and-army play when the game has workers/producers,
// garrison "send" play (Mushroom Wars) when the player owns growing garrisons.
#pragma once
#include <cstdint>
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
};

class RtsAI {
public:
    RtsAI(uint8_t player, uint64_t seed, AIConfig cfg = {}) : player_(player), rng_(seed, player), cfg_(cfg) {}
    void update(Simulation& sim);
    uint8_t player() const { return player_; }

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
    bool build(Simulation& sim, RtsModule& m, uint16_t type, int depth = 0);
    bool train(Simulation& sim, RtsModule& m, uint16_t type); // from any idle producer
    void send(Simulation& sim, Command c);

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
    uint32_t lastMechTick_ = 0;   // few workers left and one can be trained: don't spend on the army
};

} // namespace rts::game
