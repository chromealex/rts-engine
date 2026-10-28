#include "PostFx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "PostFxShaders.h"

namespace rts::render {

namespace {

// Composite feature bits (also the shader cache key).
enum : uint32_t {
    kPfPixelate = 1u << 0, kPfCa = 1u << 1, kPfSharpen = 1u << 2, kPfTilt = 1u << 3, kPfBloom = 1u << 4,
    kPfShafts = 1u << 5, kPfGrade = 1u << 6, kPfLut = 1u << 7, kPfPosterize = 1u << 8, kPfVignette = 1u << 9,
    kPfGrain = 1u << 10, kPfFade = 1u << 11, kPfLetterbox = 1u << 12,
};
const char* kPfNames[] = {"PF_PIXELATE", "PF_CA", "PF_SHARPEN", "PF_TILT", "PF_BLOOM", "PF_SHAFTS", "PF_GRADE",
                          "PF_LUT", "PF_POSTERIZE", "PF_VIGNETTE", "PF_GRAIN", "PF_FADE", "PF_LETTERBOX"};

// Target roles inside one Sized set.
enum Role { kScene = 0, kAa = 1, kBloomDown = 10, kBloomUp = 20, kTilt0 = 30, kTilt1 = 31, kShaft0 = 40, kShaft1 = 41 };

struct SimpleU { float px[4]; float p0[4]; float sun[4]; };
struct CompositeU {
    float px[4], grade0[4], grade1[4], lift[4], gamma[4], gain[4], filt[4], vig[4], bloom[4], shafts[4], tilt[4], fade[4], lut[4], lbox[4];
};
const char* kSimpleFields[] = {"px", "p0", "sun"};
const char* kCompositeFields[] = {"px", "grade0", "grade1", "lift", "gamma", "gain", "filt", "vig", "bloom", "shafts", "tilt", "fade", "lut", "lbox"};

struct Built { std::string vs, fs, msl; };

// Wraps a common-subset body (PostFxShaders.h) into GLSL ES 3.00 and MSL.
Built buildProgram(const char* const* fields, size_t fieldCount, uint32_t texMask, const std::string& defines,
                   const char* helpers, const char* body) {
    Built b;
    b.vs = "layout(location = 0) in vec2 a_pos;\nout vec2 v_screen;\n"
           "void main() { v_screen = a_pos * 0.5 + 0.5; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    std::string block = "layout(std140) uniform U0 {";
    std::string mstruct = "struct U0 {", mlocals;
    for (size_t i = 0; i < fieldCount; ++i) {
        block += std::string(" vec4 ") + fields[i] + ";";
        mstruct += std::string(" float4 ") + fields[i] + ";";
        mlocals += std::string("    const float4 ") + fields[i] + " = u." + fields[i] + ";\n";
    }
    block += " };\n";
    mstruct += " };\n";
    const std::string common = "#define TUV(p) (px.z > 0.5 ? vec2((p).x, 1.0 - (p).y) : (p))\n"
                               "#define TDIR(d) (px.z > 0.5 ? vec2((d).x, -(d).y) : (d))\n";
    std::string gTex, mArgs, gMacros, mMacros;
    for (int t = 0; t < 5; ++t) {
        if (!(texMask & (1u << t))) continue;
        const std::string n = std::to_string(t);
        gTex += "uniform sampler2D tex" + n + ";\n";
        gMacros += "#define TEX" + n + "(p) texture(tex" + n + ", p)\n";
        mArgs += ", texture2d<float> tex" + n + " [[texture(" + n + ")]], sampler tex" + n + "_s [[sampler(" + n + ")]]";
        mMacros += "#define TEX" + n + "(p) tex" + n + ".sample(tex" + n + "_s, p)\n";
    }
    b.fs = defines + block + gTex + "in vec2 v_screen;\nout vec4 o_color;\n" + common + gMacros +
           "#define RETURN(c) { o_color = (c); return; }\n#define PF_FN\n" + (helpers ? helpers : "") +
           "void main() {\n    vec2 s = v_screen;\n" + body + "}\n";
    b.msl = "#include <metal_stdlib>\nusing namespace metal;\n" + defines +
            "#define vec2 float2\n#define vec3 float3\n#define vec4 float4\n" + mstruct +
            "struct VIn { float2 pos [[attribute(0)]]; };\nstruct VOut { float4 pos [[position]]; float2 screen; };\n"
            "vertex VOut vs_main(VIn in [[stage_in]]) { VOut o; o.pos = float4(in.pos, 0.0, 1.0); o.screen = in.pos * 0.5 + 0.5; return o; }\n" +
            common + mMacros + "#define RETURN(c) return (c)\n#define PF_FN static\n" + (helpers ? helpers : "") +
            "fragment float4 fs_main(VOut in [[stage_in]], constant U0& u [[buffer(1)]]" + mArgs + ") {\n" + mlocals +
            "    vec2 s = in.screen;\n" + body + "}\n";
    return b;
}

void put(float* d, float a, float b, float c, float e) { d[0] = a; d[1] = b; d[2] = c; d[3] = e; }

} // namespace

PostFxQuality PostFx::defaultQualityFor(const gfx::Device& dev, bool mobile) {
    if (const char* q = std::getenv("RTS_POSTFX")) {   // testing override: off|low|medium|high
        PostFxQuality r;
        if (parsePostFxQuality(q, r)) return r;
    }
    const std::string& n = dev.info().name;
    if (n.find("llvmpipe") != std::string::npos || n.find("SwiftShader") != std::string::npos) return PostFxQuality::Low;
    if (mobile) {
        for (const char* weak : {"Mali-4", "Mali-T", "Adreno (TM) 3", "Adreno (TM) 4", "Adreno (TM) 50", "PowerVR SGX", "PowerVR Rogue GE"})
            if (n.find(weak) != std::string::npos) return PostFxQuality::Low;
        return PostFxQuality::Medium;
    }
    return PostFxQuality::High;
}

bool PostFx::init(gfx::Device& dev) {
    dev_ = &dev;
    const float tri[6] = {-1, -1, 3, -1, -1, 3};
    gfx::BufferDesc bd; bd.data = tri; bd.size = sizeof tri; bd.label = "postfx triangle";
    triangle_ = dev.createBuffer(bd);
    const uint32_t zero = 0xff000000u;
    gfx::TextureDesc td; td.width = td.height = 1; td.data = &zero; td.label = "postfx black";
    black_ = dev.createTexture(td);
    return triangle_.valid();
}

void PostFx::release(gfx::TextureHandle& t) { if (t.valid() && dev_) dev_->destroyTexture(t); t = {}; }

void PostFx::shutdown() {
    if (!dev_) return;
    for (auto& s : sizes_) {
        for (auto& [r, t] : s.targets) release(t.tex);
        for (auto& [k, d] : s.depths) release(d);
    }
    sizes_.clear();
    for (auto& [p, l] : luts_) release(l.tex);
    luts_.clear();
    for (int i = 0; i < kProgCount; ++i) {
        if (pipes_[i].valid()) dev_->destroyPipeline(pipes_[i]);
        if (shaders_[i].valid()) dev_->destroyShader(shaders_[i]);
        pipes_[i] = {}; shaders_[i] = {}; failed_[i] = false;
    }
    for (auto& [m, sp] : composites_) { dev_->destroyPipeline(sp.second); dev_->destroyShader(sp.first); }
    composites_.clear();
    compositeFailed_.clear();
    if (triangle_.valid()) dev_->destroyBuffer(triangle_);
    release(black_);
    triangle_ = {};
    dev_ = nullptr;
}

void PostFx::configure(const Json& game, const Json& map, std::vector<std::string>* issues) {
    std::vector<std::string> local;
    setSettings(parsePostFx(game, map, issues ? issues : &local));
    if (!issues) for (auto& m : local) std::fprintf(stderr, "[postfx] %s\n", m.c_str());
}

void PostFx::setSettings(PostFxSettings s) { settings_ = std::move(s); }

bool PostFx::active() const {
    return dev_ && triangle_.valid() && enabled_ && settings_.anyActive(quality_);
}

PostFx::Sized& PostFx::sized(int w, int h) {
    ++frame_;
    for (auto& s : sizes_) if (s.w == w && s.h == h) { s.lastUse = frame_; return s; }
    if (sizes_.size() >= 3) {   // drop the least recently used set (window resized, view closed)
        auto it = std::min_element(sizes_.begin(), sizes_.end(), [](const Sized& a, const Sized& b) { return a.lastUse < b.lastUse; });
        for (auto& [r, t] : it->targets) release(t.tex);
        for (auto& [k, d] : it->depths) release(d);
        sizes_.erase(it);
    }
    sizes_.reserve(3);
    sizes_.push_back({});
    Sized& s = sizes_.back();
    s.w = w; s.h = h; s.lastUse = frame_;
    return s;
}

PostFx::Target& PostFx::target(Sized& s, int role, int w, int h) {
    w = std::max(w, 1); h = std::max(h, 1);
    Target& t = s.targets[role];
    if (t.tex.valid() && t.w == w && t.h == h) return t;
    release(t.tex);
    gfx::TextureDesc td;
    td.width = w; td.height = h;
    td.renderTarget = true;
    td.filter = gfx::Filter::Linear;
    td.label = "postfx target";
    t.tex = dev_->createTexture(td);
    t.w = w; t.h = h;
    return t;
}

gfx::TextureHandle PostFx::depthFor(Sized& s, int w, int h) {
    const uint64_t key = (uint64_t(uint32_t(w)) << 32) | uint32_t(h);
    auto it = s.depths.find(key);
    if (it != s.depths.end()) return it->second;
    gfx::TextureDesc td;
    td.width = w; td.height = h;
    td.format = gfx::PixelFormat::Depth;
    td.renderTarget = true;
    td.filter = gfx::Filter::Nearest;
    td.label = "postfx depth";
    return s.depths[key] = dev_->createTexture(td);
}

gfx::PipelineHandle PostFx::program(Prog p) {
    if (pipes_[p].valid() || failed_[p]) return pipes_[p];
    struct Def { const char* body; const char* helpers; uint32_t tex; const char* label; };
    static const Def defs[kProgCount] = {
        {postfx::kPrefilter, nullptr, 1, "postfx prefilter"},
        {postfx::kDown, nullptr, 1, "postfx down"},
        {postfx::kUp, nullptr, 3, "postfx up"},
        {postfx::kBlur, nullptr, 1, "postfx blur"},
        {postfx::kShaftsPre, nullptr, 1, "postfx shafts mask"},
        {postfx::kShafts, nullptr, 1, "postfx shafts"},
        {postfx::kFxaa, postfx::kFxaaHelpers, 1, "postfx fxaa"},
    };
    const Def& d = defs[p];
    Built b = buildProgram(kSimpleFields, 3, d.tex, "", d.helpers, d.body);
    gfx::ShaderDesc sd;
    sd.glslVertex = b.vs.c_str(); sd.glslFragment = b.fs.c_str(); sd.msl = b.msl.c_str();
    sd.textureSlots = 2; sd.uniformSlots = 1; sd.label = d.label;
    shaders_[p] = dev_->createShader(sd);
    if (!shaders_[p].valid()) {
        failed_[p] = true;
        error_ = std::string(d.label) + ": " + dev_->lastError();
        std::fprintf(stderr, "[postfx] %s\n", error_.c_str());
        return {};
    }
    gfx::PipelineDesc pd;
    pd.shader = shaders_[p];
    pd.attrs[0] = {0, gfx::VertexFormat::Float2, 0};
    pd.attrCount = 1;
    pd.stride = 8;
    pd.indexType = gfx::IndexType::None;
    pd.cull = gfx::CullMode::None;
    pd.depthTest = false; pd.depthWrite = false;
    pd.label = d.label;
    pipes_[p] = dev_->createPipeline(pd);
    if (!pipes_[p].valid()) failed_[p] = true;
    return pipes_[p];
}

gfx::PipelineHandle PostFx::composite(uint32_t mask) {
    if (auto it = composites_.find(mask); it != composites_.end()) return it->second.second;
    if (compositeFailed_.count(mask)) return {};
    std::string defines;
    for (int i = 0; i < 13; ++i) if (mask & (1u << i)) defines += std::string("#define ") + kPfNames[i] + " 1\n";
    uint32_t tex = 1;
    if (mask & kPfBloom) tex |= 2;
    if (mask & kPfTilt) tex |= 4;
    if (mask & kPfShafts) tex |= 8;
    if (mask & kPfLut) tex |= 16;
    Built b = buildProgram(kCompositeFields, 14, tex, defines, postfx::kCompositeHelpers, postfx::kComposite);
    gfx::ShaderDesc sd;
    sd.glslVertex = b.vs.c_str(); sd.glslFragment = b.fs.c_str(); sd.msl = b.msl.c_str();
    sd.textureSlots = 5; sd.uniformSlots = 1; sd.label = "postfx composite";
    gfx::ShaderHandle sh = dev_->createShader(sd);
    if (!sh.valid()) {
        compositeFailed_[mask] = true;
        error_ = "postfx composite: " + dev_->lastError();
        std::fprintf(stderr, "[postfx] %s\n", error_.c_str());
        return {};
    }
    gfx::PipelineDesc pd;
    pd.shader = sh;
    pd.attrs[0] = {0, gfx::VertexFormat::Float2, 0};
    pd.attrCount = 1;
    pd.stride = 8;
    pd.indexType = gfx::IndexType::None;
    pd.cull = gfx::CullMode::None;
    pd.depthTest = false; pd.depthWrite = false;
    pd.label = "postfx composite";
    gfx::PipelineHandle p = dev_->createPipeline(pd);
    if (!p.valid()) { dev_->destroyShader(sh); compositeFailed_[mask] = true; return {}; }
    composites_[mask] = {sh, p};
    return p;
}

const PostFx::Lut* PostFx::lut(const std::string& path) {
    auto it = luts_.find(path);
    if (it != luts_.end()) return it->second.failed ? nullptr : &it->second;
    Lut& l = luts_[path];
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!loader_ || !loader_(path, w, h, rgba)) {
        l.failed = true;
        std::fprintf(stderr, "[postfx] lut '%s': can't read the image\n", path.c_str());
        return nullptr;
    }
    if (h < 2 || w != h * h || rgba.size() < size_t(w) * size_t(h) * 4) {
        l.failed = true;
        std::fprintf(stderr, "[postfx] lut '%s': expected a strip of N squares N x N (e.g. 256x16, 1024x32), got %dx%d\n", path.c_str(), w, h);
        return nullptr;
    }
    gfx::TextureDesc td;
    td.width = w; td.height = h; td.data = rgba.data();
    td.filter = gfx::Filter::Linear;
    td.label = "postfx lut";
    l.tex = dev_->createTexture(td);
    l.n = h;
    if (!l.tex.valid()) { l.failed = true; return nullptr; }
    return &l;
}

gfx::PassDesc PostFx::scenePass(const gfx::PassDesc& out, int w, int h) {
    Sized& s = sized(w, h);
    gfx::PassDesc p = out;
    p.colorTarget = target(s, kScene, w, h).tex;
    p.depthTarget = depthFor(s, w, h);
    p.clearColorEnabled = true;     // the composite covers every pixel; clearing is cheaper than loading on tilers
    p.clearDepthEnabled = true;
    return p;
}

void PostFx::draw(Sized& s, const Target& dst, gfx::PipelineHandle pipe, const void* u, size_t size,
                  std::initializer_list<std::pair<int, gfx::TextureHandle>> tex) {
    gfx::PassDesc p;
    p.colorTarget = dst.tex;
    p.depthTarget = depthFor(s, dst.w, dst.h);
    p.clearColor[0] = p.clearColor[1] = p.clearColor[2] = 0; p.clearColor[3] = 1;
    dev_->beginPass(p);
    dev_->setViewport({0, 0, dst.w, dst.h});
    dev_->setPipeline(pipe);
    dev_->setVertexBuffer(triangle_);
    dev_->setUniforms(0, u, size);
    for (auto& [slot, t] : tex) dev_->setTexture(slot, t);
    dev_->draw(3);
    dev_->endPass();
    ++stats_.passes;
}

void PostFx::resolve(gfx::Device& dev, const gfx::PassDesc& out, const PostFrame& f) {
    stats_ = {};
    const int w = std::max(f.width, 1), h = std::max(f.height, 1);
    stats_.width = w; stats_.height = h;
    Sized& set = sized(w, h);
    const Target scene = target(set, kScene, w, h);
    const float flip = dev.info().renderTargetFlipY ? 1.0f : 0.0f;
    const auto active = settings_.activeAt(quality_);
    stats_.effects = int(active.size());
    auto find = [&](PostFxType t) -> const PostFxEffect* {
        for (auto* e : active) if (e->type == t) return e;
        return nullptr;
    };
    const float seed = float(frame_ % 61) * 0.173f;

    // ---- FXAA: the composite reads the anti-aliased image instead of the scene
    Target src = scene;
    if (const PostFxEffect* e = find(PostFxType::Fxaa); e && program(kFxaa).valid()) {
        SimpleU u{};
        put(u.px, 1.0f / float(w), 1.0f / float(h), flip, 0);
        put(u.p0, e->get("subpixel"), 0, 0, 0);
        src = target(set, kAa, w, h);
        draw(set, src, pipes_[kFxaa], &u, sizeof u, {{0, scene.tex}});
    }

    // ---- bloom: prefilter -> dual-filter down chain -> up chain
    gfx::TextureHandle bloomTex;
    float bloomNorm = 1;
    const PostFxEffect* bloom = find(PostFxType::Bloom);
    if (bloom && program(kPrefilter).valid() && program(kDown).valid() && program(kUp).valid()) {
        const BloomPlan plan = bloomPlan(quality_);
        std::vector<Target> down;
        int lw = std::max(1, w / plan.startDiv), lh = std::max(1, h / plan.startDiv);
        for (int i = 0; i < plan.levels && lw >= 2 && lh >= 2; ++i, lw /= 2, lh /= 2) down.push_back(target(set, kBloomDown + i, lw, lh));
        if (!down.empty()) {
            SimpleU u{};
            const float spread = float(plan.startDiv) * 0.5f;
            put(u.px, spread / float(w), spread / float(h), flip, 0);
            put(u.p0, bloom->get("threshold"), bloom->get("knee"), 0, 0);
            draw(set, down[0], pipes_[kPrefilter], &u, sizeof u, {{0, src.tex}});
            for (size_t i = 1; i < down.size(); ++i) {
                put(u.px, 1.0f / float(down[i - 1].w), 1.0f / float(down[i - 1].h), flip, 0);
                draw(set, down[i], pipes_[kDown], &u, sizeof u, {{0, down[i - 1].tex}});
            }
            const float scatter = 0.3f + 0.7f * bloom->get("radius");
            Target low = down.back();
            for (int i = int(down.size()) - 2; i >= 0; --i) {
                const Target up = target(set, kBloomUp + i, down[size_t(i)].w, down[size_t(i)].h);
                put(u.px, 1.0f / float(low.w), 1.0f / float(low.h), flip, 0);
                put(u.p0, scatter, 0, 0, 0);
                draw(set, up, pipes_[kUp], &u, sizeof u, {{0, low.tex}, {1, down[size_t(i)].tex}});
                low = up;
            }
            bloomTex = low.tex;
            bloomNorm = 0;
            for (size_t k = 0; k < down.size(); ++k) bloomNorm += std::pow(scatter, float(k));
        }
    }

    // ---- tilt-shift: downsample + one separable gaussian (H, V)
    gfx::TextureHandle tiltTex;
    const PostFxEffect* tilt = find(PostFxType::TiltShift);
    if (tilt && program(kDown).valid() && program(kBlur).valid()) {
        const BlurPlan plan = tiltBlurPlan(quality_);
        const Target a = target(set, kTilt0, w / plan.div, h / plan.div), b = target(set, kTilt1, w / plan.div, h / plan.div);
        SimpleU u{};
        put(u.px, float(plan.div) * 0.5f / float(w), float(plan.div) * 0.5f / float(h), flip, 0);
        draw(set, a, pipes_[kDown], &u, sizeof u, {{0, src.tex}});
        const float spread = (0.5f + 2.0f * tilt->get("blur")) * (plan.div == 2 ? 2.0f : 1.0f);   // same radius on screen
        put(u.px, 1.0f / float(a.w), 1.0f / float(a.h), flip, 0);
        put(u.p0, spread, 0, 0, 0);
        draw(set, b, pipes_[kBlur], &u, sizeof u, {{0, a.tex}});
        put(u.p0, 0, spread, 0, 0);
        draw(set, a, pipes_[kBlur], &u, sizeof u, {{0, b.tex}});
        tiltTex = a.tex;
    }

    // ---- sun shafts: from the sun on screen, or (top-down RTS cameras rarely see the sun) from a point
    // just past the screen edge in the direction the sunlight comes from
    gfx::TextureHandle shaftTex;
    float shaftColor[3] = {0, 0, 0};
    const PostFxEffect* shafts = find(PostFxType::SunShafts);
    if (shafts && program(kShaftsPre).valid() && program(kShafts).valid()) {
        const float3 sd = normalize(f.lighting.sunDir);
        const float4 c = f.viewProj * float4{sd.x, sd.y, sd.z, 0.0f};   // direction: the sun at infinity
        float nx = 0, ny = 0;
        bool placed = false;
        if (c.w > 1e-4f && std::max(std::fabs(c.x / c.w), std::fabs(c.y / c.w)) < 1.3f) { nx = c.x / c.w; ny = c.y / c.w; placed = true; }
        else {
            const mat4 inv = f.viewProj.inverse();
            const float4 m = inv * float4{0, 0, 0.5f, 1};
            if (std::fabs(m.w) > 1e-6f) {
                const float3 p0{m.x / m.w, m.y / m.w, m.z / m.w};
                const float3 p1 = p0 + sd * std::max(1.0f, length(p0 - f.cameraPos) * 0.25f);
                const float4 a0 = f.viewProj * float4{p0.x, p0.y, p0.z, 1}, a1 = f.viewProj * float4{p1.x, p1.y, p1.z, 1};
                if (a0.w > 1e-4f && a1.w > 1e-4f) {
                    float dx = a1.x / a1.w - a0.x / a0.w, dy = a1.y / a1.w - a0.y / a0.w;
                    const float l = std::sqrt(dx * dx + dy * dy);
                    if (l > 1e-5f) { nx = dx / l * 1.35f; ny = dy / l * 1.35f; placed = true; }
                }
            }
        }
        const float vis = placed ? std::clamp(f.lighting.sunIntensity, 0.0f, 1.0f) * std::clamp(sd.y * 4.0f + 0.2f, 0.0f, 1.0f) : 0.0f;
        {
            if (vis > 0.01f) {
                const ShaftsPlan plan = shaftsPlan(quality_);
                float sx = nx * 0.5f + 0.5f, sy = ny * 0.5f + 0.5f;
                if (flip > 0.5f) sy = 1.0f - sy;
                const Target a = target(set, kShaft0, w / plan.div, h / plan.div), b = target(set, kShaft1, w / plan.div, h / plan.div);
                SimpleU u{};
                const float spread = float(plan.div) * 0.5f;
                put(u.px, spread / float(w), spread / float(h), flip, 0);
                put(u.p0, shafts->get("threshold"), 0, 0, 0);
                put(u.sun, sx, sy, 0, float(w) / float(h));
                draw(set, a, pipes_[kShaftsPre], &u, sizeof u, {{0, src.tex}});
                put(u.px, 1.0f / float(a.w), 1.0f / float(a.h), flip, 0);
                put(u.p0, float(plan.samples), 0.3f + 0.6f * shafts->get("length"), 0.97f, 1.6f);
                draw(set, b, pipes_[kShafts], &u, sizeof u, {{0, a.tex}});
                shaftTex = b.tex;
                const float* col = shafts->color("color");
                const float k = shafts->get("intensity") * vis;
                for (int i = 0; i < 3; ++i) shaftColor[i] = col[i] * k;
            }
        }
    }

    // ---- composite into the caller's pass (left open)
    CompositeU u{};
    uint32_t mask = 0;
    put(u.px, 1.0f / float(w), 1.0f / float(h), flip, seed);
    put(u.grade1, 1, 1, 1, 1);
    put(u.gamma, 1, 1, 1, 0.5f);
    put(u.gain, 1, 1, 1, 0);
    put(u.grade0, 0, 1, 1, 8);
    put(u.filt, 0, 0, 0, 1);
    for (auto* e : active) {
        switch (e->type) {
        case PostFxType::Pixelate: mask |= kPfPixelate; u.grade1[3] = std::max(1.0f, e->get("size")); break;
        case PostFxType::ChromaticAberration: mask |= kPfCa; u.vig[3] = e->get("intensity"); break;
        case PostFxType::Sharpen: mask |= kPfSharpen; u.bloom[3] = e->get("amount"); break;
        case PostFxType::TiltShift:
            if (tiltTex.valid()) { mask |= kPfTilt; put(u.tilt, e->get("focus"), e->get("width"), e->get("falloff"), 0); }
            break;
        case PostFxType::Bloom:
            if (bloomTex.valid()) {
                mask |= kPfBloom;
                const float* t = e->color("tint");
                const float k = e->get("intensity") / std::max(bloomNorm, 1.0f);
                for (int i = 0; i < 3; ++i) u.bloom[i] = t[i] * k;
            }
            break;
        case PostFxType::SunShafts:
            if (shaftTex.valid()) { mask |= kPfShafts; for (int i = 0; i < 3; ++i) u.shafts[i] = shaftColor[i]; }
            break;
        case PostFxType::ColorGrade: {
            mask |= kPfGrade;
            const float ex = std::exp2(e->get("exposure"));
            const float t = e->get("temperature"), ti = e->get("tint");
            const float* fl = e->color("filter");
            const float wb[3] = {1.0f + 0.25f * t + 0.1f * ti, 1.0f - 0.2f * ti, 1.0f - 0.25f * t + 0.1f * ti};
            for (int i = 0; i < 3; ++i) u.grade1[i] = ex * wb[i] * fl[i];
            const float* li = e->color("lift");
            const float* ga = e->color("gamma");
            const float* gn = e->color("gain");
            for (int i = 0; i < 3; ++i) { u.lift[i] = li[i]; u.gamma[i] = 1.0f / std::max(ga[i], 0.05f); u.gain[i] = gn[i]; }
            u.grade0[1] = e->get("contrast");
            u.grade0[2] = e->get("saturation");
            break;
        }
        case PostFxType::Lut:
            if (const Lut* l = lut(e->text)) { mask |= kPfLut; u.lut[0] = float(l->n); u.lut[1] = e->get("flipY"); u.shafts[3] = e->get("intensity"); }
            break;
        case PostFxType::Posterize: mask |= kPfPosterize; u.grade0[3] = std::max(2.0f, e->get("levels")); break;
        case PostFxType::Vignette: {
            mask |= kPfVignette;
            u.lift[3] = e->get("intensity");
            u.gamma[3] = e->get("softness");
            const float* c = e->color("color");
            for (int i = 0; i < 3; ++i) u.vig[i] = c[i];
            break;
        }
        case PostFxType::Grain:
            mask |= kPfGrain;
            u.gain[3] = e->get("intensity");
            u.filt[3] = std::max(1.0f, e->get("size"));
            if (e->get("animated") < 0.5f) u.px[3] = 0;
            break;
        case PostFxType::Fade: { mask |= kPfFade; const float* c = e->color("color"); put(u.fade, c[0], c[1], c[2], e->get("amount")); break; }
        case PostFxType::Letterbox: { mask |= kPfLetterbox; const float* c = e->color("color"); put(u.lbox, c[0], c[1], c[2], e->get("amount")); break; }
        default: break;
        }
    }
    gfx::PipelineHandle pipe = composite(mask);
    if (!pipe.valid()) pipe = composite(0);   // a broken variant still shows the scene
    dev.beginPass(out);
    dev.setViewport({0, 0, w, h});
    if (pipe.valid()) {
        dev.setPipeline(pipe);
        dev.setVertexBuffer(triangle_);
        dev.setUniforms(0, &u, sizeof u);
        dev.setTexture(0, src.tex);
        dev.setTexture(1, bloomTex.valid() ? bloomTex : black_);
        dev.setTexture(2, tiltTex.valid() ? tiltTex : black_);
        dev.setTexture(3, shaftTex.valid() ? shaftTex : black_);
        const Lut* l = (mask & kPfLut) ? lut(find(PostFxType::Lut)->text) : nullptr;
        dev.setTexture(4, l ? l->tex : black_);
        dev.draw(3);
    }
    ++stats_.passes;
}

} // namespace rts::render
