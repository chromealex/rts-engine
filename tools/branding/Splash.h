// Splash on the host: native launch screens (shown by the OS before the engine starts) and CPU
// previews of the in-engine splash (editor, MCP splash_preview). The timeline is the same code
// the client runs (engine/splash/SplashData.h).
//
// Native launch screens use a static image + background colour: game.json "splash.native"
//   {"image": "branding/logo.png", "background": "#101418", "scale": 0.4, "androidIcon": "branding/splash_icon.xml"}
// or, without it, the first frame of the first logo (else the base image) on splash.background.
#pragma once
#include <string>
#include <vector>

#include "engine/splash/SplashData.h"
#include "tools/branding/Icons.h"

namespace rts::branding {

struct NativeSplash {
    bool enabled = false;      // a "splash" block exists
    Color background{0.06f, 0.07f, 0.08f, 1};
    std::string image;         // project-relative, may be empty (colour only)
    float scale = 0.4f;        // image width as a fraction of the screen's short side
    std::string androidIcon;   // optional animated vector drawable (XML) for Android 12+
    Color progress{0.88f, 0.69f, 0.25f, 1};
};
NativeSplash nativeSplash(const Json& game);

// Splash images the app must ship in the base pack (split builds): the in-engine splash's images.
std::vector<std::string> splashBaseFiles(const Json& game);

// iOS: <genDir>/LaunchScreen.storyboard (image view centred on the colour) + LaunchImage.imageset and
// LaunchBackground.colorset in <xcassets> (also referenced by Info.plist UILaunchScreen).
void generateIOSLaunch(const NativeSplash& n, const std::string& projectDir, const std::string& xcassets, const std::string& genDir, Generated& g);
// Android: res/values/rts_splash.xml (colour), values/rts_themes.xml (pre-12: windowBackground =
// drawable/rts_launch.xml, colour + centred logo), values-v31/rts_themes.xml (Android 12+ splash
// API: windowSplashScreenBackground / AnimatedIcon), drawable-*dpi/rts_splash_{logo,icon}.png.
// The activity uses @style/RtsLaunchTheme (rtsbuild patches AndroidManifest.xml).
void generateAndroidLaunch(const NativeSplash& n, const std::string& projectDir, const std::string& resDir, Generated& g);
// Web: splash-logo.png in <outDir>; returns the <head> snippet (favicon/manifest links + splash
// colours) that rtsbuild injects into index.html.
std::string generateWebLaunch(const NativeSplash& n, const std::string& projectDir, const std::string& outDir, Generated& g);

// CPU render of the in-engine splash at timeline time t, `progress` 0..1, W x H pixels.
struct SplashImages {      // decoded layer images (frames or sheet) of a spec
    std::vector<std::vector<Image>> frames;   // per layer
    std::vector<Image> sheets;                // per layer (empty when frames are used)
    std::vector<std::string> warnings;
};
SplashImages loadSplashImages(const splash::Spec& s, const std::string& projectDir);
Image renderSplashFrame(const splash::Spec& s, const SplashImages& imgs, float t, float progress, int W, int H);
// Strip of frames side by side at the given times (labelled with the time), for agents.
Image splashStrip(const splash::Spec& s, const std::string& projectDir, const std::vector<float>& times, int W, int H, std::vector<std::string>* warnings);

} // namespace rts::branding
