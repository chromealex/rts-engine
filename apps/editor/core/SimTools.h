// Shared helpers for tools that drive a live or headless simulation (play_command, scenario_run,
// match_run): names of events, orders and commands, building a player command from JSON, resolving
// unit references and describing an object. No rendering, no editor state.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "engine/data/Json.h"
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsModule.h"

namespace rts::editor {

const char* simEventName(uint16_t type);          // "" for unknown types
int simEventByName(const std::string& name);      // -1 if unknown
std::string simEventNames();                      // comma separated, for error messages
const char* orderName(game::OrderType t);
std::string commandNames();

// Unit references in tool JSON: a number (packed entity id, as play_units returns), a string
// (scenario / map entity "id"), or a filter object {"type", "owner", "area": [x, z, w, h],
// "near": [x, z], "radius"} matching every live object.
struct SimRefs {
    Simulation& sim;
    game::RtsModule& mod;
    std::map<std::string, Entity> ids;               // named objects
    void resolve(const Json& ref, std::vector<Entity>& out) const;   // throws on unknown names
    Entity one(const Json& ref) const;               // first match (kNullEntity if a filter matches nothing)
    Json refJson(Entity e) const;                    // the name as a string, else the packed id as a number
    bool matches(const Json& filter, Entity e) const;
};

// Map entities that carry an "id" -> the live entity spawned for them (same type and owner,
// nearest to the map position). Call right after Simulation::setup.
std::map<std::string, Entity> mapEntityIds(Simulation& sim, game::RtsModule& mod, const Json& map);

// Player command from JSON (keys documented in play_command). defPlayer: slot when "player" is absent.
Command commandFromJson(const SimRefs& refs, const Json& p, uint8_t defPlayer);
extern const char* kCommandHelp;                  // description of the command keys for tool schemas
const char* commandSchemaProps();                 // JSON schema "properties" body (no braces) for those keys

// Object state: id, type, owner, x, z, hp, maxHp, order, energy, statuses, garrison, cargo, level.
Json entityJson(const SimRefs& refs, Entity e);

// The map as the game loads it: tile layers baked into terrain (2D tile games). gameDir resolves tilesets.
Json mapForSim(Json map, const std::string& gameDir);

Fixed toFixed(double v);
} // namespace rts::editor
