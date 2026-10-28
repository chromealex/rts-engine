// Editor theme: palette -> ImGui style, fonts, ini persistence. See EditorTheme.h.
#include "EditorTheme.h"
#include "EditorIcons.h"
#include "imgui_internal.h" // ImGuiSettingsHandler, MenuItemEx

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include "engine/platform/Platform.h"

namespace rts::editor {

namespace {
ImVec4 hex(unsigned rgb, float a = 1.0f) {
    return ImVec4(float((rgb >> 16) & 255) / 255.0f, float((rgb >> 8) & 255) / 255.0f, float(rgb & 255) / 255.0f, a);
}
ImVec4 alpha(ImVec4 c, float a) { c.w = a; return c; }
ImVec4 mix(ImVec4 a, ImVec4 b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }

const EditorTheme::Accent kAccents[EditorTheme::kAccentCount] = {
    {"blue", "Синий", hex(0x3D8BFF)},
    {"violet", "Фиолетовый", hex(0x8B6CF6)},
    {"teal", "Бирюзовый", hex(0x14B8A6)},
    {"orange", "Оранжевый", hex(0xF97316)},
};

// Font file of the editor: from the engine checkout, else from the app's content (macOS bundle).
bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return !out.empty();
}
bool readFont(const std::string& engineDir, const char* rel, std::string& out) {
    if (readFile(engineDir + "/third_party/fonts/" + rel, out)) return true;
    return platform::readContentFile(std::string("engine/editor-fonts/") + rel, out) && !out.empty();
}

// Inter keeps alternates in the private use area, where the icon font lives: exclude it from text fonts.
const ImWchar kNoPrivateUse[] = {0xE000, 0xF8FF, 0};

ImFont* addFont(const std::string& data, float size, const char* name, ImFont* mergeInto, ImVec2 offset = {}, float minAdvance = 0,
                const ImWchar* exclude = kNoPrivateUse) {
    void* mem = IM_ALLOC(data.size());
    std::memcpy(mem, data.data(), data.size());
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;
    cfg.MergeMode = mergeInto != nullptr;
    cfg.DstFont = mergeInto;
    cfg.GlyphOffset = offset;
    cfg.GlyphMinAdvanceX = minAdvance;
    cfg.OversampleH = 2;
    cfg.GlyphExcludeRanges = exclude;
    std::snprintf(cfg.Name, sizeof cfg.Name, "%s", name);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(mem, int(data.size()), size, &cfg);
}
} // namespace

const EditorTheme::Accent& EditorTheme::accent(int i) { return kAccents[(i % kAccentCount + kAccentCount) % kAccentCount]; }

bool EditorTheme::loadFonts(const std::string& engineDir) {
    std::string regular, semibold, noto, symbols, icons;
    if (!readFont(engineDir, "inter/Inter-Regular.ttf", regular)) return false;
    readFont(engineDir, "inter/Inter-SemiBold.ttf", semibold);
    readFont(engineDir, "noto-symbols2/NotoSansSymbols2-editor.ttf", symbols);
    readFont(engineDir, "lucide/lucide.ttf", icons);
    // Noto Sans (the engine UI font) covers the rare Latin/Cyrillic letters Inter lacks
    if (!readFile(engineDir + "/engine/assets/fonts/ui.ttf", noto)) platform::readContentFile("engine/fonts/ui.ttf", noto);
    auto build = [&](const std::string& main, float size, const char* name) {
        ImFont* f = addFont(main, size, name, nullptr);
        if (!noto.empty()) addFont(noto, size, "Noto Sans (fallback)", f);
        if (!symbols.empty()) addFont(symbols, size, "Noto Sans Symbols 2", f, ImVec2(0, 1));
        // icons: same advance for all so toolbars line up; Lucide sits slightly high on Inter's baseline
        if (!icons.empty()) addFont(icons, size, "Lucide", f, ImVec2(0, size * 0.12f), size * 1.15f, nullptr);
        return f;
    };
    ImGuiIO& io = ImGui::GetIO();
    ImFont* old = io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0] : nullptr;
    ui_ = build(regular, kUiSize, "Inter");
    heading_ = semibold.empty() ? ui_ : build(semibold, kHeadingSize, "Inter SemiBold");
    io.FontDefault = ui_;
    if (old) io.Fonts->RemoveFont(old); // ImGuiLayer's default font: unused by the editor
    return true;
}

// ------------------------------------------------------------ persistence

void EditorTheme::registerSettings() {
    ImGuiSettingsHandler h;
    h.TypeName = "RtsEditor";
    h.TypeHash = ImHashStr("RtsEditor");
    h.UserData = this;
    h.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* hh, const char* name) -> void* {
        return std::strcmp(name, "Theme") == 0 ? hh->UserData : nullptr;
    };
    h.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
        auto* t = static_cast<EditorTheme*>(entry);
        char v[32] = {};
        int n = 0;
        if (std::sscanf(line, "Mode=%31s", v) == 1) t->mode_ = std::strcmp(v, "light") == 0 ? Mode::Light : Mode::Dark;
        else if (std::sscanf(line, "Accent=%31s", v) == 1) {
            for (int i = 0; i < kAccentCount; ++i) if (std::strcmp(v, kAccents[i].id) == 0) t->accent_ = i;
        } else if (std::sscanf(line, "Layout=%d", &n) == 1) t->loadedLayout_ = n;
    };
    h.ApplyAllFn = [](ImGuiContext*, ImGuiSettingsHandler* hh) { static_cast<EditorTheme*>(hh->UserData)->pending_ = true; };
    h.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* hh, ImGuiTextBuffer* buf) {
        auto* t = static_cast<EditorTheme*>(hh->UserData);
        buf->appendf("[%s][Theme]\nMode=%s\nAccent=%s\nLayout=%d\n\n", hh->TypeName, t->mode_ == Mode::Light ? "light" : "dark",
                     kAccents[t->accent_].id, kLayoutVersion);
    };
    ImGui::AddSettingsHandler(&h);
}

void EditorTheme::set(Mode m, int a) {
    mode_ = m;
    accent_ = (a % kAccentCount + kAccentCount) % kAccentCount;
    pending_ = true;
    ImGui::MarkIniSettingsDirty();
}

void EditorTheme::applyPending() {
    if (pending_) { pending_ = false; apply(); }
}

void EditorTheme::drawMenu() {
    if (ImGui::MenuItemEx("Тёмная", ICON_MOON, nullptr, mode_ == Mode::Dark)) set(Mode::Dark, accent_);
    if (ImGui::MenuItemEx("Светлая", ICON_SUN, nullptr, mode_ == Mode::Light)) set(Mode::Light, accent_);
    ImGui::SeparatorText("Акцент");
    for (int i = 0; i < kAccentCount; ++i) {
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        if (ImGui::MenuItemEx(kAccents[i].name, "", nullptr, accent_ == i)) set(mode_, i);
        // the swatch goes where the icon column is
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.55f, p.y + h * 0.5f + 1), h * 0.32f, ImGui::GetColorU32(kAccents[i].color));
        ImGui::PopID();
    }
}

// ------------------------------------------------------------ style

void EditorTheme::apply() {
    const bool dark = mode_ == Mode::Dark;
    const ImVec4 acc = kAccents[accent_].color;
    Palette& p = pal_;
    if (dark) {
        p.appBg = hex(0x0F1012); p.canvas = hex(0x222327); p.panel = hex(0x18191C); p.panelRaised = hex(0x202125); p.tabBar = hex(0x131416);
        p.frame = hex(0x232428); p.frameHover = hex(0x2B2C31); p.border = hex(0x2A2B30); p.separator = hex(0x26272C);
        p.text = hex(0xE2E4E9); p.textDim = hex(0x8A8F99); p.hover = ImVec4(1, 1, 1, 0.06f);
        p.success = hex(0x34D399); p.warning = hex(0xFBBF24); p.danger = hex(0xF87171);
        p.accentHover = mix(acc, ImVec4(1, 1, 1, 1), 0.15f);
    } else {
        p.appBg = hex(0xE4E6EA); p.canvas = hex(0xDCDFE4); p.panel = hex(0xF8F9FA); p.panelRaised = hex(0xFFFFFF); p.tabBar = hex(0xECEEF1);
        p.frame = hex(0xFFFFFF); p.frameHover = hex(0xF1F2F5); p.border = hex(0xD5D8DE); p.separator = hex(0xE1E3E7);
        p.text = hex(0x1C1F24); p.textDim = hex(0x6B7280); p.hover = ImVec4(0, 0, 0, 0.05f);
        p.success = hex(0x059669); p.warning = hex(0xD97706); p.danger = hex(0xDC2626);
        p.accentHover = mix(acc, ImVec4(0, 0, 0, 1), 0.10f);
    }
    p.accent = acc;
    p.onAccent = ImVec4(1, 1, 1, 1);

    ImGuiStyle s; // from defaults, so switching themes never accumulates
    s.FontSizeBase = kUiSize;
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(6, 4);
    s.IndentSpacing = 16;
    s.ScrollbarSize = 10; s.ScrollbarRounding = 6; s.ScrollbarPadding = 2;
    s.GrabMinSize = 10; s.GrabRounding = 4;
    s.WindowRounding = 6; s.ChildRounding = 6; s.FrameRounding = 5; s.PopupRounding = 6;
    s.TabRounding = 5; s.MenuItemRounding = 4; s.ImageRounding = 0;
    s.WindowBorderSize = 1; s.ChildBorderSize = 1; s.PopupBorderSize = 1;
    s.FrameBorderSize = dark ? 0 : 1;
    s.TabBorderSize = 0; s.TabBarBorderSize = 1; s.TabBarOverlineSize = 2;
    s.TabCloseButtonMinWidthSelected = 0; s.TabCloseButtonMinWidthUnselected = 0; // close buttons on hover only
    s.TabMinWidthBase = 1;
    s.WindowTitleAlign = ImVec2(0, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.DockingNodeHasCloseButton = false;
    s.DockingSeparatorSize = 3;
    s.SeparatorTextBorderSize = 1; s.SeparatorTextPadding = ImVec2(0, 4); s.SeparatorTextAlign = ImVec2(0, 0.5f);
    s.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesToNodes; s.TreeLinesSize = 1; s.TreeLinesRounding = 4;
    s.DisabledAlpha = 0.45f;
    s.InputTextCursorSize = 1.5f;

    ImVec4* c = s.Colors;
    const ImVec4 none(0, 0, 0, 0);
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textDim;
    c[ImGuiCol_WindowBg] = p.panel;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = p.panelRaised;
    c[ImGuiCol_Border] = dark ? p.border : hex(0xD0D3DA);
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = p.frame;
    c[ImGuiCol_FrameBgHovered] = p.frameHover;
    c[ImGuiCol_FrameBgActive] = dark ? hex(0x303137) : hex(0xE9EBEF);
    c[ImGuiCol_TitleBg] = p.tabBar;
    c[ImGuiCol_TitleBgActive] = p.tabBar;
    c[ImGuiCol_TitleBgCollapsed] = p.tabBar;
    c[ImGuiCol_MenuBarBg] = p.appBg;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = dark ? ImVec4(1, 1, 1, 0.12f) : ImVec4(0, 0, 0, 0.16f);
    c[ImGuiCol_ScrollbarGrabHovered] = dark ? ImVec4(1, 1, 1, 0.20f) : ImVec4(0, 0, 0, 0.26f);
    c[ImGuiCol_ScrollbarGrabActive] = alpha(acc, 0.8f);
    c[ImGuiCol_CheckMark] = acc;
    c[ImGuiCol_CheckboxSelectedBg] = p.frame;
    c[ImGuiCol_SliderGrab] = acc;
    c[ImGuiCol_SliderGrabActive] = p.accentHover;
    c[ImGuiCol_Button] = dark ? hex(0x26272C) : hex(0xFFFFFF);
    c[ImGuiCol_ButtonHovered] = dark ? hex(0x303137) : hex(0xF0F1F4);
    c[ImGuiCol_ButtonActive] = dark ? hex(0x393A41) : hex(0xE3E5EA);
    c[ImGuiCol_Header] = alpha(acc, dark ? 0.24f : 0.16f);
    c[ImGuiCol_HeaderHovered] = dark ? ImVec4(1, 1, 1, 0.07f) : ImVec4(0, 0, 0, 0.05f);
    c[ImGuiCol_HeaderActive] = alpha(acc, dark ? 0.34f : 0.24f);
    c[ImGuiCol_Separator] = p.separator;
    c[ImGuiCol_SeparatorHovered] = alpha(acc, 0.6f);
    c[ImGuiCol_SeparatorActive] = acc;
    c[ImGuiCol_ResizeGrip] = none;
    c[ImGuiCol_ResizeGripHovered] = alpha(acc, 0.4f);
    c[ImGuiCol_ResizeGripActive] = alpha(acc, 0.8f);
    c[ImGuiCol_InputTextCursor] = acc;
    c[ImGuiCol_Tab] = p.tabBar;
    c[ImGuiCol_TabHovered] = dark ? hex(0x1F2024) : hex(0xF4F5F7);
    c[ImGuiCol_TabSelected] = p.panel;
    c[ImGuiCol_TabSelectedOverline] = acc;
    c[ImGuiCol_TabDimmed] = p.tabBar;
    c[ImGuiCol_TabDimmedSelected] = p.panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = none;
    c[ImGuiCol_DockingPreview] = alpha(acc, 0.35f);
    c[ImGuiCol_DockingEmptyBg] = p.appBg;
    c[ImGuiCol_PlotLines] = acc;
    c[ImGuiCol_PlotLinesHovered] = p.accentHover;
    c[ImGuiCol_PlotHistogram] = acc;
    c[ImGuiCol_PlotHistogramHovered] = p.accentHover;
    c[ImGuiCol_TableHeaderBg] = dark ? hex(0x1E1F23) : hex(0xEEF0F3);
    c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableBorderLight] = p.separator;
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = dark ? ImVec4(1, 1, 1, 0.025f) : ImVec4(0, 0, 0, 0.025f);
    c[ImGuiCol_TextLink] = acc;
    c[ImGuiCol_TextSelectedBg] = alpha(acc, 0.35f);
    c[ImGuiCol_TreeLines] = dark ? hex(0x34353B) : hex(0xD5D8DE);
    c[ImGuiCol_DragDropTarget] = acc;
    c[ImGuiCol_DragDropTargetBg] = alpha(acc, 0.12f);
    c[ImGuiCol_UnsavedMarker] = acc;
    c[ImGuiCol_NavCursor] = acc;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, dark ? 0.5f : 0.3f);
    ImGui::GetStyle() = s;
}

} // namespace rts::editor
