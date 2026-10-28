// Orbit camera for planet maps: from a near-surface RTS view (local up = screen up) over the whole planet
// to the whole star system, with smooth transitions and focus changes between bodies.
//
// The renderer works in a "view world": world space moved and rotated so that the focus point on the
// surface is the origin and its local up is +Y. Then the classic RTSCamera, shadow fitting, light map
// and overlays work unchanged near the focus; far away the planet simply curves down.
//   view = rot * (world - origin)      (PlanetViewFrame)
// Zoom regimes by camera distance d (from the focus point): surface (target on the ground, local
// frame) -> planet (target slides to the body centre) -> system (target slides to the system centre,
// frame turns to the orbital plane).
#pragma once
#include <algorithm>
#include <cmath>
#include "../../../engine/render/Math3D.h"
#include "RtsPlanet.h"

namespace rts::game {

struct PlanetViewFrame {
    render::float3 origin;               // world point mapped to the view-world origin
    render::float3 right{1, 0, 0}, up{0, 1, 0}, back{0, 0, 1};   // world directions of view X / Y / Z
    render::float3 toView(render::float3 w) const { render::float3 d = w - origin; return {dot(d, right), dot(d, up), dot(d, back)}; }
    render::float3 dirToView(render::float3 d) const { return {dot(d, right), dot(d, up), dot(d, back)}; }
    render::float3 toWorld(render::float3 v) const { return origin + right * v.x + up * v.y + back * v.z; }
    render::float3 dirToWorld(render::float3 v) const { return right * v.x + up * v.y + back * v.z; }
    // world -> view world as a matrix (for chunk / model transforms)
    render::mat4 matrix() const {
        render::mat4 m;
        const render::float3 t = {-dot(origin, right), -dot(origin, up), -dot(origin, back)};
        m.at(0, 0) = right.x; m.at(0, 1) = right.y; m.at(0, 2) = right.z; m.at(0, 3) = t.x;
        m.at(1, 0) = up.x;    m.at(1, 1) = up.y;    m.at(1, 2) = up.z;    m.at(1, 3) = t.y;
        m.at(2, 0) = back.x;  m.at(2, 1) = back.y;  m.at(2, 2) = back.z;  m.at(2, 3) = t.z;
        return m;
    }
};

class PlanetCamera {
public:
    float pitch = 0.95f;          // radians above the local horizon
    float distance = 24;          // from the focus point
    float minDistance = 6;
    float fovY = 0.75f;

    // Start over `body` at the centre of face 2 (or at `dir`), looking along the face's -v ("north").
    void reset(const PlanetWorld& w, int body, const PlanetSurface* s, double seconds, render::float3 dir = {0, 0, 0}) {
        using namespace render;
        body_ = body;
        dir_ = dot(dir, dir) > 0 ? normalize(dir) : s ? s->dirFromFace(2, s->faceSize() * 0.5f, s->faceSize() * 0.5f) : float3{0, 1, 0};
        fwd_ = tangent(float3{0, 0, -1});
        distance = 24; pitch = 0.95f;
        snap_ = true; init_ = false;
        update(w, s, seconds, 0);
    }
    int body() const { return body_; }
    render::float3 dir() const { return dir_; }
    render::float3 forward() const { return fwd_; }
    void setDir(render::float3 d) { using namespace render; const float3 n = normalize(d); transport(n); }
    // Focus another body (smooth transition of the frame).
    void focus(int body) { if (body != body_) { body_ = body; transition_ = true; } }
    bool inTransition() const { return transition_; }

    // Screen-space drag in pixels (viewport height h): the ground follows the cursor near the surface;
    // at planet zoom the globe spins.
    void pan(float dx, float dy, float h) {
        if (h <= 0) return;
        const float worldPerPx = 2.0f * std::tan(fovY * 0.5f) * std::max(distance * (1.0f - planetW_) + radius_ * planetW_ * 1.2f, 1.0f) / h;
        move(-dx * worldPerPx, dy * worldPerPx / std::max(std::sin(pitch), 0.35f) * (1.0f - planetW_) + dy * worldPerPx * planetW_);
    }
    // Move the focus over the surface by (right, forward) world units at the surface radius. The
    // forward vector is parallel-transported: no singular poles or seams.
    void move(float right, float forward) {
        using namespace render;
        const float R = std::max(radius_, 1.0f);
        const float3 r = cross(fwd_, dir_);
        transport(normalize(dir_ + r * (right / R) + fwd_ * (forward / R)));
    }
    void zoom(float factor) { distance = std::clamp(distance * factor, minDistance, maxDistance_); }
    void rotate(float dHeading) {   // around the local up
        using namespace render;
        const float c = std::cos(dHeading), sn = std::sin(dHeading);
        fwd_ = normalize(fwd_ * c + cross(dir_, fwd_) * sn);
    }
    float maxDistance() const { return maxDistance_; }
    float planetBlend() const { return planetW_; }   // 0 surface .. 1 whole planet
    float systemBlend() const { return systemW_; }   // 0 planet .. 1 system

    // Per frame: smooth focus changes, build the RTSCamera (view world) and the frame.
    void update(const PlanetWorld& w, const PlanetSurface* s, double seconds, float dt) {
        using namespace render;
        if (body_ < 0 || body_ >= int(w.bodies.size())) body_ = w.start;
        if (body_ < 0 || body_ >= int(w.bodies.size())) return;
        const PlanetBody& b = w.bodies[size_t(body_)];
        radius_ = b.radius;
        int root = 0;   // system centre: the first body without an orbit
        for (size_t i = 0; i < w.bodies.size(); ++i) if (w.bodies[i].parent < 0) { root = int(i); break; }
        const float3 sysC = w.center(root, seconds);
        float extent = b.radius * 4;
        for (size_t i = 0; i < w.bodies.size(); ++i) extent = std::max(extent, length(w.center(int(i), seconds) - sysC) + w.bodies[i].radius);
        maxDistance_ = extent * 2.6f;
        distance = std::clamp(distance, minDistance, maxDistance_);
        const float ground = s ? std::max(s->heightAt(dir_), b.seaLevel > -1e8f ? b.seaLevel : -1e9f) : 0.0f;
        const float3 c = w.center(body_, seconds);
        const float3 surf = c + dir_ * (b.radius + ground);
        auto smooth = [](float e0, float e1, float x) { float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); };
        planetW_ = smooth(b.radius * 0.3f, b.radius * 2.2f, distance);
        systemW_ = smooth(b.radius * 5.0f, std::max(b.radius * 6.0f, extent * 0.9f), distance);
        // desired frame: local (up = dir, back = -forward), turned into the orbital plane at system zoom
        float3 U = dir_, B = -fwd_;
        if (systemW_ > 0) {
            float3 gb = float3{-fwd_.x, 0, -fwd_.z};
            gb = dot(gb, gb) > 1e-6f ? normalize(gb) : float3{0, 0, 1};
            U = normalize(lerp(U, float3{0, 1, 0}, systemW_));
            B = normalize(lerp(B, gb, systemW_));
        }
        B = normalize(B - U * dot(B, U));
        const float3 targetW = lerp(lerp(surf, c, planetW_), sysC, systemW_);
        // smoothing: frame and target follow exponentially (focus / regime changes); position snaps
        // while panning on the same body so the ground stays under the cursor
        const float k = snap_ || dt <= 0 ? 1.0f : 1.0f - std::exp(-dt * 7.0f);
        if (!init_) { cur_.origin = targetW; cur_.up = U; cur_.back = B; init_ = true; }
        const bool follow = !transition_;   // panning / zooming on one body: no lag
        cur_.origin = follow ? targetW : lerp(cur_.origin, targetW, k);
        cur_.up = follow ? U : normalize(lerp(cur_.up, U, k));
        cur_.back = follow ? B : normalize(lerp(cur_.back, B, k));
        if (transition_ && length(cur_.origin - targetW) < std::max(b.radius * 0.01f, 0.05f) && dot(cur_.up, U) > 0.9999f) transition_ = false;
        cur_.back = normalize(cur_.back - cur_.up * dot(cur_.back, cur_.up));
        cur_.right = cross(cur_.up, cur_.back);
        snap_ = false;
        frame_ = cur_;
        cam_.target = {0, 0, 0};
        cam_.distance = distance;
        cam_.pitch = std::clamp(pitch, 0.05f, 1.5607f);
        cam_.yaw = 0;
        cam_.fovY = fovY;
        cam_.ortho = false;
        cam_.minDistance = minDistance; cam_.maxDistance = maxDistance_;
        const float3 eyeW = frame_.toWorld(cam_.eye());
        const float alt = std::max(length(eyeW - c) - b.radius - std::max(0.0f, ground), 0.5f);
        cam_.zNear = std::clamp(alt * 0.08f, 0.25f, 200.0f);
        cam_.zFar = std::max(1000.0f, distance + extent * 2.5f);
    }
    const render::RTSCamera& camera() const { return cam_; }
    const PlanetViewFrame& frame() const { return frame_; }

private:
    render::float3 tangent(render::float3 hint) const {
        using namespace render;
        float3 t = hint - dir_ * dot(hint, dir_);
        if (dot(t, t) < 1e-6f) { t = float3{1, 0, 0} - dir_ * dir_.x; }
        return normalize(t);
    }
    // move the focus to `n`, rotating the forward vector with the same rotation (parallel transport)
    void transport(render::float3 n) {
        using namespace render;
        const float3 axis = cross(dir_, n);
        const float sn = length(axis), cs = dot(dir_, n);
        if (sn > 1e-7f) {
            const float3 k = axis * (1.0f / sn);
            fwd_ = fwd_ * cs + cross(k, fwd_) * sn + k * (dot(k, fwd_) * (1.0f - cs));
        }
        dir_ = n;
        fwd_ = tangent(fwd_);
    }
    int body_ = 0;
    render::float3 dir_{0, 1, 0}, fwd_{0, 0, -1};
    float radius_ = 1, maxDistance_ = 1000, planetW_ = 0, systemW_ = 0;
    bool snap_ = true, init_ = false, transition_ = false;
    PlanetViewFrame cur_, frame_;
    render::RTSCamera cam_;
};

} // namespace rts::game
