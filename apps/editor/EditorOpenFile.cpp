// Opening projects by .rtsproj: requests from the OS (double-click / Open With, macOS openURLs ->
// EventType::OpenFile), the hub and the menus go through requestOpenProject, which asks to save
// unsaved map / rules changes before switching. Also the .rtsproj file association (FileAssoc.h).
#include <filesystem>
#include "Editor.h"
#include "EditorIcons.h"
#include "EditorUtil.h"
#include "core/ProjectFile.h"

namespace fs = std::filesystem;

namespace rts::editor {

void Editor::initFileAssoc() {
    const std::string done = ensureFileAssociation(opt_.editorExe);
    if (!done.empty()) ws_.log("editor", done);
    assoc_ = fileAssociationStatus(opt_.editorExe);
}

void Editor::registerFileAssoc() {
    std::string msg;
    const bool ok = registerFileAssociation(opt_.editorExe, &msg);
    ws_.log("editor", msg, !ok);
    status = msg;
    assoc_ = fileAssociationStatus(opt_.editorExe);
}

void Editor::requestOpenProject(const std::string& path) {
    pendingOpen_ = path;
    openAsked_ = false;
}

void Editor::drawOpenRequest() {
    if (pendingOpen_.empty()) return;
    static std::string target, targetName;   // resolved folder of the pending request
    const char* kPopup = "Несохранённые изменения";
    if (!openAsked_) {
        std::string err;
        target = resolveProjectDir(pendingOpen_, &err);
        if (target.empty()) {
            status = "Не удалось открыть проект: " + err;
            ws_.log("editor", status, true);
            pendingOpen_.clear();
            return;
        }
        std::error_code ec;
        if (!ws_.project.dir.empty() && fs::equivalent(target, ws_.project.dir, ec)) {   // e.g. the file the editor was started with
            showPicker_ = false;
            pendingOpen_.clear();
            return;
        }
        const bool unsaved = doc().dirty || ws_.project.rulesDirty;
        if (!unsaved) { openGame(pendingOpen_); pendingOpen_.clear(); return; }
        try { targetName = Json::parseFile((fs::path(target) / "game.json").string()).getStr("name", fs::path(target).filename().string()); }
        catch (...) { targetName = fs::path(target).filename().string(); }
        openAsked_ = true;
        ImGui::OpenPopup(kPopup);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (openAsked_ && !ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);   // lost (another modal closed it): ask again
        return;
    }
    ImGui::Text(ICON_WARNING "  В проекте «%s» есть несохранённые изменения:", ws_.project.manifest.getStr("name", "без имени").c_str());
    if (doc().dirty) ImGui::BulletText("карта %s", doc().path.empty() ? "(без имени)" : ws_.relative(doc().path).c_str());
    if (ws_.project.rulesDirty) ImGui::BulletText("правила (%s)", fs::path(ws_.project.rulesPath()).filename().string().c_str());
    ImGui::Spacing();
    ImGui::Text("Сохранить их перед открытием «%s»?", targetName.c_str());
    ImGui::Spacing();
    bool go = false, close = false;
    const bool canSave = !doc().dirty || !doc().path.empty();   // an unnamed map needs the Save As dialog of the Map menu
    ImGui::BeginDisabled(!canSave);
    if (ui::button(ICON_SAVE, "Сохранить", true, ImVec2(140, 0))) {
        bool ok = true;
        if (doc().dirty) ok = saveMap(doc().path);
        if (ok && ws_.project.rulesDirty) {
            std::string err;
            ok = ws_.project.saveRules(&err);
            if (!ok) status = "Ошибка сохранения правил: " + err;
        }
        go = ok;
        close = !ok;
    }
    ImGui::EndDisabled();
    if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Карта ещё без имени: сохраните её через «Карта → Сохранить как»");
    ImGui::SameLine();
    if (ImGui::Button("Не сохранять", ImVec2(140, 0))) go = true;
    ImGui::SameLine();
    if (ImGui::Button("Отмена", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
    if (go || close) {
        ImGui::CloseCurrentPopup();
        if (go) openGame(pendingOpen_);
        pendingOpen_.clear();
        openAsked_ = false;
    }
    ImGui::EndPopup();
}

} // namespace rts::editor
