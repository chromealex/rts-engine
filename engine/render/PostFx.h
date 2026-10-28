// Post-processing on the RHI: the effect chain of game.json / map "postfx" blocks (PostFxData.h).
// Plugs into Renderer3D through the PostProcess hook:
//
//   r3d.render(pass, &postFx);   // the scene goes into postFx's target when an effect is active
//   particles.render(...);       // still part of the scene
//   r3d.resolvePostFx();         // effects -> `pass`, which stays open for UI
//
// Every effect has GLSL + MSL code (PostFxShaders.h, one body for both) and runs per quality tier
// (PostFxQuality, PostFxData plans). GLES 3.0 floor: RGBA8 targets only, no depth reads, no compute.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "PostFxData.h"
#include "Renderer3D.h"

namespace rts::render {

struct PostFxStats { int passes = 0; int effects = 0; int width = 0, height = 0; };

class PostFx : public PostProcess {
public:
    bool init(gfx::Device& dev);
    void shutdown();

    // game.json "postfx" then the map's (either may be null); problems go to `issues`.
    void configure(const Json& game, const Json& map, std::vector<std::string>* issues = nullptr);
    void setSettings(PostFxSettings s);
    const PostFxSettings& settings() const { return settings_; }

    void setQuality(PostFxQuality q) { quality_ = q; }
    PostFxQuality quality() const { return quality_; }
    static PostFxQuality defaultQualityFor(const gfx::Device& dev, bool mobile);   // RTS_POSTFX overrides
    void setEnabled(bool on) { enabled_ = on; }          // editor: preview on / off
    bool enabled() const { return enabled_; }

    // Reads an image of the game (LUTs): path -> RGBA8 top row first. Client: content files; editor: project dir.
    using ImageLoader = std::function<bool(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba)>;
    void setImageLoader(ImageLoader fn) { loader_ = std::move(fn); luts_.clear(); }

    // PostProcess
    bool active() const override;
    gfx::PassDesc scenePass(const gfx::PassDesc& out, int w, int h) override;
    void resolve(gfx::Device& dev, const gfx::PassDesc& out, const PostFrame& frame) override;

    const PostFxStats& stats() const { return stats_; }
    const std::string& lastError() const { return error_; }

private:
    struct Target { gfx::TextureHandle tex; int w = 0, h = 0; };
    struct Sized {                         // targets for one output size (editor: Scene + Game views)
        int w = 0, h = 0;
        uint64_t lastUse = 0;
        std::map<int, Target> targets;     // by role
        std::map<uint64_t, gfx::TextureHandle> depths;   // Metal: every pass has a depth attachment, shared per size
    };
    struct Lut { gfx::TextureHandle tex; int n = 0; bool failed = false; };
    enum Prog { kPrefilter, kDown, kUp, kBlur, kShaftsPre, kShafts, kFxaa, kProgCount };

    Target& target(Sized& s, int role, int w, int h);
    gfx::TextureHandle depthFor(Sized& s, int w, int h);
    Sized& sized(int w, int h);
    gfx::PipelineHandle program(Prog p);
    gfx::PipelineHandle composite(uint32_t mask);
    void draw(Sized& s, const Target& dst, gfx::PipelineHandle pipe, const void* u, size_t size,
              std::initializer_list<std::pair<int, gfx::TextureHandle>> tex);
    const Lut* lut(const std::string& path);
    void release(gfx::TextureHandle& t);

    gfx::Device* dev_ = nullptr;
    PostFxSettings settings_;
    PostFxQuality quality_ = PostFxQuality::Medium;
    bool enabled_ = true;
    gfx::BufferHandle triangle_;
    gfx::TextureHandle black_;
    gfx::ShaderHandle shaders_[kProgCount];
    gfx::PipelineHandle pipes_[kProgCount];
    bool failed_[kProgCount] = {};
    std::map<uint32_t, std::pair<gfx::ShaderHandle, gfx::PipelineHandle>> composites_;
    std::map<uint32_t, bool> compositeFailed_;
    std::vector<Sized> sizes_;
    std::map<std::string, Lut> luts_;
    ImageLoader loader_;
    uint64_t frame_ = 0;
    PostFxStats stats_;
    std::string error_;
};

} // namespace rts::render
