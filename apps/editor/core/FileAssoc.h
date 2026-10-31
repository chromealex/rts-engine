// Opening .rtsproj project files with the editor by double-click / "Open with", per user (no
// admin rights):
//   Linux:   shared-mime-info type application/x-rtsengine-project (*.rtsproj) + a .desktop entry
//            (Exec=<editor> %f) + icons in ~/.local/share, default handler in ~/.config/mimeapps.list
//            (xdg-mime when installed); update-mime-database / update-desktop-database when present.
//   Windows: HKCU\Software\Classes\.rtsproj -> RTSEngine.Project (DefaultIcon, shell\open\command).
//   macOS:   the bundle's Info.plist declares the type (com.rtsengine.project); the editor re-registers
//            its bundle with LaunchServices (lsregister -f) when it moved and claims the default role.
// The editor calls ensureFileAssociation() at startup (cheap file / registry check, registers only
// when missing or pointing at another editor); the Project menu and the hub can force it.
#pragma once
#include <string>

namespace rts::editor {

inline constexpr const char* kProjectMimeType = "application/x-rtsengine-project";
inline constexpr const char* kProjectUti = "com.rtsengine.project";
inline constexpr const char* kProjectProgId = "RTSEngine.Project";
inline constexpr const char* kEditorDesktopId = "rtsengine-editor.desktop";

struct AssocStatus {
    bool supported = false;      // this OS / way of running can register (macOS: only from RTSEditor.app)
    bool registered = false;     // .rtsproj opens with this editor (as far as we can tell)
    bool otherDefault = false;   // installed, but the user picked another program for .rtsproj (not overridden at startup)
    std::string detail;          // readable state (Russian), e.g. who handles .rtsproj instead
};

// Master image for the icons registration writes (editor logo, apps/editor/branding/icon_1024.png);
// empty or unreadable: the engine's built-in icon.
void setFileAssocLogo(const std::string& pngPath);

AssocStatus fileAssociationStatus(const std::string& editorExe);
// Registers this editor (exe path; macOS: the binary inside RTSEditor.app) as the handler of .rtsproj.
bool registerFileAssociation(const std::string& editorExe, std::string* message);
// Startup: registers silently when needed. Off with RTS_FILE_ASSOC=off and in automated runs
// (RTS_SCREENSHOT / RTS_EXIT_AFTER_FRAMES); never overrides a program the user picked. Returns a log line when something was done.
std::string ensureFileAssociation(const std::string& editorExe);

// Linux helpers (also used by tests): where the files go for the current $HOME / $XDG_* variables.
std::string xdgDataHome();       // $XDG_DATA_HOME or ~/.local/share
std::string xdgConfigHome();     // $XDG_CONFIG_HOME or ~/.config
// The Exec= value of the .desktop entry for an editor path (quoted per the Desktop Entry spec).
std::string desktopExec(const std::string& editorExe);

} // namespace rts::editor
