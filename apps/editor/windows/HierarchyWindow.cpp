// Hierarchy: everything on the open map as a tree — players and their objects, neutral objects,
// creeps, areas, regions, waves, triggers. Click selects (Inspector), double-click focuses Scene.
#include <algorithm>
#include <map>
#include "../Editor.h"
#include "../EditorUtil.h"

namespace rts::editor {

class HierarchyWindow : public EditorWindow {
public:
    const char* id() const override { return "hierarchy"; }
    const char* title() const override { return "Hierarchy"; }
    int order() const override { return 0; }
    const char* dockArea() const override { return "left"; }

    void draw(Editor& ed) override {
        ui::inputString("##filter", filter_);
        if (filter_.empty()) { ImGui::SameLine(12); ImGui::TextDisabled("поиск"); }
        auto& d = ed.doc().data;
        const auto& rules = ed.rules();
        auto match = [&](const std::string& s) { return filter_.empty() || s.find(filter_) != std::string::npos; };
        auto leaf = [&](const std::string& label, Selection s, float fx = -1, float fz = -1) {
            bool sel = ed.selection == s;
            if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                ed.select(s);
                if (ImGui::IsMouseDoubleClicked(0) && fx >= 0) ed.focusScene(fx, fz, 18);
            }
        };
        ImGuiTreeNodeFlags open = ImGuiTreeNodeFlags_DefaultOpen;
        if (ImGui::TreeNodeEx(("Карта: " + d.name).c_str(), open)) {
            if (ImGui::IsItemClicked()) ed.select({Selection::MapProps, -1, {}});
            // objects grouped by owner, then by type
            std::map<int, std::map<std::string, std::vector<int>>> byOwner;
            for (size_t i = 0; i < d.entities.size(); ++i) byOwner[d.entities[i].owner][d.entities[i].type].push_back(int(i));
            for (int owner = 1; owner <= int(d.players.size()); ++owner) {
                const auto& pl = d.players[size_t(owner - 1)];
                ImGui::PushID(owner);
                bool nodeOpen = ImGui::TreeNodeEx(("Игрок " + std::to_string(owner) + ": " + pl.name).c_str(), byOwner[owner].empty() ? ImGuiTreeNodeFlags_Leaf : 0);
                if (ImGui::IsItemClicked()) ed.select({Selection::Player, owner - 1, {}});
                if (nodeOpen) { group(ed, byOwner[owner], match, leaf); ImGui::TreePop(); }
                ImGui::PopID();
            }
            for (int owner : {0, -1}) {
                if (byOwner[owner].empty()) continue;
                ImGui::PushID(owner + 100);
                if (ImGui::TreeNode(owner == 0 ? "Нейтральные" : "Враждебные монстры")) { group(ed, byOwner[owner], match, leaf); ImGui::TreePop(); }
                ImGui::PopID();
            }
            const Json& areas = d.extra["areas"];
            if (areas.size() && ImGui::TreeNode("Зоны")) {
                for (auto& [name, a] : areas.members())
                    if (match(name)) leaf(name, {Selection::Area, -1, name}, float(a[0].toDouble() + a[2].toDouble() / 2), float(a[1].toDouble() + a[3].toDouble() / 2));
                ImGui::TreePop();
            }
            std::vector<char> letters;
            for (char c : d.regions) if (c != '.' && std::find(letters.begin(), letters.end(), c) == letters.end()) letters.push_back(c);
            std::sort(letters.begin(), letters.end());
            if (!letters.empty() && ImGui::TreeNode("Регионы")) {
                for (char c : letters) {
                    std::string nm = std::string(1, c) + (d.regionNames.count(c) ? " — " + d.regionNames.at(c) : "");
                    if (match(nm)) leaf(nm, {Selection::Region, -1, std::string(1, c)});
                }
                ImGui::TreePop();
            }
            const Json& waves = d.extra["waves"];
            if (waves.size() && ImGui::TreeNode("Волны")) {
                for (size_t i = 0; i < waves.size(); ++i) {
                    const Json& w = waves[i];
                    std::string nm = "Волна " + std::to_string(i + 1) + " (t=" + std::to_string(w["time"].integer()) + " с)";
                    leaf(nm, {Selection::Wave, int(i), {}}, float(w["at"][0].toDouble()), float(w["at"][1].toDouble()));
                }
                ImGui::TreePop();
            }
            const Json& trig = d.extra["triggers"];
            if (trig.size() && ImGui::TreeNode("Триггеры карты")) {
                for (size_t i = 0; i < trig.size(); ++i) leaf(trig[i].getStr("id", "trigger" + std::to_string(i)), {Selection::Trigger, int(i), {}});
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
        if (ImGui::BeginPopupContextWindow("##ctx", ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::MenuItem("Добавить волну")) {
                ed.doc().beginEdit();
                Json w = Json::parse(R"({"time": 60, "player": 2, "at": [10, 10], "spawn": [], "every": 0})");
                if (!rules.types.empty()) { Json s = Json::object(); s.set("type", Json::string(rules.types[0].id)); s.set("count", Json::number(int64_t(5))); w.find("spawn")->push(s); }
                if (!ed.doc().data.extra["waves"].isArray()) ed.doc().data.extra.set("waves", Json::array());
                ed.doc().data.extra.find("waves")->push(w);
                ed.select({Selection::Wave, int(ed.doc().data.extra["waves"].size() - 1), {}});
            }
            if (ImGui::MenuItem("Добавить триггер")) {
                ed.doc().beginEdit();
                Json t = Json::parse(R"({"id": "new_trigger", "event": {"type": "time", "seconds": 30}, "actions": [{"type": "message", "text": "Прошло 30 секунд"}]})");
                if (!ed.doc().data.extra["triggers"].isArray()) ed.doc().data.extra.set("triggers", Json::array());
                ed.doc().data.extra.find("triggers")->push(t);
                ed.select({Selection::Trigger, int(ed.doc().data.extra["triggers"].size() - 1), {}});
            }
            ImGui::EndPopup();
        }
    }

private:
    template <class Match, class Leaf>
    void group(Editor& ed, const std::map<std::string, std::vector<int>>& byType, Match&& match, Leaf&& leaf) {
        auto& d = ed.doc().data;
        for (auto& [type, idx] : byType) {
            int ti = ed.rules().findType(type);
            std::string tname = ti >= 0 ? ed.rules().types[size_t(ti)].name : type + " (?)";
            if (!match(tname) && !match(type)) continue;
            if (idx.size() == 1) {
                const auto& e = d.entities[size_t(idx[0])];
                leaf(tname + (e.id.empty() ? "" : "  #" + e.id) + "##" + std::to_string(idx[0]), {Selection::Entity, idx[0], {}}, e.x, e.z);
                continue;
            }
            if (ImGui::TreeNode((tname + " ×" + std::to_string(idx.size()) + "##" + type).c_str())) {
                for (int i : idx) {
                    const auto& e = d.entities[size_t(i)];
                    char buf[96];
                    std::snprintf(buf, sizeof buf, "%s (%.0f, %.0f)##%d", e.id.empty() ? tname.c_str() : e.id.c_str(), e.x, e.z, i);
                    leaf(buf, {Selection::Entity, i, {}}, e.x, e.z);
                }
                ImGui::TreePop();
            }
        }
    }
    std::string filter_;
};

RTS_EDITOR_WINDOW(HierarchyWindow);

} // namespace rts::editor
