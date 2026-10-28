#include "Renderer3D.h"
#include "Shaders.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rts::render {

namespace {
constexpr float kPi = 3.14159265358979f;

struct DrawUniforms { float model[16]; float color[4]; float params[4]; };
struct FrameUniforms {
    float viewProj[16];
    float lightViewProj[16];
    float sunDir[4], sunColor[4], skyColor[4], groundColor[4], fog[4], camPos[4], shadow[4], quality[4];
};
void put3(float* d, float3 v, float w) { d[0] = v.x; d[1] = v.y; d[2] = v.z; d[3] = w; }
} // namespace

// ============================== Renderer3D ==============================

ShadowQuality Renderer3D::defaultQualityFor(const gfx::Device& dev, bool mobile) {
    if (const char* q = std::getenv("RTS_SHADOWS")) { // testing override: off|low|medium|high
        std::string v = q;
        return v == "off" ? ShadowQuality::Off : v == "low" ? ShadowQuality::Low : v == "high" ? ShadowQuality::High : ShadowQuality::Medium;
    }
    const std::string& n = dev.info().name;
    if (n.find("llvmpipe") != std::string::npos || n.find("SwiftShader") != std::string::npos) return ShadowQuality::Low;
    if (mobile) {
        // Old/weak mobile GPUs get the cheapest shadows; everything else medium.
        for (const char* weak : {"Mali-4", "Mali-T", "Adreno (TM) 3", "Adreno (TM) 4", "Adreno (TM) 50", "PowerVR SGX", "PowerVR Rogue GE"})
            if (n.find(weak) != std::string::npos) return ShadowQuality::Low;
        return ShadowQuality::Medium;
    }
    return ShadowQuality::High;
}

bool Renderer3D::init(gfx::Device& dev) {
    dev_ = &dev;
    gfx::ShaderDesc ls; ls.glslVertex = shaders::kLitVS; ls.glslFragment = shaders::kLitFS; ls.msl = shaders::kLitMSL;
    ls.uniformSlots = 3; ls.textureSlots = 3; ls.label = "lit"; // U2/tex1/tex2: Lights (tex2 water reflection)
    litShader_ = dev.createShader(ls);
    gfx::ShaderDesc ss; ss.glslVertex = shaders::kShadowVS; ss.glslFragment = shaders::kShadowFS; ss.msl = shaders::kShadowMSL;
    ss.uniformSlots = 2; ss.label = "shadow";
    shadowShader_ = dev.createShader(ss);
    gfx::ShaderDesc cs; cs.glslVertex = shaders::kColorVS; cs.glslFragment = shaders::kColorFS; cs.msl = shaders::kColorMSL;
    cs.uniformSlots = 2; cs.label = "color";
    colorShader_ = dev.createShader(cs);
    if (!litShader_.valid() || !shadowShader_.valid() || !colorShader_.valid()) return false;

    gfx::PipelineDesc mp;
    mp.shader = litShader_;
    mp.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
    mp.attrs[1] = {1, gfx::VertexFormat::Float3, 12};
    mp.attrs[2] = {2, gfx::VertexFormat::UByte4N, 24};
    mp.attrCount = 3; mp.stride = sizeof(MeshVertex);
    mp.indexType = gfx::IndexType::UInt32;
    litPipe_ = dev.createPipeline(mp);
    mp.blend = gfx::BlendMode::Alpha; mp.depthWrite = false;
    litBlendPipe_ = dev.createPipeline(mp);

    gfx::PipelineDesc sp;
    sp.shader = shadowShader_;
    sp.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
    sp.attrCount = 1; sp.stride = sizeof(MeshVertex);
    sp.indexType = gfx::IndexType::UInt32;
    sp.depthOnly = true;
    sp.cull = gfx::CullMode::Back;   // models are closed meshes; back faces would self-shadow less but cost more
    sp.depthBiasConstant = 2.0f; sp.depthBiasSlope = 2.5f;
    shadowPipe_ = dev.createPipeline(sp);

    gfx::PipelineDesc lp;
    lp.shader = colorShader_;
    lp.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
    lp.attrs[1] = {1, gfx::VertexFormat::UByte4N, 12};
    lp.attrCount = 2; lp.stride = sizeof(ColorVertex);
    lp.indexType = gfx::IndexType::None;
    lp.primitive = gfx::Primitive::Lines;
    lp.cull = gfx::CullMode::None;
    lp.blend = gfx::BlendMode::Alpha;
    lp.depthWrite = false;
    linePipe_ = dev.createPipeline(lp);
    lp.primitive = gfx::Primitive::Triangles;
    triPipe_ = dev.createPipeline(lp);

    gfx::BufferDesc vb; vb.usage = gfx::Usage::Dynamic; vb.size = 64 * 1024;
    lineVB_ = dev.createBuffer(vb);
    triVB_ = dev.createBuffer(vb);
    setShadowQuality(quality_);
    initModels();
    if (!lights.init(dev)) return false;
    return litPipe_.valid() && shadowPipe_.valid() && linePipe_.valid() && triPipe_.valid();
}

void Renderer3D::setShadowQuality(ShadowQuality q) {
    quality_ = q;
    if (!dev_) return;
    int size = q == ShadowQuality::High ? 2048 : q == ShadowQuality::Medium ? 1536 : 1024;
    if (q == ShadowQuality::Off) size = 0;
    if (size == shadowSize_ && shadowMap_.valid()) return;
    if (shadowMap_.valid()) dev_->destroyTexture(shadowMap_);
    shadowMap_ = {};
    shadowSize_ = size;
    gfx::TextureDesc td;
    td.width = td.height = std::max(size, 16);
    td.format = gfx::PixelFormat::Depth;
    td.renderTarget = true; td.compare = true; td.filter = gfx::Filter::Linear;
    shadowMap_ = dev_->createTexture(td);
}

void Renderer3D::shutdown() {
    if (!dev_) return;
    shutdownModels();
    lights.shutdown();
    dev_->destroyTexture(shadowMap_);
    dev_->destroyBuffer(lineVB_); dev_->destroyBuffer(triVB_);
    for (auto p : {litPipe_, litBlendPipe_, shadowPipe_, linePipe_, triPipe_}) dev_->destroyPipeline(p);
    for (auto s : {litShader_, shadowShader_, colorShader_}) dev_->destroyShader(s);
    dev_ = nullptr;
}

void Renderer3D::beginScene(const RTSCamera& cam, int fbW, int fbH) {
    sceneW_ = fbW; sceneH_ = fbH; // viewport of the main pass (screen or an offscreen target of this size)
    float aspect = fbH > 0 ? float(fbW) / float(fbH) : 1.0f;
    viewProj_ = cam.proj(aspect, dev_->info().clipDepthZeroToOne) * cam.view();
    camPos_ = cam.eye();
    cam_ = cam;
    items_.clear(); lines_.clear(); tris_.clear(); custom_.clear(); customOpaque_.clear(); customShadow_.clear();
    modelItems_.clear(); jointData_.clear(); morphData_.clear();
    computeShadowMatrix(cam, aspect);
    lights.begin(cam, aspect);
}

void Renderer3D::computeShadowMatrix(const RTSCamera& cam, float aspect) {
    // Fit an orthographic light frustum around the part of the ground the camera sees.
    float3 pts[8];
    int n = 0;
    const float w = 1000.0f, h = w / aspect;
    const float sx[4] = {0, w, 0, w}, sy[4] = {0, 0, h, h};
    for (int i = 0; i < 4; ++i) {
        float3 g;
        if (!cam.screenToGround(sx[i], sy[i], w, h, g) || length(g - cam.target) > cam.distance * 3.0f) {
            float3 o, d;
            cam.screenRay(sx[i], sy[i], w, h, o, d);
            float3 flatD = normalize(float3{d.x, 0, d.z});
            g = cam.target + flatD * (cam.distance * 1.6f);
        }
        pts[n++] = g;
        pts[n++] = g + float3{0, 5.0f, 0}; // tall objects
    }
    float3 L = normalize(lighting.sunDir);
    float3 up = std::fabs(L.y) > 0.99f ? float3{0, 0, 1} : float3{0, 1, 0};
    mat4 view = mat4::lookAt(cam.target + L * 60.0f, cam.target, up);
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, minZ = 1e9f, maxZ = -1e9f;
    for (int i = 0; i < n; ++i) {
        float3 p = view.transformPoint(pts[i]);
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
    }
    // texel snapping keeps shadow edges stable while the camera pans
    float size = std::max(maxX - minX, maxY - minY) + 2.0f;
    float texel = size / float(std::max(shadowSize_, 16));
    shadowTexelWorld_ = texel;
    shadowExtent_ = size;
    float cx = std::floor((minX + maxX) * 0.5f / texel) * texel, cy = std::floor((minY + maxY) * 0.5f / texel) * texel;
    mat4 proj = mat4::ortho(cx - size * 0.5f, cx + size * 0.5f, cy - size * 0.5f, cy + size * 0.5f,
                            -maxZ - 40.0f, -minZ + 10.0f, dev_->info().clipDepthZeroToOne);
    lightViewProj_ = proj * view;
}

void Renderer3D::drawMesh(const Mesh& m, const mat4& model, float4 color, uint32_t flags, float specular, float emissive) {
    items_.push_back({&m, model, color, flags, specular, emissive});
}

void Renderer3D::line(float3 a, float3 b, uint32_t c) { lines_.push_back({a, c}); lines_.push_back({b, c}); }
void Renderer3D::triangle(float3 a, float3 b, float3 c, uint32_t col) { tris_.push_back({a, col}); tris_.push_back({b, col}); tris_.push_back({c, col}); }

void Renderer3D::quadXZ(float3 a, float3 b, float w, uint32_t c, float y) {
    float3 d = b - a; d.y = 0;
    float len = length(d);
    if (len < 1e-5f) return;
    float3 side = float3{-d.z, 0, d.x} * (w * 0.5f / len);
    a.y = b.y = y;
    triangle(a - side, b - side, b + side, c);
    triangle(a - side, b + side, a + side, c);
}

void Renderer3D::circleXZ(float3 c, float r, uint32_t col, int seg, float y) {
    for (int i = 0; i < seg; ++i) {
        float a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
        line({c.x + std::cos(a0) * r, y, c.z + std::sin(a0) * r}, {c.x + std::cos(a1) * r, y, c.z + std::sin(a1) * r}, col);
    }
}

void Renderer3D::uploadFrameUniforms(bool forShadow) {
    FrameUniforms f{};
    std::memcpy(f.viewProj, viewProj_.m, sizeof f.viewProj);
    std::memcpy(f.lightViewProj, lightViewProj_.m, sizeof f.lightViewProj);
    put3(f.sunDir, normalize(lighting.sunDir), lighting.sunIntensity);
    put3(f.sunColor, lighting.sunColor, 1);
    put3(f.skyColor, lighting.skyColor, 1);
    put3(f.groundColor, lighting.groundColor, 1);
    put3(f.fog, lighting.fogColor, lighting.fogDensity);
    put3(f.camPos, camPos_, 1);
    const bool zeroToOne = dev_->info().clipDepthZeroToOne;
    f.shadow[0] = dev_->info().renderTargetFlipY ? 1.0f : 0.0f;
    f.shadow[1] = zeroToOne ? 1.0f : 0.5f;
    f.shadow[2] = zeroToOne ? 0.0f : 0.5f;
    f.shadow[3] = 1.0f / float(std::max(shadowSize_, 16));
    // taps: High 16 rotated Poisson, Medium 8 rotated Poisson, Low 4 fixed (each tap is a 2x2 hardware PCF)
    f.quality[0] = quality_ == ShadowQuality::High ? 16.0f : quality_ == ShadowQuality::Medium ? 8.0f : 4.0f;
    {
        // never filter narrower than ~1.5 texels, otherwise the map resolution shows as stair steps
        float radiusWorld = std::max(lighting.shadowSoftness, shadowTexelWorld_ * 1.5f);
        // Low (4 fixed taps): a wide radius would show 4 offset copies, keep it around two texels
        if (quality_ == ShadowQuality::Low) radiusWorld = std::min(radiusWorld, shadowTexelWorld_ * 2.0f);
        f.quality[2] = radiusWorld / std::max(shadowExtent_, 1.0f);
        // normal offset grows with the kernel: wide filters otherwise compare against the receiver itself (acne)
        f.quality[3] = std::max(shadowTexelWorld_ * 1.5f, radiusWorld * 0.9f);
    }
    f.quality[1] = quality_ == ShadowQuality::Off || forShadow ? 0.0f : 1.0f;
    dev_->setUniforms(1, &f, sizeof f);
}

void Renderer3D::bindLitFrame() {
    uploadFrameUniforms(false);
    dev_->setTexture(0, shadowMap_);
    lights.bind();
}

void Renderer3D::render(const gfx::PassDesc& screenPass) {
    drawCalls_ = 0;
    // ---- shadow pass
    if (quality_ != ShadowQuality::Off && shadowMap_.valid()) {
        gfx::PassDesc sp;
        sp.depthTarget = shadowMap_;
        sp.clearColorEnabled = false;
        sp.clearDepthEnabled = true;
        dev_->beginPass(sp);
        dev_->setViewport({0, 0, shadowSize_, shadowSize_});
        dev_->setPipeline(shadowPipe_);
        uploadFrameUniforms(true);
        for (auto& it : items_) {
            if (it.flags & (kDrawNoCastShadow | kDrawUnlit) || it.color.w < 0.99f) continue;
            DrawUniforms u{};
            std::memcpy(u.model, it.model.m, sizeof u.model);
            dev_->setUniforms(0, &u, sizeof u);
            dev_->setVertexBuffer(it.mesh->vb);
            dev_->setIndexBuffer(it.mesh->ib);
            dev_->drawIndexed(it.mesh->indexCount);
            ++drawCalls_;
        }
        renderModels(0);
        for (auto& fn : customShadow_) fn(*dev_);
        dev_->endPass();
    }

    lights.renderMap();
    renderReflection();   // High: mirrored pass for water (Renderer3DReflect.cpp)

    // ---- main pass (left open for the caller's UI)
    dev_->beginPass(screenPass);
    dev_->setViewport({0, 0, sceneW_ > 0 ? sceneW_ : dev_->framebufferWidth(), sceneH_ > 0 ? sceneH_ : dev_->framebufferHeight()});
    for (auto& fn : customOpaque_) fn(*dev_);
    dev_->setPipeline(litPipe_);
    uploadFrameUniforms(false);
    dev_->setTexture(0, shadowMap_);
    lights.bind(2, 1, 2);
    auto drawItem = [&](const DrawItem& it) {
        DrawUniforms u{};
        std::memcpy(u.model, it.model.m, sizeof u.model);
        u.color[0] = it.color.x; u.color[1] = it.color.y; u.color[2] = it.color.z; u.color[3] = it.color.w;
        u.params[0] = (it.flags & kDrawUnlit) ? 1.f : 0.f;
        u.params[1] = it.specular;
        u.params[2] = (it.flags & kDrawNoReceiveShadow) ? 0.f : 1.f;
        u.params[3] = it.emissive;
        dev_->setUniforms(0, &u, sizeof u);
        dev_->setVertexBuffer(it.mesh->vb);
        dev_->setIndexBuffer(it.mesh->ib);
        dev_->drawIndexed(it.mesh->indexCount);
        ++drawCalls_;
    };
    for (auto& it : items_) if (it.color.w >= 0.99f) drawItem(it);
    renderModels(1);
    renderModels(3);
    // translucent meshes (water, ghosts) before overlays so that overlays (fog of war, markers) stay on top
    bool any = false;
    for (auto& it : items_) {
        if (it.color.w >= 0.99f) continue;
        if (!any) { dev_->setPipeline(litBlendPipe_); uploadFrameUniforms(false); dev_->setTexture(0, shadowMap_); lights.bind(2, 1, 2); any = true; }
        drawItem(it);
    }
    renderModels(2);
    lights.drawHalos(viewProj_);   // additive glows, depth-tested against the scene
    // custom renderers (sprites) after translucent meshes: they don't write depth, so water drawn
    // later would tint them; overlays (fog of war, markers) still come on top
    for (auto& fn : custom_) fn(*dev_);
    if (!tris_.empty()) {
        dev_->updateBuffer(triVB_, tris_.data(), tris_.size() * sizeof(ColorVertex));
        dev_->setPipeline(triPipe_);
        uploadFrameUniforms(false);
        dev_->setVertexBuffer(triVB_);
        dev_->draw(uint32_t(tris_.size()));
        ++drawCalls_;
    }
    if (!lines_.empty()) {
        dev_->updateBuffer(lineVB_, lines_.data(), lines_.size() * sizeof(ColorVertex));
        dev_->setPipeline(linePipe_);
        uploadFrameUniforms(false);
        dev_->setVertexBuffer(lineVB_);
        dev_->draw(uint32_t(lines_.size()));
        ++drawCalls_;
    }
}

} // namespace rts::render
