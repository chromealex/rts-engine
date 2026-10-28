// Editor look: the ImGui style (dark / light palette with one accent colour), the editor fonts
// (Inter + symbol fallback + Lucide icons, third_party/fonts) and the theme choice persisted in the
// editor's layout file (<userDataDir>/editor_layout.ini, section [RtsEditor][Theme]).
// Only the editor uses it: the game's ImGui debug overlay keeps engine/render/ImGuiLayer's style.
#pragma once
#include <string>
#include "imgui.h"

namespace rts::editor {

class EditorTheme {
public:
    enum class Mode { Dark, Light };
    struct Accent { const char* id; const char* name; ImVec4 color; };
    static constexpr int kAccentCount = 4;
    static const Accent& accent(int i);

    // Semantic colours for the editor's own drawing (top bar, status bar, pills, empty states).
    struct Palette {
        ImVec4 appBg, canvas, panel, panelRaised, tabBar, frame, frameHover, border, separator;
        ImVec4 text, textDim, accent, accentHover, onAccent, success, warning, danger, hover;
    };

    // Fonts: UI (uiSize px) and heading (semibold, headingSize px), each with symbol and icon fallbacks.
    // Returns false (and keeps ImGui's current font) when the files are missing.
    bool loadFonts(const std::string& engineDir);
    ImFont* uiFont() const { return ui_; }
    ImFont* headingFont() const { return heading_ ? heading_ : ui_; }
    static constexpr float kUiSize = 15.0f, kHeadingSize = 16.5f;

    // Registers the ini handler; call before the first frame (ImGui reads the ini on its first NewFrame).
    void registerSettings();
    // Layout version stored in the ini: lower than kLayoutVersion means the saved docking refers to
    // old window names (before Title###id) and must be rebuilt.
    static constexpr int kLayoutVersion = 2;
    int loadedLayoutVersion() const { return loadedLayout_; }

    Mode mode() const { return mode_; }
    int accentIndex() const { return accent_; }
    void set(Mode m, int accent);          // applied at the start of the next frame (applyPending)
    void applyPending();                   // call outside of any ImGui frame
    void apply();                          // writes the whole ImGui style
    const Palette& palette() const { return pal_; }

    void drawMenu();                       // contents of Окна -> Тема

private:
    Mode mode_ = Mode::Dark;
    int accent_ = 0;
    bool pending_ = false;
    int loadedLayout_ = 0;
    Palette pal_{};
    ImFont* ui_ = nullptr;
    ImFont* heading_ = nullptr;
};

} // namespace rts::editor
