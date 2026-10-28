#include "engine/splash/SplashScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "engine/atlas/AtlasFormat.h"
#include "engine/splash/SplashShaders.h"

namespace rts::splash {

namespace {
constexpr int kMaxTexture = 2048;   // splash images are downscaled to this on load (weak GPUs, memory)

// Halves the image until it fits kMaxTexture (box filter, straight alpha weighted by alpha).
void fitTexture(atlas::Image& img) {
    while (img.width > kMaxTexture || img.height > kMaxTexture) {
        atlas::Image h;
        h.width = std::max(1, img.width / 2);
        h.height = std::max(1, img.height / 2);
        h.rgba.resize(size_t(h.width) * size_t(h.height) * 4);
        for (int y = 0; y < h.height; ++y)
            for (int x = 0; x < h.width; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const uint8_t* p = img.px(std::min(img.width - 1, x * 2 + dx), std::min(img.height - 1, y * 2 + dy));
                        float a = p[3];
                        acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += a;
                    }
                uint8_t* o = h.px(x, y);
                for (int c = 0; c < 3; ++c) o[c] = uint8_t(acc[3] > 0 ? std::lround(acc[c] / acc[3]) : 0);
                o[3] = uint8_t(std::lround(acc[3] / 4));
            }
        img = std::move(h);
    }
}
} // namespace

SplashScreen::SplashScreen() = default;
SplashScreen::~SplashScreen() { shutdown(); }

bool SplashScreen::init(gfx::Device& dev, const Json& block, ReadFn read) {
    dev_ = &dev;
    read_ = std::move(read);
    try {
        spec_ = parseSpec(block, &warnings_);
    } catch (const std::exception& e) {
        error_ = e.what();
        Spec fallback;
        fallback.enabled = true;
        Color bg;
        if (parseColor(block["background"], bg)) fallback.background = bg;
        spec_ = fallback;
    }
    tex_.assign(spec_.layers.size(), {});
    pending_.clear();
    for (size_t i = 0; i < spec_.layers.size(); ++i) {
        const Layer& l = spec_.layers[i];
        if (!l.sheet.empty()) pending_.push_back({int(i), -1});
        for (size_t f = 0; f < l.images.size(); ++f) pending_.push_back({int(i), int(f)});
        tex_[i].frames.assign(l.images.size(), {});
    }
    images_ = std::make_unique<atlas::ImageSource>(read_, [](const std::string& b, atlas::Image& o) { return atlas::decodeImageFile(b, o); });

    gfx::ShaderDesc sd;
    sd.glslVertex = shaders::kSplashVS;
    sd.glslFragment = shaders::kSplashFS;
    sd.msl = shaders::kSplashMSL;
    sd.textureSlots = 1;
    sd.uniformSlots = 1;
    sd.label = "splash";
    shader_ = dev.createShader(sd);
    if (!shader_.valid()) { error_ = "splash shader: " + dev.lastError(); return false; }
    gfx::PipelineDesc pd;
    pd.shader = shader_;
    pd.attrs[0] = {0, gfx::VertexFormat::Float2, 0};
    pd.attrs[1] = {1, gfx::VertexFormat::Float2, 8};
    pd.attrs[2] = {2, gfx::VertexFormat::UByte4N, 16};
    pd.attrCount = 3;
    pd.stride = sizeof(Vtx);
    pd.indexType = gfx::IndexType::None;
    pd.cull = gfx::CullMode::None;
    pd.blend = gfx::BlendMode::Alpha;
    pd.depthTest = false;
    pd.depthWrite = false;
    pd.label = "splash";
    pipe_ = dev.createPipeline(pd);
    gfx::BufferDesc bd;
    bd.type = gfx::BufferType::Vertex;
    bd.usage = gfx::Usage::Dynamic;
    bd.size = sizeof(Vtx) * 6 * 16;
    bd.label = "splash";
    vb_ = dev.createBuffer(bd);
    const uint32_t white = 0xffffffffu;
    gfx::TextureDesc td;
    td.width = td.height = 1;
    td.data = &white;
    td.label = "splash white";
    white_ = dev.createTexture(td);
    return pipe_.valid();
}

void SplashScreen::shutdown() {
    if (!dev_) return;
    for (auto& t : tex_) {
        for (auto& f : t.frames) if (f.valid()) dev_->destroyTexture(f);
        if (t.sheet.valid()) dev_->destroyTexture(t.sheet);
    }
    tex_.clear();
    if (white_.valid()) dev_->destroyTexture(white_);
    if (vb_.valid()) dev_->destroyBuffer(vb_);
    if (pipe_.valid()) dev_->destroyPipeline(pipe_);
    if (shader_.valid()) dev_->destroyShader(shader_);
    white_ = {}; vb_ = {}; pipe_ = {}; shader_ = {};
    images_.reset();
    dev_ = nullptr;
}

void SplashScreen::loadImage(int index) {
    auto [li, fi] = pending_[size_t(index)];
    const Layer& l = spec_.layers[size_t(li)];
    const std::string& path = fi < 0 ? l.sheet : l.images[size_t(fi)];
    atlas::Image img;
    if (!images_ || !images_->load(path, img) || img.empty()) {
        warnings_.push_back("splash: cannot load " + path);
        return;
    }
    LayerTex& t = tex_[size_t(li)];
    const int srcW = img.width, srcH = img.height;
    fitTexture(img);
    gfx::TextureDesc td;
    td.width = img.width;
    td.height = img.height;
    td.data = img.rgba.data();
    td.mipmaps = fi >= 0;   // sheets: neighbouring frames would bleed into each other's mips
    td.label = "splash image";
    gfx::TextureHandle h = dev_->createTexture(td);
    if (fi < 0) {
        t.sheet = h;
        t.sheetW = srcW; t.sheetH = srcH;
        t.w = l.frameW; t.h = l.frameH;
    } else {
        t.frames[size_t(fi)] = h;
        if (t.w == 0) { t.w = srcW; t.h = srcH; }
    }
}

void SplashScreen::update(double dt, bool stillLoading) {
    if (nextImage_ < int(pending_.size())) loadImage(nextImage_++);
    // the clocks start once the first images are on screen, so a slow decode doesn't eat the fade-in
    const bool firstReady = pending_.empty() || nextImage_ >= std::min<int>(int(pending_.size()), 2);
    if (!firstReady) return;
    elapsed_ += float(dt);
    timeline_ = advanceTimeline(spec_, timeline_, float(dt), stillLoading);
    // displayed progress eases towards the real one and never goes backwards
    shown_ = std::min(1.0f, std::max(shown_, shown_ + (progress_ - shown_) * std::min(1.0f, float(dt) * 8.0f)));
    if (!stillLoading) shown_ = std::min(1.0f, shown_ + float(dt) * 2.0f);
}

void SplashScreen::setProgress(float p) { progress_ = std::clamp(std::max(progress_, p), 0.0f, 1.0f); }

void SplashScreen::quad(float cx, float cy, float w, float h, float rot, const float uv[4], uint32_t color) {
    const float c = std::cos(rot), s = std::sin(rot), hw = w * 0.5f, hh = h * 0.5f;
    auto P = [&](float x, float y, float u, float v) { return Vtx{cx + x * c - y * s, cy + x * s + y * c, u, v, color}; };
    Vtx a = P(-hw, -hh, uv[0], uv[1]), b = P(hw, -hh, uv[2], uv[1]), d = P(hw, hh, uv[2], uv[3]), e = P(-hw, hh, uv[0], uv[3]);
    verts_.insert(verts_.end(), {a, b, d, a, d, e});
}

void SplashScreen::draw(gfx::Device& dev, float opacity, bool clear, float pixelScale) {
    const float W = float(dev.framebufferWidth()), H = float(dev.framebufferHeight());
    if (W <= 0 || H <= 0) return;
    gfx::PassDesc pass;
    pass.clearColor[0] = spec_.background.r; pass.clearColor[1] = spec_.background.g; pass.clearColor[2] = spec_.background.b; pass.clearColor[3] = 1;
    pass.clearColorEnabled = clear;
    pass.clearDepthEnabled = clear;
    dev.beginPass(pass);
    if (!pipe_.valid()) { dev.endPass(); return; }
    opacity = std::clamp(opacity, 0.0f, 1.0f);

    std::vector<ImageSize> sizes(spec_.layers.size());
    for (size_t i = 0; i < tex_.size(); ++i) sizes[i] = {tex_[i].w, tex_[i].h};
    struct Call { gfx::TextureHandle tex; uint32_t first, count; };
    std::vector<Call> calls;
    verts_.clear();
    auto add = [&](gfx::TextureHandle t, auto&& emit) {
        uint32_t first = uint32_t(verts_.size());
        emit();
        if (!calls.empty() && calls.back().tex == t) calls.back().count += uint32_t(verts_.size()) - first;
        else calls.push_back({t, first, uint32_t(verts_.size()) - first});
    };
    auto tint = [&](const Color& c, float a) { Color k = c; k.a *= a * opacity; return k.rgba8(); };
    // fading out over the game: the background colour goes too
    if (!clear && opacity > 0) {
        const float full[4] = {0, 0, 1, 1};
        add(white_, [&] { quad(W * 0.5f, H * 0.5f, W, H, 0, full, tint(spec_.background, 1)); });
    }
    for (const DrawItem& d : evaluate(spec_, sizes, timeline_, W, H, pixelScale)) {
        const LayerTex& t = tex_[size_t(d.layer)];
        const Layer& l = spec_.layers[size_t(d.layer)];
        float uv[4] = {0, 0, 1, 1};
        gfx::TextureHandle th;
        if (t.sheet.valid()) {
            int cols = std::max(1, t.sheetW / std::max(1, l.frameW));
            int fx = d.frame % cols, fy = d.frame / cols;
            uv[0] = float(fx * l.frameW) / float(t.sheetW); uv[1] = float(fy * l.frameH) / float(t.sheetH);
            uv[2] = float((fx + 1) * l.frameW) / float(t.sheetW); uv[3] = float((fy + 1) * l.frameH) / float(t.sheetH);
            th = t.sheet;
        } else if (size_t(d.frame) < t.frames.size()) {
            th = t.frames[size_t(d.frame)];
        }
        if (!th.valid()) continue;   // frame not decoded yet
        add(th, [&] { quad(d.cx, d.cy, d.w, d.h, d.rotation, uv, tint({1, 1, 1, 1}, d.opacity)); });
    }
    float px, py, pw, ph;
    if (progressRect(spec_, W, H, pixelScale, px, py, pw, ph)) {
        const float full[4] = {0, 0, 1, 1};
        add(white_, [&] {
            quad(px + pw * 0.5f, py + ph * 0.5f, pw, ph, 0, full, tint(spec_.progress.track, 1));
            float fw = std::round(pw * shown_);
            if (fw > 0) quad(px + fw * 0.5f, py + ph * 0.5f, fw, ph, 0, full, tint(spec_.progress.color, 1));
        });
    }
    if (!verts_.empty()) {
        dev.updateBuffer(vb_, verts_.data(), verts_.size() * sizeof(Vtx));
        dev.setViewport({0, 0, int(W), int(H)});
        dev.setPipeline(pipe_);
        dev.setVertexBuffer(vb_);
        const float target[4] = {W, H, 0, 0};
        dev.setUniforms(0, target, sizeof target);
        for (auto& c : calls) {
            dev.setTexture(0, c.tex);
            dev.draw(c.count, c.first);
        }
    }
    dev.endPass();
}

} // namespace rts::splash
