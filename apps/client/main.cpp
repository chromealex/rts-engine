// Generic RTS game client. Everything game-specific comes from the project's data (game.json,
// rules.json, maps): the same executable plays a Warcraft-like, a Red-Alert-like or a
// Mushroom-Wars-like game.
//
// Mouse:  left click / drag box = select (Shift adds, double click = all of that type on screen)
//         right click = smart command (move / attack / gather / return / send garrison), Shift queues
//         A = attack-move, S = stop, H = hold, P = patrol, Esc = cancel / pause
//         Ctrl+1..9 = set control group, 1..9 = recall; WASD/arrows or screen edge = scroll, wheel = zoom
//         Garrison buildings: drag from your building to a target to send troops (percent in the HUD)
// Touch:  tap = select / command, drag = scroll (or send from your garrison building), pinch = zoom
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/gfx/Gfx.h"
#include "engine/platform/Platform.h"
#include "engine/render/ImGuiLayer.h"
#include "engine/render/Renderer3D.h"
#include "engine/sim/Simulation.h"
#include "imgui.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"
#include "modules/rts/view/RtsView.h"

using namespace rts;
using namespace rts::render;
using namespace rts::game;
using platform::Event;
using platform::EventType;
using platform::Key;

namespace {

struct MapEntry { std::string file, title; };
struct Notice { std::string text; double until; };

class GameClient {
public:
    void init(gfx::Device& dev);
    void frame(gfx::Device& dev, double dt);
    void event(const Event& e);
    void shutdown();
    void autostart(size_t i) { startMatch(i); }
    void spectate() { spectator_ = true; }

private:
    enum class Screen { Menu, Playing, Paused, Over };
    enum class Mode { Normal, AttackMove, Patrol, Place, Rally, Ability, Claim };

    void startMatch(size_t idx);
    void fitCamera();
    RtsModule& mod() { return *dynamic_cast<RtsModule*>(&sim_->module()); }
    bool groundAt(float sx, float sy, float3& out) const;
    Entity pick(float sx, float sy) const;
    void selectBox(float x0, float y0, float x1, float y1, bool add);
    void selectSameType(Entity e);
    void command(uint16_t type, float sx, float sy, bool queue);
    void issue(Command c) { c.player = human_; sim_->submit(c); }
    void processEvents();
    void notice(const std::string& t) { notices_.push_back({t, time_ + 2.5}); if (notices_.size() > 4) notices_.erase(notices_.begin()); }
    void cleanSelection();
    bool selectionHasGarrison() const;
    Vec3 placeCentre(float sx, float sy, bool& ok);

    void drawMenu();
    void drawHud();
    void drawCommandCard();
    void drawMinimap();
    void drawPause();

    gfx::Device* dev_ = nullptr;
    ImGuiLayer ui_;
    Renderer3D r3d_;
    RtsView view_;
    RTSCamera cam_;
    Json manifest_, rulesJson_;
    std::vector<MapEntry> maps_;
    std::string gameName_ = "RTSEngine";
    Rules rules_;

    Screen screen_ = Screen::Menu;
    std::unique_ptr<Simulation> sim_;
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
    bool keys_[int(Key::Count)] = {};
    std::map<int64_t, std::pair<float, float>> touches_;
    float pinchDist_ = 0;
    bool touchMoved_ = false;
    std::vector<std::pair<float3, double>> markers_;
};

GameClient* g_client = nullptr;

// ------------------------------------------------------------------ setup

void GameClient::init(gfx::Device& dev) {
    dev_ = &dev;
    std::string text;
    try {
        if (!platform::readContentFile("game.json", text)) throw std::runtime_error("game.json not found in content");
        manifest_ = Json::parse(text);
        gameName_ = manifest_.getStr("name", gameName_);
        if (!platform::readContentFile(manifest_.getStr("rules", "rules.json"), text)) throw std::runtime_error("rules not found");
        rulesJson_ = Json::parse(text);
        rules_ = loadRules(rulesJson_);
        for (auto& m : manifest_["maps"].items()) maps_.push_back({m.getStr("file"), m.getStr("title", m.getStr("file"))});
    } catch (const std::exception& e) {
        error_ = e.what();
    }
    platform::setTitle(gameName_);
    ui_.init(dev, "fonts/ui.ttf", 17.0f);
    if (!r3d_.init(dev)) error_ = "renderer: " + dev.lastError();
    r3d_.setShadowQuality(Renderer3D::defaultQualityFor(dev, platform::isTouchDevice()));
    view_.init(dev, rules_);
}

void GameClient::shutdown() {
    view_.shutdown(*dev_);
    r3d_.shutdown();
    ui_.shutdown();
}

void GameClient::startMatch(size_t idx) {
    if (idx >= maps_.size()) return;
    std::string text;
    if (!platform::readContentFile(maps_[idx].file, text)) { error_ = "map not found: " + maps_[idx].file; return; }
    try {
        Json map = Json::parse(text);
        SimConfig cfg;
        cfg.seed = uint64_t(std::time(nullptr));
        sim_ = std::make_unique<Simulation>(cfg, std::make_unique<RtsModule>());
        sim_->setup(rulesJson_, map);
    } catch (const std::exception& e) {
        error_ = e.what(); sim_.reset(); return;
    }
    mapIndex_ = idx;
    bots_.clear();
    for (auto& p : sim_->players()) {
        if (p.slot == human_ && !spectator_) continue;
        AIConfig c;
        c.thinkTicks = difficulty_ == 0 ? 30 : difficulty_ == 1 ? 15 : 8;
        c.firstAttackArmy = difficulty_ == 0 ? 12 : difficulty_ == 1 ? 8 : 6;
        bots_.emplace_back(p.slot, sim_->config().seed, c);
    }
    selection_.clear();
    for (auto& g : groups_) g.clear();
    view_.buildTerrain(*dev_, mod().state(*sim_).grid);
    sceneFromSim(*sim_, scene_, 0, spectator_ ? 0 : human_);
    fitCamera();
    mode_ = Mode::Normal;
    screen_ = Screen::Playing;
}

void GameClient::fitCamera() {
    auto& g = mod().state(*sim_).grid;
    float3 c{g.w * 0.5f, 0, g.h * 0.5f};
    bool small = std::max(g.w, g.h) <= 48;
    if (!small) for (auto& o : scene_.objects) if (o.owner == human_ && rules_.types[o.type].structure) { c = o.pos; break; }
    cam_.target = {c.x, 0, c.z};
    cam_.pitch = 0.95f; cam_.yaw = 0;
    cam_.maxDistance = std::max(40.0f, float(std::max(g.w, g.h)) * 1.3f);
    float aspect = platform::windowHeight() > 0 ? platform::windowWidth() / platform::windowHeight() : 1.6f;
    cam_.distance = small ? std::max(g.w / aspect, float(g.h)) * 1.05f + 4 : 24.0f;
}

// ------------------------------------------------------------------ picking / selection

bool GameClient::groundAt(float sx, float sy, float3& out) const {
    return cam_.screenToGround(sx, sy, platform::windowWidth(), platform::windowHeight(), out);
}

Entity GameClient::pick(float sx, float sy) const {
    float3 g;
    if (!groundAt(sx, sy, g)) return kNullEntity;
    Entity best; float bestD = 1e9f;
    for (auto& o : scene_.objects) {
        if (!o.visible || o.isProjectile) continue;
        const UnitType& t = rules_.types[o.type];
        if (t.layer & kLayerAir) { // flyers are drawn above the ground: test in screen space
            float2 sp;
            if (cam_.worldToScreen({o.pos.x, 2.4f, o.pos.z}, platform::windowWidth(), platform::windowHeight(), sp)) {
                float d = std::hypot(sp.x - sx, sp.y - sy);
                if (d < 22 && d * 0.05f < bestD) { best = o.entity; bestD = d * 0.05f; }
            }
            continue;
        }
        float r = view_.selectionRadius(o.type) + (t.structure ? 0.2f : 0.35f);
        float d = std::hypot(g.x - o.pos.x, g.z - o.pos.z);
        if (d < r && d < bestD) { best = o.entity; bestD = d; }
    }
    return best;
}

void GameClient::cleanSelection() {
    World& w = sim_->world();
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](Entity e) { return !w.alive(e) || !w.has<Unit>(e); }), selection_.end());
}

bool GameClient::selectionHasGarrison() const {
    for (Entity e : selection_) if (sim_->world().has<Garrison>(e)) return true;
    return false;
}

void GameClient::selectBox(float x0, float y0, float x1, float y1, bool add) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    std::vector<Entity> units, structs;
    for (auto& o : scene_.objects) {
        if (!o.visible || o.isProjectile || o.owner != human_) continue;
        float2 sp;
        float y = (rules_.types[o.type].layer & kLayerAir) ? 2.4f : 0.3f;
        if (!cam_.worldToScreen({o.pos.x, y, o.pos.z}, platform::windowWidth(), platform::windowHeight(), sp)) continue;
        if (sp.x < x0 || sp.x > x1 || sp.y < y0 || sp.y > y1) continue;
        (rules_.types[o.type].structure ? structs : units).push_back(o.entity);
    }
    if (!add) selection_.clear();
    for (Entity e : units.empty() ? structs : units)
        if (std::find(selection_.begin(), selection_.end(), e) == selection_.end()) selection_.push_back(e);
}

void GameClient::selectSameType(Entity e) {
    auto* u = sim_->world().tryGet<Unit>(e);
    if (!u) return;
    selection_.clear();
    for (auto& o : scene_.objects) {
        if (o.owner != human_ || o.isProjectile || o.type != u->type || !o.visible) continue;
        float2 sp;
        if (cam_.worldToScreen(o.pos, platform::windowWidth(), platform::windowHeight(), sp) && sp.x >= 0 && sp.y >= 0 &&
            sp.x <= platform::windowWidth() && sp.y <= platform::windowHeight())
            selection_.push_back(o.entity);
    }
}

void GameClient::command(uint16_t type, float sx, float sy, bool queue) {
    cleanSelection();
    if (selection_.empty()) return;
    Command c;
    c.type = type;
    c.flags = queue ? kCmdQueue : 0;
    c.units = selection_;
    float3 g;
    if (groundAt(sx, sy, g)) c.point = {Fixed::fromInt(int64_t(std::lround(g.x * 256))) / 256, Fixed{}, Fixed::fromInt(int64_t(std::lround(g.z * 256))) / 256};
    Entity t = pick(sx, sy);
    if (type != kCmdMove && type != kCmdPatrol) c.target = t;
    if (type == kCmdSmart && selectionHasGarrison()) {
        if (!t.valid() || !sim_->world().has<Garrison>(t)) return;
        c.type = kCmdSend; c.param = sendPercent_;
        std::vector<Entity> src;
        for (Entity e : selection_) if (e != t && sim_->world().has<Garrison>(e)) src.push_back(e);
        c.units = src;
    }
    issue(c);
    markers_.push_back({g, time_ + 0.6});
}

Vec3 GameClient::placeCentre(float sx, float sy, bool& ok) {
    ok = false;
    float3 g;
    if (placeType_ < 0 || !groundAt(sx, sy, g)) return {};
    const UnitType& t = rules_.types[size_t(placeType_)];
    float cx = std::floor(g.x) + (t.footprintW % 2 ? 0.5f : 0.0f), cz = std::floor(g.z) + (t.footprintH % 2 ? 0.5f : 0.0f);
    ok = true;
    return {Fixed::fromInt(int64_t(cx * 2)) / 2, Fixed{}, Fixed::fromInt(int64_t(cz * 2)) / 2};
}

// ------------------------------------------------------------------ input

void GameClient::event(const Event& e) {
    bool uiWants = ui_.handleEvent(e);
    if (e.type == EventType::KeyDown || e.type == EventType::KeyUp) keys_[int(e.key)] = e.type == EventType::KeyDown;
    if (e.type == EventType::MouseMove || e.type == EventType::MouseDown) { mouseX_ = e.x; mouseY_ = e.y; }
    if (screen_ != Screen::Playing || !sim_) return;
    World& w = sim_->world();
    switch (e.type) {
    case EventType::MouseDown:
        if (uiWants) return;
        downX_ = e.x; downY_ = e.y;
        if (e.button == 0) {
            if (mode_ == Mode::Place) {
                bool ok;
                Vec3 c = placeCentre(e.x, e.y, ok);
                if (ok && placeProducer_.valid()) {
                    Command cmd; cmd.type = kCmdPlace; cmd.units = {placeProducer_}; cmd.point = c;
                    issue(cmd);
                    placeProducer_ = kNullEntity;
                    mode_ = Mode::Normal;
                    return;
                }
                if (ok) {
                    Command cmd; cmd.type = kCmdBuild; cmd.param = placeType_; cmd.flags = (e.mods & platform::kModShift) ? kCmdQueue : 0;
                    for (Entity s : selection_)
                        if (auto* u = w.tryGet<Unit>(s); u && std::find(rules_.types[u->type].builds.begin(), rules_.types[u->type].builds.end(), uint16_t(placeType_)) != rules_.types[u->type].builds.end()) { cmd.units = {s}; break; }
                    cmd.point = c;
                    if (!cmd.units.empty()) issue(cmd);
                }
                if (!(e.mods & platform::kModShift)) mode_ = Mode::Normal;
                return;
            }
            if (mode_ == Mode::Ability || mode_ == Mode::Claim) {
                float3 g;
                if (groundAt(e.x, e.y, g)) {
                    Command c;
                    c.type = mode_ == Mode::Claim ? kCmdClaim : kCmdAbility;
                    c.point = {Fixed::fromRatio(int64_t(g.x * 256), 256), Fixed{}, Fixed::fromRatio(int64_t(g.z * 256), 256)};
                    if (mode_ == Mode::Ability) {
                        c.param = abilityIndex_;
                        c.target = pick(e.x, e.y);
                        for (Entity s2 : selection_) {
                            auto& st = rules_.types[w.get<Unit>(s2).type];
                            if (size_t(abilityIndex_) < st.abilities.size()) c.units.push_back(s2);
                        }
                    }
                    issue(c);
                }
                mode_ = Mode::Normal;
                return;
            }
            if (mode_ == Mode::AttackMove || mode_ == Mode::Patrol || mode_ == Mode::Rally) {
                command(mode_ == Mode::AttackMove ? kCmdAttack : mode_ == Mode::Patrol ? kCmdPatrol : kCmdRally, e.x, e.y, e.mods & platform::kModShift);
                mode_ = Mode::Normal;
                return;
            }
            Entity t = pick(e.x, e.y);
            if (t.valid() && w.has<Garrison>(t) && w.get<Owner>(t).player == human_) {
                sendDrag_ = true; // Mushroom-Wars gesture: drag from own building to a target
                if (std::find(selection_.begin(), selection_.end(), t) == selection_.end()) {
                    if (!(e.mods & platform::kModShift)) selection_.clear();
                    selection_.push_back(t);
                }
                return;
            }
            if (t.valid() && selectionHasGarrison() && w.has<Garrison>(t)) { command(kCmdSmart, e.x, e.y, false); return; }
            boxing_ = true;
        } else if (e.button == 1) {
            if (mode_ != Mode::Normal) { mode_ = Mode::Normal; return; }
            command(kCmdSmart, e.x, e.y, e.mods & platform::kModShift);
        } else {
            panning_ = true;
        }
        break;
    case EventType::MouseMove:
        hover_ = uiWants ? kNullEntity : pick(e.x, e.y);
        if (panning_) {
            float3 a, b;
            if (groundAt(downX_, downY_, a) && groundAt(e.x, e.y, b)) cam_.target += a - b;
            downX_ = e.x; downY_ = e.y;
        }
        if (sendDrag_) {
            Entity t = pick(e.x, e.y);
            if (t.valid() && w.has<Garrison>(t) && w.get<Owner>(t).player == human_ &&
                std::find(selection_.begin(), selection_.end(), t) == selection_.end()) selection_.push_back(t); // swipe-select
        }
        break;
    case EventType::MouseUp:
        if (e.button == 0) {
            if (sendDrag_) {
                Entity t = pick(e.x, e.y);
                bool moved = std::hypot(e.x - downX_, e.y - downY_) > 10;
                if (moved && t.valid() && w.has<Garrison>(t)) command(kCmdSmart, e.x, e.y, false);
                sendDrag_ = false;
            } else if (boxing_) {
                boxing_ = false;
                bool add = e.mods & platform::kModShift;
                if (std::hypot(e.x - downX_, e.y - downY_) > 6) selectBox(downX_, downY_, e.x, e.y, add);
                else {
                    Entity t = pick(e.x, e.y);
                    if (t.valid() && time_ - lastClickTime_ < 0.3 && w.get<Owner>(t).player == human_) selectSameType(t);
                    else if (t.valid()) {
                        if (!add) selection_.clear();
                        if (std::find(selection_.begin(), selection_.end(), t) == selection_.end()) selection_.push_back(t);
                    } else if (!add) selection_.clear();
                    lastClickTime_ = time_;
                }
            }
        }
        panning_ = false;
        break;
    case EventType::MouseWheel:
        if (uiWants) return;
        cam_.distance = std::clamp(cam_.distance * std::pow(0.88f, e.wheelY), cam_.minDistance, cam_.maxDistance);
        break;
    case EventType::TouchBegin:
        if (uiWants && touches_.empty()) return;
        touches_[e.touchId] = {e.x, e.y};
        if (touches_.size() == 1) {
            downX_ = mouseX_ = e.x; downY_ = mouseY_ = e.y; touchMoved_ = false;
            Entity t = pick(e.x, e.y);
            sendDrag_ = t.valid() && w.has<Garrison>(t) && w.get<Owner>(t).player == human_;
            if (sendDrag_ && std::find(selection_.begin(), selection_.end(), t) == selection_.end()) selection_ = {t};
        } else if (touches_.size() == 2) {
            sendDrag_ = false;
            auto it = touches_.begin(); auto a = it->second; auto b = (++it)->second;
            pinchDist_ = std::hypot(a.first - b.first, a.second - b.second);
            downX_ = (a.first + b.first) * 0.5f; downY_ = (a.second + b.second) * 0.5f;
        }
        break;
    case EventType::TouchMove: {
        if (!touches_.count(e.touchId)) return;
        touches_[e.touchId] = {e.x, e.y};
        if (touches_.size() == 1) {
            if (std::hypot(e.x - downX_, e.y - downY_) > 12) touchMoved_ = true;
            mouseX_ = e.x; mouseY_ = e.y;
            if (sendDrag_) {
                Entity t = pick(e.x, e.y);
                if (t.valid() && w.has<Garrison>(t) && w.get<Owner>(t).player == human_ &&
                    std::find(selection_.begin(), selection_.end(), t) == selection_.end()) selection_.push_back(t);
            } else if (touchMoved_) {
                float3 a, b;
                if (groundAt(downX_, downY_, a) && groundAt(e.x, e.y, b)) cam_.target += a - b;
                downX_ = e.x; downY_ = e.y;
            }
        } else if (touches_.size() == 2) {
            auto it = touches_.begin(); auto a = it->second; auto b = (++it)->second;
            float d = std::hypot(a.first - b.first, a.second - b.second);
            if (pinchDist_ > 1 && d > 1) cam_.distance = std::clamp(cam_.distance * pinchDist_ / d, cam_.minDistance, cam_.maxDistance);
            pinchDist_ = d;
            float cx = (a.first + b.first) * 0.5f, cy = (a.second + b.second) * 0.5f;
            float3 p0, p1;
            if (groundAt(downX_, downY_, p0) && groundAt(cx, cy, p1)) cam_.target += p0 - p1;
            downX_ = cx; downY_ = cy;
            touchMoved_ = true;
        }
        break;
    }
    case EventType::TouchEnd: case EventType::TouchCancel: {
        if (!touches_.count(e.touchId)) return;
        bool single = touches_.size() == 1 && e.type == EventType::TouchEnd;
        touches_.erase(e.touchId);
        if (single) {
            if (sendDrag_) {
                if (touchMoved_) command(kCmdSmart, e.x, e.y, false);
            } else if (!touchMoved_ && !uiWants) {
                Entity t = pick(e.x, e.y);
                if (mode_ == Mode::Place || mode_ == Mode::Ability || mode_ == Mode::Claim) {
                    Event fake = e; fake.type = EventType::MouseDown; fake.button = 0; fake.mods = 0;
                    event(fake);
                } else if (t.valid() && w.get<Owner>(t).player == human_ && !selectionHasGarrison()) {
                    if (time_ - lastClickTime_ < 0.3) selectSameType(t); else selection_ = {t};
                    lastClickTime_ = time_;
                } else if (!selection_.empty()) {
                    command(mode_ == Mode::AttackMove ? kCmdAttack : kCmdSmart, e.x, e.y, false);
                    mode_ = Mode::Normal;
                } else if (t.valid() && w.get<Owner>(t).player == human_) {
                    selection_ = {t};
                }
            }
        }
        sendDrag_ = false;
        break;
    }
    case EventType::KeyDown: {
        if (uiWants) return;
        const bool ctrl = e.mods & (platform::kModCtrl | platform::kModSuper);
        if (e.key == Key::Escape) { if (mode_ != Mode::Normal) mode_ = Mode::Normal; else screen_ = Screen::Paused; }
        if (e.key >= Key::Num1 && e.key <= Key::Num9) {
            int gi = int(e.key) - int(Key::Num0);
            if (ctrl) groups_[size_t(gi)] = selection_;
            else if (selectionHasGarrison() && gi <= 4) sendPercent_ = gi * 25; // garrison games: 1-4 = 25..100%
            else if (!groups_[size_t(gi)].empty()) selection_ = groups_[size_t(gi)];
        }
        if (!ctrl && !selection_.empty()) {
            if (e.key == Key::A) mode_ = Mode::AttackMove;
            if (e.key == Key::P) mode_ = Mode::Patrol;
            if (e.key == Key::S) { Command c; c.type = kCmdStop; c.units = selection_; issue(c); }
            if (e.key == Key::H) { Command c; c.type = kCmdHold; c.units = selection_; issue(c); }
        }
        if (e.key == Key::Space) fitCamera();
        break;
    }
    default: break;
    }
}

void GameClient::processEvents() {
    for (auto& ev : sim_->events()) {
        if (ev.player != human_ || spectator_) continue;
        if (ev.type == kEvNotEnough) {
            if (ev.value == 255) notice("Нужно больше лимита (дома / склады)");
            else if (ev.value == 254) notice("Нет места для постройки");
            else if (ev.value >= 0 && size_t(ev.value) < rules_.resources.size()) notice("Не хватает: " + rules_.resources[size_t(ev.value)].name);
        } else if (ev.type == kEvBuildDone) notice("Построено: " + rules_.types[size_t(ev.value)].name);
        else if (ev.type == kEvReadyToPlace) notice("Готово к установке: " + rules_.types[size_t(ev.value)].name);
        else if (ev.type == kEvResearched) notice("Исследовано: " + rules_.techs[size_t(ev.value)].name);
        else if (ev.type == kEvLevelUp) notice("Герой достиг уровня " + std::to_string(ev.value));
        else if (ev.type == kEvLowPower) notice("Не хватает энергии!");
        else if (ev.type == kEvUpgraded) notice("Улучшено: " + rules_.types[size_t(ev.value)].name);
        else if (ev.type == kEvRegionClaimed) notice("Регион присоединён");
        else if (ev.type == kEvPlayerDefeated) notice("Вы проиграли");
    }
}

// ------------------------------------------------------------------ UI

void GameClient::drawMenu() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(440.f, io.DisplaySize.x - 32), 0));
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(gameName_.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    if (!error_.empty()) { ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str()); ImGui::Separator(); }
    ImGui::TextUnformatted("Сложность");
    ImGui::RadioButton("Лёгкая", &difficulty_, 0); ImGui::SameLine();
    ImGui::RadioButton("Обычная", &difficulty_, 1); ImGui::SameLine();
    ImGui::RadioButton("Сложная", &difficulty_, 2);
    ImGui::Checkbox("Только наблюдать (боты против ботов)", &spectator_);
    ImGui::Spacing();
    ImGui::TextUnformatted("Карта");
    for (size_t i = 0; i < maps_.size(); ++i)
        if (ImGui::Button(maps_[i].title.c_str(), ImVec2(-1, 0))) startMatch(i);
    ImGui::Spacing();
    ImGui::TextDisabled("%s · %s", platform::name(), dev_->info().name.c_str());
    ImGui::End();
}

void GameClient::drawHud() {
    ImGuiIO& io = ImGui::GetIO();
    auto& gs = mod().state(*sim_);
    const auto& eco = gs.players[spectator_ ? 1 : human_];
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 6), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::Begin("##res", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing);
    bool firstItem = true;
    auto sep = [&] { if (!firstItem) ImGui::SameLine(0, 18); firstItem = false; };
    for (size_t r = 0; r < rules_.resources.size(); ++r) {
        sep();
        const auto& rd = rules_.resources[r];
        if (rd.kind == ResourceKind::Capacity) {
            bool low = eco.capUsed[r] > eco.capProvided[r];
            ImGui::TextColored(low ? ImVec4(1, 0.45f, 0.35f, 1) : ImVec4(1, 1, 1, 1), "%s: %lld/%lld", rd.name.c_str(),
                               (long long)eco.capUsed[r].toInt(), (long long)eco.capProvided[r].toInt());
            continue;
        }
        ImGui::Text("%s: %lld", rd.name.c_str(), (long long)eco.stock[r].toInt());
        if ((rd.kind == ResourceKind::Flow || rd.capped) && eco.cap[r].raw > 0) { ImGui::SameLine(0, 2); ImGui::TextDisabled("/%lld", (long long)eco.cap[r].toInt()); }
        if (rd.kind == ResourceKind::Flow) { ImGui::SameLine(0, 4); ImGui::TextDisabled("(%+.1f/с)", eco.income[r].toDouble()); }
    }
    if (eco.supplyCap.raw > 0 || eco.supplyUsed.raw > 0) {
        sep();
        bool full = eco.supplyUsed >= eco.supplyCap;
        ImGui::TextColored(full ? ImVec4(1, 0.5f, 0.4f, 1) : ImVec4(1, 1, 1, 1), "Лимит: %lld/%lld", (long long)eco.supplyUsed.toInt(), (long long)eco.supplyCap.toInt());
    }
    if (rules_.resources.empty()) {
        for (auto& p : sim_->players()) {
            int g = 0;
            sim_->world().each<Garrison, Owner>([&](Entity, Garrison& gg, Owner& o) { if (o.player == p.slot) g += gg.count; });
            sim_->world().each<Unit, Owner>([&](Entity en, Unit&, Owner& o) { if (o.player == p.slot && sim_->world().has<Mobile>(en)) ++g; });
            float4 c = RtsView::playerColor(p.slot);
            sep();
            ImGui::TextColored(ImVec4(c.x, c.y, c.z, 1), "%s %d%s", p.name.c_str(), g, p.alive ? "" : " (выбыл)");
        }
    }
    // mode-specific counters
    const auto& vic = gs.victory;
    if (vic.mode == VictoryMode::Tickets)
        for (size_t t = 1; t < gs.tickets.size(); ++t) { sep(); ImGui::Text("Команда %zu: %lld", t, (long long)gs.tickets[t].toInt()); }
    if (vic.mode == VictoryMode::Score)
        for (size_t t = 1; t < gs.score.size(); ++t) { sep(); float4 c = RtsView::playerColor(uint8_t(t)); ImGui::TextColored(ImVec4(c.x, c.y, c.z, 1), "%lld", (long long)gs.score[t].toInt()); }
    if (vic.mode == VictoryMode::Survive || vic.mode == VictoryMode::Score) {
        int64_t left = std::max<int64_t>(0, (vic.surviveSeconds * int64_t(sim_->tickRate())).toInt() - int64_t(sim_->tick())) / sim_->tickRate();
        sep(); ImGui::Text("Осталось %lld:%02lld", (long long)(left / 60), (long long)(left % 60));
    }
    if (uint32_t wt = mod().ticksUntilWave(*sim_)) { sep(); ImGui::Text("Волна через %u", wt / sim_->tickRate() + 1); }
    if (gs.regionCount > 0 && !spectator_) {
        int owned = 0;
        for (auto o : gs.regionOwner) if (o == human_) ++owned;
        sep(); ImGui::Text("Регионы: %d", owned);
        ImGui::SameLine();
        if (ImGui::SmallButton("Занять")) mode_ = Mode::Claim;
    }
    if (!rules_.autoBuildPool.empty() && !spectator_) {
        sep();
        auto cost = mod().autoBuildCost(*sim_, human_, rules_.autoBuildPool[0]);
        std::string label = "СТРОИТЬ";
        for (auto& c : cost) label += " " + std::to_string(c.value.toInt()) + " " + rules_.resources[c.res].name;
        if (ImGui::Button(label.c_str())) { Command c; c.type = kCmdAutoBuild; c.param = -1; issue(c); }
    }
    sep();
    ImGui::Text("%u:%02u", sim_->tick() / sim_->tickRate() / 60, sim_->tick() / sim_->tickRate() % 60);
    ImGui::SameLine(0, 18);
    if (ImGui::SmallButton("Меню")) screen_ = Screen::Paused;
    ImGui::End();

    float y = 48;
    for (auto& n : notices_) {
        if (n.until < time_) continue;
        ImVec2 ts = ImGui::CalcTextSize(n.text.c_str());
        ImGui::GetForegroundDrawList()->AddText(ImVec2(io.DisplaySize.x * 0.5f - ts.x * 0.5f, y), IM_COL32(255, 230, 140, 255), n.text.c_str());
        y += ts.y + 4;
    }

    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (boxing_ && std::hypot(mouseX_ - downX_, mouseY_ - downY_) > 6) {
        ImVec2 a(std::min(downX_, mouseX_), std::min(downY_, mouseY_)), b(std::max(downX_, mouseX_), std::max(downY_, mouseY_));
        fg->AddRectFilled(a, b, IM_COL32(120, 255, 120, 30));
        fg->AddRect(a, b, IM_COL32(120, 255, 120, 200));
    }
    if (sendDrag_) {
        for (Entity s : selection_) {
            auto* t = sim_->world().tryGet<Transform>(s);
            float2 a;
            if (t && cam_.worldToScreen({float(t->pos.x.toDouble()), 0.5f, float(t->pos.z.toDouble())}, platform::windowWidth(), platform::windowHeight(), a))
                fg->AddLine(ImVec2(a.x, a.y), ImVec2(mouseX_, mouseY_), IM_COL32(255, 255, 255, 200), 4);
        }
    }
    if (mode_ != Mode::Normal) {
        const char* m = mode_ == Mode::AttackMove ? "Атака: выберите цель" : mode_ == Mode::Patrol ? "Патруль: выберите точку"
                      : mode_ == Mode::Rally ? "Точка сбора" : mode_ == Mode::Ability ? "Способность: выберите цель"
                      : mode_ == Mode::Claim ? "Выберите соседний регион" : "Постройка: выберите место (Esc — отмена)";
        fg->AddText(ImVec2(mouseX_ + 16, mouseY_ + 8), IM_COL32(255, 255, 255, 230), m);
    }
}

void GameClient::drawCommandCard() {
    ImGuiIO& io = ImGui::GetIO();
    cleanSelection();
    World& w = sim_->world();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 8, io.DisplaySize.y - 8), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowSizeConstraints(ImVec2(260, 0), ImVec2(380, io.DisplaySize.y * 0.6f));
    ImGui::Begin("##card", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
    if (selection_.empty()) {
        ImGui::TextDisabled(platform::isTouchDevice() ? "Нажмите на свой объект" : "Выделите юнитов рамкой");
        if (ImGui::Button("Все войска", ImVec2(-1, 0))) {
            for (auto& o : scene_.objects)
                if (o.owner == human_ && !o.isProjectile && !rules_.types[o.type].structure && !rules_.types[o.type].worker && !rules_.types[o.type].weapons.empty())
                    selection_.push_back(o.entity);
        }
        ImGui::End();
        return;
    }
    std::map<uint16_t, int> counts;
    for (Entity e : selection_) counts[w.get<Unit>(e).type]++;
    bool first = true;
    for (auto& [t, n] : counts) {
        if (!first) ImGui::SameLine();
        first = false;
        if (n > 1) ImGui::Text("%s x%d", rules_.types[t].name.c_str(), n); else ImGui::Text("%s", rules_.types[t].name.c_str());
    }
    Entity e0 = selection_[0];
    const UnitType& t0 = rules_.types[w.get<Unit>(e0).type];
    if (selection_.size() == 1) {
        const Unit& u = w.get<Unit>(e0);
        if (!t0.invulnerable) ImGui::TextDisabled("Здоровье %lld / %lld", (long long)u.hp.toInt(), (long long)t0.maxHp.toInt());
        if (auto* rs = w.tryGet<ResourceStock>(e0)) ImGui::TextDisabled("Запас: %lld", (long long)rs->amount.toInt());
        if (auto* g = w.tryGet<Garrison>(e0)) ImGui::TextDisabled("Гарнизон: %d / %d", g->count, t0.garrisonMax);
    }
    if (w.get<Owner>(e0).player != human_) { ImGui::End(); return; }

    auto costText = [&](uint16_t type) {
        std::string s;
        for (auto& c : rules_.types[type].cost) s += "  " + std::to_string(c.value.toInt()) + " " + rules_.resources[c.res].name;
        if (rules_.types[type].supplyUsed.raw > 0) s += "  " + std::to_string(rules_.types[type].supplyUsed.toInt()) + " лим.";
        return s;
    };
    if (selectionHasGarrison()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Отправить:");
        for (int p : {25, 50, 75, 100}) {
            ImGui::SameLine();
            char b[8]; std::snprintf(b, sizeof b, "%d%%", p);
            if (ImGui::Selectable(b, sendPercent_ == p, 0, ImVec2(40, 0))) sendPercent_ = p;
        }
        ImGui::TextDisabled("Перетащите от здания к цели");
    }
    bool anyMobile = false, anyWeapon = false;
    std::vector<uint16_t> buildable;
    for (Entity e : selection_) {
        const UnitType& t = rules_.types[w.get<Unit>(e).type];
        if (t.speed.raw > 0) anyMobile = true;
        if (!t.weapons.empty()) anyWeapon = true;
        for (uint16_t b : t.builds) if (std::find(buildable.begin(), buildable.end(), b) == buildable.end()) buildable.push_back(b);
    }
    if (anyMobile) {
        ImGui::Separator();
        if (anyWeapon) { if (ImGui::Button("Атака (A)")) mode_ = Mode::AttackMove; ImGui::SameLine(); }
        if (ImGui::Button("Стоп (S)")) { Command c; c.type = kCmdStop; c.units = selection_; issue(c); }
        ImGui::SameLine();
        if (ImGui::Button("Держать (H)")) { Command c; c.type = kCmdHold; c.units = selection_; issue(c); }
        ImGui::SameLine();
        if (ImGui::Button("Патруль (P)")) mode_ = Mode::Patrol;
    }
    if (!buildable.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Строить:");
        for (uint16_t b : buildable) {
            bool ok = mod().canTrain(*sim_, human_, b);
            ImGui::BeginDisabled(!ok);
            std::string label = rules_.types[b].name + costText(b);
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) { mode_ = Mode::Place; placeType_ = b; placeProducer_ = kNullEntity; }
            ImGui::EndDisabled();
        }
    }
    auto reqTooltip = [&](const std::vector<Requirement>& req) {
        std::string r;
        for (auto& q : req) r += (q.tech ? rules_.techs[q.index].name : rules_.types[q.index].name) + " ";
        ImGui::SetTooltip("Требуется: %s", r.c_str());
    };
    // abilities of the selection (by index of the first unit's type)
    if (!t0.abilities.empty()) {
        ImGui::Separator();
        for (size_t i = 0; i < t0.abilities.size(); ++i) {
            const AbilityDef& a = t0.abilities[i];
            if (a.aura) { ImGui::TextDisabled("Аура: %s", a.name.c_str()); continue; }
            bool ok = mod().canCast(*sim_, e0, int(i));
            ImGui::BeginDisabled(!ok);
            std::string label = a.name;
            if (a.energyCost.raw > 0) label += "  (" + std::to_string(a.energyCost.toInt()) + " эн.)";
            ImGui::PushID(int(i) + 1000);
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
                if (a.target == TargetKind::None || a.target == TargetKind::Self) { Command c; c.type = kCmdAbility; c.param = int64_t(i); c.units = selection_; issue(c); }
                else { mode_ = Mode::Ability; abilityIndex_ = int(i); }
            }
            ImGui::PopID();
            ImGui::EndDisabled();
        }
    }
    if (auto* prod = w.tryGet<Production>(e0); prod && !(w.get<Unit>(e0).flags & kUnitUnderConstruction)) {
        ImGui::Separator();
        if (prod->ready != kNoType) {
            std::string label = "Установить: " + rules_.types[prod->ready].name;
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) { mode_ = Mode::Place; placeType_ = prod->ready; placeProducer_ = e0; }
        }
        if (t0.chargesMax > 0) ImGui::TextDisabled("Заряды: %d / %d", prod->charges, t0.chargesMax);
        for (uint16_t p : t0.produces) {
            bool ok = mod().canTrain(*sim_, human_, p);
            ImGui::BeginDisabled(!ok);
            std::string label = rules_.types[p].name + costText(p);
            ImGui::PushID(p);
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
                Command c; c.type = kCmdTrain; c.param = p;
                Entity best = e0; size_t bestQ = SIZE_MAX;
                for (Entity e : selection_)
                    if (w.get<Unit>(e).type == w.get<Unit>(e0).type)
                        if (auto* pp = w.tryGet<Production>(e); pp && pp->queue.size() < bestQ) { bestQ = pp->queue.size(); best = e; }
                c.units = {best};
                issue(c);
            }
            ImGui::PopID();
            ImGui::EndDisabled();
            if (!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) reqTooltip(rules_.types[p].requires_);
        }
        const auto& eco = mod().state(*sim_).players[human_];
        for (uint16_t k : t0.researches) {
            const TechDef& tech = rules_.techs[k];
            if (eco.researched[k]) continue;
            bool ok = !eco.researching[k] && mod().requirementsMet(*sim_, human_, tech.requires_);
            std::string label = "Исследовать: " + tech.name;
            for (auto& c : tech.cost) label += "  " + std::to_string(c.value.toInt()) + " " + rules_.resources[c.res].name;
            ImGui::BeginDisabled(!ok);
            ImGui::PushID(int(k) + 5000);
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) { Command c; c.type = kCmdResearch; c.param = k; c.units = {e0}; issue(c); }
            ImGui::PopID();
            ImGui::EndDisabled();
            if (!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) reqTooltip(tech.requires_);
        }
        for (uint16_t up : t0.upgrades) {
            std::string label = "Улучшить: " + rules_.types[up].name + costText(up);
            ImGui::PushID(int(up) + 9000);
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) { Command c; c.type = kCmdUpgrade; c.param = up; c.units = {e0}; issue(c); }
            ImGui::PopID();
        }
        for (size_t i = 0; i < prod->queue.size(); ++i) {
            const ProdItem& it = prod->queue[i];
            std::string name = it.item >= 0 ? rules_.types[size_t(it.item)].name : rules_.techs[size_t(~it.item)].name;
            if (i == 0 || t0.chargesMax > 0) ImGui::ProgressBar(float(std::min(1.0, it.progress.toDouble())), ImVec2(-1, 0), name.c_str());
        }
        for (size_t i = 0; i < prod->queue.size(); ++i) {
            const ProdItem& it = prod->queue[i];
            std::string name = it.item >= 0 ? rules_.types[size_t(it.item)].name : rules_.techs[size_t(~it.item)].name;
            if (i) ImGui::SameLine();
            ImGui::PushID(int(i));
            if (ImGui::SmallButton((name.substr(0, 6) + " x").c_str())) {
                Command c; c.type = kCmdCancelTrain; c.units = {e0}; c.param = int64_t(i); issue(c);
            }
            ImGui::PopID();
        }
        if (!t0.produces.empty() && ImGui::Button("Точка сбора")) mode_ = Mode::Rally;
    }
    {
        // structure / unit specials
        bool row = false;
        auto rowItem = [&] { if (row) ImGui::SameLine(); row = true; };
        if (t0.structure && t0.sellable && !w.has<Construction>(e0)) { rowItem(); if (ImGui::Button("Продать")) { Command c; c.type = kCmdSell; c.units = {e0}; issue(c); } }
        if (t0.deploysTo != kNoType) { rowItem(); if (ImGui::Button("Развернуть")) { Command c; c.type = kCmdDeploy; c.units = {e0}; issue(c); } }
        if (t0.members > 1) {
            rowItem();
            ImGui::Text("Бойцов: %d/%d", mod().membersAlive(*sim_, e0), t0.members);
            ImGui::SameLine();
            if (ImGui::Button("Пополнить")) { Command c; c.type = kCmdReinforce; c.units = selection_; issue(c); }
        }
        if (auto* h = w.tryGet<Hero>(e0)) { rowItem(); ImGui::Text("Уровень %d  (опыт %lld)", h->level, (long long)h->xp.toInt()); }
        if (t0.maxEnergy.raw > 0) { rowItem(); ImGui::Text("Энергия %lld/%lld", (long long)w.get<Unit>(e0).energy.toInt(), (long long)t0.maxEnergy.toInt()); }
    }
    if (w.has<Construction>(e0) && ImGui::Button("Отменить постройку")) { Command c; c.type = kCmdCancelTrain; c.units = {e0}; issue(c); }
    ImGui::End();
}

void GameClient::drawMinimap() {
    ImGuiIO& io = ImGui::GetIO();
    auto& gs = mod().state(*sim_);
    const float size = platform::isTouchDevice() ? 150.0f : 190.0f;
    float sx = size / float(std::max(gs.grid.w, gs.grid.h));
    float mw = gs.grid.w * sx, mh = gs.grid.h * sx;
    ImGui::SetNextWindowPos(ImVec2(8, io.DisplaySize.y - 8), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::Begin("##minimap", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##mm", ImVec2(mw, mh));
    if (ImGui::IsItemActive()) cam_.target = {(io.MousePos.x - o.x) / sx, 0, (io.MousePos.y - o.y) / sx};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + mw, o.y + mh), IM_COL32(60, 92, 50, 255));
    const auto* vis = spectator_ ? nullptr : &gs.vision[human_];
    int step = std::max(1, std::max(gs.grid.w, gs.grid.h) / 96);
    for (int y = 0; y < gs.grid.h; y += step)
        for (int x = 0; x < gs.grid.w; x += step) {
            uint8_t f = gs.grid.flags[size_t(gs.grid.index(x, y))];
            ImU32 c = 0;
            if (!(f & kCellGround) && (f & kCellWater)) c = IM_COL32(50, 100, 150, 255);
            else if (!(f & kCellGround)) c = IM_COL32(110, 106, 100, 255);
            uint8_t v = vis ? (*vis)[size_t(gs.grid.index(x, y))] : 2;
            if (v == 0) c = IM_COL32(12, 14, 18, 255);
            else if (v == 1) c = c ? ((c & 0x00ffffffu) | 0x99000000u) : IM_COL32(40, 60, 34, 255);
            if (c) dl->AddRectFilled(ImVec2(o.x + x * sx, o.y + y * sx), ImVec2(o.x + (x + step) * sx, o.y + (y + step) * sx), c);
        }
    for (auto& ob : scene_.objects) {
        if (!ob.visible || ob.isProjectile) continue;
        const UnitType& t = rules_.types[ob.type];
        if (ob.owner == 0 && t.resourceNode >= 0 && t.footprintW <= 1) continue; // trees: too many dots
        float4 c = ob.owner == 0 && t.resourceNode >= 0 ? float4{1, 0.85f, 0.2f, 1} : RtsView::playerColor(ob.owner);
        float r = t.structure ? 2.5f : 1.5f;
        dl->AddRectFilled(ImVec2(o.x + ob.pos.x * sx - r, o.y + ob.pos.z * sx - r), ImVec2(o.x + ob.pos.x * sx + r, o.y + ob.pos.z * sx + r),
                          ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 1)));
    }
    float W = platform::windowWidth(), H = platform::windowHeight();
    float3 c[4];
    if (groundAt(0, 0, c[0]) && groundAt(W, 0, c[1]) && groundAt(W, H, c[2]) && groundAt(0, H, c[3]))
        for (int i = 0; i < 4; ++i) dl->AddLine(ImVec2(o.x + c[i].x * sx, o.y + c[i].z * sx), ImVec2(o.x + c[(i + 1) % 4].x * sx, o.y + c[(i + 1) % 4].z * sx), IM_COL32(255, 255, 255, 200));
    ImGui::End();
}

void GameClient::drawPause() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("##pause", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
    if (screen_ == Screen::Over) {
        int res = sim_->result();
        auto* me = sim_->player(human_);
        bool won = !spectator_ && res > 0 && me && me->team == res;
        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextUnformatted(spectator_ ? "Матч окончен" : won ? "Победа!" : "Поражение");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Text("Время: %u:%02u", sim_->tick() / sim_->tickRate() / 60, sim_->tick() / sim_->tickRate() % 60);
    } else {
        ImGui::TextUnformatted("Пауза");
        if (ImGui::Button("Продолжить", ImVec2(220, 0))) screen_ = Screen::Playing;
    }
    static const char* q[] = {"Тени: выкл", "Тени: низкие", "Тени: средние", "Тени: высокие"};
    int qi = int(r3d_.shadowQuality());
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("##shadows", &qi, q, 4)) r3d_.setShadowQuality(ShadowQuality(qi));
    if (ImGui::Button("Заново", ImVec2(220, 0))) startMatch(mapIndex_);
    if (ImGui::Button("В меню", ImVec2(220, 0))) { screen_ = Screen::Menu; sim_.reset(); }
    ImGui::End();
}

void GameClient::frame(gfx::Device& dev, double dt) {
    time_ += dt;
    float alpha = 0;
    if (sim_ && screen_ == Screen::Playing) {
        // keyboard / edge scrolling
        float speed = cam_.distance * 0.9f * float(dt);
        float fx = std::sin(cam_.yaw), fz = std::cos(cam_.yaw);
        float3 fwd{-fx, 0, -fz}, right{fz, 0, -fx};
        auto key = [&](Key k) { return keys_[int(k)]; };
        float W = platform::windowWidth(), H = platform::windowHeight();
        // edge scrolling only while the cursor is inside the window (macOS reports moves outside it too)
        bool edges = !platform::isTouchDevice() && std::string(platform::name()) != "web" && mouseX_ >= 0 && mouseY_ >= 0 && mouseX_ <= W && mouseY_ <= H;
        bool noSel = selection_.empty();
        if (!ui_.wantsKeyboard()) {
            if (key(Key::Up) || (noSel && key(Key::W)) || (edges && mouseY_ < 3)) cam_.target += fwd * speed;
            if (key(Key::Down) || (noSel && key(Key::S)) || (edges && mouseY_ > H - 3)) cam_.target += fwd * -speed;
            if (key(Key::Right) || (noSel && key(Key::D)) || (edges && mouseX_ > W - 3)) cam_.target += right * speed;
            if (key(Key::Left) || (noSel && key(Key::A)) || (edges && mouseX_ < 3)) cam_.target += right * -speed;
        }
        auto& g = mod().state(*sim_).grid;
        cam_.target.x = std::clamp(cam_.target.x, 0.0f, float(g.w));
        cam_.target.z = std::clamp(cam_.target.z, 0.0f, float(g.h));

        for (auto& b : bots_) b.update(*sim_);
        sim_->advance(dt);
        alpha = float(sim_->alpha());
        processEvents();
        if (sim_->result() != 0) screen_ = Screen::Over;
    }
    if (sim_) {
        sceneFromSim(*sim_, scene_, alpha, spectator_ ? 0 : human_);
        cleanSelection();
        for (auto& o : scene_.objects) {
            o.selected = std::find(selection_.begin(), selection_.end(), o.entity) != selection_.end();
            o.hovered = o.entity == hover_;
        }
        scene_.moveMarkers.clear();
        for (auto& [p, until] : markers_) if (until > time_) scene_.moveMarkers.push_back(p);
        markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](auto& m) { return m.second < time_; }), markers_.end());
    }

    ui_.newFrame(dt);
    gfx::PassDesc pass;
    pass.clearColor[0] = 0.55f; pass.clearColor[1] = 0.66f; pass.clearColor[2] = 0.78f;
    if (sim_) {
        r3d_.beginScene(cam_, dev.framebufferWidth(), dev.framebufferHeight());
        auto& gs = mod().state(*sim_);
        view_.draw(r3d_, scene_, time_, spectator_ ? nullptr : &gs.vision[human_], gs.grid);
        if (mode_ == Mode::Place && placeType_ >= 0) { // ghost footprint of the building to place
            bool ok;
            Vec3 c = placeCentre(mouseX_, mouseY_, ok);
            if (ok) {
                const UnitType& t = rules_.types[size_t(placeType_)];
                bool can = mod().canPlace(*sim_, human_, uint16_t(placeType_), c);
                uint32_t col = can ? rgba(80, 255, 120, 110) : rgba(255, 70, 60, 110);
                float x0 = float(c.x.toDouble()) - t.footprintW * 0.5f, z0 = float(c.z.toDouble()) - t.footprintH * 0.5f;
                r3d_.triangle({x0, 0.08f, z0}, {x0, 0.08f, z0 + t.footprintH}, {x0 + t.footprintW, 0.08f, z0 + t.footprintH}, col);
                r3d_.triangle({x0, 0.08f, z0}, {x0 + t.footprintW, 0.08f, z0 + t.footprintH}, {x0 + t.footprintW, 0.08f, z0}, col);
            }
        }
        r3d_.render(pass);
        view_.drawOverlays(cam_, scene_, platform::windowWidth(), platform::windowHeight());
        drawHud();
        if (!spectator_) drawCommandCard();
        drawMinimap();
        if (screen_ == Screen::Paused || screen_ == Screen::Over) drawPause();
    } else {
        pass.clearColor[0] = 0.10f; pass.clearColor[1] = 0.12f; pass.clearColor[2] = 0.14f;
        dev.beginPass(pass);
    }
    if (screen_ == Screen::Menu) drawMenu();
    ui_.render();
    dev.endPass();
}

} // namespace

int rtsMain(int argc, char** argv) {
    static GameClient client;
    g_client = &client;
    platform::AppConfig cfg;
    cfg.title = "RTSEngine";
    cfg.width = 1280; cfg.height = 760;
    platform::AppCallbacks cb;
    cb.init = [](gfx::Device& d) { g_client->init(d); };
    cb.frame = [](gfx::Device& d, double dt) { g_client->frame(d, dt); };
    cb.event = [](const Event& e) { g_client->event(e); };
    cb.shutdown = [] { g_client->shutdown(); };
    // --map N [--spectate]: jump straight into a match (CI screenshots, quick testing)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--spectate") g_client->spectate();
        if (a == "--map" && i + 1 < argc) {
            size_t idx = size_t(std::stoul(argv[++i]));
            auto oldInit = cb.init;
            cb.init = [oldInit, idx](gfx::Device& d) { oldInit(d); g_client->autostart(idx); };
        }
    }
    return platform::run(cfg, cb);
}
