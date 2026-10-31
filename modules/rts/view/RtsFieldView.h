// Grid field overlays (rules "fields".<id>.view, docs/fields.md): every visible field is a sheet draped over the
// ground that samples an RGBA8 texture of the field (a = value normalised to the colour range, rgb = owner
// colour of per-player fields) and colours it in the shader: a ramp of up to 4 stops, a threshold, alpha or
// additive blending, animated noise (fire flicker, water ripples, creep pulse, drifting gas). Optional
// particles: a bounded number of one-shot effects (fx/<name>.json) on hot cells the viewer can see, handed to
// the host with takeEffects(). Presentation only: reads RtsScene::fields (sim or editor data).
// Quality: lowTier (weak GPUs: Renderer3D materialQuality Low or shadows off/low, RTS_FIELDS=low) uses a
// half-resolution texture, no animation and no particles. GLSL + MSL in RtsFieldView.cpp.
//
// "view": {"show": true, "preset": "fire" | "water" | "lava" | "gas" | "creep" | "heat",
//          "colors": [[value, "#rrggbb", alpha], ...] (up to 4 stops, values in field units),
//          "range": [lo, hi], "threshold": v, "blend": "alpha" | "add", "animate": "none" | "fire" | "water" |
//          "pulse" | "drift", "speed": 1, "height": 0.06, "cells": false, "fog": "explored" | "visible" | "none",
//          "particles": {"effect": "fire_smoke", "above": v, "max": 24, "every": 1.5}}
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../../engine/gfx/Gfx.h"
#include "../../../engine/render/Renderer3D.h"
#include "../RtsGridFieldData.h"

namespace rts::game {

struct MapGrid;
class RtsView;

// One field layer set as the view sees it (RtsScene::fields): values of all layers, row-major per layer.
struct FieldSceneLayer {
    int field = 0;
    int w = 0, h = 0, res = 1, layers = 1;
    const int32_t* v = nullptr;
    uint32_t version = 0;
};

class RtsFieldView {
public:
    struct Look {
        bool show = true;
        float stops[4] = {0, 0.33f, 0.66f, 1};
        float color[4][4] = {{1, 1, 1, 0}, {1, 0.8f, 0.3f, 0.5f}, {1, 0.4f, 0.1f, 0.7f}, {0.8f, 0.1f, 0.05f, 0.85f}};
        double lo = 0, hi = 100, threshold = 1;
        bool additive = false, cells = false;
        int animate = 0;               // 0 none, 1 fire, 2 water, 3 pulse, 4 drift
        float speed = 1, height = 0.06f;
        int fog = 1;                   // 0 none, 1 explored, 2 visible
        std::string particle;
        double particleAbove = 1;
        int particleMax = 24;
        float particleEvery = 1.5f;
    };
    struct Effect { std::string name; render::float3 pos; };

    bool init(gfx::Device& dev);
    void shutdown();
    void setFields(const std::vector<GridFieldDef>& defs);    // looks from the definitions' "view" blocks
    void invalidate() { meshDirty_ = true; }                   // terrain changed: rebuild the sheet
    void draw(render::Renderer3D& r, const std::vector<FieldSceneLayer>& layers, const std::vector<uint8_t>* vision,
              const MapGrid& grid, const RtsView& view, double time, bool lowTier, uint8_t viewer);
    std::vector<Effect> takeEffects() { std::vector<Effect> e; e.swap(effects_); return e; }
    const Look* look(int field) const { return field >= 0 && size_t(field) < looks_.size() ? &looks_[size_t(field)] : nullptr; }
    static Look parseLook(const GridFieldDef& d);

private:
    struct Tex { gfx::TextureHandle tex; int w = 0, h = 0; uint32_t version = ~0u; bool low = false; double lastFog = -1; std::vector<uint8_t> px; double nextFx = 0; uint32_t fxSeed = 0; };
    void buildMesh(const MapGrid& g, const RtsView& view);
    void upload(Tex& t, const FieldSceneLayer& L, const Look& look, const std::vector<uint8_t>* vision, const MapGrid& g, bool low, uint8_t viewer);
    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle shader_;
    gfx::PipelineHandle alpha_, add_;
    gfx::BufferHandle vb_, ib_;
    uint32_t indexCount_ = 0;
    int meshW_ = 0, meshH_ = 0;
    bool meshDirty_ = true;
    std::vector<Look> looks_;
    std::vector<uint8_t> fixed_;    // per field: Q16 values
    double unitScale_ = 1;
    std::vector<Tex> tex_;
    std::vector<Effect> effects_;
};

} // namespace rts::game
