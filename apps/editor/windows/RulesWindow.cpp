// Rules: game-wide settings (victory, heights, regions, upkeep, auto-build, wave clock, ...),
// rules-level triggers, or the whole rules.json as text. Items (types etc.) are edited in the Inspector.
#include "../Editor.h"
#include "../EditorUtil.h"

namespace rts::editor {

class RulesWindow : public EditorWindow {
public:
    const char* id() const override { return "rules"; }
    const char* title() const override { return "Rules"; }
    int order() const override { return 1; }
    const char* dockArea() const override { return "right"; }

    void draw(Editor& ed) override {
        static const char* modes[] = {"Общие правила", "Триггеры правил", "Весь rules.json"};
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##m", &mode_, modes, 3);
        Json& R = ed.ws().project.rulesJson;
        Json current;
        if (mode_ == 0) {
            current = Json::object();
            for (auto& [k, v] : R.members()) if (k != "types" && k != "resources" && k != "techs" && k != "factions" && k != "triggers") current.set(k, v);
            ImGui::TextDisabled("supplyCap, maxQueue, costModel, victory, heights, regions, upkeep, autoBuild, waveClock, damageTable…");
        } else if (mode_ == 1) current = R["triggers"].isNull() ? Json::array() : R["triggers"];
        else current = R;
        std::string key = std::to_string(mode_) + "\n" + current.dump();
        if (key != textFor_) { text_ = current.dump(); textFor_ = key; }
        ui::inputMultiline("##t", text_, ImVec2(-1, ImGui::GetContentRegionAvail().y - 60));
        if (ImGui::Button("Применить и сохранить")) {
            try {
                Json v = Json::parse(text_);
                Json backup = R;
                if (mode_ == 0) for (auto& [k, x] : v.members()) R.set(k, x);
                else if (mode_ == 1) R.set("triggers", v);
                else R = v;
                if (!ed.ws().project.reloadRules()) { message_ = ed.ws().project.rulesError; R = backup; ed.ws().project.reloadRules(); }
                else { std::string err; ed.ws().project.saveRules(&err); message_ = err.empty() ? "Сохранено" : err; ed.reloadRules(); }
                textFor_.clear();
            } catch (const std::exception& e) { message_ = std::string("JSON: ") + e.what(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Сбросить")) textFor_.clear();
        if (!message_.empty()) ImGui::TextWrapped("%s", message_.c_str());
    }

private:
    int mode_ = 0;
    std::string text_, textFor_, message_;
};

RTS_EDITOR_WINDOW(RulesWindow);

} // namespace rts::editor
