// Build: everything about a build before it is made. Platform and configuration, quality
// settings (pack compression, texture atlases, sound profile, split/download), an estimate of the
// build size (rtsbuild --estimate: the real content pack, no app build), the heaviest files, and
// per-file settings: zstd level or store, image size per platform, leave a file out. Settings are
// stored in game.json ("packaging", "atlas"), so builds from the command line and CI match.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <filesystem>
#include "../Editor.h"
#include "../EditorUtil.h"

namespace fs = std::filesystem;

namespace rts::editor {

namespace {
struct Target { const char* id; const char* name; const char* audio; double engineMB; double storeLimitMB; const char* storeNote; };
// Engine/runtime size without content (measured Release builds, rounded); store limits are where
// the platform starts to make life harder (cellular download, base module, web first load).
const Target kTargets[] = {
    {"macos", "macOS (.app)", "desktop", 16, 0, ""},
    {"ios", "iOS", "mobile", 9, 200, "App Store: больше 200 МБ не качается по сотовой сети без подтверждения"},
    {"android", "Android", "mobile", 12, 200, "Google Play: базовый модуль AAB до 200 МБ (остальное — докачкой)"},
    {"web", "Web (HTML5)", "web", 6, 30, "Web: первая загрузка больше ~30 МБ заметно теряет игроков"},
    {"windows", "Windows (.exe)", "desktop", 9, 0, ""},
    {"linux", "Linux", "desktop", 9, 0, ""},
};
constexpr int kTargetCount = int(sizeof(kTargets) / sizeof(kTargets[0]));

std::string mb(double bytes) {
    char b[32];
    if (bytes >= 1048576) std::snprintf(b, sizeof b, "%.1f МБ", bytes / 1048576);
    else if (bytes >= 1024) std::snprintf(b, sizeof b, "%.0f КБ", bytes / 1024);
    else std::snprintf(b, sizeof b, "%.0f Б", bytes);
    return b;
}
std::string quote(const std::string& s) {
#if defined(_WIN32)
    return "\"" + s + "\"";
#else
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
#endif
}
bool isImage(const std::string& p) {
    auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = p.substr(dot);
    for (auto& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
}
} // namespace

class BuildWindow : public EditorWindow {
public:
    const char* id() const override { return "build"; }
    const char* title() const override { return "Сборка"; }
    const char* dockArea() const override { return "center"; }
    int order() const override { return 5; }
    bool openByDefault() const override { return false; }

    void update(Editor& ed, double dt) override {
        if (estimating_ && !ed.ws().building()) { estimating_ = false; loadReports(ed); }
        if (dirtyIn_ > 0 && (dirtyIn_ -= dt) <= 0 && autoEstimate_ && open) estimate(ed);
        // first look at the window: estimate right away, the numbers are the point of it
        if (open && !ed.ws().project.dir.empty() && reports_.find(kTargets[target_].id) == reports_.end() && !estimating_ && !ed.ws().building() && askedFor_ != target_ * 1000 + int(ed.ws().project.dir.size())) {
            askedFor_ = target_ * 1000 + int(ed.ws().project.dir.size());
            estimate(ed);
        }
    }

    void draw(Editor& ed) override {
        auto& ws = ed.ws();
        if (ws.project.dir.empty()) { ui::emptyStateHere(ICON_BUILD, "Игра не открыта", "Игра → Открыть"); return; }
        if (lastProject_ != ws.project.dir) { lastProject_ = ws.project.dir; reports_.clear(); }
        header(ed);
        ImGui::Separator();
        if (ImGui::BeginTable("##build", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("settings", ImGuiTableColumnFlags_WidthFixed, 300);
            ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextColumn();
            ImGui::BeginChild("##settings", ImVec2(0, 0));
            settings(ed);
            ImGui::EndChild();
            ImGui::TableNextColumn();
            ImGui::BeginChild("##report", ImVec2(0, 0));
            report(ed);
            ImGui::EndChild();
            ImGui::EndTable();
        }
    }

private:
    // ------------------------------------------------------------ top bar
    void header(Editor& ed) {
        auto& ws = ed.ws();
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("##target", kTargets[target_].name)) {
            for (int i = 0; i < kTargetCount; ++i)
                if (ImGui::Selectable(kTargets[i].name, i == target_)) { target_ = i; touched(); }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::RadioButton("Release", &release_, 1); ImGui::SameLine(); ImGui::RadioButton("Debug", &release_, 0);
        ImGui::SameLine(0, 20);
        ImGui::BeginDisabled(ws.building());
        if (ui::button(ICON_HARD_DRIVE, "Оценить размер")) estimate(ed);
        ImGui::SameLine();
        if (ui::button(ICON_BUILD, ("Собрать: " + std::string(kTargets[target_].name)).c_str(), true)) {
            ws.startBuild(kTargets[target_].id, release_ ? "Release" : "Debug", buildFlags());
            if (auto* c = ed.window("console")) c->open = true;
        }
        ImGui::SameLine();
        if (ui::button(ICON_LAYOUT, "Все платформы")) {
            ws.startBuild("all", release_ ? "Release" : "Debug", buildFlags());
            if (auto* c = ed.window("console")) c->open = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Пересчитывать сразу", &autoEstimate_);
        ui::help("После каждого изменения настроек размер оценивается заново (собирается только пак контента, без приложения — секунды).");
        if (ws.building()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s %s", ICON_REFRESH, estimating_ ? "оценка…" : "идёт сборка…"); }
    }

    std::string buildFlags() const {
        std::string f;
        if (audio_ != 0) { static const char* a[] = {"auto", "desktop", "mobile", "web", "off"}; f += " --audio " + std::string(a[audio_]); }
        return f;
    }

    void estimate(Editor& ed) {
        auto& ws = ed.ws();
        if (ws.building()) { dirtyIn_ = 0.5; return; }
        outDir_ = (fs::path(ws.project.dir).parent_path() / "dist" / "_estimate").string();
        ws.startBuild(kTargets[target_].id, "Release", "--estimate --out " + quote(outDir_) + buildFlags());
        estimating_ = true;
        dirtyIn_ = 0;
    }

    void loadReports(Editor& ed) {
        for (auto& t : kTargets) {
            fs::path p = fs::path(outDir_) / t.id / "size-report.json";
            std::error_code ec;
            if (!fs::exists(p, ec)) continue;
            try { reports_[t.id] = Json::parseFile(p.string()); } catch (const std::exception&) {}
        }
        if (ed.ws().buildResult() != 0) error_ = "оценка не удалась — подробности в Console"; else error_.clear();
    }

    void touched() { dirtyIn_ = 0.8; }

    // ------------------------------------------------------------ game.json helpers
    Json& manifest(Editor& ed) { return ed.ws().project.manifest; }
    void save(Editor& ed, const std::string& path, const Json& value) {
        Json p = Json::object();
        p.set("path", Json::string(path));
        p.set("value", value);
        Json r = ed.ws().call("set_manifest", p);
        if (r.has("error")) error_ = r.getStr("error");
        touched();
    }
    // packaging.files (all platforms) or packaging.targets.<t>.files (this platform)
    std::string rulesPath() const { return perTarget_ ? std::string("packaging.targets.") + kTargets[target_].id + ".files" : "packaging.files"; }
    Json rulesList(Editor& ed) {
        const Json* j = jsonPath(manifest(ed), rulesPath());
        return j && j->isArray() ? *j : Json::array();
    }
    const Json* ruleFor(const Json& list, const std::string& path) {
        for (auto& r : list.items()) if (r.getStr("match") == path) return &r;
        return nullptr;
    }
    void setRule(Editor& ed, const std::string& path, const char* key, const Json& value) {
        Json list = rulesList(ed);
        Json out = Json::array();
        bool found = false;
        for (auto& r : list.items()) {
            if (r.getStr("match") != path) { out.push(r); continue; }
            found = true;
            Json n = r;
            if (value.isNull()) n.erase(key); else n.set(key, value);
            if (n.size() > 1) out.push(n);      // only "match" left: the rule is gone
        }
        if (!found && !value.isNull()) {
            Json n = Json::object();
            n.set("match", Json::string(path));
            n.set(key, value);
            out.push(n);
        }
        save(ed, rulesPath(), out);
    }

    // ------------------------------------------------------------ left column
    void settings(Editor& ed) {
        Json& m = manifest(ed);
        ImGui::SeparatorText("Сжатие пака");
        int level = int(m["packaging"]["level"].integer(19));
        ImGui::SetNextItemWidth(180);
        if (ImGui::SliderInt("zstd", &level, 1, 22)) {}
        if (ImGui::IsItemDeactivatedAfterEdit()) save(ed, "packaging.level", Json::number(int64_t(level)));
        ui::help("Уровень сжатия ресурс-пака для всех файлов (19 — хороший баланс; 22 — меньше, но сборка дольше; на скорость загрузки в игре почти не влияет). Отдельным файлам можно задать свой уровень в таблице справа.");

        ImGui::SeparatorText("Атласы текстур");
        bool atlas = m["atlas"].getBool("enable", true);
        if (ImGui::Checkbox("Собирать атласы", &atlas)) save(ed, "atlas.enable", Json::fromBool(atlas));
        ui::help("Мелкие 2D-картинки (UI, спрайты, тайлы) собираются в страницы-атласы: меньше переключений текстур и меньше пак.");
        const Json& mp = m["atlas"]["maxPage"];
        const char* tid = kTargets[target_].id;
        int page = int(mp.isNumber() ? mp.integer() : mp.has(tid) ? mp[tid].integer() : mp.has("default") ? mp["default"].integer()
                                                                  : (std::string(tid) == "web" || std::string(tid) == "android" ? 2048 : 4096));
        static const int pages[] = {1024, 2048, 4096, 8192};
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("Страница", std::to_string(page).c_str())) {
            for (int pg : pages)
                if (ImGui::Selectable(std::to_string(pg).c_str(), pg == page)) save(ed, std::string("atlas.maxPage.") + tid, Json::number(int64_t(pg)));
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::TextDisabled("для %s", kTargets[target_].name);

        ImGui::SeparatorText("Звук");
        static const char* audios[] = {"по платформе", "desktop", "mobile", "web", "без пережатия"};
        ImGui::SetNextItemWidth(160);
        if (ImGui::Combo("Профиль", &audio_, audios, 5)) touched();
        ui::help("Звуки и музыка пережимаются при сборке по профилю платформы (audio.json -> build). Качество профилей задаётся там же.");
        ImGui::TextDisabled("по платформе: %s", kTargets[target_].audio);

        ImGui::SeparatorText("Докачка (разбиение)");
        const Json& pt = m["packaging"]["targets"][tid];
        bool split = pt.has("split") ? pt.getBool("split", false) : m["packaging"].getBool("split", false);
        if (ImGui::Checkbox("Делить на базу и докачку", &split)) save(ed, std::string("packaging.targets.") + tid + ".split", Json::fromBool(split));
        ui::help("В сборке остаётся только нужное для старта, остальное — части для CDN, игра докачивает их сама (packaging.sources).");
        if (split) {
            std::string base = pt.has("baseSize") ? pt.getStr("baseSize") : m["packaging"].getStr("baseSize", "");
            ImGui::SetNextItemWidth(100);
            if (ui::inputString("База, не больше", base, ImGuiInputTextFlags_EnterReturnsTrue)) save(ed, std::string("packaging.targets.") + tid + ".baseSize", Json::string(base));
            std::string chunk = pt.has("chunkSize") ? pt.getStr("chunkSize") : m["packaging"].getStr("chunkSize", "8MB");
            ImGui::SetNextItemWidth(100);
            if (ui::inputString("Часть", chunk, ImGuiInputTextFlags_EnterReturnsTrue)) save(ed, std::string("packaging.targets.") + tid + ".chunkSize", Json::string(chunk));
            ImGui::TextDisabled("размеры: 40MB, 512KB, 1GB (Enter — сохранить)");
        }

        ImGui::SeparatorText("Правила файлов");
        ImGui::Checkbox("Только для этой платформы", &perTarget_);
        ui::help("Выключено: настройки файлов в таблице действуют на все платформы (packaging.files). Включено: только на выбранную (packaging.targets.<платформа>.files) — например, уменьшить текстуры только на Android.");
        Json list = rulesList(ed);
        if (list.size() == 0) ImGui::TextDisabled("нет — всё по умолчанию");
        for (size_t i = 0; i < list.size(); ++i) {
            const Json& r = list[i];
            std::string d = r.getStr("match") + ":";
            if (r.getBool("exclude", false)) d += " исключён";
            if (r.has("level")) d += r["level"].integer() == 0 ? " без сжатия" : " zstd " + std::to_string(r["level"].integer());
            if (r.has("maxSize")) d += " до " + std::to_string(r["maxSize"].integer()) + " px";
            if (r.has("scale")) { char b[24]; std::snprintf(b, sizeof b, " x%.2g", r["scale"].toDouble()); d += b; }
            ImGui::PushID(int(i));
            if (ImGui::SmallButton("x")) {
                Json out = Json::array();
                for (size_t k = 0; k < list.size(); ++k) if (k != i) out.push(list[k]);
                save(ed, rulesPath(), out);
            }
            ImGui::SameLine();
            ImGui::TextWrapped("%s", d.c_str());
            ImGui::PopID();
        }
        if (!error_.empty()) { ImGui::Spacing(); ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str()); }
    }

    // ------------------------------------------------------------ right column
    void report(Editor& ed) {
        const Target& t = kTargets[target_];
        auto it = reports_.find(t.id);
        if (it == reports_.end()) {
            ImGui::TextWrapped("Размер ещё не оценён для %s. Нажмите «Оценить размер»: соберётся только пак контента с текущими настройками (атласы, звук, правила файлов), без сборки приложения.", t.name);
            return;
        }
        const Json& r = it->second;
        const Json& pk = r["pack"];
        const double pack = pk["packBytes"].toDouble(), raw = pk["rawBytes"].toDouble();
        const Json& sp = r["split"];
        const double shipped = sp.isObject() ? sp["baseBytes"].toDouble() : pack;
        const double engine = t.engineMB * 1048576;
        ImGui::SeparatorText((std::string("Оценка: ") + t.name).c_str());
        ImGui::Text("Сборка ~ %s", mb(shipped + engine).c_str());
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("контент %s + движок ~ %s", mb(shipped).c_str(), mb(engine).c_str());
        ImGui::Text("Исходные файлы %s -> в паке %s (%.0f%%), файлов %lld, повторов %lld", mb(raw).c_str(), mb(pack).c_str(),
                    raw > 0 ? pack / raw * 100 : 0, (long long)pk["entries"].integer(), (long long)pk["duplicates"].integer());
        if (sp.isObject())
            ImGui::Text("Докачка: %lld част., всего с докачкой %s", (long long)sp["chunks"].size(), mb(sp["totalBytes"].toDouble()).c_str());
        if (t.storeLimitMB > 0 && (shipped + engine) / 1048576 > t.storeLimitMB)
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", t.storeNote);
        const Json& at = r["atlas"];
        if (at.getBool("enabled", false) && at["pages"].integer() > 0) {
            ImGui::SeparatorText("Атласы");
            ImGui::Text("%lld текстур -> %lld стр., заполнение %.0f%%, экономия в паке %s", (long long)at["atlased"].integer(), (long long)at["pages"].integer(),
                        at["occupancyPercent"].toDouble(), mb(at["bytesSaved"].toDouble()).c_str());
            ImGui::Text("Видеопамять %s -> %s, переключений текстур %lld -> %lld", mb(at["gpuBytesBefore"].toDouble()).c_str(), mb(at["gpuBytesAfter"].toDouble()).c_str(),
                        (long long)at["textureBindsBefore"].integer(), (long long)at["textureBindsAfter"].integer());
            if (at["excluded"].size() && ImGui::TreeNode("Не в атласе")) {
                for (auto& e : at["excluded"].items()) ImGui::BulletText("%s — %s", e.getStr("path").c_str(), e.getStr("reason").c_str());
                ImGui::TreePop();
            }
        }
        if (r["fileRules"].size()) {
            ImGui::SeparatorText("Что сделали правила файлов");
            for (auto& e : r["fileRules"].items())
                ImGui::BulletText("%s: %s%s", e.getStr("path").c_str(), e.getStr("what").c_str(),
                                  e["after"].integer() ? (" (" + mb(e["before"].toDouble()) + " -> " + mb(e["after"].toDouble()) + ")").c_str() : "");
        }
        files(ed, r);
    }

    void files(Editor& ed, const Json& r) {
        ImGui::SeparatorText("Файлы (самые тяжёлые сверху)");
        ImGui::SetNextItemWidth(220);
        ui::inputString("##filefilter", filter_);
        if (filter_.empty()) { ImGui::SameLine(14); ImGui::TextDisabled("фильтр: sprites/, .ogg ..."); }
        struct Row { std::string path, codec; double raw, stored; };
        std::vector<Row> rows;
        for (auto& f : r["files"].items()) {
            std::string p = f.getStr("path");
            if (!filter_.empty() && p.find(filter_) == std::string::npos) continue;
            rows.push_back({p, f.getStr("codec"), f["rawBytes"].toDouble(), f["storedBytes"].toDouble()});
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.stored > b.stored; });
        double total = 0;
        for (auto& rw : rows) total += rw.stored;
        Json list = rulesList(ed);
        const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
        if (!ImGui::BeginTable("##files", 6, tf, ImVec2(0, 0))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Файл", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Исходный", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("В паке", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Сжатие", ImGuiTableColumnFlags_WidthFixed, 118);
        ImGui::TableSetupColumn("Размер картинки", ImGuiTableColumnFlags_WidthFixed, 108);
        ImGui::TableSetupColumn("Не включать", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(rows.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const Row& rw = rows[size_t(i)];
                const Json* rule = ruleFor(list, rw.path);
                ImGui::PushID(rw.path.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const float share = total > 0 ? float(rw.stored / total) : 0;
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + ImGui::GetContentRegionAvail().x * share, p0.y + ImGui::GetTextLineHeight()), IM_COL32(90, 140, 220, 60));
                ImGui::TextUnformatted(rw.path.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s -> %s (%s)", rw.path.c_str(), mb(rw.raw).c_str(), mb(rw.stored).c_str(), rw.codec.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(mb(rw.raw).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", mb(rw.stored).c_str());
                ImGui::TableNextColumn();
                {   // compression: default / store / level
                    int lv = rule && rule->has("level") ? int((*rule)["level"].integer()) : -1;
                    std::string cur = lv < 0 ? (rw.codec == "store" ? "как есть" : "по умолчанию") : lv == 0 ? "без сжатия" : "zstd " + std::to_string(lv);
                    ImGui::SetNextItemWidth(110);
                    if (ImGui::BeginCombo("##lv", cur.c_str())) {
                        if (ImGui::Selectable("по умолчанию", lv < 0)) setRule(ed, rw.path, "level", Json());
                        if (ImGui::Selectable("без сжатия", lv == 0)) setRule(ed, rw.path, "level", Json::number(int64_t(0)));
                        for (int l : {3, 9, 15, 19, 22})
                            if (ImGui::Selectable(("zstd " + std::to_string(l)).c_str(), lv == l)) setRule(ed, rw.path, "level", Json::number(int64_t(l)));
                        ImGui::EndCombo();
                    }
                }
                ImGui::TableNextColumn();
                if (isImage(rw.path) && rw.path.rfind("_atlas/", 0) != 0) {
                    std::string cur = "как есть";
                    if (rule && rule->has("maxSize")) cur = "до " + std::to_string((*rule)["maxSize"].integer()) + " px";
                    else if (rule && rule->has("scale")) cur = (*rule)["scale"].toDouble() >= 0.5 ? "50%" : "25%";
                    ImGui::SetNextItemWidth(100);
                    if (ImGui::BeginCombo("##sz", cur.c_str())) {
                        if (ImGui::Selectable("как есть")) { setRule(ed, rw.path, "maxSize", Json()); setRule(ed, rw.path, "scale", Json()); }
                        if (ImGui::Selectable("50%")) { setRule(ed, rw.path, "maxSize", Json()); setRule(ed, rw.path, "scale", Json::number(0.5, 2)); }
                        if (ImGui::Selectable("25%")) { setRule(ed, rw.path, "maxSize", Json()); setRule(ed, rw.path, "scale", Json::number(0.25, 2)); }
                        for (int s : {2048, 1024, 512, 256})
                            if (ImGui::Selectable(("до " + std::to_string(s) + " px").c_str())) { setRule(ed, rw.path, "scale", Json()); setRule(ed, rw.path, "maxSize", Json::number(int64_t(s))); }
                        ImGui::EndCombo();
                    }
                } else ImGui::TextDisabled("—");
                ImGui::TableNextColumn();
                bool ex = rule && rule->getBool("exclude", false);
                if (rw.path.rfind("engine/", 0) == 0 || rw.path == "game.json" || rw.path.rfind("_atlas/", 0) == 0) ImGui::TextDisabled("—");
                else if (ImGui::Checkbox("##ex", &ex)) setRule(ed, rw.path, "exclude", ex ? Json::fromBool(true) : Json());
                ImGui::PopID();
            }
        ImGui::EndTable();
    }

    int target_ = 3, release_ = 1, audio_ = 0;
    bool estimating_ = false, autoEstimate_ = true, perTarget_ = false;
    double dirtyIn_ = 0;
    int askedFor_ = -1;
    std::string outDir_, filter_, error_, lastProject_;
    std::map<std::string, Json> reports_;
};

RTS_EDITOR_WINDOW(BuildWindow);

} // namespace rts::editor
