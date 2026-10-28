// Post effects: the "postfx" chain of game.json and of the open map (bloom, colour grading, LUT,
// vignette, tilt-shift, ...), with the frame cost per device class and warnings for heavy effects,
// plus a preview of the quality tiers. Keeps Editor::postFx() in sync with the data.
// Data: engine/render/PostFxData.h; MCP: apps/editor/core/PostFxOps.cpp; docs/postfx.md.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include "../Editor.h"
#include "../EditorUtil.h"
#include "../core/PostFxOps.h"
#include "engine/render/PostFxData.h"
#include "engine/render/TextureCache.h"

namespace rts::editor {

using namespace rts::render;

namespace {
ImVec4 costColor(PostFxCostClass c) {
    return c == PostFxCostClass::Cheap ? ImVec4(0.45f, 0.8f, 0.45f, 1) : c == PostFxCostClass::Medium ? ImVec4(0.95f, 0.75f, 0.3f, 1) : ImVec4(1.0f, 0.4f, 0.35f, 1);
}
// 1..3 dots in the cost colour (cheap / medium / heavy)
void costDots(PostFxCostClass c) {
    const float r = 3.5f, step = 9.0f, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const int n = int(c) + 1;
    for (int i = 0; i < 3; ++i)
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r + float(i) * step, p.y + h * 0.5f), r,
                                                    i < n ? ImGui::GetColorU32(costColor(c)) : ImGui::GetColorU32(ImGuiCol_FrameBg));
    ImGui::Dummy(ImVec2(step * 3, h));
}
const char* costRu(PostFxCostClass c) { return c == PostFxCostClass::Cheap ? "лёгкий" : c == PostFxCostClass::Medium ? "средний" : "тяжёлый"; }
const char* kTiersRu[] = {"выкл", "низкое (слабые телефоны)", "среднее (телефоны)", "высокое (ПК, мощные)"};
const char* kDeviceRu[] = {"Слабый телефон", "Средний телефон", "ПК"};
const char* kDeviceTip[] = {"Mali-T720 / Adreno 306, 1280x720, уровень «низкое»", "Adreno 610 / Mali-G52, 1920x864, уровень «среднее»", "2560x1440, уровень «высокое»"};
} // namespace

class PostFxWindow : public EditorWindow {
public:
    const char* id() const override { return "postfx"; }
    const char* title() const override { return "Постэффекты"; }
    const char* icon() const override { return ICON_PALETTE; }
    const char* dockArea() const override { return "right"; }
    int order() const override { return 7; }
    bool openByDefault() const override { return false; }

    void update(Editor& ed, double) override {
        if (!registered_) { registerOps(ed); registered_ = true; }
        PostFx& pf = ed.postFx();
        if (ed.ws().project.dir != projectDir_) {
            projectDir_ = ed.ws().project.dir;
            const std::string dir = projectDir_;
            pf.setImageLoader([dir](const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
                std::ifstream f(dir + "/" + path, std::ios::binary);
                if (!f) return false;
                std::string bytes((std::istreambuf_iterator<char>(f)), {});
                return decodeImage(bytes, w, h, rgba);
            });
            lastKey_.clear();
        }
        // follow the data (agents change it over MCP, the window while dragging)
        const std::string key = ed.ws().project.manifest["postfx"].dump(-1) + "|" + ed.doc().data.extra["postfx"].dump(-1);
        if (key != lastKey_) {
            issues_.clear();
            pf.configure(ed.ws().project.manifest["postfx"], ed.doc().data.extra["postfx"], &issues_);
            lastKey_ = key;
        }
        pf.setQuality(tier_);
        ed.postFxInScene = inScene_;
    }

    void draw(Editor& ed) override {
        PostFx& pf = ed.postFx();
        if (!ImGui::IsAnyItemActive() && dirty_) save(ed);
        ImGui::SeparatorText("Предпросмотр");
        int t = int(tier_);
        if (ImGui::Combo("Качество", &t, kTiersRu, 4)) tier_ = PostFxQuality(t);
        ui::help("Только для просмотра: в игре уровень выбирается по видеокарте (слабые Android — низкое, телефоны — среднее, ПК — высокое), "
                 "игрок может снизить его, а при медленных кадрах он снижается сам.");
        ImGui::Checkbox("Показывать в Scene", &inScene_);
        ImGui::TextDisabled("Последний кадр: проходов %d, эффектов %d", pf.stats().passes, pf.stats().effects);

        ImGui::SeparatorText("Где");
        ImGui::RadioButton("game.json (вся игра)", &scope_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Эта карта", &scope_, 1);
        ui::help("Эффект карты заменяет эффект того же типа из game.json (только для этой карты); \"enabled\": false на карте выключает его.");
        const std::string scope = scope_ ? "map" : "game";

        drawPresets(ed, scope);
        drawEffects(ed, scope);
        drawAdd(ed, scope);
        drawCost(ed);
        if (!pf.lastError().empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "%s", pf.lastError().c_str());
    }

private:
    Json block(Editor& ed, const std::string& scope) { return postFxBlock(ed.ws(), scope); }
    Json& blockMut(Editor& ed, const std::string& scope) {
        Json& owner = scope == "map" ? ed.doc().data.extra : ed.ws().project.manifest;
        if (!owner["postfx"].isObject()) {
            Json b = Json::object();
            b.set("effects", owner["postfx"].isArray() ? owner["postfx"] : Json::array());
            owner.set("postfx", b);
        }
        Json& b = *owner.find("postfx");
        if (!b["effects"].isArray()) b.set("effects", Json::array());
        return b;
    }
    // In-memory edit (one undo step / one save per drag), written by save() when the mouse is released.
    template <class F> void edit(Editor& ed, const std::string& scope, F&& f) {
        if (scope == "map" && !dirty_) ed.doc().beginEdit();
        f(*blockMut(ed, scope).find("effects"));
        if (scope == "map") ed.doc().touched();
        dirty_ = true;
        dirtyScope_ = scope;
    }
    void save(Editor& ed) {
        dirty_ = false;
        if (dirtyScope_ == "game") {
            std::string err;
            if (!ed.ws().project.saveManifest(&err)) ed.ws().log("postfx", err, true);
        }
    }

    void drawPresets(Editor& ed, const std::string& scope) {
        ImGui::SeparatorText("Готовые наборы");
        const auto& ps = postFxPresets();
        preset_ = std::clamp(preset_, 0, int(ps.size()) - 1);
        if (ImGui::BeginCombo("##preset", ps[size_t(preset_)].label)) {
            for (int i = 0; i < int(ps.size()); ++i) {
                if (ImGui::Selectable(ps[size_t(i)].label, i == preset_)) preset_ = i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ps[size_t(i)].help);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Заменить")) applyPreset(ed, scope, "replace");
        ImGui::SameLine();
        if (ImGui::Button("Добавить")) applyPreset(ed, scope, "merge");
    }
    void applyPreset(Editor& ed, const std::string& scope, const char* mode) {
        Json p = Json::object();
        p.set("preset", Json::string(postFxPresets()[size_t(preset_)].id));
        p.set("mode", Json::string(mode));
        p.set("scope", Json::string(scope));
        lastAnswer_ = ed.ws().call("postfx_preset", p);
    }

    void drawEffects(Editor& ed, const std::string& scope) {
        ImGui::SeparatorText(scope == "map" ? "Эффекты карты" : "Эффекты игры");
        const Json effects = block(ed, scope).isArray() ? block(ed, scope) : block(ed, scope)["effects"];
        if (effects.size() == 0) { ImGui::TextDisabled("Нет эффектов: картинка идёт в экран без постобработки (ноль затрат)."); return; }
        int removeAt = -1;
        for (size_t i = 0; i < effects.size(); ++i) {
            const Json& ej = effects[i];
            const PostFxTypeDef* d = postFxType(ej.getStr("type"));
            ImGui::PushID(int(i));
            if (!d) { ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "неизвестный тип '%s'", ej.getStr("type").c_str()); ImGui::PopID(); continue; }
            PostFxEffect e;
            try { e = parsePostFxEffect(ej, false); } catch (...) { ImGui::PopID(); continue; }
            bool en = e.enabled;
            if (ImGui::Checkbox("##en", &en)) edit(ed, scope, [&](Json& arr) { arr.at(i).set("enabled", Json::fromBool(en)); });
            ImGui::SameLine();
            costDots(d->cost);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s эффект", costRu(d->cost));
            ImGui::SameLine();
            const bool open = ImGui::CollapsingHeader((std::string(d->label) + "###h").c_str());
            if (open) {
                drawParams(ed, scope, i, *d, e);
                ImGui::Indent();
                if (ImGui::SmallButton("Удалить эффект")) removeAt = int(i);
                ImGui::Unindent();
                ImGui::Spacing();
            }
            ImGui::PopID();
        }
        if (removeAt >= 0) edit(ed, scope, [&](Json& arr) { arr.eraseAt(size_t(removeAt)); });
    }

    void drawParams(Editor& ed, const std::string& scope, size_t i, const PostFxTypeDef& d, const PostFxEffect& e) {
        ImGui::Indent();
        ImGui::TextWrapped("%s", d.help);
        int mq = int(e.minQuality) - 1;
        const char* mqs[] = {"низкое", "среднее", "высокое"};
        if (ImGui::Combo("Минимальное качество", &mq, mqs, 3))
            edit(ed, scope, [&](Json& arr) { arr.at(i).set("minQuality", Json::string(postFxQualityName(PostFxQuality(mq + 1)))); });
        ui::help("На более низких уровнях эффект пропускается: так тяжёлые эффекты не попадают на слабые телефоны.");
        for (auto& w : warningsRu(e)) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1), "%s", w.c_str());
        int off = 0;
        for (auto& p : d.params) {
            switch (p.kind) {
            case PostFxParamKind::Number: {
                float v = e.values[size_t(off)];
                if (ImGui::SliderFloat(p.label, &v, p.lo, p.hi, "%.2f"))
                    edit(ed, scope, [&](Json& arr) { arr.at(i).set(p.name, Json::number(double(v), 3)); });
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", p.name, p.help);
                off += 1;
                break;
            }
            case PostFxParamKind::Bool: {
                bool v = e.values[size_t(off)] > 0.5f;
                if (ImGui::Checkbox(p.label, &v)) edit(ed, scope, [&](Json& arr) { arr.at(i).set(p.name, Json::fromBool(v)); });
                off += 1;
                break;
            }
            case PostFxParamKind::Color: {
                float c[3] = {e.values[size_t(off)], e.values[size_t(off + 1)], e.values[size_t(off + 2)]};
                const bool hdr = p.hi > 1.0f || p.lo < 0.0f;
                if (hdr ? ImGui::DragFloat3(p.label, c, 0.01f, p.lo, p.hi, "%.2f") : ImGui::ColorEdit3(p.label, c))
                    edit(ed, scope, [&](Json& arr) {
                        Json a = Json::array();
                        for (float x : c) a.push(Json::number(double(std::clamp(x, p.lo, p.hi)), 3));
                        arr.at(i).set(p.name, a);
                    });
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", p.name, p.help);
                off += 3;
                break;
            }
            case PostFxParamKind::Text: {
                std::string s = e.text;
                if (ui::inputString(p.label, s, ImGuiInputTextFlags_EnterReturnsTrue))
                    edit(ed, scope, [&](Json& arr) { arr.at(i).set(p.name, Json::string(s)); });
                ui::help("Путь в папке игры, Enter — применить. Нейтральная таблица для правки: кнопка ниже (или MCP postfx_lut_neutral).");
                if (ImGui::SmallButton("Создать нейтральную LUT")) {
                    Json r = ed.ws().call("postfx_lut_neutral", Json::object());
                    if (r.has("path")) edit(ed, scope, [&](Json& arr) { arr.at(i).set(p.name, r["path"]); });
                }
                break;
            }
            }
        }
        ImGui::Unindent();
    }

    void drawAdd(Editor& ed, const std::string& scope) {
        ImGui::Spacing();
        if (ImGui::BeginCombo("##add", "Добавить эффект…")) {
            for (auto& d : postFxCatalog()) {
                char label[160];
                std::snprintf(label, sizeof label, "%s  (%s)", d.label, costRu(d.cost));
                if (ImGui::Selectable(label)) {
                    Json p = Json::object();
                    p.set("type", Json::string(d.id));
                    p.set("scope", Json::string(scope));
                    if (d.type == PostFxType::Lut) {
                        Json r = ed.ws().call("postfx_lut_neutral", Json::object());
                        p.set("texture", r.has("path") ? r["path"] : Json::string("postfx/neutral_lut.png"));
                    }
                    lastAnswer_ = ed.ws().call("postfx_add", p);
                }
                if (ImGui::IsItemHovered()) {
                    const PostFxEffect e = defaultPostFxEffect(d.type);
                    ImGui::SetTooltip("%s\nслабый телефон ~%.1f мс, средний ~%.1f мс, ПК ~%.2f мс за кадр", d.help,
                                      double(postFxMs(postFxEffectWork(e, PostFxQuality::Low, true), postFxProfiles()[0])),
                                      double(postFxMs(postFxEffectWork(e, PostFxQuality::Medium, true), postFxProfiles()[1])),
                                      double(postFxMs(postFxEffectWork(e, PostFxQuality::High, true), postFxProfiles()[2])));
                }
            }
            ImGui::EndCombo();
        }
        if (lastAnswer_.has("error")) ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "%s", lastAnswer_.getStr("error").c_str());
    }

    void drawCost(Editor& ed) {
        ImGui::SeparatorText("Стоимость кадра (игра + карта)");
        const PostFxSettings s = parsePostFx(ed.ws().project.manifest["postfx"], ed.doc().data.extra["postfx"]);
        if (ImGui::BeginTable("cost", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Устройство", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Эфф.");
            ImGui::TableSetupColumn("мс");
            ImGui::TableSetupColumn("60 →");
            ImGui::TableHeadersRow();
            for (size_t k = 0; k < postFxProfiles().size(); ++k) {
                const auto& p = postFxProfiles()[k];
                PostFxWork sum;
                const auto active = s.activeAt(p.tier);
                if (!active.empty()) {
                    sum = postFxBaseWork();
                    for (auto* e : active) { const PostFxWork w = postFxEffectWork(*e, p.tier); sum.units += w.units; sum.passes += w.passes; }
                }
                const float ms = postFxMs(sum, p);
                const float budget = k == 2 ? 1.5f : 3.0f;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(kDeviceRu[k]);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kDeviceTip[k]);
                ImGui::TableNextColumn(); ImGui::Text("%zu", active.size());
                ImGui::TableNextColumn(); ImGui::TextColored(ms > budget ? ImVec4(1, 0.4f, 0.35f, 1) : ImVec4(0.7f, 0.85f, 0.7f, 1), "%.2f", double(ms));
                ImGui::TableNextColumn(); ImGui::Text("%.0f fps", double(1000.0f / (1000.0f / 60.0f + ms)));
            }
            ImGui::EndTable();
        }
        ui::help("Оценка по модели заполнения (engine/render/PostFxData.cpp): один полноэкранный проход с одной выборкой ~0.8 мс на слабом "
                 "телефоне в 720p. Лёгкие эффекты собраны в один общий проход, тяжёлые добавляют свои.");
        for (auto& is : issues_) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1), "%s", is.c_str());
    }

    static std::vector<std::string> warningsRu(const PostFxEffect& e) {
        std::vector<std::string> out;
        const PostFxTypeDef& d = postFxType(e.type);
        const float weak = postFxMs(postFxEffectWork(e, PostFxQuality::Low, true), postFxProfiles()[0]);
        const float mid = postFxMs(postFxEffectWork(e, PostFxQuality::Medium, true), postFxProfiles()[1]);
        char b[320];
        if (d.cost == PostFxCostClass::Heavy) {
            std::snprintf(b, sizeof b, "Тяжёлый эффект: ~%.1f мс за кадр на среднем телефоне, ~%.1f мс на слабом.", double(mid), double(weak));
            out.push_back(b);
            if (e.minQuality == PostFxQuality::Low) {
                std::snprintf(b, sizeof b, "Минимальное качество «низкое» включает его и на слабых телефонах (%.0f%% кадра при 60 fps). Лучше «среднее» или «высокое».",
                              double(weak / 16.7f * 100.0f));
                out.push_back(b);
            }
        } else if (d.cost == PostFxCostClass::Medium && e.minQuality == PostFxQuality::Low && weak >= 1.0f) {
            std::snprintf(b, sizeof b, "~%.1f мс на слабом телефоне; минимальное качество «среднее» его там пропустит.", double(weak));
            out.push_back(b);
        }
        return out;
    }

    void registerOps(Editor& ed) {
        // the core registers a stub (rtsmcp lists it); the window replaces it with the real preview
        ed.ws().registerOp({"view_postfx", "Editor preview of post-processing (does not change the game): quality off | low | medium | high shows what "
                            "weak phones (low) or strong devices (high) get; scene=false hides effects in the Scene view (Game view keeps them). "
                            "Combine with view_screenshot to compare tiers.",
                            Json::parse(R"({"properties": {"quality": {"type": "string", "enum": ["off", "low", "medium", "high"]}, "scene": {"type": "boolean"}}})"),
                            [this](const Json& p) {
                                PostFxQuality q;
                                if (p.has("quality")) {
                                    if (!parsePostFxQuality(p.getStr("quality"), q)) throw std::runtime_error("quality must be off, low, medium or high");
                                    tier_ = q;
                                }
                                if (p.has("scene")) inScene_ = p.getBool("scene", true);
                                Json r = Json::object();
                                r.set("ok", Json::fromBool(true));
                                r.set("quality", Json::string(postFxQualityName(tier_)));
                                return r;
                            }, true});
    }

    bool registered_ = false, inScene_ = true, dirty_ = false;
    int scope_ = 0, preset_ = 0;
    PostFxQuality tier_ = PostFxQuality::High;
    std::string projectDir_, lastKey_, dirtyScope_;
    std::vector<std::string> issues_;
    Json lastAnswer_;
};

RTS_EDITOR_WINDOW(PostFxWindow);

} // namespace rts::editor
