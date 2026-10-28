// Fixed-timestep deterministic simulation.
//
//   Simulation = World (ECS) + RNG + tick counter + ordered systems + command buffer.
//   Gameplay lives in a GameModule (graph-war, free-move RTS, ...), the core knows nothing
//   about buildings or units.
//
// Presentation (renderer, audio, UI) never writes into the Simulation. It reads state,
// interpolates between ticks with alpha(), and consumes events().
#pragma once
#include "../core/Archive.h"
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
    // Save states: write / read the module's dynamic state (World pools and resources). The reading side runs
    // on a simulation set up from the same rules and map. Default: not supported.
    virtual bool serializeState(Simulation&, Archive&) { return false; }
};

// Everything that changes while the simulation runs (rollback netcode saves one per tick).
// GameModule implementations keep all mutable state in the World; module members are read-only
// after setup(), so they are not part of the snapshot.
struct SimSnapshot {
    World world;
    Random rng;
    uint32_t tick = 0;
    std::map<uint32_t, std::vector<Command>> pending;
    size_t executedCount = 0;
    std::vector<PlayerInfo> players;
    std::map<uint8_t, uint32_t> seqByPlayer;
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

    // Network play: when a sink is set, submit() hands the command to it (stamped with the
    // player's next seq) instead of queuing it; the sink returns the tick it will execute on.
    // The net session feeds confirmed/predicted commands back through inject().
    using CommandSink = std::function<uint32_t(Command&&)>;
    void setCommandSink(CommandSink sink) { sink_ = std::move(sink); }
    bool hasCommandSink() const { return bool(sink_); }
    // Queue a command exactly as given (tick and seq kept), bypassing the sink.
    void inject(Command cmd);

    // Advance exactly one fixed tick.
    void step();
    // Advance by real time; returns number of ticks executed. Use alpha() for render interpolation.
    int advance(double realSeconds, int maxTicks = 8);
    double alpha() const { return accumulator_ * tickRate_; }

    uint64_t checksum();

    // Rollback support. loadState() also drops executed commands recorded after the snapshot.
    void saveState(SimSnapshot& out) const;
    void loadState(const SimSnapshot& in);
    // Save states in bytes: identical on every platform, versioned, checked by the state checksum on load.
    // Load into a simulation set up from the same rules + map (and seed); false (state untouched on a bad header,
    // possibly partial otherwise) with a reason in err.
    std::string serialize();
    bool deserialize(const std::string& bytes, std::string* err = nullptr);

    void addSystem(std::string name, SystemFn fn) { systems_.push_back({std::move(name), std::move(fn)}); }
    // Profiling (tools only, never affects the simulation): with a clock set, step() adds each system's
    // wall time (clock units, e.g. ns) to systemTime(i).
    void setProfileClock(int64_t (*clock)()) { profClock_ = clock; profNs_.assign(systems_.size(), 0); }
    size_t systemCount() const { return systems_.size(); }
    const std::string& systemName(size_t i) const { return systems_[i].name; }
    int64_t systemTime(size_t i) const { return i < profNs_.size() ? profNs_[i] : 0; }
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
    int64_t (*profClock_)() = nullptr;
    std::vector<int64_t> profNs_;
    std::map<uint32_t, std::vector<Command>> pending_;
    std::vector<Command> executed_;
    std::vector<SimEvent> events_;
    std::vector<PlayerInfo> players_;
    std::map<uint8_t, uint32_t> seqByPlayer_;
    CommandSink sink_;
};

} // namespace rts
