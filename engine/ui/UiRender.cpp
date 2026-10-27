#include "UiInternal.h"

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Log.h>

#include <cstring>

#include "engine/render/Math3D.h"
#include "stb_image.h"

namespace rts::ui {

namespace {

// RmlUi produces premultiplied colours. The shader converts back to straight alpha so the RHI's
// standard alpha blend (src*a + dst*(1-a)) gives exactly the premultiplied result.
// U0: mat4 mvp; vec4 translate (xy)
const char* kVS = R"(
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec2 a_uv;
layout(std140) uniform U0 { mat4 u_mvp; vec4 u_tr; };
out vec2 v_uv;
out vec4 v_col;
void main() { v_uv = a_uv; v_col = a_col; gl_Position = u_mvp * vec4(a_pos + u_tr.xy, 0.0, 1.0); }
)";
const char* kFS = R"(
uniform sampler2D tex0;
in vec2 v_uv;
in vec4 v_col;
out vec4 o_color;
void main() {
    vec4 c = v_col * texture(tex0, v_uv);
    o_color = c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
}
)";
const char* kMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct U0 { float4x4 mvp; float4 tr; };
struct VIn { float2 pos [[attribute(0)]]; float4 col [[attribute(1)]]; float2 uv [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float2 uv; float4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) {
    VOut o; o.pos = u.mvp * float4(in.pos + u.tr.xy, 0.0, 1.0); o.uv = in.uv; o.col = in.col; return o;
}
fragment float4 fs_main(VOut in [[stage_in]], texture2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    float4 c = in.col * tex0.sample(s0, in.uv);
    return c.a > 0.0 ? float4(c.rgb / c.a, c.a) : float4(0.0);
}
)";

void mul(const float* a, const float* b, float* out) { // column-major a * b
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

} // namespace

template <class T> uintptr_t UiRenderer::put(std::vector<T>& v, std::vector<uintptr_t>& freeList, T item) {
    if (!freeList.empty()) {
        uintptr_t h = freeList.back();
        freeList.pop_back();
        v[h - 1] = std::move(item);
        return h;
    }
    v.push_back(std::move(item));
    return v.size(); // handles are index + 1 (0 = invalid)
}

bool UiRenderer::init(gfx::Device& dev) {
    dev_ = &dev;
    gfx::ShaderDesc sd;
    sd.glslVertex = kVS; sd.glslFragment = kFS; sd.msl = kMSL;
    sd.textureSlots = 1; sd.label = "rmlui";
    shader_ = dev.createShader(sd);
    if (!shader_.valid()) { Rml::Log::Message(Rml::Log::LT_ERROR, "ui shader: %s", dev.lastError().c_str()); return false; }
    gfx::PipelineDesc pd;
    pd.shader = shader_;
    pd.attrs[0] = {0, gfx::VertexFormat::Float2, offsetof(Rml::Vertex, position)};
    pd.attrs[1] = {1, gfx::VertexFormat::UByte4N, offsetof(Rml::Vertex, colour)};
    pd.attrs[2] = {2, gfx::VertexFormat::Float2, offsetof(Rml::Vertex, tex_coord)};
    pd.attrCount = 3;
    pd.stride = sizeof(Rml::Vertex);
    pd.indexType = gfx::IndexType::UInt32;
    pd.cull = gfx::CullMode::None;
    pd.blend = gfx::BlendMode::Alpha;
    pd.depthTest = false; pd.depthWrite = false;
    pd.label = "rmlui";
    pipeline_ = dev.createPipeline(pd);
    const uint32_t white = 0xffffffffu;
    gfx::TextureDesc td;
    td.width = td.height = 1; td.data = &white; td.label = "ui-white";
    white_ = dev.createTexture(td);
    return pipeline_.valid();
}

void UiRenderer::shutdown() {
    if (!dev_) return;
    for (auto& g : geos_) { if (g.vb.valid()) dev_->destroyBuffer(g.vb); if (g.ib.valid()) dev_->destroyBuffer(g.ib); }
    for (auto& t : texs_) if (t.owned && t.tex.valid()) dev_->destroyTexture(t.tex);
    geos_.clear(); texs_.clear(); freeGeos_.clear(); freeTexs_.clear();
    dev_->destroyTexture(white_);
    dev_->destroyPipeline(pipeline_);
    dev_->destroyShader(shader_);
    dev_ = nullptr;
}

void UiRenderer::beginFrame(int fbW, int fbH) {
    fbW_ = fbW; fbH_ = fbH;
    render::mat4 p = render::mat4::ortho(0, float(fbW), float(fbH), 0, -1, 1, dev_->info().clipDepthZeroToOne);
    std::memcpy(proj_, p.m, sizeof proj_);
    hasTransform_ = false;
    scissorOn_ = false;
    dev_->setViewport({0, 0, fbW, fbH});
    dev_->setScissor({});
}

void UiRenderer::setDynamicTexture(const std::string& name, gfx::TextureHandle tex, int w, int h) {
    dynamic_[name] = {tex, w, h};
    for (auto& t : texs_) if (t.dynamicName == name) t.tex = tex; // already loaded by a document
}

Rml::CompiledGeometryHandle UiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    if (vertices.empty() || indices.empty()) return 0;
    Geometry g;
    gfx::BufferDesc vb;
    vb.size = vertices.size() * sizeof(Rml::Vertex); vb.data = vertices.data(); vb.label = "ui-vb";
    g.vb = dev_->createBuffer(vb);
    gfx::BufferDesc ib;
    ib.type = gfx::BufferType::Index;
    ib.size = indices.size() * sizeof(int); ib.data = indices.data(); ib.label = "ui-ib";
    g.ib = dev_->createBuffer(ib);
    g.count = uint32_t(indices.size());
    return put(geos_, freeGeos_, g);
}

void UiRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) {
    if (!handle || handle > geos_.size()) return;
    const Geometry& g = geos_[handle - 1];
    gfx::TextureHandle tex = white_;
    if (texture && texture <= texs_.size() && texs_[texture - 1].tex.valid()) tex = texs_[texture - 1].tex;
    struct { float mvp[16]; float tr[4]; } u{};
    if (hasTransform_) mul(proj_, transform_, u.mvp);
    else std::memcpy(u.mvp, proj_, sizeof u.mvp);
    u.tr[0] = translation.x; u.tr[1] = translation.y;
    dev_->setPipeline(pipeline_);
    dev_->setVertexBuffer(g.vb);
    dev_->setIndexBuffer(g.ib);
    dev_->setTexture(0, tex);
    dev_->setUniforms(0, &u, sizeof u);
    dev_->drawIndexed(g.count);
}

void UiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    if (!handle || handle > geos_.size()) return;
    Geometry& g = geos_[handle - 1];
    if (g.vb.valid()) dev_->destroyBuffer(g.vb);
    if (g.ib.valid()) dev_->destroyBuffer(g.ib);
    g = {};
    freeGeos_.push_back(handle);
}

Rml::TextureHandle UiRenderer::LoadTexture(Rml::Vector2i& dims, const Rml::String& source) {
    if (!source.empty() && source[0] == '#') {
        Texture t;
        t.dynamicName = source.substr(1);
        auto it = dynamic_.find(t.dynamicName);
        if (it != dynamic_.end()) { t.tex = it->second.tex; dims = {it->second.w, it->second.h}; }
        else dims = {1, 1};
        return put(texs_, freeTexs_, t);
    }
    std::string file;
    if (!Rml::GetFileInterface()->LoadFile(source, file) || file.empty()) return 0;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(file.data()), int(file.size()), &w, &h, &n, 4);
    if (!px) { Rml::Log::Message(Rml::Log::LT_WARNING, "ui: cannot decode image '%s'", source.c_str()); return 0; }
    for (int i = 0; i < w * h; ++i) { // premultiply, like every other RmlUi colour
        unsigned char* p = px + i * 4;
        for (int c = 0; c < 3; ++c) p[c] = static_cast<unsigned char>((p[c] * p[3] + 127) / 255);
    }
    dims = {w, h};
    Rml::TextureHandle th = GenerateTexture({reinterpret_cast<const Rml::byte*>(px), size_t(w) * size_t(h) * 4}, dims);
    stbi_image_free(px);
    return th;
}

Rml::TextureHandle UiRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dims) {
    gfx::TextureDesc td;
    td.width = dims.x; td.height = dims.y; td.data = source.data(); td.label = "ui-tex";
    Texture t;
    t.tex = dev_->createTexture(td);
    t.owned = true;
    if (!t.tex.valid()) return 0;
    return put(texs_, freeTexs_, t);
}

void UiRenderer::ReleaseTexture(Rml::TextureHandle handle) {
    if (!handle || handle > texs_.size()) return;
    Texture& t = texs_[handle - 1];
    if (t.owned && t.tex.valid()) dev_->destroyTexture(t.tex);
    t = {};
    freeTexs_.push_back(handle);
}

void UiRenderer::EnableScissorRegion(bool enable) {
    scissorOn_ = enable;
    if (!enable) dev_->setScissor({});
    else SetScissorRegion(scissor_);
}

void UiRenderer::SetScissorRegion(Rml::Rectanglei r) {
    scissor_ = r;
    if (!scissorOn_) return;
    // the RHI treats w == 0 as "disabled": keep empty regions clipping (at most one pixel shows)
    dev_->setScissor({r.Left(), r.Top(), std::max(r.Width(), 1), std::max(r.Height(), 1)});
}

void UiRenderer::SetTransform(const Rml::Matrix4f* t) {
    hasTransform_ = t != nullptr;
    if (t) std::memcpy(transform_, t->data(), sizeof transform_); // RmlUi default: column-major, like the RHI
}

} // namespace rts::ui
