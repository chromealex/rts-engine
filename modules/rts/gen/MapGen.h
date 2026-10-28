// Deterministic map generator (flat maps and cube-sphere bodies). Integer / fixed point only: the same
// request (preset + params + seed) produces a bit-identical map on every platform, so it can run at match
// start inside lockstep (map key "generate" with "seed": "match"), in headless and replays, and in the
// editor / MCP (bake to a static map). Format and presets: docs/game-format.md "Map generation".
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "engine/data/Json.h"
#include "modules/rts/RtsData.h"

namespace rts::gen {

struct GenRequest {
    std::string preset = "continents";
    uint64_t seed = 1;
    int players = 2;
    std::string symmetry = "auto";
    Json params = Json::object();            // overrides the preset's params
};

// Parses a "generate" block. `matchSeed` replaces "seed": "match" (or a missing seed).
bool parseRequest(const Json& block, uint64_t matchSeed, int defaultPlayers, GenRequest& out, std::string* err);

// Generates into `map` (a map JSON: kept keys such as name, victory, triggers, players' names/factions/teams
// stay; terrain, fields, entities, regions, roads, terrainMaterial and players' starts are replaced).
// `rules` decides the terrain model and which types are placed; `rulesJson` may carry "mapgen": {"presets"}
// (extra / overriding presets) and the visual "terrain" block (material layer ids). `visual` optionally gives
// the game.json "terrain" block (layers) for material mapping.
bool generateMap(const GenRequest& req, const game::Rules& rules, const Json& rulesJson, Json& map, std::string* err,
                 const Json* visual = nullptr);

// Match-time hook (RtsModule::setup): if `map` has "generate", replaces it with the generated map (the
// "generate" block is removed, a "generated" summary block is added). `players`: the simulation's player
// count (authoritative). Returns false with a readable error.
bool applyGenerate(Json& map, const game::Rules& rules, const Json& rulesJson, uint64_t matchSeed, int players, std::string* err);
// Seed-independent loader step (called by applyTilesToMap, so every loader runs it before Simulation::setup):
// resizes the players array to generate.players (the simulation creates its players from it), attaches a
// template map (generate.template = "maps/x.json") as generate.templateData and the visual material layer ids
// of game.json terrain.layers as generate.layerIds. `read` reads content-relative files.
bool prepareGenerate(Json& map, const std::function<bool(const std::string&, std::string&)>& read, std::string* err);

// Presets: built-in + rules "mapgen.presets". Each entry: {"id", "title", "genre", "params": {...}}.
Json presetList(const Json& rulesJson);
bool presetParams(const std::string& id, const Json& rulesJson, Json& out, std::string* err);

uint64_t mapHash(const Json& map);   // FNV-1a over the compact dump (golden tests, MCP)

} // namespace rts::gen
