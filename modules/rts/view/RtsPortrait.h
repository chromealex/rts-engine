// Unit portraits: one type rendered by the game view into a texture (HUD portrait, production
// buttons, editor previews). Owns its renderer and render targets; call render() outside of an
// open render pass (before the frame's scene), then sample texture(). On Metal the texture's v
// grows downward (DeviceInfo::renderTargetFlipY): flip the UVs when drawing it.
#pragma once
#include "RtsView.h"

namespace rts::game {

class Portrait {
public:
    bool init(gfx::Device& dev, int size = 256);
    void shutdown();
    // Renders `type` of `owner` at animation time `time` (idle clip, slowly turning when yaw < 0).
    void render(RtsView& view, uint16_t type, uint8_t owner, double time, float yaw = -1);
    gfx::TextureHandle texture() const { return color_; }
    int size() const { return size_; }

private:
    gfx::Device* dev_ = nullptr;
    render::Renderer3D r3d_;
    gfx::TextureHandle color_, depth_;
    int size_ = 0;
};

} // namespace rts::game
