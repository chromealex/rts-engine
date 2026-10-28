// Editor: lifecycle, docking layout, menus, documents, play-test and viewport rendering.
// Window contents live in apps/editor/windows/*.cpp; the agent link in EditorAgent.cpp.
#include "Editor.h"
#include "imgui.h"
#include "imgui_internal.h" // DockBuilder

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "modules/rts/RtsTiles.h"
#include "modules/rts/view/RtsCamera.h"
#include <sstream>

namespace fs = std::filesystem;

namespace rts::editor {

using namespace render;

namespace {
// Models / textures of the open game (view blocks reference them relative to the game folder).
FileReader projectReader(const std::string& dir) {
    return [dir](const std::string& p, std::string& out) {
        std::ifstream f(dir + "/" + p, std::ios::binary);
        if (!f) return platform::readContentFile(p, out);
        std::stringstream ss; ss << f.rdbuf(); out = ss.str();
        return true;
    };
}
} // namespace

// ============================ viewport ============================

void Viewport::ensure(gfx::Device& dev, int pw, int ph) {
    pw = std::max(pw, 8); ph = std::max(ph, 8);
    if (pw == w && ph == h && color.valid()) return;
    release(dev);
    gfx::TextureDesc td;
    td.width = pw; td.height = ph;
    td.renderTarget = true;
    td.label = "editor viewport";
    color = dev.createTexture(td);
    td.format = gfx::PixelFormat::Depth;
    td.filter = gfx::Filter::Nearest;
    depth = dev.createTexture(td);
    w = pw; h = ph;
}

void Viewport::release(gfx::Device& dev) {
    if (color.valid()) dev.destroyTexture(color);
    if (depth.valid()) dev.destroyTexture(depth);
    color = depth = {};
    w = h = 0;
}

void Viewport::image(gfx::Device& dev, float wPts, float hPts) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    originX = p.x; originY = p.y;
    pointsW = wPts; pointsH = hPts;
    // GL render targets are bottom-up; Metal's are top-down
    bool flip = !dev.info().renderTargetFlipY;
    ImGui::Image(ImTextureRef(ImTextureID(color.id)), ImVec2(wPts, hPts), ImVec2(0, flip ? 1.f : 0.f), ImVec2(1, flip ? 0.f : 1.f));
    hovered = ImGui::IsItemHovered();
}

// ============================ lifecycle ============================

void Editor::init(gfx::Device& dev) {
    dev_ = &dev;
    ws_.engineDir = opt_.engineDir;
    ws_.buildTool = opt_.buildTool;
    ws_.clock = [this] { return time_; };
    std::string font = opt_.engineDir + "/engine/assets/fonts/ui.ttf";
    std::error_code fec;
    if (!fs::exists(font, fec)) font = "engine/fonts/ui.ttf";
    ui_.init(dev, font.c_str(), 16.0f);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    static std::string iniPath = platform::userDataDir() + "/editor_layout.ini";
    fs::create_directories(platform::userDataDir(), fec);
    io.IniFilename = iniPath.c_str();
    resetLayout_ = !fs::exists(iniPath, fec);
    r3d_.init(dev);
    r3d_.setShadowQuality(Renderer3D::defaultQualityFor(dev, false));
    for (auto& f : WindowRegistry::factories()) {
        windows_.push_back(f());
        windows_.back()->open = windows_.back()->openByDefault();
    }
    // registration order depends on the linker; tabs follow order()
    std::stable_sort(windows_.begin(), windows_.end(), [](auto& a, auto& b) { return a->order() < b->order(); });
    registerGuiOps();
    if (!opt_.projectDir.empty()) {
        std::string err;
        if (!ws_.openGame(opt_.projectDir, &err)) { status = "Ошибка проекта: " + err; showPicker_ = true; }
    } else showPicker_ = true;
    view_.setContentBase(ws_.project.dir.empty() ? "" : ws_.project.dir + "/");
    view_.init(dev, rules(), projectReader(ws_.project.dir));
    for (auto& w : view_.warnings()) ws_.log("view", w, true);
    if (!opt_.openMap.empty()) openMap(opt_.openMap);
    else if (doc().data.cells.empty()) newMap(48, 32);
    ws_.changes.clear();
    syncGrid();
    fitScene();
    gameCam = sceneCam;
    if (opt_.listenPort > 0) {
        if (server_.listen(opt_.listenPort)) ws_.log("editor", "Канал для агентов (MCP) открыт на порту " + std::to_string(opt_.listenPort));
        else ws_.log("editor", "Порт " + std::to_string(opt_.listenPort) + " занят: агенты подключатся к другому окну редактора", true);
    }
}

void Editor::shutdown() {
    if (!dev_) return;
    sim_.reset();
    windows_.clear(); // windows release their viewports in their destructors via update()? keep simple: textures die with the device
    view_.shutdown(*dev_);
    r3d_.shutdown();
    ui_.shutdown();
    dev_ = nullptr;
}

EditorWindow* Editor::window(const std::string& id) {
    for (auto& w : windows_) if (id == w->id()) return w.get();
    return nullptr;
}

// ============================ documents ============================

void Editor::openGame(const std::string& idOrDir) {
    std::string err;
    if (playing()) stopPlay();
    if (!ws_.openGame(idOrDir, &err)) { status = "Не удалось открыть игру: " + err; return; }
    ws_.changes.clear();
    reloadRules();
    selection = {};
    showPicker_ = false;
    fitScene();
    status = "Игра: " + ws_.project.manifest.getStr("name", idOrDir);
}

void Editor::reloadRules() {
    view_.shutdown(*dev_);
    view_.setContentBase(ws_.project.dir.empty() ? "" : ws_.project.dir + "/");
    view_.init(*dev_, rules(), projectReader(ws_.project.dir));
    for (auto& w : view_.warnings()) ws_.log("view", w, true);
    gridRevision_ = ~0ull;
    placeType = std::clamp(placeType, 0, std::max(0, int(rules().types.size()) - 1));
}

void Editor::newMap(int w, int h) {
    doc().reset(w, h);
    selection = {};
    syncGrid();
    fitScene();
    status = "Новая карта " + std::to_string(w) + "x" + std::to_string(h);
}

void Editor::openMap(const std::string& file) {
    std::string err;
    std::error_code ec;
    std::string norm = fs::weakly_canonical(fs::path(file), ec).string();
    if (doc().load(ec ? file : norm, &err)) {
        status = "Открыто: " + ws_.relative(doc().path);
        selection = {};
        syncGrid();
        fitScene();
    } else status = "Не удалось открыть: " + err;
}

bool Editor::saveMap(const std::string& file) {
    std::string err;
    std::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    if (!doc().save(file, &err)) { status = "Ошибка сохранения: " + err; return false; }
    status = "Сохранено: " + ws_.relative(file);
    return true;
}

void Editor::runBuild(const std::string& target) {
    try {
        ws_.startBuild(target, buildConfig ? "Release" : "Debug");
        if (auto* w = window("console")) w->open = true;
    } catch (const std::exception& e) { status = e.what(); }
}

// ============================ camera / helpers ============================

void Editor::fitScene() {
    const auto& d = doc().data;
    sceneCam.target = {d.w * 0.5f, 0, d.h * 0.5f};
    sceneCam.maxDistance = std::max(80.0f, float(std::max(d.w, d.h)) * 2.0f);
    sceneCam.minDistance = 3.0f;
    sceneCam.distance = std::clamp(float(std::max(d.w, d.h)) * 0.95f + 8, sceneCam.minDistance, sceneCam.maxDistance);
    sceneFlying_ = false;
    // 2D games: projection and angle of game.json "camera" (zoom stays the editor's)
    game::CameraSetup cs = game::cameraSetupFrom(ws_.project.manifest);
    sceneCam.ortho = cs.ortho;
    if (cs.present) { sceneCam.pitch = std::min(cs.pitch, 1.5707f); sceneCam.yaw = cs.yaw; sceneCam.fovY = cs.fovY; }
}

void Editor::focusScene(float x, float z, float distance) {
    sceneGoal_ = {x, 0, z};
    sceneFlying_ = true;
    if (distance > 0) sceneCam.distance = std::clamp(distance, sceneCam.minDistance, sceneCam.maxDistance);
}

void Editor::flash(float x0, float z0, float x1, float z1, double seconds) { flashes_.push_back({x0, z0, x1, z1, time_ + seconds, seconds}); }

bool Editor::groundAt(const Viewport& vp, const RTSCamera& cam, float sx, float sy, float& x, float& z) const {
    float3 g;
    if (!cam.screenToGround(sx - vp.originX, sy - vp.originY, vp.pointsW, vp.pointsH, g)) return false;
    x = g.x; z = g.z;
    return true;
}

float Editor::snapCoord(float v, int typeIdx) const {
    if (!snap || rules().types.empty()) return v;
    const auto& t = rules().types[size_t(std::clamp(typeIdx, 0, int(rules().types.size()) - 1))];
    if (t.structure) return (t.footprintW % 2) ? std::floor(v) + 0.5f : std::round(v);
    return std::round(v * 4) / 4;
}

int Editor::pickEntity(const Viewport& vp, const RTSCamera& cam, float sx, float sy) const {
    float x, z;
    if (!groundAt(vp, cam, sx, sy, x, z)) return -1;
    int best = -1;
    float bestD = 1e9f;
    const auto& d = ws_.map.data;
    for (size_t i = 0; i < d.entities.size(); ++i) {
        const auto& e = d.entities[i];
        int t = rules().findType(e.type);
        float r = t >= 0 ? const_cast<game::RtsView&>(view_).selectionRadius(uint16_t(t)) + 0.2f : 0.6f;
        float dd = std::hypot(x - e.x, z - e.z);
        if (dd < r && dd < bestD) { best = int(i); bestD = dd; }
    }
    return best;
}

int Editor::pickRoad(const Viewport& vp, const RTSCamera& cam, float sx, float sy) const {
    float x, z;
    if (!groundAt(vp, cam, sx, sy, x, z)) return -1;
    int best = -1;
    float bestD = 0.5f;
    const auto& d = ws_.map.data;
    for (size_t i = 0; i < d.roads.size(); ++i) {
        auto [a, b] = d.roads[i];
        float ax = d.entities[size_t(a)].x, az = d.entities[size_t(a)].z, bx = d.entities[size_t(b)].x, bz = d.entities[size_t(b)].z;
        float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
        float t = l2 > 0 ? std::clamp(((x - ax) * dx + (z - az) * dz) / l2, 0.0f, 1.0f) : 0.0f;
        float dd = std::hypot(ax + dx * t - x, az + dz * t - z);
        if (dd < bestD) { bestD = dd; best = int(i); }
    }
    return best;
}

// ============================ scene data ============================

void Editor::syncGrid() {
    const auto& d = doc().data;
    if (grid_.w != d.w || grid_.h != d.h) grid_.init(d.w, d.h);
    for (int i = 0; i < d.w * d.h; ++i) {
        grid_.flags[size_t(i)] = game::cellFlagsFromChar(d.cells[size_t(i)]);
        grid_.height[size_t(i)] = d.heights[size_t(i)];
        grid_.level[size_t(i)] = d.levels[size_t(i)];
        grid_.region[size_t(i)] = d.regions[size_t(i)] == '.' ? int16_t(-1) : int16_t(d.regions[size_t(i)]);
        int8_t fr = d.fieldRes[size_t(i)];
        grid_.fieldRes[size_t(i)] = fr >= 0 ? int16_t(std::max(0, rules().findResource(d.fieldResources[size_t(fr)]))) : int16_t(-1);
        grid_.fieldAmt[size_t(i)] = Fixed::fromInt(d.fieldAmt[size_t(i)]);
    }
    view_.setTiles(d.extra["tiles"]);
    view_.buildTerrain(*dev_, grid_);
    gridRevision_ = doc().revision;
}

void Editor::buildEditScene(game::RtsScene& out) const {
    const auto& d = ws_.map.data;
    out.objects.clear();
    out.roads.clear();
    out.moveMarkers.clear();
    const int sel = selectedEntity();
    for (size_t i = 0; i < d.entities.size(); ++i) {
        const auto& e = d.entities[i];
        int t = rules().findType(e.type);
        if (t < 0) continue;
        game::SceneObject o;
        o.type = uint16_t(t);
        o.owner = e.owner < 0 ? uint8_t(game::kHostileNeutral) : uint8_t(std::clamp(e.owner, 0, 16));
        o.pos = {e.x, 0, e.z};
        if (rules().types[size_t(t)].garrisonMax > 0) o.garrison = e.garrison >= 0 ? e.garrison : 0;
        o.selected = int(i) == sel || int(i) == roadStart;
        out.objects.push_back(o);
    }
    for (auto [a, b] : d.roads) out.roads.push_back({{d.entities[size_t(a)].x, 0, d.entities[size_t(a)].z}, {d.entities[size_t(b)].x, 0, d.entities[size_t(b)].z}});
}

void Editor::drawEditOverlays3D() {
    const auto& d = doc().data;
    const float W = float(d.w), H = float(d.h);
    uint32_t border = rgba(255, 255, 255, 120);
    r3d_.line({0, 0.05f, 0}, {W, 0.05f, 0}, border); r3d_.line({W, 0.05f, 0}, {W, 0.05f, H}, border);
    r3d_.line({W, 0.05f, H}, {0, 0.05f, H}, border); r3d_.line({0, 0.05f, H}, {0, 0.05f, 0}, border);
    auto cellQuad = [&](float x0, float z0, float x1, float z1, uint32_t col, float y) {
        r3d_.triangle({x0, y, z0}, {x0, y, z1}, {x1, y, z1}, col);
        r3d_.triangle({x0, y, z0}, {x1, y, z1}, {x1, y, z0}, col);
    };
    for (int y = 0; y < d.h; ++y)
        for (int x = 0; x < d.w; ++x) {
            size_t i = d.idx(x, y);
            bool field = d.fieldRes[i] >= 0 && d.fieldAmt[i] > 0, region = showRegions && d.regions[i] != '.', ramp = showLevels && d.cells[i] == '/';
            if (!field && !region && !ramp) continue;
            float gy = view_.heightAt(grid_, x + 0.5f, y + 0.5f) + 0.06f;
            if (field) {
                uint32_t a = uint32_t(60 + d.fieldAmt[i] * 18);
                uint32_t col = d.fieldRes[i] % 3 == 0 ? rgba(240, 170, 40, uint8_t(a)) : d.fieldRes[i] % 3 == 1 ? rgba(80, 220, 255, uint8_t(a)) : rgba(230, 90, 220, uint8_t(a));
                cellQuad(x + 0.08f, y + 0.08f, x + 0.92f, y + 0.92f, col, gy);
            }
            if (region) {
                char c = d.regions[i];
                uint32_t col = rgba(uint8_t(80 + (c * 47) % 175), uint8_t(80 + (c * 91) % 175), uint8_t(80 + (c * 23) % 175), 230);
                if (d.inside(x + 1, y) && d.regions[d.idx(x + 1, y)] != c) r3d_.line({x + 1.0f, gy + 0.02f, float(y)}, {x + 1.0f, gy + 0.02f, y + 1.0f}, col);
                if (d.inside(x, y + 1) && d.regions[d.idx(x, y + 1)] != c) r3d_.line({float(x), gy + 0.02f, y + 1.0f}, {x + 1.0f, gy + 0.02f, y + 1.0f}, col);
            }
            if (ramp) cellQuad(x + 0.2f, y + 0.2f, x + 0.8f, y + 0.8f, rgba(255, 255, 255, 60), gy);
        }
    if (showAreas)
        for (auto& [name, a] : d.extra["areas"].members()) {
            float x0 = float(a[0].toDouble()), z0 = float(a[1].toDouble()), x1 = x0 + float(a[2].toDouble(1)), z1 = z0 + float(a[3].toDouble(1));
            bool sel = selection.kind == Selection::Area && selection.key == name;
            uint32_t c = sel ? rgba(255, 255, 255, 255) : rgba(255, 220, 80, 200);
            r3d_.line({x0, 0.3f, z0}, {x1, 0.3f, z0}, c); r3d_.line({x1, 0.3f, z0}, {x1, 0.3f, z1}, c);
            r3d_.line({x1, 0.3f, z1}, {x0, 0.3f, z1}, c); r3d_.line({x0, 0.3f, z1}, {x0, 0.3f, z0}, c);
            if (sel) cellQuad(x0, z0, x1, z1, rgba(255, 220, 80, 50), 0.3f);
        }
    for (size_t p = 0; p < d.players.size(); ++p) {
        if (!d.players[p].hasStart) continue;
        float4 c = game::RtsView::playerColor(int(p + 1));
        float sx = d.players[p].startX, sz = d.players[p].startZ, gy = view_.heightAt(grid_, sx, sz);
        r3d_.circleXZ({sx, 0, sz}, 1.2f, rgbaf({c.x, c.y, c.z, 1}), 24, gy + 0.15f);
        r3d_.line({sx, gy, sz}, {sx, gy + 2.5f, sz}, rgbaf({c.x, c.y, c.z, 1}));
    }
    for (auto& f : flashes_) {
        float k = float(std::clamp((f.until - time_) / f.length, 0.0, 1.0));
        uint32_t c = rgba(120, 220, 255, uint8_t(40 + 150 * k));
        float y = 0.35f + (1 - k) * 0.4f;
        cellQuad(f.x0, f.z0, f.x1, f.z1, rgba(120, 220, 255, uint8_t(70 * k)), y);
        r3d_.line({f.x0, y, f.z0}, {f.x1, y, f.z0}, c); r3d_.line({f.x1, y, f.z0}, {f.x1, y, f.z1}, c);
        r3d_.line({f.x1, y, f.z1}, {f.x0, y, f.z1}, c); r3d_.line({f.x0, y, f.z1}, {f.x0, y, f.z0}, c);
    }
}

void Editor::renderScene(Viewport& vp, const RTSCamera& cam, bool editOverlays) {
    if (!vp.color.valid()) return;
    if (playing()) { renderGame(vp, cam); return; }
    buildEditScene(scene_);
    r3d_.beginScene(cam, vp.w, vp.h);
    view_.draw(r3d_, scene_, time_, nullptr, grid_);
    if (editOverlays) drawEditOverlays3D();
    gfx::PassDesc pass;
    pass.colorTarget = vp.color;
    pass.depthTarget = vp.depth;
    pass.clearColor[0] = 0.13f; pass.clearColor[1] = 0.15f; pass.clearColor[2] = 0.17f;
    r3d_.render(pass);
    dev_->endPass();
    view_.drawOverlays(cam, scene_, vp.pointsW, vp.pointsH, vp.originX, vp.originY, ImGui::GetWindowDrawList());
}

void Editor::renderGame(Viewport& vp, const RTSCamera& cam) {
    if (!vp.color.valid() || !sim_) return;
    auto& gs = simModule()->state(*sim_);
    if (gs.fieldsDirty || !playTerrainBuilt_) { view_.buildTerrain(*dev_, gs.grid); gs.fieldsDirty = false; playTerrainBuilt_ = true; }
    game::RtsScene sc;
    game::sceneFromSim(*sim_, sc, float(sim_->alpha()), uint8_t(human_));
    for (auto& o : sc.objects) o.selected = std::find(gameSelection.begin(), gameSelection.end(), o.entity) != gameSelection.end();
    r3d_.beginScene(cam, vp.w, vp.h);
    view_.draw(r3d_, sc, time_, human_ ? &gs.vision[size_t(human_)] : nullptr, gs.grid);
    gfx::PassDesc pass;
    pass.colorTarget = vp.color;
    pass.depthTarget = vp.depth;
    pass.clearColor[0] = 0.55f; pass.clearColor[1] = 0.66f; pass.clearColor[2] = 0.78f;
    r3d_.render(pass);
    dev_->endPass();
    view_.drawOverlays(cam, sc, vp.pointsW, vp.pointsH, vp.originX, vp.originY, ImGui::GetWindowDrawList());
}

// Agent edits: fly the scene camera there and flash the area.
void Editor::applyChanges() {
    if (ws_.changes.empty()) return;
    bool rulesChanged = false, projectChanged = false;
    for (auto& c : ws_.changes) {
        if (c.kind == Change::Rules) rulesChanged = true;
        if (c.kind == Change::Project) projectChanged = true;
        if (c.x1 >= c.x0 && c.y1 >= c.y0) {
            flash(float(c.x0), float(c.y0), float(c.x1 + 1), float(c.y1 + 1));
            if (followAgent && c.kind != Change::Map) focusScene((c.x0 + c.x1 + 1) * 0.5f, (c.y0 + c.y1 + 1) * 0.5f);
        }
        if (c.entity >= 0 && c.kind == Change::Entity) selectEntity(c.entity);
    }
    if (projectChanged || rulesChanged) reloadRules();
    if (projectChanged) { fitScene(); selection = {}; }
    ws_.changes.clear();
    if (selectedEntity() >= int(doc().data.entities.size())) selection = {};
}

// ============================ play-test ============================

void Editor::startPlay(int humanSlot) {
    auto issues = doc().validate(rules(), ws_.project.rulesJson);
    for (auto& i : issues) if (i.error) { status = "Нельзя запустить: " + i.text; return; }
    try {
        SimConfig cfg;
        cfg.seed = uint64_t(std::time(nullptr));
        sim_ = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
        Json mapJson = doc().toJson();
        std::string terr, dir = ws_.project.dir;
        auto read = [&dir](const std::string& p, std::string& out) {
            std::ifstream f(dir + "/" + p, std::ios::binary);
            if (!f) return false;
            out.assign(std::istreambuf_iterator<char>(f), {});
            return true;
        };
        if (!game::applyTilesToMap(mapJson, read, &terr)) throw std::runtime_error(terr);
        sim_->setup(ws_.project.rulesJson, mapJson);
    } catch (const std::exception& e) {
        status = std::string("Ошибка запуска: ") + e.what();
        sim_.reset();
        return;
    }
    human_ = humanSlot;
    bots_.clear();
    for (auto& p : sim_->players()) if (p.slot != human_) bots_.emplace_back(p.slot, sim_->config().seed);
    playPaused = false;
    playTerrainBuilt_ = false;
    playHistory.clear();
    gameSelection.clear();
    gameCam = sceneCam;
    gameCam.pitch = 0.95f;
    if (human_ > 0) {
        const auto& d = doc().data;
        for (auto& e : d.entities) if (e.owner == human_) { gameCam.target = {e.x, 0, e.z}; break; }
        if (size_t(human_ - 1) < d.players.size() && d.players[size_t(human_ - 1)].hasStart) gameCam.target = {d.players[size_t(human_ - 1)].startX, 0, d.players[size_t(human_ - 1)].startZ};
        gameCam.distance = 24;
    }
    game::applyCameraSetup(game::cameraSetupFrom(ws_.project.manifest), gameCam, 720.0f);
    if (auto* w = window("game")) { w->open = true; ImGui::SetWindowFocus(w->title()); }
    status = human_ ? "Тест: вы играете за игрока " + std::to_string(human_) : "Тест: боты играют на карте";
}

void Editor::stopPlay() {
    sim_.reset();
    bots_.clear();
    gameSelection.clear();
    gridRevision_ = ~0ull; // the sim's grid was shown; rebuild the editor's
    status = "Редактирование";
}

void Editor::issueCommand(Command c) {
    if (!sim_ || human_ <= 0) return;
    c.player = uint8_t(human_);
    sim_->submit(c);
}

void Editor::samplePlayStats() {
    if (!sim_) return;
    float t = float(sim_->tick()) / float(sim_->tickRate());
    if (!playHistory.empty() && t - playHistory.back().t < 1.0f) return;
    auto* m = simModule();
    auto& gs = m->state(*sim_);
    PlaySample s;
    s.t = t;
    for (auto& p : sim_->players()) {
        int units = 0, structs = 0;
        sim_->world().each<game::Unit, Owner>([&](rts::Entity, game::Unit& u, Owner& o) { if (o.player == p.slot) (rules().types[u.type].structure ? structs : units)++; });
        s.units.push_back(units);
        s.structures.push_back(structs);
        std::vector<float> res;
        for (size_t r = 0; r < rules().resources.size(); ++r) res.push_back(float(gs.players[p.slot].stock[r].toDouble()));
        s.resources.push_back(res);
    }
    playHistory.push_back(std::move(s));
    if (playHistory.size() > 3600) playHistory.erase(playHistory.begin());
}

// ============================ frame ============================

void Editor::event(const platform::Event& e) {
    ui_.handleEvent(e);
    if (e.type == platform::EventType::QuitRequested) quit_ = true;
}

void Editor::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    const bool mod = io.KeyCtrl || io.KeySuper;
    if (showPicker_ || io.WantTextInput) return;
    if (mod && ImGui::IsKeyPressed(ImGuiKey_S, false) && !doc().path.empty()) saveMap(doc().path);
    if (mod && ImGui::IsKeyPressed(ImGuiKey_Z, false) && !playing()) { if (io.KeyShift) doc().redo(); else doc().undo(); selection = {}; }
    if (mod && ImGui::IsKeyPressed(ImGuiKey_Y, false) && !playing()) { doc().redo(); selection = {}; }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { if (!playing()) startPlay(0); else stopPlay(); }
}

void Editor::drawDockspace() {
    ImGuiViewport* mv = ImGui::GetMainViewport();
    const float statusH = ImGui::GetFrameHeight() + 6;
    ImVec2 pos(mv->WorkPos.x, mv->WorkPos.y), size(mv->WorkSize.x, mv->WorkSize.y - statusH);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(mv->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##dockhost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                           ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(3);
    ImGuiID dock = ImGui::GetID("MainDock");
    if (resetLayout_ || !ImGui::DockBuilderGetNode(dock)) {
        resetLayout_ = false;
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dock, size);
        ImGuiID center = dock;
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.19f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.27f, nullptr, &center);
        ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45f, nullptr, &left);
        for (auto& w : windows_) {
            std::string a = w->dockArea();
            ImGuiID node = a == "left" ? left : a == "left-bottom" ? leftBottom : a == "bottom" ? bottom : a == "center" ? center : right;
            ImGui::DockBuilderDockWindow(w->title(), node);
        }
        ImGui::DockBuilderFinish(dock);
        focusFirst_ = 2; // select the first tab of every area once the windows exist
    }
    ImGui::DockSpace(dock, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void Editor::frame(gfx::Device& dev, double dt) {
    auto t0 = std::chrono::steady_clock::now();
    time_ += dt;
    pollAgentLink();
    applyChanges();
    if (!playing() && gridRevision_ != doc().revision && time_ >= gridRebuildAt_) { syncGrid(); gridRebuildAt_ = time_ + 0.05; }
    if (sceneFlying_) {
        float k = float(std::min(1.0, dt * 4.0));
        sceneCam.target += (sceneGoal_ - sceneCam.target) * k;
        if (length(sceneGoal_ - sceneCam.target) < 0.05f) sceneFlying_ = false;
    }
    if (sim_ && !playPaused && sim_->result() == 0) {
        auto s0 = std::chrono::steady_clock::now();
        for (auto& b : bots_) b.update(*sim_);
        sim_->advance(dt * double(playSpeed), 200);
        lastSimMs_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - s0).count();
        samplePlayStats();
    }
    flashes_.erase(std::remove_if(flashes_.begin(), flashes_.end(), [&](const Flash& f) { return f.until < time_; }), flashes_.end());
    for (auto& w : windows_) w->update(*this, dt);

    ui_.newFrame(dt);
    handleShortcuts();
    if (showPicker_) {
        drawGamePicker();
    } else {
        drawMenuBar();
        drawDockspace();
        for (auto& w : windows_) {
            if (!w->open) continue;
            if (w->zeroPadding()) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            if (focusFirst_ == 1 && w->order() == 0) ImGui::SetNextWindowFocus();
            bool visible = ImGui::Begin(w->title(), &w->open, w->flags());
            if (w->zeroPadding()) ImGui::PopStyleVar();
            if (visible) w->draw(*this);
            ImGui::End();
        }
        drawStatusBar();
        drawActivityToasts();
        if (focusFirst_ > 0) --focusFirst_;
    }
    // the screen: only the UI (3D views were rendered offscreen by their windows)
    gfx::PassDesc pass;
    pass.clearColor[0] = 0.10f; pass.clearColor[1] = 0.11f; pass.clearColor[2] = 0.13f;
    dev.beginPass(pass);
    dev.setViewport({0, 0, dev.framebufferWidth(), dev.framebufferHeight()});
    std::string title = "RTS Editor — " + ws_.project.manifest.getStr("name", "без игры") + " — " +
                        (doc().path.empty() ? std::string("без имени") : fs::path(doc().path).filename().string()) + (doc().dirty ? " *" : "");
    static std::string lastTitle;
    if (title != lastTitle) { platform::setTitle(title); lastTitle = title; }
    ui_.render();
    dev.endPass();
    deliverShots(dev);
    FrameStats fs_{float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()), lastSimMs_, r3d_.drawCalls()};
    frameHistory.push_back(fs_);
    if (frameHistory.size() > 240) frameHistory.pop_front();
    if (quit_) platform::requestQuit();
}

} // namespace rts::editor
