// Presentation for the generic RTS module, shared by the game client and the editor.
// Objects are drawn from their type's "view" block in rules.json, e.g.
//   "view": { "shape": "soldier", "scale": 1.0, "color": [0.8, 0.7, 0.5] }
//   "view": { "model": "models/knight.glb", "height": 1.1, "team": ["Cloth"],
//             "animations": { "move": {"clip": "Walk", "speedRef": 2.8}, "attack": "Slash" },
//             "procedural": [{ "type": "aim", "node": "Turret" }] }
// The full format (models, materials, clips, procedural animation) is in docs/models.md.
// Built-in procedural shapes cover common archetypes and are the fallback when a model fails:
//   units:      worker, soldier, ranged, vehicle, tank, flyer, mushroom, generic
//   structures: hall, house, barracks, workshop, factory, tower, mine, tree, rock, mushroomhouse, structure
#pragma once
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../../engine/render/Animation.h"
#include "../../../engine/render/Renderer3D.h"
#include "../../../engine/sim/Simulation.h"
#include "../RtsData.h"
#include "../RtsMap.h"
#include "Rts2DView.h"

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
    uint8_t action = 0;          // SpriteAction (sprite animations): idle, move, attack, work
    // animation hints (sceneFromSim fills them; editor placements leave the defaults)
    uint8_t order = 0;           // current order: 0 none, else OrderType + 1
    bool moving = false;
    bool producing = false;      // structure training / researching
    bool hasTarget = false;      // combat target (turrets aim at it)
    render::float3 target;
};

struct RtsScene {
    std::vector<SceneObject> objects;
    std::vector<std::pair<render::float3, render::float3>> roads;
    std::vector<render::float3> moveMarkers;
    // events of the last simulation tick (attacks, hits, deaths, spawns) for animations; `tick`
    // tells the view whether they are new
    std::vector<SimEvent> events;
    uint32_t tick = 0;
    // drag-to-send arrows (Mushroom-Wars input) and box selection are drawn by the client
};

class RtsView {
public:
    // reader: content files (models, textures); default platform::readContentFile.
    bool init(gfx::Device& dev, const Rules& rules, render::FileReader reader = {});
    void shutdown(gfx::Device& dev);
    // Terrain mesh from the grid; call again after the editor changes the grid.
    void buildTerrain(gfx::Device& dev, const MapGrid& grid);
    void draw(render::Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid);
    // ImGui: bars, counts. ox/oy: top-left of the view on screen; drawList: ImDrawList* (default: background list)
    void drawOverlays(const render::RTSCamera& cam, const RtsScene& s, float viewW, float viewH, float ox = 0, float oy = 0, void* drawList = nullptr);
    static render::float4 playerColor(int player);
    float selectionRadius(uint16_t type) const;
    float heightAt(const MapGrid& grid, float x, float z) const;
    // Problems found while loading view blocks ("type 'knight': models/knight.glb: file not found").
    const std::vector<std::string>& warnings() const { return warnings_; }
    const render::Model* modelOf(uint16_t type) const;

    enum AnimKind { kAnimIdle, kAnimMove, kAnimAttack, kAnimGather, kAnimBuild, kAnimCarry, kAnimDie, kAnimSpawn,
                    kAnimHit, kAnimWork, kAnimConstruct, kAnimCast, kAnimCount };
    static const char* animName(int kind);

    // ---- 2D games (Rts2DView): sprite types and tile maps
    // Prefix for content paths (editor: "<project dir>/"); call before init().
    void setContentBase(const std::string& base) { contentBase_ = base; }
    // The map's "tiles" block (null = none); takes effect at the next buildTerrain().
    void setTiles(const Json& tiles) { if (twoD_) twoD_->setTiles(tiles); }
    bool hasTiles() const { return twoD_ && twoD_->hasTiles(); }
    // Screen-space hit test for sprite objects (they are drawn above their ground point).
    bool spriteHit(const render::RTSCamera& cam, const SceneObject& o, const MapGrid& grid, float sx, float sy, float w, float h) const;

private:
    struct ClipSpec {
        std::vector<int> clips;    // random pick per play
        float speed = 1, speedRef = 0, fade = 0.2f;
        int layer = 0;             // 0 base, 1 overlay (attack / cast), 2 overlay (hit)
        int mask = -1;             // overlay limited to this node's subtree
        bool loop = true;
    };
    struct ModelChoice {
        render::Model* model = nullptr;
        std::vector<render::Material> materials;  // per-type copy with overrides applied
        render::mat4 fix;                         // forward axis, fit, offset
        ClipSpec clips[kAnimCount];
        bool hasClip[kAnimCount] = {};
        std::vector<render::ProcAnim> procs;
        bool needsPose = false;                   // clips or node-targeted procedural animation
        int variant = -1;                         // KHR_materials_variants index
    };
    struct TypeLook {
        std::vector<ModelChoice> models;          // variants (picked per object)
        float scale = 1;
        render::float4 tint{1, 1, 1, 1};          // linear
        bool air = false, structure = false, shape = true;
        float sx = 1, sz = 1;                     // shapes: footprint scaling
        float corpse = 2.5f;                      // seconds a dead object stays visible
        float turnSpeed = 10;                     // rad/s, 0 = instant
        float flyHeight = 2.4f;
        float unitSpeed = 0;
    };
    struct EntityAnim {
        uint16_t type = 0;
        int choice = 0;
        render::Animator anim;
        render::Pose pose;
        std::vector<render::mat4> world;
        std::vector<render::ProcRuntime> rt;
        render::mat4 objXf;                       // procedural whole-object offset
        float yaw = 0, speed = 0, distance = 0, turnRate = 0, accel = 0, phase = 0;
        float eventAge[render::kProcEvCount] = {1e9f, 1e9f, 1e9f, 1e9f};
        float castAge = 1e9f;
        render::float3 lastPos;
        bool initialized = false, dead = false;
        float deathAge = 0;
        int baseKind = -1;
        uint64_t seen = 0;
        SceneObject last;
    };

    void buildShapes(gfx::Device& dev);
    void buildLook(gfx::Device& dev, const UnitType& t, TypeLook& out);
    bool loadChoice(gfx::Device& dev, const UnitType& t, const Json& view, const std::string& path, ModelChoice& out);
    void configureChoice(gfx::Device& dev, const UnitType& t, const Json& view, ModelChoice& c, bool isShape);
    int loadTexture(gfx::Device& dev, const UnitType& t, render::Model& m, const std::string& path);
    void parseProcedural(const UnitType& t, const Json& list, ModelChoice& c);
    void defaultProcedural(const UnitType& t, const TypeLook& look, ModelChoice& c);
    void warn(const UnitType& t, const std::string& msg);
    void handleEvents(const RtsScene& s);
    EntityAnim& stateFor(uint64_t key, const SceneObject& o);
    void animate(EntityAnim& st, const SceneObject& o, float dt);
    void drawObject(render::Renderer3D& r, EntityAnim& st, const SceneObject& o, float ground, float alpha);

    const Rules* rules_ = nullptr;
    render::FileReader read_;
    std::map<std::string, std::unique_ptr<render::Model>> models_;  // by path; shapes as "shape:<name>"
    std::vector<TypeLook> looks_;
    std::vector<std::string> warnings_;
    std::unordered_map<uint64_t, EntityAnim> anims_;   // presentation only: iteration order never matters
    render::Mesh terrain_, water_, projectile_;
    bool hasTerrain_ = false;
    std::unique_ptr<Rts2DView> twoD_;
    std::string contentBase_;
    std::vector<render::float3> bars_;   // per scene object: health bar anchor of sprite objects
    double lastTime_ = -1;
    uint32_t lastTick_ = ~0u;
    uint64_t frame_ = 0;
};

// Build a scene from the live simulation (positions interpolated with alpha). `viewer` filters
// enemies by fog of war (0 = see everything).
void sceneFromSim(Simulation& sim, RtsScene& out, float alpha, uint8_t viewer);

} // namespace rts::game
