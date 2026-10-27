#include "SpriteBatch.h"
#include "SpriteShaders.h"
#include <algorithm>
#include <cstring>

namespace rts::render {

namespace {
constexpr uint32_t kMaxQuadsPerDraw = 16384;   // 16-bit indices
struct Uniforms { float viewProj[16]; float params[4]; };
} // namespace

bool SpriteBatch::init(gfx::Device& dev) {
    dev_ = &dev;
    gfx::ShaderDesc sd;
    sd.glslVertex = shaders::kSpriteVS; sd.glslFragment = shaders::kSpriteFS; sd.msl = shaders::kSpriteMSL;
    sd.textureSlots = 1; sd.uniformSlots = 1; sd.label = "sprite";
    shader_ = dev.createShader(sd);
    if (!shader_.valid()) return false;
    gfx::PipelineDesc pd;
    pd.shader = shader_;
    pd.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
    pd.attrs[1] = {1, gfx::VertexFormat::Float2, 12};
    pd.attrs[2] = {2, gfx::VertexFormat::UByte4N, 20};
    pd.attrs[3] = {3, gfx::VertexFormat::UByte4N, 24};
    pd.attrCount = 4; pd.stride = sizeof(SpriteVertex);
    pd.indexType = gfx::IndexType::UInt16;
    pd.cull = gfx::CullMode::None;          // mirrored facings flip the winding
    pd.blend = gfx::BlendMode::Alpha;
    pd.depthTest = true;
    pd.depthWrite = false;                  // painter order between sprites, depth only against the 3D scene
    pd.label = "sprite";
    blendPipe_ = dev.createPipeline(pd);
    pd.depthWrite = true;                   // tile ground: later geometry (overlays) tests against it
    pd.label = "sprite ground";
    groundPipe_ = dev.createPipeline(pd);

    std::vector<uint16_t> idx(size_t(kMaxQuadsPerDraw) * 6);
    for (uint32_t q = 0; q < kMaxQuadsPerDraw; ++q) {
        uint16_t b = uint16_t(q * 4);
        uint16_t* p = &idx[size_t(q) * 6];
        p[0] = b; p[1] = uint16_t(b + 1); p[2] = uint16_t(b + 2); p[3] = b; p[4] = uint16_t(b + 2); p[5] = uint16_t(b + 3);
    }
    gfx::BufferDesc ib; ib.type = gfx::BufferType::Index; ib.size = idx.size() * sizeof(uint16_t); ib.data = idx.data(); ib.label = "sprite quads";
    ib_ = dev.createBuffer(ib);
    gfx::BufferDesc vb; vb.usage = gfx::Usage::Dynamic; vb.size = 1024 * 4 * sizeof(SpriteVertex); vb.label = "sprites";
    vb_ = dev.createBuffer(vb);
    return blendPipe_.valid() && groundPipe_.valid() && ib_.valid() && vb_.valid();
}

void SpriteBatch::shutdown() {
    if (!dev_) return;
    dev_->destroyBuffer(vb_); dev_->destroyBuffer(ib_);
    dev_->destroyPipeline(blendPipe_); dev_->destroyPipeline(groundPipe_);
    dev_->destroyShader(shader_);
    dev_ = nullptr;
}

void SpriteBatch::appendQuad(std::vector<SpriteVertex>& out, const TextureRef& t, const float3 (&c)[4], float4 uv, uint32_t color) {
    const float2 st[4] = {{t.u(uv.x), t.v(uv.y)}, {t.u(uv.z), t.v(uv.y)}, {t.u(uv.z), t.v(uv.w)}, {t.u(uv.x), t.v(uv.w)}};
    for (int i = 0; i < 4; ++i) out.push_back({c[i], st[i], color, 0});
}

void SpriteBatch::quad(const TextureRef& t, const float3 (&c)[4], float4 uv, uint32_t color, uint32_t team, int layer, float sortKey) {
    if (!t.valid()) return;
    Quad q;
    q.tex = t.tex; q.layer = layer; q.key = sortKey; q.seq = uint32_t(quads_.size());
    const float2 st[4] = {{t.u(uv.x), t.v(uv.y)}, {t.u(uv.z), t.v(uv.y)}, {t.u(uv.z), t.v(uv.w)}, {t.u(uv.x), t.v(uv.w)}};
    for (int i = 0; i < 4; ++i) q.v[i] = {c[i], st[i], color, team};
    quads_.push_back(q);
}

void SpriteBatch::drawRange(gfx::Device& dev, gfx::BufferHandle vb, uint32_t first, uint32_t count) {
    while (count > 0) {
        uint32_t n = std::min(count, kMaxQuadsPerDraw);
        dev.setVertexBuffer(vb);
        dev.setIndexBuffer(ib_);
        dev.drawIndexed(n * 6, 0, int32_t(first * 4));
        ++drawCalls_;
        first += n; count -= n;
    }
}

void SpriteBatch::flush(gfx::Device& dev, const mat4& viewProj) {
    drawCalls_ = 0;
    if (quads_.empty()) return;
    order_.resize(quads_.size());
    for (uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
    std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
        const Quad& A = quads_[a]; const Quad& B = quads_[b];
        if (A.layer != B.layer) return A.layer < B.layer;
        if (A.key != B.key) return A.key < B.key;
        return A.seq < B.seq;
    });
    verts_.clear();
    verts_.reserve(quads_.size() * 4);
    for (uint32_t i : order_) for (auto& v : quads_[i].v) verts_.push_back(v);
    dev.updateBuffer(vb_, verts_.data(), verts_.size() * sizeof(SpriteVertex));
    dev.setPipeline(blendPipe_);
    Uniforms u{};
    std::memcpy(u.viewProj, viewProj.m, sizeof u.viewProj);
    u.params[0] = 0.02f;
    dev.setUniforms(0, &u, sizeof u);
    uint32_t start = 0;
    while (start < order_.size()) {
        gfx::TextureHandle tex = quads_[order_[start]].tex;
        uint32_t end = start + 1;
        while (end < order_.size() && quads_[order_[end]].tex == tex) ++end;
        dev.setTexture(0, tex);
        drawRange(dev, vb_, start, end - start);
        start = end;
    }
}

SpriteMesh SpriteBatch::buildMesh(gfx::Device& dev, const std::vector<SpriteVertex>& verts) {
    SpriteMesh m;
    if (verts.empty()) return m;
    gfx::BufferDesc vb; vb.size = verts.size() * sizeof(SpriteVertex); vb.data = verts.data(); vb.label = "sprite mesh";
    m.vb = dev.createBuffer(vb);
    m.quads = uint32_t(verts.size() / 4);
    return m;
}

void SpriteBatch::destroyMesh(gfx::Device& dev, SpriteMesh& m) {
    if (m.vb.valid()) dev.destroyBuffer(m.vb);
    m = {};
}

void SpriteBatch::drawMesh(gfx::Device& dev, const SpriteMesh& m, gfx::TextureHandle tex, const mat4& viewProj) {
    if (!m.vb.valid() || !tex.valid()) return;
    dev.setPipeline(groundPipe_);
    Uniforms u{};
    std::memcpy(u.viewProj, viewProj.m, sizeof u.viewProj);
    u.params[0] = 0.02f;
    dev.setUniforms(0, &u, sizeof u);
    dev.setTexture(0, tex);
    drawRange(dev, m.vb, 0, m.quads);
}

} // namespace rts::render
