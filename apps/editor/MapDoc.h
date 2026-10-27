// Editable map document for the generic RTS format: terrain grid, players, placed objects, roads.
// Load/save JSON, validation against the game's rules, undo/redo, symmetry.
#pragma once
#include <cstdint>
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
    int owner = 0;
    int garrison = -1;       // -1 = default
    float amount = -1;       // resource override, -1 = default
};

struct DocPlayer {
    std::string name;
    int team = 1;
    std::vector<std::pair<std::string, float>> resources; // start overrides
};

struct MapData {
    std::string name = "New map";
    int w = 64, h = 48;
    bool roadsOnly = false;
    std::vector<char> cells;            // terrain chars: '.' ground, '~' water, '#' blocked, ',' no-build, '=' shallow
    std::vector<int16_t> heights;       // 1/16 units
    std::vector<DocPlayer> players{{"Blue", 1, {}}, {"Red", 2, {}}};
    std::vector<DocEntity> entities;
    std::vector<std::pair<int, int>> roads; // entity indices
    char cell(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h ? cells[size_t(y * w + x)] : '#'; }
};

struct Issue { bool error = true; std::string text; int entity = -1; };

class MapDoc {
public:
    MapData data;
    std::string path;
    bool dirty = false;

    void reset(int w, int h);
    bool load(const std::string& file, std::string* err);
    bool save(const std::string& file, std::string* err);
    std::string toJsonText() const;

    void beginEdit();
    int addEntity(const DocEntity& e);
    void removeEntity(int i);
    bool toggleRoad(int a, int b);
    void removeRoad(size_t i);
    void resize(int w, int h);
    void paint(int cx, int cy, int radius, char c);
    void raise(int cx, int cy, int radius, int delta);
    std::string uniqueId(const std::string& base) const;
    void mirrorHorizontal();          // copy the left half onto the right, owners 1<->2
    void mirrorPoint();               // 180° rotation of the left half

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();
    void redo();

    std::vector<Issue> validate(const game::Rules& rules) const;

private:
    void mirror(bool point);
    std::vector<MapData> undo_, redo_;
};

} // namespace rts::editor
