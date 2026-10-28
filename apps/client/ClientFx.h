// Particle effects in the game client: one ParticleSystem + the RTS event bridge, fed from the
// match each frame. Kept in its own file so the client only calls a handful of methods.
#pragma once
#include "engine/fx/ParticleSystem.h"
#include "engine/render/Renderer3D.h"
#include "modules/rts/fx/RtsFx.h"
#include "modules/rts/view/RtsView.h"

namespace rts::client {

class ClientFx {
public:
    void init(gfx::Device& dev, const game::Rules& rules);
    void shutdown();
    void startMatch(Simulation& sim, uint8_t viewer, const game::RtsView& view, const game::MapGrid& grid);
    void stopMatch();
    // After Renderer3D::beginScene (needs its viewProj); dt = 0 while paused.
    void update(Simulation& sim, const game::RtsScene& scene, const render::RTSCamera& cam, const render::mat4& viewProj,
                int fbW, int fbH, float dt);
    // After Renderer3D::render(): draws into the still open main pass.
    void render(const render::Lighting& lighting);

    int quality() const { return int(ps_.quality()); }
    void setQuality(int q) { ps_.setQuality(fx::FxQuality(q < 0 ? 0 : q > 3 ? 3 : q)); }
    fx::ParticleSystem& particles() { return ps_; }

private:
    fx::ParticleSystem ps_;
    game::RtsFx bridge_;
    bool attached_ = false;
};

} // namespace rts::client
