// Agent: the MCP link (how to connect an AI agent) and the feed of everything agents did.
#include <filesystem>
#include "../Editor.h"

namespace fs = std::filesystem;

namespace rts::editor {

class AgentWindow : public EditorWindow {
public:
    const char* id() const override { return "agent"; }
    const char* title() const override { return "Agent"; }
    const char* dockArea() const override { return "bottom"; }

    void draw(Editor& ed) override {
        auto& ws = ed.ws();
        std::string mcp = (fs::path(ed.options().buildTool).parent_path() / "rtsmcp").string();
        std::string cmd = "claude mcp add rtsengine -- \"" + mcp + "\" --engine \"" + ed.options().engineDir + "\"";
        ImGui::TextWrapped("Подключить агента (Claude Code):");
        ImGui::TextDisabled("%s", cmd.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Копировать")) ImGui::SetClipboardText(cmd.c_str());
        ImGui::Checkbox("Камера Scene следует за правками агента", &ed.followAgent);
        ImGui::SeparatorText("Действия");
        ImGui::BeginChild("##feed", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (auto it = ws.activity.rbegin(); it != ws.activity.rend(); ++it) {
            int sec = int(it->time);
            ImGui::TextDisabled("%02d:%02d %-6s", sec / 60, sec % 60, it->source.c_str());
            ImGui::SameLine();
            if (it->error) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", it->text.c_str());
            else ImGui::TextWrapped("%s", it->text.c_str());
        }
        ImGui::EndChild();
    }
};

RTS_EDITOR_WINDOW(AgentWindow);

} // namespace rts::editor
