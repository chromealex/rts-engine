// Agent: the MCP link (how to connect an AI agent) and the feed of everything agents did.
#include <filesystem>
#include "../Editor.h"
#include "../core/McpInstall.h"

namespace fs = std::filesystem;

namespace rts::editor {

class AgentWindow : public EditorWindow {
public:
    const char* id() const override { return "agent"; }
    const char* title() const override { return "Agent"; }
    int order() const override { return 1; }
    const char* dockArea() const override { return "bottom"; }

    void draw(Editor& ed) override {
        auto& ws = ed.ws();
        connectSection(ed);
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

private:
    McpServerSpec spec(Editor& ed) const {
        McpServerSpec s;
        s.command = ed.options().mcpTool;
        if (s.command.empty()) s.command = (fs::path(ed.options().buildTool).parent_path() / "rtsmcp").string();
        s.args = {"--engine", ed.options().engineDir};
        if (!ed.options().editorExe.empty()) { s.args.push_back("--editor"); s.args.push_back(ed.options().editorExe); }
        return s;
    }

    // One click per AI client: writes the rtsmcp entry into its MCP config (see core/McpInstall.h).
    void connectSection(Editor& ed) {
        const McpServerSpec sp = spec(ed);
        if (clients_.empty() || ImGui::GetTime() > nextScan_) { clients_ = mcpClients(sp, ed.options().engineDir); nextScan_ = ImGui::GetTime() + 5; }
        if (ImGui::CollapsingHeader(ICON_PLUG "  Подключить ИИ-агента", connectedAny() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("Кнопка прописывает сервер rtsmcp в настройки клиента. После этого перезапустите клиент.");
            if (ImGui::BeginTable("##mcp", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                for (auto& c : clients_) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(c.name.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.configPath.c_str());
                    ImGui::TableNextColumn();
                    if (c.upToDate) ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1), ICON_CHECK " подключено");
                    else if (c.installed) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "старый путь — обновите");
                    else if (c.present) ImGui::TextDisabled("найден");
                    else ImGui::TextDisabled("не установлен");
                    ImGui::TableNextColumn();
                    ImGui::PushID(c.id.c_str());
                    if (ImGui::SmallButton(c.upToDate ? "Переподключить" : c.installed ? "Обновить" : "Подключить")) run(ed, c, sp, false);
                    if (c.installed) { ImGui::SameLine(); if (ImGui::SmallButton(ICON_TRASH)) run(ed, c, sp, true); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Отключить"); }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (!message_.empty()) ImGui::TextColored(messageError_ ? ImVec4(1, 0.5f, 0.4f, 1) : ImVec4(0.4f, 0.85f, 0.5f, 1), "%s", message_.c_str());
            std::string cmd = "claude mcp add rtsengine -- \"" + sp.command + "\"";
            for (auto& a : sp.args) cmd += " \"" + a + "\"";
            ImGui::TextDisabled("Вручную:");
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_COPY " Копировать команду")) ImGui::SetClipboardText(cmd.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cmd.c_str());
        }
    }

    void run(Editor& ed, const McpClientInfo& c, const McpServerSpec& sp, bool remove) {
        std::string err;
        const bool ok = remove ? mcpUninstall(c.id, sp, ed.options().engineDir, &err) : mcpInstall(c.id, sp, ed.options().engineDir, &err);
        messageError_ = !ok;
        message_ = ok ? (remove ? c.name + ": отключено" : c.name + ": подключено. Перезапустите " + c.name + " — инструменты rtsengine появятся в списке.") : err;
        ed.ws().log("editor", ok ? message_ : "MCP " + c.name + ": " + err, !ok);
        clients_.clear();
    }

    bool connectedAny() const { for (auto& c : clients_) if (c.upToDate) return true; return false; }

    std::vector<McpClientInfo> clients_;
    double nextScan_ = 0;
    std::string message_;
    bool messageError_ = false;
};

RTS_EDITOR_WINDOW(AgentWindow);

} // namespace rts::editor
