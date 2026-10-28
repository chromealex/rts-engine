// rts_projicon <out dir>: the editor's app icon and the .rtsproj document icon (ProjectIcon.h).
// Used by the macOS editor bundle build (Resources/AppIcon.icns, Resources/rtsproj.icns).
#include <cstdio>
#include <string>

#include "tools/branding/ProjectIcon.h"

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: rts_projicon <out dir>\n"); return 2; }
    std::string err;
    if (rts::branding::writeEditorIcons(argv[1], &err).empty()) { std::fprintf(stderr, "rts_projicon: %s\n", err.c_str()); return 1; }
    return 0;
}
