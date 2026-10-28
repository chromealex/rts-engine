// Dockable editor windows. Each window is a small class registered with RTS_EDITOR_WINDOW; the
// editor creates one instance of each, lists them in the "Окна" menu, docks them, saves the layout
// and lets agents open/close them over MCP. See docs/editor-windows.md.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "imgui.h"
#include "EditorIcons.h"

namespace rts::editor {

class Editor;

class EditorWindow {
public:
    virtual ~EditorWindow() = default;
    virtual const char* id() const = 0;              // stable, latin: "scene", "inspector"
    virtual const char* title() const = 0;           // shown in the tab: "Scene", "Инспектор"
    virtual const char* icon() const { return nullptr; } // tab icon (EditorIcons.h); nullptr: by id()
    virtual const char* dockArea() const { return "right"; } // default place: left, right, center, bottom
    virtual bool openByDefault() const { return true; }
    virtual int order() const { return 100; }                // tab order inside a dock area (lower = first, the first is shown)
    virtual ImGuiWindowFlags flags() const { return 0; }
    virtual bool zeroPadding() const { return false; } // viewports want edge-to-edge content
    virtual void draw(Editor& ed) = 0;               // inside ImGui::Begin/End of the window
    virtual void update(Editor&, double /*dt*/) {}   // every frame, even when closed or hidden
    bool open = true;
    // ImGui window name: "<icon> <title>###<id>". The ###id part is the ImGui id, so docking, saved
    // layouts and focus lookups stay stable whatever the visible title is. Use this (not title())
    // for ImGui::Begin / DockBuilderDockWindow / SetWindowFocus / FindWindowByName.
    const char* imguiName() const {
        if (name_.empty()) name_ = std::string(icon() ? icon() : windowIconFor(id())) + "  " + title() + "###" + id();
        return name_.c_str();
    }

private:
    mutable std::string name_;
};

struct WindowRegistry {
    using Factory = std::function<std::unique_ptr<EditorWindow>()>;
    static std::vector<Factory>& factories() { static std::vector<Factory> f; return f; }
};

struct WindowRegistrar {
    explicit WindowRegistrar(WindowRegistry::Factory f) { WindowRegistry::factories().push_back(std::move(f)); }
};

} // namespace rts::editor

// Put this once in the .cpp of a window class (files in apps/editor/windows/ are built automatically).
#define RTS_EDITOR_WINDOW(Class) \
    static ::rts::editor::WindowRegistrar rtsEditorWindow_##Class([] { return std::unique_ptr<::rts::editor::EditorWindow>(new Class()); })
