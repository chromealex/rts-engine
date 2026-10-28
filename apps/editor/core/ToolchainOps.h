// Toolchain operations of the editor core (MCP agents and the Build window), over rtsbuild
// (tools/rtsbuild/Toolchains.h):
//   toolchain_check    what a target needs on this machine: found (path, version) / missing, sizes
//   toolchain_install  install what a target (or one requirement id) needs into
//                      ~/.rtsengine/toolchains, in the background; poll build_status
#pragma once
#include "Workspace.h"

namespace rts::editor {

void registerToolchainOps(Workspace& ws);

} // namespace rts::editor
