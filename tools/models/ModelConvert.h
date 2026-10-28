// Host-side conversion of model formats the runtime does not read (.blend, .lwo, .x, .ms3d, .md2,
// .md3, .smd, .b3d, .ac, .ase, .3mf, ... — everything assimp imports) into glTF binary.
//
// The converted file is a "sidecar" next to the source: models/ship.lwo -> models/ship.lwo.glb.
// rules.json keeps referring to models/ship.lwo; render::loadModel picks the sidecar up. Builds
// (rtsbuild) convert and ship only the sidecar; the editor converts on open / save.
//
// Without assimp (RTS_ASSIMP=OFF, the default) conversion is unavailable, but packing still works:
// pre-converted sidecars are used and sources are left out of the pack.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace rts::pack { class PackWriter; }

namespace rts::models {

// Reads a file by path (relative paths of textures / .mtl next to the model resolve through it).
using ReadFn = std::function<bool(const std::string& path, std::string& out)>;

bool converterAvailable();               // built with assimp
std::string converterVersion();          // "assimp 5.4.3" or ""

// Model extensions (lower case, with the dot) that need conversion: what the converter imports,
// minus the formats the runtime reads natively. Without assimp: a built-in list of common ones.
std::vector<std::string> convertibleExtensions();
bool needsConversion(const std::string& path);
std::string sidecarPath(const std::string& path);   // path + ".glb"

struct ConvertInfo {
    int meshes = 0, materials = 0, textures = 0, clips = 0, nodes = 0, bones = 0;
    std::vector<std::string> warnings;
};
// Converts one model to .glb bytes. `read` resolves the model and its dependencies.
bool convertModel(const std::string& path, const ReadFn& read, std::string& glb, ConvertInfo* info, std::string* error);
// Disk convenience: writes `out` (default: the sidecar). Skips when the sidecar is newer than the
// source unless `force`; `converted` tells whether anything was written.
bool convertFile(const std::string& in, const std::string& out, bool force, bool* converted, ConvertInfo* info, std::string* error);

struct ConvertReport {
    struct Item { std::string path, result; bool ok = true; };
    std::vector<Item> items;
    std::vector<std::string> warnings;
};
// Every convertible model under `dir` (recursively) gets an up-to-date sidecar.
bool convertDirectory(const std::string& dir, bool force, ConvertReport& report, std::string* error);
// Pack step: converts convertible model inputs of `w` (or uses their sidecar when one is already in
// the pack), adds the sidecars and leaves the sources out. Sources that can't be converted stay
// out too and are reported (the game would fail to load them anyway).
bool applyModelConversion(pack::PackWriter& w, ConvertReport& report, std::string* error);

} // namespace rts::models
