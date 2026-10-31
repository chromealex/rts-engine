// Icons of the editor and of its project files (.rtsproj), drawn from the editor logo
// (apps/editor/branding/icon_1024.png; the engine's built-in icon when it's missing), so every OS gets
// them from the same code — the macOS bundle at build time (rts_projicon), Windows / Linux when the
// editor registers the file type.
#pragma once
#include <string>
#include <vector>

#include "tools/branding/BrandImage.h"

namespace rts::branding {

// `logo`: square master image (nullptr = the engine's built-in icon)
Image editorAppIcon(int size, const Image* logo = nullptr);        // rounded tile (launcher icon, Linux)
Image projectDocumentIcon(int size, const Image* logo = nullptr);  // a page with a folded corner, the logo and an "RTS" band

// Writes into `dir`: rtsproj.png (256), rtsproj.ico, rtsproj.icns, editor.png (256), editor.ico, from
// the logo PNG at `logoPath` (empty / unreadable: built-in icon). The macOS app icon itself is the
// committed apps/editor/branding/AppIcon.icns. Returns the written paths, or empty + err.
std::vector<std::string> writeEditorIcons(const std::string& dir, const std::string& logoPath = "", std::string* err = nullptr);

} // namespace rts::branding
