// Overlay system of the RTS view: data-driven helper elements on screen and in the world (build grids,
// range rings, rally lines, damage numbers, bars, HUD labels...). Definitions come from
// RtsOverlayData (rules.json / game.json "overlays" + the built-in defaults); this class evaluates
// their conditions against the scene, the read-only simulation and the input state the client gives
// (OverlayContext), and draws them:
//   world pass (RtsView::draw)          hairlines / flat fills through Renderer3D, textured, additive and
//                                       always-on-top geometry through a small batched renderer of its
//                                       own (RtsOverlayShaders.h, GLSL + MSL)
//   screen pass (RtsView::drawOverlays) text, bars, screen-space shapes through the ImGui draw list
//   minimap marks                       minimapMarks(), rasterized by the client's minimap
// Presentation only: never writes into the simulation. Per-frame buffers are reused (no allocations in
// the hot loops once they have grown). Quality tiers cap the work of game overlays on weak GPUs
// (built-in overlays are never capped, so games without the block look exactly as before).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../../engine/render/Renderer3D.h"
#include "../../../engine/render/TextureCache.h"
#include "../../../engine/sim/Simulation.h"
#include "../RtsMap.h"
#include "RtsOverlayData.h"

namespace rts::game {

struct RtsScene;
struct SceneObject;
class RtsView;

struct OverlayPing { render::float3 pos; uint8_t player = 0; };

// Input state of the local player for one frame (client / editor fill it before drawing).
struct OverlayContext {
    Simulation* sim = nullptr;            // live match, read only (null: editor scene, previews)
    uint8_t player = 0;                   // local player slot (0: spectator / editor)
    bool hasCursor = false;
    render::float3 cursor;                // cursor on the ground (world)
    int placeType = -1;                   // structure being placed (-1: not placing)
    render::float3 placeAt;               // its snapped centre
    bool placeOk = true;                  // placement valid there
    const bool* keys = nullptr;           // held keys, indexed by platform::Key
    int keyCount = 0;
    const char* mode = "normal";          // client input mode: normal, attack, patrol, place, rally, ability, claim
    bool paused = false, over = false;
    const std::vector<OverlayPing>* pings = nullptr;     // pings that happened this frame
    const Json* areas = nullptr;          // map "areas" (name -> [x, z, w, h]) when there is no live match
    const std::vector<std::string>* preview = nullptr;   // editor: overlay ids drawn regardless of `when`
};

struct OverlayMinimapMark { float x = 0, z = 0, radius = 1; uint32_t rgba = 0; bool ring = true; };

enum class OverlayQuality : uint8_t { Low, Medium, High };

class RtsOverlays {
public:
    // Definitions (no GPU: tests and tools evaluate overlays without a device).
    void init(const Rules& rules, const Json& rulesOverlays, const Json& gameOverlays, const std::string& contentBase);
    void initGpu(gfx::Device& dev);   // after init: icon textures, batch shader (RtsOverlaysDraw.cpp)
    void shutdown(gfx::Device& dev);
    void setContext(const OverlayContext& c) { ctx_ = c; }
    const OverlayContext& context() const { return ctx_; }
    // Default: from the renderer's material quality (RTS_OVERLAYS=low|medium|high overrides).
    void setQuality(OverlayQuality q) { quality_ = q; qualityForced_ = true; }
    OverlayQuality quality() const { return quality_; }
    const OverlaySet& set() const { return set_; }
    const std::vector<OverlayMinimapMark>& minimapMarks() const { return marks_; }

    void drawWorld(render::Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsView& view);
    void drawScreen(const render::RTSCamera& cam, const RtsScene& s, float w, float h, float ox, float oy, void* drawList, const RtsView& view);

    // ---- evaluation (drawWorld calls prepare; tests call it directly)
    void prepare(const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, float zoom);
    bool test(const OvCondition& c, const SceneObject* o) const;   // o: anchored entity (null: global scope)
    bool matches(const OvFilter& f, const SceneObject& o) const;
    bool cellTest(OvPred p, int x, int y) const;
    uint8_t relation(uint8_t owner) const;                          // OvRelation of an owner slot
    uint32_t entityFlags(const SceneObject& o) const;
    size_t popupCount() const { return popups_.size(); }

private:
    struct Inst {
        render::float3 pos;              // world point (y = ground)
        int obj = -1;                    // scene object index
        uint16_t type = kNoType;
        uint8_t owner = 0;
        bool ok = true;                  // ghost: placement valid
        float alpha = 1, age = 0;        // fade; event popups: seconds alive
        int popup = -1;
        uint32_t pathBegin = 0, pathCount = 0;
        bool hasTo = false;
        render::float3 to;
        float rect[4] = {0, 0, 0, 0};    // area anchors
        float sx = 0, sy = 0;            // screen anchors (points)
    };
    struct Popup {
        uint16_t def = 0;
        render::float3 pos;
        double born = 0;
        int64_t value = 0;
        float amount = 0;
        int16_t res = -1;
        uint8_t owner = 0, player = 0;
        uint16_t type = kNoType, kind = 0;
        float jx = 0, jz = 0;
    };
    struct DefState { double since = -1, lastTrue = -1; bool on = false; };
    struct BatchVertex { render::float3 pos; float u, v; uint32_t rgba; };
    struct Bucket { gfx::TextureHandle tex; uint8_t pipe = 0; std::vector<BatchVertex> verts; };

    // RtsOverlays.cpp
    void processEvents(const RtsScene& s);
    void spawnPreviewPopups();
    bool gather(const OverlayDef& d, size_t di, bool screenPass);
    bool defVisible(const OverlayDef& d, size_t di, bool cond, float& alpha);
    bool previewed(const OverlayDef& d) const;
    float radiusFor(const OverlayDef& d, const Inst& in) const;
    render::float4 colorFor(const OverlayDef& d, const Inst& in, int pred) const;
    float animAlpha(const OverlayDef& d, const Inst& in) const;
    float valueOf(const OverlayDef& d, const SceneObject& o) const;
    const std::string& format(const OverlayDef& d, const Inst& in);
    size_t instCap(const OverlayDef& d) const;
    size_t cellCap(const OverlayDef& d) const;
    bool resolveTo(const OverlayDef& d, const Inst& in, render::float3& out) const;
    // RtsOverlaysDraw.cpp: world geometry
    void emit(const OverlayDef& d, size_t di, const Inst& in);
    float groundY(const OverlayDef& d, float x, float z, float anchorGround) const;
    void seg(const OverlayDef& d, render::float3 a, render::float3 b, uint32_t col);
    void tri(const OverlayDef& d, render::float3 a, render::float3 b, render::float3 c, uint32_t col, gfx::TextureHandle tex = {},
             const float* uv = nullptr);
    void polyline(const OverlayDef& d, const render::float3* pts, size_t n, bool closed, uint32_t col, float anchorGround, bool follow);
    void ring(const OverlayDef& d, const Inst& in, float radius, uint32_t col, uint32_t fill);
    void rectShape(const OverlayDef& d, const Inst& in, float x0, float z0, float x1, float z1, uint32_t col, uint32_t fill);
    void grid(const OverlayDef& d, const Inst& in);
    void cells(const OverlayDef& d, const Inst& in);
    void icon(const OverlayDef& d, size_t di, const Inst& in, float size, uint32_t col);
    void arrowHead(const OverlayDef& d, render::float3 a, render::float3 b, uint32_t col, float anchorGround);
    Bucket& bucket(gfx::TextureHandle tex, uint8_t pipe);
    void flush(render::Renderer3D& r);
    // RtsOverlaysDraw.cpp: screen elements
    void drawScreenInst(const OverlayDef& d, size_t di, const Inst& in, float sx, float sy, void* dl);

    const Rules* rules_ = nullptr;
    gfx::Device* dev_ = nullptr;
    std::string base_;
    render::TextureCache textures_;
    std::vector<render::TextureRef> images_;   // per def
    OverlaySet set_;
    std::vector<DefState> state_;
    std::vector<uint16_t> screenEntityDefs_, screenOtherDefs_, worldDefs_;
    OverlayContext ctx_;
    OverlayQuality quality_ = OverlayQuality::High;
    bool qualityForced_ = false;

    // frame state (prepare)
    const RtsScene* scene_ = nullptr;
    const MapGrid* grid_ = nullptr;
    const std::vector<uint8_t>* vision_ = nullptr;
    const RtsView* view_ = nullptr;
    render::Renderer3D* r_ = nullptr;
    double time_ = 0, lastPreview_ = -1;
    float zoom_ = 30;
    float lift_ = 0.05f;                      // current element: style lift (+ popup rise)
    render::float3 camTarget_, camRight_{1, 0, 0}, camUp_{0, 1, 0};
    std::vector<int> selected_;
    int hovered_ = -1;
    struct Base { float x, z, r; };
    std::vector<Base> bases_;
    uint32_t lastTick_ = ~0u;
    std::vector<Popup> popups_;
    std::unordered_map<uint64_t, std::pair<float, int16_t>> carry_;   // entity -> last carried amount (resource popups)
    std::vector<Inst> insts_;
    std::vector<render::float3> pathPts_, tmpPts_;
    std::string text_;
    std::vector<OverlayMinimapMark> marks_;

    // batched renderer
    std::vector<Bucket> buckets_;
    std::vector<BatchVertex> upload_;
    gfx::ShaderHandle shader_;
    gfx::PipelineHandle pipes_[4];             // bit 0: additive, bit 1: always on top
    gfx::BufferHandle vb_;
    size_t vbBytes_ = 0;
    gfx::TextureHandle white_;
};

} // namespace rts::game
