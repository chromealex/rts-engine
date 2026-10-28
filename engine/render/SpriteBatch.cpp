#include "SpriteBatch.h"
#include "SpriteShaders.h"
#include <algorithm>
#include <cstring>

namespace rts::render {

namespace {
constexpr uint32_t kMaxQuadsPerDraw = 16384;   // 16-bit indices
struct Uniforms { float viewProj[16]; float params[4]; };
struct LitUniforms { float viewProj[16]; float params[4]; float ref[4]; };
constexpr float kAlphaCut = 0.02f;
constexpr float kCasterCut = 0.5f;
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
    pd.attrs[4] = {4, gfx::VertexFormat::Float4, 28};
    pd.attrCount = 4; pd.stride = sizeof(SpriteVertex);
    pd.indexType = gfx::IndexType::UInt16;
    pd.cull = gfx::CullMode::None;          // mirrored facings flip the winding
    pd.blend = gfx::BlendMode::Alpha;
    pd.depthTest = true;
    pd.depthWrite = false;                  // painter order between sprites, depth only against the 3D scene
    pd.label = "sprite";
    blendPipe_ = dev.createPipeline(pd);
    // decals are cut from the ground surface: same triangles, pulled towards the camera in depth
    pd.depthBiasConstant = -4.0f; pd.depthBiasSlope = -1.5f;
    pd.label = "sprite decal";
    decalPipe_ = dev.createPipeline(pd);
    pd.depthBiasConstant = 0; pd.depthBiasSlope = 0;
    pd.depthWrite = true;                   // tile ground: later geometry (overlays) tests against it
    pd.label = "sprite ground";
    groundPipe_ = dev.createPipeline(pd);

    // lit variant (optional: a failure only leaves sprites unlit)
    gfx::ShaderDesc ls;
    ls.glslVertex = shaders::kSpriteLitVS; ls.glslFragment = shaders::kSpriteLitFS; ls.msl = shaders::kSpriteLitMSL;
    ls.textureSlots = 3; ls.uniformSlots = 3; ls.label = "sprite lit";
    litShader_ = dev.createShader(ls);
    if (litShader_.valid()) {
        gfx::PipelineDesc lp = pd;
        lp.shader = litShader_;
        lp.attrCount = 5;
        lp.depthWrite = true; lp.label = "sprite lit ground";
        litGroundPipe_ = dev.createPipeline(lp);
        lp.depthWrite = false; lp.label = "sprite lit";
        litPipe_ = dev.createPipeline(lp);
        lp.depthBiasConstant = -4.0f; lp.depthBiasSlope = -1.5f; lp.label = "sprite lit decal";
        litDecalPipe_ = dev.createPipeline(lp);
    }
    gfx::ShaderDesc cs;
    cs.glslVertex = shaders::kSpriteCasterVS; cs.glslFragment = shaders::kSpriteCasterFS; cs.msl = shaders::kSpriteCasterMSL;
    cs.textureSlots = 1; cs.uniformSlots = 1; cs.label = "sprite caster";
    casterShader_ = dev.createShader(cs);
    if (casterShader_.valid()) {
        gfx::PipelineDesc cp;
        cp.shader = casterShader_;
        cp.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
        cp.attrs[1] = {1, gfx::VertexFormat::Float2, 12};
        cp.attrCount = 2; cp.stride = sizeof(SpriteVertex);
        cp.indexType = gfx::IndexType::UInt16;
        cp.cull = gfx::CullMode::None;
        cp.depthOnly = true;
        // almost no bias: the quads stand nearly along the light, a slope bias would push their shadow
        // away from the foot (peter-panning). Lit sprites receive away from their own quad (shader).
        cp.depthBiasConstant = 0.5f;
        cp.label = "sprite caster";
        casterPipe_ = dev.createPipeline(cp);
    }

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
    vb.size = 256 * 4 * sizeof(SpriteVertex); vb.label = "sprite casters";   // own buffer: written in the same frame
    casterVb_ = dev.createBuffer(vb);
    return blendPipe_.valid() && groundPipe_.valid() && decalPipe_.valid() && ib_.valid() && vb_.valid();
}

void SpriteBatch::shutdown() {
    if (!dev_) return;
    dev_->destroyBuffer(vb_); dev_->destroyBuffer(ib_); dev_->destroyBuffer(casterVb_);
    for (auto p : {blendPipe_, groundPipe_, decalPipe_, litPipe_, litGroundPipe_, litDecalPipe_, casterPipe_})
        if (p.valid()) dev_->destroyPipeline(p);
    for (auto sh : {shader_, litShader_, casterShader_}) if (sh.valid()) dev_->destroyShader(sh);
    dev_ = nullptr;
}

void SpriteBatch::appendQuad(std::vector<SpriteVertex>& out, const TextureRef& t, const float3 (&c)[4], float4 uv, uint32_t color) {
    const float2 st[4] = {{t.u(uv.x), t.v(uv.y)}, {t.u(uv.z), t.v(uv.y)}, {t.u(uv.z), t.v(uv.w)}, {t.u(uv.x), t.v(uv.w)}};
    for (int i = 0; i < 4; ++i) out.push_back({c[i], st[i], color, 0, {c[i].x, c[i].y, c[i].z, 0}});
}

void SpriteBatch::quad(const TextureRef& t, const float3 (&c)[4], float4 uv, uint32_t color, uint32_t team, int layer, float sortKey) {
    if (!t.valid()) return;
    Quad q;
    q.tex = t.tex; q.layer = layer; q.key = sortKey; q.seq = uint32_t(quads_.size()); q.flags = 0;
    const float2 st[4] = {{t.u(uv.x), t.v(uv.y)}, {t.u(uv.z), t.v(uv.y)}, {t.u(uv.z), t.v(uv.w)}, {t.u(uv.x), t.v(uv.w)}};
    for (int i = 0; i < 4; ++i) q.v[i] = {c[i], st[i], color, team, {c[i].x, c[i].y, c[i].z, 0}};
    quads_.push_back(q);
}

void SpriteBatch::quadEx(gfx::TextureHandle tex, const SpriteVertex (&v)[4], int layer, float sortKey, uint32_t flags) {
    if (!tex.valid()) return;
    Quad q;
    q.tex = tex; q.layer = layer; q.key = sortKey; q.seq = uint32_t(quads_.size()); q.flags = flags;
    for (int i = 0; i < 4; ++i) q.v[i] = v[i];
    quads_.push_back(q);
}

void SpriteBatch::caster(gfx::TextureHandle tex, const float3 (&c)[4], const float2 (&uv)[4]) {
    if (!tex.valid()) return;
    Caster k;
    k.tex = tex;
    for (int i = 0; i < 4; ++i) k.v[i] = {c[i], uv[i], 0, 0, {}};
    casters_.push_back(k);
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

void SpriteBatch::flushCasters(gfx::Device& dev, const mat4& lightViewProj) {
    if (casters_.empty() || !casterPipe_.valid()) return;
    // group by texture (no painter order in a depth pass)
    std::stable_sort(casters_.begin(), casters_.end(), [](const Caster& a, const Caster& b) { return a.tex.id < b.tex.id; });
    casterVerts_.clear();
    casterVerts_.reserve(casters_.size() * 4);
    for (auto& k : casters_) for (auto& v : k.v) casterVerts_.push_back(v);
    dev.updateBuffer(casterVb_, casterVerts_.data(), casterVerts_.size() * sizeof(SpriteVertex));
    dev.setPipeline(casterPipe_);
    Uniforms u{};
    std::memcpy(u.viewProj, lightViewProj.m, sizeof u.viewProj);
    u.params[0] = kCasterCut;
    dev.setUniforms(0, &u, sizeof u);
    uint32_t start = 0;
    while (start < casters_.size()) {
        gfx::TextureHandle tex = casters_[start].tex;
        uint32_t end = start + 1;
        while (end < casters_.size() && casters_[end].tex == tex) ++end;
        dev.setTexture(0, tex);
        drawRange(dev, casterVb_, start, end - start);
        start = end;
    }
}

void SpriteBatch::setUniforms(gfx::Device& dev, const mat4& viewProj, bool lit, const SpriteLighting* lighting) {
    if (lit) {
        lighting->bind();
        LitUniforms u{};
        std::memcpy(u.viewProj, viewProj.m, sizeof u.viewProj);
        u.params[0] = kAlphaCut;
        u.params[1] = lighting->receiveShadows ? 1.0f : 0.0f;
        u.ref[0] = std::max(lighting->ref.x, 1e-3f); u.ref[1] = std::max(lighting->ref.y, 1e-3f); u.ref[2] = std::max(lighting->ref.z, 1e-3f);
        dev.setUniforms(0, &u, sizeof u);
    } else {
        Uniforms u{};
        std::memcpy(u.viewProj, viewProj.m, sizeof u.viewProj);
        u.params[0] = kAlphaCut;
        dev.setUniforms(0, &u, sizeof u);
    }
}

void SpriteBatch::flush(gfx::Device& dev, const mat4& viewProj, const SpriteLighting* lighting) {
    drawCalls_ = 0;
    if (quads_.empty()) return;
    const bool canLight = lighting && lighting->bind && litPipe_.valid() && litDecalPipe_.valid();
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
    // runs of equal texture and pipeline; the lit shader takes the sheet in slot 2 (0/1: shadow + lights)
    auto flagsOf = [&](uint32_t qi) { uint32_t f = quads_[qi].flags; if (!canLight) f &= ~uint32_t(kSpriteLit); return f; };
    uint32_t curFlags = ~0u;
    uint32_t start = 0;
    while (start < order_.size()) {
        const gfx::TextureHandle tex = quads_[order_[start]].tex;
        const uint32_t fl = flagsOf(order_[start]);
        uint32_t end = start + 1;
        while (end < order_.size() && quads_[order_[end]].tex == tex && flagsOf(order_[end]) == fl) ++end;
        const bool lit = fl & kSpriteLit;
        if (fl != curFlags) {
            const bool decal = fl & kSpriteDecal;
            dev.setPipeline(lit ? (decal ? litDecalPipe_ : litPipe_) : (decal ? decalPipe_ : blendPipe_));
            setUniforms(dev, viewProj, lit, lighting);
            curFlags = fl;
        }
        dev.setTexture(lit ? 2 : 0, tex);
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

void SpriteBatch::drawMesh(gfx::Device& dev, const SpriteMesh& m, gfx::TextureHandle tex, const mat4& viewProj, const SpriteLighting* lighting) {
    if (!m.vb.valid() || !tex.valid()) return;
    const bool lit = lighting && lighting->bind && litGroundPipe_.valid();
    dev.setPipeline(lit ? litGroundPipe_ : groundPipe_);
    setUniforms(dev, viewProj, lit, lighting);
    dev.setTexture(lit ? 2 : 0, tex);
    drawRange(dev, m.vb, 0, m.quads);
}

} // namespace rts::render
