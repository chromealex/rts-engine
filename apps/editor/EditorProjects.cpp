// Project hub (like Unity Hub): recent projects anywhere on disk, "new project" from a template into
// a folder the user picks, and a folder browser (ImGui, works the same on every desktop OS).
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include "Editor.h"
#include "EditorIcons.h"
#include "EditorUtil.h"
#include "core/Projects.h"

namespace fs = std::filesystem;

namespace rts::editor {

namespace {

struct HubState {
    int tab = 0;                       // 0 projects, 1 new project
    char search[128] = "";
    char title[128] = "Моя стратегия";
    char location[1024] = "";
    int templ = 1;                     // index into listTemplates (0 = empty)
    std::string error;
    // folder browser
    bool browsing = false;
    int browseFor = 0;                 // 0: location of a new project, 1: open an existing project
    std::string browseDir;
    char newFolder[128] = "";
    std::vector<TemplateInfo> templates;
    double templatesAt = -1;
};
HubState g_hub;

std::string ago(long long t) {
    if (t <= 0) return "";
    const long long d = (long long)std::time(nullptr) - t;
    if (d < 120) return "только что";
    if (d < 3600) return std::to_string(d / 60) + " мин назад";
    if (d < 86400) return std::to_string(d / 3600) + " ч назад";
    return std::to_string(d / 86400) + " дн назад";
}

bool contains(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return h.find(n) != std::string::npos;
}

ImU32 tint(const std::string& id) {   // stable colour per template for the card badge
    unsigned h = 2166136261u;
    for (unsigned char c : id) h = (h ^ c) * 16777619u;
    const float hue = float(h % 360) / 360.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.45f, 0.75f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, 1));
}

} // namespace

void Editor::showProjectHub(int tab) { showPicker_ = true; g_hub.tab = tab; g_hub.error.clear(); }

// Folder browser as a modal: returns true when a folder was chosen (in `out`).
static bool folderBrowser(bool& open, std::string& dir, char* newFolder, size_t newFolderSize, bool needProject, std::string& out) {
    bool chosen = false;
    if (open) ImGui::OpenPopup("Выбор папки");
    ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Выбор папки", &open)) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) dir = defaultProjectsDir();
        if (!fs::is_directory(dir, ec)) { const char* h = std::getenv("HOME"); if (!h) h = std::getenv("USERPROFILE"); dir = h ? h : "/"; }
        if (ui::toolButton(ICON_FOLDER_UP, "Вверх", false, fs::path(dir).has_parent_path() && fs::path(dir).parent_path() != fs::path(dir)))
            dir = fs::path(dir).parent_path().string();
        ImGui::SameLine();
        if (ui::toolButton(ICON_HOME, "Домашняя папка", false, true)) { const char* h = std::getenv("HOME"); if (!h) h = std::getenv("USERPROFILE"); if (h) dir = h; }
        ImGui::SameLine();
        static char pathBuf[1024];
        std::snprintf(pathBuf, sizeof pathBuf, "%s", dir.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##path", pathBuf, sizeof pathBuf, ImGuiInputTextFlags_EnterReturnsTrue) && fs::is_directory(pathBuf, ec)) dir = pathBuf;
        ImGui::BeginChild("##dirs", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f), ImGuiChildFlags_Borders);
        std::vector<fs::path> subs;
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2) && it->path().filename().string()[0] != '.') subs.push_back(it->path());
        }
        std::sort(subs.begin(), subs.end());
        for (auto& s : subs) {
            const bool isProject = fs::exists(s / "game.json", ec);
            std::string label = std::string(isProject ? ICON_GAME : ICON_PROJECT) + "  " + s.filename().string();
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (needProject && isProject && ImGui::IsMouseDoubleClicked(0)) { out = s.string(); chosen = true; }
                else if (ImGui::IsMouseDoubleClicked(0) || !needProject) dir = s.string();
                else dir = s.string();
            }
        }
        if (subs.empty()) ImGui::TextDisabled("Пустая папка");
        ImGui::EndChild();
        if (!needProject) {
            ImGui::SetNextItemWidth(200);
            ImGui::InputTextWithHint("##nf", "новая папка", newFolder, newFolderSize);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FOLDER_PLUS " Создать") && newFolder[0]) {
                fs::create_directories(fs::path(dir) / newFolder, ec);
                if (!ec) dir = (fs::path(dir) / newFolder).string();
                newFolder[0] = 0;
            }
            ImGui::SameLine();
        }
        const bool here = !needProject || fs::exists(fs::path(dir) / "game.json", ec);
        ImGui::BeginDisabled(!here);
        if (ImGui::Button(needProject ? "Открыть этот проект" : "Выбрать эту папку", ImVec2(180, 0))) { out = dir; chosen = true; }
        ImGui::EndDisabled();
        if (needProject && !here) { ImGui::SameLine(); ImGui::TextDisabled("Папка проекта содержит game.json"); }
        ImGui::SameLine(ImGui::GetWindowWidth() - 110);
        if (ImGui::Button("Отмена", ImVec2(90, 0))) open = false;
        if (chosen) { open = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    return chosen;
}

void Editor::drawGamePicker() {
    ImGuiIO& io = ImGui::GetIO();
    HubState& H = g_hub;
    if (H.location[0] == 0) std::snprintf(H.location, sizeof H.location, "%s", defaultProjectsDir().c_str());
    if (H.templates.empty() || ImGui::GetTime() - H.templatesAt > 10) { H.templates = listTemplates(ws_.engineDir); H.templatesAt = ImGui::GetTime(); }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1000.f, io.DisplaySize.x - 40), std::min(660.f, io.DisplaySize.y - 40)));
    ImGui::Begin("Проекты", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar);

    // left: navigation
    ImGui::BeginChild("##nav", ImVec2(190, 0));
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), ICON_CASTLE "  RTS Engine");
    ImGui::Spacing(); ImGui::Spacing();
    if (ImGui::Selectable(ICON_PROJECT "  Проекты", H.tab == 0, 0, ImVec2(0, 30))) H.tab = 0;
    if (ImGui::Selectable(ICON_PLUS "  Новый проект", H.tab == 1, 0, ImVec2(0, 30))) H.tab = 1;
    if (!ws_.project.dir.empty()) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 40);
        if (ImGui::Button("Закрыть", ImVec2(-1, 0))) showPicker_ = false;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_Borders);

    std::string browsed;
    if (H.tab == 0) {
        ui::heading("Проекты");
        ImGui::SameLine(std::max(140.0f, ImGui::GetWindowContentRegionMax().x - 400));
        ImGui::SetNextItemWidth(170);
        ImGui::InputTextWithHint("##search", ICON_SEARCH " поиск", H.search, sizeof H.search);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FOLDER_OPEN " Открыть…")) { H.browsing = true; H.browseFor = 1; H.browseDir = defaultProjectsDir(); }
        ImGui::SameLine();
        if (ImGui::Button(ICON_PLUS " Новый")) H.tab = 1;
        ImGui::Separator();
        auto projects = recentProjects();
        ImGui::BeginChild("##list");
        int shown = 0;
        for (auto& pr : projects) {
            if (!contains(pr.name + " " + pr.dir, H.search)) continue;
            ++shown;
            ImGui::PushID(pr.dir.c_str());
            const bool current = pr.dir == ws_.project.dir;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##p", current, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 48)) &&
                ImGui::IsMouseDoubleClicked(0) && pr.exists)
                openGame(pr.dir);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(p0.x + 6, p0.y + 8), ImVec2(p0.x + 38, p0.y + 40), tint(pr.name), 6);
            const std::string letter = pr.name.empty() ? "?" : pr.name.substr(0, (unsigned char)pr.name[0] >= 0xC0 ? 2 : 1);
            dl->AddText(ImVec2(p0.x + 16, p0.y + 15), IM_COL32(255, 255, 255, 230), letter.c_str());
            ImGui::SetCursorScreenPos(ImVec2(p0.x + 50, p0.y + 6));
            const float right = ImGui::GetWindowContentRegionMax().x;
            const float textMax = ImGui::GetWindowPos().x + right - 260;   // keep clear of the date and buttons
            ImGui::PushClipRect(ImVec2(p0.x, p0.y), ImVec2(textMax, p0.y + 48), true);
            ImGui::BeginGroup();
            ImGui::TextUnformatted(pr.name.c_str());
            if (pr.exists) ImGui::TextDisabled("%s", pr.dir.c_str());
            else ImGui::TextColored(ImVec4(1, 0.55f, 0.4f, 1), ICON_WARNING " папка не найдена: %s", pr.dir.c_str());
            ImGui::EndGroup();
            ImGui::PopClipRect();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pr.dir.c_str());
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + right - 250, p0.y + 14));
            ImGui::TextDisabled("%s", ago(pr.lastOpened).c_str());
            ImGui::SameLine(right - 140);
            ImGui::BeginDisabled(!pr.exists);
            if (ImGui::Button(current ? "Открыт" : "Открыть", ImVec2(90, 0))) openGame(pr.dir);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ui::toolButton(ICON_CLOSE, "Убрать из списка (файлы не удаляются)", false, true)) forgetProject(pr.dir);
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 52));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
        if (shown == 0) {
            ImGui::Spacing();
            ImGui::TextDisabled(projects.empty() ? "Проектов пока нет. Создайте новый из шаблона или откройте папку проекта."
                                                 : "Ничего не найдено.");
            if (projects.empty() && ImGui::Button(ICON_PLUS " Создать проект")) H.tab = 1;
        }
        ImGui::EndChild();
    } else {
        ui::heading("Новый проект");
        ImGui::TextDisabled("Шаблоны из %s", (fs::path(ws_.engineDir) / "templates").string().c_str());
        ImGui::Separator();
        const float formH = 150;
        ImGui::BeginChild("##templates", ImVec2(0, -formH));
        const float cardW = 210, cardH = 92;
        const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (cardW + 10)));
        H.templ = std::clamp(H.templ, 0, int(H.templates.size()) - 1);
        for (size_t i = 0; i < H.templates.size(); ++i) {
            const TemplateInfo& t = H.templates[i];
            if (i % size_t(cols)) ImGui::SameLine();
            ImGui::PushID(int(i));
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##t", H.templ == int(i), ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cardW, cardH))) H.templ = int(i);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRect(p0, ImVec2(p0.x + cardW, p0.y + cardH), ImGui::GetColorU32(H.templ == int(i) ? ImGuiCol_CheckMark : ImGuiCol_Border), 6, 0, H.templ == int(i) ? 2.0f : 1.0f);
            dl->AddRectFilled(ImVec2(p0.x + 10, p0.y + 10), ImVec2(p0.x + 42, p0.y + 42), t.id == "empty" ? ImGui::GetColorU32(ImGuiCol_FrameBg) : tint(t.id), 6);
            dl->AddText(ImVec2(p0.x + 18, p0.y + 17), IM_COL32(255, 255, 255, 230), t.id == "empty" ? ICON_PLUS : ICON_TEMPLATE);
            dl->PushClipRect(p0, ImVec2(p0.x + cardW - 6, p0.y + cardH), true);
            dl->AddText(ImVec2(p0.x + 52, p0.y + 12), ImGui::GetColorU32(ImGuiCol_Text), t.name.c_str());
            const std::string sub = t.id == "empty" ? std::string("с нуля") : t.id + (t.maps ? " · карт: " + std::to_string(t.maps) : "");
            dl->AddText(ImVec2(p0.x + 52, p0.y + 32), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
            if (!t.description.empty()) dl->AddText(nullptr, 0, ImVec2(p0.x + 10, p0.y + 52), ImGui::GetColorU32(ImGuiCol_TextDisabled), t.description.c_str(), nullptr, cardW - 20);
            dl->PopClipRect();
            if (ImGui::IsItemHovered() && !t.description.empty()) ImGui::SetTooltip("%s", t.description.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("Название", H.title, sizeof H.title);
        ImGui::SetNextItemWidth(520);
        ImGui::InputText("##loc", H.location, sizeof H.location);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FOLDER_OPEN " Папка…")) { H.browsing = true; H.browseFor = 0; H.browseDir = H.location; }
        ImGui::SameLine();
        ImGui::TextUnformatted("Расположение");
        const std::string target = (fs::path(H.location) / projectFolderName(H.title)).string();
        ImGui::TextDisabled("Будет создан: %s", target.c_str());
        if (!H.error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", H.error.c_str());
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 190);
        if (ui::button(ICON_PLUS, "Создать проект", true, ImVec2(180, 0))) {
            std::string err;
            const std::string dir = createProject(ws_.engineDir, H.templates[size_t(H.templ)].id, H.title, H.location, "", &err);
            if (dir.empty()) H.error = err;
            else { H.error.clear(); openGame(dir); }
        }
    }
    if (folderBrowser(H.browsing, H.browseDir, H.newFolder, sizeof H.newFolder, H.browseFor == 1, browsed)) {
        if (H.browseFor == 0) std::snprintf(H.location, sizeof H.location, "%s", browsed.c_str());
        else openGame(browsed);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace rts::editor
