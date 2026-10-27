// Project: the open game — maps, rules items (types, resources, techs, factions), assets.
#include <algorithm>
#include <filesystem>
#include "../Editor.h"
#include "../EditorUtil.h"

namespace fs = std::filesystem;

namespace rts::editor {

class ProjectWindow : public EditorWindow {
public:
    const char* id() const override { return "project"; }
    const char* title() const override { return "Project"; }
    const char* dockArea() const override { return "bottom"; }

    void draw(Editor& ed) override {
        auto& ws = ed.ws();
        ImGui::TextUnformatted(ws.project.manifest.getStr("name", "(игра не открыта)").c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Другая игра…")) ed.showGamePicker();
        ImGui::SameLine();
        ui::inputString("##f", filter_);
        if (!ws.project.rulesError.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Правила: %s", ws.project.rulesError.c_str());
        if (ImGui::BeginTable("##proj", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Карты");
            ImGui::TableSetupColumn("Типы");
            ImGui::TableSetupColumn("Ресурсы");
            ImGui::TableSetupColumn("Техи");
            ImGui::TableSetupColumn("Фракции");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::error_code ec;
            std::vector<std::string> maps;
            for (auto& e : fs::directory_iterator(ws.mapsDir(), ec)) if (e.path().extension() == ".json") maps.push_back(e.path().filename().string());
            std::sort(maps.begin(), maps.end());
            for (auto& m : maps) {
                bool cur = fs::path(ed.doc().path).filename() == m;
                if (ImGui::Selectable(m.c_str(), cur, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) ed.openMap(ws.mapsDir() + "/" + m);
            }
            ImGui::TextDisabled("двойной клик — открыть");
            column(ed, "types", Selection::Type);
            column(ed, "resources", Selection::Resource);
            column(ed, "techs", Selection::Tech);
            column(ed, "factions", Selection::Faction);
            ImGui::EndTable();
        }
    }

private:
    void column(Editor& ed, const char* key, Selection::Kind kind) {
        ImGui::TableNextColumn();
        ImGui::PushID(key);
        const Json& R = ed.ws().project.rulesJson;
        for (auto& [id, v] : R[key].members()) {
            std::string label = v.getStr("name", id);
            if (!filter_.empty() && label.find(filter_) == std::string::npos && id.find(filter_) == std::string::npos) continue;
            bool sel = ed.selection.kind == kind && ed.selection.key == id;
            if (ImGui::Selectable((label + "##" + id).c_str(), sel)) ed.select({kind, -1, id});
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(id.c_str()); ImGui::EndTooltip(); }
        }
        if (ImGui::SmallButton("+")) ImGui::OpenPopup("new");
        if (ImGui::BeginPopup("new")) {
            static char nid[64] = "";
            ImGui::InputTextWithHint("##id", "id", nid, sizeof nid);
            if (ImGui::Button("Создать") && nid[0]) {
                Json v = Json::object();
                v.set("name", Json::string(nid));
                if (kind == Selection::Type) v.set("health", Json::parse(R"({"max": 100})"));
                Json prm = Json::object();
                prm.set("path", Json::string(std::string(key) + "." + nid));
                prm.set("value", v);
                Json r = ed.ws().call("rules_set", prm);
                if (r.has("error")) ed.status = r.getStr("error"); else { ed.reloadRules(); ed.select({kind, -1, nid}); }
                nid[0] = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    std::string filter_;
};

RTS_EDITOR_WINDOW(ProjectWindow);

} // namespace rts::editor
