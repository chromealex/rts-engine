// Lighting: time-of-day / weather preview, the map's "lighting" block, decorative map lights, and the
// light quality tier (to see what weak Android GPUs get). Also registers the light_* MCP operations.
// Data format: modules/rts/view/RtsLights.h.
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "../Editor.h"
#include "../EditorUtil.h"
#include "../core/LightOps.h"

namespace rts::editor {

namespace {
Json n3(double v) { return Json::number(v, 3); }
float jf(const Json& j, const char* k, float def) { return j.has(k) ? float(j[k].toDouble(def)) : def; }
Json& mapLights(Editor& ed) { return mapLightsMut(ed.doc()); }
} // namespace

class LightingWindow : public EditorWindow {
public:
    const char* id() const override { return "lighting"; }
    const char* title() const override { return "Lighting"; }
    const char* dockArea() const override { return "right"; }
    int order() const override { return 6; }
    bool openByDefault() const override { return false; }

    void update(Editor& ed, double) override {
        if (!registered_) { registerOps(ed); registered_ = true; }
        game::RtsLights& L = ed.view().lights();
        L.setPreview(preview_, time_, clouds_, storm_);
        // Scene view: follow the open map (play mode configures itself from the saved JSON too)
        const uint64_t rev = ed.doc().revision;
        if (rev != lastRevision_ || !configured_) {
            L.configure(ed.ws().project.manifest["lighting"], ed.doc().data.extra["lighting"], ed.doc().data.extra["lights"]);
            lastRevision_ = rev;
            configured_ = true;
        }
    }

    void draw(Editor& ed) override {
        render::LightSystem& ls = ed.renderer().lights;
        if (!ImGui::IsAnyItemActive()) editing_ = false;
        ImGui::SeparatorText("Предпросмотр");
        ImGui::Checkbox("Время суток и погода вручную", &preview_);
        ui::help("Только для просмотра в редакторе: в игре время идёт по блоку \"lighting\" карты или game.json.");
        ImGui::BeginDisabled(!preview_);
        ImGui::SliderFloat("Время суток", &time_, 0.0f, 1.0f, timeLabel(time_));
        ImGui::SliderFloat("Облачность", &clouds_, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Гроза", &storm_, 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();
        int q = int(ls.quality());
        const char* tiers[] = {"выкл", "низкое (старые Android)", "среднее", "высокое"};
        if (ImGui::Combo("Качество света", &q, tiers, 4)) ls.setQuality(render::LightQuality(q));
        ImGui::TextDisabled("Источников в кадре: %zu, ночь %.2f", ls.count(), ed.view().lights().night());

        ImGui::SeparatorText("Блок \"lighting\" карты");
        Json& extra = ed.doc().data.extra;
        const Json& lb = extra["lighting"];
        if (!lb.isObject()) {
            ImGui::TextDisabled("Нет: действует game.json или день по умолчанию.");
            if (ImGui::Button("Создать из предпросмотра")) {
                ed.doc().beginEdit();
                Json b = Json::object();
                b.set("time", n3(time_)); b.set("dayLength", n3(0)); b.set("clouds", n3(clouds_));
                if (storm_ > 0) b.set("storm", n3(storm_));
                extra.set("lighting", b);
                ed.doc().touched();
            }
        } else {
            Json b = lb;
            bool changed = false;
            {
                const char* presets[] = {"(нет)", "desert", "snow", "volcanic", "overcast", "cave"};
                int cur = 0;
                for (int i = 1; i < 6; ++i) if (b.getStr("preset") == presets[i]) cur = i;
                if (ImGui::Combo("Пресет", &cur, presets, 6)) {
                    if (cur == 0) b.erase("preset"); else b.set("preset", Json::string(presets[cur]));
                    changed = true;
                }
                ui::help("desert, snow, volcanic, overcast — палитры дня и ночи; cave — подземелье: вечная ночь, свет только от источников.");
            }
            changed |= field(b, "time", "Время старта", 0, 1);
            changed |= field(b, "dayLength", "Длина суток, с", 0, 3600);
            changed |= field(b, "nightLevel", "Темнота ночи", 0, 1, 1);
            changed |= field(b, "clouds", "Облачность", 0, 1);
            changed |= field(b, "storm", "Гроза", 0, 1);
            if (changed) commit(ed, [&] { ed.doc().data.extra.set("lighting", b); });
            if (ImGui::Button("Удалить блок")) commit(ed, [&] { ed.doc().data.extra.erase("lighting"); });
        }

        ImGui::SeparatorText("Источники света на карте");
        if (ImGui::Button("Добавить в центре камеры")) {
            commit(ed, [&] {
                Json l = Json::object();
                l.set("x", n3(std::round(ed.sceneCam.target.x * 2) / 2)); l.set("z", n3(std::round(ed.sceneCam.target.z * 2) / 2));
                l.set("height", n3(1)); l.set("radius", n3(4)); l.set("intensity", n3(1.5)); l.set("flicker", n3(0.15));
                Json c = Json::array(); c.push(n3(1)); c.push(n3(0.6)); c.push(n3(0.3)); l.set("color", c);
                mapLights(ed).push(l);
            });
        }
        const Json& arr = ed.doc().data.extra["lights"];   // read-only: don't add an empty block to every map
        int removeAt = -1;
        for (size_t i = 0; i < arr.size(); ++i) {
            ImGui::PushID(int(i));
            Json l = arr[i];
            const auto ml = game::RtsLights::parseMapLights([&] { Json a = Json::array(); a.push(l); return a; }());
            const auto& m = ml.front();
            const std::string head = (l.getStr("name").empty() ? "Свет " + std::to_string(i + 1) : l.getStr("name")) +
                                     "  (" + std::to_string(int(m.x)) + ", " + std::to_string(int(m.z)) + ")";
            if (ImGui::TreeNode(head.c_str())) {
                float pos[2] = {m.x, m.z}, col[3] = {m.color.x, m.color.y, m.color.z};
                float h = m.height, r = m.radius, in = m.intensity, fl = m.flicker, ha = std::max(m.halo, 0.0f);
                bool ch = false;
                ch |= ImGui::DragFloat2("x, z", pos, 0.1f);
                ch |= ImGui::DragFloat("Высота", &h, 0.05f, 0.0f, 10.0f);
                ch |= ImGui::ColorEdit3("Цвет", col);
                ch |= ImGui::DragFloat("Радиус", &r, 0.05f, 0.2f, 30.0f);
                ch |= ImGui::DragFloat("Яркость", &in, 0.02f, 0.0f, 4.0f);
                ch |= ImGui::DragFloat("Мерцание", &fl, 0.01f, 0.0f, 1.0f);
                ch |= ImGui::DragFloat("Ореол", &ha, 0.02f, 0.0f, 5.0f);
                if (ch) {
                    Json p = Json::object();
                    p.set("x", n3(pos[0])); p.set("z", n3(pos[1])); p.set("height", n3(h)); p.set("radius", n3(r));
                    p.set("intensity", n3(in)); p.set("flicker", n3(fl)); p.set("halo", n3(ha));
                    Json c = Json::array(); for (float v : col) c.push(n3(v)); p.set("color", c);
                    commit(ed, [&] { applyLightParams(mapLights(ed).at(i), p); });
                }
                if (ImGui::SmallButton("Показать")) ed.focusScene(m.x, m.z, 14);
                ImGui::SameLine();
                if (ImGui::SmallButton("Удалить")) removeAt = int(i);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeAt >= 0) commit(ed, [&] { mapLights(ed).eraseAt(size_t(removeAt)); });
        if (arr.size() == 0) ImGui::TextDisabled("Фонари, костры, лава: свет без юнита. Видны после разведки клетки.");
    }

private:
    static const char* timeLabel(float t) {
        static char buf[32];
        const int minutes = int(std::fmod(t, 1.0f) * 24 * 60 + 0.5f) % (24 * 60);
        std::snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
        return buf;
    }
    // One undo step per drag: snapshot when the widget becomes active.
    template <class F> void commit(Editor& ed, F&& f) {
        if (!editing_) ed.doc().beginEdit();
        editing_ = ImGui::IsAnyItemActive();
        f();
        ed.doc().touched();
    }
    bool field(Json& b, const char* key, const char* label, float lo, float hi, float def = 0) {
        float v = jf(b, key, def);
        if (!ImGui::DragFloat(label, &v, (hi - lo) / 300.0f, lo, hi, "%.2f")) return false;
        b.set(key, n3(v));
        return true;
    }

    // Data operations (light_*, lighting_set) live in core/LightOps.cpp.
    void registerOps(Editor& ed) {
        auto add = [&ed](const char* name, const char* desc, const char* schema, bool gui, Workspace::OpFn fn) {
            ed.ws().registerOp({name, desc, Json::parse(schema), std::move(fn), gui});
        };
        // the core registers a stub (so rtsmcp lists it); the window replaces it with the real preview
        add("view_lighting", "Editor preview of time of day / clouds / storm (does not change the map): time 0..1, clouds 0..1, storm 0..1; off=true returns to the map's own lighting. Combine with view_screenshot to check night scenes.",
            R"({"properties": {"time": {"type": "number"}, "clouds": {"type": "number"}, "storm": {"type": "number"}, "off": {"type": "boolean"}}})", true, [this](const Json& p) {
                preview_ = !p.getBool("off");
                time_ = jf(p, "time", time_); clouds_ = jf(p, "clouds", clouds_); storm_ = jf(p, "storm", storm_);
                Json r = Json::object(); r.set("ok", Json::fromBool(true)); return r;
            });
    }

    bool registered_ = false, configured_ = false, preview_ = false, editing_ = false;
    uint64_t lastRevision_ = 0;
    float time_ = 0.5f, clouds_ = 0, storm_ = 0;
};

RTS_EDITOR_WINDOW(LightingWindow);

} // namespace rts::editor
