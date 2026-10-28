// Game icons for every platform from game.json "icons" (or the old single "icon"), generated on
// the host with BrandImage (no sips/iconutil): iOS asset catalog, macOS .icns, Android mipmaps +
// adaptive icons, Windows .ico + .rc, web favicon / PWA manifest, Linux hicolor + .desktop.
// Format: docs/game-format.md "Icons and splash".
//
//   "icons": {
//     "default": "branding/icon.png",                 // master, square, >= 1024 px
//     "background": "#1b2a3a", "padding": 0.0, "cornerRadius": 0.0,   // defaults for opaque icons
//     "ios": "branding/icon_ios.png",                 // path or {"image", "background", "padding"}
//     "macos": {"margin": 0.098, "cornerRadius": 0.225},
//     "android": {"legacy": ..., "round": ..., "foreground": "branding/fg.png", "background": "#1b2a3a",
//                 "monochrome": "branding/mono.png", "foregroundPadding": 0.17},
//     "windows": ..., "web": ..., "linux": ...
//   }
#pragma once
#include <string>
#include <vector>

#include "engine/data/Json.h"
#include "tools/branding/BrandImage.h"

namespace rts::branding {

// How one platform's icon is drawn from an image: the icon square has a transparent `margin`
// around a body (rounded by cornerRadius, a fraction of the body size); the body is filled with
// `background` (if set) and holds the image inset by `padding` (fraction of the body).
struct IconLook {
    std::string image;            // project-relative; empty = the master
    Color background{0, 0, 0, 0};
    bool hasBackground = false;
    bool opaque = false;          // the platform forbids alpha: flattened on background (or the image's edge colour)
    bool circle = false;          // round mask instead of the rounded rectangle
    float padding = 0, margin = 0, cornerRadius = 0;
};

struct AndroidIcons {
    IconLook legacy, round;
    std::string foreground;       // adaptive foreground (108dp layer); empty = master with foregroundPadding
    float foregroundPadding = -1; // inset of the foreground art; -1: 0 for a given foreground, by the master otherwise
                                  // (logos on transparency 0.19, full-bleed art 0.17)
    std::string backgroundImage;  // adaptive background image, else backgroundColor
    Color backgroundColor{1, 1, 1, 1};
    bool hasBackgroundColor = false;
    std::string monochrome;       // Android 13 themed icon (alpha mask)
};

struct IconSpec {
    std::string master;           // project-relative path of the master image; empty = built-in default icon
    IconLook ios, macos, windows, web, webMaskable, appleTouch, linux;
    AndroidIcons android;
};

// Parses game.json["icons"] (falls back to "icon"). Throws std::runtime_error on malformed values
// ("icons.android.background: expected a colour or an image path").
IconSpec parseIcons(const Json& game);
// Checks the files: missing, smaller than 1024, not square, alpha where the platform forbids it.
// Returns readable warnings (never fatal).
std::vector<std::string> validateIcons(const IconSpec& spec, const std::string& projectDir);

// Loads a project image (or the built-in default icon when path is empty / missing; noted in warnings).
Image loadIconSource(const std::string& projectDir, const std::string& path, std::vector<std::string>* warnings);
Image defaultIcon(int size);      // a mushroom on a green tile (no art in the game yet)
// Renders one icon of `size` pixels with a look.
Image renderIcon(const Image& src, int size, const IconLook& look);

struct Generated {
    std::vector<std::string> files;     // written paths (absolute)
    std::vector<std::string> warnings;
};

struct GameMeta {
    std::string name = "Game", exe = "Game", id = "com.example.game", orientation = "landscape";
    std::string themeColor;             // web manifest theme (#rrggbb); default: splash background
    std::string backgroundColor;        // web manifest / launch background
};

// iOS: <xcassets>/AppIcon.appiconset (iPhone, iPad and App Store sizes) + Contents.json files.
void generateIOS(const IconSpec& s, const std::string& projectDir, const std::string& xcassetsDir, Generated& g);
// macOS: <genDir>/AppIcon.iconset (10 PNGs) and <genDir>/AppIcon.icns (written directly).
void generateMac(const IconSpec& s, const std::string& projectDir, const std::string& genDir, Generated& g);
// Android: <resDir>/mipmap-*/ic_launcher*.png, mipmap-anydpi-v26/ic_launcher(_round).xml,
// values/ic_launcher_background.xml; Play Store 512 at <playStorePath>.
void generateAndroid(const IconSpec& s, const std::string& projectDir, const std::string& resDir, const std::string& playStorePath, Generated& g);
// Windows: <genDir>/app.ico (16..256) and <genDir>/app.rc (icon resource 1 = the window class icon).
void generateWindows(const IconSpec& s, const std::string& projectDir, const std::string& genDir, Generated& g);
// Web: favicon.ico, favicon-32.png, apple-touch-icon.png, icons/icon-{192,512}.png,
// icons/maskable-{192,512}.png, manifest.webmanifest (PWA) in <outDir>.
void generateWeb(const IconSpec& s, const std::string& projectDir, const std::string& outDir, const GameMeta& m, Generated& g);
// <head> links for the files generateWeb writes (favicons, apple-touch-icon, PWA manifest, theme colour).
std::string webIconHead(const GameMeta& m);
// Linux: <outDir>/share/icons/hicolor/<n>x<n>/apps/<exe>.png and <outDir>/share/applications/<exe>.desktop.
void generateLinux(const IconSpec& s, const std::string& projectDir, const std::string& outDir, const GameMeta& m, Generated& g);

// Every icon a platform gets, as (label, image) — the editor's previews and icons_preview.
struct IconPreview { std::string platform, label; Image image; int size = 0; };
std::vector<IconPreview> previewIcons(const IconSpec& s, const std::string& projectDir, std::vector<std::string>* warnings);
// Contact sheet: one row per platform, every size at its real pixel size (plus masked variants:
// iOS rounded rect, Android circle / squircle), labelled.
Image iconsContactSheet(const IconSpec& s, const std::string& projectDir, std::vector<std::string>* warnings);

// Size tables (tests, docs).
struct IosIcon { const char* idiom; const char* size; const char* scale; int pixels; };
const std::vector<IosIcon>& iosIconSet();
const std::vector<int>& windowsIcoSizes();
const std::vector<int>& linuxIconSizes();

} // namespace rts::branding
