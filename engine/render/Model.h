// Models: node hierarchy, meshes, PBR materials, textures, skins and animation clips.
//
// Sources: glTF 2.0 (.gltf / .glb), Wavefront OBJ (+ .mtl), FBX (.fbx), and procedural meshes built with
// MeshBuilder (the built-in shapes). Every source ends up in the same Model structure, so the
// renderer, the animation system and the game view treat them identically.
//
// Conventions: Y up, right-handed, metres = map cells. glTF assets face +Z; the game view rotates
// them to the engine's +X forward (see RtsView "forward").
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../gfx/Gfx.h"
#include "Math3D.h"

namespace rts::render {

class MeshBuilder;

enum class AlphaMode : uint8_t { Opaque, Mask, Blend };
enum class ShadingModel : uint8_t { Pbr, Unlit };
// Where the team (player) colour goes:
//   Tint        whole material multiplied by the team colour (procedural shapes, simple models)
//   Mask        team mask texture, red channel 1 = team colour
//   BaseAlpha   base colour texture alpha, 0 = team colour (Warcraft III style); the material stays opaque
//   VertexAlpha vertex colour alpha, 0 = team colour
enum class TeamMode : uint8_t { None, Tint, Mask, BaseAlpha, VertexAlpha };

struct Material {
    std::string name;
    ShadingModel shading = ShadingModel::Pbr;
    float4 baseColor{1, 1, 1, 1};          // linear
    float metallic = 0, roughness = 0.8f;
    float normalScale = 1, occlusionStrength = 1;
    float3 emissive{0, 0, 0};              // linear, multiplied by emissiveStrength
    float emissiveStrength = 1;
    AlphaMode alpha = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool vertexColors = true;              // multiply by COLOR_0
    bool castShadow = true, receiveShadow = true;
    TeamMode team = TeamMode::None;
    float teamStrength = 1;
    // texture image indices into Model::images (-1 = none)
    int baseTex = -1, mrTex = -1, normalTex = -1, occlusionTex = -1, emissiveTex = -1, teamTex = -1;
    // KHR_texture_transform of the base colour texture, applied to every texture of the material
    float2 uvOffset{0, 0}, uvScale{1, 1};
    float uvRotation = 0;
};

struct Image {
    std::string name;               // file name or "#index" for embedded images
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;      // CPU copy, released after upload
    bool repeat = true, nearest = false;
    gfx::TextureHandle tex;
};

// 60 bytes, one interleaved stream for every model (unskinned meshes leave joints/weights zero).
struct ModelVertex {
    float3 pos;
    float3 nrm;
    float2 uv;
    float4 tangent{1, 0, 0, 1};     // xyz tangent, w handedness
    uint32_t color = 0xffffffffu;   // RGBA8, linear
    uint8_t joints[4] = {0, 0, 0, 0};   // indices into Primitive::jointPalette
    uint8_t weights[4] = {0, 0, 0, 0};  // normalized
};
static_assert(sizeof(ModelVertex) == 60, "ModelVertex layout is shared with the shaders");

constexpr int kMaxPaletteJoints = 80;  // joint matrices per draw (3 x vec4 each, fits a 4 KB uniform block)

struct Primitive {
    uint32_t firstIndex = 0, indexCount = 0;
    int material = 0;
    std::vector<uint16_t> jointPalette;  // palette slot -> joint index of the node's skin (empty: not skinned)
    std::vector<int> variantMaterial;    // KHR_materials_variants: variant index -> material (-1 = default)
    float3 boundsMin, boundsMax;         // mesh space (bind pose)
};

struct MeshDef {
    std::string name;
    std::vector<Primitive> primitives;
};

struct Node {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    float3 t{0, 0, 0};
    quat r;
    float3 s{1, 1, 1};
    int mesh = -1, skin = -1;
};

struct Skin {
    std::string name;
    std::vector<int> joints;          // node indices
    std::vector<mat4> inverseBind;
};

struct AnimChannel {
    enum Path : uint8_t { Translation, Rotation, Scale };
    enum Interp : uint8_t { Step, Linear, CubicSpline };
    int node = -1;
    Path path = Translation;
    Interp interp = Linear;
    std::vector<float> times;
    std::vector<float> values;        // 3 or 4 floats per key; cubic: in-tangent, value, out-tangent per key
};

struct AnimClip {
    std::string name;
    float duration = 0;
    std::vector<AnimChannel> channels;
};

struct Model {
    std::string path;
    std::vector<Node> nodes;
    std::vector<int> order;            // node indices, parents before children (finalize())
    std::vector<mat4> bindWorld;       // node world matrices of the bind pose (finalize())
    std::vector<MeshDef> meshes;
    std::vector<Material> materials;   // never empty after loading (a default material is added)
    std::vector<Image> images;
    std::vector<Skin> skins;
    std::vector<AnimClip> clips;
    std::vector<std::string> variants; // KHR_materials_variants names
    std::vector<ModelVertex> vertices; // CPU copy, released after upload
    std::vector<uint32_t> indices;
    gfx::BufferHandle vb, ib;
    float3 boundsMin, boundsMax;       // model space, bind pose (all meshes under their nodes)

    int findNode(const std::string& name) const;
    int findClip(const std::string& name) const;     // exact, then case-insensitive
    int findMaterial(const std::string& name) const;
    int findVariant(const std::string& name) const;
    bool skinned() const { return !skins.empty(); }
    // Children lists, evaluation order, primitive and model bounds. Loaders call it.
    void finalize();
};

using FileReader = std::function<bool(const std::string& path, std::string& out)>;

// Loads .gltf / .glb / .obj / .fbx by extension. Throws std::runtime_error with a readable message
// ("models/knight.glb: accessor 3 out of range"). Unsupported optional features are reported in
// `warnings` and skipped (morph targets, compressed meshes fall back to an error).
std::unique_ptr<Model> loadModel(const std::string& path, const FileReader& read, std::vector<std::string>* warnings = nullptr);
std::unique_ptr<Model> loadGltf(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings);
std::unique_ptr<Model> loadObj(const std::string& path, const std::string& text, const FileReader& read, std::vector<std::string>* warnings);
// FBX (binary / ASCII) through ufbx; converted to Y up, right-handed, metres.
std::unique_ptr<Model> loadFbx(const std::string& path, const std::string& bytes, const FileReader& read, std::vector<std::string>* warnings);
// PNG / JPEG / TGA from memory into RGBA8. Returns false on failure.
bool decodeImage(const std::string& bytes, Image& out);

// Builds a model from named procedural parts (one node per part). Colors of the builder are
// treated as sRGB and converted to linear vertex colors.
struct ModelPart { std::string name; const MeshBuilder* mesh; Material material; mat4 xf; };
std::unique_ptr<Model> modelFromParts(const std::string& name, const std::vector<ModelPart>& parts);

// Missing tangents for normal mapping (per-vertex, from UVs).
void generateTangents(std::vector<ModelVertex>& v, const std::vector<uint32_t>& idx, uint32_t first, uint32_t count);

// GPU resources. Textures get mipmaps. CPU copies are released unless keepCpu.
bool uploadModel(gfx::Device& dev, Model& m, bool keepCpu = false);
int addImage(gfx::Device& dev, Model& m, Image img);   // extra texture for material overrides; returns index
void releaseModel(gfx::Device& dev, Model& m);

} // namespace rts::render
