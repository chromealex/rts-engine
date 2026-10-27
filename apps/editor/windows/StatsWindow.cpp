// Statistics: editor performance (frame time, sim time, draw calls), map contents, and during a
// play-test per-player graphs of army, structures and resources over time.
#include <algorithm>
#include "../Editor.h"

namespace rts::editor {

class StatsWindow : public EditorWindow {
public:
    const char* id() const override { return "stats"; }
    const char* title() const override { return "Statistics"; }
    int order() const override { return 2; }
    const char* dockArea() const override { return "bottom"; }

    void draw(Editor& ed) override {
        std::vector<float> frame, sim;
        uint32_t draws = 0;
        for (auto& f : ed.frameHistory) { frame.push_back(f.frameMs); sim.push_back(f.simMs); draws = f.drawCalls; }
        float avg = 0;
        for (float v : frame) avg += v;
        avg = frame.empty() ? 0 : avg / float(frame.size());
        ImGui::Text("Кадр редактора: %.2f мс (CPU), отрисовок: %u, FPS ~%.0f", avg, draws, ImGui::GetIO().Framerate);
        if (!frame.empty()) ImGui::PlotLines("##frame", frame.data(), int(frame.size()), 0, "кадр, мс", 0, 33, ImVec2(-1, 40));
        if (ed.playing() && !sim.empty()) ImGui::PlotLines("##sim", sim.data(), int(sim.size()), 0, "симуляция, мс", 0, 16, ImVec2(-1, 40));
        const auto& d = ed.doc().data;
        ImGui::Text("Карта: %d×%d, объектов %zu, дорог %zu, игроков %zu", d.w, d.h, d.entities.size(), d.roads.size(), d.players.size());
        if (Simulation* s = ed.sim()) {
            ImGui::SeparatorText("Тест");
            ImGui::Text("Тик %u (%.1f с), сущностей: %zu, снарядов: %zu", s->tick(), double(s->tick()) / s->tickRate(), s->world().count<game::Unit>(), s->world().count<game::Projectile>());
            const auto& hist = ed.playHistory;
            if (hist.size() >= 2) {
                for (size_t p = 0; p < hist.back().units.size(); ++p) {
                    render::float4 c = game::RtsView::playerColor(int(p + 1));
                    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(c.x, c.y, c.z, 1));
                    std::vector<float> units, structs;
                    for (auto& h : hist) { units.push_back(float(p < h.units.size() ? h.units[p] : 0)); structs.push_back(float(p < h.structures.size() ? h.structures[p] : 0)); }
                    std::string name = p < s->players().size() ? s->players()[p].name : "?";
                    ImGui::PlotLines(("##u" + std::to_string(p)).c_str(), units.data(), int(units.size()), 0, (name + ": армия").c_str(), 0, FLT_MAX, ImVec2(-1, 36));
                    ImGui::PlotLines(("##s" + std::to_string(p)).c_str(), structs.data(), int(structs.size()), 0, (name + ": здания").c_str(), 0, FLT_MAX, ImVec2(-1, 30));
                    for (size_t r = 0; r < ed.rules().resources.size(); ++r) {
                        std::vector<float> res;
                        for (auto& h : hist) res.push_back(p < h.resources.size() && r < h.resources[p].size() ? h.resources[p][r] : 0);
                        ImGui::PlotLines(("##r" + std::to_string(p) + "_" + std::to_string(r)).c_str(), res.data(), int(res.size()), 0,
                                         (name + ": " + ed.rules().resources[r].name).c_str(), 0, FLT_MAX, ImVec2(-1, 26));
                    }
                    ImGui::PopStyleColor();
                }
            }
        }
    }
};

RTS_EDITOR_WINDOW(StatsWindow);

} // namespace rts::editor
