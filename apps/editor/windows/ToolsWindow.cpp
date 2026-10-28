// Tools: settings of the current Scene tool (palette of types, terrain kinds, brush, levels...).
#include <algorithm>
#include <cctype>
#include "../Editor.h"
#include "../EditorUtil.h"

namespace rts::editor {

class ToolsWindow : public EditorWindow {
public:
    const char* id() const override { return "tools"; }
    const char* title() const override { return "Tools"; }
    int order() const override { return 0; }
    const char* dockArea() const override { return "left-bottom"; }

    void draw(Editor& ed) override {
        static const char* names[] = {ICON_SELECT "  Выбор", ICON_OBJECTS "  Объекты", ICON_CELLS "  Тип клеток", ICON_LEVELS "  Уровни / плато",
                                      ICON_HEIGHT "  Высота", ICON_FIELDS "  Поля ресурсов", ICON_REGIONS "  Регионы", ICON_ZONES "  Зоны для триггеров",
                                      ICON_STARTS "  Старты игроков", ICON_ROADS "  Дороги", ICON_ERASER "  Удаление"};
        ImGui::SetNextItemWidth(-1);
        int t = int(ed.tool);
        if (ImGui::Combo("##tool", &t, names, int(Tool::Count))) { ed.tool = Tool(t); ed.roadStart = -1; }
        auto& d = ed.doc().data;
        const auto& rules = ed.rules();
        auto brush = [&] { ImGui::SliderInt("Кисть", &ed.brush, 1, 16); ImGui::TextDisabled("[ и ] — размер, Shift — стереть"); };
        switch (ed.tool) {
        case Tool::Place: {
            std::string cur = ed.placeOwner == 0 ? "Нейтральный" : ed.placeOwner < 0 ? "Враждебные монстры" : ed.placeOwner <= int(d.players.size()) ? d.players[size_t(ed.placeOwner - 1)].name : "?";
            if (ImGui::BeginCombo("Владелец", cur.c_str())) {
                if (ImGui::Selectable("Нейтральный", ed.placeOwner == 0)) ed.placeOwner = 0;
                if (ImGui::Selectable("Враждебные монстры", ed.placeOwner == -1)) ed.placeOwner = -1;
                for (size_t p = 0; p < d.players.size(); ++p) if (ImGui::Selectable(d.players[p].name.c_str(), ed.placeOwner == int(p + 1))) ed.placeOwner = int(p + 1);
                ImGui::EndCombo();
            }
            ui::inputString("##flt", filter_);
            if (filter_.empty()) { ImGui::SameLine(16); ImGui::TextDisabled(ICON_SEARCH "  поиск типа"); }
            ImGui::BeginChild("##palette", ImVec2(0, 0), ImGuiChildFlags_Borders);
            static const char* groups[] = {"Юниты", "Здания", "Ресурсы и декор"};
            for (int group = 0; group < 3; ++group) {
                ImGui::SeparatorText(groups[group]);
                for (size_t i = 0; i < rules.types.size(); ++i) {
                    const auto& ty = rules.types[i];
                    int g = ty.resourceNode >= 0 || (ty.invulnerable && ty.garrisonMax == 0) ? 2 : ty.structure ? 1 : 0;
                    if (g != group) continue;
                    if (!filter_.empty() && ty.name.find(filter_) == std::string::npos && ty.id.find(filter_) == std::string::npos) continue;
                    if (ImGui::Selectable((ty.name + "##" + ty.id).c_str(), ed.placeType == int(i))) ed.placeType = int(i);
                }
            }
            ImGui::EndChild();
            break;
        }
        case Tool::Terrain: {
            static const std::pair<char, const char*> cells[] = {{'.', "Земля"}, {'~', "Вода"}, {'#', "Скалы (непроходимо)"}, {',', "Земля без стройки"},
                                                                 {'=', "Брод"}, {'/', "Рампа (подъём между уровнями)"}, {'R', "Камень (Dune: стройка)"}};
            for (auto& [c, n] : cells) if (ImGui::RadioButton(n, ed.paintCell == c)) ed.paintCell = c;
            brush();
            break;
        }
        case Tool::Level:
            ImGui::SliderInt("Уровень", &ed.paintLevel, 0, 9);
            ImGui::TextWrapped("Юниты переходят между уровнями только по рампам. Высота даёт бонусы из rules.heights.");
            brush();
            break;
        case Tool::Height:
            ImGui::SliderInt("Кисть", &ed.brush, 1, 16);
            ImGui::TextWrapped("Плавная высота (только вид). Shift — опускать.");
            break;
        case Tool::Field:
            if (rules.resources.empty()) { ImGui::TextWrapped("В правилах нет ресурсов"); break; }
            ed.fieldRes = std::clamp(ed.fieldRes, 0, int(rules.resources.size()) - 1);
            if (ImGui::BeginCombo("Ресурс", rules.resources[size_t(ed.fieldRes)].name.c_str())) {
                for (size_t r = 0; r < rules.resources.size(); ++r) if (ImGui::Selectable(rules.resources[r].name.c_str(), ed.fieldRes == int(r))) ed.fieldRes = int(r);
                ImGui::EndCombo();
            }
            ImGui::SliderInt("Плотность", &ed.fieldAmount, 1, 9);
            if (!rules.resources[size_t(ed.fieldRes)].field) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "У ресурса нет \"field\": true");
            brush();
            break;
        case Tool::Region: {
            char buf[2] = {ed.regionLetter, 0};
            ImGui::SetNextItemWidth(40);
            if (ImGui::InputText("Буква", buf, 2) && buf[0]) ed.regionLetter = char(std::toupper(static_cast<unsigned char>(buf[0])));
            std::string name = d.regionNames.count(ed.regionLetter) ? d.regionNames[ed.regionLetter] : "";
            if (ui::inputString("Название", name)) { d.regionNames[ed.regionLetter] = name; ed.doc().touched(); }
            brush();
            break;
        }
        case Tool::Area: ImGui::TextWrapped("Протяните прямоугольник в Scene — получится зона для триггеров и волн."); break;
        case Tool::Start:
            for (size_t p = 0; p < d.players.size(); ++p) ImGui::RadioButton((d.players[p].name + "##st" + std::to_string(p)).c_str(), &ed.startPlayer, int(p + 1));
            ImGui::TextWrapped("Клик в Scene — стартовая точка: там появятся стартовые юниты фракции.");
            break;
        case Tool::Road: ImGui::TextWrapped("Клик по объекту A, затем по B — дорога. Shift — продолжить цепочку."); break;
        case Tool::Select: ImGui::TextWrapped("Перетаскивание двигает, Alt — копирует. ПКМ — обзор, ПКМ+WASD — полёт, колесо — масштаб, F — фокус."); break;
        default: ImGui::TextWrapped("Клик по объекту или дороге удаляет их.");
        }
        ImGui::Separator();
        ImGui::Checkbox("Привязка к сетке", &ed.snap);
    }

private:
    std::string filter_;
};

RTS_EDITOR_WINDOW(ToolsWindow);

} // namespace rts::editor
