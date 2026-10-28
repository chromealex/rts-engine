// Console: build log (rtsbuild) and the editor's own messages.
#include "../Editor.h"

namespace rts::editor {

class ConsoleWindow : public EditorWindow {
public:
    const char* id() const override { return "console"; }
    const char* title() const override { return "Console"; }
    int order() const override { return 3; }
    const char* dockArea() const override { return "bottom"; }

    void draw(Editor& ed) override {
        std::string log = ed.ws().buildLog();
        if (ed.ws().building()) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Идёт сборка…");
        else if (log.empty()) ImGui::TextDisabled(ICON_CONSOLE "  Сборка ещё не запускалась (меню «Сборка»)");
        ImGui::SameLine();
        if (!log.empty() && ImGui::SmallButton(ICON_COPY " Копировать журнал")) ImGui::SetClipboardText(log.c_str());
        ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(log.c_str());
        if (ed.ws().building() && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
};

RTS_EDITOR_WINDOW(ConsoleWindow);

} // namespace rts::editor
