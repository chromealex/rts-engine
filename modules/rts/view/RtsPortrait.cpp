#include "RtsPortrait.h"

#include <algorithm>
#include <cmath>

namespace rts::game {

using namespace render;

bool Portrait::init(gfx::Device& dev, int size) {
    dev_ = &dev;
    size_ = size;
    if (!r3d_.init(dev)) return false;
    r3d_.setShadowQuality(ShadowQuality::Low);
    gfx::TextureDesc td;
    td.width = td.height = size;
    td.renderTarget = true;
    td.label = "portrait";
    color_ = dev.createTexture(td);
    td.format = gfx::PixelFormat::Depth;
    td.filter = gfx::Filter::Nearest;
    depth_ = dev.createTexture(td);
    return color_.valid() && depth_.valid();
}

void Portrait::shutdown() {
    if (!dev_) return;
    r3d_.shutdown();
    if (color_.valid()) dev_->destroyTexture(color_);
    if (depth_.valid()) dev_->destroyTexture(depth_);
    color_ = depth_ = {};
    dev_ = nullptr;
}

void Portrait::render(RtsView& view, uint16_t type, uint8_t owner, double time, float yaw) {
    if (!dev_ || !color_.valid()) return;
    // frame the bind-pose bounds (look scale and fit are applied by the view; approximate with the model)
    float radius = 0.8f, height = 1.0f;
    if (const Model* m = view.modelOf(type)) {
        const float3 e = m->boundsMax - m->boundsMin;
        height = std::max(0.3f, std::min(e.y, 4.0f));
        radius = std::max({e.x, e.y, e.z, 0.3f}) * 0.6f;
    }
    const float s = view.selectionRadius(type);
    radius = std::max(radius, s);
    RTSCamera cam;
    cam.target = {0, std::min(height, radius * 1.4f) * 0.45f, 0};
    cam.distance = radius * 3.2f;
    cam.minDistance = 0.1f;
    cam.pitch = 0.32f;
    cam.yaw = 0.5f;
    cam.fovY = 0.6f;
    r3d_.beginScene(cam, size_, size_);
    view.drawPreview(r3d_, type, owner, time, yaw < 0 ? float(time) * 0.6f : yaw);
    gfx::PassDesc pass;
    pass.colorTarget = color_;
    pass.depthTarget = depth_;
    pass.clearColor[0] = 0.16f; pass.clearColor[1] = 0.18f; pass.clearColor[2] = 0.22f; pass.clearColor[3] = 1;
    r3d_.render(pass);
    dev_->endPass();
}

} // namespace rts::game
