// Game client state shared by the client's translation units:
//   main.cpp      match flow, camera, picking, input -> Commands
//   ClientUi.cpp  RmlUi documents (menu, HUD, command card, pause) and their data model
//   Minimap.cpp   minimap image (CPU raster -> dynamic UI texture)
//   Lobby.cpp     online play: NetClient, lobby / room screens, network matches
#pragma once
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/EventListener.h>

#include "engine/audio/Audio.h"
#include "engine/gfx/Gfx.h"
#include "engine/i18n/GameLanguage.h"
#include "engine/net/NetClient.h"
#include "engine/platform/Platform.h"
#include "engine/render/ImGuiLayer.h"
#include "engine/render/Renderer3D.h"
#include "engine/sim/Simulation.h"
#include "engine/ui/UiLayer.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"
#include "modules/rts/audio/RtsAudio.h"
#include "modules/rts/view/RtsView.h"
#include "ClientFx.h"

namespace Rml { class ElementDocument; class Element; }

namespace rts::client {

using namespace rts::render;
using namespace rts::game;

struct MapEntry { std::string file, title; };
struct Notice { std::string text; double until; };

// Values bound to the "game" data model (see engine/assets/ui/*.rml). Compared every frame so only
// changed variables are marked dirty.
struct UiItem {          // resource bar entry
    std::string text, sub, style;  // style: inline css (player colour)
    bool warn = false;
    bool operator==(const UiItem&) const = default;
};
struct UiAction {        // command card button; `id` indexes GameClient::actions_
    std::string label, cost, hotkey, tip;
    std::string kind;         // layout of the section it belongs to: css class ("list", "row", "grid", "queue")
    bool header = false;      // section start: shows `label` as a title (may be empty: just a separator)
    bool enabled = true, active = false;
    float progress = -1;  // >= 0: shows a progress bar (production queue)
    int id = 0;
    int alt = -1;             // optional second button of the row (e.g. "watch" next to "join")
    std::string altLabel;
    bool operator==(const UiAction&) const = default;
};
struct UiSection {             // built per section, flattened into UiState::card for the document
    std::string title, kind;
    std::vector<UiAction> actions;
};
struct UiState {
    std::string screen = "menu";   // menu | lobby | playing | paused | over
    std::string gameName, error, platformInfo;
    std::vector<std::string> maps;
    std::vector<UiAction> languages;
    // lobby (Lobby.cpp)
    std::string lobby;             // connect | connecting | browse | room
    std::string netStatus, netError, roomTitle, roomInfo, spectators, waitHint;
    std::vector<UiAction> queues, rooms, createMaps, slots;
    std::vector<std::string> chat;
    bool isHost = false, myReady = false, canStart = false, netMatch = false, searching = false;
    std::vector<UiItem> top;
    std::vector<UiAction> topActions;
    std::string clock;
    std::vector<std::string> notices;
    bool hasSelection = false, ownSelection = false;
    std::string selTitle, emptyHint;
    std::vector<std::string> selInfo;
    std::vector<UiAction> card;       // flat: one nesting level keeps RmlUi's data-for updates simple
    std::string overTitle, overTime;
    std::string modeHint, hintStyle, boxStyle, minimapStyle;
    std::vector<std::string> lines;   // send-drag lines as inline styles of rotated bars
    bool touch = false, showCard = true;
};

class Minimap {
public:
    void build(gfx::Device& dev, const GameState& gs, const Rules& rules);  // per match: terrain colours, texture
    void update(gfx::Device& dev, const GameState& gs, const Rules& rules, const RtsScene& scene, const std::vector<uint8_t>* vision,
                const float3 (&viewQuad)[4], bool haveQuad);
    void release(gfx::Device& dev);
    gfx::TextureHandle texture() const { return tex_; }
    int width() const { return w_; }
    int height() const { return h_; }
    float cellsPerPixel() const { return cellsPerPx_; }

private:
    gfx::TextureHandle tex_;
    int w_ = 0, h_ = 0, gridW_ = 0, gridH_ = 0;
    float cellsPerPx_ = 1;
    std::vector<uint32_t> base_, pixels_;
};

class GameClient {
public:
    void init(gfx::Device& dev);
    void frame(gfx::Device& dev, double dt);
    void event(const platform::Event& e);
    void shutdown();
    void autostart(size_t i) { startMatch(i); }
    void forceLanguage(std::string tag) { forcedLanguage_ = std::move(tag); }
    void spectate() { spectator_ = true; }

    // called from UI event listeners
    void minimapPointer(float u, float v);   // u, v in [0, 1] of the minimap image

private:
    enum class Screen { Menu, Lobby, Playing, Paused, Over };
    enum class Mode { Normal, AttackMove, Patrol, Place, Rally, Ability, Claim };

    void startMatch(size_t idx);
    void fitCamera();
    RtsModule& mod() { return *static_cast<RtsModule*>(&sim_->module()); }
    bool groundAt(float sx, float sy, float3& out) const;
    Entity pick(float sx, float sy) const;
    void selectBox(float x0, float y0, float x1, float y1, bool add);
    void selectSameType(Entity e);
    void command(uint16_t type, float sx, float sy, bool queue);
    void issue(Command c) {
        c.player = human_;
        if (!c.units.empty()) sound_.onCommand(*sim_, c.units[0], c.type);
        sim_->submit(c);
    }
    void processEvents();
    void notice(const std::string& t) { notices_.push_back({t, time_ + 2.5}); if (notices_.size() > 4) notices_.erase(notices_.begin()); }
    void cleanSelection();
    bool selectionHasGarrison() const;
    Vec3 placeCentre(float sx, float sy, bool& ok);
    void setScreen(Screen s);
    void toMenu();

    // audio (main.cpp)
    void initAudio();
    void updateAudio(double dt);
    void menuMusic() { if (!audio_.playMusic("menu")) audio_.stopMusic(); }
    void saveAudioSettings();

    // ClientUi.cpp
    void initUi();
    void shutdownUi();
    void updateUi();
    void buildTopBar(UiState& s);
    void buildCommandCard(UiState& s);
    void buildOverlays(UiState& s);
    void loadDocuments();
    // localization (presentation only: rules_ is the display copy, the sim loads its own rules)
    void applyLanguage(const std::string& tag, bool save);
    std::string tr(std::string_view key, const i18n::Args& a = {}) const { return loc_.tr(key, a); }

    // Lobby.cpp
    void netAction(const std::string& what, int arg);
    void updateNet(double dt, bool& advanced);
    void startNetMatch();
    void leaveNetMatch();
    void buildLobby(UiState& s);
    void describeNetMaps();
    void loadNetSettings();
    void saveNetSettings() const;
    // disabled buttons get an empty callback: clicks on them do nothing
    int addAction(std::function<void()> fn, bool enabled = true) { nextActions_.push_back(enabled ? std::move(fn) : nullptr); return int(nextActions_.size()) - 1; }

    gfx::Device* dev_ = nullptr;
    ImGuiLayer overlay_;      // world-space overlays of RtsView (health bars) still draw through ImGui
    ui::UiLayer ui_;
    Rml::DataModelHandle model_;
    Rml::ElementDocument* menuDoc_ = nullptr;
    Rml::ElementDocument* hudDoc_ = nullptr;
    Rml::ElementDocument* lobbyDoc_ = nullptr;
    bool reloadDocs_ = false;
    std::unique_ptr<Rml::EventListener> minimapListener_;
    Rml::Element* minimapEl_ = nullptr;
    UiState ui_state_, bound_;
    std::vector<std::function<void()>> actions_, nextActions_;
    int shadowQuality_ = 0, boundShadowQuality_ = 0;
    int volume_ = 100, boundVolume_ = 100, musicVolume_ = 100, boundMusicVolume_ = 100;  // percent, pause menu
    int fxQuality_ = 0, boundFxQuality_ = 0;   // pause menu: particles off/low/medium/high
    bool uiOwnsPointer_ = false;
    Minimap minimap_;

    Renderer3D r3d_;
    RtsView view_;
    ClientFx fx_;                         // particle effects (ClientFx.cpp)
    audio::AudioEngine audio_;
    RtsAudio sound_;
    std::vector<Entity> heardSelection_;  // selection when the last select sound played
    RTSCamera cam_;
    Json manifest_, rulesJson_;
    std::vector<MapEntry> maps_;
    std::string gameName_ = "RTSEngine";
    Rules rules_;
    i18n::Localization loc_;
    i18n::GameLanguages langs_;
    std::vector<std::string> langNames_;
    std::string forcedLanguage_;

    // online play
    std::vector<net::MapInfo> netMaps_;
    std::string netAddress_ = "127.0.0.1", netName_, netError_, chatInput_, roomName_;
    std::vector<std::string> chat_;
    uint64_t netKey_ = 0;
    bool netMatch_ = false, prevSpectator_ = false;

    Screen screen_ = Screen::Menu;
    std::unique_ptr<Simulation> sim_;
    std::unique_ptr<net::NetClient> net_;     // after sim_: destroyed first, so it never outlives the sim it drives
    std::vector<RtsAI> bots_;
    RtsScene scene_;
    uint8_t human_ = 1;
    bool spectator_ = false;
    int difficulty_ = 1;
    size_t mapIndex_ = 0;
    double time_ = 0;
    std::string error_;
    std::vector<Notice> notices_;

    // input
    std::vector<Entity> selection_;
    std::array<std::vector<Entity>, 10> groups_;
    Mode mode_ = Mode::Normal;
    Entity placeProducer_;   // sidebar placement: structure comes from this producer
    int abilityIndex_ = -1;
    int placeType_ = -1;
    int sendPercent_ = 50;
    bool boxing_ = false, panning_ = false, sendDrag_ = false;
    float downX_ = 0, downY_ = 0, mouseX_ = -1, mouseY_ = -1;
    double lastClickTime_ = 0;
    Entity hover_;
    bool keys_[int(platform::Key::Count)] = {};
    std::map<int64_t, std::pair<float, float>> touches_;
    float pinchDist_ = 0;
    bool touchMoved_ = false;
    std::vector<std::pair<float3, double>> markers_;
};

} // namespace rts::client
