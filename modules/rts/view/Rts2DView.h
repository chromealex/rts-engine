// 2D presentation for the generic RTS view: sprite-sheet units/buildings and tile maps, drawn with
// render::SpriteBatch inside Renderer3D's main pass. Used by RtsView for every type with a
// "sprite" block (rules.json) and for maps with a "tiles" layer; everything else keeps the 3D
// shapes and terrain, so 2D and 3D objects can share one map.
//
// Sprite sheet layout: one row per facing (row 0 = facing east/+X, then clockwise on screen:
// south-east, south, ...), one column per animation frame; actions name column ranges.
// "mirror": true stores only the rows from north clockwise to south (facings/2 + 1 rows) and
// draws the west side flipped.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../../engine/render/Renderer3D.h"
#include "../../../engine/render/SpriteBatch.h"
#include "../../../engine/render/TextureCache.h"
#include "../RtsData.h"
#include "../RtsMap.h"
#include "../RtsTiles.h"

namespace rts::game {

struct SceneObject;

enum SpriteAction : uint8_t { kActIdle, kActMove, kActAttack, kActWork, kActBuild, kActDie, kActCount };

struct SpriteLook {
    enum Mode : uint8_t { Billboard, Upright, Ground };
    struct Anim { int start = 0, count = 1; float fps = 8; bool loop = true; bool defined = false; };
    bool valid = false;
    std::string sheet;
    render::TextureRef tex;
    int frameW = 32, frameH = 32, facings = 1;
    bool mirror = false, team = true, nearest = true;
    Mode mode = Billboard;
    float anchorX = 0.5f, anchorY = 1.0f;   // pivot in the frame (0..1 from the top-left)
    float width = 1, height = 1;            // world size (cells)
    float offsetRight = 0, offsetFront = 0; // anchor point relative to the object centre (screen right / towards the camera)
    float depthBias = 0;                    // pull towards the camera so terrain does not cut the sprite
    float lift = 0;                         // air units: drawn above the ground, shadow stays
    float corpseTime = 4;                   // seconds the last die frame stays (then fades)
    float shadowW = 0, shadowH = 0, shadowAlpha = 0.35f;
    Anim anims[kActCount];
};

class Rts2DView {
public:
    bool init(gfx::Device& dev, const Rules& rules, const std::string& contentBase);
    void shutdown(gfx::Device& dev);
    bool hasSprite(uint16_t type) const { return type < looks_.size() && looks_[type].valid; }
    const SpriteLook& look(uint16_t type) const { return looks_[type]; }

    // ---- per frame (RtsView::draw)
    void beginFrame(const render::RTSCamera& cam, double time);
    void addObject(const SceneObject& o, float groundY);
    void endFrame(const MapGrid& grid);          // death animations of objects that disappeared
    // queues tiles + sprites into the renderer's main pass (`grid` must outlive the frame)
    void submit(render::Renderer3D& r, const MapGrid& grid, float levelHeight);

    // world point at the top centre of the drawn sprite (health bars)
    render::float3 barPoint(const SceneObject& o, float groundY) const;
    // screen-space hit test against the drawn sprite quad
    bool hit(const render::RTSCamera& cam, const SceneObject& o, float groundY, float sx, float sy, float w, float h) const;

    // ---- tiles (RtsTileView.cpp)
    void setTiles(const Json& tiles);             // map "tiles" block (null: no tile map)
    bool hasTiles() const { return tileset_ != nullptr; }
    void buildTiles(gfx::Device& dev, const MapGrid& grid, float levelHeight);
    void refreshTiles(gfx::Device& dev, const MapGrid& grid, float levelHeight); // depleted fields

private:
    struct Corpse { uint16_t type; uint8_t owner; render::float3 pos, facing; double start; };
    struct Seen { uint16_t type; uint8_t owner; render::float3 pos, facing; bool visible; uint32_t frame; };
    void loadLook(SpriteLook& L, const UnitType& t, const Json& s);
    void emit(const SpriteLook& L, render::float3 ground, render::float3 facing, int act, double t0, uint32_t team, uint32_t tint, bool loopHold, float fade);
    void corners(const SpriteLook& L, render::float3 ground, render::float3 (&c)[4], float& sortKey) const;
    int facingIndex(const SpriteLook& L, render::float3 facing, bool& flip) const;

    std::string base_;
    render::TextureCache textures_;
    render::SpriteBatch batch_;
    render::TextureRef shadowTex_;
    std::vector<SpriteLook> looks_;
    const Rules* rules_ = nullptr;
    bool ready_ = false;

    render::RTSCamera cam_;
    render::float3 camRight_, camUp_, camFwd_, groundRight_, groundFront_;
    double time_ = 0;
    uint32_t frame_ = 0;
    std::map<uint64_t, Seen> seen_;
    std::vector<Corpse> corpses_;

    // tiles
    std::unique_ptr<Tileset> tileset_;
    std::string tilesetPath_;
    Json tilesJson_;
    TileLayer layer_;
    render::TextureRef atlas_;
    render::SpriteMesh tileMesh_;
    std::vector<uint32_t> fieldCells_;     // cells of field terrains (watched for depletion)
    std::vector<uint8_t> fieldState_;
    int depletedOf(int terrain) const;
};

} // namespace rts::game
