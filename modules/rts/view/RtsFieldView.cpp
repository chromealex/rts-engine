#include "RtsFieldView.h"
#include "RtsView.h"
#include "../RtsMap.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rts::game {

using namespace render;

namespace {
// U0: viewProj; p0 (world w, world h, y offset, time); p1 (animate, speed, per player, cells); c[4] ramp colours
// (rgb, alpha); stops (ramp positions 0..1); p2 (soft edge, brightness, additive, unused).
const char* kFieldVS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; vec4 u_p0; vec4 u_p1; vec4 u_c0; vec4 u_c1; vec4 u_c2; vec4 u_c3; vec4 u_stops; vec4 u_p2; };
layout(location = 0) in vec3 a_pos;
out vec3 v_wpos;
void main() { vec3 p = a_pos + vec3(0.0, u_p0.z, 0.0); v_wpos = p; gl_Position = u_viewProj * vec4(p, 1.0); }
)";
const char* kFieldFS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; vec4 u_p0; vec4 u_p1; vec4 u_c0; vec4 u_c1; vec4 u_c2; vec4 u_c3; vec4 u_stops; vec4 u_p2; };
uniform sampler2D tex0;
in vec3 v_wpos;
out vec4 o_color;
highp float hash2(highp vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(highp vec2 p) {
    highp vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i), hash2(i + vec2(1.0, 0.0)), u.x), mix(hash2(i + vec2(0.0, 1.0)), hash2(i + vec2(1.0, 1.0)), u.x), u.y);
}
vec4 ramp(float n) {
    if (n <= u_stops.x) return u_c0;
    if (n <= u_stops.y) return mix(u_c0, u_c1, (n - u_stops.x) / max(u_stops.y - u_stops.x, 0.0001));
    if (n <= u_stops.z) return mix(u_c1, u_c2, (n - u_stops.y) / max(u_stops.z - u_stops.y, 0.0001));
    if (n <= u_stops.w) return mix(u_c2, u_c3, (n - u_stops.z) / max(u_stops.w - u_stops.z, 0.0001));
    return u_c3;
}
void main() {
    vec2 uv = v_wpos.xz / u_p0.xy;
    if (u_p1.w > 0.5) uv = (floor(v_wpos.xz) + 0.5) / u_p0.xy;
    vec4 t = texture(tex0, uv);
    float v = t.a;
    if (v < 0.004) discard;
    float n = v;
    float tm = u_p0.w * u_p1.y;
    int anim = int(u_p1.x + 0.5);
    float shimmer = 0.0;
    if (anim == 1) {
        float f = vnoise(v_wpos.xz * 1.7 + vec2(0.0, -tm * 1.3)) * 0.65 + vnoise(v_wpos.xz * 4.3 + vec2(tm * 0.7, -tm * 2.4)) * 0.35;
        n = clamp(n * (0.6 + 0.8 * f), 0.0, 1.0);
    } else if (anim == 2) {
        float f = vnoise(v_wpos.xz * 1.3 + vec2(tm * 0.35, tm * 0.2)) + 0.5 * vnoise(v_wpos.xz * 2.9 - vec2(tm * 0.3, tm * 0.45));
        shimmer = (f / 1.5 - 0.5) * 0.22;
    } else if (anim == 3) {
        n = n * (0.85 + 0.15 * sin(tm * 2.5 + v_wpos.x * 0.21 + v_wpos.z * 0.17));
    } else if (anim == 4) {
        n = clamp(n * (0.55 + 0.9 * vnoise(v_wpos.xz * 0.8 + vec2(tm * 0.25, tm * 0.1))), 0.0, 1.0);
    }
    vec4 c = ramp(n);
    if (u_p1.z > 0.5) c.rgb = t.rgb * (0.55 + 0.6 * n);
    c.rgb = max(c.rgb + vec3(shimmer), vec3(0.0)) * u_p2.y;
    float a = c.a * smoothstep(0.0, u_p2.x, v);
    if (a < 0.003) discard;
    if (u_p2.z > 0.5) o_color = vec4(c.rgb * a, a);
    else o_color = vec4(c.rgb, a);
}
)";
const char* kFieldMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct U0 { float4x4 viewProj; float4 p0; float4 p1; float4 c0; float4 c1; float4 c2; float4 c3; float4 stops; float4 p2; };
struct VIn { float3 pos [[attribute(0)]]; };
struct VOut { float4 pos [[position]]; float3 wpos; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) {
    VOut o; float3 p = in.pos + float3(0.0, u.p0.z, 0.0); o.wpos = p; o.pos = u.viewProj * float4(p, 1.0); return o;
}
static float hash2(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
static float vnoise(float2 p) {
    float2 i = floor(p), f = fract(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i), hash2(i + float2(1.0, 0.0)), u.x), mix(hash2(i + float2(0.0, 1.0)), hash2(i + float2(1.0, 1.0)), u.x), u.y);
}
static float4 ramp(constant U0& u, float n) {
    if (n <= u.stops.x) return u.c0;
    if (n <= u.stops.y) return mix(u.c0, u.c1, (n - u.stops.x) / max(u.stops.y - u.stops.x, 0.0001));
    if (n <= u.stops.z) return mix(u.c1, u.c2, (n - u.stops.y) / max(u.stops.z - u.stops.y, 0.0001));
    if (n <= u.stops.w) return mix(u.c2, u.c3, (n - u.stops.z) / max(u.stops.w - u.stops.z, 0.0001));
    return u.c3;
}
fragment float4 fs_main(VOut in [[stage_in]], constant U0& u [[buffer(1)]], texture2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    float2 uv = in.wpos.xz / u.p0.xy;
    if (u.p1.w > 0.5) uv = (floor(in.wpos.xz) + 0.5) / u.p0.xy;
    float4 t = tex0.sample(s0, uv);
    float v = t.a;
    if (v < 0.004) discard_fragment();
    float n = v;
    float tm = u.p0.w * u.p1.y;
    int anim = int(u.p1.x + 0.5);
    float shimmer = 0.0;
    if (anim == 1) {
        float f = vnoise(in.wpos.xz * 1.7 + float2(0.0, -tm * 1.3)) * 0.65 + vnoise(in.wpos.xz * 4.3 + float2(tm * 0.7, -tm * 2.4)) * 0.35;
        n = clamp(n * (0.6 + 0.8 * f), 0.0, 1.0);
    } else if (anim == 2) {
        float f = vnoise(in.wpos.xz * 1.3 + float2(tm * 0.35, tm * 0.2)) + 0.5 * vnoise(in.wpos.xz * 2.9 - float2(tm * 0.3, tm * 0.45));
        shimmer = (f / 1.5 - 0.5) * 0.22;
    } else if (anim == 3) {
        n = n * (0.85 + 0.15 * sin(tm * 2.5 + in.wpos.x * 0.21 + in.wpos.z * 0.17));
    } else if (anim == 4) {
        n = clamp(n * (0.55 + 0.9 * vnoise(in.wpos.xz * 0.8 + float2(tm * 0.25, tm * 0.1))), 0.0, 1.0);
    }
    float4 c = ramp(u, n);
    if (u.p1.z > 0.5) c.rgb = t.rgb * (0.55 + 0.6 * n);
    c.rgb = max(c.rgb + float3(shimmer), float3(0.0)) * u.p2.y;
    float a = c.a * smoothstep(0.0, u.p2.x, v);
    if (a < 0.003) discard_fragment();
    if (u.p2.z > 0.5) return float4(c.rgb * a, a);
    return float4(c.rgb, a);
}
)";

struct Uniforms { float viewProj[16]; float p0[4], p1[4], c[4][4], stops[4], p2[4]; };

bool hexColor(const Json& j, float out[3]) {
    if (j.isArray() && j.size() >= 3) {
        for (int i = 0; i < 3; ++i) { double v = j[size_t(i)].toDouble(); out[i] = float(v > 1.0 ? v / 255.0 : v); }
        return true;
    }
    const std::string s = j.str();
    if (s.size() == 7 && s[0] == '#') {
        const unsigned v = unsigned(std::strtoul(s.c_str() + 1, nullptr, 16));
        out[0] = float((v >> 16) & 255) / 255.0f; out[1] = float((v >> 8) & 255) / 255.0f; out[2] = float(v & 255) / 255.0f;
        return true;
    }
    return false;
}
void setStops(RtsFieldView::Look& L, std::initializer_list<std::array<float, 5>> s) {
    int i = 0;
    for (auto& st : s) {
        if (i >= 4) break;
        L.stops[i] = st[0];
        for (int k = 0; k < 4; ++k) L.color[i][k] = st[size_t(k) + 1];
        ++i;
    }
    for (; i < 4; ++i) { L.stops[i] = 1; std::memcpy(L.color[i], L.color[i - 1], sizeof L.color[i]); }
}
double num(const Json& j, double def) { return j.isNumber() ? j.toDouble(def) : def; }
} // namespace

RtsFieldView::Look RtsFieldView::parseLook(const GridFieldDef& d) {
    Look L;
    const Json& v = d.view;
    auto toUnits = [&](int32_t raw) { return d.fixed ? double(raw) / 65536.0 : double(raw); };
    const bool bounded = d.max != INT32_MAX && d.min != INT32_MIN;
    L.lo = bounded ? std::max(toUnits(d.min), toUnits(d.def)) : toUnits(d.def);
    L.hi = bounded ? toUnits(d.max) : L.lo + (d.fixed ? 1.0 : 100.0);
    L.threshold = L.lo + (d.fixed ? 0.01 : 1.0);
    const std::string preset = v.getStr("preset", d.perPlayer ? "creep" : "heat");
    if (preset == "fire") {
        setStops(L, {{{0.0f, 0.35f, 0.05f, 0.0f, 0.35f}}, {{0.35f, 0.95f, 0.30f, 0.04f, 0.75f}}, {{0.75f, 1.0f, 0.62f, 0.15f, 0.9f}}, {{1.0f, 1.0f, 0.93f, 0.6f, 0.95f}}});
        L.animate = 1; L.additive = true;
        L.particleAbove = L.threshold;
    } else if (preset == "lava") {
        setStops(L, {{{0.0f, 0.15f, 0.03f, 0.02f, 0.8f}}, {{0.4f, 0.75f, 0.12f, 0.02f, 0.92f}}, {{0.8f, 1.0f, 0.45f, 0.05f, 0.96f}}, {{1.0f, 1.0f, 0.85f, 0.35f, 1.0f}}});
        L.animate = 1; L.speed = 0.3f;
    } else if (preset == "water") {
        setStops(L, {{{0.0f, 0.25f, 0.5f, 0.75f, 0.35f}}, {{0.25f, 0.18f, 0.42f, 0.72f, 0.6f}}, {{1.0f, 0.06f, 0.18f, 0.45f, 0.85f}}});
        L.animate = 2;
    } else if (preset == "gas") {
        setStops(L, {{{0.0f, 0.45f, 0.75f, 0.2f, 0.1f}}, {{1.0f, 0.55f, 0.85f, 0.15f, 0.6f}}});
        L.animate = 4; L.height = 0.5f;
    } else if (preset == "creep") {
        setStops(L, {{{0.0f, 0.45f, 0.2f, 0.55f, 0.25f}}, {{1.0f, 0.55f, 0.25f, 0.65f, 0.55f}}});
        L.animate = 3;
    } else {   // heat
        setStops(L, {{{0.0f, 0.1f, 0.2f, 0.9f, 0.3f}}, {{0.5f, 0.95f, 0.85f, 0.2f, 0.55f}}, {{1.0f, 0.95f, 0.15f, 0.1f, 0.7f}}});
    }
    L.show = v.getBool("show", true);
    if (const Json& r = v["range"]; r.isArray() && r.size() == 2) { L.lo = r[size_t(0)].toDouble(L.lo); L.hi = r[size_t(1)].toDouble(L.hi); }
    if (L.hi <= L.lo) L.hi = L.lo + 1;
    L.threshold = num(v["threshold"], L.threshold);
    if (const Json& cs = v["colors"]; cs.isArray() && cs.size() > 0) {
        int i = 0;
        for (auto& c : cs.items()) {
            if (i >= 4 || !c.isArray() || c.size() < 2) continue;
            L.stops[i] = float(std::clamp((c[size_t(0)].toDouble() - L.lo) / (L.hi - L.lo), 0.0, 1.0));
            float rgb[3] = {1, 1, 1};
            hexColor(c[size_t(1)], rgb);
            L.color[i][0] = rgb[0]; L.color[i][1] = rgb[1]; L.color[i][2] = rgb[2];
            L.color[i][3] = c.size() > 2 ? float(c[size_t(2)].toDouble(0.7)) : 0.7f;
            ++i;
        }
        for (; i < 4; ++i) { L.stops[i] = 1; std::memcpy(L.color[i], L.color[i - 1], sizeof L.color[i]); }
    }
    if (v.has("blend")) L.additive = v.getStr("blend") == "add";
    if (v.has("animate")) {
        const std::string a = v.getStr("animate");
        L.animate = a == "fire" ? 1 : a == "water" ? 2 : a == "pulse" ? 3 : a == "drift" ? 4 : 0;
    }
    L.speed = float(num(v["speed"], L.speed));
    L.height = float(num(v["height"], L.height));
    L.cells = v.getBool("cells", false);
    if (v.has("fog")) { const std::string f = v.getStr("fog"); L.fog = f == "none" ? 0 : f == "visible" ? 2 : 1; }
    if (const Json& p = v["particles"]; p.isObject()) {
        L.particle = p.getStr("effect");
        L.particleAbove = num(p["above"], L.threshold);
        L.particleMax = std::clamp(int(p.getInt("max", 24)), 0, 256);
        L.particleEvery = float(std::max(0.1, num(p["every"], 1.5)));
    }
    return L;
}

bool RtsFieldView::init(gfx::Device& dev) {
    dev_ = &dev;
    gfx::ShaderDesc sd;
    sd.glslVertex = kFieldVS; sd.glslFragment = kFieldFS; sd.msl = kFieldMSL;
    sd.textureSlots = 1; sd.uniformSlots = 1; sd.label = "grid field";
    shader_ = dev.createShader(sd);
    if (!shader_.valid()) return false;
    gfx::PipelineDesc pd;
    pd.shader = shader_;
    pd.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
    pd.attrCount = 1; pd.stride = 12;
    pd.indexType = gfx::IndexType::UInt32;
    pd.cull = gfx::CullMode::None;
    pd.depthTest = true;
    pd.depthWrite = false;
    pd.blend = gfx::BlendMode::Alpha;
    pd.label = "grid field";
    alpha_ = dev.createPipeline(pd);
    pd.blend = gfx::BlendMode::Premultiplied;   // shader premultiplies; one + (1 - a) dst keeps a dark base
    pd.label = "grid field add";
    add_ = dev.createPipeline(pd);
    return alpha_.valid();
}

void RtsFieldView::shutdown() {
    if (!dev_) return;
    for (auto* b : {&vb_, &ib_}) if (b->valid()) { dev_->destroyBuffer(*b); *b = {}; }
    for (auto& t : tex_) if (t.tex.valid()) dev_->destroyTexture(t.tex);
    tex_.clear();
    for (auto* p : {&alpha_, &add_}) if (p->valid()) { dev_->destroyPipeline(*p); *p = {}; }
    if (shader_.valid()) { dev_->destroyShader(shader_); shader_ = {}; }
    dev_ = nullptr;
    meshW_ = meshH_ = 0;
}

void RtsFieldView::setFields(const std::vector<GridFieldDef>& defs) {
    looks_.clear();
    fixed_.clear();
    for (auto& d : defs) { looks_.push_back(parseLook(d)); fixed_.push_back(d.fixed); }
    if (dev_) for (auto& t : tex_) if (t.tex.valid()) dev_->destroyTexture(t.tex);
    tex_.assign(defs.size(), {});
}

void RtsFieldView::buildMesh(const MapGrid& g, const RtsView& view) {
    for (auto* b : {&vb_, &ib_}) if (b->valid()) { dev_->destroyBuffer(*b); *b = {}; }
    const int w = g.w, h = g.h;
    const int step = std::max(1, std::max(w, h) / 256);
    const int cw = (w + step - 1) / step, ch = (h + step - 1) / step;
    std::vector<float> v;
    v.reserve(size_t(cw + 1) * size_t(ch + 1) * 3);
    for (int j = 0; j <= ch; ++j)
        for (int i = 0; i <= cw; ++i) {
            const float x = float(std::min(i * step, w)), z = float(std::min(j * step, h));
            float gy = -1e9f;   // highest neighbouring cell centre: the sheet never dips under a plateau
            for (int dz = -1; dz <= 0; ++dz)
                for (int dx = -1; dx <= 0; ++dx) {
                    const float cx = std::clamp(x + float(dx * step) + float(step) * 0.5f, 0.5f, float(w) - 0.5f);
                    const float cz = std::clamp(z + float(dz * step) + float(step) * 0.5f, 0.5f, float(h) - 0.5f);
                    gy = std::max(gy, view.heightAt(g, cx, cz));
                }
            v.push_back(x); v.push_back(gy); v.push_back(z);
        }
    std::vector<uint32_t> idx;
    idx.reserve(size_t(cw) * size_t(ch) * 6);
    for (int j = 0; j < ch; ++j)
        for (int i = 0; i < cw; ++i) {
            const uint32_t a = uint32_t(j * (cw + 1) + i), b = a + 1, c = a + uint32_t(cw + 1), d = c + 1;
            idx.insert(idx.end(), {a, c, d, a, d, b});
        }
    gfx::BufferDesc vb; vb.size = v.size() * sizeof(float); vb.data = v.data(); vb.label = "field sheet";
    vb_ = dev_->createBuffer(vb);
    gfx::BufferDesc ib; ib.type = gfx::BufferType::Index; ib.size = idx.size() * sizeof(uint32_t); ib.data = idx.data(); ib.label = "field sheet idx";
    ib_ = dev_->createBuffer(ib);
    indexCount_ = uint32_t(idx.size());
    meshW_ = w; meshH_ = h;
    meshDirty_ = false;
}

void RtsFieldView::upload(Tex& t, const FieldSceneLayer& L, const Look& look, const std::vector<uint8_t>* vision, const MapGrid& g, bool low, uint8_t viewer) {
    const int s = low ? 2 : 1;
    const int tw = (L.w + s - 1) / s, th = (L.h + s - 1) / s;
    if (!t.tex.valid() || t.w != tw || t.h != th) {
        if (t.tex.valid()) dev_->destroyTexture(t.tex);
        t.px.assign(size_t(tw) * size_t(th) * 4, 0);
        gfx::TextureDesc td;
        td.width = tw; td.height = th; td.format = gfx::PixelFormat::RGBA8; td.filter = look.cells ? gfx::Filter::Nearest : gfx::Filter::Linear;
        td.data = t.px.data(); td.label = "grid field";
        t.tex = dev_->createTexture(td);
        t.w = tw; t.h = th;
    }
    const size_t cells = size_t(L.w) * size_t(L.h);
    const double range = look.hi - look.lo;
    std::fill(t.px.begin(), t.px.end(), uint8_t(0));
    for (int y = 0; y < L.h; ++y)
        for (int x = 0; x < L.w; ++x) {
            const size_t c = size_t(y * L.w + x);
            // fog of war: the cell's map cells (top-left one) must be explored / visible for the viewer
            if (vision && look.fog > 0) {
                const int mx = std::min(g.w - 1, x * L.res), my = std::min(g.h - 1, y * L.res);
                const size_t mi = size_t(my * g.w + mx);
                if (mi < vision->size() && (*vision)[mi] < (look.fog == 2 ? 2 : 1)) continue;
            }
            double best = -1e300; int owner = 0;
            for (int l = 0; l < L.layers; ++l) {
                const double v = double(L.v[size_t(l) * cells + c]) * unitScale_;
                if (v > best) { best = v; owner = l; }
            }
            if (best < look.threshold) continue;
            const uint8_t a = uint8_t(std::clamp((best - look.lo) / range, 1.0 / 255.0, 1.0) * 255.0 + 0.5);
            uint8_t* p = &t.px[(size_t(y / s) * size_t(tw) + size_t(x / s)) * 4];
            if (a <= p[3]) continue;
            if (L.layers > 1) {
                const float4 pc = RtsView::playerColor(owner);
                p[0] = uint8_t(pc.x * 255.0f); p[1] = uint8_t(pc.y * 255.0f); p[2] = uint8_t(pc.z * 255.0f);
            }
            p[3] = a;
        }
    (void)viewer;
    dev_->updateTexture(t.tex, 0, 0, tw, th, t.px.data(), tw * 4);
}

void RtsFieldView::draw(Renderer3D& r, const std::vector<FieldSceneLayer>& layers, const std::vector<uint8_t>* vision, const MapGrid& grid,
                        const RtsView& view, double time, bool lowTier, uint8_t viewer) {
    if (!dev_ || !alpha_.valid() || layers.empty()) return;
    if (const char* q = std::getenv("RTS_FIELDS")) lowTier = std::string(q) == "low";
    if (meshDirty_ || meshW_ != grid.w || meshH_ != grid.h) buildMesh(grid, view);
    for (const FieldSceneLayer& L : layers) {
        if (L.field < 0 || size_t(L.field) >= looks_.size() || !L.v) continue;
        const Look& look = looks_[size_t(L.field)];
        if (!look.show) continue;
        Tex& t = tex_[size_t(L.field)];
        unitScale_ = size_t(L.field) < fixed_.size() && fixed_[size_t(L.field)] ? 1.0 / 65536.0 : 1.0;   // raw -> field units
        // re-upload when the values changed (or, with fog masking, a few times per second for the vision)
        const bool fogTick = vision && look.fog > 0 && time - t.lastFog > 0.25;
        if (t.version != L.version || t.low != lowTier || fogTick || !t.tex.valid()) {
            upload(t, L, look, vision, grid, lowTier, viewer);
            t.version = L.version; t.low = lowTier; t.lastFog = time;
        }
        Uniforms u{};
        std::memcpy(u.viewProj, r.viewProj().m, sizeof u.viewProj);
        u.p0[0] = float(L.w * L.res); u.p0[1] = float(L.h * L.res); u.p0[2] = look.height; u.p0[3] = float(std::fmod(time, 3600.0));
        u.p1[0] = lowTier ? 0.0f : float(look.animate); u.p1[1] = look.speed; u.p1[2] = L.layers > 1 ? 1.0f : 0.0f; u.p1[3] = look.cells ? 1.0f : 0.0f;
        std::memcpy(u.c, look.color, sizeof u.c);
        std::memcpy(u.stops, look.stops, sizeof u.stops);
        u.p2[0] = 0.08f; u.p2[1] = 1.0f; u.p2[2] = look.additive ? 1.0f : 0.0f;
        gfx::PipelineHandle pipe = look.additive && add_.valid() ? add_ : alpha_;
        gfx::BufferHandle vb = vb_, ib = ib_;
        gfx::TextureHandle tex = t.tex;
        const uint32_t count = indexCount_;
        r.drawCustom([pipe, vb, ib, tex, count, u](gfx::Device& dev) {
            dev.setPipeline(pipe);
            dev.setUniforms(0, &u, sizeof u);
            dev.setTexture(0, tex);
            dev.setVertexBuffer(vb);
            dev.setIndexBuffer(ib);
            dev.drawIndexed(count, 0, 0);
        });
        // particles on hot visible cells: a bounded, rotating sample every `particleEvery` seconds
        if (lowTier || look.particle.empty() || look.particleMax <= 0 || time < t.nextFx) continue;
        t.nextFx = time + look.particleEvery;
        const size_t cells = size_t(L.w) * size_t(L.h);
        std::vector<size_t> hot;
        for (size_t c = 0; c < cells; ++c) {
            double best = -1e300;
            for (int l = 0; l < L.layers; ++l) best = std::max(best, double(L.v[size_t(l) * cells + c]) * unitScale_);
            if (best < look.particleAbove) continue;
            if (vision) {
                const int mx = std::min(grid.w - 1, int(c % size_t(L.w)) * L.res), my = std::min(grid.h - 1, int(c / size_t(L.w)) * L.res);
                if ((*vision)[size_t(my * grid.w + mx)] < 2) continue;
            }
            hot.push_back(c);
        }
        if (hot.empty()) continue;
        const size_t n = std::min(hot.size(), size_t(look.particleMax));
        const size_t stride = std::max<size_t>(1, hot.size() / n);
        t.fxSeed = t.fxSeed * 1664525u + 1013904223u;
        for (size_t k = 0; k < n; ++k) {
            const size_t c = hot[(size_t(t.fxSeed % 977u) + k * stride) % hot.size()];
            const float x = (float(c % size_t(L.w)) + 0.5f) * float(L.res), z = (float(c / size_t(L.w)) + 0.5f) * float(L.res);
            effects_.push_back({look.particle, {x, view.heightAt(grid, x, z), z}});
        }
        if (effects_.size() > 512) effects_.erase(effects_.begin(), effects_.begin() + ptrdiff_t(effects_.size() - 512));
    }
}

} // namespace rts::game
