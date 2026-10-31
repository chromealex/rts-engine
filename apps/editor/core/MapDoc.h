// Editable map document for the generic RTS format.
// Terrain layers (cell kinds, cliff levels, fine heights, resource fields, regions) and placed
// objects/players are structured for fast editing; every other map key (areas, waves, triggers,
// victory, future blocks) is kept verbatim in `extra`, so the editor never drops data it doesn't
// know about. Undo/redo are whole-document snapshots (maps are small).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "engine/data/Json.h"
#include "modules/rts/RtsData.h"

namespace rts::editor {

struct DocEntity {
    std::string id;          // optional; required for road endpoints
    std::string type;
    float x = 0, z = 0;
    int owner = 0;           // 0 neutral, 1..N players, -1 hostile creeps
    int garrison = -1;       // -1 = default
    float amount = -1;       // resource override, -1 = default
    Json at;                 // planet maps: {"body", "face", "x", "y"} (x/z above mirror at.x/at.y); null on flat maps
    Json extra = Json::object();
};

struct DocPlayer {
    DocPlayer() = default;
    DocPlayer(std::string n, int t) : name(std::move(n)), team(t) {}
    std::string name;
    int team = 1;
    std::string faction;     // empty = rules default
    bool hasStart = false;
    float startX = 0, startZ = 0;
    bool waves = false;      // scripted side (not a real opponent)
    std::vector<std::pair<std::string, float>> resources; // start overrides
    Json startAt;            // planet maps: start as {"body", "face", "x", "y"} (hasStart set); null on flat maps
    Json extra = Json::object();
};

// One body of a planet map's `world` block. Surface bodies keep their per-face gameplay layers and corner
// heights structured (faces 0..5, docs/planets.md §2); every other body key (kind, radius, orbit, visual,
// generate, ...) round-trips through `meta`, other terrain keys through `terrainExtra`.
struct DocBody {
    std::string id;
    int faceSize = 0;                   // 0: no surface (star)
    std::vector<char> cells;            // 6 * N * N, face-major then row-major ('.' etc. as flat maps)
    std::vector<int8_t> levels;         // 6 * N * N
    std::vector<int16_t> corners;       // 6 * (N+1)^2, 1/16 units; seam duplicates equal
    std::string cornersFile;            // terrain.cornersFile as read ("" = inline)
    Json meta = Json::object();
    Json terrainExtra = Json::object(); // per-face extra keys live in faceExtra
    std::vector<Json> faceExtra;        // 6 objects (fields, material, ... per face)
    // visual detail grid (terrain.detail, docs/terrain.md): 6 * (N*k+1)^2 int16 1/256 units, seam duplicates
    // equal; empty = none (the view synthesises one when the style is smooth). Written to detailFile on save.
    int detailScale = 0;
    std::vector<int16_t> detail;
    std::string detailFile;
    bool surface() const { return faceSize > 0; }
    int detailSide() const { return faceSize * detailScale; }   // M
    size_t detailIndex(int face, int i, int j) const { const size_t M1 = size_t(detailSide() + 1); return (size_t(face) * M1 + size_t(j)) * M1 + size_t(i); }
    size_t cellIndex(int face, int x, int y) const { return (size_t(face) * size_t(faceSize) + size_t(y)) * size_t(faceSize) + size_t(x); }
    size_t cornerIndex(int face, int i, int j) const { return (size_t(face) * size_t(faceSize + 1) + size_t(j)) * size_t(faceSize + 1) + size_t(i); }
};

struct MapData {
    std::string name = "New map";
    int w = 64, h = 48;
    bool roadsOnly = false;
    std::vector<char> cells;            // '.' ground '~' water '#' blocked ',' no-build '=' shallow '/' ramp 'R' rock
    std::vector<int8_t> levels;         // cliff level 0..9
    std::vector<int16_t> heights;       // fine height, 1/16 units
    std::vector<int16_t> corners;       // heightfield corner heights (w+1)*(h+1), 1/16 units; empty = none
    std::string cornersFile;            // terrain.cornersFile as read (relative to the game folder); "" = inline / default name
    // visual detail heightmap (terrain.detail, docs/terrain.md): (w*k+1)*(h*k+1) int16 1/16 units; empty = none.
    // detailSplat: optional RGBA8 layer weights per detail sample (painted materials at detail resolution).
    int detailScale = 0;
    std::vector<int16_t> detail;
    std::vector<uint8_t> detailSplat;
    std::string detailFile;             // terrain.detail.file as read ("" = default name next to the map)
    std::vector<std::string> fieldResources;  // resource ids used by fields
    std::vector<int8_t> fieldRes;       // index into fieldResources, -1 none
    std::vector<uint8_t> fieldAmt;      // 0..9 (fraction of the resource's cellMax)
    std::vector<char> regions;          // '.' none, otherwise region letter
    std::map<char, std::string> regionNames;
    std::vector<DocPlayer> players{{"Blue", 1}, {"Red", 2}};
    std::vector<DocEntity> entities;
    std::vector<std::pair<int, int>> roads;   // entity indices
    Json extra = Json::object();               // areas, waves, triggers, victory, ...
    // planet maps (`world` block): the bodies; the flat layers above are then unused (kept at 4x4)
    std::vector<DocBody> bodies;
    Json worldExtra = Json::object();          // world keys other than bodies (start, ...)
    bool planet() const { return !bodies.empty(); }
    int findBody(const std::string& id) const { for (size_t i = 0; i < bodies.size(); ++i) if (bodies[i].id == id) return int(i); return -1; }

    size_t idx(int x, int y) const { return size_t(y * w + x); }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    char cell(int x, int y) const { return inside(x, y) ? cells[idx(x, y)] : '#'; }
    int level(int x, int y) const { return inside(x, y) ? levels[idx(x, y)] : 0; }
};

struct Issue { bool error = true; std::string text; int entity = -1; };

// Maps whose corner grid is larger than this are saved with terrain.cornersFile (raw int16 LE next to the
// map) instead of an inline "corners" array; smaller maps keep the inline array.
constexpr size_t kInlineCornersMax = 257 * 257;

class MapDoc {
public:
    MapData data;
    std::string path;
    bool dirty = false;
    uint64_t revision = 0;   // bumps on every change (views rebuild when it moves)

    void reset(int w, int h);
    // load/save resolve terrain.cornersFile against the game folder (parent of maps/), see cornersPath()
    bool load(const std::string& file, std::string* err);
    bool loadJson(const Json& j, std::string* err);
    bool save(const std::string& file, std::string* err);
    // Big maps carry their corners as terrain.cornersFile + an in-memory terrain.cornersBlob (what the
    // simulation loader reads); save() writes the blob to the .r16 file and never into the JSON.
    Json toJson() const;
    std::string cornersFileName() const;                                          // relative, e.g. maps/big.r16
    std::string detailFileName() const;                                           // maps/<map>.detail.r16
    std::string bodyCornersFileName(const DocBody& b) const;                      // maps/<map>.<body>.r16
    std::string bodyDetailFileName(const DocBody& b) const;                       // maps/<map>.<body>.detail.r16
    static std::string cornersPath(const std::string& mapFile, const std::string& rel); // absolute file
    std::string toJsonText() const { return toJson().dump(); }

    void beginEdit();        // snapshot for undo, marks dirty
    void touched() { dirty = true; ++revision; }
    int addEntity(const DocEntity& e);
    void removeEntity(int i);
    int findEntity(const std::string& id) const;
    bool toggleRoad(int a, int b);
    void removeRoad(size_t i);
    void resize(int w, int h);
    void setCorners(bool on);  // add a flat heightfield (all 0) / remove it (undoable)
    // brushes (no snapshot: call beginEdit() once per stroke)
    void paint(int cx, int cy, int radius, char c);
    void paintRect(int x0, int y0, int x1, int y1, char c);
    void raise(int cx, int cy, int radius, int delta);
    void setLevel(int cx, int cy, int radius, int level);
    void setLevelRect(int x0, int y0, int x1, int y1, int level);
    void paintField(int cx, int cy, int radius, const std::string& res, int amount); // amount 0 erases
    void paintRegion(int cx, int cy, int radius, char letter);                       // '.' erases
    void paintRegionRect(int x0, int y0, int x1, int y1, char letter);
    void autoRamps();        // marks single-step level changes next to existing ramps' rows (helper)
    std::string uniqueId(const std::string& base) const;
    void mirrorHorizontal(); // copy the left half onto the right, owners 1<->2
    void mirrorPoint();      // 180° rotation of the left half
    std::string ascii(int maxW = 128) const; // text preview for agents (terrain + objects)

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();
    void redo();

    // Checks against the rules, then a real simulation setup (catches everything the loader rejects).
    std::vector<Issue> validate(const game::Rules& rules, const Json& rulesJson) const;

    Json worldJson() const;  // the `world` block of a planet map (corners of big bodies as cornersFile + cornersBlob)

private:
    static void loadWorld(const Json& w, MapData& d);
    void validateWorld(const game::Rules& rules, std::vector<Issue>& out) const;
    void mirror(bool point);
    void ensureLayers();
    std::vector<MapData> undo_, redo_;
};

} // namespace rts::editor
