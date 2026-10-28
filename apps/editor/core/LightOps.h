// Lighting operations of the editor core (MCP agents and the Lighting window): the map's decorative
// "lights" array and its "lighting" block (time of day, clouds, storm). Format: modules/rts/view/RtsLights.h.
#pragma once
#include "Workspace.h"

namespace rts::editor {

void registerLightOps(Workspace& ws);
// Copies the known light fields (x, z, height, color, radius, intensity, flicker, halo, name) from
// params into a light object, keeping any other fields.
void applyLightParams(Json& light, const Json& params);
Json& mapLightsMut(MapDoc& doc);   // the map's "lights" array (created when missing)

} // namespace rts::editor
