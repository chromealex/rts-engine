// Presentation for the generic RTS module, shared by the game client and the editor.
// Objects are drawn from their type's "view" block in rules.json:
//   "view": { "shape": "soldier", "scale": 1.0, "color": [0.8, 0.7, 0.5] }
// Built-in procedural shapes cover common archetypes until real models (glTF) are added:
//   units:      worker, soldier, ranged, vehicle, tank, flyer, mushroom, generic
//   structures: hall, house, barracks, workshop, factory, tower, mine, tree, rock, mushroomhouse, generic
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "../../../engine/render/Renderer3D.h"
#include "../../../engine/sim/Simulation.h"
#include "../RtsData.h"
#include "../RtsMap.h"

namespace rts::game {

struct SceneObject {
    Entity entity;               // invalid for editor placements
    uint16_t type = 0;
    uint8_t owner = 0;
    render::float3 pos, facing{1, 0, 0};
    float hp = 1;                // 0..1
    float build = 1;             // construction progress 0..1
    int garrison = -1;           // shown when >= 0
    float carry = 0;             // > 0: carrying resources
    bool selected = false, hovered = false, visible = true, isProjectile = false;
};

struct RtsScene {
    std::vector<SceneObject> objects;
    std::vector<std::pair<render::float3, render::float3>> roads;
    std::vector<render::float3> moveMarkers;
    // drag-to-send arrows (Mushroom-Wars input) and box selection are drawn by the client
};

class RtsView {
public:
    bool init(gfx::Device& dev, const Rules& rules);
    void shutdown(gfx::Device& dev);
    // Terrain mesh from the grid; call again after the editor changes the grid.
    void buildTerrain(gfx::Device& dev, const MapGrid& grid);
    void draw(render::Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid);
    // ImGui: bars, counts. ox/oy: top-left of the view on screen; drawList: ImDrawList* (default: background list)
    void drawOverlays(const render::RTSCamera& cam, const RtsScene& s, float viewW, float viewH, float ox = 0, float oy = 0, void* drawList = nullptr);
    static render::float4 playerColor(int player);
    float selectionRadius(uint16_t type) const;
    float heightAt(const MapGrid& grid, float x, float z) const;

private:
    struct Part { render::Mesh* mesh; render::mat4 xf; render::float4 color; bool team; };
    struct Shape { std::vector<Part> parts; };
    struct TypeLook { const Shape* shape; float scale; render::float4 tint; bool air; float sx, sz; };
    void buildShapes(gfx::Device& dev);
    render::Mesh& mesh(gfx::Device& dev, const std::string& key, const render::MeshBuilder& b);

    const Rules* rules_ = nullptr;
    std::unordered_map<std::string, render::Mesh> meshes_;
    std::unordered_map<std::string, Shape> shapes_;
    std::vector<TypeLook> looks_;
    render::Mesh terrain_, water_, projectile_;
    bool hasTerrain_ = false;
};

// Build a scene from the live simulation (positions interpolated with alpha). `viewer` filters
// enemies by fog of war (0 = see everything).
void sceneFromSim(Simulation& sim, RtsScene& out, float alpha, uint8_t viewer);

} // namespace rts::game
