// Planet maps in the client: orbit camera input (mouse, keys, touch) on top of the regular RTS input.
// Flat maps never reach this file (GameClient::planet() is false without a map `world` block).
//   drag middle button / one-finger drag on empty ground: rotate over the surface (spin the globe)
//   wheel / pinch: zoom surface -> planet -> system;  Q / E or two-finger twist: turn around the local up
//   arrows / WASD (no selection) / screen edges: move over the surface;  Tab: focus the next body
//   R / F (PageUp / PageDown): camera pitch
#include "GameClient.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rts::client {

using platform::Event;
using platform::EventType;
using platform::Key;

bool GameClient::objectScreen(const SceneObject& o, float lift, float2& sp) const {
    const float W = platform::windowWidth(), H = platform::windowHeight();
    if (!planet()) return cam_.worldToScreen({o.pos.x, lift, o.pos.z}, W, H, sp);
    return cam_.worldToScreen(view_.objectViewPos(o, lift), W, H, sp);
}

// Start over the player's base on the start body. RTS_PLANET_CAM="distance[,pitch deg[,body id]]"
// overrides the start view (screenshots of the planet / system zoom levels).
void GameClient::fitPlanetCamera() {
    PlanetCamera& pc = view_.planetCamera();
    const PlanetWorld& W = view_.planets().world();
    int body = W.start;
    float3 dir{0, 0, 0};
    for (auto& o : scene_.objects) if (o.owner == human_ && rules_.types[o.type].structure) {
        int b; float3 local;
        if (view_.flatToBody(o.pos, b, local)) { body = b; dir = local; }
        break;
    }
    float dist = -1, pitchDeg = -1;
    char id[64] = {};
    if (const char* e = std::getenv("RTS_PLANET_CAM")) {
        std::sscanf(e, "%f,%f,%63s", &dist, &pitchDeg, id);
        if (id[0] && W.find(id) >= 0) { body = W.find(id); dir = {0, 0, 0}; }
    }
    pc.reset(W, body, view_.planets().surface(body), scene_.seconds, dir);
    if (dist > 0) pc.distance = dist;
    if (pitchDeg > 0) pc.pitch = pitchDeg * 3.14159265f / 180.0f;
}

void GameClient::planetCameraKeys(double dt) {
    PlanetCamera& pc = view_.planetCamera();
    auto key = [&](Key k) { return keys_[int(k)]; };
    const float W = platform::windowWidth(), H = platform::windowHeight();
    const bool edges = !platform::isTouchDevice() && std::string(platform::name()) != "web" && mouseX_ >= 0 && mouseY_ >= 0 && mouseX_ <= W && mouseY_ <= H;
    const bool noSel = selection_.empty();
    if (ui_.textInputFocused()) return;
    const float speed = std::max(pc.distance, 6.0f) * 0.9f * float(dt) * (1.0f - 0.6f * pc.planetBlend());
    if (key(Key::Up) || (noSel && key(Key::W)) || (edges && mouseY_ < 3)) pc.move(0, speed);
    if (key(Key::Down) || (noSel && key(Key::S)) || (edges && mouseY_ > H - 3)) pc.move(0, -speed);
    if (key(Key::Right) || (noSel && key(Key::D)) || (edges && mouseX_ > W - 3)) pc.move(speed, 0);
    if (key(Key::Left) || (noSel && key(Key::A)) || (edges && mouseX_ < 3)) pc.move(-speed, 0);
    if (key(Key::Q)) pc.rotate(float(dt) * 1.6f);
    if (key(Key::E)) pc.rotate(-float(dt) * 1.6f);
    if (key(Key::R) || key(Key::PageUp)) pc.pitch = std::min(pc.pitch + float(dt), 1.55f);
    if (key(Key::F) || key(Key::PageDown)) pc.pitch = std::max(pc.pitch - float(dt), 0.25f);
}

// Returns true when the event was consumed by the camera.
bool GameClient::planetCameraEvent(const Event& e, bool uiWants) {
    PlanetCamera& pc = view_.planetCamera();
    const float H = platform::windowHeight();
    switch (e.type) {
    case EventType::MouseDown:
        if (uiWants || e.button != 2) return false;
        downX_ = e.x; downY_ = e.y; panning_ = true;
        return true;
    case EventType::MouseMove:
        if (!panning_) return false;
        pc.pan(e.x - downX_, e.y - downY_, H);
        downX_ = e.x; downY_ = e.y;
        hover_ = kNullEntity;
        return true;
    case EventType::MouseUp:
        if (e.button != 2 || !panning_) return false;
        panning_ = false;
        return true;
    case EventType::MouseWheel:
        if (uiWants) return true;
        pc.zoom(std::pow(0.88f, e.wheelY));
        return true;
    case EventType::KeyDown:
        if (uiWants) return false;
        if (e.key == Key::Tab) { // next body with a surface (stars cannot be focused)
            const auto& W = view_.planets().world();
            for (size_t k = 1; k <= W.bodies.size(); ++k) {
                const int b = int((size_t(pc.body()) + k) % W.bodies.size());
                if (W.bodies[size_t(b)].surface()) { pc.focus(b); break; }
            }
            return true;
        }
        if (e.key == Key::Space) { fitCamera(); return true; }
        return false;
    case EventType::TouchMove: {
        if (!touches_.count(e.touchId)) return false;
        if (touches_.size() == 1 && !sendDrag_) {
            const auto prev = touches_[e.touchId];
            touches_[e.touchId] = {e.x, e.y};
            if (std::hypot(e.x - downX_, e.y - downY_) > 12) touchMoved_ = true;
            mouseX_ = e.x; mouseY_ = e.y;
            if (touchMoved_) pc.pan(e.x - prev.first, e.y - prev.second, H);
            return true;
        }
        if (touches_.size() == 2) {
            auto it = touches_.begin();
            const auto a0 = it->second, b0 = std::next(it)->second;
            touches_[e.touchId] = {e.x, e.y};
            it = touches_.begin();
            const auto a = it->second, b = std::next(it)->second;
            const float d = std::hypot(a.first - b.first, a.second - b.second);
            if (pinchDist_ > 1 && d > 1) pc.zoom(pinchDist_ / d);
            pinchDist_ = d;
            const float ang0 = std::atan2(b0.second - a0.second, b0.first - a0.first), ang1 = std::atan2(b.second - a.second, b.first - a.first);
            float da = ang1 - ang0;
            if (da > 3.14159f) da -= 6.28318f;
            if (da < -3.14159f) da += 6.28318f;
            pc.rotate(da);
            const float cx = (a.first + b.first) * 0.5f, cy = (a.second + b.second) * 0.5f;
            pc.pan(cx - downX_, cy - downY_, H);
            downX_ = cx; downY_ = cy;
            touchMoved_ = true;
            return true;
        }
        return false;
    }
    default: return false;
    }
}

} // namespace rts::client
