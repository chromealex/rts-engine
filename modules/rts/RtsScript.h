// Lua scripting (Lua 5.4, third_party/lua): game mechanics written as scripts in the game folder,
// changed without rebuilding the engine. Reference for script authors: docs/scripting.md.
//
// Two sandboxes:
//  * Simulation scripts (rules/maps "scripts": ["scripts/x.lua"]) run inside the lockstep tick and
//    must replay exactly on every platform. Numbers: Lua integers plus `fx` values (Q47.16 Fixed as
//    userdata with arithmetic metamethods). Floats are kept out of the simulation at three levels:
//      1. float literals (1.5, 1e3, 0x1p4) are rejected when a file is loaded, with file:line;
//      2. every float arithmetic primitive of the VM (/, ^, float + - * % //, constant folding)
//         raises "float arithmetic is not allowed" (guard macros in third_party/lua/rts_luaconf.h);
//      3. every API call that takes a number accepts only integers or fx ("use fx() or integers").
//    `7 // 2` is integer division; fractional math is `fx(7) / 2`, `fx("1.25")`, `fx(3, 4)`.
//    The library is reduced to deterministic parts: no os/io/debug/package/load/dofile/collectgarbage/
//    next; math keeps integer functions only and math.random uses the simulation RNG; pairs() iterates
//    keys in sorted order (integers, then strings) so iteration never depends on hashing or history;
//    tostring() never shows addresses; string hashing and table.sort pivots are seeded with constants.
//    An instruction budget per tick (count hook) aborts runaway code; any error is logged with
//    file:line and disables that handler (deterministically: the flag is simulation state).
//  * View scripts (rules "viewScripts") are presentation only: normal Lua numbers, read-only queries,
//    notices / effects / sounds as requests drained by the host (modules/rts/view/RtsViewScript.h).
//
// Persistent script state: the global table `state` (and the trigger variables). It is serialized
// canonically (sorted keys; integers, fx, strings, booleans, nested tables) into the ScriptStore world
// resource after every tick, so it is hashed (RtsModule::hashState), copied by rollback snapshots and
// restored into the VM when a rollback rewinds the world. Globals are read-only after loading and
// file-level locals must not be mutated by handlers (that state would escape snapshots) - see docs.
// Replays re-run setup, so the VM is re-created identically from the same sources.
#pragma once
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>
#include "../../engine/core/ECS.h"
#include "../../engine/core/Fixed.h"
#include "../../engine/data/Json.h"

namespace rts { class Simulation; class Hasher; }

namespace rts::game {

class RtsModule;
struct EffectDef;
struct EffectCtx;

using ScriptReader = std::function<bool(const std::string& relativePath, std::string& out)>;

// Script state of a match (a World resource, present only in games with scripts).
struct ScriptStore {
    std::string state;                          // canonical serialization of the `state` table
    uint64_t serial = 0;                        // write stamp: the VM restores `state` when it differs
    bool started = false;                       // "start" handlers ran
    std::vector<uint8_t> disabled;              // per handler: switched off after an error
    std::vector<std::string> disabledFns;       // functions called from data that failed (sorted)
    std::vector<std::vector<uint64_t>> inside;  // per area handler: entities inside (sorted)
    struct Timer { uint32_t tick = 0; uint32_t seq = 0; std::string fn, arg; }; // game.after(); arg serialized
    std::vector<Timer> timers;                  // sorted by (tick, seq)
    uint32_t timerSeq = 0;
    bool userScripts = false;                   // the game has its own simulation scripts: hashed only then (the
                                                // standard library keeps its state in the C++ GameState)
    void hash(Hasher& h) const;
};

struct ScriptLogEntry {
    bool error = false;
    uint32_t tick = 0;
    std::string text;                           // "scripts/x.lua:12: message" for errors
};

// Script files of a game: rules "scripts" + map "scripts" (view: rules "viewScripts" + map "viewScripts").
std::vector<std::string> scriptFiles(const Json& rules, const Json& map, bool view = false);
// Reads every script file (and the files they require()) with `read` into rules["$scriptSources"]
// ({path: text}), so the simulation can load them without file access (headless, server, tests).
// Without embedded sources the module has no scripts to run, so every loader that starts a
// simulation calls this next to applyTilesToMap. Returns false with `err` when a listed file is missing
// (the others are embedded anyway).
bool embedScripts(Json& rules, const Json& map, const ScriptReader& read, std::string* err);
// Compile without running: "" when fine, else "name:line: message". sim = simulation rules (float literals).
std::string checkScriptSyntax(const std::string& name, const std::string& text, bool sim = true);
// The API reference for agents and the editor (functions, events, rules) as JSON.
Json scriptApiReference();

// Systems implemented by the engine's standard Lua library (modules/rts/lua/<name>.lua defines
// std.<name>(tick)); a game overrides one with scripts/std/<name>.lua.
enum class StdFn : uint8_t { Victory, Waves, Triggers, AutoBuild, Sell, Count };

// The simulation VM of one match (owned by RtsModule). It always runs the standard library;
// game scripts (rules/map "scripts") are loaded on top.
class ScriptEngine {
public:
    explicit ScriptEngine(RtsModule& module);
    ~ScriptEngine();
    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    // Create the VM and run every file (errors are logged, never thrown).
    void load(Simulation& sim, const Json& rules, const Json& map);
    void tick(Simulation& sim);   // the "scripts" system (last in the tick)
    // A standard-library system (no instruction budget: engine code). Errors are logged once.
    // Extra integer arguments follow the tick (entities as packed handles).
    void callStd(Simulation& sim, StdFn fn, std::initializer_list<int64_t> args = {});
    bool hasUserScripts() const { return userScripts_; }

    // Entry points for data: trigger condition / action {"type": "script", "fn": ...} and the ability
    // effect {"type": "script", "fn": ...} (queued, runs in this tick's scripts phase).
    bool callCondition(Simulation& sim, const std::string& fn, const Json& args, uint8_t player, Entity unit, Vec3 at);
    void callAction(Simulation& sim, const std::string& fn, const Json& args, uint8_t player, Entity unit, Vec3 at, const std::string& trigger);
    void queueEffect(const EffectDef& e, const EffectCtx& ctx, std::vector<Entity> targets);

    bool hasFunction(const std::string& name) const;
    const std::vector<ScriptLogEntry>& log() const { return log_; }
    int errorCount() const { return errors_; }
    // Log sink (editor console); called for every new entry.
    std::function<void(const ScriptLogEntry&)> onLog;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    std::vector<ScriptLogEntry> log_;
    int errors_ = 0;
    bool userScripts_ = false;
    friend struct Impl;
};

} // namespace rts::game
