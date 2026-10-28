#include "tools/branding/Splash.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace rts::branding {

namespace {

struct Density { const char* name; float scale; };
const Density kDensities[] = {{"mdpi", 1}, {"hdpi", 1.5f}, {"xhdpi", 2}, {"xxhdpi", 3}, {"xxxhdpi", 4}};

void save(const std::string& path, const Image& img, Generated& g) {
    std::string err;
    if (writePng(path, img, &err)) g.files.push_back(path); else g.warnings.push_back(err);
}
void saveText(const std::string& path, const std::string& text, Generated& g) {
    std::string err;
    if (writeFile(path, text, &err)) g.files.push_back(path); else g.warnings.push_back(err);
}
bool loadOpt(const std::string& dir, const std::string& rel, Image& out, Generated& g) {
    if (rel.empty()) return false;
    std::string err;
    if (loadImageFile((fs::path(dir) / rel).string(), out, &err)) return true;
    g.warnings.push_back("splash: " + err);
    return false;
}
std::string f3(float v) { char b[16]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

} // namespace

NativeSplash nativeSplash(const Json& game) {
    NativeSplash n;
    const Json& j = game["splash"];
    if (!j.isObject()) return n;
    n.enabled = true;
    splash::Spec spec;
    try { spec = splash::parseSpec(j); } catch (const std::exception&) {}
    n.background = spec.background;
    n.progress = spec.progress.color;
    // the logo the engine splash starts with, so the hand-over looks continuous
    for (size_t i = 0; i < spec.layers.size() && n.image.empty(); ++i) {
        const splash::Layer& l = spec.layers[i];
        if (i == 0 && spec.layers.size() > 1 && l.fit == splash::Fit::Cover) continue;   // a full-screen base image: prefer the logo
        n.image = !l.images.empty() ? l.images[0] : l.sheet;
    }
    const Json& nj = j["native"];
    if (nj.isObject()) {
        if (nj.has("image")) n.image = nj.getStr("image");
        splash::parseColor(nj["background"], n.background);
        if (nj["scale"].isNumber()) n.scale = std::clamp(float(nj["scale"].toDouble(0.4)), 0.05f, 1.0f);
        n.androidIcon = nj.getStr("androidIcon");
    }
    return n;
}

std::vector<std::string> splashBaseFiles(const Json& game) {
    std::vector<std::string> v;
    try { v = splash::parseSpec(game["splash"]).images(); } catch (const std::exception&) {}
    NativeSplash n = nativeSplash(game);
    if (!n.image.empty() && std::find(v.begin(), v.end(), n.image) == v.end()) v.push_back(n.image);
    return v;
}

// ------------------------------------------------------------------ iOS
void generateIOSLaunch(const NativeSplash& n, const std::string& dir, const std::string& xcassets, const std::string& genDir, Generated& g) {
    const fs::path xc(xcassets);
    saveText((xc / "LaunchBackground.colorset" / "Contents.json").string(),
             "{\n  \"colors\": [ { \"idiom\": \"universal\", \"color\": { \"color-space\": \"srgb\", \"components\": { \"red\": \"" + f3(n.background.r) +
                 "\", \"green\": \"" + f3(n.background.g) + "\", \"blue\": \"" + f3(n.background.b) + "\", \"alpha\": \"1.000\" } } } ],\n"
                 "  \"info\": { \"author\": \"rtsbuild\", \"version\": 1 }\n}\n", g);
    Image logo;
    int w1 = 0, h1 = 0;
    if (loadOpt(dir, n.image, logo, g)) {
        // @1x is the logo at ~scale of a 390pt-wide phone; @2x/@3x for retina
        float pts = 390 * n.scale;
        Image one = fitContain(logo, int(pts), int(pts));
        w1 = one.width; h1 = one.height;
        std::string images;
        for (int s = 1; s <= 3; ++s) {
            std::string file = "launch@" + std::to_string(s) + "x.png";
            save((xc / "LaunchImage.imageset" / file).string(), s == 1 ? one : fitContain(logo, int(pts * s), int(pts * s)), g);
            if (!images.empty()) images += ",\n";
            images += "    { \"filename\": \"" + file + "\", \"idiom\": \"universal\", \"scale\": \"" + std::to_string(s) + "x\" }";
        }
        saveText((xc / "LaunchImage.imageset" / "Contents.json").string(),
                 "{\n  \"images\": [\n" + images + "\n  ],\n  \"info\": { \"author\": \"rtsbuild\", \"version\": 1 }\n}\n", g);
    }
    const std::string bg = "red=\"" + f3(n.background.r) + "\" green=\"" + f3(n.background.g) + "\" blue=\"" + f3(n.background.b) + "\" alpha=\"1\"";
    std::string mult = f3(n.scale);
    std::string imageView = w1 == 0 ? std::string() :
        "                        <subviews>\n"
        "                            <imageView clipsSubviews=\"YES\" userInteractionEnabled=\"NO\" contentMode=\"scaleAspectFit\" image=\"LaunchImage\" translatesAutoresizingMaskIntoConstraints=\"NO\" id=\"rts-Lg-Img\"/>\n"
        "                        </subviews>\n";
    std::string constraints = w1 == 0 ? std::string() :
        "                        <constraints>\n"
        "                            <constraint firstItem=\"rts-Lg-Img\" firstAttribute=\"centerX\" secondItem=\"rts-Vw-Main\" secondAttribute=\"centerX\" id=\"rts-Cn-CX\"/>\n"
        "                            <constraint firstItem=\"rts-Lg-Img\" firstAttribute=\"centerY\" secondItem=\"rts-Vw-Main\" secondAttribute=\"centerY\" id=\"rts-Cn-CY\"/>\n"
        "                            <constraint firstItem=\"rts-Lg-Img\" firstAttribute=\"width\" secondItem=\"rts-Vw-Main\" secondAttribute=\"width\" multiplier=\"" + mult + "\" id=\"rts-Cn-W\"/>\n"
        "                            <constraint firstItem=\"rts-Lg-Img\" firstAttribute=\"height\" secondItem=\"rts-Vw-Main\" secondAttribute=\"height\" multiplier=\"" + mult + "\" id=\"rts-Cn-H\"/>\n"
        "                        </constraints>\n";
    std::string resources = w1 == 0 ? std::string() :
        "    <resources>\n        <image name=\"LaunchImage\" width=\"" + std::to_string(w1) + "\" height=\"" + std::to_string(h1) + "\"/>\n    </resources>\n";
    saveText((fs::path(genDir) / "LaunchScreen.storyboard").string(),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!-- Generated by rtsbuild from game.json \"splash\": static launch screen shown by iOS before the engine starts. -->\n"
             "<document type=\"com.apple.InterfaceBuilder3.CocoaTouch.Storyboard.XIB\" version=\"3.0\" toolsVersion=\"22505\" targetRuntime=\"iOS.CocoaTouch\" "
             "propertyAccessControl=\"none\" useAutolayout=\"YES\" launchScreen=\"YES\" useTraitCollections=\"YES\" useSafeAreas=\"YES\" colorMatched=\"YES\" initialViewController=\"rts-Vc-Main\">\n"
             "    <device id=\"retina6_12\" orientation=\"portrait\" appearance=\"light\"/>\n"
             "    <dependencies>\n"
             "        <plugIn identifier=\"com.apple.InterfaceBuilder.IBCocoaTouchPlugin\" version=\"22504\"/>\n"
             "        <capability name=\"Safe area layout guides\" minToolsVersion=\"9.0\"/>\n"
             "        <capability name=\"documents saved in the Xcode 8 format\" minToolsVersion=\"8.0\"/>\n"
             "    </dependencies>\n"
             "    <scenes>\n"
             "        <scene sceneID=\"rts-Sc-Main\">\n"
             "            <objects>\n"
             "                <viewController id=\"rts-Vc-Main\" sceneMemberID=\"viewController\">\n"
             "                    <view key=\"view\" contentMode=\"scaleToFill\" id=\"rts-Vw-Main\">\n"
             "                        <rect key=\"frame\" x=\"0.0\" y=\"0.0\" width=\"393\" height=\"852\"/>\n"
             "                        <autoresizingMask key=\"autoresizingMask\" widthSizable=\"YES\" heightSizable=\"YES\"/>\n" +
                 imageView +
             "                        <viewLayoutGuide key=\"safeArea\" id=\"rts-Sa-Main\"/>\n"
             "                        <color key=\"backgroundColor\" " + bg + " colorSpace=\"custom\" customColorSpace=\"sRGB\"/>\n" +
                 constraints +
             "                    </view>\n"
             "                </viewController>\n"
             "                <placeholder placeholderIdentifier=\"IBFirstResponder\" id=\"rts-Fr-Main\" userLabel=\"First Responder\" sceneMemberID=\"firstResponder\"/>\n"
             "            </objects>\n"
             "            <point key=\"canvasLocation\" x=\"53\" y=\"375\"/>\n"
             "        </scene>\n"
             "    </scenes>\n" + resources +
             "</document>\n", g);
}

// ------------------------------------------------------------------ Android
void generateAndroidLaunch(const NativeSplash& n, const std::string& dir, const std::string& resDir, Generated& g) {
    const fs::path res(resDir);
    const std::string color = splash::colorHex(n.background);
    saveText((res / "values" / "rts_splash.xml").string(),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n    <color name=\"rts_splash_background\">" + color + "</color>\n</resources>\n", g);
    Image logo;
    const bool hasLogo = loadOpt(dir, n.image, logo, g);
    const float logoDp = 360 * n.scale;   // of a typical 360dp-wide phone
    if (hasLogo)
        for (auto& d : kDensities) {
            int px = int(std::lround(logoDp * d.scale));
            save((res / (std::string("drawable-") + d.name) / "rts_splash_logo.png").string(), fitContain(logo, px, px), g);
        }
    saveText((res / "drawable" / "rts_launch.xml").string(),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- Launch window (Android 8-11): splash colour + centred logo until the engine draws. -->\n"
             "<layer-list xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
             "    <item android:drawable=\"@color/rts_splash_background\" />\n" +
                 std::string(hasLogo ? "    <item>\n        <bitmap android:gravity=\"center\" android:src=\"@drawable/rts_splash_logo\" />\n    </item>\n" : "") +
                 "</layer-list>\n", g);
    // Android 12+ draws its own splash: colour + icon in a 288dp square, visible inside a 192dp circle
    std::string icon;
    if (!n.androidIcon.empty()) {
        std::ifstream f(fs::path(dir) / n.androidIcon, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        if (f) { saveText((res / "drawable" / "rts_splash_icon.xml").string(), ss.str(), g); icon = "@drawable/rts_splash_icon"; }
        else g.warnings.push_back("splash.native.androidIcon: cannot read " + n.androidIcon);
    }
    if (icon.empty() && hasLogo) {
        IconLook l;
        l.padding = 0.25f;   // keeps the logo inside the circle
        for (auto& d : kDensities) {
            int px = int(std::lround(288 * d.scale));
            save((res / (std::string("drawable-") + d.name) / "rts_splash_icon.png").string(), renderIcon(logo, px, l), g);
        }
        icon = "@drawable/rts_splash_icon";
    }
    const std::string parent = "@android:style/Theme.NoTitleBar.Fullscreen";
    saveText((res / "values" / "rts_themes.xml").string(),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n    <style name=\"RtsLaunchTheme\" parent=\"" + parent + "\">\n"
             "        <item name=\"android:windowBackground\">@drawable/rts_launch</item>\n    </style>\n</resources>\n", g);
    saveText((res / "values-v31" / "rts_themes.xml").string(),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n    <style name=\"RtsLaunchTheme\" parent=\"" + parent + "\">\n"
             "        <item name=\"android:windowBackground\">@color/rts_splash_background</item>\n"
             "        <item name=\"android:windowSplashScreenBackground\">@color/rts_splash_background</item>\n" +
                 (icon.empty() ? std::string() : "        <item name=\"android:windowSplashScreenAnimatedIcon\">" + icon + "</item>\n") +
                 (n.androidIcon.empty() ? std::string() : "        <item name=\"android:windowSplashScreenAnimationDuration\">1000</item>\n") +
                 "    </style>\n</resources>\n", g);
}

// ------------------------------------------------------------------ web
std::string generateWebLaunch(const NativeSplash& n, const std::string& dir, const std::string& outDir, Generated& g) {
    Image logo;
    bool hasLogo = loadOpt(dir, n.image, logo, g);
    if (hasLogo) save((fs::path(outDir) / "splash-logo.png").string(), fitContain(logo, std::min(512, logo.width), std::min(512, logo.height)), g);
    char vmin[16];
    std::snprintf(vmin, sizeof vmin, "%.0fvmin", n.scale * 100);
    return "<style>:root{--rts-splash-bg:" + splash::colorHex(n.background) + ";--rts-splash-bar:" + splash::colorHex(n.progress) +
           ";--rts-splash-logo:" + vmin + "}</style>\n<script>window.RTS_SPLASH={logo:" + std::string(hasLogo ? "\"splash-logo.png\"" : "null") + "};</script>\n";
}

// ------------------------------------------------------------------ CPU preview
SplashImages loadSplashImages(const splash::Spec& s, const std::string& dir) {
    SplashImages r;
    for (auto& l : s.layers) {
        r.frames.emplace_back();
        r.sheets.emplace_back();
        std::string err;
        for (auto& p : l.images) {
            Image img;
            if (!loadImageFile((fs::path(dir) / p).string(), img, &err)) r.warnings.push_back("splash: " + err);
            r.frames.back().push_back(std::move(img));
        }
        if (!l.sheet.empty() && !loadImageFile((fs::path(dir) / l.sheet).string(), r.sheets.back(), &err)) r.warnings.push_back("splash: " + err);
    }
    return r;
}

Image renderSplashFrame(const splash::Spec& s, const SplashImages& im, float t, float progress, int W, int H) {
    Image out = makeImage(W, H, {s.background.r, s.background.g, s.background.b, 1});
    std::vector<splash::ImageSize> sizes;
    for (size_t i = 0; i < s.layers.size(); ++i) {
        const splash::Layer& l = s.layers[i];
        if (!l.sheet.empty()) sizes.push_back({im.sheets[i].empty() ? 0 : l.frameW, im.sheets[i].empty() ? 0 : l.frameH});
        else if (!im.frames[i].empty() && !im.frames[i][0].empty()) sizes.push_back({im.frames[i][0].width, im.frames[i][0].height});
        else sizes.push_back({});
    }
    const float pixelScale = float(H) / 720.0f;   // previews stand for a 720-point-tall screen
    for (const splash::DrawItem& d : splash::evaluate(s, sizes, t, float(W), float(H), pixelScale)) {
        const splash::Layer& l = s.layers[size_t(d.layer)];
        if (!l.sheet.empty()) {
            const Image& sh = im.sheets[size_t(d.layer)];
            int cols = std::max(1, sh.width / l.frameW), fx = d.frame % cols, fy = d.frame / cols;
            drawQuad(out, sh, d.cx, d.cy, d.w, d.h, d.rotation, d.opacity, float(fx * l.frameW) / sh.width, float(fy * l.frameH) / sh.height,
                     float((fx + 1) * l.frameW) / sh.width, float((fy + 1) * l.frameH) / sh.height);
        } else if (size_t(d.frame) < im.frames[size_t(d.layer)].size()) {
            drawQuad(out, im.frames[size_t(d.layer)][size_t(d.frame)], d.cx, d.cy, d.w, d.h, d.rotation, d.opacity);
        }
    }
    float x, y, w, h;
    if (splash::progressRect(s, float(W), float(H), pixelScale, x, y, w, h)) {
        fillRect(out, int(x), int(y), int(w), int(h), s.progress.track);
        int fw = int(std::round(w * std::clamp(progress, 0.0f, 1.0f)));
        if (fw > 0) fillRect(out, int(x), int(y), fw, int(h), s.progress.color);
    }
    flatten(out, s.background);
    return out;
}

Image splashStrip(const splash::Spec& s, const std::string& dir, const std::vector<float>& times, int W, int H, std::vector<std::string>* warnings) {
    SplashImages im = loadSplashImages(s, dir);
    if (warnings) warnings->insert(warnings->end(), im.warnings.begin(), im.warnings.end());
    const int pad = 8, label = 14;
    Image strip = makeImage(int(times.size()) * (W + pad) + pad, H + label + pad * 2, {0.12f, 0.12f, 0.13f, 1});
    const float total = std::max(0.5f, s.minimumTime() > 0 ? s.minimumTime() : (times.empty() ? 1.0f : times.back()));
    for (size_t i = 0; i < times.size(); ++i) {
        Image f = renderSplashFrame(s, im, times[i], std::min(1.0f, times[i] / total), W, H);
        int x = pad + int(i) * (W + pad);
        blit(strip, f, x, pad);
        char b[24];
        std::snprintf(b, sizeof b, "T=%.2fS", times[i]);
        drawText(strip, x, pad + H + 4, b, {0.85f, 0.88f, 0.92f, 1});
    }
    return strip;
}

} // namespace rts::branding
