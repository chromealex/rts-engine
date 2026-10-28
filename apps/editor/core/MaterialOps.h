// Material operations of the editor core (MCP agents and the Materials window): the game's shared
// material library (materials.json), applying library or inline materials to a type's view (by
// model material name or by node), and procedural textures written as PNG into the game folder.
// Format: docs/models.md "Material library".
#pragma once
#include "Workspace.h"

namespace rts::editor {

void registerMaterialOps(Workspace& ws);

// materials.json of the open game ({"materials": {...}}); empty object when missing.
Json loadMaterialLibrary(const Workspace& ws);
void saveMaterialLibrary(Workspace& ws, const Json& lib, const std::string& what);   // throws on failure
// Keys a material understands (override keys + "base" / "use" / "replace" / "description").
const std::vector<std::string>& materialKeys();
// Problems of one material definition ("unknown key 'colour'", "metallic must be 0..1").
std::vector<std::string> checkMaterial(const Json& m);

} // namespace rts::editor
