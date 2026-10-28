#include "RtsFog.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace rts::game {

using namespace render;

namespace {
// GLSL ES 3.00 / MSL. U0: viewProj; color (rgb, a = unexplored opacity); p0 (explored opacity, map w,
// map h, style); grid (rgb, alpha); p1 (noise, time, grid everywhere, unused).
const char* kFogVS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; vec4 u_color; vec4 u_p0; vec4 u_grid; vec4 u_p1; };
layout(location = 0) in vec3 a_pos;
out vec3 v_wpos;
void main() { v_wpos = a_pos; gl_Position = u_viewProj * vec4(a_pos, 1.0); }
)";
const char* kFogFS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; vec4 u_color; vec4 u_p0; vec4 u_grid; vec4 u_p1; };
uniform sampler2D tex0;
in vec3 v_wpos;
out vec4 o_color;
highp float hash2(highp vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(highp vec2 p) {
    highp vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i), hash2(i + vec2(1.0, 0.0)), u.x), mix(hash2(i + vec2(0.0, 1.0)), hash2(i + vec2(1.0, 1.0)), u.x), u.y);
}
void main() {
    vec2 size = u_p0.yz;
    vec2 uv = v_wpos.xz / size;
    int style = int(u_p0.w + 0.5);
    if (style == 1 || style == 2) uv = (floor(v_wpos.xz) + 0.5) / size;     // hard cells
    float v = texture(tex0, uv).r;
    if (style == 0) v = smoothstep(0.0, 1.0, v);                             // softer ramp between the cells
    if (u_p1.x > 0.0 && v > 0.01 && v < 0.99) {                              // cloudy edges
        float n = vnoise(v_wpos.xz * 0.7 + vec2(u_p1.y * 0.05, u_p1.y * 0.03)) + 0.5 * vnoise(v_wpos.xz * 1.9 - u_p1.y * 0.04);
        v = clamp(v + (n / 1.5 - 0.5) * u_p1.x * 0.6, 0.0, 1.0);
    }
    float a = v < 0.5 ? mix(u_color.a, u_p0.x, v * 2.0) : mix(u_p0.x, 0.0, (v - 0.5) * 2.0);
    if (style == 3) {                                                          // dithered edge, classic look
        ivec2 q = ivec2(mod(gl_FragCoord.xy, 4.0));
        int b = (q.x & 1) * 8 + (q.y & 1) * 4 + (q.x & 2) + (q.y & 2) / 2;
        float t = (float(b) + 0.5) / 16.0;
        a = (a / max(u_color.a, 0.001) > t) ? u_color.a : (a > u_p0.x * 0.5 && v < 0.75 ? u_p0.x : 0.0);
    }
    vec3 c = u_color.rgb;
    float g = 0.0;
    if (u_grid.a > 0.0 && (u_p1.z > 0.5 || a > 0.01)) {
        vec2 fw = fract(v_wpos.xz);
        float d = min(min(fw.x, 1.0 - fw.x), min(fw.y, 1.0 - fw.y));
        g = u_grid.a * (1.0 - smoothstep(0.015, 0.05, d));
    }
    if (style == 2 && a > 0.01) g = max(g, 0.18 * (1.0 - smoothstep(0.015, 0.05, min(min(fract(v_wpos.x), 1.0 - fract(v_wpos.x)), min(fract(v_wpos.z), 1.0 - fract(v_wpos.z))))));
    vec3 col = mix(c, u_grid.rgb, g / max(a + g, 0.001));
    float alpha = max(a, g);
    if (alpha < 0.003) discard;
    o_color = vec4(col, alpha);
}
)";
const char* kFogMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct U0 { float4x4 viewProj; float4 color; float4 p0; float4 grid; float4 p1; };
struct VIn { float3 pos [[attribute(0)]]; };
struct VOut { float4 pos [[position]]; float3 wpos; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) { VOut o; o.wpos = in.pos; o.pos = u.viewProj * float4(in.pos, 1.0); return o; }
static float hash2(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
static float vnoise(float2 p) {
    float2 i = floor(p), f = fract(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i), hash2(i + float2(1.0, 0.0)), u.x), mix(hash2(i + float2(0.0, 1.0)), hash2(i + float2(1.0, 1.0)), u.x), u.y);
}
fragment float4 fs_main(VOut in [[stage_in]], constant U0& u [[buffer(1)]], texture2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    float2 size = u.p0.yz;
    float2 uv = in.wpos.xz / size;
    int style = int(u.p0.w + 0.5);
    if (style == 1 || style == 2) uv = (floor(in.wpos.xz) + 0.5) / size;
    float v = tex0.sample(s0, uv).r;
    if (style == 0) v = smoothstep(0.0, 1.0, v);
    if (u.p1.x > 0.0 && v > 0.01 && v < 0.99) {
        float n = vnoise(in.wpos.xz * 0.7 + float2(u.p1.y * 0.05, u.p1.y * 0.03)) + 0.5 * vnoise(in.wpos.xz * 1.9 - u.p1.y * 0.04);
        v = clamp(v + (n / 1.5 - 0.5) * u.p1.x * 0.6, 0.0, 1.0);
    }
    float a = v < 0.5 ? mix(u.color.a, u.p0.x, v * 2.0) : mix(u.p0.x, 0.0, (v - 0.5) * 2.0);
    if (style == 3) {
        int2 q = int2(fmod(in.pos.xy, 4.0));
        int b = (q.x & 1) * 8 + (q.y & 1) * 4 + (q.x & 2) + (q.y & 2) / 2;
        float t = (float(b) + 0.5) / 16.0;
        a = (a / max(u.color.a, 0.001) > t) ? u.color.a : (a > u.p0.x * 0.5 && v < 0.75 ? u.p0.x : 0.0);
    }
    float3 c = u.color.rgb;
    float g = 0.0;
    if (u.grid.a > 0.0 && (u.p1.z > 0.5 || a > 0.01)) {
        float2 fw = fract(in.wpos.xz);
        float d = min(min(fw.x, 1.0 - fw.x), min(fw.y, 1.0 - fw.y));
        g = u.grid.a * (1.0 - smoothstep(0.015, 0.05, d));
    }
    if (style == 2 && a > 0.01) {
        float2 fw = fract(in.wpos.xz);
        g = max(g, 0.18 * (1.0 - smoothstep(0.015, 0.05, min(min(fw.x, 1.0 - fw.x), min(fw.y, 1.0 - fw.y)))));
    }
    float3 col = mix(c, u.grid.rgb, g / max(a + g, 0.001));
    float alpha = max(a, g);
    if (alpha < 0.003) discard_fragment();
    return float4(col, alpha);
}
)";

struct Uniforms { float viewProj[16]; float color[4]; float p0[4]; float grid[4]; float p1[4]; };

void readColor(const Json& c, float (&out)[3]) {
    if (c.size() < 3) return;
    for (int i = 0; i < 3; ++i) {
        double v = c[size_t(i)].toDouble();
        out[i] = float(v > 1.0 ? v / 255.0 : v);       // [8, 10, 14] or [0.03, 0.04, 0.05]
    }
}
void apply(FogLook& L, const Json& j) {
    if (!j.isObject()) return;
    if (j.has("show")) L.show = j.getBool("show", true);
    if (j.has("mode") && (j.getStr("mode") == "none" || j.getStr("mode") == "off")) L.show = false;
    readColor(j["color"], L.color);
    if (j.has("unexplored")) L.unexplored = std::clamp(float(j["unexplored"].toDouble()), 0.0f, 1.0f);
    if (j.has("explored")) L.explored = std::clamp(float(j["explored"].toDouble()), 0.0f, 1.0f);
    if (j.has("style")) {
        std::string s = j.getStr("style");
        L.style = s == "cells" ? FogLook::Cells : s == "grid" ? FogLook::Grid : s == "pattern" ? FogLook::Pattern : FogLook::Smooth;
    }
    if (j.has("softness")) L.softness = std::clamp(int(j["softness"].integer()), 0, 6);
    if (j.has("fade")) L.fade = std::clamp(float(j["fade"].toDouble()), 0.0f, 5.0f);
    if (j.has("noise")) L.noise = std::clamp(float(j["noise"].toDouble()), 0.0f, 1.0f);
    if (j.has("height")) L.height = float(j["height"].toDouble());
    const Json& g = j["grid"];
    if (g.isObject()) {
        std::string show = g.getStr("show", "fog");
        L.gridEverywhere = show == "all" || show == "always";
        L.gridAlpha = show == "none" ? 0.0f : std::clamp(float(g["alpha"].toDouble(0.12)), 0.0f, 1.0f);
        readColor(g["color"], L.gridColor);
    } else if (!g.isNull()) {
        L.gridAlpha = g.boolean(false) ? 0.12f : 0.0f;   // "grid": true
    }
}

// Box blur of radius r over a byte image (separable, integer: same result everywhere).
void boxBlur(std::vector<uint8_t>& img, std::vector<uint16_t>& tmp, int w, int h, int r) {
    if (r <= 0) return;
    tmp.assign(img.size(), 0);
    const int n = 2 * r + 1;
    for (int y = 0; y < h; ++y) {
        int sum = 0;
        const uint8_t* row = &img[size_t(y) * size_t(w)];
        for (int x = -r; x <= r; ++x) sum += row[std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            tmp[size_t(y) * size_t(w) + size_t(x)] = uint16_t(sum);
            sum += row[std::min(x + r + 1, w - 1)] - row[std::max(x - r, 0)];
        }
    }
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int y = -r; y <= r; ++y) sum += tmp[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(x)];
        for (int y = 0; y < h; ++y) {
            img[size_t(y) * size_t(w) + size_t(x)] = uint8_t((sum + n * n / 2) / (n * n));
            sum += tmp[size_t(std::min(y + r + 1, h - 1)) * size_t(w) + size_t(x)] - tmp[size_t(std::max(y - r, 0)) * size_t(w) + size_t(x)];
        }
    }
}
} // namespace

FogLook parseFogLook(const Json& rulesFog, const Json& gameFog) {
    FogLook L;
    apply(L, rulesFog);
    apply(L, gameFog);
    return L;
}

bool FogOfWar::init(gfx::Device& dev) {
    dev_ = &dev;
    gfx::ShaderDesc sd;
    sd.glslVertex = kFogVS; sd.glslFragment = kFogFS; sd.msl = kFogMSL;
    sd.textureSlots = 1; sd.uniformSlots = 1; sd.label = "fog of war";
    shader_ = dev.createShader(sd);
    if (!shader_.valid()) return false;
    gfx::PipelineDesc pd;
    pd.shader = shader_;
    pd.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
    pd.attrCount = 1; pd.stride = 12;
    pd.indexType = gfx::IndexType::UInt32;
    pd.cull = gfx::CullMode::None;
    pd.blend = gfx::BlendMode::Alpha;
    pd.depthTest = true;
    pd.depthWrite = false;
    pd.label = "fog of war";
    pipe_ = dev.createPipeline(pd);
    return pipe_.valid();
}

void FogOfWar::shutdown() {
    if (!dev_) return;
    for (auto* b : {&vb_, &ib_}) if (b->valid()) { dev_->destroyBuffer(*b); *b = {}; }
    if (tex_.valid()) { dev_->destroyTexture(tex_); tex_ = {}; }
    if (pipe_.valid()) dev_->destroyPipeline(pipe_);
    if (shader_.valid()) dev_->destroyShader(shader_);
    pipe_ = {}; shader_ = {};
    dev_ = nullptr;
    w_ = h_ = 0;
}

void FogOfWar::build(gfx::Device& dev, int w, int h, const std::function<float(float, float)>& height) {
    if (!dev_ || w <= 0 || h <= 0) return;
    for (auto* b : {&vb_, &ib_}) if (b->valid()) { dev.destroyBuffer(*b); *b = {}; }
    // draped sheet: one vertex per cell corner at the highest neighbouring cell centre, so plateaus
    // never poke through; coarser on big maps (the texture carries the detail)
    const int step = std::max(1, std::max(w, h) / 256);
    const int cw = (w + step - 1) / step, ch = (h + step - 1) / step;
    std::vector<float> v;
    v.reserve(size_t(cw + 1) * size_t(ch + 1) * 3);
    for (int j = 0; j <= ch; ++j)
        for (int i = 0; i <= cw; ++i) {
            const float x = float(std::min(i * step, w)), z = float(std::min(j * step, h));
            float g = -1e9f;
            for (int dz = -1; dz <= 0; ++dz)
                for (int dx = -1; dx <= 0; ++dx) {
                    float cx = std::clamp(x + dx * float(step) + float(step) * 0.5f, 0.5f, float(w) - 0.5f);
                    float cz = std::clamp(z + dz * float(step) + float(step) * 0.5f, 0.5f, float(h) - 0.5f);
                    g = std::max(g, height(cx, cz));
                }
            v.push_back(x); v.push_back(g + look_.height); v.push_back(z);
        }
    std::vector<uint32_t> idx;
    idx.reserve(size_t(cw) * size_t(ch) * 6);
    for (int j = 0; j < ch; ++j)
        for (int i = 0; i < cw; ++i) {
            uint32_t a = uint32_t(j * (cw + 1) + i), b = a + 1, c = a + uint32_t(cw + 1), d = c + 1;
            idx.insert(idx.end(), {a, c, d, a, d, b});
        }
    gfx::BufferDesc vb; vb.size = v.size() * sizeof(float); vb.data = v.data(); vb.label = "fog sheet";
    vb_ = dev.createBuffer(vb);
    gfx::BufferDesc ib; ib.type = gfx::BufferType::Index; ib.size = idx.size() * sizeof(uint32_t); ib.data = idx.data(); ib.label = "fog sheet idx";
    ib_ = dev.createBuffer(ib);
    indexCount_ = uint32_t(idx.size());
    if (w != w_ || h != h_ || !tex_.valid()) {
        if (tex_.valid()) dev.destroyTexture(tex_);
        w_ = w; h_ = h;
        target_.assign(size_t(w) * size_t(h), 0);
        cur_ = target_; field_ = target_;
        gfx::TextureDesc td;
        td.width = w; td.height = h; td.format = gfx::PixelFormat::R8; td.filter = gfx::Filter::Linear; td.data = field_.data(); td.label = "fog of war";
        tex_ = dev.createTexture(td);
        lastTime_ = -1;
        dirty_ = true;
    }
}

bool FogOfWar::update(const std::vector<uint8_t>& vision, int w, int h, double time) {
    if (w != w_ || h != h_ || vision.size() != target_.size()) return false;
    const float dt = lastTime_ < 0 ? 1e6f : float(std::max(0.0, time - lastTime_));
    lastTime_ = time;
    bool changed = dirty_;
    for (size_t i = 0; i < vision.size(); ++i) {
        uint8_t t = vision[i] >= 2 ? 255 : vision[i] == 1 ? 128 : 0;
        if (target_[i] != t) { target_[i] = t; changed = true; fading_ = true; }
    }
    if (fading_ || dirty_) {
        // fade every cell towards its target at 255 / fade per second
        const int stepPx = look_.fade <= 0 ? 255 : std::max(1, int(dt * 255.0f / look_.fade));
        bool any = false;
        for (size_t i = 0; i < cur_.size(); ++i) {
            int c = cur_[i], t = target_[i];
            if (c == t) continue;
            c = c < t ? std::min(t, c + stepPx) : std::max(t, c - stepPx);
            cur_[i] = uint8_t(c);
            any = any || c != t;
            changed = true;
        }
        fading_ = any;
    }
    if (!changed) return false;
    field_ = cur_;
    if (look_.style == FogLook::Smooth || look_.style == FogLook::Pattern) boxBlur(field_, tmp_, w_, h_, look_.softness);
    dirty_ = false;
    upload();
    return true;
}

void FogOfWar::upload() {
    if (dev_ && tex_.valid()) dev_->updateTexture(tex_, 0, 0, w_, h_, field_.data(), w_);
}

void FogOfWar::draw(Renderer3D& r, double time, bool lowTier) {
    if (!dev_ || !pipe_.valid() || !vb_.valid() || !tex_.valid() || !look_.show) return;
    Uniforms u{};
    std::memcpy(u.viewProj, r.viewProj().m, sizeof u.viewProj);
    u.color[0] = look_.color[0]; u.color[1] = look_.color[1]; u.color[2] = look_.color[2]; u.color[3] = look_.unexplored;
    const int style = lowTier && look_.style == FogLook::Smooth ? FogLook::Cells : look_.style;
    u.p0[0] = look_.explored; u.p0[1] = float(w_); u.p0[2] = float(h_); u.p0[3] = float(style);
    u.grid[0] = look_.gridColor[0]; u.grid[1] = look_.gridColor[1]; u.grid[2] = look_.gridColor[2]; u.grid[3] = look_.gridAlpha;
    u.p1[0] = lowTier ? 0.0f : look_.noise; u.p1[1] = float(std::fmod(time, 10000.0)); u.p1[2] = look_.gridEverywhere ? 1.0f : 0.0f;
    gfx::PipelineHandle pipe = pipe_;
    gfx::BufferHandle vb = vb_, ib = ib_;
    gfx::TextureHandle tex = tex_;
    const uint32_t count = indexCount_;
    r.drawCustom([pipe, vb, ib, tex, count, u](gfx::Device& dev) {
        dev.setPipeline(pipe);
        dev.setUniforms(0, &u, sizeof u);
        dev.setTexture(0, tex);
        dev.setVertexBuffer(vb);
        dev.setIndexBuffer(ib);
        dev.drawIndexed(count, 0, 0);
    });
}

} // namespace rts::game
