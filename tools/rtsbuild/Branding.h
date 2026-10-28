// rtsbuild: icons and launch screens per target (game.json "icons" + "splash", tools/branding).
// Everything is generated on the host without platform tools, so every target's files can be
// produced (and checked) from any OS; iconutil is used on a Mac only to re-pack the .icns.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "engine/data/Json.h"

namespace rts::build {

struct BrandingContext {
    std::filesystem::path project;
    Json game;
    std::string name, exe, id, orientation;
};

// Each returns the files it wrote (absolute) and prints warnings through `info`.
using InfoFn = void (*)(const std::string&);
std::vector<std::string> brandIOS(const BrandingContext& c, const std::filesystem::path& genDir, InfoFn info);   // gen/Assets.xcassets, gen/LaunchScreen.storyboard
std::vector<std::string> brandMac(const BrandingContext& c, const std::filesystem::path& genDir, InfoFn info);   // gen/AppIcon.iconset, gen/AppIcon.icns
// Android: <project>/app/src/main/res/... + ic_launcher-playstore.png, patches AndroidManifest.xml
// (roundIcon, @style/RtsLaunchTheme).
std::vector<std::string> brandAndroid(const BrandingContext& c, const std::filesystem::path& gradleProject, InfoFn info);
std::vector<std::string> brandWindows(const BrandingContext& c, const std::filesystem::path& genDir, InfoFn info); // gen/app.ico, gen/app.rc
// Web: icons, manifest.webmanifest, splash-logo.png in outDir; injects the <head> snippet into
// outDir/index.html when it exists (call again after the build copied index.html).
std::vector<std::string> brandWeb(const BrandingContext& c, const std::filesystem::path& outDir, InfoFn info);
std::vector<std::string> brandLinux(const BrandingContext& c, const std::filesystem::path& outDir, InfoFn info);

// "0xRRGGBB" of splash.background (or "" without a splash block): window colour before the first frame.
std::string splashBackgroundHex(const Json& game);
// Files the base pack must hold for the splash (split builds).
std::vector<std::string> splashBaseFiles(const Json& game);

} // namespace rts::build
