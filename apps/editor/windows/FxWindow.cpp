// "Эффекты": particle effects of the open game (templates/<id>/fx/*.json) with a live preview, tier
// switch (how it looks on an old Android) and an overdraw meter; plus the event bindings (fx.json).
// Agents get the same through MCP (apps/editor/core/FxOps.cpp): fx_list, fx_get, fx_set, fx_delete,
// fx_templates, fx_import_unity, fx_bind, and fx_preview (EditorFxOps.cpp); their writes bump
// Editor::fxRevision and this window re-reads.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "../Editor.h"
#include "../EditorUtil.h"
#include "engine/fx/ParticleSystem.h"
#include "engine/render/TextureCache.h"

namespace rts::editor {

namespace fs = std::filesystem;
using fx::EffectDef;
using fx::EmitterDef;

namespace {

const char* kQualityLabels[] = {"выкл", "низк. (старые Android)", "средн.", "выс."};

bool validName(const std::string& n) {
    if (n.empty() || n.size() > 48) return false;
    for (char c : n) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool readText(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return bool(f);
}

const char* kTemplate = R"({
  "emitters": [
    { "texture": "builtin:soft", "blend": "additive", "burst": 12,
      "life": [0.4, 0.8], "speed": [1, 3], "size": [0.3, 0.5],
      "color": [[0, "#ffe0a0ff"], [1, "#ff602000"]] }
  ]
}
)";

} // namespace

class FxWindow : public EditorWindow {
public:
    const char* id() const override { return "effects"; }
    const char* title() const override { return "Эффекты"; }
    const char* dockArea() const override { return "center"; }
    int order() const override { return 4; }
    bool openByDefault() const override { return false; }

    ~FxWindow() override {
        if (dev_) { vp_.release(*dev_); preview_.shutdown(); textures_.shutdown(); }
    }

    void update(Editor& ed, double) override {
        if (ed.fxRevision != revision_) {   // an agent changed fx files: list, open effect (unless being edited), bindings
            revision_ = ed.fxRevision;
            listDirty_ = true;
            if (!sel_.empty() && !dirty_ && !bindings_) {
                if (fs::exists(fxDir(ed) / (sel_ + ".json"))) select(ed, sel_);
                else sel_.clear();
            }
            if (bindings_) openBindings(ed);
        }
        if (ed.ws().project.dir != project_) {
            project_ = ed.ws().project.dir;
            listDirty_ = true;
            sel_.clear();
            bindings_ = false;
        }
    }

    void draw(Editor& ed) override {
        if (!dev_) {
            dev_ = &ed.device();
            preview_.setQuality(fx::FxQuality::High);
            preview_.setFileReader([&ed](const std::string& path, std::string& out) { return readText(fs::path(ed.ws().project.dir) / path, out); });
            textures_.init(*dev_);
            preview_.setTextureLoader([this, &ed](const std::string& name, fx::FxTexture& out) {   // the sprites' loader
                render::TextureRef r = textures_.resolve(name, false, ed.ws().project.dir + "/");
                if (!r.valid()) return false;
                out = {r.tex, r.u0, r.v0, r.u1, r.v1};
                return true;
            });
            if (!preview_.init(dev_)) ed.status = preview_.lastError();
        }
        if (listDirty_) refreshList(ed);

        ImGui::BeginChild("##fxlist", ImVec2(ImGui::GetFontSize() * 11, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        drawList(ed);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##fxmain", ImVec2(0, 0));
        if (bindings_) drawBindings(ed);
        else if (!sel_.empty()) drawEffect(ed);
        else {
            ImGui::TextWrapped("Эффекты игры лежат в папке fx/: один файл — один эффект. Выберите эффект слева или создайте новый.");
            ImGui::TextWrapped("Какой эффект играет на какое событие (смерть, попадание, шлейф снаряда, горящее здание) — в «Привязки (fx.json)».");
        }
        ImGui::EndChild();
    }

private:
    // ------------------------------------------------------------------ files
    fs::path fxDir(Editor& ed) const { return fs::path(ed.ws().project.dir) / "fx"; }

    void refreshList(Editor& ed) {
        listDirty_ = false;
        names_.clear();
        std::error_code ec;
        for (auto it = fs::directory_iterator(fxDir(ed), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
            std::string n = it->path().stem().string();
            if (n != "fx") names_.push_back(n);
        }
        std::sort(names_.begin(), names_.end());
    }

    void select(Editor& ed, const std::string& name) {
        sel_ = name;
        bindings_ = false;
        errors_.clear();
        jsonText_.clear();
        readText(fxDir(ed) / (name + ".json"), jsonText_);
        rawIsSource_ = true;
        dirty_ = false;
        def_ = {};
        try { fx::parseEffect(Json::parse(jsonText_), "fx/" + name + ".json", def_, errors_); }
        catch (const std::exception& e) { errors_.push_back(e.what()); }
        applyPreview();
    }

    bool save(Editor& ed) {
        const std::string text = rawIsSource_ ? jsonText_ : fx::effectToJson(def_).dump(2) + "\n";
        if (!writeText(fxDir(ed) / (sel_ + ".json"), text)) { ed.status = "Не удалось записать fx/" + sel_ + ".json"; return false; }
        jsonText_ = text;
        rawIsSource_ = true;
        dirty_ = false;
        ed.reloadFx();   // play-test picks the new version up
        ed.status = "Сохранено: fx/" + sel_ + ".json";
        return true;
    }

    // ------------------------------------------------------------------ preview
    void applyPreview() {
        if (!dev_) return;
        previewId_ = def_.emitters.empty() ? -1 : preview_.define("_preview", def_);
        handle_ = {};
        nextPlay_ = 0;
    }

    void runPreview(Editor& ed, float w, float h) {
        const double now = ed.time();
        const float dt = lastTime_ < 0 ? 0.0f : float(now - lastTime_) * speed_;
        lastTime_ = now;
        preview_.setQuality(fx::FxQuality(quality_));
        moveT_ += dt;
        fx::float3 pos{0, 0.5f, 0}, dir{0, 1, 0};
        if (moving_) {
            pos = {std::cos(moveT_ * 1.6f) * 3.0f, 1.2f, std::sin(moveT_ * 1.6f) * 3.0f};
            dir = render::normalize(fx::float3{-std::sin(moveT_ * 1.6f), 0, std::cos(moveT_ * 1.6f)});
        }
        if (previewId_ >= 0) {
            if (preview_.playing(handle_)) preview_.move(handle_, pos, moving_ ? dir : fx::float3{0, 1, 0});
            else if (now >= nextPlay_ && (loop_ || !played_)) {
                handle_ = preview_.play(previewId_, pos, moving_ ? dir : fx::float3{0, 1, 0});
                played_ = true;
                const EffectDef* d = preview_.def(previewId_);
                nextPlay_ = now + (d && !d->looping() ? std::min(8.0, double(d->lifetime()) + 0.4) / std::max(0.05f, speed_) : 0.5);
            }
        }

        const float dpi = ImGui::GetIO().DisplayFramebufferScale.x;
        vp_.ensure(*dev_, int(w * dpi), int(h * dpi));
        auto& r = ed.renderer();
        r.beginScene(cam_, vp_.w, vp_.h);
        const uint32_t grid = render::rgba(80, 84, 92, 255), axis = render::rgba(120, 126, 136, 255);
        for (int i = -6; i <= 6; ++i) {
            r.line({float(i), 0, -6}, {float(i), 0, 6}, i == 0 ? axis : grid);
            r.line({-6, 0, float(i)}, {6, 0, float(i)}, i == 0 ? axis : grid);
        }
        // 1 m reference cube: sizes in the effect are metres
        const uint32_t cube = render::rgba(150, 160, 175, 255);
        const float c = 0.5f;
        for (int k = 0; k < 4; ++k) {
            float x0 = (k & 1) ? c : -c, z0 = (k & 2) ? c : -c;
            r.line({x0 + 2.5f, 0, z0}, {x0 + 2.5f, 1, z0}, cube);
        }
        for (float y : {0.0f, 1.0f}) {
            r.line({2.0f, y, -c}, {3.0f, y, -c}, cube); r.line({3.0f, y, -c}, {3.0f, y, c}, cube);
            r.line({3.0f, y, c}, {2.0f, y, c}, cube); r.line({2.0f, y, c}, {2.0f, y, -c}, cube);
        }
        preview_.setView(cam_, r.viewProj(), vp_.w, vp_.h);
        preview_.update(dt);
        gfx::PassDesc pass;
        pass.colorTarget = vp_.color;
        pass.depthTarget = vp_.depth;
        pass.clearColor[0] = 0.13f; pass.clearColor[1] = 0.14f; pass.clearColor[2] = 0.16f;
        r.render(pass);
        preview_.render({r.lighting.fogColor, 0});
        dev_->endPass();
        vp_.image(*dev_, w, h);

        // orbit: drag to rotate, wheel to zoom
        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                cam_.yaw -= io.MouseDelta.x * 0.01f;
                cam_.pitch = std::clamp(cam_.pitch + io.MouseDelta.y * 0.01f, 0.05f, 1.5f);
            }
            if (io.MouseWheel != 0) cam_.distance = std::clamp(cam_.distance * (io.MouseWheel > 0 ? 0.9f : 1.1f), 2.0f, 60.0f);
        }
    }

    // ------------------------------------------------------------------ UI
    void drawList(Editor& ed) {
        if (ImGui::Selectable("Привязки (fx.json)", bindings_)) openBindings(ed);
        ImGui::Separator();
        for (auto& n : names_) {
            const bool cur = !bindings_ && n == sel_;
            if (ImGui::Selectable((n + (cur && dirty_ ? " *" : "")).c_str(), cur) && !cur) {
                if (dirty_) pendingSelect_ = n, ImGui::OpenPopup("##unsaved");
                else select(ed, n);
            }
        }
        if (ImGui::BeginPopup("##unsaved")) {
            ImGui::Text("В «%s» есть несохранённые изменения.", sel_.c_str());
            if (ImGui::Button("Сохранить")) { if (save(ed)) select(ed, pendingSelect_); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Отбросить")) { select(ed, pendingSelect_); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::Separator();
        if (ImGui::Button("+ Новый")) { newName_.clear(); ImGui::OpenPopup("##newfx"); }
        if (ImGui::BeginPopup("##newfx")) {
            ImGui::TextUnformatted("Имя файла (латиница, цифры, _):");
            ui::inputString("##name", newName_);
            const bool ok = validName(newName_) && std::find(names_.begin(), names_.end(), newName_) == names_.end();
            ImGui::BeginDisabled(!ok);
            if (ImGui::Button("Создать")) {
                if (writeText(fxDir(ed) / (newName_ + ".json"), kTemplate)) { refreshList(ed); select(ed, newName_); }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        if (!ed.fxWarnings.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Предупреждений: %d", int(ed.fxWarnings.size()));
            if (ImGui::BeginItemTooltip()) { for (auto& w : ed.fxWarnings) ImGui::TextUnformatted(w.c_str()); ImGui::EndTooltip(); }
        }
    }

    void openBindings(Editor& ed) {
        bindings_ = true;
        bindingsText_.clear();
        if (!readText(fxDir(ed) / "fx.json", bindingsText_)) bindingsText_ = "{\n  \"events\": {\n  }\n}\n";
        bindingsError_.clear();
    }

    void drawBindings(Editor& ed) {
        ImGui::TextWrapped("Какие эффекты играть на события симуляции. Порядок поиска: тип → оружие/способность → events.<событие>.<structure|air|unit> → events.<событие>. "
                           "Значение — имя файла эффекта, null (без эффекта) или { \"effect\", \"height\", \"scale\", \"teamColor\", \"below\" }.");
        ui::help("События: attack, hit, miss, died, spawned, ready, buildPlaced, buildDone, gathered, captured, levelUp, ability, "
                 "upgraded, sold, regionClaimed, wave, trail (шлейф снаряда), damaged (пока здоровье ниже below).");
        if (ImGui::Button("Проверить и сохранить")) {
            try {
                Json j = Json::parse(bindingsText_);
                if (!j.isObject()) throw std::runtime_error("ожидается объект { \"events\": ... }");
                if (writeText(fxDir(ed) / "fx.json", bindingsText_)) { ed.reloadFx(); bindingsError_.clear(); ed.status = "Сохранено: fx/fx.json"; }
            } catch (const std::exception& e) { bindingsError_ = e.what(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Перечитать")) openBindings(ed);
        if (!bindingsError_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "%s", bindingsError_.c_str());
        for (auto& w : ed.fxWarnings) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", w.c_str());
        ui::inputMultiline("##bindings", bindingsText_, ImVec2(-1, -1));
    }

    void drawEffect(Editor& ed) {
        // toolbar
        if (ImGui::Button("Повторить")) { preview_.clear(); handle_ = {}; nextPlay_ = 0; played_ = false; }
        ImGui::SameLine(); ImGui::Checkbox("Цикл", &loop_);
        ImGui::SameLine(); ImGui::Checkbox("Движение", &moving_);
        ui::help("Эмиттер летает по кругу: так видно шлейфы (perMeter) и эффекты, привязанные к объекту.");
        ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6); ImGui::SliderFloat("Скорость", &speed_, 0.1f, 2.0f, "%.1fx");
        ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11); ImGui::Combo("Качество", &quality_, kQualityLabels, 4);
        ui::help("Так эффект выглядит на этом уровне качества: меньше частиц, ограничен размер на экране, на «низк.» нет "
                 "сортировки и выключены эмиттеры с minQuality выше. Уровень в игре выбирается по GPU и меняется в меню паузы.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button(dirty_ ? "Сохранить *" : "Сохранить")) save(ed);
        ImGui::SameLine();
        if (ImGui::Button("Откатить")) select(ed, sel_);
        ImGui::EndDisabled();

        const auto& st = preview_.stats();
        const ImVec4 fillCol = st.fill > 2.5f ? ImVec4(1, 0.4f, 0.35f, 1) : st.fill > 1.0f ? ImVec4(1, 0.8f, 0.3f, 1) : ImVec4(0.6f, 0.85f, 0.6f, 1);
        ImGui::Text("Частиц: %d   вызовов отрисовки: %d   ", st.particles, st.drawCalls);
        ImGui::SameLine(0, 0);
        ImGui::TextColored(fillCol, "заполнение экрана: %.2fx", double(st.fill));
        ui::help("Сумма площадей частиц к площади кадра (перерисовка). Старым Mali/Adreno тяжело уже от 1–1.5x на эффект: "
                 "уменьшайте size, burst, sizeOverLife или поднимайте minQuality у второстепенных эмиттеров.");

        const float pw = ImGui::GetContentRegionAvail().x;
        const float ph = std::max(160.0f, std::min(ImGui::GetContentRegionAvail().y * 0.45f, pw * 0.6f));
        runPreview(ed, pw, ph);

        for (auto& e : errors_) ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "%s", e.c_str());
        if (ImGui::BeginTabBar("##fxtabs")) {
            if (ImGui::BeginTabItem("Параметры")) { ImGui::BeginChild("##params"); drawParams(); ImGui::EndChild(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("JSON")) { drawJson(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }

    void changed() {
        dirty_ = true;
        rawIsSource_ = false;
        errors_.clear();
        EffectDef check;   // round trip through the loader: the same errors the game would report
        fx::parseEffect(fx::effectToJson(def_), "fx/" + sel_ + ".json", check, errors_);
        jsonText_ = fx::effectToJson(def_).dump(2) + "\n";
        applyPreview();
        played_ = false;
    }

    void drawJson() {
        ImGui::TextDisabled("Правка текста. Комментарии сохраняются, если сохранять отсюда; правка во вкладке «Параметры» перезапишет файл без них.");
        if (ImGui::Button("Применить")) {
            EffectDef d;
            std::vector<std::string> errs;
            try { fx::parseEffect(Json::parse(jsonText_), "fx/" + sel_ + ".json", d, errs); }
            catch (const std::exception& e) { errs.push_back(e.what()); }
            errors_ = errs;
            if (!d.emitters.empty()) { def_ = d; dirty_ = true; rawIsSource_ = true; applyPreview(); played_ = false; }
        }
        ui::inputMultiline("##json", jsonText_, ImVec2(-1, -1));
    }

    void drawParams() {
        bool ch = false;
        int remove = -1, dup = -1;
        for (size_t i = 0; i < def_.emitters.size(); ++i) {
            EmitterDef& d = def_.emitters[i];
            ImGui::PushID(int(i));
            const std::string head = "Эмиттер " + std::to_string(i + 1) + " — " + d.texture + (d.blend == fx::FxBlend::Additive ? ", additive" : ", alpha");
            if (ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ch |= emitterUi(d);
                if (ImGui::SmallButton("Копия")) dup = int(i);
                ImGui::SameLine();
                if (ImGui::SmallButton("Удалить")) remove = int(i);
            }
            ImGui::PopID();
        }
        if (dup >= 0 && def_.emitters.size() < size_t(fx::kMaxEmitters)) { def_.emitters.insert(def_.emitters.begin() + dup + 1, def_.emitters[size_t(dup)]); ch = true; }
        if (remove >= 0 && def_.emitters.size() > 1) { def_.emitters.erase(def_.emitters.begin() + remove); ch = true; }
        ImGui::BeginDisabled(def_.emitters.size() >= size_t(fx::kMaxEmitters));
        if (ImGui::Button("+ Эмиттер")) { EmitterDef d; d.burst = 10; def_.emitters.push_back(d); ch = true; }
        ImGui::EndDisabled();
        if (ch) changed();
    }

    static bool range(const char* label, fx::FxRange& r, float speed, float lo, float hi, const char* fmt = "%.2f") {
        return ImGui::DragFloatRange2(label, &r.min, &r.max, speed, lo, hi, fmt, nullptr, ImGuiSliderFlags_AlwaysClamp);
    }

    bool emitterUi(EmitterDef& d) {
        bool ch = false;
        const float w = ImGui::GetFontSize() * 16;
        ImGui::PushItemWidth(w);
        // texture
        if (ImGui::BeginCombo("Текстура", d.texture.c_str())) {
            for (auto& b : fx::ParticleSystem::builtinTextures()) {
                std::string n = "builtin:" + b;
                if (ImGui::Selectable(n.c_str(), d.texture == n)) { d.texture = n; ch = true; }
            }
            ImGui::EndCombo();
        }
        ui::help("Встроенные: soft, spark, ring, smoke. Или путь к спрайту — он грузится той же функцией, что и спрайты игры.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        std::string tex = d.texture;
        if (ui::inputString("##texpath", tex, ImGuiInputTextFlags_EnterReturnsTrue) && !tex.empty()) { d.texture = tex; ch = true; }
        int frames[2] = {d.frameCols, d.frameRows};
        if (ImGui::InputInt2("Кадры (колонки, строки)", frames)) { d.frameCols = std::clamp(frames[0], 1, 16); d.frameRows = std::clamp(frames[1], 1, 16); ch = true; }
        if (d.frameCols * d.frameRows > 1) ch |= ImGui::SliderFloat("Кадров в секунду", &d.fps, 0, 60, d.fps == 0 ? "за жизнь" : "%.0f");
        int blend = int(d.blend);
        if (ImGui::Combo("Смешивание", &blend, "alpha (дым, пыль)\0additive (огонь, искры, свет)\0")) { d.blend = fx::FxBlend(blend); ch = true; }
        int align = int(d.align);
        if (ImGui::Combo("Ориентация", &align, "к камере\0по скорости (искры)\0по земле (кольца)\0")) { d.align = fx::FxAlign(align); ch = true; }
        if (d.align == fx::FxAlign::Velocity) ch |= ImGui::SliderFloat("Растяжение", &d.stretch, 0, 0.5f, "%.3f");

        ImGui::SeparatorText("Рождение");
        ch |= ImGui::SliderInt("Сразу (burst)", &d.burst, 0, 200);
        ch |= ImGui::SliderFloat("В секунду (rate)", &d.rate, 0, 200, "%.1f");
        ch |= ImGui::SliderFloat("На метр пути (perMeter)", &d.perMeter, 0, 40, "%.1f");
        ui::help("Для шлейфов: частицы оставляются по пути, сколько бы кадров ни прошло.");
        ch |= ImGui::SliderFloat("Задержка", &d.delay, 0, 5, "%.2f с");
        ch |= ImGui::SliderFloat("Длительность", &d.duration, 0, 10, "%.2f с");
        ImGui::SameLine();
        ch |= ImGui::Checkbox("Цикл##loop", &d.loop);
        int mq = std::max(1, int(d.minQuality)) - 1;
        if (ImGui::Combo("Начиная с качества", &mq, "низкого (всегда)\0среднего\0высокого\0")) { d.minQuality = fx::FxQuality(mq + 1); ch = true; }
        ui::help("Второстепенные эмиттеры (искры, ударные волны) лучше включать со «среднего»: на слабых телефонах их не будет.");

        ImGui::SeparatorText("Движение");
        ch |= range("Жизнь, с", d.life, 0.01f, 0.01f, 30);
        ch |= range("Скорость, м/с", d.speed, 0.05f, 0, 100);
        ch |= ImGui::DragFloat3("Направление", &d.direction.x, 0.02f, -1, 1, "%.2f");
        ch |= ImGui::SliderFloat("Разброс, °", &d.spread, 0, 180, "%.0f");
        int shape = int(d.shape);
        if (ImGui::Combo("Форма", &shape, "точка\0шар\0диск (XZ)\0кольцо (XZ)\0")) { d.shape = fx::FxShape(shape); ch = true; }
        if (d.shape != fx::FxShape::Point) ch |= ImGui::SliderFloat("Радиус, м", &d.radius, 0, 10, "%.2f");
        ch |= ImGui::DragFloat3("Гравитация", &d.gravity.x, 0.05f, -50, 50, "%.2f");
        ch |= ImGui::SliderFloat("Сопротивление", &d.drag, 0, 10, "%.2f");

        ImGui::SeparatorText("Вид");
        ch |= range("Размер, м", d.size, 0.01f, 0.01f, 50);
        ch |= range("Поворот, °", d.rotation, 1, -360, 360, "%.0f");
        ch |= range("Вращение, °/с", d.spin, 1, -720, 720, "%.0f");
        ch |= keysUi(d);
        ImGui::PopItemWidth();
        return ch;
    }

    bool keysUi(EmitterDef& d) {
        bool ch = false;
        ImGui::TextUnformatted("Цвет за жизнь");
        ui::help("Ключи: время 0..1 и цвет с прозрачностью. Последний ключ обычно с альфой 0 — частица гаснет, а не исчезает.");
        int del = -1;
        for (size_t k = 0; k < d.color.size(); ++k) {
            ImGui::PushID(int(100 + k));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
            ch |= ImGui::SliderFloat("##t", &d.color[k].t, 0, 1, "%.2f");
            ImGui::SameLine();
            ch |= ImGui::ColorEdit4("##c", &d.color[k].rgba.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoInputs);
            if (d.color.size() > 1) { ImGui::SameLine(); if (ImGui::SmallButton("x")) del = int(k); }
            ImGui::PopID();
        }
        if (del >= 0) { d.color.erase(d.color.begin() + del); ch = true; }
        if (ImGui::SmallButton("+ цвет")) { auto k = d.color.back(); k.t = std::min(1.0f, k.t + 0.25f); d.color.push_back(k); ch = true; }

        ImGui::TextUnformatted("Размер за жизнь (множитель)");
        del = -1;
        for (size_t k = 0; k < d.sizeOverLife.size(); ++k) {
            ImGui::PushID(int(200 + k));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
            ch |= ImGui::SliderFloat("##t", &d.sizeOverLife[k].t, 0, 1, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
            ch |= ImGui::SliderFloat("##k", &d.sizeOverLife[k].k, 0, 6, "x%.2f");
            if (d.sizeOverLife.size() > 1) { ImGui::SameLine(); if (ImGui::SmallButton("x")) del = int(k); }
            ImGui::PopID();
        }
        if (del >= 0) { d.sizeOverLife.erase(d.sizeOverLife.begin() + del); ch = true; }
        if (ImGui::SmallButton("+ ключ")) { auto k = d.sizeOverLife.back(); k.t = std::min(1.0f, k.t + 0.25f); d.sizeOverLife.push_back(k); ch = true; }
        if (ch) {
            std::stable_sort(d.color.begin(), d.color.end(), [](auto& a, auto& b) { return a.t < b.t; });
            std::stable_sort(d.sizeOverLife.begin(), d.sizeOverLife.end(), [](auto& a, auto& b) { return a.t < b.t; });
        }
        return ch;
    }

    gfx::Device* dev_ = nullptr;
    fx::ParticleSystem preview_;
    render::TextureCache textures_;
    Viewport vp_;
    render::RTSCamera cam_{{0, 0.8f, 0}, 9.0f, 0.45f, 0.7f};
    std::string project_, sel_, pendingSelect_, newName_, jsonText_, bindingsText_, bindingsError_;
    std::vector<std::string> names_, errors_;
    EffectDef def_;
    int previewId_ = -1, quality_ = 3;
    fx::FxHandle handle_;
    double nextPlay_ = 0, lastTime_ = -1;
    float speed_ = 1, moveT_ = 0;
    bool listDirty_ = true, bindings_ = false, dirty_ = false, rawIsSource_ = true, loop_ = true, moving_ = false, played_ = false;
    int revision_ = 0;
};

RTS_EDITOR_WINDOW(FxWindow);

} // namespace rts::editor
