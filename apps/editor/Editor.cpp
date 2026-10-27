#include "Editor.h"
#include "imgui.h"
#include "modules/rts/RtsModule.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

namespace fs = std::filesystem;

namespace rts::editor {

using namespace render;
using platform::Event;
using platform::EventType;

namespace {
const char* kToolNames[] = {"Выбор (V)", "Объекты (B)", "Рельеф: тип (T)", "Рельеф: высота (E)", "Дороги (R)", "Удаление (X)"};
struct CellKind { char c; const char* name; };
const CellKind kCells[] = {{'.', "Земля"}, {'~', "Вода"}, {'#', "Скалы"}, {',', "Земля без стройки"}, {'=', "Брод"}};

float distPointSegment(float px, float pz, float ax, float az, float bx, float bz) {
    float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
    float t = l2 > 0 ? std::clamp(((px - ax) * dx + (pz - az) * dz) / l2, 0.0f, 1.0f) : 0.0f;
    float qx = ax + dx * t - px, qz = az + dz * t - pz;
    return std::sqrt(qx * qx + qz * qz);
}
std::string shellQuote(const std::string& s) {
#if defined(_WIN32)
    return "\"" + s + "\"";
#else
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}
} // namespace

Editor::~Editor() { if (buildThread_.joinable()) buildThread_.join(); }

// ============================ project / files ============================

void Editor::loadProject() {
    projectError_.clear();
    try {
        Json manifest = Json::parseFile(opt_.projectDir + "/game.json");
        gameName_ = manifest.getStr("name", "Game");
        rulesJson_ = Json::parseFile(opt_.projectDir + "/" + manifest.getStr("rules", "rules.json"));
        rules_ = game::loadRules(rulesJson_);
        status_ = "Проект: " + gameName_ + " (" + std::to_string(rules_.types.size()) + " типов объектов)";
    } catch (const std::exception& e) {
        projectError_ = e.what();
        status_ = "Ошибка проекта: " + projectError_;
        rules_ = game::Rules{};
        game::UnitType t; t.id = "unknown"; t.structure = true; t.footprintW = t.footprintH = 1;
        rules_.types.push_back(t);
    }
    placeType_ = std::clamp(placeType_, 0, int(rules_.types.size()) - 1);
}

std::string Editor::mapsDir() const { return (fs::path(opt_.projectDir) / "maps").string(); }

std::string Editor::relativeToProject(const std::string& file) const {
    std::error_code ec;
    fs::path rel = fs::relative(fs::path(file), fs::path(opt_.projectDir), ec);
    return ec ? file : rel.generic_string();
}

void Editor::init(gfx::Device& dev) {
    dev_ = &dev;
    loadProject();
    ui_.init(dev, (opt_.projectDir + "/fonts/ui.ttf").c_str(), 16.0f);
    r3d_.init(dev);
    r3d_.setShadowQuality(Renderer3D::defaultQualityFor(dev, false));
    view_.init(dev, rules_);
    if (!opt_.openMap.empty()) openMap(opt_.openMap);
    else newMap(48, 32);
}

void Editor::shutdown() {
    view_.shutdown(*dev_);
    r3d_.shutdown();
    ui_.shutdown();
}

void Editor::newMap(int w, int h) {
    doc_.reset(w, h);
    selected_ = -1;
    syncGrid();
    fitCamera();
    status_ = "Новая карта " + std::to_string(w) + "x" + std::to_string(h);
}

void Editor::openMap(const std::string& file) {
    std::string err;
    std::error_code ec;
    std::string norm = fs::weakly_canonical(fs::path(file), ec).string();
    if (doc_.load(ec ? file : norm, &err)) {
        status_ = "Открыто: " + relativeToProject(doc_.path);
        selected_ = -1;
        syncGrid();
        fitCamera();
    } else status_ = "Не удалось открыть: " + err;
}

bool Editor::saveMap(const std::string& file) {
    std::string err;
    std::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    if (!doc_.save(file, &err)) { status_ = "Ошибка сохранения: " + err; return false; }
    status_ = "Сохранено: " + relativeToProject(file);
    return true;
}

void Editor::addCurrentMapToGame() {
    if (doc_.path.empty()) { status_ = "Сначала сохраните карту"; return; }
    std::string manifestPath = opt_.projectDir + "/game.json";
    std::ifstream in(manifestPath);
    std::stringstream ss; ss << in.rdbuf();
    std::string text = ss.str(), rel = relativeToProject(doc_.path);
    if (text.find("\"" + rel + "\"") != std::string::npos) { status_ = "Карта уже есть в game.json"; return; }
    size_t k = text.find("\"maps\"");
    size_t br = k == std::string::npos ? k : text.find('[', k);
    if (br == std::string::npos) { status_ = "В game.json нет массива maps"; return; }
    std::string entry = "\n    { \"file\": \"" + rel + "\", \"title\": \"" + doc_.data.name + "\" },";
    size_t next = text.find_first_not_of(" \t\r\n", br + 1);
    if (next != std::string::npos && text[next] == ']') entry.pop_back();
    text.insert(br + 1, entry);
    std::ofstream out(manifestPath, std::ios::binary);
    out << text;
    status_ = "Карта добавлена в game.json";
}

// ============================ scene ============================

void Editor::syncGrid() {
    const auto& d = doc_.data;
    if (grid_.w != d.w || grid_.h != d.h) grid_.init(d.w, d.h);
    for (int i = 0; i < d.w * d.h; ++i) {
        char c = d.cells[size_t(i)];
        grid_.flags[size_t(i)] = c == '~' ? uint8_t(game::kCellWater) : c == '#' ? uint8_t(0) : c == ',' ? uint8_t(game::kCellGround | game::kCellNoBuild)
                               : c == '=' ? uint8_t(game::kCellGround | game::kCellWater | game::kCellNoBuild) : uint8_t(game::kCellGround);
        grid_.height[size_t(i)] = d.heights[size_t(i)];
    }
    view_.buildTerrain(*dev_, grid_);
    gridDirty_ = false;
}

void Editor::buildScene() {
    if (mode_ == Mode::Play && sim_) { game::sceneFromSim(*sim_, scene_, float(sim_->alpha()), 0); return; }
    const auto& d = doc_.data;
    scene_.objects.clear();
    scene_.roads.clear();
    for (size_t i = 0; i < d.entities.size(); ++i) {
        const auto& e = d.entities[i];
        int t = rules_.findType(e.type);
        if (t < 0) continue;
        game::SceneObject o;
        o.type = uint16_t(t); o.owner = uint8_t(std::clamp(e.owner, 0, 8));
        o.pos = {e.x, 0, e.z};
        if (rules_.types[size_t(t)].garrisonMax > 0) o.garrison = e.garrison >= 0 ? e.garrison : 0;
        o.selected = int(i) == selected_ || int(i) == roadStart_;
        o.hovered = int(i) == hover_;
        scene_.objects.push_back(o);
    }
    for (auto [a, b] : d.roads) scene_.roads.push_back({{d.entities[size_t(a)].x, 0, d.entities[size_t(a)].z}, {d.entities[size_t(b)].x, 0, d.entities[size_t(b)].z}});
    scene_.moveMarkers.clear();
}

void Editor::fitCamera() {
    cam_.target = {doc_.data.w * 0.5f, 0, doc_.data.h * 0.5f};
    cam_.maxDistance = std::max(60.0f, float(std::max(doc_.data.w, doc_.data.h)) * 1.6f);
    cam_.distance = std::clamp(float(std::max(doc_.data.w, doc_.data.h)) * 0.95f + 8, cam_.minDistance, cam_.maxDistance);
}

bool Editor::groundAt(float sx, float sy, float& x, float& z) const {
    float3 g;
    if (!cam_.screenToGround(sx, sy, platform::windowWidth(), platform::windowHeight(), g)) return false;
    x = g.x; z = g.z;
    return true;
}

float Editor::snap(float v, int typeIdx) const {
    if (!snap_) return v;
    const auto& t = rules_.types[size_t(std::clamp(typeIdx, 0, int(rules_.types.size()) - 1))];
    if (t.structure) return (t.footprintW % 2) ? std::floor(v) + 0.5f : std::round(v);
    return std::round(v * 4) / 4;
}

int Editor::pick(float sx, float sy) const {
    float x, z;
    if (!groundAt(sx, sy, x, z)) return -1;
    int best = -1; float bestD = 1e9f;
    for (size_t i = 0; i < doc_.data.entities.size(); ++i) {
        const auto& e = doc_.data.entities[i];
        int t = rules_.findType(e.type);
        float r = t >= 0 ? view_.selectionRadius(uint16_t(t)) + 0.2f : 0.6f;
        float d = std::hypot(x - e.x, z - e.z);
        if (d < r && d < bestD) { best = int(i); bestD = d; }
    }
    return best;
}

int Editor::pickRoad(float sx, float sy) const {
    float x, z;
    if (!groundAt(sx, sy, x, z)) return -1;
    int best = -1; float bestD = 0.5f;
    for (size_t i = 0; i < doc_.data.roads.size(); ++i) {
        auto [a, b] = doc_.data.roads[i];
        const auto& ea = doc_.data.entities[size_t(a)];
        const auto& eb = doc_.data.entities[size_t(b)];
        float d = distPointSegment(x, z, ea.x, ea.z, eb.x, eb.z);
        if (d < bestD) { bestD = d; best = int(i); }
    }
    return best;
}

// ============================ input ============================

void Editor::onPrimaryDown(float sx, float sy, uint8_t mods) {
    int n = pick(sx, sy);
    float x = 0, z = 0;
    bool ground = groundAt(sx, sy, x, z);
    switch (tool_) {
    case Tool::Select:
        selected_ = n;
        if (n >= 0) {
            doc_.beginEdit();
            dragging_ = true;
            dragOffX_ = doc_.data.entities[size_t(n)].x - x;
            dragOffZ_ = doc_.data.entities[size_t(n)].z - z;
            if (mods & platform::kModAlt) { // Alt+drag copies
                DocEntity copy = doc_.data.entities[size_t(n)];
                copy.id.clear();
                doc_.data.entities.push_back(copy);
                selected_ = int(doc_.data.entities.size() - 1);
            }
        } else panning_ = true;
        break;
    case Tool::Place: {
        if (n >= 0 && !(mods & platform::kModShift)) { selected_ = n; break; }
        if (!ground) break;
        DocEntity e;
        e.type = rules_.types[size_t(placeType_)].id;
        e.owner = placeOwner_;
        e.x = snap(x, placeType_); e.z = snap(z, placeType_);
        selected_ = doc_.addEntity(e);
        status_ = "Добавлено: " + rules_.types[size_t(placeType_)].name;
        break;
    }
    case Tool::Terrain:
    case Tool::Height:
        if (!ground) break;
        doc_.beginEdit();
        painting_ = true;
        onPrimaryDrag(sx, sy, mods);
        break;
    case Tool::Road:
        if (n < 0) { roadStart_ = -1; panning_ = true; break; }
        if (roadStart_ < 0) { roadStart_ = n; break; }
        if (n != roadStart_) {
            bool added = doc_.toggleRoad(roadStart_, n);
            status_ = added ? "Дорога добавлена" : "Дорога удалена";
            roadStart_ = (mods & platform::kModShift) ? n : -1;
        }
        break;
    case Tool::Erase:
        if (n >= 0) { doc_.removeEntity(n); selected_ = -1; }
        else if (int r = pickRoad(sx, sy); r >= 0) doc_.removeRoad(size_t(r));
        else panning_ = true;
        break;
    }
}

void Editor::onPrimaryDrag(float sx, float sy, uint8_t mods) {
    float x, z;
    if (!groundAt(sx, sy, x, z)) return;
    if (dragging_ && selected_ >= 0) {
        auto& e = doc_.data.entities[size_t(selected_)];
        int t = rules_.findType(e.type);
        e.x = snap(x + dragOffX_, t); e.z = snap(z + dragOffZ_, t);
    }
    if (painting_) {
        int cx = int(std::floor(x)), cz = int(std::floor(z));
        if (tool_ == Tool::Terrain) doc_.paint(cx, cz, brush_ - 1, paintCell_);
        else doc_.raise(cx, cz, brush_, (mods & platform::kModShift) ? -2 : 2);
        if (time_ >= gridRebuildAt_) { syncGrid(); gridRebuildAt_ = time_ + 0.05; } else gridDirty_ = true;
    }
}

void Editor::event(const Event& e) {
    bool uiWants = ui_.handleEvent(e);
    switch (e.type) {
    case EventType::QuitRequested: quit_ = true; break;
    case EventType::MouseDown:
        if (uiWants) return;
        lastX_ = e.x; lastY_ = e.y;
        if (e.button == 0 && mode_ == Mode::Edit) onPrimaryDown(e.x, e.y, e.mods);
        else if (e.button == 0) panning_ = true;
        else if (e.button == 1) { rotating_ = (e.mods & platform::kModAlt) != 0; panning_ = !rotating_; }
        else panning_ = true;
        break;
    case EventType::MouseMove:
        hover_ = uiWants && !dragging_ ? -1 : pick(e.x, e.y);
        if (dragging_ || painting_) onPrimaryDrag(e.x, e.y, e.mods);
        if (panning_) {
            float3 a, b;
            float w = platform::windowWidth(), h = platform::windowHeight();
            if (cam_.screenToGround(lastX_, lastY_, w, h, a) && cam_.screenToGround(e.x, e.y, w, h, b)) cam_.target += a - b;
        }
        if (rotating_) {
            cam_.yaw -= (e.x - lastX_) * 0.008f;
            cam_.pitch = std::clamp(cam_.pitch + (e.y - lastY_) * 0.006f, 0.35f, 1.5f);
        }
        lastX_ = e.x; lastY_ = e.y;
        break;
    case EventType::MouseUp:
        dragging_ = painting_ = panning_ = rotating_ = false;
        if (gridDirty_) syncGrid();
        break;
    case EventType::MouseWheel:
        if (uiWants) return;
        cam_.distance = std::clamp(cam_.distance * std::pow(0.88f, e.wheelY), cam_.minDistance, cam_.maxDistance);
        break;
    default: break;
    }
}

void Editor::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    const bool mod = io.KeyCtrl || io.KeySuper;
    if (mod && ImGui::IsKeyPressed(ImGuiKey_S, false)) { if (doc_.path.empty()) { fileDialogOpen_ = true; fileDialogSave_ = true; } else saveMap(doc_.path); }
    if (mod && ImGui::IsKeyPressed(ImGuiKey_O, false)) { fileDialogOpen_ = true; fileDialogSave_ = false; }
    if (mod && ImGui::IsKeyPressed(ImGuiKey_N, false)) newDialogOpen_ = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { if (mode_ == Mode::Edit) startPlay(); else stopPlay(); }
    if (mode_ != Mode::Edit) return;
    if (mod && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) doc_.redo(); else doc_.undo(); selected_ = -1; syncGrid(); }
    if (mod && ImGui::IsKeyPressed(ImGuiKey_Y, false)) { doc_.redo(); selected_ = -1; syncGrid(); }
    if (io.WantTextInput || mod) return;
    if (ImGui::IsKeyPressed(ImGuiKey_V, false)) tool_ = Tool::Select;
    if (ImGui::IsKeyPressed(ImGuiKey_B, false)) tool_ = Tool::Place;
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) tool_ = Tool::Terrain;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) tool_ = Tool::Height;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) { tool_ = Tool::Road; roadStart_ = -1; }
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) tool_ = Tool::Erase;
    if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) && selected_ >= 0) { doc_.removeEntity(selected_); selected_ = -1; }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { selected_ = -1; roadStart_ = -1; }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) fitCamera();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) brush_ = std::max(1, brush_ - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) brush_ = std::min(12, brush_ + 1);
}

// ============================ play-test / build ============================

void Editor::startPlay() {
    issues_ = doc_.validate(rules_);
    for (auto& i : issues_) if (i.error) { status_ = "Нельзя запустить: " + i.text; return; }
    try {
        SimConfig cfg;
        cfg.seed = uint64_t(std::time(nullptr));
        sim_ = std::make_unique<Simulation>(cfg, std::make_unique<game::RtsModule>());
        sim_->setup(rulesJson_, Json::parse(doc_.toJsonText()));
    } catch (const std::exception& e) {
        status_ = std::string("Ошибка запуска: ") + e.what();
        sim_.reset();
        return;
    }
    bots_.clear();
    for (auto& p : sim_->players()) bots_.emplace_back(p.slot, sim_->config().seed);
    mode_ = Mode::Play;
    playPaused_ = false;
    selected_ = -1;
    status_ = "Тест: боты играют на карте (F5 — стоп)";
}

void Editor::stopPlay() {
    mode_ = Mode::Edit;
    sim_.reset();
    bots_.clear();
    status_ = "Редактирование";
}

void Editor::runBuild(const std::string& target) {
    if (building_) return;
    if (buildThread_.joinable()) buildThread_.join();
    if (doc_.dirty && !doc_.path.empty()) saveMap(doc_.path);
    buildWindow_ = true;
    building_ = true;
    { std::lock_guard<std::mutex> lk(logMutex_); buildLog_ = "$ rtsbuild --target " + target + "\n"; }
    std::string cmd = shellQuote(opt_.buildTool) + " --project " + shellQuote(opt_.projectDir) + " --engine " + shellQuote(opt_.engineDir) +
                      " --target " + target + (buildConfig_ == 0 ? " --config Debug" : " --config Release") + " 2>&1";
    buildThread_ = std::thread([this, cmd] {
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) { std::lock_guard<std::mutex> lk(logMutex_); buildLog_ += "Не удалось запустить rtsbuild\n"; }
        else {
            char buf[512];
            while (std::fgets(buf, sizeof buf, p)) { std::lock_guard<std::mutex> lk(logMutex_); buildLog_ += buf; buildScrollToEnd_ = true; }
            int rc = pclose(p);
            std::lock_guard<std::mutex> lk(logMutex_);
            buildLog_ += rc == 0 ? "\n=== Готово ===\n" : "\n=== Ошибка сборки ===\n";
            buildScrollToEnd_ = true;
        }
        building_ = false;
    });
}

// ============================ UI ============================

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("Файл")) {
        if (ImGui::MenuItem("Новая карта…", "Ctrl+N")) newDialogOpen_ = true;
        if (ImGui::MenuItem("Открыть…", "Ctrl+O")) { fileDialogOpen_ = true; fileDialogSave_ = false; }
        if (ImGui::MenuItem("Сохранить", "Ctrl+S")) { if (doc_.path.empty()) { fileDialogOpen_ = true; fileDialogSave_ = true; } else saveMap(doc_.path); }
        if (ImGui::MenuItem("Сохранить как…")) { fileDialogOpen_ = true; fileDialogSave_ = true; }
        ImGui::Separator();
        if (ImGui::MenuItem("Перечитать правила (rules.json)")) { loadProject(); view_.shutdown(*dev_); view_.init(*dev_, rules_); syncGrid(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Выход")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Правка")) {
        bool edit = mode_ == Mode::Edit;
        if (ImGui::MenuItem("Отменить", "Ctrl+Z", false, doc_.canUndo() && edit)) { doc_.undo(); selected_ = -1; syncGrid(); }
        if (ImGui::MenuItem("Повторить", "Ctrl+Y", false, doc_.canRedo() && edit)) { doc_.redo(); selected_ = -1; syncGrid(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Удалить выбранное", "Del", false, selected_ >= 0 && edit)) { doc_.removeEntity(selected_); selected_ = -1; }
        if (ImGui::MenuItem("Зеркально: левая половина -> правая", nullptr, false, edit)) { doc_.mirrorHorizontal(); syncGrid(); status_ = "Отражено, игроки 1 и 2 поменяны в копии"; }
        if (ImGui::MenuItem("Центральная симметрия левой половины", nullptr, false, edit)) { doc_.mirrorPoint(); syncGrid(); status_ = "Отражено (поворот на 180°)"; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Вид")) {
        ImGui::MenuItem("Привязка к сетке", nullptr, &snap_);
        if (ImGui::MenuItem("Показать всю карту", "F")) fitCamera();
        static const char* q[] = {"Тени: выкл", "Тени: низкие", "Тени: средние", "Тени: высокие"};
        int qi = int(r3d_.shadowQuality());
        if (ImGui::Combo("##sq", &qi, q, 4)) r3d_.setShadowQuality(ShadowQuality(qi));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Игра")) {
        if (mode_ == Mode::Edit) { if (ImGui::MenuItem("Тест карты (боты)", "F5")) startPlay(); }
        else if (ImGui::MenuItem("Остановить тест", "F5")) stopPlay();
        if (ImGui::MenuItem("Добавить карту в game.json")) addCurrentMapToGame();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Сборка")) {
        ImGui::BeginDisabled(building_);
        if (ImGui::MenuItem("macOS (.app)")) runBuild("macos");
        if (ImGui::MenuItem("iOS (проект Xcode)")) runBuild("ios");
        if (ImGui::MenuItem("Android (проект Gradle + APK)")) runBuild("android");
        if (ImGui::MenuItem("Web (HTML5)")) runBuild("web");
        if (ImGui::MenuItem("Windows (.exe)")) runBuild("windows");
        ImGui::Separator();
        if (ImGui::MenuItem("Все платформы")) runBuild("all");
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::RadioButton("Debug", &buildConfig_, 0); ImGui::SameLine(); ImGui::RadioButton("Release", &buildConfig_, 1);
        if (ImGui::MenuItem("Журнал сборки")) buildWindow_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Editor::drawToolsPanel() {
    const float top = ImGui::GetFrameHeight();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(8, top + 8), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(260, io.DisplaySize.y - top - 50), ImGuiCond_Always);
    ImGui::Begin("Инструменты", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (mode_ == Mode::Play) { ImGui::TextUnformatted("Идёт тест карты"); ImGui::End(); return; }
    for (int i = 0; i < 6; ++i)
        if (ImGui::RadioButton(kToolNames[i], tool_ == Tool(i))) { tool_ = Tool(i); roadStart_ = -1; }
    ImGui::Separator();
    if (tool_ == Tool::Place) {
        std::string cur = placeOwner_ == 0 ? "Нейтральный" : placeOwner_ <= int(doc_.data.players.size()) ? doc_.data.players[size_t(placeOwner_ - 1)].name : "?";
        if (ImGui::BeginCombo("Владелец", cur.c_str())) {
            if (ImGui::Selectable("Нейтральный", placeOwner_ == 0)) placeOwner_ = 0;
            for (size_t p = 0; p < doc_.data.players.size(); ++p)
                if (ImGui::Selectable(doc_.data.players[p].name.c_str(), placeOwner_ == int(p + 1))) placeOwner_ = int(p + 1);
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Shift+клик — ставить поверх");
        ImGui::BeginChild("##palette", ImVec2(0, 0), ImGuiChildFlags_Borders);
        static const char* groups[] = {"Юниты", "Здания", "Ресурсы и декор"};
        for (int group = 0; group < 3; ++group) {
            ImGui::SeparatorText(groups[group]);
            for (size_t i = 0; i < rules_.types.size(); ++i) {
                const auto& t = rules_.types[i];
                int g = t.resourceNode >= 0 || (t.invulnerable && t.garrisonMax == 0) ? 2 : t.structure ? 1 : 0;
                if (g != group) continue;
                if (ImGui::Selectable(t.name.c_str(), placeType_ == int(i))) placeType_ = int(i);
            }
        }
        ImGui::EndChild();
    } else if (tool_ == Tool::Terrain) {
        for (auto& k : kCells) if (ImGui::RadioButton(k.name, paintCell_ == k.c)) paintCell_ = k.c;
        ImGui::SliderInt("Кисть", &brush_, 1, 12);
        ImGui::TextDisabled("[ и ] — размер кисти");
    } else if (tool_ == Tool::Height) {
        ImGui::SliderInt("Кисть", &brush_, 1, 12);
        ImGui::TextWrapped("Клик/перетаскивание поднимает, с Shift — опускает.");
    } else if (tool_ == Tool::Road) {
        ImGui::TextWrapped("Клик по объекту A, затем по B — добавить или убрать дорогу. Shift — продолжить цепочку. Для карт в стиле Mushroom Wars включите «Только по дорогам» в свойствах карты.");
    } else if (tool_ == Tool::Select) {
        ImGui::TextWrapped("Перетаскивание двигает объект, Alt+перетаскивание копирует. ПКМ — сдвиг камеры, Alt+ПКМ — поворот, колесо — масштаб.");
    } else ImGui::TextWrapped("Клик по объекту или дороге удаляет их.");
    ImGui::Separator();
    ImGui::Checkbox("Привязка к сетке", &snap_);
    ImGui::End();
}

void Editor::drawPropertiesPanel() {
    ImGuiIO& io = ImGui::GetIO();
    const float top = ImGui::GetFrameHeight(), w = 310;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - w - 8, top + 8), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, io.DisplaySize.y - top - 50), ImGuiCond_Always);
    ImGui::Begin("Свойства", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (!projectError_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", projectError_.c_str());

    if (mode_ == Mode::Play && sim_) {
        ImGui::SliderFloat("Скорость", &playSpeed_, 0.25f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Пауза", &playPaused_);
        ImGui::Text("Время: %u:%02u", sim_->tick() / sim_->tickRate() / 60, sim_->tick() / sim_->tickRate() % 60);
        auto* m = dynamic_cast<game::RtsModule*>(&sim_->module());
        auto& gs = m->state(*sim_);
        for (auto& p : sim_->players()) {
            float4 c = game::RtsView::playerColor(p.slot);
            ImGui::TextColored(ImVec4(c.x, c.y, c.z, 1), "%s%s", p.name.c_str(), p.alive ? "" : " (выбыл)");
            int units = 0, structs = 0;
            sim_->world().each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) { if (o.player == p.slot) (rules_.types[u.type].structure ? structs : units)++; });
            std::string line = "   юниты " + std::to_string(units) + ", здания " + std::to_string(structs);
            for (size_t r = 0; r < rules_.resources.size(); ++r) line += ", " + rules_.resources[r].name + " " + std::to_string(gs.players[p.slot].stock[r].toInt());
            ImGui::TextUnformatted(line.c_str());
        }
        if (int res = sim_->result(); res != 0) {
            ImGui::Separator();
            if (res < 0) ImGui::TextUnformatted("Ничья");
            else for (auto& p : sim_->players()) if (p.team == res && p.alive) { ImGui::Text("Победил: %s", p.name.c_str()); break; }
            if (ImGui::Button("Перезапустить")) startPlay();
        }
        ImGui::Separator();
        if (ImGui::Button("Остановить тест (F5)", ImVec2(-1, 0))) stopPlay();
        ImGui::End();
        return;
    }

    auto& d = doc_.data;
    if (selected_ >= 0 && selected_ < int(d.entities.size())) {
        auto& e = d.entities[size_t(selected_)];
        int ti = rules_.findType(e.type);
        ImGui::SeparatorText("Объект");
        if (ImGui::BeginCombo("Тип", ti >= 0 ? rules_.types[size_t(ti)].name.c_str() : e.type.c_str())) {
            for (auto& t : rules_.types) if (ImGui::Selectable(t.name.c_str(), t.id == e.type)) { doc_.beginEdit(); d.entities[size_t(selected_)].type = t.id; }
            ImGui::EndCombo();
        }
        std::string cur = e.owner == 0 ? "Нейтральный" : e.owner <= int(d.players.size()) ? d.players[size_t(e.owner - 1)].name : "?";
        if (ImGui::BeginCombo("Владелец", cur.c_str())) {
            if (ImGui::Selectable("Нейтральный", e.owner == 0)) { doc_.beginEdit(); d.entities[size_t(selected_)].owner = 0; }
            for (size_t p = 0; p < d.players.size(); ++p)
                if (ImGui::Selectable(d.players[p].name.c_str(), e.owner == int(p + 1))) { doc_.beginEdit(); d.entities[size_t(selected_)].owner = int(p + 1); }
            ImGui::EndCombo();
        }
        float pos[2] = {e.x, e.z};
        bool posChanged = ImGui::DragFloat2("Позиция", pos, 0.1f);
        if (ImGui::IsItemActivated()) doc_.beginEdit();
        if (posChanged) { d.entities[size_t(selected_)].x = pos[0]; d.entities[size_t(selected_)].z = pos[1]; doc_.dirty = true; }
        if (ti >= 0 && rules_.types[size_t(ti)].garrisonMax > 0) {
            int g = std::max(0, d.entities[size_t(selected_)].garrison);
            bool ch = ImGui::SliderInt("Гарнизон", &g, 0, rules_.types[size_t(ti)].garrisonMax * 2);
            if (ImGui::IsItemActivated()) doc_.beginEdit();
            if (ch) { d.entities[size_t(selected_)].garrison = g; doc_.dirty = true; }
        }
        if (ti >= 0 && rules_.types[size_t(ti)].resourceNode >= 0) {
            float a = d.entities[size_t(selected_)].amount >= 0 ? d.entities[size_t(selected_)].amount : float(rules_.types[size_t(ti)].resourceAmount.toDouble());
            bool ch = ImGui::DragFloat("Запас", &a, 10.0f, 0.0f, 1e7f, "%.0f");
            if (ImGui::IsItemActivated()) doc_.beginEdit();
            if (ch) { d.entities[size_t(selected_)].amount = a; doc_.dirty = true; }
        }
        char idb[64];
        std::snprintf(idb, sizeof idb, "%s", d.entities[size_t(selected_)].id.c_str());
        if (ImGui::InputText("id", idb, sizeof idb, ImGuiInputTextFlags_EnterReturnsTrue)) { doc_.beginEdit(); d.entities[size_t(selected_)].id = idb; }
        if (ImGui::Button("Удалить объект", ImVec2(-1, 0))) { doc_.removeEntity(selected_); selected_ = -1; }
    } else {
        ImGui::SeparatorText("Карта");
        char name[128];
        std::snprintf(name, sizeof name, "%s", d.name.c_str());
        if (ImGui::InputText("Название", name, sizeof name)) { d.name = name; doc_.dirty = true; }
        ImGui::Text("Размер %d x %d, объектов %zu", d.w, d.h, d.entities.size());
        if (resize_[0] == 0) { resize_[0] = d.w; resize_[1] = d.h; }
        ImGui::SetNextItemWidth(150);
        ImGui::InputInt2("##size", resize_);
        ImGui::SameLine();
        if (ImGui::Button("Изменить размер")) { doc_.resize(resize_[0], resize_[1]); syncGrid(); }
        bool ro = d.roadsOnly;
        if (ImGui::Checkbox("Только по дорогам (Mushroom Wars)", &ro)) { doc_.beginEdit(); d.roadsOnly = ro; }
        ImGui::SeparatorText("Игроки");
        for (size_t p = 0; p < d.players.size(); ++p) {
            ImGui::PushID(int(p));
            float4 c = game::RtsView::playerColor(int(p + 1));
            ImGui::ColorButton("##c", ImVec4(c.x, c.y, c.z, 1), ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
            ImGui::SameLine();
            char pn[64];
            std::snprintf(pn, sizeof pn, "%s", d.players[p].name.c_str());
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputText("##name", pn, sizeof pn)) { d.players[p].name = pn; doc_.dirty = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (ImGui::InputInt("##team", &d.players[p].team)) doc_.dirty = true;
            ImGui::SameLine();
            bool removed = d.players.size() > 2 && ImGui::SmallButton("x");
            for (auto& rd : rules_.resources) {
                float v = float(rd.start.toDouble());
                for (auto& [rid, val] : d.players[p].resources) if (rid == rd.id) v = val;
                ImGui::Indent(26);
                ImGui::SetNextItemWidth(100);
                if (ImGui::DragFloat(rd.name.c_str(), &v, 5.0f, 0.0f, 1e6f, "%.0f")) {
                    bool found = false;
                    for (auto& [rid, val] : d.players[p].resources) if (rid == rd.id) { val = v; found = true; }
                    if (!found) d.players[p].resources.push_back({rd.id, v});
                    doc_.dirty = true;
                }
                ImGui::Unindent(26);
            }
            ImGui::PopID();
            if (removed) {
                doc_.beginEdit();
                for (auto& e : d.entities) { if (e.owner == int(p + 1)) e.owner = 0; else if (e.owner > int(p + 1)) --e.owner; }
                d.players.erase(d.players.begin() + long(p));
                break;
            }
        }
        ImGui::TextDisabled("Число справа от имени — команда (союзники)");
        if (d.players.size() < 8 && ImGui::Button("+ Игрок")) {
            doc_.beginEdit();
            static const char* names[] = {"Blue", "Red", "Green", "Yellow", "Purple", "Teal", "Orange", "Pink"};
            d.players.push_back({names[d.players.size()], int(d.players.size() + 1), {}});
        }
    }
    ImGui::SeparatorText("Проверка");
    if (ImGui::Button("Проверить карту", ImVec2(-1, 0))) issues_ = doc_.validate(rules_);
    for (size_t i = 0; i < issues_.size(); ++i) {
        const auto& is = issues_[i];
        ImGui::PushID(int(i) + 1000);
        ImGui::PushStyleColor(ImGuiCol_Text, is.error ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.85f, 0.4f, 1));
        if (ImGui::Selectable(is.text.c_str(), false) && is.entity >= 0 && is.entity < int(d.entities.size())) {
            selected_ = is.entity;
            cam_.target = {d.entities[size_t(is.entity)].x, 0, d.entities[size_t(is.entity)].z};
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (!issues_.empty() && std::none_of(issues_.begin(), issues_.end(), [](const Issue& i) { return i.error; }))
        ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "Ошибок нет");
    ImGui::End();
}

void Editor::drawStatusBar() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, 0));
    ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    std::string file = doc_.path.empty() ? "(не сохранено)" : relativeToProject(doc_.path);
    ImGui::Text("%s%s  |  %s", file.c_str(), doc_.dirty ? " *" : "", status_.c_str());
    float x = 0, z = 0;
    if (groundAt(lastX_, lastY_, x, z)) { ImGui::SameLine(io.DisplaySize.x - 300); ImGui::TextDisabled("x %.1f  z %.1f", x, z); }
    if (building_) { ImGui::SameLine(io.DisplaySize.x - 140); ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Идёт сборка…"); }
    ImGui::End();
}

void Editor::drawNewMapDialog() {
    if (newDialogOpen_) { ImGui::OpenPopup("Новая карта"); newDialogOpen_ = false; }
    if (!ImGui::BeginPopupModal("Новая карта", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::InputInt2("Размер", newSize_);
    newSize_[0] = std::clamp(newSize_[0], 8, 512); newSize_[1] = std::clamp(newSize_[1], 8, 512);
    if (ImGui::Button("Создать", ImVec2(120, 0))) { newMap(newSize_[0], newSize_[1]); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Отмена", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::drawFileDialog() {
    if (fileDialogOpen_) {
        ImGui::OpenPopup("##file");
        std::string def = doc_.path.empty() ? "new_map.json" : fs::path(doc_.path).filename().string();
        std::snprintf(fileName_, sizeof fileName_, "%s", def.c_str());
        fileDialogOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##file", nullptr, ImGuiWindowFlags_NoTitleBar)) return;
    ImGui::TextUnformatted(fileDialogSave_ ? "Сохранить карту" : "Открыть карту");
    ImGui::TextDisabled("%s", mapsDir().c_str());
    ImGui::Separator();
    ImGui::BeginChild("##list", ImVec2(0, -70), ImGuiChildFlags_Borders);
    std::error_code ec;
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(mapsDir(), ec)) if (e.path().extension() == ".json") files.push_back(e.path().filename().string());
    std::sort(files.begin(), files.end());
    for (auto& f : files)
        if (ImGui::Selectable(f.c_str(), f == fileName_, ImGuiSelectableFlags_AllowDoubleClick)) {
            std::snprintf(fileName_, sizeof fileName_, "%s", f.c_str());
            if (ImGui::IsMouseDoubleClicked(0) && !fileDialogSave_) { openMap(mapsDir() + "/" + f); ImGui::CloseCurrentPopup(); }
        }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##fn", fileName_, sizeof fileName_);
    if (ImGui::Button(fileDialogSave_ ? "Сохранить" : "Открыть", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        std::string fn = fileName_;
        if (fs::path(fn).extension() != ".json") fn += ".json";
        std::string full = fs::path(fn).is_absolute() ? fn : mapsDir() + "/" + fn;
        if (fileDialogSave_) saveMap(full); else openMap(full);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Отмена", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::drawBuildWindow() {
    if (!buildWindow_) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(std::min(820.f, io.DisplaySize.x - 40), 380), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 60), ImGuiCond_Appearing, ImVec2(0.5f, 1));
    if (ImGui::Begin("Сборка", &buildWindow_)) {
        if (ImGui::Button("Копировать журнал")) { std::lock_guard<std::mutex> lk(logMutex_); ImGui::SetClipboardText(buildLog_.c_str()); }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", opt_.buildTool.c_str());
        ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lk(logMutex_);
            ImGui::TextUnformatted(buildLog_.c_str());
            if (buildScrollToEnd_) { ImGui::SetScrollHereY(1.0f); buildScrollToEnd_ = false; }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void Editor::frame(gfx::Device& dev, double dt) {
    time_ += dt;
    if (mode_ == Mode::Play && sim_ && !playPaused_ && sim_->result() == 0) {
        for (auto& b : bots_) b.update(*sim_);
        sim_->advance(dt * double(playSpeed_), 200);
    }
    buildScene();
    ui_.newFrame(dt);
    handleShortcuts();

    gfx::PassDesc pass;
    pass.clearColor[0] = 0.13f; pass.clearColor[1] = 0.15f; pass.clearColor[2] = 0.17f;
    r3d_.beginScene(cam_, dev.framebufferWidth(), dev.framebufferHeight());
    const game::MapGrid& g = mode_ == Mode::Play && sim_ ? dynamic_cast<game::RtsModule*>(&sim_->module())->state(*sim_).grid : grid_;
    view_.draw(r3d_, scene_, time_, nullptr, g);
    const float W = float(doc_.data.w), H = float(doc_.data.h);
    uint32_t border = rgba(255, 255, 255, 120);
    r3d_.line({0, 0.05f, 0}, {W, 0.05f, 0}, border); r3d_.line({W, 0.05f, 0}, {W, 0.05f, H}, border);
    r3d_.line({W, 0.05f, H}, {0, 0.05f, H}, border); r3d_.line({0, 0.05f, H}, {0, 0.05f, 0}, border);
    if (mode_ == Mode::Edit) {
        float x, z;
        bool ground = groundAt(lastX_, lastY_, x, z) && !ui_.wantsMouse();
        if (ground && (tool_ == Tool::Terrain || tool_ == Tool::Height))
            r3d_.circleXZ({std::floor(x) + 0.5f, 0, std::floor(z) + 0.5f}, float(tool_ == Tool::Terrain ? brush_ - 1 : brush_) + 0.5f, rgba(255, 255, 255, 180), 40, 0.4f);
        if (ground && tool_ == Tool::Place && hover_ < 0) {
            const auto& t = rules_.types[size_t(placeType_)];
            float cx = snap(x, placeType_), cz = snap(z, placeType_);
            if (t.structure) {
                float x0 = cx - t.footprintW * 0.5f, z0 = cz - t.footprintH * 0.5f;
                uint32_t col = rgba(255, 255, 255, 70);
                r3d_.triangle({x0, 0.1f, z0}, {x0, 0.1f, z0 + t.footprintH}, {x0 + t.footprintW, 0.1f, z0 + t.footprintH}, col);
                r3d_.triangle({x0, 0.1f, z0}, {x0 + t.footprintW, 0.1f, z0 + t.footprintH}, {x0 + t.footprintW, 0.1f, z0}, col);
            } else r3d_.circleXZ({cx, 0, cz}, float(t.radius.toDouble()) + 0.1f, rgba(255, 255, 255, 160), 20, 0.1f);
        }
        if (tool_ == Tool::Road && roadStart_ >= 0 && roadStart_ < int(doc_.data.entities.size()) && ground) {
            const auto& e = doc_.data.entities[size_t(roadStart_)];
            r3d_.quadXZ({e.x, 0, e.z}, {x, 0, z}, 0.2f, rgba(255, 255, 255, 170), 0.08f);
        }
        if (tool_ == Tool::Erase && hover_ < 0) {
            int r = pickRoad(lastX_, lastY_);
            if (r >= 0) {
                auto [a, b] = doc_.data.roads[size_t(r)];
                r3d_.quadXZ({doc_.data.entities[size_t(a)].x, 0, doc_.data.entities[size_t(a)].z}, {doc_.data.entities[size_t(b)].x, 0, doc_.data.entities[size_t(b)].z}, 0.7f, rgba(255, 80, 60, 170), 0.06f);
            }
        }
    }
    r3d_.render(pass);
    view_.drawOverlays(cam_, scene_, platform::windowWidth(), platform::windowHeight());

    drawMenuBar();
    drawToolsPanel();
    drawPropertiesPanel();
    drawStatusBar();
    drawFileDialog();
    drawNewMapDialog();
    drawBuildWindow();

    std::string title = "RTS Editor — " + gameName_ + " — " + (doc_.path.empty() ? std::string("без имени") : fs::path(doc_.path).filename().string()) + (doc_.dirty ? " *" : "");
    static std::string lastTitle;
    if (title != lastTitle) { platform::setTitle(title); lastTitle = title; }
    ui_.render();
    dev.endPass();
    if (quit_) platform::requestQuit();
}

} // namespace rts::editor
