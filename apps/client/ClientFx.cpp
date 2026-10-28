#include "ClientFx.h"
#include <cstdio>
#include "engine/platform/Platform.h"

namespace rts::client {

void ClientFx::init(gfx::Device& dev, const game::Rules& rules) {
    ps_.setQuality(fx::defaultQualityFor(dev, platform::isTouchDevice()));
    ps_.setFileReader([](const std::string& path, std::string& out) { return platform::readContentFile(path, out); });
    // Sprite textures for particles: connect the engine's sprite loader with ps_.setTextureLoader(...)
    // (built-in textures work without it).
    if (!ps_.init(&dev)) std::fprintf(stderr, "fx: %s\n", ps_.lastError().c_str());
    std::string text;
    Json bank;
    if (platform::readContentFile("fx/fx.json", text)) {   // optional: games without effects have none
        try { bank = Json::parse(text); }
        catch (const std::exception& e) { std::fprintf(stderr, "fx/fx.json: %s\n", e.what()); }
    }
    std::string err;
    if (!bridge_.init(ps_, rules, bank, &err)) std::fprintf(stderr, "%s\n", err.c_str());
    for (auto& w : bridge_.warnings()) std::fprintf(stderr, "fx: %s\n", w.c_str());
}

void ClientFx::shutdown() {
    stopMatch();
    ps_.shutdown();
}

void ClientFx::startMatch(Simulation& sim, uint8_t viewer, const game::RtsView& view, const game::MapGrid& grid) {
    ps_.clear();
    bridge_.setHeightFn([&view, &grid](float x, float z) { return view.heightAt(grid, x, z); });
    bridge_.setPlayerColorFn([](int p) { return game::RtsView::playerColor(p); });
    bridge_.attach(sim, viewer);
    attached_ = true;
}

void ClientFx::stopMatch() {
    if (attached_) bridge_.detach();
    attached_ = false;
    ps_.clear();
}

void ClientFx::update(Simulation& sim, const game::RtsScene& scene, const render::RTSCamera& cam, const render::mat4& viewProj,
                      int fbW, int fbH, float dt) {
    ps_.setView(cam, viewProj, fbW, fbH);
    if (attached_ && dt > 0) bridge_.update(sim, &scene.objects);
    ps_.update(dt);
}

void ClientFx::render(const render::Lighting& lighting) { ps_.render({lighting.fogColor, lighting.fogDensity}); }

} // namespace rts::client
