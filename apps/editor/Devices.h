// Device presets for the Game window simulator and device runs. Sizes are logical points (iOS)
// or dp (Android) in the device's natural orientation; insets are the system safe area
// (status bar / cut-out / home indicator) as the OS reports it, so a HUD can be checked against it.
#pragma once
#include <string>

namespace rts::editor {

enum class Cutout : unsigned char { None, Notch, Island, PunchCenter, PunchLeft, Waterdrop };

struct Insets4 { float left = 0, top = 0, right = 0, bottom = 0; };

struct DeviceProfile {
    const char* id;
    const char* name;
    const char* group;           // section in the device menu
    int w, h;                    // logical points, natural orientation (phones: portrait)
    float scale;                 // pixels per point
    Insets4 portrait;            // safe area in the natural orientation
    Insets4 landscape;           // safe area rotated 90° (cut-out on the left)
    float corner;                // screen corner radius (points)
    Cutout cutout;
    float cutW, cutH, cutY;      // cut-out size and distance from the top edge (points)
    bool touch;
    int gpuTier;                 // 0 weak, 1 mid, 2 high -> shadow quality
};

// Insets shorthands: phone with a cut-out (iOS style: both landscape sides inset), Android
// (cut-out side only), and devices without system bars.
constexpr Insets4 ins(float l, float t, float r, float b) { return {l, t, r, b}; }

inline const DeviceProfile kDevices[] = {
    {"none", "Свободный размер", "", 0, 0, 1, {}, {}, 0, Cutout::None, 0, 0, 0, false, 2},

    // ---------------------------------------------------------------- iPhone
    {"iphone-se", "iPhone SE (2/3)", "iPhone", 375, 667, 2, ins(0, 20, 0, 0), ins(0, 0, 0, 0), 0, Cutout::None, 0, 0, 0, true, 1},
    {"iphone-8-plus", "iPhone 8 Plus", "iPhone", 414, 736, 3, ins(0, 20, 0, 0), ins(0, 0, 0, 0), 0, Cutout::None, 0, 0, 0, true, 1},
    {"iphone-x", "iPhone X / XS / 11 Pro", "iPhone", 375, 812, 3, ins(0, 44, 0, 34), ins(44, 0, 44, 21), 39, Cutout::Notch, 209, 30, 0, true, 1},
    {"iphone-xr", "iPhone XR / 11", "iPhone", 414, 896, 2, ins(0, 48, 0, 34), ins(48, 0, 48, 21), 41, Cutout::Notch, 230, 33, 0, true, 1},
    {"iphone-xs-max", "iPhone XS Max / 11 Pro Max", "iPhone", 414, 896, 3, ins(0, 44, 0, 34), ins(44, 0, 44, 21), 39, Cutout::Notch, 209, 30, 0, true, 2},
    {"iphone-mini", "iPhone 12 / 13 mini", "iPhone", 375, 812, 3, ins(0, 50, 0, 34), ins(50, 0, 50, 21), 44, Cutout::Notch, 226, 34, 0, true, 2},
    {"iphone-12", "iPhone 12 / 13 / 14 / 16e", "iPhone", 390, 844, 3, ins(0, 47, 0, 34), ins(47, 0, 47, 21), 47, Cutout::Notch, 210, 32, 0, true, 2},
    {"iphone-12-pro-max", "iPhone 12/13 Pro Max, 14 Plus", "iPhone", 428, 926, 3, ins(0, 47, 0, 34), ins(47, 0, 47, 21), 53, Cutout::Notch, 210, 32, 0, true, 2},
    {"iphone-15", "iPhone 14 Pro / 15 / 15 Pro / 16", "iPhone", 393, 852, 3, ins(0, 59, 0, 34), ins(59, 0, 59, 21), 55, Cutout::Island, 126, 37, 11, true, 2},
    {"iphone-15-plus", "iPhone 14 Pro Max / 15 Plus / 15 Pro Max / 16 Plus", "iPhone", 430, 932, 3, ins(0, 59, 0, 34), ins(59, 0, 59, 21), 55, Cutout::Island, 126, 37, 11, true, 2},
    {"iphone-16-pro", "iPhone 16 Pro / 17 Pro", "iPhone", 402, 874, 3, ins(0, 62, 0, 34), ins(62, 0, 62, 21), 62, Cutout::Island, 126, 37, 14, true, 2},
    {"iphone-16-pro-max", "iPhone 16 Pro Max / 17 Pro Max", "iPhone", 440, 956, 3, ins(0, 62, 0, 34), ins(62, 0, 62, 21), 62, Cutout::Island, 126, 37, 14, true, 2},

    // ---------------------------------------------------------------- iPad
    {"ipad-9", "iPad 9 (10.2\")", "iPad", 810, 1080, 2, ins(0, 20, 0, 0), ins(0, 20, 0, 0), 0, Cutout::None, 0, 0, 0, true, 1},
    {"ipad-mini", "iPad mini 6/7", "iPad", 744, 1133, 2, ins(0, 24, 0, 20), ins(0, 24, 0, 20), 21, Cutout::None, 0, 0, 0, true, 2},
    {"ipad", "iPad 10/11 (10.9\")", "iPad", 820, 1180, 2, ins(0, 24, 0, 20), ins(0, 24, 0, 20), 18, Cutout::None, 0, 0, 0, true, 2},
    {"ipad-air-13", "iPad Air 13\"", "iPad", 1024, 1366, 2, ins(0, 24, 0, 20), ins(0, 24, 0, 20), 18, Cutout::None, 0, 0, 0, true, 2},
    {"ipad-pro-11", "iPad Pro 11\"", "iPad", 834, 1210, 2, ins(0, 24, 0, 20), ins(0, 24, 0, 20), 18, Cutout::None, 0, 0, 0, true, 2},
    {"ipad-pro-13", "iPad Pro 13\" (M4)", "iPad", 1032, 1376, 2, ins(0, 24, 0, 20), ins(0, 24, 0, 20), 18, Cutout::None, 0, 0, 0, true, 2},

    // ---------------------------------------------------------------- Android: Google
    {"pixel-6a", "Pixel 6a", "Android — Google", 412, 892, 2.625f, ins(0, 32, 0, 16), ins(32, 0, 0, 16), 28, Cutout::PunchCenter, 22, 22, 8, true, 1},
    {"pixel-7", "Pixel 7 / 8", "Android — Google", 412, 915, 2.625f, ins(0, 40, 0, 24), ins(40, 0, 0, 24), 30, Cutout::PunchCenter, 24, 24, 10, true, 2},
    {"pixel-8-pro", "Pixel 8 Pro / 9 Pro XL", "Android — Google", 448, 998, 3, ins(0, 44, 0, 24), ins(44, 0, 0, 24), 32, Cutout::PunchCenter, 24, 24, 12, true, 2},
    {"pixel-9", "Pixel 9 / 9 Pro", "Android — Google", 412, 923, 2.625f, ins(0, 44, 0, 24), ins(44, 0, 0, 24), 32, Cutout::PunchCenter, 24, 24, 12, true, 2},
    {"pixel-fold", "Pixel 9 Pro Fold (раскрыт)", "Android — Google", 841, 891, 2.5f, ins(0, 32, 0, 24), ins(0, 32, 0, 24), 24, Cutout::PunchLeft, 20, 20, 10, true, 2},

    // ---------------------------------------------------------------- Android: Samsung
    {"galaxy-s21", "Galaxy S21", "Android — Samsung", 360, 800, 3, ins(0, 32, 0, 16), ins(32, 0, 0, 16), 24, Cutout::PunchCenter, 20, 20, 8, true, 2},
    {"galaxy-s24", "Galaxy S22 / S23 / S24", "Android — Samsung", 360, 780, 3, ins(0, 36, 0, 16), ins(36, 0, 0, 16), 26, Cutout::PunchCenter, 20, 20, 9, true, 2},
    {"galaxy-s24-plus", "Galaxy S24+", "Android — Samsung", 384, 832, 3.75f, ins(0, 36, 0, 16), ins(36, 0, 0, 16), 26, Cutout::PunchCenter, 20, 20, 9, true, 2},
    {"galaxy-s24-ultra", "Galaxy S24 Ultra", "Android — Samsung", 384, 824, 3.75f, ins(0, 36, 0, 16), ins(36, 0, 0, 16), 8, Cutout::PunchCenter, 20, 20, 9, true, 2},
    {"galaxy-a15", "Galaxy A14 / A15 / A25", "Android — Samsung", 384, 854, 2.8125f, ins(0, 32, 0, 48), ins(32, 0, 0, 48), 20, Cutout::Waterdrop, 30, 26, 0, true, 0},
    {"galaxy-a54", "Galaxy A54 / A55", "Android — Samsung", 384, 854, 2.8125f, ins(0, 36, 0, 16), ins(36, 0, 0, 16), 24, Cutout::PunchCenter, 20, 20, 9, true, 1},
    {"galaxy-fold-cover", "Galaxy Z Fold5 (внешний)", "Android — Samsung", 344, 882, 2.625f, ins(0, 32, 0, 16), ins(32, 0, 0, 16), 20, Cutout::PunchCenter, 18, 18, 8, true, 2},
    {"galaxy-fold-inner", "Galaxy Z Fold5 (раскрыт)", "Android — Samsung", 690, 842, 2.625f, ins(0, 32, 0, 16), ins(0, 32, 0, 16), 20, Cutout::None, 0, 0, 0, true, 2},
    {"galaxy-flip", "Galaxy Z Flip5", "Android — Samsung", 412, 1004, 2.625f, ins(0, 36, 0, 16), ins(36, 0, 0, 16), 28, Cutout::PunchCenter, 20, 20, 9, true, 2},
    {"galaxy-tab-s9", "Galaxy Tab S9", "Android — Samsung", 800, 1280, 2, ins(0, 24, 0, 24), ins(0, 24, 0, 24), 16, Cutout::None, 0, 0, 0, true, 2},
    {"galaxy-tab-a8", "Galaxy Tab A8 (слабый)", "Android — Samsung", 800, 1280, 1.5f, ins(0, 24, 0, 48), ins(0, 24, 0, 48), 8, Cutout::None, 0, 0, 0, true, 0},

    // ---------------------------------------------------------------- Android: other
    {"redmi-note-13", "Redmi Note 13 / Poco X6", "Android — другие", 393, 873, 2.75f, ins(0, 34, 0, 16), ins(34, 0, 0, 16), 28, Cutout::PunchCenter, 20, 20, 9, true, 1},
    {"redmi-13c", "Redmi 13C / A-серия (слабый)", "Android — другие", 360, 800, 2, ins(0, 30, 0, 48), ins(30, 0, 0, 48), 20, Cutout::Waterdrop, 30, 26, 0, true, 0},
    {"oneplus-12", "OnePlus 12", "Android — другие", 412, 919, 3.5f, ins(0, 40, 0, 16), ins(40, 0, 0, 16), 30, Cutout::PunchLeft, 22, 22, 10, true, 2},
    {"moto-g", "Moto G (720p)", "Android — другие", 360, 800, 2, ins(0, 28, 0, 48), ins(28, 0, 0, 48), 18, Cutout::PunchLeft, 18, 18, 8, true, 0},
    {"android-low", "Слабый Android 720p (без выреза)", "Android — другие", 360, 720, 2, ins(0, 24, 0, 48), ins(0, 24, 0, 48), 6, Cutout::None, 0, 0, 0, true, 0},
    {"android-tiny", "Очень слабый 480×854", "Android — другие", 320, 569, 1.5f, ins(0, 24, 0, 48), ins(0, 24, 0, 48), 0, Cutout::None, 0, 0, 0, true, 0},

    // ---------------------------------------------------------------- desktop / handheld
    {"desktop-720", "ПК 1280×720", "ПК и консоли", 1280, 720, 1, {}, {}, 0, Cutout::None, 0, 0, 0, false, 1},
    {"desktop-768", "Ноутбук 1366×768", "ПК и консоли", 1366, 768, 1, {}, {}, 0, Cutout::None, 0, 0, 0, false, 1},
    {"desktop-1080", "ПК 1920×1080", "ПК и консоли", 1920, 1080, 1, {}, {}, 0, Cutout::None, 0, 0, 0, false, 2},
    {"desktop-1440", "ПК 2560×1440", "ПК и консоли", 2560, 1440, 1, {}, {}, 0, Cutout::None, 0, 0, 0, false, 2},
    {"desktop-ultrawide", "Ultrawide 3440×1440", "ПК и консоли", 3440, 1440, 1, {}, {}, 0, Cutout::None, 0, 0, 0, false, 2},
    {"desktop-4k", "4K 3840×2160 (×2)", "ПК и консоли", 1920, 1080, 2, {}, {}, 0, Cutout::None, 0, 0, 0, false, 2},
    {"macbook-air", "MacBook Air 13\"", "ПК и консоли", 1470, 956, 2, ins(0, 32, 0, 0), ins(0, 32, 0, 0), 10, Cutout::Notch, 200, 32, 0, false, 2},
    {"steam-deck", "Steam Deck", "ПК и консоли", 1280, 800, 1, {}, {}, 0, Cutout::None, 0, 0, 0, true, 1},
};

inline const DeviceProfile* findDevice(const std::string& id) {
    for (auto& d : kDevices) if (id == d.id) return &d;
    return nullptr;
}

// Safe area of a device in the given orientation (rotated profile; desktop: as is).
inline Insets4 deviceInsets(const DeviceProfile& d, bool landscape) {
    const bool naturalPortrait = d.w < d.h;
    return naturalPortrait == !landscape ? d.portrait : d.landscape;
}

} // namespace rts::editor
