#include "Lights.h"
#include "LightShaders.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rts::render {

LightQuality LightSystem::defaultQualityFor(const gfx::Device& dev, bool mobile) {
    if (const char* q = std::getenv("RTS_LIGHTS")) { // testing override: off|low|medium|high
        std::string v = q;
        return v == "off" ? LightQuality::Off : v == "low" ? LightQuality::Low : v == "high" ? LightQuality::High : LightQuality::Medium;
    }
    const std::string& n = dev.info().name;
    if (n.find("llvmpipe") != std::string::npos || n.find("SwiftShader") != std::string::npos) return LightQuality::Low;
    if (mobile) {
        for (const char* weak : {"Mali-4", "Mali-T", "Adreno (TM) 3", "Adreno (TM) 4", "Adreno (TM) 50", "PowerVR SGX", "PowerVR Rogue GE"})
            if (n.find(weak) != std::string::npos) return LightQuality::Low;
        return LightQuality::Medium;
    }
    return LightQuality::High;
}

bool LightSystem::init(gfx::Device& dev) {
    dev_ = &dev;
    gfx::ShaderDesc sd;
    sd.glslVertex = shaders::kLightMapVS; sd.glslFragment = shaders::kLightMapFS; sd.msl = shaders::kLightMapMSL;
    sd.uniformSlots = 0; sd.label = "lightmap";
    shader_ = dev.createShader(sd);
    if (!shader_.valid()) return false;
    gfx::PipelineDesc pd;
    pd.shader = shader_;
    pd.attrs[0] = {0, gfx::VertexFormat::Float2, 0};
    pd.attrs[1] = {1, gfx::VertexFormat::Float2, 8};
    pd.attrs[2] = {2, gfx::VertexFormat::Float4, 16};
    pd.attrCount = 3; pd.stride = sizeof(MapVertex);
    pd.indexType = gfx::IndexType::None;
    pd.cull = gfx::CullMode::None;
    pd.blend = gfx::BlendMode::Additive;
    pd.depthTest = false; pd.depthWrite = false;
    pd.label = "lightmap";
    pipe_ = dev.createPipeline(pd);
    gfx::BufferDesc bd; bd.usage = gfx::Usage::Dynamic; bd.size = kMaxLights * 6 * sizeof(MapVertex); bd.label = "lightmap";
    vb_ = dev.createBuffer(bd);
    const uint8_t zero[4] = {0, 0, 0, 0};
    gfx::TextureDesc td; td.width = td.height = 1; td.data = zero; td.label = "nolight";
    black_ = dev.createTexture(td);

    gfx::ShaderDesc hs;
    hs.glslVertex = shaders::kHaloVS; hs.glslFragment = shaders::kHaloFS; hs.msl = shaders::kHaloMSL;
    hs.uniformSlots = 1; hs.label = "halo";
    haloShader_ = dev.createShader(hs);
    if (!haloShader_.valid()) return false;
    gfx::PipelineDesc hp;
    hp.shader = haloShader_;
    hp.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
    hp.attrs[1] = {1, gfx::VertexFormat::Float2, 12};
    hp.attrs[2] = {2, gfx::VertexFormat::Float4, 20};
    hp.attrCount = 3; hp.stride = sizeof(HaloVertex);
    hp.indexType = gfx::IndexType::None;
    hp.cull = gfx::CullMode::None;
    hp.blend = gfx::BlendMode::Additive;
    hp.depthTest = true; hp.depthWrite = false;
    hp.label = "halo";
    haloPipe_ = dev.createPipeline(hp);
    gfx::BufferDesc hb; hb.usage = gfx::Usage::Dynamic; hb.size = 256 * 6 * sizeof(HaloVertex); hb.label = "halos";
    haloVB_ = dev.createBuffer(hb);
    setQuality(quality_);
    return pipe_.valid() && vb_.valid() && black_.valid() && haloPipe_.valid() && haloVB_.valid();
}

void LightSystem::createMap(int size) {
    if (size == mapSize_ && (size == 0 || map_.valid())) return;
    if (map_.valid()) dev_->destroyTexture(map_);
    if (mapDepth_.valid()) dev_->destroyTexture(mapDepth_);
    map_ = mapDepth_ = {};
    mapSize_ = size;
    mapReady_ = false;
    if (size == 0) return;
    gfx::TextureDesc td;
    td.width = td.height = size;
    td.renderTarget = true; td.filter = gfx::Filter::Linear; td.label = "lightmap";
    map_ = dev_->createTexture(td);
    // Metal pipelines always declare a depth attachment, so the pass gets one too (never written).
    gfx::TextureDesc dd = td;
    dd.format = gfx::PixelFormat::Depth; dd.filter = gfx::Filter::Nearest; dd.label = "lightmap depth";
    mapDepth_ = dev_->createTexture(dd);
}

void LightSystem::setQuality(LightQuality q) {
    quality_ = q;
    if (!dev_) return;
    // The map is low frequency (lights are soft blobs), so a small target is enough; linear
    // filtering hides the texels.
    createMap(q == LightQuality::Off ? 0 : q == LightQuality::Low ? 128 : 256);
}

void LightSystem::shutdown() {
    if (!dev_) return;
    createMap(0);
    dev_->destroyTexture(black_);
    dev_->destroyBuffer(vb_);
    dev_->destroyBuffer(haloVB_);
    dev_->destroyPipeline(pipe_);
    dev_->destroyPipeline(haloPipe_);
    dev_->destroyShader(shader_);
    dev_->destroyShader(haloShader_);
    dev_ = nullptr;
}

void LightSystem::begin(const RTSCamera& cam, float aspect) {
    lights_.clear();
    focus_ = cam.target;
    {
        const mat4 v = cam.view();   // rows 0/1 of the view matrix: camera right / up in world space
        camRight_ = {v.at(0, 0), v.at(0, 1), v.at(0, 2)};
        camUp_ = {v.at(1, 0), v.at(1, 1), v.at(1, 2)};
    }
    // Cover the ground the camera sees (same idea as the shadow frustum fit).
    const float w = 1000.0f, h = w / std::max(aspect, 0.1f);
    const float sx[4] = {0, w, 0, w}, sy[4] = {0, 0, h, h};
    float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
    for (int i = 0; i < 4; ++i) {
        float3 g;
        if (!cam.screenToGround(sx[i], sy[i], w, h, g) || length(g - cam.target) > cam.distance * 3.0f) {
            float3 o, d;
            cam.screenRay(sx[i], sy[i], w, h, o, d);
            g = cam.target + normalize(float3{d.x, 0, d.z}) * (cam.distance * 1.6f);
        }
        minX = std::min(minX, g.x); maxX = std::max(maxX, g.x);
        minZ = std::min(minZ, g.z); maxZ = std::max(maxZ, g.z);
    }
    extent_ = std::clamp(std::max(maxX - minX, maxZ - minZ) + 6.0f, 16.0f, 256.0f);
    // texel snapping keeps the blobs from shimmering while the camera pans
    const float texel = extent_ / float(std::max(mapSize_, 16));
    originX_ = std::floor(((minX + maxX) * 0.5f - extent_ * 0.5f) / texel) * texel;
    originZ_ = std::floor(((minZ + maxZ) * 0.5f - extent_ * 0.5f) / texel) * texel;
}

void LightSystem::add(const PointLight& l) {
    if (quality_ == LightQuality::Off || lights_.size() >= kMaxLights || l.radius <= 0.01f || l.intensity <= 0.0f) return;
    lights_.push_back(l);
}

void LightSystem::pickPixelLights() {
    perPixel_.assign(lights_.size(), 0);
    int n = 0;
    if (quality_ == LightQuality::High && !lights_.empty()) {
        // Brightest, biggest lights near the screen centre get the precise path.
        std::vector<std::pair<float, uint32_t>> score;
        score.reserve(lights_.size());
        for (uint32_t i = 0; i < lights_.size(); ++i) {
            const PointLight& l = lights_[i];
            float dx = l.pos.x - focus_.x, dz = l.pos.z - focus_.z;
            float lum = l.color.x * 0.3f + l.color.y * 0.55f + l.color.z * 0.15f;
            score.push_back({lum * l.intensity * l.radius * (1.0f + l.priority) / (1.0f + std::sqrt(dx * dx + dz * dz) * 0.08f), i});
        }
        n = std::min<int>(kMaxPixelLights, int(score.size()));
        std::partial_sort(score.begin(), score.begin() + n, score.end(), [](auto& a, auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
        for (int k = 0; k < n; ++k) {
            const PointLight& l = lights_[score[size_t(k)].second];
            perPixel_[score[size_t(k)].second] = 1;
            u_.pos[k][0] = l.pos.x; u_.pos[k][1] = l.pos.y; u_.pos[k][2] = l.pos.z; u_.pos[k][3] = l.radius;
            u_.col[k][0] = l.color.x * l.intensity; u_.col[k][1] = l.color.y * l.intensity; u_.col[k][2] = l.color.z * l.intensity; u_.col[k][3] = 0;
        }
    }
    u_.info[0] = float(n);
}

void LightSystem::renderMap() {
    pickPixelLights();
    if (quality_ == LightQuality::Off || !map_.valid() || !pipe_.valid()) { mapReady_ = false; return; }
    // Low: refresh every other frame; the old map stays valid because it is world-aligned.
    if (quality_ == LightQuality::Low && (++mapFrame_ & 1u) && mapReady_) return;
    verts_.clear();
    const float inv = 2.0f / extent_, k = 1.0f / kMapRange;
    for (size_t i = 0; i < lights_.size(); ++i) {
        if (perPixel_[i]) continue;
        const PointLight& l = lights_[i];
        const float cx = (l.pos.x - originX_) * inv - 1.0f, cy = (l.pos.z - originZ_) * inv - 1.0f, r = l.radius * inv;
        if (cx + r < -1 || cx - r > 1 || cy + r < -1 || cy - r > 1) continue;
        const float cr = std::min(l.color.x * l.intensity * k, 1.0f), cg = std::min(l.color.y * l.intensity * k, 1.0f),
                    cb = std::min(l.color.z * l.intensity * k, 1.0f);
        const MapVertex a{cx - r, cy - r, -1, -1, cr, cg, cb, 1}, b{cx + r, cy - r, 1, -1, cr, cg, cb, 1},
                        c{cx + r, cy + r, 1, 1, cr, cg, cb, 1}, d{cx - r, cy + r, -1, 1, cr, cg, cb, 1};
        verts_.insert(verts_.end(), {a, b, c, a, c, d});
    }
    mapReady_ = true;
    mapX_ = originX_; mapZ_ = originZ_; mapExtent_ = extent_;
    mapHasLights_ = !verts_.empty();
    if (!mapHasLights_) return; // nothing in the map: no pass, the shaders skip the fetch
    gfx::PassDesc p;
    p.colorTarget = map_;
    p.depthTarget = mapDepth_;
    p.clearColor[0] = p.clearColor[1] = p.clearColor[2] = p.clearColor[3] = 0.0f;
    p.clearColorEnabled = true;
    p.clearDepthEnabled = true;
    dev_->beginPass(p);
    dev_->setViewport({0, 0, mapSize_, mapSize_});
    dev_->setPipeline(pipe_);
    dev_->updateBuffer(vb_, verts_.data(), verts_.size() * sizeof(MapVertex));
    dev_->setVertexBuffer(vb_);
    dev_->draw(uint32_t(verts_.size()));
    dev_->endPass();
}

void LightSystem::bind(int uniformSlot, int textureSlot) {
    if (!dev_) return;
    const bool useMap = mapReady_ && mapHasLights_ && quality_ != LightQuality::Off;
    u_.map[0] = mapX_; u_.map[1] = mapZ_; u_.map[2] = 1.0f / mapExtent_; u_.map[3] = useMap ? 1.0f : 0.0f;
    u_.info[1] = kMapRange;
    u_.info[2] = dev_->info().renderTargetFlipY ? 1.0f : 0.0f;
    u_.info[3] = nightGlow_;
    if (quality_ == LightQuality::Off) u_.info[0] = 0;
    dev_->setUniforms(uniformSlot, &u_, sizeof u_);
    dev_->setTexture(textureSlot, useMap ? map_ : black_);
}

void LightSystem::drawHalos(const mat4& viewProj) {
    if (!dev_ || quality_ == LightQuality::Off || !haloPipe_.valid()) return;
    halos_.clear();
    for (const PointLight& l : lights_) {
        if (l.halo <= 0 || l.haloAlpha <= 0) continue;
        const float3 r = camRight_ * l.halo, u = camUp_ * l.halo;
        // colour in display space: keep the hue, cap the brightness
        const float m = std::max({l.color.x, l.color.y, l.color.z, 1e-3f});
        const float cr = l.color.x / m, cg = l.color.y / m, cb = l.color.z / m, a = std::min(l.haloAlpha, 1.5f);
        const float3 p0 = l.pos - r - u, p1 = l.pos + r - u, p2 = l.pos + r + u, p3 = l.pos - r + u;
        const HaloVertex v0{p0.x, p0.y, p0.z, -1, -1, cr, cg, cb, a}, v1{p1.x, p1.y, p1.z, 1, -1, cr, cg, cb, a},
                         v2{p2.x, p2.y, p2.z, 1, 1, cr, cg, cb, a}, v3{p3.x, p3.y, p3.z, -1, 1, cr, cg, cb, a};
        halos_.insert(halos_.end(), {v0, v1, v2, v0, v2, v3});
    }
    if (halos_.empty()) return;
    dev_->setPipeline(haloPipe_);
    dev_->setUniforms(0, viewProj.m, sizeof viewProj.m);
    dev_->updateBuffer(haloVB_, halos_.data(), halos_.size() * sizeof(HaloVertex));
    dev_->setVertexBuffer(haloVB_);
    dev_->draw(uint32_t(halos_.size()));
}

} // namespace rts::render
