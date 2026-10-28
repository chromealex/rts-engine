// Build step: replaces small 2D images in a pack by texture atlas pages + a manifest
// (host tools only: rtsbuild, assetpipe, tests). Configuration: the optional "atlas" block of
// game.json (docs/game-format.md, "Texture atlases"). Output is reproducible.
//
// Grouping (what may share a page) — an image joins group key
//   <usage group> | <kind> [| opaque|alpha when the group splits by alpha] [| mips]
// usage group : "files" rule "group", else derived from the top folder (ui/icons/portraits/cursors
//               -> ui; sprites/units/buildings -> units; tiles/tilesets/terrain -> tiles;
//               effects/fx/particles -> effects); other folders (models/, textures/) are not
//               atlased unless a rule names a group;
// kind        : color | normal | mask — normal maps (*_n, *_nrm, *_normal, normals/) never share a
//               page with colour;
// never atlased: noAtlas, wrap "repeat" (or *_repeat/*_tiling), images above maxTextureSize,
//               undecodable files. Inside a group, images of one folder (a unit's frames, a
//               faction) are kept on the same page when possible.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "engine/atlas/AtlasFormat.h"
#include "engine/data/Json.h"
#include "tools/atlas/AtlasPacker.h"

namespace rts::pack { class PackWriter; struct WriteStats; }

namespace rts::atlas {

struct FileProps {
    std::string group;            // "" = not a candidate
    std::string kind = "color";   // color | normal | mask
    bool noAtlas = false, repeat = false, rotate = false, mips = false, trim = true, splitAlpha = false;
    int padding = -1, extrude = -1; // -1 = profile default
};

struct AtlasConfig {
    bool enabled = true;
    std::string profile;          // rtsbuild target (web, android, ios, macos, windows, linux) or custom
    PackSettings pack;            // maxPage from the profile, padding, extrude, pow2
    bool trim = true;
    int maxTextureSize = 0;       // 0 = maxPage / 4
    int mipLevels = 2;            // mip-safe padding/alignment = 2^levels texels for groups with mips
    std::string pageFormat = "rtex"; // rtex | png
    Json groups = Json::object(); // per-group overrides
    Json files = Json::object();  // glob -> props, in file order, later rules override earlier
    // Images used as frame-grid sprite sheets ("sheet": "<path>" anywhere in the game's JSON):
    // renderers address frames by linear uv over the whole image, so they are never trimmed or
    // rotated. applyAtlas fills it from the pack inputs.
    std::set<std::string> sheets;
    // Parses game.json["atlas"] for one profile; throws std::runtime_error with a readable message.
    static AtlasConfig fromGame(const Json& gameJson, const std::string& profile);
    FileProps classify(const std::string& packPath) const;
    std::string key() const;      // identifies the output (packs for equal keys are identical)
};

struct SourceImage { std::string path, bytes; };

struct GroupStats {
    std::string name;
    int textures = 0, unique = 0, pages = 0;
    uint64_t usedArea = 0, pageArea = 0;
};

struct AtlasStats {
    bool enabled = false;
    std::string profile;
    int maxPage = 0;
    int candidates = 0, atlased = 0, duplicates = 0, pages = 0;
    uint64_t sourceBytes = 0;        // original files replaced by the atlas
    uint64_t pageBytes = 0;          // page files + manifest, uncompressed
    uint64_t storedBytes = 0;        // page files + manifest as stored in the pack (after write)
    uint64_t gpuBytesBefore = 0, gpuBytesAfter = 0; // RGBA8 texture memory
    uint64_t usedArea = 0, pageArea = 0;
    std::vector<GroupStats> groups;
    std::vector<std::pair<std::string, std::string>> excluded; // path, reason
    std::vector<std::string> warnings;
    double occupancy() const { return pageArea ? double(usedArea) / double(pageArea) : 0.0; }
    Json toJson() const;
    std::string report() const;      // user-facing (Russian), appended to the size report
};

struct AtlasOutput {
    Manifest manifest;
    std::vector<std::pair<std::string, std::string>> files; // pack path -> bytes (pages + manifest)
    std::vector<std::string> consumed;                       // inputs replaced by the atlas
    std::vector<Image> pageImages;                           // for previews / tests
    AtlasStats stats;
};

bool buildAtlas(const std::vector<SourceImage>& images, const AtlasConfig& cfg, AtlasOutput& out, std::string* error = nullptr);

// rtsbuild/assetpipe: atlases the writer's image inputs in place (removes consumed files, adds
// pages + manifest). No-op when disabled or nothing qualifies.
bool applyAtlas(pack::PackWriter& w, const AtlasConfig& cfg, AtlasStats& stats, std::string* error = nullptr);
// After PackWriter::write: fills storedBytes from the pack statistics.
void finishStats(AtlasStats& stats, const pack::WriteStats& ws);

bool isImagePath(const std::string& path);
// Collects every string value of a "sheet" key (recursively) into `out`.
void collectSheets(const Json& j, std::set<std::string>& out);
void scanSheets(const pack::PackWriter& w, AtlasConfig& cfg);   // over the writer's .json inputs
std::string encodePng(const Image& img);

} // namespace rts::atlas
