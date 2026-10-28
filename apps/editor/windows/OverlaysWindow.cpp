// Overlays of the open game (rules.json / game.json "overlays", docs/game-format.md "Overlays"):
// list in draw order (built-ins + game entries), add from templates, edit the JSON of an entry,
// enable / disable, delete, and preview: checked overlays are drawn in the Scene and Game views
// regardless of their conditions (with a ghost of a chosen structure for placement overlays).
// Every change goes through the overlay_* operations (validated, saved, live in the play-test).
#include <algorithm>
#include "../Editor.h"
#include "../EditorUtil.h"
#include "modules/rts/view/RtsOverlayData.h"

namespace rts::editor {

class OverlaysWindow : public EditorWindow {
public:
    const char* id() const override { return "overlays"; }
    const char* title() const override { return "Оверлеи"; }
    const char* dockArea() const override { return "right"; }
    int order() const override { return 7; }
    bool openByDefault() const override { return false; }

    void update(Editor& ed, double) override {
        if (registered_) return;
        registered_ = true;
        Editor* e = &ed;
        ed.ws().registerOp({"overlay_preview",
            "Editor preview: draw these overlays in the Scene and Game views regardless of their 'when' (selected-entity overlays "
            "use every object when nothing is selected, event overlays spawn sample popups). placeType = a structure type id to "
            "preview ghost / placement overlays at the cursor (or the view centre); x, z move the Scene camera there. off=true ends "
            "the preview. Combine with view_screenshot.",
            Json::parse(R"({"properties": {"ids": {"type": "array", "items": {"type": "string"}}, "placeType": {"type": "string"},
                "x": {"type": "number"}, "z": {"type": "number"}, "off": {"type": "boolean"}}})"),
            [e](const Json& p) {
                if (p.getBool("off")) { e->overlayPreview.clear(); e->overlayPreviewType = -1; }
                else {
                    if (p["ids"].isArray()) {
                        e->overlayPreview.clear();
                        for (auto& i : p["ids"].items()) e->overlayPreview.push_back(i.str());
                    }
                    if (p.has("placeType")) {
                        const int t = e->rules().findType(p.getStr("placeType"));
                        if (t < 0 && !p.getStr("placeType").empty()) throw std::runtime_error("unknown type '" + p.getStr("placeType") + "'");
                        e->overlayPreviewType = t;
                    }
                    if (p["x"].isNumber() && p["z"].isNumber()) e->focusScene(float(p["x"].toDouble()), float(p["z"].toDouble()));
                }
                Json r = Json::object();
                Json ids = Json::array();
                for (auto& i : e->overlayPreview) ids.push(Json::string(i));
                r.set("ok", Json::fromBool(true));
                r.set("preview", ids);
                return r;
            }, true});
    }

    void draw(Editor& ed) override {
        auto& ws = ed.ws();
        if (ws.project.dir.empty()) { ImGui::TextDisabled("Откройте игру"); return; }
        Json list = ws.call("overlay_list", Json::object());
        if (list.has("error")) { ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", list.getStr("error").c_str()); return; }

        // ---- add from a template
        const Json tpl = game::overlayTemplates();
        ImGui::SetNextItemWidth(-90);
        const std::string cur = tpl[size_t(tplIndex_)].getStr("name");
        if (ImGui::BeginCombo("##tpl", cur.c_str())) {
            for (size_t i = 0; i < tpl.size(); ++i)
                if (ImGui::Selectable(tpl[i].getStr("name").c_str(), int(i) == tplIndex_)) tplIndex_ = int(i);
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tpl[size_t(tplIndex_)].getStr("description").c_str());
        ImGui::SameLine();
        if (ImGui::Button("Добавить")) {
            Json p = Json::object();
            p.set("template", Json::string(tpl[size_t(tplIndex_)].getStr("id")));
            Json r = ws.call("overlay_set", p);
            message_ = r.has("error") ? r.getStr("error") : r.getStr("summary");
            if (!r.has("error")) select(ed, r.getStr("id"));
        }

        // ---- preview options
        ImGui::SeparatorText("Предпросмотр");
        ui::help("Отмеченные оверлеи рисуются в Scene и Game без условий «when». Для оверлеев размещения (сетка, зона) выберите здание: "
                 "его призрак стоит под курсором. В Scene инструмент «Объект» тоже показывает оверлеи размещения.");
        const auto& R = ed.rules();
        const char* ghost = ed.overlayPreviewType >= 0 && size_t(ed.overlayPreviewType) < R.types.size() ? R.types[size_t(ed.overlayPreviewType)].name.c_str() : "— нет —";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##ghost", ghost)) {
            if (ImGui::Selectable("— нет —", ed.overlayPreviewType < 0)) ed.overlayPreviewType = -1;
            for (size_t i = 0; i < R.types.size(); ++i)
                if (R.types[i].structure && ImGui::Selectable((R.types[i].name + "##" + R.types[i].id).c_str(), int(i) == ed.overlayPreviewType))
                    ed.overlayPreviewType = int(i);
            ImGui::EndCombo();
        }

        // ---- list
        ImGui::SeparatorText("Оверлеи (порядок отрисовки)");
        const float listH = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.45f);
        ImGui::BeginChild("##list", ImVec2(0, listH), ImGuiChildFlags_Borders);
        for (auto& o : list["overlays"].items()) {
            const std::string id = o.getStr("id");
            ImGui::PushID(id.c_str());
            bool pv = std::find(ed.overlayPreview.begin(), ed.overlayPreview.end(), id) != ed.overlayPreview.end();
            if (ImGui::Checkbox("##pv", &pv)) {
                if (pv) ed.overlayPreview.push_back(id);
                else ed.overlayPreview.erase(std::remove(ed.overlayPreview.begin(), ed.overlayPreview.end(), id), ed.overlayPreview.end());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Предпросмотр без условий");
            ImGui::SameLine();
            bool en = o.getBool("enabled", true);
            if (ImGui::Checkbox("##en", &en)) {
                Json p = Json::object();
                Json e = Json::object();
                e.set("id", Json::string(id));
                e.set("enabled", Json::fromBool(en));
                p.set("overlay", e);
                p.set("merge", Json::fromBool(true));
                Json r = ws.call("overlay_set", p);
                message_ = r.has("error") ? r.getStr("error") : r.getStr("summary");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Включён");
            ImGui::SameLine();
            std::string label = id + "  [" + o.getStr("shape") + "]  " + o.getStr("origin");
            if (o.has("error")) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.35f, 1));
            if (ImGui::Selectable(label.c_str(), id == selected_)) select(ed, id);
            if (o.has("error")) { ImGui::PopStyleColor(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o.getStr("error").c_str()); }
            ImGui::PopID();
        }
        ImGui::EndChild();

        // ---- editor of the selected entry
        if (!selected_.empty()) {
            ImGui::SeparatorText(("Правка: " + selected_).c_str());
            ImGui::RadioButton("rules.json", &target_, 0); ImGui::SameLine();
            ImGui::RadioButton("game.json", &target_, 1);
            ui::help("Куда записать. Для встроенных оверлеев (selection, health_bars, ...) запись — это переопределение: достаточно "
                     "изменённых ключей, остальное берётся из значения по умолчанию.");
            ui::inputMultiline("##json", text_, ImVec2(-1, std::max(160.0f, ImGui::GetContentRegionAvail().y - 60)));
            if (ImGui::Button("Применить")) {
                try {
                    Json p = Json::object();
                    p.set("overlay", Json::parse(text_));
                    p.set("target", Json::string(target_ ? "game" : "rules"));
                    Json r = ws.call("overlay_set", p);
                    message_ = r.has("error") ? r.getStr("error") : r.getStr("summary");
                } catch (const std::exception& e) { message_ = std::string("JSON: ") + e.what(); }
            }
            ImGui::SameLine();
            if (ImGui::Button("Удалить запись")) {
                Json p = Json::object();
                p.set("id", Json::string(selected_));
                p.set("target", Json::string(target_ ? "game" : "rules"));
                Json r = ws.call("overlay_delete", p);
                message_ = r.has("error") ? r.getStr("error") : r.getStr("summary");
                if (!r.has("error")) select(ed, selected_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Перечитать")) select(ed, selected_);
        }
        if (!message_.empty()) ImGui::TextWrapped("%s", message_.c_str());
    }

private:
    // Edit the entry as written in rules / game.json, or (built-in without an override) the default.
    void select(Editor& ed, const std::string& id) {
        selected_ = id;
        Json p = Json::object();
        p.set("id", Json::string(id));
        Json r = ed.ws().call("overlay_get", p);
        if (r.has("error")) { message_ = r.getStr("error"); text_.clear(); return; }
        const Json& own = target_ ? r["game"] : r["rules"];
        text_ = (own.isObject() ? own : r["overlay"]).dump(2);
    }

    bool registered_ = false;
    int tplIndex_ = 0, target_ = 0;
    std::string selected_, text_, message_;
};

RTS_EDITOR_WINDOW(OverlaysWindow);

} // namespace rts::editor
