#include "Lights.h"
#include "LightShaders.h"
#include <algorithm>
#include <chrono>
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
    pd.attrs[3] = {3, gfx::VertexFormat::Float4, 32};
    pd.attrs[4] = {4, gfx::VertexFormat::Float4, 48};
    pd.attrCount = 5; pd.stride = sizeof(MapVertex);
    pd.indexType = gfx::IndexType::None;
    pd.cull = gfx::CullMode::None;
    pd.blend = gfx::BlendMode::Additive;
    pd.depthTest = false; pd.depthWrite = false;
    pd.label = "lightmap";
    pipe_ = dev.createPipeline(pd);
    gfx::BufferDesc bd; bd.usage = gfx::Usage::Dynamic; bd.size = kMaxLights * 6 * sizeof(MapVertex) * 2; bd.label = "lightmap";
    vb_ = dev.createBuffer(bd);
    vbCap_ = bd.size;
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
    gfx::BufferDesc hb; hb.usage = gfx::Usage::Dynamic; hb.size = 2048 * 6 * sizeof(HaloVertex); hb.label = "halos";
    haloVB_ = dev.createBuffer(hb);
    smallCap_ = hb.size;
    gfx::ShaderDesc ds = hs;
    ds.glslFragment = shaders::kDecalFS; ds.msl = shaders::kDecalMSL; ds.label = "contact shadow";
    decalShader_ = dev.createShader(ds);
    if (!decalShader_.valid()) return false;
    gfx::PipelineDesc dp = hp;
    dp.shader = decalShader_;
    dp.blend = gfx::BlendMode::Alpha;
    dp.label = "contact shadow";
    decalPipe_ = dev.createPipeline(dp);
    gfx::ShaderDesc bs = hs;
    bs.glslFragment = shaders::kBeamFS; bs.msl = shaders::kBeamMSL; bs.label = "light shaft";
    beamShader_ = dev.createShader(bs);
    if (!beamShader_.valid()) return false;
    gfx::PipelineDesc bp = hp;
    bp.shader = beamShader_;
    bp.label = "light shaft";
    beamPipe_ = dev.createPipeline(bp);
    gfx::BufferDesc bb = hb; bb.label = "light shafts";
    beamVB_ = dev.createBuffer(bb);
    gfx::BufferDesc db = hb; db.label = "contact shadows";
    decalVB_ = dev.createBuffer(db);
    setQuality(quality_);
    return pipe_.valid() && vb_.valid() && black_.valid() && haloPipe_.valid() && haloVB_.valid() && decalPipe_.valid() && decalVB_.valid();
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

size_t LightSystem::stream(gfx::BufferHandle buf, size_t& cursor, size_t capacity, const void* data, size_t bytes) {
    // Wrapping is safe as long as one frame uploads less than the capacity: by the time the ring
    // comes back to an offset, that frame's copy is no longer in flight. A single upload larger
    // than the ring starts at 0 and grows the buffer (updateBuffer reallocates; rare, at startup).
    cursor = (cursor + 15) & ~size_t(15);
    if (cursor + bytes > capacity) cursor = 0;
    const size_t at = cursor;
    dev_->updateBuffer(buf, data, bytes, at);
    cursor += bytes;
    return at;
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
    if (reflColor_.valid()) dev_->destroyTexture(reflColor_);
    if (reflDepth_.valid()) dev_->destroyTexture(reflDepth_);
    reflColor_ = reflDepth_ = {};
    dev_->destroyTexture(black_);
    dev_->destroyBuffer(vb_);
    dev_->destroyBuffer(haloVB_);
    dev_->destroyBuffer(decalVB_);
    dev_->destroyPipeline(decalPipe_);
    dev_->destroyShader(decalShader_);
    dev_->destroyPipeline(beamPipe_);
    dev_->destroyBuffer(beamVB_);
    dev_->destroyShader(beamShader_);
    dev_->destroyPipeline(pipe_);
    dev_->destroyPipeline(haloPipe_);
    dev_->destroyShader(shader_);
    dev_->destroyShader(haloShader_);
    dev_ = nullptr;
}

void LightSystem::begin(const RTSCamera& cam, float aspect) {
    lights_.clear();
    decals_.clear();
    reflReady_ = false;
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
    // margin: lights (and long spot beams) just off screen still reach into view; the soft edge stays off screen
    extent_ = std::clamp(std::max(maxX - minX, maxZ - minZ) * 1.15f + 12.0f, 16.0f, 256.0f);
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
            const float spotK = l.spot() ? 1.5f : 1.0f;   // beams show their cone best on the precise path
            const float keep = l.key && std::binary_search(prevKeys_.begin(), prevKeys_.end(), l.key) ? 1.6f : 1.0f;
            const float in = l.rank >= 0 ? l.rank : l.intensity;
            score.push_back({lum * in * l.radius * spotK * keep * (1.0f + l.priority) / (1.0f + std::sqrt(dx * dx + dz * dz) * 0.08f), i});
        }
        n = std::min<int>(kMaxPixelLights, int(score.size()));
        std::partial_sort(score.begin(), score.begin() + n, score.end(), [](auto& a, auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
        prevKeys_.clear();
        for (int k = 0; k < n; ++k) {
            const PointLight& l = lights_[score[size_t(k)].second];
            if (l.key) prevKeys_.push_back(l.key);
            perPixel_[score[size_t(k)].second] = 1;
            u_.pos[k][0] = l.pos.x; u_.pos[k][1] = l.pos.y; u_.pos[k][2] = l.pos.z; u_.pos[k][3] = l.radius;
            u_.col[k][0] = l.color.x * l.intensity; u_.col[k][1] = l.color.y * l.intensity; u_.col[k][2] = l.color.z * l.intensity; u_.col[k][3] = l.cosInner;
            u_.spot[k][0] = l.dir.x; u_.spot[k][1] = l.dir.y; u_.spot[k][2] = l.dir.z; u_.spot[k][3] = l.spot() ? l.cosOuter : -2.0f;
        }
    }
    std::sort(prevKeys_.begin(), prevKeys_.end());
    u_.info[0] = float(n);
}

void LightSystem::renderMap() {
    const auto t0 = std::chrono::steady_clock::now();
    stats_ = LightStats{};
    stats_.mapSize = mapSize_;
    struct Done { LightSystem& s; std::chrono::steady_clock::time_point t0;
        ~Done() { s.stats_.mapMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count(); } } done{*this, t0};
    pickPixelLights();
    stats_.lights = int(lights_.size());
    stats_.perPixel = int(u_.info[0]);
    if (quality_ == LightQuality::Off || !map_.valid() || !pipe_.valid()) { mapReady_ = false; return; }
    // Low: refresh every other frame; the old map stays valid because it is world-aligned.
    // Only when the map already covers this view's area: two views a frame (editor) differ.
    const float texel = extent_ / float(std::max(mapSize_, 16));
    const bool sameArea = std::fabs(originX_ - mapX_) < texel * 0.5f && std::fabs(originZ_ - mapZ_) < texel * 0.5f &&
                          std::fabs(extent_ - mapExtent_) < texel * 0.5f;
    if (quality_ == LightQuality::Low && (++mapFrame_ & 1u) && mapReady_ && sameArea) return;
    verts_.clear();
    const float inv = 2.0f / extent_, k = 1.0f / kMapRange;
    for (size_t i = 0; i < lights_.size(); ++i) {
        const PointLight& l = lights_[i];   // every light, per-pixel ones too (they only add detail)
        const float cx = (l.pos.x - originX_) * inv - 1.0f, cy = (l.pos.z - originZ_) * inv - 1.0f, r = l.radius * inv;
        if (cx + r < -1 || cx - r > 1 || cy + r < -1 || cy - r > 1) continue;
        const float cr = std::min(l.color.x * l.intensity * k, 1.0f), cg = std::min(l.color.y * l.intensity * k, 1.0f),
                    cb = std::min(l.color.z * l.intensity * k, 1.0f);
        const float h = std::max(l.pos.y - (l.groundY > -1e8f ? l.groundY : l.pos.y - 1.0f), 0.1f);
        const float co = l.spot() ? l.cosOuter : -2.0f;
        const MapVertex a{cx - r, cy - r, -1, -1, cr, cg, cb, 1, l.dir.x, l.dir.y, l.dir.z, h, co, l.cosInner, l.radius, 0},
                        b{cx + r, cy - r, 1, -1, cr, cg, cb, 1, l.dir.x, l.dir.y, l.dir.z, h, co, l.cosInner, l.radius, 0},
                        c{cx + r, cy + r, 1, 1, cr, cg, cb, 1, l.dir.x, l.dir.y, l.dir.z, h, co, l.cosInner, l.radius, 0},
                        d{cx - r, cy + r, -1, 1, cr, cg, cb, 1, l.dir.x, l.dir.y, l.dir.z, h, co, l.cosInner, l.radius, 0};
        verts_.insert(verts_.end(), {a, b, c, a, c, d});
    }
    mapReady_ = true;
    mapX_ = originX_; mapZ_ = originZ_; mapExtent_ = extent_;
    mapHasLights_ = !verts_.empty();
    stats_.mapQuads = int(verts_.size() / 6);
    stats_.mapDrawn = mapHasLights_;
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
    dev_->setVertexBuffer(vb_, stream(vb_, vbCursor_, vbCap_, verts_.data(), verts_.size() * sizeof(MapVertex)));
    dev_->draw(uint32_t(verts_.size()));
    dev_->endPass();
}

void LightSystem::setClouds(float coverage, float strength, float scale, float offsetX, float offsetZ) {
    const float k = 0.11f * std::max(scale, 0.05f);   // ~1/9 rad per unit: clouds 20-40 units across
    u_.cloud[0] = offsetX * k; u_.cloud[1] = offsetZ * k; u_.cloud[2] = k;
    u_.cloud[3] = std::clamp(coverage, 0.0f, 1.0f);
    u_.sky[0] = coverage > 0.0f ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
}

bool LightSystem::beginReflection(int viewW, int viewH, float3 clear) {
    if (!reflectionsWanted() || viewW <= 0 || viewH <= 0) return false;
    const int w = std::max(viewW / 2, 16), h = std::max(viewH / 2, 16);
    if (w != reflW_ || h != reflH_ || !reflColor_.valid()) {
        if (reflColor_.valid()) dev_->destroyTexture(reflColor_);
        if (reflDepth_.valid()) dev_->destroyTexture(reflDepth_);
        gfx::TextureDesc td;
        td.width = w; td.height = h; td.renderTarget = true; td.filter = gfx::Filter::Linear; td.label = "water reflection";
        reflColor_ = dev_->createTexture(td);
        gfx::TextureDesc dd = td;
        dd.format = gfx::PixelFormat::Depth; dd.filter = gfx::Filter::Nearest; dd.label = "water reflection depth";
        reflDepth_ = dev_->createTexture(dd);
        reflW_ = w; reflH_ = h;
    }
    if (!reflColor_.valid() || !reflDepth_.valid()) return false;
    viewW_ = float(viewW); viewH_ = float(viewH);
    gfx::PassDesc p;
    p.colorTarget = reflColor_;
    p.depthTarget = reflDepth_;
    p.clearColor[0] = clear.x; p.clearColor[1] = clear.y; p.clearColor[2] = clear.z; p.clearColor[3] = 1;
    dev_->beginPass(p);
    dev_->setViewport({0, 0, w, h});
    return true;
}

void LightSystem::endReflection() {
    dev_->endPass();
    reflReady_ = true;
    stats_.reflection = true; stats_.reflW = reflW_; stats_.reflH = reflH_;
}

void LightSystem::bind(int uniformSlot, int textureSlot, int reflectionSlot) {
    if (!dev_) return;
    const bool useMap = mapReady_ && mapHasLights_ && quality_ != LightQuality::Off;
    u_.map[0] = mapX_; u_.map[1] = mapZ_; u_.map[2] = 1.0f / mapExtent_; u_.map[3] = useMap ? 1.0f : 0.0f;
    u_.info[1] = kMapRange;
    u_.info[2] = dev_->info().renderTargetFlipY ? 1.0f : 0.0f;
    u_.info[3] = nightGlow_;
    if (quality_ == LightQuality::Off) u_.info[0] = 0;
    // the reflection is sampled by gl_FragCoord of the main view (its size, not the half-res target)
    u_.sky[1] = reflReady_ ? 0.9f : 0.0f;
    u_.sky[2] = 1.0f / viewW_; u_.sky[3] = 1.0f / viewH_;
    u_.env[0] = quality_ == LightQuality::Medium || quality_ == LightQuality::High ? 1.0f : 0.0f;
    dev_->setUniforms(uniformSlot, &u_, sizeof u_);
    dev_->setTexture(textureSlot, useMap ? map_ : black_);
    if (reflectionSlot >= 0) dev_->setTexture(reflectionSlot, reflReady_ ? reflColor_ : black_);
}

void LightSystem::addContactShadow(float3 c, float hx, float hz, float strength, bool box, float softness) {
    if (!contact_ || strength <= 0 || decals_.size() >= kMaxLights * 6) return;
    const float y = c.y + 0.025f, s = box ? 1.0f : 0.0f;
    const HaloVertex v0{c.x - hx, y, c.z - hz, -1, -1, s, softness, 0, strength}, v1{c.x + hx, y, c.z - hz, 1, -1, s, softness, 0, strength},
                     v2{c.x + hx, y, c.z + hz, 1, 1, s, softness, 0, strength}, v3{c.x - hx, y, c.z + hz, -1, 1, s, softness, 0, strength};
    decals_.insert(decals_.end(), {v0, v1, v2, v0, v2, v3});
}

void LightSystem::drawHalos(const mat4& viewProj, bool decals) {
    if (dev_ && decals && !decals_.empty() && decalPipe_.valid()) {
        dev_->setPipeline(decalPipe_);
        dev_->setUniforms(0, viewProj.m, sizeof viewProj.m);
        dev_->setVertexBuffer(decalVB_, stream(decalVB_, decalCursor_, smallCap_, decals_.data(), decals_.size() * sizeof(HaloVertex)));
        dev_->draw(uint32_t(decals_.size()));
    }
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
    // spot shafts: a strip from the light along the beam to where it meets the ground
    beams_.clear();
    const float3 fwd = normalize(cross(camUp_, camRight_));
    for (const PointLight& l : lights_) {
        if (!l.spot() || l.beam <= 0) continue;
        const float h = std::max(l.pos.y - (l.groundY > -1e8f ? l.groundY : l.pos.y - 1.0f), 0.1f);
        const float len = l.dir.y < -0.05f ? std::min(h / -l.dir.y, l.radius) : l.radius * 0.6f;
        const float tanHalf = std::sqrt(std::max(1.0f - l.cosOuter * l.cosOuter, 0.0f)) / std::max(l.cosOuter, 0.05f);
        const float w0 = 0.25f, w1 = std::max(len * tanHalf * 1.2f, w0 * 2.0f);
        const float3 axis = l.dir * len, p0 = l.pos, p1 = l.pos + axis;
        float3 side = cross(l.dir, fwd);
        side = length(side) < 1e-3f ? camRight_ : normalize(side);
        const float m = std::max({l.color.x, l.color.y, l.color.z, 1e-3f});
        const float cr = l.color.x / m, cg = l.color.y / m, cb = l.color.z / m, a = std::min(l.beam, 1.0f);
        const float3 a0 = p0 - side * w0, a1 = p0 + side * w0, b1 = p1 + side * w1, b0 = p1 - side * w1;
        const HaloVertex v0{a0.x, a0.y, a0.z, -1, 0, cr, cg, cb, a}, v1{a1.x, a1.y, a1.z, 1, 0, cr, cg, cb, a},
                         v2{b1.x, b1.y, b1.z, 1, 1, cr, cg, cb, a}, v3{b0.x, b0.y, b0.z, -1, 1, cr, cg, cb, a};
        beams_.insert(beams_.end(), {v0, v1, v2, v0, v2, v3});
    }
    if (decals) { stats_.beams = int(beams_.size() / 6); stats_.halos = int(halos_.size() / 6); stats_.decals = int(decals_.size() / 6); }
    if (!beams_.empty() && beamPipe_.valid()) {
        dev_->setPipeline(beamPipe_);
        dev_->setUniforms(0, viewProj.m, sizeof viewProj.m);
        dev_->setVertexBuffer(beamVB_, stream(beamVB_, beamCursor_, smallCap_, beams_.data(), beams_.size() * sizeof(HaloVertex)));
        dev_->draw(uint32_t(beams_.size()));
    }
    if (halos_.empty()) return;
    dev_->setPipeline(haloPipe_);
    dev_->setUniforms(0, viewProj.m, sizeof viewProj.m);
    dev_->setVertexBuffer(haloVB_, stream(haloVB_, haloCursor_, smallCap_, halos_.data(), halos_.size() * sizeof(HaloVertex)));
    dev_->draw(uint32_t(halos_.size()));
}

} // namespace rts::render
