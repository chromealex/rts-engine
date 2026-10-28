// rtsmcp — Model Context Protocol server (stdio) for building games with the RTS engine.
//
// Every tool is an editor operation (see apps/editor/core/Workspace.cpp). When the editor is
// running (or can be started) the call is forwarded to it over localhost, so the user watches the
// agent work live: the camera flies to each change, it is listed in the activity feed and can be
// undone. Without an editor the same operations run here directly on the project files.
//
// The server only touches game projects (games/<id>/...). It never edits engine code.
//
//   rtsmcp [--engine DIR] [--game ID] [--port N] [--no-editor] [--editor PATH]
//
// Claude Code:   claude mcp add rtsengine -- /path/to/rtsmcp --engine /path/to/RTSEngine
// Claude Desktop (claude_desktop_config.json):
//   {"mcpServers": {"rtsengine": {"command": "/path/to/rtsmcp", "args": ["--engine", "/path/to/RTSEngine"]}}}
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "apps/editor/core/Net.h"
#include "apps/editor/core/Workspace.h"

#ifndef RTS_SOURCE_DIR
#define RTS_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;
using namespace rts;
using namespace rts::editor;

namespace {

struct Options {
    std::string engine = RTS_SOURCE_DIR, game, editorPath;
    int port = net::kDefaultPort;
    bool noEditor = false;
};

std::string exeDir() {
    std::error_code ec;
#if defined(__linux__)
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path().string();
#endif
    return fs::current_path(ec).string();
}

std::string findEditor(const Options& o, const std::string& self) {
    if (!o.editorPath.empty()) return o.editorPath;
    for (fs::path c : {fs::path(self) / "rts_editor", fs::path(self) / "RTSEditor.app/Contents/MacOS/RTSEditor",
                       fs::path(self) / "rts_editor.exe", fs::path(self) / "Release/rts_editor.exe"}) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c.string();
    }
    return {};
}

std::string findTool(const std::string& self, const char* name) {
    for (fs::path c : {fs::path(self) / name, fs::path(self) / (std::string(name) + ".exe")}) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c.string();
    }
    return name;
}

// Connection to the editor. Reconnects lazily; launches the editor once if allowed.
class EditorLink {
public:
    EditorLink(Options o, std::string self) : opt_(std::move(o)), self_(std::move(self)) {}
    bool ensure() {
        if (cli_.connected()) return true;
        if (cli_.connect(opt_.port)) return true;
        if (opt_.noEditor || launched_) return false;
        launched_ = true;
        std::string ed = findEditor(opt_, self_);
        if (ed.empty()) return false;
        std::string cmd = "\"" + ed + "\" --listen " + std::to_string(opt_.port) + " --engine \"" + opt_.engine + "\"";
        if (!opt_.game.empty()) cmd += " --game \"" + opt_.game + "\"";
#if defined(_WIN32)
        cmd = "start \"\" " + cmd;
#else
        cmd += " >/dev/null 2>&1 &";
#endif
        if (std::system(cmd.c_str()) != 0) return false;
        for (int i = 0; i < 100; ++i) {          // up to 10 s for the window to come up
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (cli_.connect(opt_.port)) return true;
        }
        return false;
    }
    // Returns false if the editor is gone (caller falls back to the local workspace).
    bool request(const std::string& method, const Json& params, Json& result) {
        if (!ensure()) return false;
        Json req = Json::object();
        req.set("id", Json::number(int64_t(++seq_)));
        req.set("method", Json::string(method));
        req.set("params", params);
        if (!cli_.send(req.dump(-1))) return false;
        std::string line;
        // builds and matches can take a while; the editor answers when the op is done
        while (cli_.recv(line, 600000)) {
            Json resp = Json::parse(line);
            if (resp["id"].integer() != int64_t(seq_)) continue;
            result = resp["result"];
            return true;
        }
        cli_.close();
        return false;
    }
    bool connected() const { return cli_.connected(); }

private:
    Options opt_;
    std::string self_;
    net::LineClient cli_;
    bool launched_ = false;
    uint64_t seq_ = 0;
};

void reply(const Json& id, Json result) {
    Json r = Json::object();
    r.set("jsonrpc", Json::string("2.0"));
    r.set("id", id);
    r.set("result", std::move(result));
    std::cout << r.dump(-1) << "\n" << std::flush;
}
void replyError(const Json& id, int code, const std::string& msg) {
    Json r = Json::object(), e = Json::object();
    r.set("jsonrpc", Json::string("2.0"));
    r.set("id", id);
    e.set("code", Json::number(int64_t(code)));
    e.set("message", Json::string(msg));
    r.set("error", e);
    std::cout << r.dump(-1) << "\n" << std::flush;
}

// Tool result -> MCP content. Screenshots come back as images the model can look at.
Json toContent(const Json& result) {
    Json content = Json::array();
    bool isError = result.has("error");
    if (result.has("png_base64")) {
        Json img = Json::object();
        img.set("type", Json::string("image"));
        img.set("data", result["png_base64"]);
        img.set("mimeType", Json::string("image/png"));
        content.push(img);
        Json rest = result;
        rest.erase("png_base64");
        Json t = Json::object();
        t.set("type", Json::string("text"));
        t.set("text", Json::string(rest.dump()));
        content.push(t);
    } else {
        Json t = Json::object();
        t.set("type", Json::string("text"));
        // big texts (schema, ascii maps, logs) read better unescaped
        if (result.has("text") && result.size() == 1) t.set("text", result["text"]);
        else t.set("text", Json::string(result.dump()));
        content.push(t);
    }
    Json out = Json::object();
    out.set("content", content);
    if (isError) out.set("isError", Json::fromBool(true));
    return out;
}

const char* kInstructions =
    "Tools to build real-time strategy games with RTSEngine: a game is data (game.json, rules.json, maps). "
    "Workflow: list_games / create_game / open_game -> read rules_schema (the data format reference) -> rules_list / rules_get -> "
    "edit rules with rules_set / rules_merge -> build maps (map_new, terrain_paint, terrain_level, field_paint, region_paint, "
    "entity_place, player_set, map_set for waves/triggers/areas/victory) -> map_validate -> match_run (bots, balance check) -> "
    "map_save {addToGame:true} -> view_screenshot to look at it -> build_start for a platform. "
    "map_ascii gives a quick text picture of a map. 2D tile games: tileset_list, tiles_init, tile_paint. Dungeon / territory "
    "games: terrain_paint with the game's map characters, territory_paint. Smooth relief (heightfield): terrain_sculpt, terrain_heights, "
    "terrain_material, heightmap_import / heightmap_export. Other data files (lang/*.json, audio.json, "
    "tilesets, fx, ui/*.rml): game_file_read / game_file_write; models, textures, sounds: asset_import. "
    "Balancing: balance_stats (numbers side by side: DPS per armor, value per cost) -> balance_duel / balance_matrix "
    "(arena fights at equal cost: who counters whom) -> rules_set to adjust -> balance_batch (many bot matches: win rate per "
    "slot and faction, match length, what never gets built; 'mirror' separates map side from faction). "
    "Phones: device_list {filter} (iPhone, iPad, Pixel, Galaxy, Redmi, desktops: sizes, safe areas, cut-outs) -> "
    "device_set {device, orientation, safeArea: true} -> view_screenshot {window: game}. "
    "To test a mechanic by playing: play_start, play_units, play_command (any player command, e.g. surrender, dig, ability), play_status. "
    "Mechanics the data blocks can't express: Lua scripts - script_api (reference) -> script_write {path: scripts/x.lua, register: true} "
    "-> script_check (compile + short bot match, errors with file:line); simulation scripts use integers and fx() numbers, no floats. "
    "Visual effects: fx_templates (explosion, fire, smoke, magic, weather, ...) or fx_import_unity {source: a Unity .prefab / folder} "
    "to make the same effect as a Unity one, tune with fx_get / fx_set, look with fx_preview {effect, advance, freeze} + view_screenshot, "
    "attach to events with fx_bind (died, hit, attack, ability, spawned per type / weapon / ability). "
    "Look of the game: own meshes / textures via asset_import, then the type's \"view\" block with rules_merge "
    "(model, materials overrides per material name, team colour, clips: docs/models.md; model_info lists nodes, clips, materials); "
    "lighting: lighting_set (sun, sky, day/night, presets), light_add / light_update (point lights on the map); "
    "fog of war: rules_merge {path: \"fogOfWar\"} (mode, colour, style, softness, grid, lineOfSight for 3D visibility by heights, "
    "vision.blocks on wall types, delivery.blocksVision on smoke skills); "
    "game UI: ui_contract (documents, data bindings, events) -> game_file_write ui/hud.rml, ui/skin.rcss ... (the game's ui/ overrides the engine's). "
    "Everything you do shows up live in the user's editor window and can be undone there. "
    "You can only change game projects, never the engine.";

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--engine" && i + 1 < argc) opt.engine = argv[++i];
        else if (a == "--game" && i + 1 < argc) opt.game = argv[++i];
        else if (a == "--port" && i + 1 < argc) opt.port = std::atoi(argv[++i]);
        else if (a == "--editor" && i + 1 < argc) opt.editorPath = argv[++i];
        else if (a == "--no-editor") opt.noEditor = true;
    }
    std::error_code ec;
    opt.engine = fs::weakly_canonical(opt.engine, ec).string();
    const std::string self = exeDir();

    Workspace local;                      // used when no editor is reachable
    local.engineDir = opt.engine;
    local.buildTool = findTool(self, "rtsbuild");
    if (!opt.game.empty()) { std::string err; local.openGame(opt.game, &err); }
    EditorLink link(opt, self);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty() || line == "\r") continue;
        Json msg;
        try { msg = Json::parse(line); } catch (const std::exception& e) { replyError(Json(), -32700, e.what()); continue; }
        const std::string method = msg.getStr("method");
        const Json& id = msg["id"];
        const bool isRequest = msg.has("id");
        try {
            if (method == "initialize") {
                Json r = Json::object(), caps = Json::object(), info = Json::object();
                r.set("protocolVersion", Json::string(msg["params"].getStr("protocolVersion", "2025-06-18")));
                caps.set("tools", Json::object());
                r.set("capabilities", caps);
                info.set("name", Json::string("rtsengine"));
                info.set("version", Json::string("0.2.0"));
                r.set("serverInfo", info);
                r.set("instructions", Json::string(kInstructions));
                reply(id, r);
            } else if (method == "tools/list") {
                Json r = Json::object();
                r.set("tools", local.toolList()); // same registry as the editor: the list never changes mid-session
                reply(id, r);
            } else if (method == "tools/call") {
                const std::string name = msg["params"].getStr("name");
                const Json& args = msg["params"]["arguments"];
                Json result;
                Json fwd = Json::object();
                fwd.set("name", Json::string(name));
                fwd.set("arguments", args.isObject() ? args : Json::object());
                bool viaEditor = link.request("call", fwd, result);
                if (!viaEditor) result = local.call(name, args, "agent");
                reply(id, toContent(result));
            } else if (method == "ping") {
                reply(id, Json::object());
            } else if (isRequest) {
                replyError(id, -32601, "method not found: " + method);
            }
            // notifications (initialized, cancelled...) need no answer
        } catch (const std::exception& e) {
            if (isRequest) replyError(id, -32603, e.what());
        }
    }
    return 0;
}
