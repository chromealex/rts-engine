// Editor core shared by the editor GUI and the MCP server: the open game project (manifest +
// rules), the open map, and a registry of named operations with JSON parameters. Every edit an
// agent makes goes through `call()`, so the GUI shows it live, records it in the activity feed and
// in undo history. No UI or rendering in here.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <map>
#include <string>
#include <vector>
#include "MapDoc.h"
#include "engine/data/Json.h"
#include "modules/rts/RtsData.h"

namespace rts::editor {

struct GameInfo { std::string id, dir, name; int maps = 0; };

struct Project {
    std::string dir;              // folder with game.json
    Json manifest = Json::object();
    Json rulesJson = Json::object();
    game::Rules rules;
    std::string rulesError;       // last loader error (rules stay usable from the previous good state)
    bool rulesDirty = false, manifestDirty = false;

    bool load(const std::string& dir, std::string* err);
    bool saveRules(std::string* err);
    bool saveManifest(std::string* err);
    std::string rulesPath() const;
    bool reloadRules();           // parse rulesJson -> rules; false keeps the previous rules
};

// What changed, so views can fly the camera there and flash it.
struct Change {
    enum Kind { Cells, Entity, Players, Map, Rules, Project, View, Fx } kind = Map;   // Fx: fx/*.json changed (reload effects)
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;   // cell rect (inclusive)
    int entity = -1;
    std::string text;                        // human readable, Russian
};

struct Activity { double time = 0; std::string source, text; bool error = false; };

class Workspace {
public:
    using OpFn = std::function<Json(const Json& params)>;
    struct Op { std::string name, description; Json schema; OpFn fn; bool guiOnly = false; };

    Workspace();
    ~Workspace();
    std::string engineDir;        // engine checkout (templates/, docs/, rtsbuild)
    std::string buildTool;        // rtsbuild executable
    Project project;
    MapDoc map;
    std::vector<Activity> activity;
    std::vector<Change> changes;  // consumed by the GUI each frame
    std::function<double()> clock = [] { return 0.0; };

    // Run an operation. Never throws: errors come back as {"error": "..."}.
    Json call(const std::string& name, const Json& params, const std::string& source = "editor");
    const std::vector<Op>& ops() const { return ops_; }
    void registerOp(Op op);       // GUI adds view/play/screenshot ops
    Json toolList() const;        // MCP tools/list payload

    std::vector<GameInfo> listGames() const;
    bool openGame(const std::string& idOrDir, std::string* err);
    std::string gamesDir() const { return engineDir + "/templates"; }   // project templates (sample games)
    std::string mapsDir() const { return project.dir + "/maps"; }
    std::string relative(const std::string& file) const;
    void note(Change c) { changes.push_back(std::move(c)); }
    void log(const std::string& source, const std::string& text, bool error = false);

    // background build (rtsbuild)
    bool building() const { return building_; }
    std::string logsDir() const;   // <engine>/logs, created on demand
    std::string buildLog() { std::lock_guard<std::mutex> lk(buildMutex_); return buildLog_; }
    // extraArgs: more rtsbuild flags (Build window: --estimate --out ..., --audio, --no-atlas, ...)
    void startBuild(const std::string& target, const std::string& config, const std::string& extraArgs = "");
    int buildResult() { std::lock_guard<std::mutex> lk(buildMutex_); return buildRc_; }
    // Any rtsbuild run streamed into the same log (Build window / MCP): args after the executable;
    // `label` names the log file (logs/<label>-<time>.log); withProject adds --project / --engine.
    void startTool(const std::string& args, const std::string& label, bool withProject);
    std::string runLabel() { std::lock_guard<std::mutex> lk(buildMutex_); return runLabel_; }   // "build-web", "estimate", "toolchain-web"
    std::string lastLogFile() { std::lock_guard<std::mutex> lk(buildMutex_); return logFile_; }
    int runCount() const { return runCount_; }   // increments when a run finishes
    // rtsbuild --check-toolchain <target> --json (blocking: probes tools, about a second).
    Json toolchainCheck(const std::string& target) const;

private:
    void registerCoreOps();
    std::thread buildThread_;
    std::atomic<bool> building_{false};
    std::mutex buildMutex_;
    std::string buildLog_;
    int buildRc_ = 0;
    std::string runLabel_, logFile_;
    std::atomic<int> runCount_{0};
    std::vector<Op> ops_;
};

// "types.footman.weapons[0].damage" -> pointer into j (created when `create`), nullptr if missing.
Json* jsonPath(Json& j, const std::string& path, bool create);
const Json* jsonPath(const Json& j, const std::string& path);

} // namespace rts::editor
