// Fog of war as the player sees it: colour, transparency of unexplored / explored ground, style
// (smooth, cells, grid, dithered pattern), soft edges, reveal fade, cloudy noise, cell grid.
// Data: the visual keys of the rules "fogOfWar" block, game.json "fogOfWar" overrides per key:
//   "fogOfWar": {"mode": "full", "color": [8, 10, 14], "unexplored": 0.92, "explored": 0.47,
//                "style": "smooth", "softness": 2, "fade": 0.3, "noise": 0.3, "height": 1.3,
//                "grid": {"show": "fog", "color": [255, 255, 255], "alpha": 0.12}, "show": true}
// (mode / shareAllies are read by the simulation). The processed field (fade + blur, 0 =
// unexplored, 128 = explored, 255 = visible) is drawn as a sheet draped over the ground, or handed
// to the terrain shader on heightfield maps. Presentation only.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "engine/data/Json.h"
#include "engine/gfx/Gfx.h"
#include "engine/render/Renderer3D.h"

namespace rts::game {

struct FogLook {
    enum Style : int { Smooth = 0, Cells = 1, Grid = 2, Pattern = 3 };
    bool show = true;                // false: the view draws no fog (the simulation still hides enemies)
    float color[3] = {8 / 255.0f, 10 / 255.0f, 14 / 255.0f};
    float unexplored = 0.92f, explored = 0.47f;   // opacity of the fog over never-seen / seen-before ground
    Style style = Smooth;
    int softness = 2;                // blur radius in cells (smooth, pattern)
    float fade = 0.3f;               // seconds for a cell to clear or fog over again
    float noise = 0;                 // 0..1: cloudy, moving edges
    float height = 1.3f;             // sheet height above the ground (covers units and low buildings)
    float gridColor[3] = {1, 1, 1};
    float gridAlpha = 0;             // cell grid lines (0 = none)
    bool gridEverywhere = false;     // grid over the whole map, not only under the fog
};

// rules.json "fogOfWar" first, game.json "fogOfWar" overrides key by key.
FogLook parseFogLook(const Json& rulesFog, const Json& gameFog);

class FogOfWar {
public:
    bool init(gfx::Device& dev);
    void shutdown();
    bool ready() const { return pipe_.valid(); }
    void setLook(const FogLook& look) { look_ = look; dirty_ = true; }
    const FogLook& look() const { return look_; }

    // Sheet draped over the ground (call when the terrain changed). height(x, z): ground height.
    void build(gfx::Device& dev, int w, int h, const std::function<float(float, float)>& height);
    // Per frame: vision of the viewer (0/1/2 per cell) -> faded + blurred field, uploaded when it changed.
    // Returns true when the field changed this frame.
    bool update(const std::vector<uint8_t>& vision, int w, int h, double time);
    const std::vector<uint8_t>& field() const { return field_; }
    // Optional blur replacing the box blur over the grid rectangle (planets: stacked face rows need a
    // seam-aware neighbourhood). Called with the faded field and the blur radius in cells; null = box blur.
    void setBlur(std::function<void(std::vector<uint8_t>& field, int radius)> fn) { blur_ = std::move(fn); dirty_ = true; }
    gfx::TextureHandle texture() const { return tex_; }
    // Draws the sheet (Renderer3D custom draw after the scene). `cells`: weak tier forces Cells.
    void draw(render::Renderer3D& r, double time, bool lowTier);

private:
    void upload();
    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle shader_;
    gfx::PipelineHandle pipe_;
    gfx::BufferHandle vb_, ib_;
    gfx::TextureHandle tex_;
    uint32_t indexCount_ = 0;
    int w_ = 0, h_ = 0;
    FogLook look_;
    std::vector<uint8_t> target_, cur_, field_;
    std::vector<uint16_t> tmp_;
    double lastTime_ = -1;
    bool dirty_ = true, fading_ = false;
    std::function<void(std::vector<uint8_t>&, int)> blur_;
};

} // namespace rts::game
