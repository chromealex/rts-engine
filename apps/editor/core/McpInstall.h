// One-click MCP setup: registers rtsmcp (the engine's MCP server) in the AI clients found on this
// computer, so an agent can drive the editor without typing commands.
//   Claude Code     `claude mcp add --scope user` when the CLI is on PATH, else ~/.claude.json
//   Claude Desktop  claude_desktop_config.json (macOS Application Support / %APPDATA% / ~/.config)
//   Cursor          ~/.cursor/mcp.json
//   Windsurf        ~/.codeium/windsurf/mcp_config.json
//   Codex CLI       ~/.codex/config.toml ([mcp_servers.rtsengine])
//   Project         <engine>/.mcp.json (Claude Code / Cursor opened in the engine folder)
// JSON configs are edited as text: only the "rtsengine" entry inside "mcpServers" is inserted or
// replaced, everything else in the user's file stays byte for byte (a .bak copy is written first).
#pragma once
#include <string>
#include <vector>

namespace rts::editor {

struct McpServerSpec {
    std::string name = "rtsengine";
    std::string command;               // absolute path to rtsmcp
    std::vector<std::string> args;     // --engine <dir> --editor <editor exe>
};

struct McpClientInfo {
    std::string id, name, configPath;
    bool present = false;              // the client looks installed (config folder or CLI found)
    bool installed = false;            // an "rtsengine" entry is already there
    bool upToDate = false;             // ... and points to this rtsmcp
};

std::vector<McpClientInfo> mcpClients(const McpServerSpec& spec, const std::string& engineDir);
// Adds or updates the entry. Returns false with a readable error.
bool mcpInstall(const std::string& clientId, const McpServerSpec& spec, const std::string& engineDir, std::string* error);
bool mcpUninstall(const std::string& clientId, const McpServerSpec& spec, const std::string& engineDir, std::string* error);

// Text-level JSON edit used by the installers (exposed for tests): sets root.mcpServers[name] = entryJson,
// or removes it when entryJson is empty. Keeps the rest of the text untouched.
bool mcpPatchJson(std::string& text, const std::string& name, const std::string& entryJson, std::string* error);
std::string mcpEntryJson(const McpServerSpec& spec, const char* indent = "    ");

} // namespace rts::editor
