// Small ImGui helpers shared by editor windows.
#pragma once
#include <string>
#include "imgui.h"

namespace rts::editor::ui {

inline int stringResize(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(size_t(d->BufTextLen));
        d->Buf = s->data();
    }
    return 0;
}
inline bool inputString(const char* label, std::string& s, ImGuiInputTextFlags f = 0) {
    return ImGui::InputText(label, s.data(), s.capacity() + 1, f | ImGuiInputTextFlags_CallbackResize, stringResize, &s);
}
inline bool inputMultiline(const char* label, std::string& s, ImVec2 size) {
    return ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, stringResize, &s);
}
inline void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30); ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
}

} // namespace rts::editor::ui
