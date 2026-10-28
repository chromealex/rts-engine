// Editor for every game built on the engine. Unity-like layout of dockable windows (Scene, Game,
// Hierarchy, Inspector, Tools, Project, Rules, Scenario, Statistics, Agent, Console); windows are
// plugins (EditorWindow.h). All document state lives in the shared Workspace (apps/editor/core),
// which is also what the MCP server drives, so an agent's edits show up here live.
//
// The public part of Editor is the API windows use; keep it small and stable.
#pragma once
#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "EditorWindow.h"
#include "core/Net.h"
#include "core/Workspace.h"
#include "engine/fx/ParticleSystem.h"
#include "engine/render/TextureCache.h"
#include "engine/gfx/Gfx.h"
#include "engine/platform/Platform.h"
#include "engine/render/ImGuiLayer.h"
#include "engine/render/Renderer3D.h"
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsMap.h"
#include "modules/rts/RtsModule.h"
#include "modules/rts/fx/RtsFx.h"
#include "modules/rts/view/RtsView.h"

namespace rts::editor {

// What the Inspector shows. Hierarchy/Project/Scene set it.
struct Selection {
    enum Kind { None, Entity, Player, Area, Region, Wave, Trigger, Type, Resource, Tech, Faction, MapProps, GameProps } kind = None;
    int index = -1;          // entity / player (0-based) / wave / trigger
    std::string key;         // area name, region letter, rule id
    bool operator==(const Selection& o) const { return kind == o.kind && index == o.index && key == o.key; }
};

enum class Tool { Select, Place, Terrain, Level, Height, Field, Region, Area, Start, Road, Erase, Count };

// Offscreen 3D view shown inside a window with ImGui::Image.
struct Viewport {
    gfx::TextureHandle color, depth;
    int w = 0, h = 0;                 // pixels
    float pointsW = 0, pointsH = 0;   // logical size in the window
    float originX = 0, originY = 0;   // top-left on screen (points)
    bool hovered = false;
    void ensure(gfx::Device& dev, int pw, int ph);
    void release(gfx::Device& dev);
    // ImGui::Image with the right orientation for the backend
    void image(gfx::Device& dev, float wPts, float hPts);
};

class Editor {
public:
    struct Options {
        std::string projectDir;   // folder with game.json (empty: show the game picker)
        std::string engineDir;    // engine checkout (games/, docs/, rtsbuild)
        std::string buildTool;    // path to rtsbuild
        std::string gameExe;      // path to the game client (device runs)
        std::string openMap;
        int listenPort = net::kDefaultPort; // 0 = no agent link
    };

    explicit Editor(Options o) : opt_(std::move(o)) {}
    void init(gfx::Device& dev);
    void frame(gfx::Device& dev, double dt);
    void event(const platform::Event& e);
    void shutdown();

    // ---------------- API for windows ----------------
    Workspace& ws() { return ws_; }
    MapDoc& doc() { return ws_.map; }
    const game::Rules& rules() const { return ws_.project.rules; }
    gfx::Device& device() { return *dev_; }
    render::Renderer3D& renderer() { return r3d_; }
    game::RtsView& view() { return view_; }
    const game::MapGrid& editGrid() const { return grid_; }
    const Options& options() const { return opt_; }
    double time() const { return time_; }

    Selection selection;
    void select(Selection s) { selection = std::move(s); }
    int selectedEntity() const { return selection.kind == Selection::Entity ? selection.index : -1; }
    void selectEntity(int i) { if (i < 0) selection = {}; else selection = {Selection::Entity, i, {}}; }

    // tools (Tools window edits these; Scene window applies them)
    Tool tool = Tool::Select;
    int placeType = 0, placeOwner = 0;
    char paintCell = '~';
    int paintLevel = 1, fieldRes = 0, fieldAmount = 9, startPlayer = 1, brush = 1;
    char regionLetter = 'A';
    bool snap = true, showLevels = true, showRegions = true, showAreas = true, followAgent = true;
    int roadStart = -1;

    // camera of the Scene window (agents fly it with view_focus)
    render::RTSCamera sceneCam;
    void focusScene(float x, float z, float distance = -1);
    // planet maps (world block): the Scene camera is the view's orbit camera; picking returns a body point
    bool planetMap() const { return view_.planetMode(); }
    bool planetAt(const Viewport& vp, float sx, float sy, int& body, render::float3& local) const;
    std::string planetBodyId(int body) const;
    void fitScene();

    // play-test (Game window)
    bool playing() const { return sim_ != nullptr; }
    Simulation* sim() { return sim_.get(); }
    game::RtsModule* simModule() { return sim_ ? dynamic_cast<game::RtsModule*>(&sim_->module()) : nullptr; }
    void startPlay(int humanSlot = 0);
    void stopPlay();
    int humanSlot() const { return human_; }
    float playSpeed = 1.0f;
    bool playPaused = false;
    render::RTSCamera gameCam;
    std::vector<rts::Entity> gameSelection;
    std::string gameDevice = "none";           // simulator preset (Devices.h)
    bool gameLandscape = true;
    bool gameSafeArea = false;          // Game window: show the device safe area on top of the picture
    Viewport* gameViewport = nullptr;          // set by the Game window (screenshots crop to it)
    Viewport* sceneViewport = nullptr;
    void issueCommand(Command c);             // from the human player in the Game window

    // documents
    void newMap(int w, int h);
    void openMap(const std::string& file);
    bool saveMap(const std::string& file);
    void openGame(const std::string& idOrDir);
    void reloadRules();                        // after rules change: views, palette
    void showGamePicker() { showPicker_ = true; }

    // rendering a map view into a Viewport (used by Scene and Game windows)
    void renderScene(Viewport& vp, const render::RTSCamera& cam, bool editOverlays);
    void renderGame(Viewport& vp, const render::RTSCamera& cam);

    // particle effects of the play-test (fx/fx.json + fx/*.json of the open game)
    fx::ParticleSystem& gameFx() { return gameFx_; }
    void reloadFx();                           // after fx files changed (Effects window, agents)
    std::vector<std::string> fxWarnings;       // from the last reloadFx()

    // helpers for windows
    bool groundAt(const Viewport& vp, const render::RTSCamera& cam, float sx, float sy, float& x, float& z) const;
    int pickEntity(const Viewport& vp, const render::RTSCamera& cam, float sx, float sy) const;
    int pickRoad(const Viewport& vp, const render::RTSCamera& cam, float sx, float sy) const;
    float snapCoord(float v, int typeIdx) const;
    void flash(float x0, float z0, float x1, float z1, double seconds = 1.6);
    std::string status;                        // status bar text
    void runBuild(const std::string& target);
    int buildConfig = 1;

    // windows
    EditorWindow* window(const std::string& id);
    const std::vector<std::unique_ptr<EditorWindow>>& windows() const { return windows_; }
    void resetLayout() { resetLayout_ = true; }
    void launchOnDevice(const std::string& device, bool landscape);

    // statistics
    struct FrameStats { float frameMs = 0, simMs = 0; uint32_t drawCalls = 0; };
    std::deque<FrameStats> frameHistory;
    struct PlaySample { float t; std::vector<int> units, structures; std::vector<std::vector<float>> resources; };
    std::vector<PlaySample> playHistory;

private:
    void drawMenuBar();
    void drawDockspace();
    void drawGamePicker();
    void drawStatusBar();
    void drawActivityToasts();
    void handleShortcuts();
    void syncGrid();
    void buildEditScene(game::RtsScene& out) const;
    void drawEditOverlays3D();
    void applyChanges();
    void pollAgentLink();
    void registerGuiOps();
    void registerPlayOps();   // EditorPlayOps.cpp
    void samplePlayStats();
    void deliverShots(gfx::Device& dev);

    Options opt_;
    Workspace ws_;
    gfx::Device* dev_ = nullptr;
    render::ImGuiLayer ui_;
    render::Renderer3D r3d_;
    game::RtsView view_;
    bool showPicker_ = false;
    bool resetLayout_ = false;
    int focusFirst_ = 0;
    std::vector<std::unique_ptr<EditorWindow>> windows_;

    game::MapGrid grid_;
    uint64_t gridRevision_ = ~0ull;
    uint64_t worldHash_ = 0;   // map world block as last given to the view (planets)
    double gridRebuildAt_ = 0;
    bool playTerrainBuilt_ = false;
    game::RtsScene scene_;
    double time_ = 0;
    bool quit_ = false;

    struct Flash { float x0, z0, x1, z1; double until, length; };
    std::vector<Flash> flashes_;
    bool sceneFlying_ = false;
    render::float3 sceneGoal_{};

    fx::ParticleSystem gameFx_;
    game::RtsFx gameFxBridge_;
    render::TextureCache fxTextures_;          // sprite textures of effects (same loader as sprites)
    double fxTime_ = -1;
    std::unique_ptr<Simulation> sim_;
    std::vector<game::RtsAI> bots_;
    int human_ = 0;
    float lastSimMs_ = 0;

    net::LineServer server_;
    struct PendingShot { int client; int64_t id; std::string device; };
    std::vector<PendingShot> shots_;
    bool captureArmed_ = false;
};

} // namespace rts::editor
