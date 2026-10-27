// Fixed-timestep deterministic simulation.
//
//   Simulation = World (ECS) + RNG + tick counter + ordered systems + command buffer.
//   Gameplay lives in a GameModule (graph-war, free-move RTS, ...), the core knows nothing
//   about buildings or units.
//
// Presentation (renderer, audio, UI) never writes into the Simulation. It reads state,
// interpolates between ticks with alpha(), and consumes events().
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../core/ECS.h"
#include "../core/Fixed.h"
#include "../core/Hash.h"
#include "../core/Random.h"
#include "../data/Json.h"
#include "Command.h"

namespace rts {

class Simulation;

// Something that happened during a tick, for the presentation layer (sounds, VFX, UI).
struct SimEvent {
    uint16_t type = 0;      // module-defined
    uint8_t player = 0;
    Entity a = kNullEntity, b = kNullEntity;
    int64_t value = 0;
    Vec3 pos{};
};

struct PlayerInfo {
    uint8_t slot = 0;       // 1..N
    std::string name;
    uint8_t team = 0;       // players on the same team are allies
    bool alive = true;
};

class GameModule {
public:
    virtual ~GameModule() = default;
    virtual std::string name() const = 0;
    // Build the initial state from rules + map; register systems.
    virtual void setup(Simulation& sim, const Json& rules, const Json& map) = 0;
    // Validate and apply one command. Invalid commands must be ignored (never assert): they may
    // come from a cheating or lagging client.
    virtual void applyCommand(Simulation& sim, const Command& cmd) = 0;
    // Feed every piece of gameplay state into the hasher in a fixed order.
    virtual void hashState(Simulation& sim, Hasher& h) = 0;
    // 0 while the game is running; winning team id (>0) when finished; -1 for a draw.
    virtual int result(Simulation& sim) = 0;
};

struct SimConfig {
    uint64_t seed = 1;
    uint32_t tickRate = 20;        // ticks per second
    uint32_t inputDelayTicks = 2;  // commands issued at tick T execute at T + delay (lockstep latency hiding)
};

class Simulation {
public:
    using SystemFn = std::function<void(Simulation&)>;

    Simulation(SimConfig cfg, std::unique_ptr<GameModule> module);

    void setup(const Json& rules, const Json& map);

    // Queue a command. If cmd.tick == 0 it is scheduled for currentTick + inputDelay.
    // Returns the tick it was scheduled for.
    uint32_t submit(Command cmd);

    // Advance exactly one fixed tick.
    void step();
    // Advance by real time; returns number of ticks executed. Use alpha() for render interpolation.
    int advance(double realSeconds, int maxTicks = 8);
    double alpha() const { return accumulator_ * tickRate_; }

    uint64_t checksum();

    void addSystem(std::string name, SystemFn fn) { systems_.push_back({std::move(name), std::move(fn)}); }
    void emit(const SimEvent& e) { events_.push_back(e); }

    World& world() { return world_; }
    Random& rng() { return rng_; }
    GameModule& module() { return *module_; }
    uint32_t tick() const { return tick_; }
    Fixed dt() const { return dt_; }
    uint32_t tickRate() const { return tickRate_; }
    const SimConfig& config() const { return cfg_; }
    std::vector<PlayerInfo>& players() { return players_; }
    PlayerInfo* player(uint8_t slot);
    bool allied(uint8_t a, uint8_t b);
    const std::vector<SimEvent>& events() const { return events_; }       // events of the last tick
    const std::vector<Command>& executedCommands() const { return executed_; } // all commands so far
    int result() { return module_->result(*this); }

private:
    struct System { std::string name; SystemFn fn; };

    SimConfig cfg_;
    std::unique_ptr<GameModule> module_;
    World world_;
    Random rng_;
    uint32_t tick_ = 0;
    uint32_t tickRate_ = 20;
    Fixed dt_;
    double accumulator_ = 0.0; // real-time only, never affects the simulation
    std::vector<System> systems_;
    std::map<uint32_t, std::vector<Command>> pending_;
    std::vector<Command> executed_;
    std::vector<SimEvent> events_;
    std::vector<PlayerInfo> players_;
    std::map<uint8_t, uint32_t> seqByPlayer_;
};

} // namespace rts
