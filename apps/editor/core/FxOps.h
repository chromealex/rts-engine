// Particle effect operations of the editor core (MCP agents, shared with the GUI): fx_list, fx_get,
// fx_set, fx_delete, fx_templates, fx_import_unity, fx_bind. Effects are templates/<id>/fx/<name>.json
// (format: engine/fx/FxEffect.h), event bindings templates/<id>/fx/fx.json (modules/rts/fx/RtsFx.h).
// Every write is validated with the engine's own loader first and noted as Change::Fx so the editor
// reloads its effects. fx_preview lives in the GUI (apps/editor/EditorFxOps.cpp).
#pragma once
#include <string>
#include <vector>
#include "Workspace.h"

namespace rts::editor {

void registerFxOps(Workspace& ws);

struct FxTemplate { const char* id; const char* description; const char* json; };
const FxTemplate* fxTemplates(size_t& count);   // FxTemplates.cpp

// Validation of an effect (name without .json; "fx" = the bindings) as the game would load it:
// errors block saving, warnings (missing textures / sub-effects, heavy settings) do not.
struct FxCheck { std::vector<std::string> errors, warnings; int emitters = 0; };
FxCheck checkFx(const Workspace& ws, const std::string& name, const Json& j);

bool validFxName(const std::string& n);   // a-z, 0-9, _ (1..48)

} // namespace rts::editor
