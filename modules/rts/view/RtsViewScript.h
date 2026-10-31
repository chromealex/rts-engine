// View scripts (rules/map "viewScripts"): Lua for presentation on the local machine — floats allowed,
// read-only game queries (the same `game` table as simulation scripts, without actions), and requests
// the host turns into UI notices, particle effects and sounds. Never writes into the simulation.
// docs/scripting.md § View scripts.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../../engine/data/Json.h"
#include "../RtsScript.h"
#include "../../../engine/sim/Command.h"

namespace rts { class Simulation; }

namespace rts::game {

class RtsModule;

struct ViewRequest {
    enum Kind : uint8_t { Notice, Effect, Sound } kind = Notice;
    std::string text;          // notice text / effect or sound name
    float x = 0, z = 0;
    bool positioned = false;
};

class ViewScripts {
public:
    ViewScripts();
    ~ViewScripts();
    // rules must carry "$scriptSources" (embedScripts). False when the game has no view scripts.
    bool load(Simulation& sim, const Json& rules, const Json& map, uint8_t localPlayer);
    // Collects the simulation's events every tick (a read-only system, like the fx bridge).
    void attach(Simulation& sim);
    void detach();
    // Per frame: dispatches the collected events and on_frame(dt).
    void update(Simulation& sim, float dt);
    std::vector<ViewRequest> takeRequests();
    // view.send(id, args): script commands (kCmdScript) for the host to issue as the local player
    std::vector<Command> takeCommands();
    const std::vector<ScriptLogEntry>& log() const { return log_; }
    bool active() const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    std::vector<ScriptLogEntry> log_;
};

} // namespace rts::game
