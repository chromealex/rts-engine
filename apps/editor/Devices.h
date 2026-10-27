// Device presets for the Game window simulator and device runs.
#pragma once
#include <string>

namespace rts::editor {

struct DeviceProfile {
    const char* id;
    const char* name;
    int w, h;                    // logical points, portrait (w < h) or desktop landscape
    float scale;                 // pixels per point
    float safeTop, safeBottom;   // portrait safe-area insets (points)
    float corner;                // screen corner radius (points)
    float notchW, notchH;        // camera cut-out at the top in portrait (points), 0 = none
    bool touch;
    int gpuTier;                 // 0 weak, 1 mid, 2 high -> shadow quality
};

inline const DeviceProfile kDevices[] = {
    {"none", "Свободный размер", 0, 0, 1, 0, 0, 0, 0, 0, false, 2},
    {"desktop-1080", "ПК 1920×1080", 1920, 1080, 1, 0, 0, 0, 0, 0, false, 2},
    {"desktop-1440", "ПК 2560×1440", 2560, 1440, 1, 0, 0, 0, 0, 0, false, 2},
    {"iphone-15", "iPhone 15", 393, 852, 3, 59, 34, 55, 126, 37, true, 2},
    {"iphone-se", "iPhone SE", 375, 667, 2, 20, 0, 0, 0, 0, true, 1},
    {"ipad", "iPad 10.9\"", 820, 1180, 2, 24, 20, 18, 0, 0, true, 2},
    {"pixel-8", "Pixel 8", 412, 915, 2.625f, 40, 24, 30, 24, 24, true, 2},
    {"android-low", "Слабый Android 720p", 360, 720, 2, 24, 0, 6, 0, 0, true, 0},
};

inline const DeviceProfile* findDevice(const std::string& id) {
    for (auto& d : kDevices) if (id == d.id) return &d;
    return nullptr;
}

} // namespace rts::editor
