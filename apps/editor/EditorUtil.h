// Small ImGui helpers shared by editor windows: string inputs, help marks, and the editor's toolbar
// widgets (icon tool buttons, segmented groups, toolbar strips, empty states). Colours come from the
// current ImGui style, which EditorTheme writes, so they follow the dark / light theme and the accent.
#pragma once
#include <algorithm>
#include <cstring>
#include <string>
#include "imgui.h"
#include "EditorIcons.h"

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

inline ImVec4 accentColor() { return ImGui::GetStyle().Colors[ImGuiCol_CheckMark]; } // the theme's accent
inline ImU32 rgbaU32(ImVec4 c, float a) { c.w *= a; return ImGui::GetColorU32(c); }

// The semibold heading font loaded by EditorTheme (falls back to the current font).
inline ImFont* headingFont() {
    for (ImFont* f : ImGui::GetIO().Fonts->Fonts)
        if (std::strncmp(f->GetDebugName(), "Inter SemiBold", 14) == 0) return f;
    return ImGui::GetFont();
}
// A section title in the heading font (bigger and semibold than SeparatorText).
inline void heading(const char* text) {
    ImGui::PushFont(headingFont(), 0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

// Square icon button for toolbars. active = the current tool / mode (filled with the accent);
// the tooltip (Russian label) shows on hover, also when disabled.
inline bool toolButton(const char* icon, const char* tooltip, bool active = false, bool enabled = true) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(icon);
    ImGui::PushID(tooltip ? tooltip : "");
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##tool", ImVec2(h, h));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    if (tooltip && *tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + h, p.y + h);
    const float r = ImGui::GetStyle().FrameRounding;
    const ImVec4 a = accentColor();
    if (active) dl->AddRectFilled(p, q, rgbaU32(a, held ? 0.8f : hovered ? 0.9f : 1.0f), r);
    else if (hovered && enabled) dl->AddRectFilled(p, q, ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), r);
    const ImVec2 ts = ImGui::CalcTextSize(icon);
    const ImU32 tc = active ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled, hovered ? 1.0f : 0.8f);
    dl->AddText(ImVec2(p.x + (h - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), tc, icon);
    return pressed && enabled;
}

// Segmented group of tool buttons on one raised background: beginSegment(); toolButton(); SameLine();
// toolButton(); endSegment(). Not nestable (uses draw list channels).
inline void beginSegment() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
}
inline void endSegment() {
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSetCurrent(0);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    a.x -= 2; a.y -= 2; b.x += 2; b.y += 2;
    const float r = ImGui::GetStyle().FrameRounding + 2;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), r);
    if (ImGui::GetStyle().FrameBorderSize > 0) dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), r);
    dl->ChannelsMerge();
}
// Thin vertical divider between toolbar groups.
inline void toolbarDivider() {
    ImGui::SameLine(0, 8);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 4), ImVec2(p.x, p.y + h - 4), ImGui::GetColorU32(ImGuiCol_Separator), 1);
    ImGui::Dummy(ImVec2(1, h));
    ImGui::SameLine(0, 8);
}

// Toolbar strip across the top of a window drawn with zeroPadding(): background + bottom border.
//   float y = ui::beginToolbar(); ...buttons on one line... ui::endToolbar(y);
inline float beginToolbar(float pad = 5) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight() + pad * 2 + 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_WindowBg));
    dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f), ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::SetCursorScreenPos(ImVec2(p.x + pad + 3, p.y + pad + 2)); // + room for the segment backgrounds
    return p.y + h;
}
inline void endToolbar(float bottom) {
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, bottom));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, bottom));
}

// Button with an icon and a label; primary = filled with the accent (the main action of a window).
inline bool button(const char* icon, const char* label, bool primary = false, ImVec2 size = ImVec2(0, 0)) {
    std::string text = std::string(icon) + "  " + label;
    if (primary) {
        const ImVec4 a = accentColor();
        ImGui::PushStyleColor(ImGuiCol_Button, a);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(a.x * 1.1f + 0.04f, a.y * 1.1f + 0.04f, a.z * 1.1f + 0.04f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(a.x * 0.85f, a.y * 0.85f, a.z * 0.85f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    }
    bool r = ImGui::Button(text.c_str(), size);
    if (primary) ImGui::PopStyleColor(5);
    return r;
}

// Empty-state message centred in [a, b]: a large dim icon, a title and a hint line.
inline void emptyState(ImVec2 a, ImVec2 b, const char* icon, const char* title, const char* hint = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetStyle().FontSizeBase;
    ImFont* hf = headingFont();
    const float iconSize = fs * 2.6f, titleSize = fs * 1.1f;
    const ImVec2 is = hf->CalcTextSizeA(iconSize, 1e9f, 0, icon);
    const ImVec2 ts = hf->CalcTextSizeA(titleSize, 1e9f, 0, title);
    const float wrap = std::max(120.0f, std::min(b.x - a.x - 40, fs * 30));
    const ImVec2 hs = hint ? ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), 1e9f, wrap, hint) : ImVec2(0, 0);
    const float total = is.y + 10 + ts.y + (hint ? 6 + hs.y : 0);
    float y = a.y + (b.y - a.y - total) * 0.5f;
    const float cx = (a.x + b.x) * 0.5f;
    dl->AddText(hf, iconSize, ImVec2(cx - is.x * 0.5f, y), rgbaU32(accentColor(), 0.75f), icon);
    y += is.y + 10;
    dl->AddText(hf, titleSize, ImVec2(cx - ts.x * 0.5f, y), ImGui::GetColorU32(ImGuiCol_Text), title);
    y += ts.y + 6;
    if (hint) dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cx - hs.x * 0.5f, y), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint, nullptr, wrap);
}
// The same in the rest of the current window (reserves the space).
inline void emptyStateHere(const char* icon, const char* title, const char* hint = nullptr, float height = 0) {
    const ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    const float h = height > 0 ? height : std::max(avail.y, 160.0f);
    emptyState(p, ImVec2(p.x + avail.x, p.y + h), icon, title, hint);
    ImGui::Dummy(ImVec2(avail.x, h));
}

} // namespace rts::editor::ui
