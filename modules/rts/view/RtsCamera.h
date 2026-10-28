// Data-driven game camera: the optional "camera" block of game.json.
//
//   "camera": {"projection": "ortho", "pitch": 90, "yaw": 0, "fov": 43,
//              "zoom": 18, "zoomMin": 8, "zoomMax": 40, "pixelsPerCell": 32, "rotate": false}
//
// pitch/yaw/fov in degrees. Zoom values: visible map cells (vertically) for "ortho", camera
// distance for "perspective". "pixelsPerCell" (ortho) sets the start zoom so one cell is that many
// framebuffer pixels (pixel-perfect sprites). Without the block the engine default camera is kept.
#pragma once
#include <algorithm>
#include <cmath>
#include "../../../engine/data/Json.h"
#include "../../../engine/render/Math3D.h"

namespace rts::game {

struct CameraSetup {
    bool present = false, ortho = false, rotate = true;
    float pitch = 0.95f, yaw = 0, fovY = 0.75f;      // radians
    float zoom = -1, zoomMin = -1, zoomMax = -1;    // in the units above, -1 = keep
    float pixelsPerCell = 0;
};

inline CameraSetup cameraSetupFrom(const Json& manifest) {
    CameraSetup c;
    const Json& j = manifest["camera"];
    if (!j.isObject()) return c;
    constexpr float kDeg = 3.14159265358979f / 180.0f;
    auto num = [&](const char* k, float def) { return j[k].isNumber() ? float(j[k].fx().toDouble()) : def; };
    c.present = true;
    c.ortho = j.getStr("projection", "perspective") == "ortho";
    c.pitch = std::clamp(num("pitch", 54.4f), 5.0f, 90.0f) * kDeg;
    c.yaw = num("yaw", 0) * kDeg;
    c.fovY = std::clamp(num("fov", 43.0f), 5.0f, 120.0f) * kDeg;
    c.zoom = num("zoom", -1); c.zoomMin = num("zoomMin", -1); c.zoomMax = num("zoomMax", -1);
    c.pixelsPerCell = num("pixelsPerCell", 0);
    c.rotate = j.getBool("rotate", !c.ortho);
    return c;
}

// Converts a zoom value of the block into RTSCamera::distance.
inline float cameraDistanceFor(const CameraSetup& c, float zoom) {
    return c.ortho ? zoom * 0.5f / std::tan(c.fovY * 0.5f) : zoom;
}

// Applies projection, angles and zoom limits. Sets the zoom when the block gives one (zoom or
// pixelsPerCell) and returns true; otherwise the caller's framing distance stays.
inline bool applyCameraSetup(const CameraSetup& c, render::RTSCamera& cam, float viewHeightPx) {
    if (!c.present) return false;
    cam.ortho = c.ortho;
    cam.pitch = c.pitch;
    cam.yaw = c.yaw;
    cam.fovY = c.fovY;
    if (c.zoomMin > 0) cam.minDistance = cameraDistanceFor(c, c.zoomMin);
    if (c.zoomMax > 0) cam.maxDistance = cameraDistanceFor(c, c.zoomMax);
    float zoom = c.zoom;
    if (c.ortho && c.pixelsPerCell > 0 && viewHeightPx > 0) zoom = viewHeightPx / c.pixelsPerCell;
    if (zoom <= 0) { cam.distance = std::clamp(cam.distance, cam.minDistance, cam.maxDistance); return false; }
    cam.distance = std::clamp(cameraDistanceFor(c, zoom), cam.minDistance, std::max(cam.minDistance, cam.maxDistance));
    return true;
}

} // namespace rts::game
