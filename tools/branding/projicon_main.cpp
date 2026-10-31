// rts_projicon <out dir> [logo.png]: the .rtsproj document icon (+ editor launcher icon) drawn from
// the editor logo (ProjectIcon.h). Used by the macOS editor bundle build (Resources/rtsproj.icns).
#include <cstdio>
#include <string>

#include "tools/branding/ProjectIcon.h"

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) { std::fprintf(stderr, "usage: rts_projicon <out dir> [logo.png]\n"); return 2; }
    std::string err;
    if (rts::branding::writeEditorIcons(argv[1], argc == 3 ? argv[2] : "", &err).empty()) { std::fprintf(stderr, "rts_projicon: %s\n", err.c_str()); return 1; }
    return 0;
}
