// Presentation for the generic RTS module, shared by the game client and the editor.
// Objects are drawn from their type's "view" block in rules.json, e.g.
//   "view": { "shape": "soldier", "scale": 1.0, "color": [0.8, 0.7, 0.5] }
//   "view": { "model": "models/knight.glb", "height": 1.1, "team": ["Cloth"],
//             "animations": { "move": {"clip": "Walk", "speedRef": 2.8}, "attack": "Slash" },
//             "procedural": [{ "type": "aim", "node": "Turret" }] }
// The full format (models, materials, clips, procedural animation, attachments, stages, LOD,
// effects) is in docs/models.md.
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
#include "RtsGroundFx.h"
#include "RtsLights.h"
#include "RtsFog.h"
#include "RtsPlanetRender.h"
#include "RtsOverlays.h"
#include "RtsTerrainRender.h"

namespace rts::game {

struct Lane;

struct SceneObject {
    Entity entity;               // invalid for editor placements
    uint16_t type = 0;
    uint8_t owner = 0;
    render::float3 pos, facing{1, 0, 0};
    float hp = 1;                // 0..1
    float build = 1;             // construction progress 0..1
    int garrison = -1;           // shown when >= 0
    float carry = 0;             // > 0: carrying resources
    int16_t carryRes = -1;       // resource index of the cargo
    bool selected = false, hovered = false, visible = true, isProjectile = false;
    bool unpowered = false;      // disabled by a power deficit / owner in low power (electric lights dim)
    uint8_t action = 0;          // SpriteAction (sprite animations): idle, move, attack, work
    // animation hints (sceneFromSim fills them; editor placements leave the defaults)
    uint8_t order = 0;           // current order: 0 none, else OrderType + 1
    bool moving = false;
    bool producing = false;      // structure training / researching
    bool hasTarget = false;      // combat target (turrets aim at it)
    render::float3 target;
    Entity source;               // projectiles: who fired (muzzle point)
    uint16_t shooterType = UINT16_MAX; // projectiles: type of the unit that fired it (lights)
    // planets: body index (PlanetWorld) and pos / facing / target body-local (origin = body centre);
    // -1 = flat map coordinates (on planet maps the view projects them onto the start body, RtsPlanetView.cpp)
    int16_t body = -1;
};

struct RtsScene {
    std::vector<SceneObject> objects;
    std::vector<std::pair<render::float3, render::float3>> roads;
    std::vector<render::float3> moveMarkers;
    // events of the last simulation tick (attacks, hits, deaths, spawns) for animations; `tick`
    // tells the view whether they are new
    std::vector<SimEvent> events;
    uint32_t tick = 0;
    double seconds = 0;          // game time (tick / tick rate): drives the day/night cycle
    // Day/night phase from the simulation (0..1, 0 = midnight, 0.5 = noon), when a gameplay day cycle
    // exists (mechanic E, rules.cycle); < 0: the lighting block's own clock. Visual and gameplay
    // night then always agree.
    float cycle = -1;
    // ground overlays (RtsGroundFx): the fog-of-war viewer (0 = everything) and the sim's lanes (read only)
    uint8_t viewer = 0;
    const std::vector<Lane>* lanes = nullptr;
    uint32_t laneSignature = 0;
    // drag-to-send arrows (Mushroom-Wars input) and box selection are drawn by the client
};

// Things the view noticed this frame, for sound and particles: clip events ("step", "hit"),
// "launch" (a projectile left a muzzle), "death", "spawn". Positions in world space.
struct ViewEvent {
    std::string name;
    Entity entity;
    uint16_t type = 0;
    uint8_t owner = 0;
    render::float3 pos;
};
// Effect anchors active this frame ("fx" in view blocks: smoke on a damaged building, exhaust
// on a moving tank). A particle system spawns at `pos` while the anchor stays listed.
struct ViewEffect {
    std::string effect;
    Entity entity;
    uint16_t type = 0;
    uint8_t owner = 0;
    render::float3 pos;
    float intensity = 1;
};

class RtsView {
public:
    // reader: content files (models, textures); default platform::readContentFile.
    bool init(gfx::Device& dev, const Rules& rules, render::FileReader reader = {});
    void shutdown(gfx::Device& dev);
    // Terrain from the grid (chunked: only chunks whose cells changed are rebuilt); call again after
    // the editor changes the grid. Resource fields / creep changing in a match need no call (the
    // terrain shader reads them from per-cell textures).
    void buildTerrain(gfx::Device& dev, const MapGrid& grid);
    // Optional painted material layer per cell (map key "terrainMaterial": rows of digits 0..3, '.' =
    // by cell kind; see docs/game-format.md). Takes effect at the next buildTerrain().
    void setTerrainMaterial(const Json& rows, int w, int h) { terrain_.setMaterialLayer(parseMaterialRows(rows, w, h)); }
    const TerrainRenderer& terrainRenderer() const { return terrain_; }
    void draw(render::Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid);
    // ImGui: bars, counts. ox/oy: top-left of the view on screen; drawList: ImDrawList* (default: background list)
    void drawOverlays(const render::RTSCamera& cam, const RtsScene& s, float viewW, float viewH, float ox = 0, float oy = 0, void* drawList = nullptr);
    // Building placement preview: the type's model, green when `ok`, red otherwise.
    void drawGhost(render::Renderer3D& r, uint16_t type, render::float3 pos, bool ok, const MapGrid& grid, double time);
    // One object at the origin playing its idle clip (portraits, editor previews).
    void drawPreview(render::Renderer3D& r, uint16_t type, uint8_t owner, double time, float yaw = 0.6f);
    static render::float4 playerColor(int player);
    float selectionRadius(uint16_t type) const;
    float heightAt(const MapGrid& grid, float x, float z) const;
    // Problems found while loading view blocks ("type 'knight': models/knight.glb: file not found").
    const std::vector<std::string>& warnings() const { return warnings_; }
    const render::Model* modelOf(uint16_t type) const;
    const std::vector<ViewEvent>& events() const { return events_; }
    const std::vector<ViewEffect>& effects() const { return effects_; }
    void setSelectionOutline(bool on) { outline_ = on; }
    void setAnimationLod(bool on) { animLod_ = on; }
    // Model files referenced by the view blocks (editor hot reload watches them).
    std::vector<std::string> modelFiles() const;

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
    RtsLights& lights() { return lights_; }
    const FogLook& fogLook() const { return fog_.look(); }

    // ---- planets (RtsPlanetView.cpp): maps with a `world` block (docs/planets.md)
    // Reads map["world"]; no block = the flat view, unchanged. Call before buildTerrain().
    void setWorld(const Json& map);
    // Editor: the world block changed (sculpt / paint): same bodies -> only changed chunks rebuild and the
    // camera stays; different bodies -> setWorld.
    void updateWorld(const Json& map);
    bool planetMode() const { return planets_.active(); }
    PlanetCamera& planetCamera() { return planetCam_; }
    const PlanetCamera& planetCamera() const { return planetCam_; }
    PlanetRenderer& planets() { return planets_; }
    const PlanetRenderer& planets() const { return planets_; }
    // Per frame before beginScene: orbit camera smoothing; returns the RTSCamera to render with (view world).
    const render::RTSCamera& updatePlanetCamera(double seconds, float dt, int viewH);
    // Screen point -> body + body-local surface point (terrain / ocean picking).
    bool pickPlanet(const render::RTSCamera& cam, float sx, float sy, float w, float h, int& body, render::float3& local) const;
    // Until the simulation has bodies, the flat sim map lives on the start body's face 2 (centred):
    bool flatToBody(render::float3 flat, int& body, render::float3& local) const;
    bool bodyToFlat(int body, render::float3 local, render::float3& flat) const;
    // Where the view draws an object (view world), `lift` above its ground point. Flat maps: {x, lift, z}.
    render::float3 objectViewPos(const SceneObject& o, float lift) const;
    render::float3 bodyToView(int body, render::float3 local) const;
    // Body and body-local position of a scene object (flat sim objects: mapped onto the start body).
    bool objectOnBody(const SceneObject& o, int& body, render::float3& local) const {
        if (o.body >= 0) { body = o.body; local = o.pos; return true; }
        return flatToBody(o.pos, body, local);
    }
    // Fog of war of the start body as drawn (6 * N * N, 0 unexplored .. 255 visible); empty = no fog.
    const std::vector<uint8_t>& planetVision() const { return planetVision_; }
    double planetSeconds() const { return planetSeconds_; }
    // Object frame in the view world (columns: east, up, south, ground point); false when not on a body.
    bool planetPlacement(const SceneObject& o, render::mat4& out) const;
    void setFogLook(const FogLook& l) { fog_.setLook(l); fogMeshDirty_ = true; }
    // Overlays (RtsOverlays): selection rings, bars, markers and every game-defined helper element.
    // The client / editor pass the local input state once per frame, before draw().
    void setOverlayContext(const OverlayContext& c) { overlays_.setContext(c); }
    RtsOverlays& overlays() { return overlays_; }
    const RtsOverlays& overlays() const { return overlays_; }
    // Screen point above an object where its bars / labels go (false: behind the camera).
    bool overlayTop(const render::RTSCamera& cam, const SceneObject& o, size_t index, float w, float h, render::float2& out) const;

private:
    struct ClipSpec {
        std::vector<int> clips;    // random pick per play
        float speed = 1, speedRef = 0, fade = 0.2f;
        int layer = 0;             // 0 base, 1 overlay (attack / cast), 2 overlay (hit)
        int mask = -1;             // overlay limited to this node's subtree
        bool loop = true;
        std::vector<std::pair<float, std::string>> events;   // clip time -> event name
        struct BlendPoint { int clip; float speed; };
        std::vector<BlendPoint> blend;                      // blend space by speed (move / carry)
    };
    struct Attachment {
        render::Model* model = nullptr;
        std::vector<render::Material> materials;
        int node = -1;                 // socket node of the host model (-1: object origin)
        render::mat4 local;
        uint32_t when = 0;             // ProcState bits, 0 = always
        int carry = -2;                // -2 not cargo, -1 any cargo, >= 0 resource index
    };
    struct Fx { std::string effect; int node = -1; render::float3 offset; uint32_t when = 0; float hpBelow = 2, intensity = 1; };
    struct AdditiveSpec { int clip = -1; uint32_t when = 0; float weight = 1; };
    struct LegIK { int hip = -1, knee = -1, foot = -1; };
    struct ModelChoice {
        render::Model* model = nullptr;
        std::vector<render::Material> materials;  // per-type copy with overrides applied
        render::mat4 fix;                         // forward axis, fit, offset
        ClipSpec clips[kAnimCount];
        bool hasClip[kAnimCount] = {};
        std::vector<render::ProcAnim> procs;
        bool needsPose = false;                   // a CPU pose is needed (palette skinning, node procs, sockets, IK)
        bool baked = false;                       // skinned from the baked clip texture (instanced crowds)
        int variant = -1;                         // KHR_materials_variants index
        std::vector<Attachment> attach;
        std::vector<Fx> fx;
        std::vector<AdditiveSpec> additive;
        std::vector<LegIK> legs;
        int muzzle = -1;                          // projectile launch node
        render::float3 muzzleOffset;
        float lodDistance = 0;                    // LOD: used beyond this camera distance
        std::vector<int> clipMap, nodeMap;        // LOD: base clip / node -> this model's
    };
    struct Stage { float build = -1, hp = -1; ModelChoice choice; };
    enum class Construction : uint8_t { Grow, Dissolve, Stages };
    struct TypeLook {
        std::vector<ModelChoice> models;          // variants (picked per object)
        std::vector<ModelChoice> lods;            // farther LODs of models[0] (increasing distance)
        std::vector<Stage> stages;                // construction / damage replacements
        float scale = 1;
        render::float4 tint{1, 1, 1, 1};          // linear
        bool air = false, structure = false, shape = true;
        float sx = 1, sz = 1;                     // shapes: footprint scaling
        float corpse = 2.5f;                      // seconds a dead object stays visible
        float turnSpeed = 10;                     // rad/s, 0 = instant
        float flyHeight = 2.4f;
        float unitSpeed = 0;
        Construction construction = Construction::Grow;
        bool dissolveSpawn = false, dissolveDeath = false;
    };
    struct EntityAnim {
        uint16_t type = 0;
        int choice = 0;
        render::Animator anim;
        render::Pose pose;
        std::vector<render::mat4> world;
        std::vector<render::ProcRuntime> rt;
        render::mat4 objXf;                       // procedural whole-object offset
        render::mat4 drawn;                       // last world transform of the host model
        float yaw = 0, speed = 0, distance = 0, turnRate = 0, accel = 0, phase = 0;
        float eventAge[render::kProcEvCount] = {1e9f, 1e9f, 1e9f, 1e9f};
        float castAge = 1e9f;
        render::float3 lastPos;
        bool initialized = false, dead = false;
        float deathAge = 0;
        int baseKind = -1;
        uint64_t seen = 0;
        SceneObject last;
        int prevClip[render::Animator::kLayers] = {-1, -1, -1};
        float prevTime[render::Animator::kLayers] = {0, 0, 0};
        float lodAccum = 0;                       // animation LOD: time not yet applied to the pose
        bool poseValid = false;
        render::float3 launch;                    // projectiles: offset from the muzzle, decays
        bool launched = false;
        double lastSeenTime = -1;                 // previews keep their own clock
    };

    void buildShapes(gfx::Device& dev);
    void buildLook(gfx::Device& dev, const UnitType& t, TypeLook& out);
    bool loadChoice(gfx::Device& dev, const UnitType& t, const Json& view, const std::string& path, ModelChoice& out);
    void configureChoice(gfx::Device& dev, const UnitType& t, const Json& view, ModelChoice& c, bool isShape);
    void configureExtras(gfx::Device& dev, const UnitType& t, const Json& view, ModelChoice& c);
    render::Model* loadModelCached(gfx::Device& dev, const UnitType& t, const std::string& path);
    int loadTexture(gfx::Device& dev, const UnitType& t, render::Model& m, const std::string& path);
    void parseProcedural(const UnitType& t, const Json& list, ModelChoice& c);
    void defaultProcedural(const UnitType& t, const TypeLook& look, ModelChoice& c);
    void finishChoice(const UnitType& t, ModelChoice& c);
    void warn(const UnitType& t, const std::string& msg);
    void handleEvents(const RtsScene& s);
    EntityAnim& stateFor(uint64_t key, const SceneObject& o);
    // viewPos (planets): where the object is drawn (animation LOD); o is then in its body's tangent frame
    void animate(EntityAnim& st, const SceneObject& o, float dt, const MapGrid* grid, const render::float3* viewPos = nullptr);
    void drawPlanets(render::Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid);
    // body-local placement of an object: ground point, unit up, tangent frame (east, south)
    bool placeObject(const SceneObject& o, int& body, render::float3& ground, render::float3& up, render::float3& east, render::float3& south, float& lift) const;
    // placement (planets): object frame in the view world (local +Y = up); null = flat (pos, ground)
    void drawObject(render::Renderer3D& r, EntityAnim& st, const SceneObject& o, float ground, float alpha, const render::float4* tintOverride = nullptr,
                    const render::mat4* placement = nullptr);
    void drawProjectile(render::Renderer3D& r, uint64_t key, const SceneObject& o, float dt);
    void emitClipEvents(EntityAnim& st, const ModelChoice& ch, const SceneObject& o);
    render::float3 nodeWorldPos(const EntityAnim& st, const ModelChoice& ch, int node, render::float3 offset) const;
    uint32_t stateBits(const EntityAnim& st, const SceneObject& o, bool moving) const;

    const Rules* rules_ = nullptr;
    render::FileReader read_;
    gfx::Device* dev_ = nullptr;
    std::map<std::string, std::unique_ptr<render::Model>> models_;  // by path; shapes as "shape:<name>"
    std::vector<TypeLook> looks_;
    std::vector<std::string> warnings_;
    std::unordered_map<uint64_t, EntityAnim> anims_;   // presentation only: iteration order never matters
    render::Mesh projectile_;
    TerrainRenderer terrain_;    // chunked terrain + water (RtsTerrainRender.cpp)
    bool hasTerrain_ = false;
    std::vector<ViewEvent> events_;
    std::vector<ViewEffect> effects_;
    bool outline_ = true, animLod_ = true;
    std::unique_ptr<Rts2DView> twoD_;
    std::string contentBase_;
    std::vector<render::float3> bars_;   // per scene object: health bar anchor of sprite objects
    double lastTime_ = -1;
    uint32_t lastTick_ = ~0u;
    uint64_t frame_ = 0;
    RtsLights lights_;
    RtsGroundFx groundFx_;   // territory, diggable rock, lanes, roads (RtsGroundFx.cpp)
    FogOfWar fog_;           // fog of war look + sheet (RtsFog.cpp)
    RtsOverlays overlays_;   // data-driven overlays (RtsOverlays.cpp)
    bool fogMeshDirty_ = false;
    render::mat4 viewProj_;
    render::float3 camPos_;
    // planets
    PlanetRenderer planets_;
    PlanetCamera planetCam_;
    PlanetViewFrame pframe_;
    double planetSeconds_ = 0;
    std::vector<uint8_t> barOk_, planetVision_;
    // flat sim map on the start body (until the sim has bodies): face 2, centred, scaled to fit
    int demoBody_ = -1, demoW_ = 0, demoH_ = 0;
    float demoOff_[2] = {0, 0}, demoScale_ = 1;
    bool planetNoShadow_ = false;
};

// Build a scene from the live simulation (positions interpolated with alpha). `viewer` filters
// enemies by fog of war (0 = see everything).
void sceneFromSim(Simulation& sim, RtsScene& out, float alpha, uint8_t viewer);

} // namespace rts::game
