// Console: build log (rtsbuild: builds, size estimates, toolchain installs). The log is a
// read-only text field, so a fragment can be selected and copied (Cmd/Ctrl+C); lines with errors
// and warnings are tinted. Every run is also written to <engine>/logs (Workspace::logsDir()).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include "../Editor.h"
#include "imgui_internal.h" // the multiline field's child window: scroll + line tints

namespace rts::editor {

namespace {
enum class Kind { Plain, Error, Warning, Ok };
Kind lineKind(const char* b, const char* e) {
    std::string_view l(b, size_t(e - b));
    for (const char* k : {"ОШИБКА", "error:", "Error:", "FAILED", "RTS_MISSING_TOOLCHAIN", "не удал"}) if (l.find(k) != std::string_view::npos) return Kind::Error;
    for (const char* k : {"предупреждение", "warning:", "Warning:", "ВНИМАНИЕ"}) if (l.find(k) != std::string_view::npos) return Kind::Warning;
    for (const char* k : {"=== OK ===", "Готово", "Сборка завершена", "Установка завершена"}) if (l.find(k) != std::string_view::npos) return Kind::Ok;
    return Kind::Plain;
}

void openFolder(const std::string& dir) {
#if defined(_WIN32)
    std::string cmd = "explorer \"" + dir + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open '" + dir + "' &";
#else
    std::string cmd = "xdg-open '" + dir + "' >/dev/null 2>&1 &";
#endif
    if (std::system(cmd.c_str()) != 0) {}   // nothing to do: the folder just does not open
}
} // namespace

class ConsoleWindow : public EditorWindow {
public:
    const char* id() const override { return "console"; }
    const char* title() const override { return "Console"; }
    int order() const override { return 3; }
    const char* dockArea() const override { return "bottom"; }

    void draw(Editor& ed) override {
        auto& ws = ed.ws();
        log_ = ws.buildLog();
        const bool running = ws.building();
        if (running) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", ws.runLabel().rfind("toolchain-", 0) == 0 ? "Идёт установка…" : "Идёт сборка…");
        else if (log_.empty()) ImGui::TextDisabled(ICON_CONSOLE "  Сборка ещё не запускалась (окно «Сборка»)");
        ImGui::SameLine();
        if (!log_.empty() && ImGui::SmallButton(ICON_COPY " Копировать журнал")) ImGui::SetClipboardText(log_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FOLDER_OPEN " Открыть папку логов")) openFolder(ws.logsDir());
        if (std::string f = ws.lastLogFile(); !f.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", f.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Файл журнала этой сборки (последняя — logs/build-last.log)");
        }
        // Read-only multiline field over the log: selectable, Cmd/Ctrl+C, Cmd/Ctrl+A.
        const char* label = "##buildlog";
        ImGuiWindow* parent = ImGui::GetCurrentWindow();
        const ImGuiID id = ImGui::GetID(label);
        ImGui::InputTextMultiline(label, log_.data(), log_.size() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        // the field's scrolling child (name as BeginChildEx makes it) for tints and auto-scroll
        char name[512];
        ImFormatString(name, sizeof name, "%s/%s_%08X", parent->Name, label, id);
        ImGuiWindow* child = ImGui::FindWindowByName(name);
        if (!child) return;
        const float lineH = ImGui::GetFontSize();
        const float top = child->InnerRect.Min.y + ImGui::GetStyle().FramePadding.y - child->Scroll.y;
        const int first = std::max(0, int((child->Scroll.y - ImGui::GetStyle().FramePadding.y) / lineH) - 1);
        const int last = first + int(child->InnerRect.GetHeight() / lineH) + 2;
        int line = 0;
        for (const char *b = log_.c_str(), *end = b + log_.size(); b < end && line <= last; ++line) {
            const char* e = static_cast<const char*>(std::memchr(b, '\n', size_t(end - b)));
            if (!e) e = end;
            if (line >= first) {
                Kind k = lineKind(b, e);
                if (k != Kind::Plain) {
                    ImU32 c = k == Kind::Error ? IM_COL32(255, 80, 70, 60) : k == Kind::Warning ? IM_COL32(255, 200, 60, 50) : IM_COL32(80, 220, 110, 45);
                    float y = top + float(line) * lineH;
                    child->DrawList->AddRectFilled(ImVec2(child->InnerRect.Min.x, y), ImVec2(child->InnerRect.Max.x, y + lineH), c);
                }
            }
            b = e + 1;
        }
        // follow the end while the log grows, unless the user scrolled up or is selecting
        const bool grew = log_.size() != lastSize_;
        lastSize_ = log_.size();
        if (!grew) follow_ = child->Scroll.y >= child->ScrollMax.y - lineH;
        else if (follow_ && ImGui::GetActiveID() != id) ImGui::SetScrollY(child, 1e9f);
    }

private:
    std::string log_;
    size_t lastSize_ = 0;
    bool follow_ = true;
};

RTS_EDITOR_WINDOW(ConsoleWindow);

} // namespace rts::editor
