// Icons and splash ("Иконки и заставка"): game.json "icons" and "splash" (docs/game-format.md).
// Pick images from the game folder, see every generated icon at its real pixel size (iOS home
// screen mask, Android adaptive circle / squircle / rounded), and play the splash animation.
// Everything is written through ws().call("set_manifest"), like the Build window does.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

#include "../Editor.h"
#include "../EditorUtil.h"
#include "tools/branding/Icons.h"
#include "tools/branding/Splash.h"

namespace fs = std::filesystem;

namespace rts::editor {

namespace {
struct Tex { gfx::TextureHandle h; int w = 0, h2 = 0; };
} // namespace

class BrandingWindow : public EditorWindow {
public:
    const char* id() const override { return "branding"; }
    const char* title() const override { return "Иконки и заставка"; }
    const char* dockArea() const override { return "center"; }
    int order() const override { return 6; }
    bool openByDefault() const override { return false; }

    void update(Editor& ed, double dt) override {
        if (playing_) {
            time_ += float(dt);
            const float len = std::max(2.0f, std::max(spec_.sequenceLength(), spec_.minimumTime()) + 1.0f);
            if (time_ > len) time_ = 0;
            splashDirty_ = true;
        }
        if (open && project_ != ed.ws().project.dir) { project_ = ed.ws().project.dir; reload(ed); }
    }

    void draw(Editor& ed) override {
        auto& ws = ed.ws();
        if (ws.project.dir.empty()) { ImGui::TextDisabled("Откройте игру (Игра -> Открыть)"); return; }
        if (ImGui::Button("Обновить")) reload(ed);
        ImGui::SameLine();
        ImGui::TextDisabled("game.json: icons, splash · сборка: rtsbuild генерирует все размеры для каждой платформы");
        if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", error_.c_str());
        for (auto& w : warnings_) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", w.c_str());
        if (ImGui::BeginTabBar("##brand")) {
            if (ImGui::BeginTabItem("Иконки")) { icons(ed); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Заставка")) { splashTab(ed); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }

private:
    // ------------------------------------------------------------ data
    void reload(Editor& ed) {
        auto& ws = ed.ws();
        error_.clear();
        warnings_.clear();
        images_.clear();
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(ws.project.dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::string e = it->path().extension().string();
            std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (it->is_regular_file(ec) && (e == ".png" || e == ".jpg" || e == ".jpeg"))
                images_.push_back(fs::relative(it->path(), ws.project.dir, ec).generic_string());
        }
        std::sort(images_.begin(), images_.end());
        try {
            iconSpec_ = branding::parseIcons(ws.project.manifest);
            if (iconSpec_.master.empty() && fs::exists(fs::path(ws.project.dir) / "icon.png")) iconSpec_.master = "icon.png";
            for (auto& w : branding::validateIcons(iconSpec_, ws.project.dir)) warnings_.push_back(w);
            spec_ = splash::parseSpec(ws.project.manifest["splash"], &warnings_);
            splashImgs_ = branding::loadSplashImages(spec_, ws.project.dir);
            for (auto& w : splashImgs_.warnings) warnings_.push_back(w);
            for (auto& layer : splashImgs_.frames)   // the preview is small: keep the CPU raster cheap
                for (auto& im : layer)
                    if (std::max(im.width, im.height) > 384) im = branding::fitContain(im, 384, 384);
        } catch (const std::exception& e) {
            error_ = e.what();
        }
        splashJson_ = ws.project.manifest["splash"].isNull() ? std::string("{\n}") : ws.project.manifest["splash"].dump(2);
        iconsDirty_ = splashDirty_ = true;
    }

    // null removes the key (its parent object is written back without it)
    void save(Editor& ed, const std::string& path, const Json& value) {
        if (value.isNull()) {
            size_t dot = path.rfind('.');
            if (dot == std::string::npos) return;
            const Json* parent = jsonPath(ed.ws().project.manifest, path.substr(0, dot));
            if (!parent || !parent->isObject()) return;
            Json copy = *parent;
            copy.erase(path.substr(dot + 1));
            save(ed, path.substr(0, dot), copy);
            return;
        }
        Json p = Json::object();
        p.set("path", Json::string(path));
        p.set("value", value);
        Json r = ed.ws().call("set_manifest", p);
        if (r.has("error")) error_ = r.getStr("error");
        reload(ed);
    }

    // image path picker: text field + combo of the game's images
    bool pickImage(const char* label, std::string& path) {
        bool changed = false;
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo(label, path.empty() ? "(нет)" : path.c_str())) {
            if (ImGui::Selectable("(нет)", path.empty())) { path.clear(); changed = true; }
            for (auto& im : images_)
                if (ImGui::Selectable(im.c_str(), im == path)) { path = im; changed = true; }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }

    void textureFrom(Editor& ed, Tex& t, const branding::Image& img) {
        auto& dev = ed.device();
        if (t.h.valid() && (t.w != img.width || t.h2 != img.height)) { dev.destroyTexture(t.h); t.h = {}; }
        if (!t.h.valid()) {
            gfx::TextureDesc d;
            d.width = img.width; d.height = img.height; d.data = img.rgba.data(); d.label = "branding preview";
            t.h = dev.createTexture(d);
            t.w = img.width; t.h2 = img.height;
        } else {
            dev.updateTexture(t.h, 0, 0, img.width, img.height, img.rgba.data(), img.width * 4);
        }
    }

    // ------------------------------------------------------------ icons
    void icons(Editor& ed) {
        const Json& m = ed.ws().project.manifest;
        const Json& ic = m["icons"];
        ImGui::SeparatorText("Основная картинка");
        std::string master = ic.isObject() ? ic.getStr("default", m.getStr("icon")) : m.getStr("icon");
        if (pickImage("Мастер (от 1024x1024)", master)) save(ed, "icons.default", Json::string(master));
        ui::help("Квадратная картинка не меньше 1024 px; из неё сборка делает все размеры. Можно задать отдельные картинки для платформ ниже.");
        splash::Color bg{0, 0, 0, 1};
        bool hasBg = splash::parseColor(ic["background"], bg);
        float c3[3] = {bg.r, bg.g, bg.b};
        if (ImGui::Checkbox("Фон для непрозрачных иконок", &hasBg)) save(ed, "icons.background", hasBg ? Json::string(splash::colorHex(bg)) : Json());
        if (hasBg) {
            ImGui::SameLine();
            if (ImGui::ColorEdit3("##bg", c3, ImGuiColorEditFlags_NoInputs)) {}
            if (ImGui::IsItemDeactivatedAfterEdit()) save(ed, "icons.background", Json::string(splash::colorHex({c3[0], c3[1], c3[2], 1})));
        }
        ui::help("iOS, apple-touch, maskable PWA и обычная иконка Android не могут быть прозрачными: прозрачность заливается этим цветом (иначе — цветом края картинки).");
        float padding = float(ic["padding"].toDouble(0)), radius = float(ic["cornerRadius"].toDouble(0));
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Отступ", &padding, 0, 0.4f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) save(ed, "icons.padding", Json::number(double(padding), 3));
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Скругление (Android)", &radius, 0, 0.5f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) save(ed, "icons.cornerRadius", Json::number(double(radius), 3));

        if (ImGui::CollapsingHeader("Картинки для платформ")) {
            for (const char* p : {"ios", "macos", "windows", "web", "linux"}) {
                const Json& v = ic[p];
                std::string path = v.isString() ? v.str() : v.getStr("image");
                if (pickImage(p, path)) save(ed, std::string("icons.") + p + (v.isObject() ? ".image" : ""), path.empty() ? Json() : Json::string(path));
            }
            ImGui::SeparatorText("Android (адаптивная иконка)");
            for (const char* k : {"foreground", "monochrome", "legacy"}) {
                std::string path = ic["android"][k].isString() ? ic["android"][k].str() : ic["android"][k].getStr("image");
                if (pickImage(k, path)) save(ed, std::string("icons.android.") + k, path.empty() ? Json() : Json::string(path));
            }
            ui::help("foreground — слой 108dp (видна середина 72dp), monochrome — силуэт для тематических иконок Android 13, legacy — обычная квадратная иконка.");
            splash::Color abg{1, 1, 1, 1};
            bool has = splash::parseColor(ic["android"]["background"], abg);
            float a3[3] = {abg.r, abg.g, abg.b};
            ImGui::ColorEdit3("Фон адаптивной иконки", a3, ImGuiColorEditFlags_NoInputs);
            if (ImGui::IsItemDeactivatedAfterEdit()) save(ed, "icons.android.background", Json::string(splash::colorHex({a3[0], a3[1], a3[2], 1})));
            if (!has) { ImGui::SameLine(); ImGui::TextDisabled("(не задан)"); }
        }

        ImGui::SeparatorText("Как это будет выглядеть (реальный размер в пикселях)");
        if (iconsDirty_) {
            iconsDirty_ = false;
            for (auto& t : iconTex_) if (t.second.h.valid()) ed.device().destroyTexture(t.second.h);
            iconTex_.clear();
            previews_ = branding::previewIcons(iconSpec_, ed.ws().project.dir, nullptr);
            for (size_t i = 0; i < previews_.size(); ++i) textureFrom(ed, iconTex_[int(i)], previews_[i].image);
        }
        std::string platform;
        for (size_t i = 0; i < previews_.size(); ++i) {
            auto& p = previews_[i];
            if (p.platform != platform) { platform = p.platform; ImGui::TextColored(ImVec4(1, 0.85f, 0.45f, 1), "%s", platform.c_str()); }
            else ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Image(ImTextureRef(ImTextureID(iconTex_[int(i)].h.id)), ImVec2(float(p.size), float(p.size)));
            ImGui::TextDisabled("%s", p.label.c_str());
            ImGui::EndGroup();
            if (i + 1 < previews_.size() && previews_[i + 1].platform == platform && ImGui::GetItemRectMax().x + previews_[i + 1].size > ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 20) ImGui::NewLine();
        }
    }

    // ------------------------------------------------------------ splash
    void splashTab(Editor& ed) {
        const Json& sp = ed.ws().project.manifest["splash"];
        if (sp.isNull()) {
            ImGui::TextDisabled("У игры нет заставки — при загрузке показывается только фон и полоса.");
            if (ImGui::Button("Добавить заставку")) {
                Json s = Json::object();
                s.set("background", Json::string("#0f1214"));
                std::string first = iconSpec_.master;
                if (!first.empty()) {
                    Json logos = Json::array(), l = Json::object(), a = Json::object();
                    l.set("image", Json::string(first));
                    l.set("scale", Json::number(0.4, 2));
                    a.set("fadeIn", Json::number(0.5, 2));
                    a.set("pulse", Json::number(0.03, 2));
                    l.set("animation", a);
                    logos.push(l);
                    s.set("logos", logos);
                }
                save(ed, "splash", s);
            }
            return;
        }
        if (ImGui::BeginTable("##splash", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("settings", ImGuiTableColumnFlags_WidthFixed, 380);
            ImGui::TableSetupColumn("preview", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextColumn();
            splashSettings(ed, sp);
            ImGui::TableNextColumn();
            splashPreview(ed);
            ImGui::EndTable();
        }
    }

    void splashSettings(Editor& ed, const Json& sp) {
        splash::Color bg = spec_.background;
        float c3[3] = {bg.r, bg.g, bg.b};
        ImGui::ColorEdit3("Фон", c3, ImGuiColorEditFlags_NoInputs);
        if (ImGui::IsItemDeactivatedAfterEdit()) save(ed, "splash.background", Json::string(splash::colorHex({c3[0], c3[1], c3[2], 1})));
        std::string image = sp.getStr("image");
        if (pickImage("Картинка фона", image)) save(ed, "splash.image", image.empty() ? Json() : Json::string(image));
        static const char* fits[] = {"contain", "cover", "center", "stretch"};
        int fit = 1;
        for (int i = 0; i < 4; ++i) if (sp.getStr("fit", "cover") == fits[i]) fit = i;
        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("Вписать", &fit, fits, 4)) save(ed, "splash.fit", Json::string(fits[fit]));
        float minS = float(sp["minSeconds"].toDouble(-1)), maxS = float(sp["maxSeconds"].toDouble(0));
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Минимум, с", &minS, 0.05f, -1, 20, minS < 0 ? "до конца логотипов" : "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) save(ed, "splash.minSeconds", Json::number(double(minS), 2));
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Максимум, с", &maxS, 0.05f, 0, 30, maxS <= 0 ? "без ограничения" : "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) save(ed, "splash.maxSeconds", Json::number(double(maxS), 2));
        ui::help("Заставка уходит, когда всё загружено и прошло минимальное время (по умолчанию — пока не проиграются все логотипы). Максимум ограничивает минимум, но не загрузку.");
        bool skip = sp.getBool("skippable", true), loop = sp.getBool("loop", false);
        if (ImGui::Checkbox("Пропуск касанием", &skip)) save(ed, "splash.skippable", Json::fromBool(skip));
        ImGui::SameLine();
        if (ImGui::Checkbox("Повторять, пока грузится", &loop)) save(ed, "splash.loop", Json::fromBool(loop));
        bool bar = spec_.progress.show;
        if (ImGui::Checkbox("Полоса загрузки", &bar)) save(ed, "splash.progress.show", Json::fromBool(bar));
        if (bar) {
            ImGui::SameLine();
            float pc[3] = {spec_.progress.color.r, spec_.progress.color.g, spec_.progress.color.b};
            ImGui::ColorEdit3("##bar", pc, ImGuiColorEditFlags_NoInputs);
            if (ImGui::IsItemDeactivatedAfterEdit()) save(ed, "splash.progress.color", Json::string(splash::colorHex({pc[0], pc[1], pc[2], 1})));
        }
        ImGui::SeparatorText("Весь блок (логотипы, анимация)");
        ui::inputMultiline("##json", splashJson_, ImVec2(-1, 260));
        if (ImGui::Button("Применить JSON")) {
            try { save(ed, "splash", Json::parse(splashJson_)); } catch (const std::exception& e) { error_ = std::string("splash JSON: ") + e.what(); }
        }
        ui::help("logos: [{image | frames | sheet+frame+count+fps, start, duration, scale, anchor, fit, animation}]; animation — ключи [{t, opacity, scale, x, y, rotate, ease}] или {fadeIn, fadeOut, pulse, rotate, slide, zoom}. Подробно: docs/game-format.md, «Icons and splash».");
    }

    void splashPreview(Editor& ed) {
        if (ImGui::Button(playing_ ? "Пауза" : "Играть")) { playing_ = !playing_; if (playing_ && time_ <= 0) time_ = 0; }
        ImGui::SameLine();
        if (ImGui::Button("С начала")) { time_ = 0; splashDirty_ = true; }
        ImGui::SameLine();
        float len = std::max(2.0f, std::max(spec_.sequenceLength(), spec_.minimumTime()) + 1.0f);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##t", &time_, 0, len, "%.2f с")) splashDirty_ = true;
        const bool portrait = ed.ws().project.manifest.getStr("orientation") == "portrait";
        const float availW = std::max(160.0f, ImGui::GetContentRegionAvail().x);
        const int W = portrait ? 270 : 480, H = portrait ? 480 : 270;
        if (splashDirty_ && spec_.enabled) {
            splashDirty_ = false;
            float minT = std::max(0.5f, spec_.minimumTime());
            textureFrom(ed, splashTex_, branding::renderSplashFrame(spec_, splashImgs_, time_, std::min(1.0f, time_ / minT), W, H));
        }
        if (splashTex_.h.valid()) {
            float k = std::min(1.0f, availW / float(W));
            ImGui::Image(ImTextureRef(ImTextureID(splashTex_.h.id)), ImVec2(W * k, H * k));
        }
        ImGui::TextDisabled("Полоса загрузки показана так, будто загрузка длится минимальное время (%.1f с).", double(spec_.minimumTime()));
        branding::NativeSplash n = branding::nativeSplash(ed.ws().project.manifest);
        ImGui::TextDisabled("Экран запуска ОС (iOS, Android, web): %s на %s", n.image.empty() ? "без картинки" : n.image.c_str(), splash::colorHex(n.background).c_str());
    }

    std::string project_, error_, splashJson_;
    std::vector<std::string> warnings_, images_;
    branding::IconSpec iconSpec_;
    std::vector<branding::IconPreview> previews_;
    std::map<int, Tex> iconTex_;
    splash::Spec spec_;
    branding::SplashImages splashImgs_;
    Tex splashTex_;
    float time_ = 0;
    bool playing_ = false, iconsDirty_ = true, splashDirty_ = true;
};

RTS_EDITOR_WINDOW(BrandingWindow);

} // namespace rts::editor
