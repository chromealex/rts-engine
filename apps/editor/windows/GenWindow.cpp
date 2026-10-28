// Map generation ("Генерация"): preset + seed + players + symmetry + params -> generate into the open map
// (bake, undoable), regenerate with the next seed, or store a match-time "generate" block (a new map every
// match from the match seed). Agents use the same operations: gen_presets, map_generate (core/GenOps.cpp).
#include "../Editor.h"
#include "../EditorUtil.h"
#include "../core/GenOps.h"
#include "modules/rts/gen/MapGen.h"

namespace rts::editor {

class GenWindow : public EditorWindow {
public:
    const char* id() const override { return "generate"; }
    const char* title() const override { return "Генерация"; }
    const char* dockArea() const override { return "right"; }
    int order() const override { return 6; }
    bool openByDefault() const override { return false; }

    void draw(Editor& ed) override {
        Workspace& ws = ed.ws();
        if (ws.project.dir.empty()) { ImGui::TextDisabled("Откройте игру."); return; }
        const Json presets = gen::presetList(ws.project.rulesJson);
        if (preset_ >= int(presets.size())) preset_ = 0;
        const Json& cur = presets[size_t(preset_)];
        if (ImGui::BeginCombo("Пресет", cur.getStr("title").c_str())) {
            for (size_t i = 0; i < presets.size(); ++i) {
                const std::string label = presets[i].getStr("title") + "  (" + presets[i].getStr("id") + ")";
                if (ImGui::Selectable(label.c_str(), int(i) == preset_)) { preset_ = int(i); params_.clear(); }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("%s%s", cur.getStr("genre").c_str(), cur.getStr("source") == "game" ? " · из rules.json" : "");
        const bool planet = cur["params"].getStr("shape") == "planet";
        if (planet) ImGui::TextWrapped("Пресет планеты: укажите его в world.bodies[].generate (генерация при старте матча).");
        ImGui::InputScalar("Seed", ImGuiDataType_U64, &seed_);
        ImGui::SameLine();
        if (ImGui::SmallButton("+1")) ++seed_;
        ImGui::SliderInt("Игроки", &players_, 1, 8);
        static const char* syms[] = {"auto", "none", "point", "mirror", "mirror-y", "mirror4", "rot4", "rotational"};
        ImGui::Combo("Симметрия", &sym_, syms, 8);
        ui::help("auto: 2 игрока - центральная, 4 - поворот на 90° (квадрат) или две оси, иначе - вращение. Симметрия даёт честные старты.");
        ImGui::Checkbox("Объекты по образцу карты", &useTemplate_);
        ui::help("Стартовый набор игрока 1 и нейтральные объекты открытой карты задают, что и сколько ставить. Иначе - по типам из rules.json.");
        ImGui::SameLine();
        ImGui::Checkbox("Размер карты", &keepSize_);
        ui::help("Оставить размер открытой карты (иначе размер пресета).");
        ImGui::TextDisabled("Параметры пресета (JSON, переопределяют):");
        if (params_.empty()) params_ = cur["params"].dump(-1);
        ui::inputMultiline("##params", params_, ImVec2(-1, 90));

        if (planet) ImGui::BeginDisabled();
        if (ImGui::Button("Сгенерировать")) run(ed, "bake");
        ImGui::SameLine();
        if (ImGui::Button("Ещё (seed+1)")) { ++seed_; run(ed, "bake"); }
        if (planet) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("При старте матча")) run(ed, "match");
        ui::help("Записывает в карту блок generate с seed \"match\": каждый матч (и мультиплеер, и повтор) генерирует свою карту детерминированно.");
        if (ws.map.data.extra.has("generate")) {
            ImGui::TextColored(ImVec4(0.5f, 0.85f, 1, 1), "Карта генерируется при старте: %s", ws.map.data.extra["generate"].dump(-1).c_str());
            if (ImGui::SmallButton("Убрать генерацию при старте")) { ws.map.beginEdit(); ws.map.data.extra.erase("generate"); ws.map.touched(); }
        }
        if (!result_.empty()) { ImGui::Separator(); ImGui::TextWrapped("%s", result_.c_str()); }
        if (!detail_.empty()) ImGui::TextDisabled("%s", detail_.c_str());
    }

private:
    void run(Editor& ed, const char* mode) {
        static const char* syms[] = {"auto", "none", "point", "mirror", "mirror-y", "mirror4", "rot4", "rotational"};
        const Json presets = gen::presetList(ed.ws().project.rulesJson);
        Json p = Json::object();
        p.set("preset", Json::string(presets[size_t(preset_)].getStr("id")));
        p.set("seed", Json::number(int64_t(seed_ & 0x7fffffffffffffffull)));
        p.set("players", Json::number(int64_t(players_)));
        p.set("symmetry", Json::string(syms[sym_]));
        p.set("mode", Json::string(mode));
        p.set("useTemplate", Json::fromBool(useTemplate_));
        p.set("keepSize", Json::fromBool(keepSize_));
        try { if (!params_.empty()) p.set("params", Json::parse(params_)); }
        catch (const std::exception& e) { result_ = std::string("Ошибка в параметрах: ") + e.what(); return; }
        Json r = ed.ws().call("map_generate", p);
        result_ = r.has("error") ? "Ошибка: " + r.getStr("error") : r.getStr("summary");
        detail_.clear();
        const Json& g = r["generated"];
        if (g.isObject()) {
            detail_ = "симметрия " + g.getStr("symmetry") + ", пути между стартами " + g["paths"].dump(-1);
            if (g.has("objects")) detail_ += "\nобъекты " + g["objects"].dump(-1);
        }
        ed.status = result_;
    }
    int preset_ = 0, players_ = 2, sym_ = 0;
    uint64_t seed_ = 1;
    bool useTemplate_ = true, keepSize_ = false;
    std::string params_, result_, detail_;
};

RTS_EDITOR_WINDOW(GenWindow);

} // namespace rts::editor
