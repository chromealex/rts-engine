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
// F8:     RmlUi debugger. The UI itself is data: engine/assets/ui/*.rml (see ClientUi.cpp).
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ClientSplash.h"
#include "GameClient.h"
#include "modules/rts/RtsLocalize.h"
#include "modules/rts/RtsTiles.h"
#include "modules/rts/view/RtsCamera.h"

using namespace rts;
using platform::Event;
using platform::EventType;
using platform::Key;
using i18n::Args;

namespace rts::client {

GameClient* g_client = nullptr;

// ------------------------------------------------------------------ setup

void GameClient::init(gfx::Device& dev) {
    for (int i = 0; i < kInitSteps; ++i) initStep(dev, i);
}

float GameClient::initWeight(int step) {
    static const float w[kInitSteps] = {0.15f, 0.15f, 0.35f, 0.1f, 0.1f, 0.15f};   // view: models, looks, terrain
    return step >= 0 && step < kInitSteps ? w[step] : 0.0f;
}

void GameClient::initStep(gfx::Device& dev, int step) {
    switch (step) {
    case 0: {   // manifest, rules, language
        dev_ = &dev;
        { // settings (language, online name) are saved here; platforms do not create it
            std::error_code ec;
            std::filesystem::create_directories(platform::userDataDir(), ec);
        }
        std::string text;
        try {
            if (!platform::readContentFile("game.json", text)) throw std::runtime_error("game.json not found in content");
            manifest_ = Json::parse(text);
            gameName_ = manifest_.getStr("name", gameName_);
            if (!platform::readContentFile(manifest_.getStr("rules", "rules.json"), text)) throw std::runtime_error("rules not found");
            rulesJson_ = Json::parse(text);
            langs_ = i18n::readGameLanguages(manifest_);
            applyLanguage(!forcedLanguage_.empty() ? forcedLanguage_
                                                   : i18n::chooseLanguage(langs_, i18n::readSavedLanguage(platform::userDataDir())), false);
        } catch (const std::exception& e) {
            error_ = e.what();
        }
        platform::setTitle(gameName_);
        loadNetSettings();
        break;
    }
    case 1:     // renderer
        overlay_.init(dev, "fonts/ui.ttf", 17.0f);
        if (!r3d_.init(dev)) error_ = "renderer: " + dev.lastError();
        r3d_.setShadowQuality(Renderer3D::defaultQualityFor(dev, platform::isTouchDevice()));
        r3d_.setMaterialQuality(Renderer3D::defaultMaterialQualityFor(dev, platform::isTouchDevice()));
        r3d_.lights.setQuality(rts::render::LightSystem::defaultQualityFor(dev, platform::isTouchDevice()));
        break;
    case 2: view_.init(dev, rules_); break;     // models, textures, atlases of every type
    case 3: fx_.init(dev, rules_); break;
    case 4: initAudio(); break;
    case 5: initUi(); break;
    default: break;
    }
}

void GameClient::initAudio() {
    audio::AudioConfig ac;
    ac.maxVoices = platform::isTouchDevice() ? 24 : 32;
    if (const char* mode = std::getenv("RTS_AUDIO")) { // off | null (audio thread without hardware, CI)
        ac.openDevice = std::string(mode) != "off";
        ac.nullBackend = std::string(mode) == "null";
    }
    if (const char* wav = std::getenv("RTS_AUDIO_CAPTURE")) ac.capturePath = wav;
    audio_.init(ac);
    std::ifstream settings(platform::userDataDir() + "/audio.cfg");
    if (settings) audio_.loadSettings(std::string(std::istreambuf_iterator<char>(settings), {}));
    volume_ = boundVolume_ = int(std::lround(audio_.masterVolume() * 100));
    musicVolume_ = boundMusicVolume_ = int(std::lround(audio_.volume(audio::Bus::Music) * 100));
    std::string text;
    if (platform::readContentFile(manifest_.getStr("audio", "audio.json"), text)) { // a game without audio.json is silent
        try {
            Json bank = Json::parse(text);
            std::string err;
            if (!audio_.loadBank(bank, platform::readContentFile, &err) || !sound_.init(audio_, rules_, bank, &err)) error_ = err;
            for (auto& w : sound_.warnings()) std::fprintf(stderr, "audio: %s\n", w.c_str());
        } catch (const std::exception& e) {
            error_ = std::string("audio.json: ") + e.what();
        }
    }
    menuMusic();
}

void GameClient::saveAudioSettings() {
    std::ofstream f(platform::userDataDir() + "/audio.cfg");
    if (f) f << audio_.saveSettings();
}

// After the simulation advanced: sounds of what happened, acknowledgements, listener at the camera.
void GameClient::updateAudio(double dt) {
    if (sim_) {
        if (screen_ == Screen::Playing) sound_.update(*sim_, dt);
        if (selection_ != heardSelection_) { // a newly selected own unit answers
            Entity first = selection_.empty() ? Entity{} : selection_[0];
            auto* own = first.valid() ? sim_->world().tryGet<Owner>(first) : nullptr;
            if (own && own->player == human_ && std::find(heardSelection_.begin(), heardSelection_.end(), first) == heardSelection_.end())
                sound_.onSelect(*sim_, first);
            heardSelection_ = selection_;
        }
        const float3 eye = cam_.eye();
        const float target[3] = {cam_.target.x, cam_.target.y, cam_.target.z}, eyeP[3] = {eye.x, eye.y, eye.z};
        audio_.setListener(RtsAudio::cameraListener(target, eyeP));
    }
    if (volume_ != boundVolume_ || musicVolume_ != boundMusicVolume_) { // changed from the pause menu
        audio_.setMasterVolume(float(std::clamp(volume_, 0, 100)) / 100);
        audio_.setVolume(audio::Bus::Music, float(std::clamp(musicVolume_, 0, 100)) / 100);
        boundVolume_ = volume_;
        boundMusicVolume_ = musicVolume_;
        saveAudioSettings();
    }
    audio_.update(dt);
}

void GameClient::toMenu() {
    sound_.detach();
    fx_.stopMatch();
    audio_.stopAll(0.2f);
    sim_.reset();
    setScreen(Screen::Menu);
    menuMusic();
}


// Display copy of the rules and every data text in the chosen language. Safe at any time: a running
// simulation has its own Rules and never sees translated names.
void GameClient::applyLanguage(const std::string& tag, bool save) {
    auto reader = [](const std::string& path, std::string& out) { return platform::readContentFile(path, out); };
    try {
        i18n::loadGameLocalization(loc_, reader, langs_, tag);
    } catch (const std::exception& e) {
        error_ = e.what();
        loc_.clear();
        loc_.setLanguage(langs_.source);
    }
    if (save) i18n::saveLanguage(platform::userDataDir(), loc_.language());
    langNames_.clear();
    for (auto& l : langs_.available) langNames_.push_back(i18n::languageSelfName(reader, l));
    gameName_ = loc_.trOr("game.name", manifest_.getStr("name", "RTSEngine"));
    platform::setTitle(gameName_);
    rules_ = loadRules(rulesJson_);
    localizeRules(rules_, loc_);
    maps_.clear();
    for (auto& m : manifest_["maps"].items()) {
        std::string file = m.getStr("file");
        maps_.push_back({file, loc_.trOr("map." + mapStem(file) + ".title", m.getStr("title", file))});
    }
    if (!netMaps_.empty()) describeNetMaps(); // localized titles
    ui_.setKeyboardLanguage(loc_.language());
}

void GameClient::shutdown() {
    net_.reset(); // detaches from the simulation before it goes away
    fx_.shutdown();
    sound_.detach();
    audio_.shutdown();
    view_.shutdown(*dev_);
    r3d_.shutdown();
    shutdownUi();
    minimap_.release(*dev_);
    overlay_.shutdown();
}

void GameClient::startMatch(size_t idx) {
    if (idx >= maps_.size() || netMatch_) return;
    std::string text;
    if (!platform::readContentFile(maps_[idx].file, text)) { error_ = "map not found: " + maps_[idx].file; return; }
    Json map;
    try {
        map = Json::parse(text);
        std::string terr; // 2D tile maps: the tileset decides cell kinds / levels / fields
        if (!applyTilesToMap(map, [](const std::string& p, std::string& o) { return platform::readContentFile(p, o); }, &terr)) throw std::runtime_error(terr);
        SimConfig cfg;
        cfg.seed = uint64_t(std::time(nullptr));
        if (const char* sd = std::getenv("RTS_SEED")) cfg.seed = uint64_t(std::strtoull(sd, nullptr, 10));   // testing: reproducible matches
        sim_ = std::make_unique<Simulation>(cfg, std::make_unique<RtsModule>());
        sim_->setup(rulesJson_, map);
        view_.lights().configure(manifest_["lighting"], map["lighting"], map["lights"]); // day/night, map lights
        quality_.reset(int(r3d_.shadowQuality()), int(r3d_.lights.quality()));
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
    view_.setTiles(map["tiles"]);
    {   // generated maps (map / world body "generate"): the view reads the map the simulation was built from
        const Json& vm = mod().generatedMap() ? *mod().generatedMap() : map;
        // planets: bodies / heights / cells from the simulation's Surface (visual keys from the map); flat: unchanged
        if (mod().surface(*sim_)) view_.setSimWorld(mod().state(*sim_).surface, mod().state(*sim_).grid, vm);
        else view_.setWorld(vm);
        view_.setTerrainMaterial(vm["terrainMaterial"], mod().state(*sim_).grid.w, mod().state(*sim_).grid.h);
    }
    view_.buildTerrain(*dev_, mod().state(*sim_).grid);
    if (const char* ff = std::getenv("RTS_FAST_FORWARD")) { // testing (screenshots): bots play N game seconds first
        const uint32_t ticks = uint32_t(std::max(0.0, std::atof(ff)) * sim_->tickRate());
        std::vector<RtsAI> ffBots = bots_;   // the human slot is played by a bot too while fast-forwarding
        if (!spectator_) ffBots.emplace_back(human_, sim_->config().seed, AIConfig{});
        for (uint32_t i = 0; i < ticks && sim_->result() == 0; ++i) { for (auto& b : ffBots) b.update(*sim_); sim_->step(); }
        bots_.assign(ffBots.begin(), ffBots.begin() + long(bots_.size()));
    }
    sceneFromSim(*sim_, scene_, 0, spectator_ ? 0 : human_);
    fitCamera();
    if (planet()) minimap_.buildPlanet(*dev_, view_); else minimap_.build(*dev_, mod().state(*sim_), rules_);
    ui_.setDynamicTexture("minimap", minimap_.texture(), minimap_.width(), minimap_.height());
    mode_ = Mode::Normal;
    setScreen(Screen::Playing);
    audio_.stopAll(0.1f);
    sound_.attach(*sim_, spectator_ ? 0 : human_, maps_[idx].file);
    fx_.startMatch(*sim_, spectator_ ? 0 : human_, view_, mod().state(*sim_).grid);
    heardSelection_.clear();
}

void GameClient::fitCamera() {
    if (planet()) { fitPlanetCamera(); return; }
    auto& g = mod().state(*sim_).grid;
    float3 c{g.w * 0.5f, 0, g.h * 0.5f};
    bool small = std::max(g.w, g.h) <= 48;
    if (!small) for (auto& o : scene_.objects) if (o.owner == human_ && rules_.types[o.type].structure) { c = o.pos; break; }
    cam_.target = {c.x, 0, c.z};
    cam_.pitch = 0.95f; cam_.yaw = 0;
    cam_.maxDistance = std::max(40.0f, float(std::max(g.w, g.h)) * 1.3f);
    float aspect = platform::windowHeight() > 0 ? platform::windowWidth() / platform::windowHeight() : 1.6f;
    cam_.distance = small ? std::max(g.w / aspect, float(g.h)) * 1.05f + 4 : 24.0f;
    applyCameraSetup(cameraSetupFrom(manifest_), cam_, float(platform::framebufferHeight())); // game.json "camera" (2D games)
}

// ------------------------------------------------------------------ picking / selection

bool GameClient::groundAt(float sx, float sy, float3& out) const {
    if (planet()) { // planet picking -> sim-space point (anchor(body) + body-local, RtsSurface.h)
        int body; float3 local;
        if (!view_.pickPlanet(cam_, sx, sy, platform::windowWidth(), platform::windowHeight(), body, local)) return false;
        out = RtsView::bodyToSim(body, local);
        return true;
    }
    return cam_.screenToGround(sx, sy, platform::windowWidth(), platform::windowHeight(), out);
}

Entity GameClient::pick(float sx, float sy) const {
    float3 g;
    if (!groundAt(sx, sy, g)) return kNullEntity;
    Entity best; float bestD = 1e9f;
    for (auto& o : scene_.objects) {
        if (!o.visible || o.isProjectile) continue;
        const UnitType& t = rules_.types[o.type];
        if (view_.spriteHit(cam_, o, static_cast<RtsModule&>(sim_->module()).state(*sim_).grid, sx, sy, platform::windowWidth(), platform::windowHeight())) {
            if (bestD > 0.01f) { best = o.entity; bestD = 0.01f; } // sprites stand above their ground point
            continue;
        }
        if (t.layer & kLayerAir) { // flyers are drawn above the ground: test in screen space
            float2 sp;
            if (objectScreen(o, 2.4f, sp)) {
                float d = std::hypot(sp.x - sx, sp.y - sy);
                if (d < 22 && d * 0.05f < bestD) { best = o.entity; bestD = d * 0.05f; }
            }
            continue;
        }
        float r = view_.selectionRadius(o.type) + (t.structure ? 0.2f : 0.35f);
        float d = std::hypot(g.x - o.pos.x, g.z - o.pos.z);
        if (planet()) { // same body, 3D distance between the picked ground point and the object's (body-local)
            int gb; float3 gl;
            if (o.body < 0 || !RtsView::simToBody(g, gb, gl) || gb != o.body) continue;
            d = length(gl - o.pos);
        }
        if (d < r && d < bestD) { best = o.entity; bestD = d; }
    }
    return best;
}

void GameClient::cleanSelection() {
    World& w = sim_->world();
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](Entity e) { return !w.alive(e) || !w.has<Unit>(e) || (w.get<Unit>(e).flags & kUnitInHand); }), selection_.end()); // hand: off the map
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
        if (!objectScreen(o, y, sp)) continue;
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
        if (objectScreen(o, o.pos.y, sp) && sp.x >= 0 && sp.y >= 0 &&
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
    if (planet() && groundAt(sx, sy, g)) c.point = simPoint(g);   // planets: all three coordinates
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
    if (planet()) { ok = true; return simPoint(g); }   // the sim snaps footprints on faces (snapToFootprint)
    const UnitType& t = rules_.types[size_t(placeType_)];
    float cx = std::floor(g.x) + (t.footprintW % 2 ? 0.5f : 0.0f), cz = std::floor(g.z) + (t.footprintH % 2 ? 0.5f : 0.0f);
    ok = true;
    return {Fixed::fromInt(int64_t(cx * 2)) / 2, Fixed{}, Fixed::fromInt(int64_t(cz * 2)) / 2};
}

// ------------------------------------------------------------------ input

void GameClient::event(const Event& e) {
    bool uiWants = ui_.handleEvent(e);
    if (e.type == EventType::KeyDown && e.key == Key::F8 && !e.repeat) ui_.toggleDebugger();
    // a press that started on the UI keeps the pointer until release (dragging out of a panel
    // must not start a selection box); a press that started in the world ignores the UI
    if (e.type == EventType::MouseDown || e.type == EventType::TouchBegin) uiOwnsPointer_ = uiWants;
    if (e.type == EventType::MouseMove || e.type == EventType::TouchMove) uiWants = uiWants && !boxing_ && !sendDrag_ && !panning_;
    if (e.type == EventType::MouseUp || e.type == EventType::TouchEnd || e.type == EventType::TouchCancel) uiWants = uiOwnsPointer_;
    if (!uiWants) overlay_.handleEvent(e);
    if (e.type == EventType::KeyDown || e.type == EventType::KeyUp) keys_[int(e.key)] = e.type == EventType::KeyDown;
    if (e.type == EventType::MouseMove || e.type == EventType::MouseDown) { mouseX_ = e.x; mouseY_ = e.y; }
    // mobile apps go quiet in the background; desktop keeps playing when the window loses focus
    if (e.type == EventType::FocusLost && platform::isTouchDevice()) audio_.setPaused(true);
    if (e.type == EventType::FocusGained) audio_.setPaused(false);
    if (howtoOpen_ && e.type == EventType::KeyDown && e.key == Key::Escape) { howtoOpen_ = false; return; }
    if (screen_ == Screen::Paused && e.type == EventType::KeyDown && e.key == Key::Escape && !uiWants) { setScreen(Screen::Playing); return; }
    if (screen_ != Screen::Playing || !sim_) return;
    if (planet() && planetCameraEvent(e, uiWants)) return;   // orbit camera (ClientPlanet.cpp)
    World& w = sim_->world();
    switch (e.type) {
    case EventType::MouseDown:
        if (uiWants) return;
        downX_ = e.x; downY_ = e.y;
        if (toolMouseDown(e)) return;   // skills, rect tools, hand, possession (ClientActions.cpp)
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
                    if (planet()) c.point = simPoint(g);
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
            if (mode_ != Mode::Normal) { mode_ = Mode::Normal; rectDrag_ = false; return; }
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
        if (toolMouseUp(e)) { panning_ = false; break; }
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
            if (rectMode()) { rectDrag_ = true; rectX_ = e.x; rectY_ = e.y; sendDrag_ = false; break; }
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
            if (rectDrag_) break;   // painting a rect, not panning
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
        if (rectDrag_) {
            Event up = e; up.type = EventType::MouseUp; up.button = 0;
            if (single) toolMouseUp(up); else rectDrag_ = false;
            break;
        }
        if (single) {
            if (sendDrag_) {
                if (touchMoved_) command(kCmdSmart, e.x, e.y, false);
            } else if (!touchMoved_ && !uiWants) {
                Entity t = pick(e.x, e.y);
                if (mode_ == Mode::Place || mode_ == Mode::Ability || mode_ == Mode::Claim || mode_ == Mode::Skill || mode_ == Mode::Drop) {
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
        if (e.key == Key::Escape && possessedUnit().valid() && mode_ == Mode::Normal) { Command c; c.type = kCmdPossess; issue(c); break; }
        if (e.key == Key::Escape) { if (mode_ != Mode::Normal) { mode_ = Mode::Normal; rectDrag_ = false; } else setScreen(Screen::Paused); }
        if (possessedUnit().valid()) break;   // WASD steers the possessed creature
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
            if (ev.value == 255) notice(tr("notice.need_supply"));
            else if (ev.value == 254) notice(tr("notice.no_room"));
            else if (ev.value >= 0 && size_t(ev.value) < rules_.resources.size()) notice(tr("notice.not_enough", Args().str("resource", rules_.resources[size_t(ev.value)].name)));
        } else if (ev.type == kEvBuildDone) notice(tr("notice.built", Args().str("name", rules_.types[size_t(ev.value)].name)));
        else if (ev.type == kEvReadyToPlace) notice(tr("notice.ready_to_place", Args().str("name", rules_.types[size_t(ev.value)].name)));
        else if (ev.type == kEvResearched) notice(tr("notice.researched", Args().str("name", rules_.techs[size_t(ev.value)].name)));
        else if (ev.type == kEvLevelUp) notice(tr("notice.hero_level", Args().num("level", ev.value)));
        else if (ev.type == kEvLowPower) notice(tr("notice.low_power"));
        else if (ev.type == kEvUpgraded) notice(tr("notice.upgraded", Args().str("name", rules_.types[size_t(ev.value)].name)));
        else if (ev.type == kEvRegionClaimed) notice(tr("notice.region_claimed"));
        else if (ev.type == kEvPlayerDefeated) notice(tr("notice.defeated"));
    }
}

void GameClient::frame(gfx::Device& dev, double dt) {
    time_ += dt;
    float alpha = 0;
    bool netAdvanced = false;
    updateNet(dt, netAdvanced);   // online: the network session drives the sim, even behind the pause menu
    if (netAdvanced && sim_) {
        alpha = float(sim_->alpha());
        processEvents();
        if (sim_->result() != 0 && screen_ != Screen::Over) setScreen(Screen::Over);
    }
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
        if (!ui_.textInputFocused() && !possessedUnit().valid()) {
            if (key(Key::Up) || (noSel && key(Key::W)) || (edges && mouseY_ < 3)) cam_.target += fwd * speed;
            if (key(Key::Down) || (noSel && key(Key::S)) || (edges && mouseY_ > H - 3)) cam_.target += fwd * -speed;
            if (key(Key::Right) || (noSel && key(Key::D)) || (edges && mouseX_ > W - 3)) cam_.target += right * speed;
            if (key(Key::Left) || (noSel && key(Key::A)) || (edges && mouseX_ < 3)) cam_.target += right * -speed;
        }
        auto& g = mod().state(*sim_).grid;
        if (planet()) planetCameraKeys(dt);
        else {
        cam_.target.x = std::clamp(cam_.target.x, 0.0f, float(g.w));
        cam_.target.z = std::clamp(cam_.target.z, 0.0f, float(g.h));
        }
        if (quality_.update(dt)) { // too slow for these tiers: step down (keeps the pause menu in sync)
            r3d_.setShadowQuality(ShadowQuality(quality_.shadows()));
            r3d_.lights.setQuality(rts::render::LightQuality(quality_.lights()));
            shadowQuality_ = boundShadowQuality_ = quality_.shadows();
        }

        steerPossessed();
        if (!netMatch_) {
            for (auto& b : bots_) b.update(*sim_);
            sim_->advance(dt);
            alpha = float(sim_->alpha());
            processEvents();
            if (sim_->result() != 0) {
                setScreen(Screen::Over);
                auto* me = sim_->player(human_);
                if (!spectator_) sound_.onMatchEnd(me && me->team == sim_->result());
            }
        }
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

    updateAudio(dt);
    overlay_.newFrame(dt);
    updateUi();
    gfx::PassDesc pass;
    pass.clearColor[0] = 0.55f; pass.clearColor[1] = 0.66f; pass.clearColor[2] = 0.78f;
    if (sim_) {
        if (planet()) { // orbit camera -> view-world RTSCamera; space is black
            cam_ = view_.updatePlanetCamera(scene_.seconds, float(dt), dev.framebufferHeight());
            pass.clearColor[0] = pass.clearColor[1] = pass.clearColor[2] = 0.0f;
        }
        r3d_.beginScene(cam_, dev.framebufferWidth(), dev.framebufferHeight());
        auto& gs = mod().state(*sim_);
        {   // input state for the overlays (placement footprint / build grids, held keys, modes, cursor)
            static const char* kModes[] = {"normal", "attack", "patrol", "place", "rally", "ability", "claim"};
            OverlayContext oc;
            oc.sim = sim_.get();
            oc.player = spectator_ ? 0 : human_;
            float3 g;
            if (groundAt(mouseX_, mouseY_, g)) { oc.hasCursor = true; oc.cursor = g; }
            if (mode_ == Mode::Place && placeType_ >= 0) {
                bool ok;
                Vec3 c = placeCentre(mouseX_, mouseY_, ok);
                if (ok) {
                    oc.placeType = placeType_;
                    oc.placeAt = {float(c.x.toDouble()), 0, float(c.z.toDouble())};
                    oc.placeOk = mod().canPlace(*sim_, human_, uint16_t(placeType_), c);
                }
            }
            oc.keys = keys_; oc.keyCount = int(platform::Key::Count);
            oc.mode = kModes[int(mode_)];
            oc.paused = screen_ == Screen::Paused;
            oc.over = screen_ == Screen::Over;
            view_.setOverlayContext(oc);
        }
        view_.draw(r3d_, scene_, time_, spectator_ ? nullptr : &gs.vision[human_], gs.grid);
        if (!planet()) { // particles are placed in flat map coordinates: not on planets yet
            fx_.update(*sim_, scene_, cam_, r3d_.viewProj(), dev.framebufferWidth(), dev.framebufferHeight(),
                       screen_ == Screen::Playing || screen_ == Screen::Over ? float(dt) : 0.0f);
            fx_.addLights(r3d_.lights);   // particle lights ("light" module)
        }
        if (mode_ == Mode::Place && placeType_ >= 0) { // ghost of the building to place (its footprint is the "placement" overlay)
            bool ok;
            Vec3 c = placeCentre(mouseX_, mouseY_, ok);
            if (ok) {
                bool can = mod().canPlace(*sim_, human_, uint16_t(placeType_), c);
                view_.drawGhost(r3d_, uint16_t(placeType_), {float(c.x.toDouble()), 0, float(c.z.toDouble())}, can, mod().state(*sim_).grid, time_);
            }
        }
        drawToolPreview();
        r3d_.render(pass);
        if (!planet()) fx_.render(r3d_.lighting);
        view_.drawOverlays(cam_, scene_, platform::windowWidth(), platform::windowHeight());
    } else {
        pass.clearColor[0] = 0.10f; pass.clearColor[1] = 0.12f; pass.clearColor[2] = 0.14f;
        dev.beginPass(pass);
    }
    overlay_.render();
    ui_.render();
    dev.endPass();
}

} // namespace rts::client

int rtsMain(int argc, char** argv) {
    using namespace rts::client;
    static GameClient client;
    static ClientBoot boot;   // splash / loading screen (ClientSplash.h)
    static size_t autostartMap = size_t(-1);
    g_client = &client;
    platform::AppConfig cfg;
    cfg.title = "RTSEngine";
    cfg.width = 1280; cfg.height = 760;
#ifdef RTS_SPLASH_BACKGROUND   // rtsbuild: game.json splash.background, the window colour before the first frame
    cfg.clearColor[0] = float((RTS_SPLASH_BACKGROUND >> 16) & 0xff) / 255.0f;
    cfg.clearColor[1] = float((RTS_SPLASH_BACKGROUND >> 8) & 0xff) / 255.0f;
    cfg.clearColor[2] = float(RTS_SPLASH_BACKGROUND & 0xff) / 255.0f;
#endif
    platform::AppCallbacks cb;
    cb.frame = [](gfx::Device& d, double dt) { boot.frame(d, dt); };
    cb.event = [](const Event& e) { boot.event(e); };
    cb.shutdown = [] { boot.shutdown(); };
    // --howto: open "How to play"; --map N [--spectate]: jump straight into a match (CI screenshots, quick testing); --lang tag; --size WxH
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--spectate") g_client->spectate();
        if (a == "--howto") g_client->showHowto();
        if (a == "--lang" && i + 1 < argc) g_client->forceLanguage(argv[++i]);
        // --size WxH: window size on desktops (e.g. 540x960 to look at a portrait game)
        if (a == "--size" && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { cfg.width = w; cfg.height = h; }
        }
        if (a == "--map" && i + 1 < argc) autostartMap = size_t(std::stoul(argv[++i]));
    }
    // RTS_SPLASH=off: load in the init callback (old behaviour); full: the whole splash even with --map
    ClientBoot::Mode mode = autostartMap != size_t(-1) ? ClientBoot::Mode::LoadingOnly : ClientBoot::Mode::Full;
    if (const char* s = std::getenv("RTS_SPLASH")) {
        if (std::string(s) == "off") mode = ClientBoot::Mode::Off;
        else if (std::string(s) == "full") mode = ClientBoot::Mode::Full;
    }
    cb.init = [mode](gfx::Device& d) {
        boot.init(d, *g_client, mode, [] { if (autostartMap != size_t(-1)) g_client->autostart(autostartMap); });
    };
    return platform::run(cfg, cb);
}
