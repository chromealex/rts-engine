// Editor shell: main menu, game picker, status bar, agent toasts.
#include <algorithm>
#include <filesystem>
#include "Editor.h"
#include "EditorUtil.h"
#include "imgui.h"

namespace fs = std::filesystem;

namespace rts::editor {

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("Игра")) {
        if (ImGui::MenuItem("Выбрать / создать игру…")) showPicker_ = true;
        if (ImGui::BeginMenu("Открыть")) {
            for (auto& g : ws_.listGames())
                if (ImGui::MenuItem((g.name + "  (" + g.id + ")").c_str(), nullptr, g.dir == ws_.project.dir)) openGame(g.dir);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Перечитать правила с диска")) {
            std::string err;
            if (ws_.project.load(ws_.project.dir, &err)) { reloadRules(); status = "Правила перечитаны"; } else status = "Ошибка: " + err;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Выход")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Карта")) {
        if (ImGui::BeginMenu("Новая карта")) {
            static int sz[2] = {64, 48};
            ImGui::InputInt2("Размер", sz);
            if (ImGui::Button("Создать")) { newMap(std::clamp(sz[0], 8, 1024), std::clamp(sz[1], 8, 1024)); ImGui::CloseCurrentPopup(); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Открыть")) {
            std::error_code ec;
            std::vector<std::string> files;
            for (auto& e : fs::directory_iterator(ws_.mapsDir(), ec)) if (e.path().extension() == ".json") files.push_back(e.path().filename().string());
            std::sort(files.begin(), files.end());
            for (auto& f : files) if (ImGui::MenuItem(f.c_str())) openMap(ws_.mapsDir() + "/" + f);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Сохранить", "Ctrl+S", false, !doc().path.empty())) saveMap(doc().path);
        if (ImGui::BeginMenu("Сохранить как")) {
            static std::string name = "new_map.json";
            ui::inputString("Файл в maps/", name);
            if (ImGui::Button("Сохранить")) {
                std::string fn = name;
                if (fs::path(fn).extension() != ".json") fn += ".json";
                saveMap(ws_.mapsDir() + "/" + fn);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Добавить карту в game.json")) {
            Json p = Json::object(); p.set("addToGame", Json::fromBool(true));
            Json r = ws_.call("map_save", p);
            status = r.has("error") ? "Ошибка: " + r.getStr("error") : "Карта в списке игры";
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Правка")) {
        if (ImGui::MenuItem("Отменить", "Ctrl+Z", false, doc().canUndo() && !playing())) { doc().undo(); selection = {}; }
        if (ImGui::MenuItem("Повторить", "Ctrl+Y", false, doc().canRedo() && !playing())) { doc().redo(); selection = {}; }
        ImGui::Separator();
        if (ImGui::MenuItem("Удалить выбранный объект", "Del", false, selectedEntity() >= 0)) { doc().removeEntity(selectedEntity()); selection = {}; }
        if (ImGui::MenuItem("Зеркально: левая половина → правая")) { doc().mirrorHorizontal(); status = "Отражено, игроки 1 и 2 поменяны в копии"; }
        if (ImGui::MenuItem("Центральная симметрия левой половины")) { doc().mirrorPoint(); status = "Отражено (поворот на 180°)"; }
        if (ImGui::MenuItem("Достроить рампы через край плато")) { doc().beginEdit(); doc().autoRamps(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Окна")) {
        for (auto& w : windows_) ImGui::MenuItem(w->title(), nullptr, &w->open);
        ImGui::Separator();
        if (ImGui::MenuItem("Сбросить раскладку")) { for (auto& w : windows_) w->open = w->openByDefault(); resetLayout_ = true; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Тест")) {
        if (!playing()) {
            if (ImGui::MenuItem("Боты против ботов", "F5")) startPlay(0);
            for (size_t p = 0; p < doc().data.players.size(); ++p)
                if (!doc().data.players[p].waves && ImGui::MenuItem(("Играть за " + doc().data.players[p].name).c_str())) startPlay(int(p + 1));
        } else if (ImGui::MenuItem("Остановить тест", "F5")) stopPlay();
        ImGui::Separator();
        if (ImGui::BeginMenu("Запустить игру на устройстве")) {
            static const char* devices[][2] = {{"desktop-1080", "ПК 1920×1080"}, {"iphone-15", "iPhone 15"}, {"iphone-se", "iPhone SE"}, {"ipad", "iPad"},
                                               {"pixel-8", "Pixel 8"}, {"android-low", "Слабый Android 720p"}};
            for (auto& d : devices)
                if (ImGui::BeginMenu(d[1])) {
                    if (ImGui::MenuItem("Альбомная")) launchOnDevice(d[0], true);
                    if (ImGui::MenuItem("Портретная")) launchOnDevice(d[0], false);
                    ImGui::EndMenu();
                }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Сборка")) {
        ImGui::BeginDisabled(ws_.building());
        static const char* targets[][2] = {{"macos", "macOS (.app)"}, {"ios", "iOS (проект Xcode)"}, {"android", "Android (Gradle + APK)"},
                                           {"web", "Web (HTML5)"}, {"windows", "Windows (.exe)"}, {"linux", "Linux"}, {"all", "Все платформы"}};
        for (auto& t : targets) if (ImGui::MenuItem(t[1])) runBuild(t[0]);
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::RadioButton("Debug", &buildConfig, 0); ImGui::SameLine(); ImGui::RadioButton("Release", &buildConfig, 1);
        ImGui::EndMenu();
    }
    if (server_.listening()) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 170);
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 1, 1), "MCP: порт %d", server_.port());
    }
    ImGui::EndMainMenuBar();
}

void Editor::drawStatusBar() {
    ImGuiViewport* mv = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight() + 6;
    ImGui::SetNextWindowPos(ImVec2(mv->WorkPos.x, mv->WorkPos.y + mv->WorkSize.y - h));
    ImGui::SetNextWindowSize(ImVec2(mv->WorkSize.x, h));
    ImGui::SetNextWindowViewport(mv->ID);
    ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
    std::string file = doc().path.empty() ? "(карта не сохранена)" : ws_.relative(doc().path);
    ImGui::Text("%s%s  |  %s", file.c_str(), doc().dirty ? " *" : "", status.c_str());
    if (ws_.building()) { ImGui::SameLine(mv->WorkSize.x - 140); ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Идёт сборка…"); }
    ImGui::End();
}

void Editor::drawActivityToasts() {
    // the last agent actions, fading out over the main viewport
    ImGuiViewport* mv = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(mv);
    float y = mv->WorkPos.y + mv->WorkSize.y - 70;
    int shown = 0;
    for (auto it = ws_.activity.rbegin(); it != ws_.activity.rend() && shown < 5; ++it) {
        if (it->source == "editor") continue;
        double age = time_ - it->time;
        if (age > 6) break;
        float a = float(std::clamp(1.0 - (age - 4) / 2, 0.0, 1.0));
        std::string text = "Агент: " + it->text;
        ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        float x = mv->WorkPos.x + mv->WorkSize.x * 0.5f - ts.x * 0.5f;
        dl->AddRectFilled(ImVec2(x - 8, y - 4), ImVec2(x + ts.x + 8, y + ts.y + 4), IM_COL32(20, 30, 40, int(210 * a)), 5);
        dl->AddText(ImVec2(x, y), it->error ? IM_COL32(255, 140, 120, int(255 * a)) : IM_COL32(150, 225, 255, int(255 * a)), text.c_str());
        y -= ts.y + 12;
        ++shown;
    }
}

void Editor::drawGamePicker() {
    ImGuiIO& io = ImGui::GetIO();
    static char newId[64] = "mygame", newName[128] = "Моя стратегия";
    static int tpl = 0;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(720.f, io.DisplaySize.x - 40), std::min(560.f, io.DisplaySize.y - 40)));
    ImGui::Begin("Выбор игры", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);
    ImGui::TextDisabled("Игры в %s", ws_.gamesDir().c_str());
    ImGui::BeginChild("##games", ImVec2(0, -150), ImGuiChildFlags_Borders);
    auto games = ws_.listGames();
    for (auto& g : games) {
        ImGui::PushID(g.id.c_str());
        bool current = g.dir == ws_.project.dir;
        if (ImGui::Selectable("##g", current, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 40)) && ImGui::IsMouseDoubleClicked(0)) openGame(g.dir);
        ImGui::SameLine(10);
        ImGui::BeginGroup();
        ImGui::TextUnformatted(g.name.c_str());
        ImGui::TextDisabled("%s · карт: %d", g.id.c_str(), g.maps);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
        if (ImGui::Button("Открыть", ImVec2(90, 0))) openGame(g.dir);
        ImGui::PopID();
    }
    if (games.empty()) ImGui::TextUnformatted("Игр пока нет — создайте первую ниже.");
    ImGui::EndChild();
    ImGui::SeparatorText("Новая игра");
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("id (папка)", newId, sizeof newId);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("Название", newName, sizeof newName);
    std::vector<std::string> templates{"empty"};
    for (auto& g : games) templates.push_back(g.id);
    tpl = std::clamp(tpl, 0, int(templates.size()) - 1);
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Шаблон", tpl == 0 ? "Пустая игра" : ("Копия " + templates[size_t(tpl)]).c_str())) {
        for (size_t i = 0; i < templates.size(); ++i)
            if (ImGui::Selectable(i == 0 ? "Пустая игра" : ("Копия " + templates[i]).c_str(), tpl == int(i))) tpl = int(i);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Создать")) {
        Json p = Json::object();
        p.set("id", Json::string(newId));
        p.set("name", Json::string(newName));
        p.set("template", Json::string(templates[size_t(tpl)]));
        Json r = ws_.call("create_game", p);
        if (r.has("error")) status = r.getStr("error");
        else { ws_.changes.clear(); reloadRules(); showPicker_ = false; fitScene(); status = r.getStr("summary"); }
    }
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    if (!ws_.project.dir.empty()) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 110);
        if (ImGui::Button("Закрыть", ImVec2(90, 0))) showPicker_ = false;
    }
    ImGui::End();
}

} // namespace rts::editor
