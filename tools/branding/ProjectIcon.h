// Icons of the editor itself and of its project files (.rtsproj): drawn procedurally from the
// engine's built-in icon (no art files), so every OS gets them from the same code — the macOS
// bundle at build time (rts_projicon), Windows / Linux when the editor registers the file type.
#pragma once
#include <string>
#include <vector>

#include "tools/branding/BrandImage.h"

namespace rts::branding {

Image editorAppIcon(int size);          // rounded tile with the engine icon (app / launcher icon)
Image projectDocumentIcon(int size);    // a page with a folded corner, the engine icon and an "RTS" band

// Writes into `dir`: rtsproj.png (256), rtsproj.ico, rtsproj.icns, editor.png (256), editor.ico,
// AppIcon.icns. Returns the written paths, or empty + err.
std::vector<std::string> writeEditorIcons(const std::string& dir, std::string* err = nullptr);

} // namespace rts::branding
