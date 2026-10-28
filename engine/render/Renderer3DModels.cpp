// Renderer3D: models (PBR materials, GPU skinning, alpha modes) — shader permutations, culling,
// sorting and the per-draw uniform blocks. See ModelShaders.h for the uniform layouts.
#include "Renderer3D.h"
#include "ModelShaders.h"
#include "Animation.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rts::render {

namespace {
struct ModelDrawUniforms { float model[16]; float normalMat[16]; float tint[4]; float team[4]; float draw[4]; };
struct MaterialUniforms { float base[4]; float emissive[4]; float pbr[4]; float misc[4]; float uv0[4]; float uv1[4]; };
constexpr size_t kJointFloats = size_t(kMaxPaletteJoints) * 12; // 3 rows of vec4 per joint
static_assert(kJointFloats * 4 <= 4096, "joint palette must fit a 4 KB uniform block");

constexpr uint32_t kKeySkinned = 1, kKeyNormalMap = 2, kKeyAlphaMask = 4, kKeyShadow = 8;

// AABB (in `xf` space) against a clip-space frustum: false only when all corners are outside one plane.
bool boxVisible(const mat4& clip, float3 mn, float3 mx) {
    int out[6] = {0, 0, 0, 0, 0, 0};
    for (int c = 0; c < 8; ++c) {
        float4 p = clip * float4{c & 1 ? mx.x : mn.x, c & 2 ? mx.y : mn.y, c & 4 ? mx.z : mn.z, 1};
        out[0] += p.x < -p.w; out[1] += p.x > p.w;
        out[2] += p.y < -p.w; out[3] += p.y > p.w;
        out[4] += p.z < -p.w; out[5] += p.z > p.w;
    }
    for (int i = 0; i < 6; ++i) if (out[i] == 8) return false;
    return true;
}
} // namespace

MaterialQuality Renderer3D::defaultMaterialQualityFor(const gfx::Device& dev, bool mobile) {
    if (const char* q = std::getenv("RTS_MATERIALS")) { // testing override: low|medium|high
        std::string v = q;
        return v == "low" ? MaterialQuality::Low : v == "medium" ? MaterialQuality::Medium : MaterialQuality::High;
    }
    const std::string& n = dev.info().name;
    if (mobile) {
        for (const char* weak : {"Mali-4", "Mali-T", "Adreno (TM) 3", "Adreno (TM) 4", "Adreno (TM) 50", "PowerVR SGX", "PowerVR Rogue GE"})
            if (n.find(weak) != std::string::npos) return MaterialQuality::Low;
        return MaterialQuality::Medium;
    }
    return MaterialQuality::High;
}

void Renderer3D::initModels() {
    auto tex1x1 = [&](uint32_t rgbaPx, const char* label) {
        gfx::TextureDesc td; td.width = td.height = 1; td.data = &rgbaPx; td.repeat = true; td.label = label;
        return dev_->createTexture(td);
    };
    white_ = tex1x1(0xffffffffu, "white");
    black_ = tex1x1(0xff000000u, "black");
    flatNormal_ = tex1x1(0xffff8080u, "flat normal");
    jointData_.reserve(kJointFloats * 64);
}

void Renderer3D::shutdownModels() {
    for (auto& [k, p] : modelPipes_) dev_->destroyPipeline(p);
    for (auto& [k, s] : modelShaders_) if (s.shader.valid()) dev_->destroyShader(s.shader);
    modelPipes_.clear(); modelShaders_.clear();
    for (auto t : {white_, black_, flatNormal_}) dev_->destroyTexture(t);
}

void Renderer3D::setMaterialQuality(MaterialQuality q) {
    if (q == matQuality_) return;
    matQuality_ = q;
    if (!dev_) return;
    for (auto& [k, p] : modelPipes_) dev_->destroyPipeline(p);
    for (auto& [k, s] : modelShaders_) if (s.shader.valid()) dev_->destroyShader(s.shader);
    modelPipes_.clear(); modelShaders_.clear();
}

gfx::PipelineHandle Renderer3D::modelPipeline(const ModelItem& it, bool shadow) {
    const bool skinned = it.joints >= 0;
    const bool mask = it.mat->alpha == AlphaMode::Mask;
    const bool normalMap = !shadow && it.mat->normalTex >= 0 && matQuality_ != MaterialQuality::Low;
    const bool blend = !shadow && (it.mat->alpha == AlphaMode::Blend || it.tint.w < 0.99f);
    const bool twoSided = it.mat->doubleSided;
    const uint32_t shaderKey = (skinned ? kKeySkinned : 0u) | (normalMap ? kKeyNormalMap : 0u) | (mask ? kKeyAlphaMask : 0u) | (shadow ? kKeyShadow : 0u);
    const uint32_t pipeKey = shaderKey | (blend ? 16u : 0u) | (twoSided ? 32u : 0u);
    for (auto& [k, p] : modelPipes_) if (k == pipeKey) return p;

    ShaderEntry* entry = nullptr;
    for (auto& [k, s] : modelShaders_) if (k == shaderKey) entry = &s;
    if (!entry) {
        std::string defs;
        if (skinned) defs += "#define SKINNED 1\n";
        if (normalMap) defs += "#define NORMALMAP 1\n";
        if (mask) defs += "#define ALPHAMASK 1\n";
        defs += "#define RTS_Q " + std::to_string(int(matQuality_)) + "\n";
        const std::string vs = defs + (shadow ? shaders::kModelShadowVS : shaders::kModelVS);
        const std::string fs = defs + (shadow ? shaders::kModelShadowFS : shaders::kModelFS);
        const std::string msl = defs + (shadow ? shaders::kModelShadowMSL : shaders::kModelMSL);
        gfx::ShaderDesc sd;
        sd.glslVertex = vs.c_str(); sd.glslFragment = fs.c_str(); sd.msl = msl.c_str();
        sd.uniformSlots = shadow ? 4 : 5; sd.textureSlots = shadow ? 7 : 8; sd.label = shadow ? "model shadow" : "model";
        ShaderEntry e;
        e.shader = dev_->createShader(sd);
        if (!e.shader.valid()) {
            e.failed = true;
            modelError_ = "model shader (" + defs + "): " + dev_->lastError();
        }
        modelShaders_.push_back({shaderKey, e});
        entry = &modelShaders_.back().second;
    }
    gfx::PipelineHandle ph;
    if (!entry->failed) {
        gfx::PipelineDesc pd;
        pd.shader = entry->shader;
        pd.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
        pd.attrs[1] = {1, gfx::VertexFormat::Float3, 12};
        pd.attrs[2] = {2, gfx::VertexFormat::Float2, 24};
        pd.attrs[3] = {3, gfx::VertexFormat::Float4, 32};
        pd.attrs[4] = {4, gfx::VertexFormat::UByte4N, 48};
        pd.attrs[5] = {5, gfx::VertexFormat::UByte4, 52};
        pd.attrs[6] = {6, gfx::VertexFormat::UByte4N, 56};
        pd.attrCount = 7;
        pd.stride = sizeof(ModelVertex);
        pd.indexType = gfx::IndexType::UInt32;
        pd.cull = twoSided ? gfx::CullMode::None : gfx::CullMode::Back;
        if (shadow) {
            pd.depthOnly = true;
            pd.depthBiasConstant = 2.0f; pd.depthBiasSlope = 2.5f;
        } else if (blend) {
            pd.blend = gfx::BlendMode::Alpha;
            pd.depthWrite = false;
        }
        pd.label = shadow ? "model shadow" : "model";
        ph = dev_->createPipeline(pd);
        if (!ph.valid()) modelError_ = "model pipeline: " + dev_->lastError();
    }
    modelPipes_.push_back({pipeKey, ph});
    return ph;
}

void Renderer3D::drawModel(const ModelDraw& d) {
    const Model* m = d.model;
    if (!m || !m->vb.valid() || m->nodes.empty()) return;
    // whole-model culling with the bind-pose bounds (padded: animation moves limbs outside them)
    float3 c = (m->boundsMin + m->boundsMax) * 0.5f, e = (m->boundsMax - m->boundsMin) * 0.75f + float3{0.1f, 0.1f, 0.1f};
    const bool inView = boxVisible(viewProj_ * d.world, c - e, c + e);
    const bool inShadow = quality_ != ShadowQuality::Off && !(d.flags & kDrawNoCastShadow) && d.tint.w >= 0.99f &&
                          boxVisible(lightViewProj_ * d.world, c - e, c + e);
    if (!inView && !inShadow) return;
    const std::vector<mat4>& world = d.nodeWorld && d.nodeWorld->size() == m->nodes.size() ? *d.nodeWorld : m->bindWorld;
    const std::vector<Material>& mats = d.materials && d.materials->size() == m->materials.size() ? *d.materials : m->materials;
    for (int n : m->order) {
        const Node& node = m->nodes[size_t(n)];
        if (node.mesh < 0) continue;
        for (const Primitive& p : m->meshes[size_t(node.mesh)].primitives) {
            if (p.indexCount == 0) continue;
            int mi = p.material;
            if (d.variant >= 0 && d.variant < int(p.variantMaterial.size()) && p.variantMaterial[size_t(d.variant)] >= 0) mi = p.variantMaterial[size_t(d.variant)];
            ModelItem it;
            it.model = m; it.prim = &p; it.mat = &mats[size_t(std::clamp(mi, 0, int(mats.size()) - 1))];
            it.tint = d.tint; it.team = d.team; it.flash = d.flash; it.flags = d.flags;
            it.inView = inView; it.inShadow = inShadow && it.mat->castShadow && it.mat->alpha != AlphaMode::Blend;
            it.joints = -1;
            if (node.skin >= 0 && !p.jointPalette.empty()) {
                // joint matrices are in model space: the mesh node transform is ignored (glTF skinning rule)
                paletteMatrices(*m, m->skins[size_t(node.skin)], p, world, palette_);
                it.joints = int32_t(jointData_.size());
                jointData_.resize(jointData_.size() + kJointFloats, 0.0f);
                float* dst = jointData_.data() + it.joints;
                for (size_t j = 0; j < palette_.size() && j < size_t(kMaxPaletteJoints); ++j)
                    for (int r = 0; r < 3; ++r)
                        for (int col = 0; col < 4; ++col) dst[j * 12 + size_t(r) * 4 + size_t(col)] = palette_[j].at(r, col);
                it.world = d.world;
            } else {
                it.world = d.world * world[size_t(n)];
            }
            float3 wc = it.world.transformPoint((p.boundsMin + p.boundsMax) * 0.5f);
            it.depth = length(wc - camPos_);
            modelItems_.push_back(it);
        }
    }
}

void Renderer3D::drawModelItem(const ModelItem& it, bool shadow, const ModelItem*& lastMat) {
    gfx::PipelineHandle ph = modelPipeline(it, shadow);
    if (!ph.valid()) return;
    if (!(ph == currentPipe_)) {
        dev_->setPipeline(ph); currentPipe_ = ph;
        if (!shadow) lights.bind(4, 7);   // U4 / tex7: dynamic lights
    }
    ModelDrawUniforms u{};
    std::memcpy(u.model, it.world.m, sizeof u.model);
    mat4 nm = it.world.normalMatrix();
    std::memcpy(u.normalMat, nm.m, sizeof u.normalMat);
    u.tint[0] = it.tint.x; u.tint[1] = it.tint.y; u.tint[2] = it.tint.z; u.tint[3] = it.tint.w;
    u.team[0] = it.team.x; u.team[1] = it.team.y; u.team[2] = it.team.z; u.team[3] = it.flash;
    u.draw[0] = (it.mat->receiveShadow && !(it.flags & kDrawNoReceiveShadow)) ? 1.0f : 0.0f;
    dev_->setUniforms(0, &u, sizeof u);
    const Material& mt = *it.mat;
    if (!lastMat || lastMat->mat != it.mat || lastMat->model != it.model) {
        MaterialUniforms mu{};
        mu.base[0] = mt.baseColor.x; mu.base[1] = mt.baseColor.y; mu.base[2] = mt.baseColor.z; mu.base[3] = mt.baseColor.w;
        mu.emissive[0] = mt.emissive.x * mt.emissiveStrength; mu.emissive[1] = mt.emissive.y * mt.emissiveStrength; mu.emissive[2] = mt.emissive.z * mt.emissiveStrength;
        mu.pbr[0] = mt.metallic; mu.pbr[1] = mt.roughness; mu.pbr[2] = mt.normalScale; mu.pbr[3] = mt.occlusionStrength;
        mu.misc[0] = mt.alpha == AlphaMode::Mask ? mt.alphaCutoff : 0.0f;
        mu.misc[1] = float(int(mt.team)); mu.misc[2] = mt.teamStrength; mu.misc[3] = mt.vertexColors ? 1.0f : 0.0f;
        mu.uv0[0] = mt.uvOffset.x; mu.uv0[1] = mt.uvOffset.y; mu.uv0[2] = mt.uvScale.x; mu.uv0[3] = mt.uvScale.y;
        mu.uv1[0] = mt.uvRotation; mu.uv1[1] = mt.shading == ShadingModel::Unlit ? 1.0f : 0.0f;
        dev_->setUniforms(2, &mu, sizeof mu);
        auto tex = [&](int idx, gfx::TextureHandle def) {
            return idx >= 0 && idx < int(it.model->images.size()) && it.model->images[size_t(idx)].tex.valid() ? it.model->images[size_t(idx)].tex : def;
        };
        dev_->setTexture(1, tex(mt.baseTex, white_));
        if (!shadow) {
            dev_->setTexture(2, tex(mt.mrTex, white_));
            dev_->setTexture(3, tex(mt.normalTex, flatNormal_));
            dev_->setTexture(4, tex(mt.occlusionTex, white_));
            dev_->setTexture(5, tex(mt.emissiveTex, white_));
            dev_->setTexture(6, tex(mt.teamTex, black_));
        }
        lastMat = &it;
    }
    if (it.joints >= 0) dev_->setUniforms(3, jointData_.data() + it.joints, kJointFloats * sizeof(float));
    dev_->setVertexBuffer(it.model->vb);
    dev_->setIndexBuffer(it.model->ib);
    dev_->drawIndexed(it.prim->indexCount, it.prim->firstIndex);
    ++drawCalls_;
}

void Renderer3D::renderModels(int phase) {
    if (modelItems_.empty()) return;
    order_.clear();
    for (uint32_t i = 0; i < modelItems_.size(); ++i) {
        const ModelItem& it = modelItems_[i];
        const bool blended = it.mat->alpha == AlphaMode::Blend || it.tint.w < 0.99f;
        if (phase == 0 ? it.inShadow : it.inView && (phase == 2) == blended) order_.push_back(i);
    }
    if (order_.empty()) return;
    if (phase == 2) { // back to front
        std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) { return modelItems_[a].depth > modelItems_[b].depth; });
    } else {          // group by state: skinning, material
        std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
            const ModelItem &x = modelItems_[a], &y = modelItems_[b];
            if ((x.joints >= 0) != (y.joints >= 0)) return x.joints < y.joints;
            if (x.mat != y.mat) return x.mat < y.mat;
            return a < b;
        });
    }
    currentPipe_ = {};
    const ModelItem* lastMat = nullptr;
    for (uint32_t i : order_) drawModelItem(modelItems_[i], phase == 0, lastMat);
}

} // namespace rts::render
