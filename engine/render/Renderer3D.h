// Forward renderer for RTS scenes: lit meshes with real-time sun shadows, plus immediate-mode
// colored lines/triangles for overlays (selection, roads, fog). Built only on the RHI, so it runs on
// every backend unchanged.
//
// Frame:  beginScene(camera) -> drawMesh()/line()/... -> render(pass)  [shadow pass + main pass;
//         the main pass is left open so the caller can draw UI, then calls device.endPass()]
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "../gfx/Gfx.h"
#include "Lights.h"
#include "Math3D.h"
#include "Model.h"

namespace rts::render {

struct Mesh {
    gfx::BufferHandle vb, ib;
    uint32_t indexCount = 0;
    float3 boundsMin, boundsMax;
};

struct MeshVertex { float3 pos; float3 nrm; uint32_t rgba; };

inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24); }
inline uint32_t rgbaf(float4 c) {
    auto q = [](float v) { return uint8_t(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); };
    return rgba(q(c.x), q(c.y), q(c.z), q(c.w));
}

// CPU-side mesh assembly: primitives with transforms and per-vertex colors, merged into one mesh.
class MeshBuilder {
public:
    MeshBuilder& color(uint32_t c) { color_ = c; return *this; }
    MeshBuilder& transform(const mat4& m) { xf_ = m; return *this; }
    MeshBuilder& box(float3 center, float3 size);
    MeshBuilder& cylinder(float3 base, float radius, float height, int segments = 16, bool caps = true, float topRadius = -1);
    MeshBuilder& cone(float3 base, float radius, float height, int segments = 16);
    MeshBuilder& sphere(float3 center, float radius, int rings = 8, int segments = 14);
    MeshBuilder& disc(float3 center, float radius, int segments = 24); // facing +Y
    MeshBuilder& quad(float3 a, float3 b, float3 c, float3 d);          // CCW seen from the front
    // Large meshes (terrain) use 32-bit indices automatically.
    Mesh upload(gfx::Device& dev) const;
    bool empty() const { return idx_.empty(); }
    size_t vertexCount() const { return verts_.size(); }
    void clear() { verts_.clear(); idx_.clear(); }
    const std::vector<MeshVertex>& vertices() const { return verts_; }
    const std::vector<uint32_t>& indices() const { return idx_; }

    // raw access for terrain-like meshes
    uint32_t addVertex(float3 p, float3 n, uint32_t c) { verts_.push_back({p, n, c}); return uint32_t(verts_.size() - 1); }
    void addTriangle(uint32_t a, uint32_t b, uint32_t c) { idx_.push_back(a); idx_.push_back(b); idx_.push_back(c); }

private:
    uint32_t add(float3 p, float3 n);
    void face(uint32_t a, uint32_t b, uint32_t c, float3 n);
    std::vector<MeshVertex> verts_;
    std::vector<uint32_t> idx_;
    uint32_t color_ = 0xffffffffu;
    mat4 xf_;
};

struct ColorVertex { float3 pos; uint32_t rgba; };

struct Lighting {
    float3 sunDir{0.55f, 1.0f, 0.35f};    // towards the sun
    float sunIntensity = 1.05f;
    float3 sunColor{1.0f, 0.93f, 0.80f};
    float3 skyColor{0.50f, 0.58f, 0.70f};
    float3 groundColor{0.33f, 0.29f, 0.23f};
    float3 fogColor{0.62f, 0.70f, 0.80f};
    float fogDensity = 0.0025f;
    float shadowSoftness = 0.22f;          // penumbra radius in world units (same look at every quality tier)
};

enum class ShadowQuality { Off, Low, Medium, High };
// Model materials: Low = Blinn-Phong with material factors and base colour texture only;
// Medium = GGX + metallic-roughness, normal and emissive maps; High = + occlusion maps.
enum class MaterialQuality { Low, Medium, High };

struct NodePose;

// Baked skinning sample (Animator::baseSample()): clip A blends to clip B by `mix`; clip -1 = bind pose.
struct BakedSample { int clipA = -1, clipB = -1; float timeA = 0, timeB = 0, mix = 1; bool loopA = true, loopB = true; };

// One model instance. Node matrices come from an animated pose (Animation.h) or the bind pose.
// Rigid parts and baked-skinned meshes of identical instances are drawn instanced (32 per draw).
struct ModelDraw {
    const Model* model = nullptr;
    mat4 world;                                    // model space -> world
    const std::vector<mat4>* nodeWorld = nullptr;  // pose, model space; null = bind pose
    const std::vector<Material>* materials = nullptr; // per-instance override (model->materials first, extra ones after)
    const std::vector<int>* nodeMaterial = nullptr;   // per node: index into materials for its meshes (-1 = own)
    float4 tint{1, 1, 1, 1};                       // rgb multiply (linear), a = opacity (< 1 draws blended)
    float3 team{1, 1, 1};                          // team colour, linear
    float flash = 0;                               // additive highlight (hit feedback)
    int variant = -1;                              // KHR_materials_variants index
    uint32_t flags = 0;                            // DrawFlags (no cast / no receive shadow)
    const std::vector<NodePose>* pose = nullptr;   // morph weights (node weights when null)
    const BakedSample* baked = nullptr;            // skin from Model::baked instead of nodeWorld
    float dissolve = 0;                            // 0..1: burn away (spawn, death, construction)
    float4 outline{0, 0, 0, 0};                    // rgb (linear) + width in world units; a = 0: none
};

enum DrawFlags : uint32_t {
    kDrawUnlit = 1,
    kDrawNoCastShadow = 2,
    kDrawNoReceiveShadow = 4,
};

class Renderer3D {
public:
    bool init(gfx::Device& dev);
    void shutdown();
    void setShadowQuality(ShadowQuality q);
    ShadowQuality shadowQuality() const { return quality_; }
    static ShadowQuality defaultQualityFor(const gfx::Device& dev, bool mobile);

    void beginScene(const RTSCamera& cam, int fbW, int fbH);
    void drawMesh(const Mesh& m, const mat4& model, float4 color, uint32_t flags = 0, float specular = 0.25f, float emissive = 0.0f);
    void line(float3 a, float3 b, uint32_t color);
    void triangle(float3 a, float3 b, float3 c, uint32_t color);    // flat, alpha-blended
    void quadXZ(float3 a, float3 b, float width, uint32_t color, float y = 0.02f);
    void circleXZ(float3 c, float radius, uint32_t color, int segments = 40, float y = 0.03f);
    void drawModel(const ModelDraw& d);
    void render(const gfx::PassDesc& screenPass); // shadow pass + opens the main pass and draws everything
    // Extra renderers (sprites, tiles) that draw with their own pipelines inside the main pass: run
    // after opaque and translucent meshes, before overlays (fog of war stays on top).
    // Cleared by beginScene.
    using CustomDraw = std::function<void(gfx::Device&)>;
    void drawCustom(CustomDraw fn) { custom_.push_back(std::move(fn)); }
    // Inside a custom draw: bind what the lit shaders share (U1 frame, U2 + tex1 point lights, tex0
    // shadow map), so custom lit shaders built on RTS_*_SHADOW / RTS_*_LIGHTS get the same lighting.
    void bindLitFrame();
    // Opaque custom renderers (terrain): run first in the main pass, before meshes and models, so
    // translucent meshes (water) blend over them. Same lifetime rules as drawCustom.
    void drawCustomOpaque(CustomDraw fn) { customOpaque_.push_back(std::move(fn)); }
    const RTSCamera& camera() const { return cam_; }
    int sceneWidth() const { return sceneW_; }
    int sceneHeight() const { return sceneH_; }

    void setMaterialQuality(MaterialQuality q);
    MaterialQuality materialQuality() const { return matQuality_; }
    static MaterialQuality defaultMaterialQualityFor(const gfx::Device& dev, bool mobile);
    const std::string& modelError() const { return modelError_; }   // last model shader failure

    const mat4& viewProj() const { return viewProj_; }
    const float3& cameraPos() const { return camPos_; }
    Lighting lighting;
    LightSystem lights;   // dynamic point lights: add between beginScene() and render()
    uint32_t drawCalls() const { return drawCalls_; }

private:
    struct DrawItem { const Mesh* mesh; mat4 model; float4 color; uint32_t flags; float specular, emissive; };
    struct ModelItem {
        const Model* model; const Primitive* prim; const Material* mat;
        mat4 world; float4 tint; float3 team; float flash; uint32_t flags;
        int32_t joints;     // offset into jointData_ (floats), -1 when not palette-skinned
        bool baked;         // skinned from the baked texture (rows in anim)
        float4 anim;        // baked row A, row B, mix
        float dissolve;
        float4 outline;
        int32_t morph;      // offset into morphData_ (8 weights), -1 = none
        bool inView, inShadow; float depth;
    };
    void initModels();
    void shutdownModels();
    gfx::PipelineHandle modelPipeline(const ModelItem& it, int phase, bool instanced);
    void drawModelBatch(const uint32_t* items, uint32_t count, int phase);
    void renderModels(int phase);   // 0 shadow, 1 opaque, 2 blended, 3 outlines
    void renderReflection();        // water planar reflection (Renderer3DReflect.cpp, lighting agent)
    void computeShadowMatrix(const RTSCamera& cam, float aspect);
    void uploadFrameUniforms(bool forShadow);

    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle litShader_, shadowShader_, colorShader_;
    gfx::PipelineHandle litPipe_, litBlendPipe_, shadowPipe_, linePipe_, triPipe_;
    gfx::BufferHandle lineVB_, triVB_;
    gfx::TextureHandle shadowMap_;
    int shadowSize_ = 0;
    float shadowTexelWorld_ = 0.05f;   // world size of one shadow-map texel
    float shadowExtent_ = 50.0f;       // world size covered by the shadow map
    ShadowQuality quality_ = ShadowQuality::Medium;
    std::vector<DrawItem> items_;
    std::vector<ColorVertex> lines_, tris_;
    std::vector<CustomDraw> custom_, customOpaque_;
    RTSCamera cam_;
    mat4 viewProj_, lightViewProj_;
    float3 camPos_;
    uint32_t drawCalls_ = 0;
    int sceneW_ = 0, sceneH_ = 0;

    // models
    MaterialQuality matQuality_ = MaterialQuality::High;
    std::vector<ModelItem> modelItems_;
    std::vector<float> jointData_, morphData_, instData_;
    const ModelItem* lastItem_ = nullptr;   // state already bound (material, model textures, palette)
    std::vector<mat4> palette_;
    std::vector<uint32_t> order_;
    struct ShaderEntry { gfx::ShaderHandle shader; bool failed = false; };
    std::vector<std::pair<uint32_t, ShaderEntry>> modelShaders_;
    std::vector<std::pair<uint32_t, gfx::PipelineHandle>> modelPipes_;
    gfx::TextureHandle white_, black_, flatNormal_;
    gfx::PipelineHandle currentPipe_;
    std::string modelError_;
};

} // namespace rts::render
