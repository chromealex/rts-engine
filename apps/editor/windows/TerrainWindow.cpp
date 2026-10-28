// Terrain sculpting and ground materials (heightfield maps): brushes raise / lower / smooth / flatten
// / noise / set on the corner heights, painting the material layer, 16-bit PNG heightmap import /
// export. Strokes happen in the Scene view while a brush here is active (the same "external tool"
// convention as the Tiles window: ed.tool == Tool::Count). Agents use the same operations:
// terrain_sculpt, terrain_material, terrain_heights, heightmap_import / heightmap_export
// (core/TerrainOps.cpp).
#include "../Editor.h"
#include "../EditorUtil.h"
#include "../core/PlanetOps.h"
#include "../core/TerrainOps.h"

namespace rts::editor {

class TerrainWindow : public EditorWindow {
public:
    const char* id() const override { return "terrain"; }
    const char* title() const override { return "Рельеф"; }
    const char* dockArea() const override { return "right"; }
    int order() const override { return 5; }
    bool openByDefault() const override { return false; }

    void draw(Editor& ed) override {
        Workspace& ws = ed.ws();
        if (ws.project.dir.empty()) { ImGui::TextDisabled("Откройте игру."); return; }
        MapData& d = ws.map.data;
        if (ed.planetMap()) ImGui::TextWrapped("Карта с планетами: кисти работают на теле под курсором в «Сцене» (3D-кисть, переходит через рёбра граней).");
        const bool hf = !d.corners.empty();
        if (!hf) ImGui::TextWrapped("У карты нет карты высот углов. Первый мазок кисти создаст её из текущего рельефа (уровни, пандусы, высоты).");
        else ImGui::TextDisabled("Карта высот: %d x %d углов%s", d.w + 1, d.h + 1, ws.project.rules.terrain.heightfield() ? " (геймплей)" : " (только вид)");
        ui::help("Высоты влияют на игру, только если в rules.json terrain.model = \"heightfield\". Иначе рельеф только визуальный.");

        if (!ed.planetMap()) {
            if (d.detailScale > 0) ImGui::TextDisabled("Детальный рельеф: x%d (%d x %d точек)", d.detailScale, d.w * d.detailScale + 1, d.h * d.detailScale + 1);
            ImGui::Checkbox("Детально (под-клеточно)", &detail_);
            ui::help("Кисти работают по детальной карте высот (k точек на клетку, мягкий край), а не по углам клеток. "
                     "Первый мазок создаёт её из того, что нарисовано сейчас. На картах heightfield углы клеток следуют за ней.");
        }
        bool active = ed.tool == Tool::Count && owner_;
        if (ImGui::Checkbox("Кисть в окне «Сцена»", &active)) setActive(ed, active);
        static const char* ops[] = {"Поднять", "Опустить", "Сгладить", "Выровнять", "Шум", "Задать высоту", "Материал", "Эрозия", "Штамп"};
        const int nOps = detail_ && !ed.planetMap() ? 9 : 7;
        if (mode_ >= nOps) mode_ = 0;
        for (int i = 0; i < nOps; ++i) {
            if (i % 4) ImGui::SameLine();
            if (ImGui::RadioButton(ops[i], mode_ == i)) { mode_ = i; setActive(ed, true); }
        }
        if (mode_ == 8) {
            static const char* stamps[] = {"crater", "mesa", "dunes", "rocks"};
            ImGui::Combo("Штамп##s", &stamp_, stamps, 4);
            ImGui::SliderFloat("Поворот", &angle_, 0.0f, 360.0f, "%.0f°");
        }
        ImGui::SliderFloat("Радиус", &radius_, detail_ ? 0.25f : 0.5f, 32.0f, "%.2f");
        if (detail_) ImGui::SliderFloat("Жёсткость", &hardness_, 0.0f, 0.95f, "%.2f");
        if (mode_ <= 1 || mode_ == 4) ImGui::SliderFloat("Сила", &strength_, 0.02f, 2.0f, "%.2f");
        else if (mode_ <= 3 || mode_ == 7) ImGui::SliderFloat("Сила##b", &blend_, 0.05f, 1.0f, "%.2f");
        else if (mode_ == 8) ImGui::SliderFloat("Высота штампа", &strength_, -4.0f, 4.0f, "%.2f");
        else if (mode_ == 5) {
            ImGui::InputFloat("Высота", &target_, 0.25f, 1.0f, "%.2f");
            ImGui::SameLine();
            if (ImGui::SmallButton("Пипетка")) pick_ = true;
            ui::help("Пипетка: следующий щелчок в «Сцене» берёт высоту оттуда.");
        } else {
            ImGui::SliderInt("Слой", &layer_, -1, 3);
            ui::help("Индекс слоя из terrain.layers (rules.json / game.json). -1 стирает: материал по типу клетки.");
        }
        ImGui::TextDisabled("Shift: обратное действие (поднять <-> опустить, стереть материал)");

        ImGui::SeparatorText("Карта высот PNG (16 бит)");
        if (!ed.planetMap()) ImGui::Checkbox("Детальная (k точек на клетку)", &pngDetail_);
        ui::inputString("Файл", file_);
        ImGui::InputFloat("Масштаб", &scale_, 0.5f, 2.0f, "%.2f");
        ImGui::InputFloat("Смещение", &offset_, 0.25f, 1.0f, "%.2f");
        ui::help("высота = смещение + значение / 65535 * масштаб (в клетках мира). Экспорт пишет масштаб и смещение в ответе и в PNG (tEXt).");
        if (ImGui::Button("Импорт")) {
            Json p = Json::object();
            p.set("file", Json::string(file_)); p.set("scale", Json::number(double(scale_), 3)); p.set("offset", Json::number(double(offset_), 3));
            p.set("detail", Json::fromBool(pngDetail_));
            report(ed, ws.call("heightmap_import", p));
        }
        ImGui::SameLine();
        if (ImGui::Button("Экспорт") && (hf || (pngDetail_ && d.detailScale > 0))) {
            Json p = Json::object();
            p.set("file", Json::string(file_));
            p.set("detail", Json::fromBool(pngDetail_));
            Json r = ws.call("heightmap_export", p);
            if (!r.has("error")) { scale_ = float(r["scale"].toDouble(scale_)); offset_ = float(r["offset"].toDouble(offset_)); }
            report(ed, r);
        }
        if (hf) {
            ImGui::SameLine();
            if (ImGui::Button("Удалить рельеф")) {
                Json p = Json::object(); p.set("enabled", Json::fromBool(false));
                report(ed, ws.call("map_heightfield", p));
            }
        }
    }

    void update(Editor& ed, double) override {
        if (ed.tool != Tool::Count) owner_ = false;
        if (!owner_ || ed.playing() || !ed.sceneViewport) { painting_ = false; return; }
        ImGuiIO& io = ImGui::GetIO();
        Viewport& vp = *ed.sceneViewport;
        if (!io.MouseDown[0]) { painting_ = false; stamped_ = false; return; }
        if (stamped_) return;
        if (!painting_ && !(io.MouseClicked[0] && vp.hovered)) return;
        if (ed.planetMap()) { planetStroke(ed, vp, io); return; }
        float x, z;
        if (!ed.groundAt(vp, ed.sceneCam, io.MousePos.x, io.MousePos.y, x, z)) return;
        if (pick_) { target_ = ed.view().heightAt(ed.editGrid(), x, z); pick_ = false; return; }
        // dabs every ~1/4 radius along the stroke, at most ~30 per second
        const double now = ed.time();
        if (painting_ && (std::hypot(x - lastX_, z - lastZ_) < radius_ * 0.25f && now - lastT_ < 0.033)) return;
        if (!painting_) ed.doc().beginEdit();
        painting_ = true;
        lastX_ = x; lastZ_ = z; lastT_ = now;
        const bool inv = io.KeyShift;
        const float lh = float(ed.rules().levelHeight.toDouble());
        if (detail_) { // sub-cell brushes (TerrainDetailOps; one undo step per stroke, begun above)
            if (mode_ == 6) { paintDetailMaterial(ed.ws(), x, z, radius_, std::max(0, layer_), inv ? 0.0f : 0.35f, hardness_); return; }
            Sculpt op = mode_ == 0 ? (inv ? Sculpt::Lower : Sculpt::Raise) : mode_ == 1 ? (inv ? Sculpt::Raise : Sculpt::Lower)
                      : mode_ == 2 ? Sculpt::Smooth : mode_ == 3 ? Sculpt::Flatten : mode_ == 4 ? Sculpt::Noise : mode_ == 5 ? Sculpt::Set
                      : mode_ == 7 ? Sculpt::Erode : Sculpt::Stamp;
            const float s = mode_ <= 1 || mode_ == 4 ? strength_ * 0.25f : mode_ == 5 ? target_ : mode_ == 8 ? (inv ? -strength_ : strength_) : blend_ * 0.5f;
            static const char* stamps[] = {"crater", "mesa", "dunes", "rocks"};
            int x0, y0, x1, y1;
            std::string err;
            if (!detailSculpt(ed.ws(), op, {{x, z}}, radius_, s, hardness_, seed_, stamps[stamp_], angle_ * 0.0174533f, x0, y0, x1, y1, &err)) ed.status = err;
            if (mode_ == 8) stamped_ = true;   // one stamp per click
            return;
        }
        if (mode_ == 6) { paintMaterial(ed.doc(), int(std::floor(x)), int(std::floor(z)), int(radius_), inv ? -1 : layer_); return; }
        Sculpt op = mode_ == 0 ? (inv ? Sculpt::Lower : Sculpt::Raise) : mode_ == 1 ? (inv ? Sculpt::Raise : Sculpt::Lower)
                  : mode_ == 2 ? Sculpt::Smooth : mode_ == 3 ? Sculpt::Flatten : mode_ == 4 ? Sculpt::Noise : Sculpt::Set;
        float s = mode_ <= 1 || mode_ == 4 ? strength_ * 0.25f : mode_ == 5 ? target_ : blend_ * 0.5f; // per dab
        sculpt(ed.doc(), lh, op, x, z, radius_, s, seed_);
    }

private:
    // planets: the same brushes as 3D dabs on the body under the cursor (PlanetOps, crosses face seams)
    void planetStroke(Editor& ed, Viewport& vp, ImGuiIO& io) {
        int body; render::float3 local;
        if (!ed.planetAt(vp, io.MousePos.x, io.MousePos.y, body, local)) return;
        const render::float3 dir = normalize(local);
        const game::PlanetSurface* s = ed.view().planets().surface(body);
        if (pick_) { target_ = s ? s->heightAt(dir) : 0; pick_ = false; return; }
        const double now = ed.time();
        const float cellAngle = s ? 1.5708f / float(s->faceSize()) : 0.05f;
        if (painting_ && std::acos(std::clamp(dot(dir, lastDir_), -1.0f, 1.0f)) < radius_ * 0.25f * cellAngle && now - lastT_ < 0.033) return;
        if (!painting_) ed.doc().beginEdit();
        painting_ = true;
        lastDir_ = dir; lastT_ = now;
        const bool inv = io.KeyShift;
        const std::string id = ed.planetBodyId(body);
        if (mode_ == 6) { planetMaterial(ed.doc(), id, dir, radius_, inv ? -1 : layer_); return; }
        Sculpt op = mode_ == 0 ? (inv ? Sculpt::Lower : Sculpt::Raise) : mode_ == 1 ? (inv ? Sculpt::Raise : Sculpt::Lower)
                  : mode_ == 2 ? Sculpt::Smooth : mode_ == 3 ? Sculpt::Flatten : mode_ == 4 ? Sculpt::Noise : Sculpt::Set;
        const float st = mode_ <= 1 || mode_ == 4 ? strength_ * 0.25f : mode_ == 5 ? target_ : blend_ * 0.5f;
        planetSculpt(ed.doc(), id, op, dir, radius_, st, seed_);
    }

    void setActive(Editor& ed, bool on) {
        owner_ = on;
        ed.tool = on ? Tool::Count : Tool::Select;
        if (on) ++seed_;
    }
    static void report(Editor& ed, const Json& r) { ed.status = r.has("error") ? r.getStr("error") : r.getStr("summary"); }

    int mode_ = 0, layer_ = 0, stamp_ = 0;
    float radius_ = 4, strength_ = 0.5f, blend_ = 0.5f, target_ = 0, scale_ = 8, offset_ = 0, hardness_ = 0.2f, angle_ = 0;
    bool detail_ = true, pngDetail_ = false, stamped_ = false;
    std::string file_ = "maps/heightmap.png";
    bool owner_ = false, painting_ = false, pick_ = false;
    float lastX_ = -1e9f, lastZ_ = -1e9f;
    render::float3 lastDir_;
    double lastT_ = 0;
    uint32_t seed_ = 1;
};

RTS_EDITOR_WINDOW(TerrainWindow);

} // namespace rts::editor
