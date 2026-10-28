// In-engine splash / loading screen: draws the game.json "splash" block (SplashData.h) through the
// RHI from the very first frame while the client loads content step by step, then crossfades to
// the game. Images are decoded one per frame so the first frame never waits for them; everything
// is a handful of textured quads (one pipeline, no render targets), fine for the weakest GPUs.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/gfx/Gfx.h"
#include "engine/splash/SplashData.h"

namespace rts::atlas { class ImageSource; }

namespace rts::splash {

class SplashScreen {
public:
    using ReadFn = std::function<bool(const std::string& path, std::string& out)>;
    SplashScreen();
    ~SplashScreen();

    // Parses the spec (errors go to error(); the screen then shows the background + progress only)
    // and creates the pipeline. Does not decode images yet.
    bool init(gfx::Device& dev, const Json& splashBlock, ReadFn read);
    void shutdown();

    // Per frame: advances the clocks and decodes at most one pending image.
    void update(double dt, bool stillLoading);
    void setProgress(float p);             // 0..1, real loading progress (shown smoothed, never backwards)
    void skip() { if (spec_.skippable) skipped_ = true; }
    bool readyToLeave(bool loaded) const { return splash::readyToLeave(spec_, elapsed_, loaded && imagesLoaded(), skipped_); }
    bool imagesLoaded() const { return nextImage_ >= int(pending_.size()); }

    // Draws in its own screen pass. clear = paint the background (first draw of the frame);
    // otherwise blends over what is already there with `opacity` (crossfade to the menu).
    void draw(gfx::Device& dev, float opacity, bool clear, float pixelScale);

    const Spec& spec() const { return spec_; }
    const std::string& error() const { return error_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    float elapsed() const { return elapsed_; }
    float timeline() const { return timeline_; }
    float shownProgress() const { return shown_; }

private:
    struct LayerTex { std::vector<gfx::TextureHandle> frames; gfx::TextureHandle sheet; int w = 0, h = 0, sheetW = 0, sheetH = 0; };
    void loadImage(int index);
    void quad(float cx, float cy, float w, float h, float rot, const float uv[4], uint32_t color);

    gfx::Device* dev_ = nullptr;
    ReadFn read_;
    Spec spec_;
    std::string error_;
    std::vector<std::string> warnings_;
    std::vector<LayerTex> tex_;
    std::vector<std::pair<int, int>> pending_;   // (layer, frame index or -1 = sheet)
    int nextImage_ = 0;
    gfx::ShaderHandle shader_;
    gfx::PipelineHandle pipe_;
    gfx::BufferHandle vb_;
    gfx::TextureHandle white_;
    struct Vtx { float x, y, u, v; uint32_t c; };
    std::vector<Vtx> verts_;
    float elapsed_ = 0, timeline_ = 0, progress_ = 0, shown_ = 0;
    bool skipped_ = false;
    std::unique_ptr<atlas::ImageSource> images_;
};

} // namespace rts::splash
