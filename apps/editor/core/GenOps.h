// Map generation in the editor core (Генерация window and MCP agents): generate into the open map with a
// preset + params + seed, regenerate, keep a match-time "generate" block or bake a static map, list presets.
// The generator itself is modules/rts/gen (deterministic, the same code runs at match start).
#pragma once
#include <string>
#include "Workspace.h"

namespace rts::editor {

void registerGenOps(Workspace& ws);

struct GenOptions {
    std::string preset = "classic";
    uint64_t seed = 1;
    int players = 2;
    std::string symmetry = "auto";
    Json params = Json::object();
    bool useTemplate = true;      // the open map's kit and objects drive what is placed (else rules heuristics)
    bool keepSize = false;        // keep the open map's size (otherwise the preset's size, unless params.size)
};

// Generates into the open map (undoable, one snapshot). Returns the "generated" summary or throws.
Json generateIntoMap(Workspace& ws, const GenOptions& o);
// Writes a match-time "generate" block into the map (the map regenerates at every match start).
void setMatchGenerate(Workspace& ws, const GenOptions& o, bool matchSeed);

} // namespace rts::editor
