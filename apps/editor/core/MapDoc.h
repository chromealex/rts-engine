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
    Json extra = Json::object();
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
    std::vector<std::string> fieldResources;  // resource ids used by fields
    std::vector<int8_t> fieldRes;       // index into fieldResources, -1 none
    std::vector<uint8_t> fieldAmt;      // 0..9 (fraction of the resource's cellMax)
    std::vector<char> regions;          // '.' none, otherwise region letter
    std::map<char, std::string> regionNames;
    std::vector<DocPlayer> players{{"Blue", 1}, {"Red", 2}};
    std::vector<DocEntity> entities;
    std::vector<std::pair<int, int>> roads;   // entity indices
    Json extra = Json::object();               // areas, waves, triggers, victory, ...

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

private:
    void mirror(bool point);
    void ensureLayers();
    std::vector<MapData> undo_, redo_;
};

} // namespace rts::editor
