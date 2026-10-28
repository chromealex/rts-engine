// Grid fields (docs/fields.md): the fields of the open game (rules.json "fields" + map "gridFields"), their
// kernels / effects, and a brush that paints the map's initial values in the Scene view (fire to start with,
// water pools, fuel, creep per player). Same "external tool" convention as the Terrain window
// (ed.tool == Tool::Count). Agents use field_list / field_get / field_paint / field_set_rules (core/FieldOps.cpp).
#include "../Editor.h"
#include "../EditorUtil.h"
#include "../core/FieldOps.h"
#include <cmath>

namespace rts::editor {

class FieldsWindow : public EditorWindow {
public:
    const char* id() const override { return "fields"; }
    const char* title() const override { return "Поля"; }
    const char* dockArea() const override { return "right"; }
    int order() const override { return 6; }
    bool openByDefault() const override { return false; }

    void draw(Editor& ed) override {
        Workspace& ws = ed.ws();
        if (ws.project.dir.empty()) { ImGui::TextDisabled("Откройте игру."); return; }
        std::vector<game::GridFieldDef> defs;
        try { defs = mapFieldDefs(ws.project.rules, ws.map); }
        catch (const std::exception& e) { ImGui::TextWrapped("Ошибка в полях карты: %s", e.what()); return; }
        if (defs.empty()) {
            ImGui::TextWrapped("В игре нет клеточных полей. Добавьте rules.json \"fields\" (огонь, вода, крип, влияние...) "
                               "или вызовите field_set_rules.");
            return;
        }
        if (sel_ >= int(defs.size())) sel_ = 0;
        if (ImGui::BeginCombo("Поле", defs[size_t(sel_)].id.c_str())) {
            for (size_t i = 0; i < defs.size(); ++i)
                if (ImGui::Selectable(defs[i].id.c_str(), sel_ == int(i))) sel_ = int(i);
            ImGui::EndCombo();
        }
        const game::GridFieldDef& d = defs[size_t(sel_)];
        static const char* kk[] = {"decay", "diffuse", "spread", "flow", "consume"};
        static const char* ek[] = {"damage", "speed", "blockMove", "blockVision", "status", "yield", "reveal"};
        std::string ks, es;
        for (auto& k : d.kernels) ks += std::string(ks.empty() ? "" : ", ") + kk[int(k.kind)];
        for (auto& e : d.effects) es += std::string(es.empty() ? "" : ", ") + ek[int(e.kind)];
        ImGui::TextDisabled("%s, %d клет./ячейку%s", d.fixed ? "fixed" : "int", d.resolution, d.perPlayer ? ", по игрокам" : "");
        ImGui::TextDisabled("Ядра: %s", ks.empty() ? "нет" : ks.c_str());
        ImGui::TextDisabled("Эффекты: %s", es.empty() ? "нет" : es.c_str());
        auto toUnits = [&](int32_t raw) { return d.fixed ? double(raw) / 65536.0 : double(raw); };
        const double lo = d.min != INT32_MIN ? toUnits(d.min) : -1000.0, hi = d.max != INT32_MAX ? toUnits(d.max) : 1000.0;
        ImGui::SeparatorText("Кисть");
        bool active = ed.tool == Tool::Count && owner_;
        if (ImGui::Checkbox("Рисовать в окне «Сцена»", &active)) { owner_ = active; ed.tool = active ? Tool::Count : Tool::Select; }
        static const char* modes[] = {"Задать", "Добавить", "Максимум", "Стереть"};
        for (int i = 0; i < 4; ++i) { if (i) ImGui::SameLine(); if (ImGui::RadioButton(modes[i], mode_ == i)) mode_ = i; }
        float v = float(value_);
        if (ImGui::SliderFloat("Значение", &v, float(lo), float(hi), d.fixed ? "%.2f" : "%.0f")) value_ = v;
        ImGui::SliderInt("Радиус", &radius_, 0, 24);
        if (d.perPlayer) ImGui::SliderInt("Игрок", &player_, 1, std::max(1, int(ws.map.data.players.size())));
        ImGui::TextDisabled("Shift: стереть");
        if (ImGui::Button("Очистить поле")) {
            Json p = Json::object();
            p.set("id", Json::string(d.id)); p.set("mode", Json::string("erase")); p.set("player", Json::number(int64_t(player_)));
            Json r = Json::array(); r.push(Json::number(int64_t(0))); r.push(Json::number(int64_t(0)));
            r.push(Json::number(int64_t(ws.map.data.w))); r.push(Json::number(int64_t(ws.map.data.h)));
            p.set("rect", r);
            Json res = ws.call("field_paint", p);
            ed.status = res.has("error") ? res.getStr("error") : res.getStr("summary");
        }
        ui::help("Начальные значения хранятся в карте (\"gridFields\".<id>.values, RLE). Ядра и эффекты поля задаются в rules.json "
                 "\"fields\" (field_set_rules); во время игры поле считает C++ (docs/fields.md).");
        id_ = d.id;
    }

    void update(Editor& ed, double) override {
        if (ed.tool != Tool::Count) owner_ = false;
        if (!owner_ || ed.playing() || !ed.sceneViewport || id_.empty()) { painting_ = false; return; }
        ImGuiIO& io = ImGui::GetIO();
        Viewport& vp = *ed.sceneViewport;
        if (!io.MouseDown[0]) { painting_ = false; return; }
        if (!painting_ && !(io.MouseClicked[0] && vp.hovered)) return;
        float x, z;
        if (!ed.groundAt(vp, ed.sceneCam, io.MousePos.x, io.MousePos.y, x, z)) return;
        const double now = ed.time();
        const int cx = int(std::floor(x)), cz = int(std::floor(z));
        if (painting_ && cx == lastX_ && cz == lastZ_ && now - lastT_ < 0.1) return;
        if (!painting_) ed.doc().beginEdit();   // one undo step per stroke
        painting_ = true;
        lastX_ = cx; lastZ_ = cz; lastT_ = now;
        std::string err;
        if (paintMapField(ed.doc(), ed.rules(), id_, cx, cz, radius_, value_, io.KeyShift ? 3 : mode_, player_, &err) < 0) ed.status = err;
    }

private:
    int sel_ = 0, mode_ = 0, radius_ = 2, player_ = 1;
    double value_ = 100;
    std::string id_;
    bool owner_ = false, painting_ = false;
    int lastX_ = -1, lastZ_ = -1;
    double lastT_ = 0;
};

RTS_EDITOR_WINDOW(FieldsWindow);

} // namespace rts::editor
