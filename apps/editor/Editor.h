// Map editor for any game built on the generic RTS layer. The palette, owners and properties come
// from the project's rules.json, so the same editor works for every game.
#pragma once
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MapDoc.h"
#include "engine/gfx/Gfx.h"
#include "engine/platform/Platform.h"
#include "engine/render/ImGuiLayer.h"
#include "engine/render/Renderer3D.h"
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsMap.h"
#include "modules/rts/view/RtsView.h"

namespace rts::editor {

class Editor {
public:
    struct Options {
        std::string projectDir;   // folder with game.json
        std::string engineDir;    // engine checkout (for rtsbuild)
        std::string buildTool;    // path to rtsbuild
        std::string openMap;
    };

    explicit Editor(Options o) : opt_(std::move(o)) {}
    ~Editor();
    void init(gfx::Device& dev);
    void frame(gfx::Device& dev, double dt);
    void event(const platform::Event& e);
    void shutdown();
    void selectForDemo(int i) { selected_ = i; }
    void playForDemo() { startPlay(); }

private:
    enum class Tool { Select, Place, Terrain, Height, Road, Erase };
    enum class Mode { Edit, Play };

    void drawMenuBar();
    void drawToolsPanel();
    void drawPropertiesPanel();
    void drawStatusBar();
    void drawFileDialog();
    void drawBuildWindow();
    void drawNewMapDialog();
    void handleShortcuts();

    int pick(float sx, float sy) const;
    int pickRoad(float sx, float sy) const;
    bool groundAt(float sx, float sy, float& x, float& z) const;
    float snap(float v, int typeIdx) const;
    void onPrimaryDown(float x, float y, uint8_t mods);
    void onPrimaryDrag(float x, float y, uint8_t mods);
    void fitCamera();
    void syncGrid();             // MapDoc terrain -> MapGrid (+ terrain mesh)
    void buildScene();

    void newMap(int w, int h);
    void openMap(const std::string& file);
    bool saveMap(const std::string& file);
    void loadProject();
    void addCurrentMapToGame();
    std::string mapsDir() const;
    std::string relativeToProject(const std::string& file) const;

    void startPlay();
    void stopPlay();
    void runBuild(const std::string& target);

    Options opt_;
    gfx::Device* dev_ = nullptr;
    render::ImGuiLayer ui_;
    render::Renderer3D r3d_;
    game::RtsView view_;
    render::RTSCamera cam_;
    Json rulesJson_;
    game::Rules rules_;
    std::string gameName_ = "Game";
    std::string projectError_;

    MapDoc doc_;
    game::MapGrid grid_;
    bool gridDirty_ = true;
    double gridRebuildAt_ = 0;
    game::RtsScene scene_;
    Tool tool_ = Tool::Select;
    Mode mode_ = Mode::Edit;
    int placeType_ = 0;
    int placeOwner_ = 0;
    char paintCell_ = '~';
    int brush_ = 1;
    bool snap_ = true;
    int selected_ = -1;
    int hover_ = -1;
    int roadStart_ = -1;
    bool dragging_ = false, painting_ = false, panning_ = false, rotating_ = false;
    float lastX_ = 0, lastY_ = 0, dragOffX_ = 0, dragOffZ_ = 0;
    double time_ = 0;
    bool quit_ = false;
    std::string status_;
    std::vector<Issue> issues_;

    bool fileDialogOpen_ = false, fileDialogSave_ = false, newDialogOpen_ = false;
    char fileName_[256] = {};
    int newSize_[2] = {64, 48};
    int resize_[2] = {0, 0};

    std::unique_ptr<Simulation> sim_;
    std::vector<game::RtsAI> bots_;
    float playSpeed_ = 1.0f;
    bool playPaused_ = false;

    bool buildWindow_ = false;
    std::thread buildThread_;
    std::atomic<bool> building_{false};
    std::mutex logMutex_;
    std::string buildLog_;
    bool buildScrollToEnd_ = false;
    int buildConfig_ = 1;
};

} // namespace rts::editor
