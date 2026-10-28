// Gallery mode of rts_modelview: every type of a game in a row, drawn by the game's own view
// (RtsView: view blocks, material overrides, clips, procedural animation), driven by a scripted
// scene that cycles each object through idle -> move -> attack (+ hit) -> death -> respawn.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../../modules/rts/view/RtsView.h"

namespace rts::modelview {

class Gallery {
public:
    // dir: game folder (contains rules.json). types: ids to show (empty = all).
    bool init(gfx::Device& dev, const std::string& dir, const std::vector<std::string>& types, std::string& err);
    void shutdown(gfx::Device& dev);
    void update(double time);
    void draw(render::Renderer3D& r, double time);
    render::float3 center() const { return center_; }
    float extent() const { return extent_; }
    const std::vector<std::string>& warnings() const { return view_.warnings(); }

private:
    struct Slot { uint16_t type; render::float3 base; uint32_t gen = 0; float lastPhase = -1; bool alive = true; };
    game::Rules rules_;
    game::RtsView view_;
    game::MapGrid grid_;
    game::RtsScene scene_;
    std::vector<Slot> slots_;
    std::string dir_;
    render::float3 center_;
    float extent_ = 10;
    uint32_t tick_ = 0;
};

} // namespace rts::modelview
