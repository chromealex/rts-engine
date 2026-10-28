// Scripts: the open game's Lua files (docs/scripting.md), the script log of the running play-test and
// hot reload: when a .lua file under scripts/ changes on disk, the play-test restarts with the new
// scripts (the simulation VM is created from the sources at setup, so a restart is the reload).
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include "../Editor.h"
#include "../EditorIcons.h"
#include "../EditorUtil.h"
#include "modules/rts/RtsScript.h"
#include "modules/rts/view/RtsViewScript.h"

namespace fs = std::filesystem;

namespace rts::editor {

class ScriptsWindow : public EditorWindow {
public:
    const char* id() const override { return "scripts"; }
    const char* title() const override { return "Скрипты"; }
    const char* dockArea() const override { return "bottom"; }
    int order() const override { return 6; }
    bool openByDefault() const override { return false; }
    const char* icon() const override { return ICON_CONSOLE; }

    void draw(Editor& ed) override {
        const auto& rules = ed.ws().project.rulesJson;
        const Json map = ed.doc().toJson();
        auto sims = game::scriptFiles(rules, map, false), views = game::scriptFiles(rules, map, true);
        if (sims.empty() && views.empty())
            ui::help("У игры нет скриптов. Добавьте файл scripts/x.lua и строку \"scripts\": [\"scripts/x.lua\"] в rules.json "
                     "(или MCP script_write {register: true}). Справка: docs/scripting.md, MCP script_api.");
        for (auto& f : sims) ImGui::BulletText("%s  (симуляция)", f.c_str());
        for (auto& f : views) ImGui::BulletText("%s  (вид)", f.c_str());
        ImGui::Checkbox("Перезапускать тест при изменении файлов", &autoRestart_);
        ImGui::Separator();
        auto* m = ed.simModule();
        auto* se = m ? m->scripts() : nullptr;
        if (!se) { ImGui::TextDisabled(ed.playing() ? "В тесте нет скриптов симуляции" : "Журнал появится во время теста (F5)"); return; }
        ImGui::Text("Ошибок: %d", se->errorCount());
        ImGui::BeginChild("##scriptlog", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        for (auto& e : se->log()) {
            const std::string line = "[" + std::to_string(e.tick) + "] " + e.text;
            if (e.error) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", line.c_str());
            else ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    void update(Editor& ed, double dt) override {
        // script messages of a new play-test go to the activity feed / console
        auto* m = ed.simModule();
        auto* se = m ? m->scripts() : nullptr;
        const bool restarted = ed.sim() && ed.sim()->tick() < lastTick_;   // same address, new match
        lastTick_ = ed.sim() ? ed.sim()->tick() : 0;
        if (ed.sim() != viewSim_ || restarted) {   // a new play-test: view scripts for it
            viewSim_ = ed.sim();
            attached_ = nullptr;
            view_.detach();
            if (viewSim_) {
                const Json map = ed.doc().toJson();
                if (view_.load(*viewSim_, ed.ws().project.simRules(map), map, uint8_t(std::max(0, ed.humanSlot())))) view_.attach(*viewSim_);
                for (auto& e : view_.log()) if (e.error) ed.ws().log("view-script", e.text, true);
            }
        }
        if (viewSim_ && view_.active()) {
            const size_t before = view_.log().size();
            view_.update(*viewSim_, float(dt));
            for (size_t i = before; i < view_.log().size(); ++i) ed.ws().log("view-script", view_.log()[i].text, view_.log()[i].error);
            for (auto& r : view_.takeRequests())
                if (r.kind == game::ViewRequest::Notice) ed.status = r.text;
        }
        if (se != attached_) {
            attached_ = se;
            if (se) {
                Workspace* ws = &ed.ws();
                for (auto& e : se->log()) if (e.error) ws->log("script", e.text, true);
                se->onLog = [ws](const game::ScriptLogEntry& e) { if (e.error) ws->log("script", e.text, true); };
            }
        }
        // hot reload: poll scripts/**/*.lua once per second
        since_ += dt;
        if (since_ < 1.0) return;
        since_ = 0;
        const std::string& dir = ed.ws().project.dir;
        if (dir != dir_) { dir_ = dir; stamps_.clear(); }
        if (dir.empty()) return;
        std::error_code ec;
        const fs::path root = fs::path(dir) / "scripts";
        if (!fs::exists(root, ec)) return;
        std::vector<std::string> changed;
        for (auto& f : fs::recursive_directory_iterator(root, ec)) {
            if (!f.is_regular_file(ec) || f.path().extension() != ".lua") continue;
            const auto t = fs::last_write_time(f.path(), ec);
            if (ec) continue;
            const std::string key = f.path().generic_string();
            auto it = stamps_.find(key);
            if (it == stamps_.end()) stamps_[key] = t;
            else if (it->second != t) { it->second = t; changed.push_back(fs::relative(f.path(), dir, ec).generic_string()); }
        }
        if (changed.empty()) return;
        for (auto& c : changed) {
            std::string text, err;
            std::ifstream in(fs::path(dir) / c, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), {});
            err = game::checkScriptSyntax(c, text, c.rfind("scripts/view/", 0) != 0);
            ed.ws().log("script", err.empty() ? "изменён " + c : err, !err.empty());
        }
        if (autoRestart_ && ed.playing()) {
            const int slot = ed.humanSlot();
            ed.stopPlay();
            ed.startPlay(slot);
            ed.status = "Скрипты перезагружены: тест перезапущен";
        }
    }

private:
    bool autoRestart_ = true;
    double since_ = 0;
    std::string dir_;
    game::ScriptEngine* attached_ = nullptr;
    Simulation* viewSim_ = nullptr;
    uint32_t lastTick_ = 0;
    game::ViewScripts view_;
    std::map<std::string, fs::file_time_type> stamps_;
};

RTS_EDITOR_WINDOW(ScriptsWindow);

} // namespace rts::editor
