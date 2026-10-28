#include "tools/rtsbuild/Branding.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "tools/branding/Icons.h"
#include "tools/branding/Splash.h"

namespace fs = std::filesystem;

namespace rts::build {

namespace {

branding::IconSpec iconSpec(const BrandingContext& c, InfoFn info) {
    branding::IconSpec s;
    try {
        s = branding::parseIcons(c.game);
    } catch (const std::exception& e) {
        info(std::string("предупреждение: ") + e.what() + " — иконки по умолчанию");
    }
    std::error_code ec;
    if (s.master.empty() && fs::exists(c.project / "icon.png", ec)) s.master = "icon.png";   // the old default name
    for (auto& w : branding::validateIcons(s, c.project.string())) info("предупреждение: " + w);
    return s;
}

branding::GameMeta meta(const BrandingContext& c) {
    branding::GameMeta m;
    m.name = c.name; m.exe = c.exe; m.id = c.id; m.orientation = c.orientation;
    branding::NativeSplash n = branding::nativeSplash(c.game);
    m.backgroundColor = rts::splash::colorHex(n.background);
    return m;
}

std::vector<std::string> finish(branding::Generated& g, InfoFn info, const std::string& what) {
    for (auto& w : g.warnings) info("предупреждение: " + w);
    info(what + ": " + std::to_string(g.files.size()) + " файл(ов)");
    return g.files;
}

std::string readText(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
bool replaceOnce(std::string& s, const std::string& a, const std::string& b) {
    size_t pos = s.find(a);
    if (pos == std::string::npos) return false;
    s.replace(pos, a.size(), b);
    return true;
}

} // namespace

std::string splashBackgroundHex(const Json& game) {
    if (!game["splash"].isObject()) return "";
    branding::NativeSplash n = branding::nativeSplash(game);
    auto b = [](float v) { return int(v * 255 + 0.5f); };
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%02x%02x%02x", b(n.background.r), b(n.background.g), b(n.background.b));
    return buf;
}

std::vector<std::string> splashBaseFiles(const Json& game) { return branding::splashBaseFiles(game); }

std::vector<std::string> brandIOS(const BrandingContext& c, const fs::path& gen, InfoFn info) {
    branding::Generated g;
    branding::IconSpec s = iconSpec(c, info);
    branding::generateIOS(s, c.project.string(), (gen / "Assets.xcassets").string(), g);
    branding::NativeSplash n = branding::nativeSplash(c.game);
    if (n.enabled) branding::generateIOSLaunch(n, c.project.string(), (gen / "Assets.xcassets").string(), gen.string(), g);
    return finish(g, info, n.enabled ? "иконки iOS + LaunchScreen.storyboard" : "иконки iOS");
}

std::vector<std::string> brandMac(const BrandingContext& c, const fs::path& gen, InfoFn info) {
    branding::Generated g;
    branding::generateMac(iconSpec(c, info), c.project.string(), gen.string(), g);
#if defined(__APPLE__)
    // Apple's own packer when present (identical content; also validates the iconset)
    if (std::system("command -v iconutil >/dev/null 2>&1") == 0) {
        std::string cmd = "iconutil -c icns '" + (gen / "AppIcon.iconset").string() + "' -o '" + (gen / "AppIcon.icns").string() + "'";
        if (std::system(cmd.c_str()) != 0) info("предупреждение: iconutil не справился — оставлен AppIcon.icns, записанный rtsbuild");
    }
#endif
    return finish(g, info, "иконки macOS (.iconset + .icns)");
}

std::vector<std::string> brandAndroid(const BrandingContext& c, const fs::path& proj, InfoFn info) {
    branding::Generated g;
    const fs::path main = proj / "app" / "src" / "main";
    branding::generateAndroid(iconSpec(c, info), c.project.string(), (main / "res").string(), (main / "ic_launcher-playstore.png").string(), g);
    branding::NativeSplash n = branding::nativeSplash(c.game);
    if (n.enabled) branding::generateAndroidLaunch(n, c.project.string(), (main / "res").string(), g);
    // manifest: round icon for launchers that ask for it, the launch theme when there is a splash
    fs::path manifest = main / "AndroidManifest.xml";
    std::string m = readText(manifest);
    if (!m.empty()) {
        if (m.find("android:roundIcon") == std::string::npos)
            replaceOnce(m, "android:icon=\"@mipmap/ic_launcher\"", "android:icon=\"@mipmap/ic_launcher\"\n        android:roundIcon=\"@mipmap/ic_launcher_round\"");
        if (n.enabled && !replaceOnce(m, "android:theme=\"@android:style/Theme.NoTitleBar.Fullscreen\"", "android:theme=\"@style/RtsLaunchTheme\""))
            info("предупреждение: AndroidManifest.xml: тема активности не найдена — экран запуска не подключён");
        std::ofstream(manifest, std::ios::binary) << m;
    }
    return finish(g, info, n.enabled ? "иконки Android + экран запуска" : "иконки Android");
}

std::vector<std::string> brandWindows(const BrandingContext& c, const fs::path& gen, InfoFn info) {
    branding::Generated g;
    branding::generateWindows(iconSpec(c, info), c.project.string(), gen.string(), g);
    return finish(g, info, "иконка Windows (.ico + .rc)");
}

std::vector<std::string> brandWeb(const BrandingContext& c, const fs::path& out, InfoFn info) {
    branding::Generated g;
    branding::GameMeta m = meta(c);
    branding::generateWeb(iconSpec(c, info), c.project.string(), out.string(), m, g);
    std::string head = branding::webIconHead(m);
    branding::NativeSplash n = branding::nativeSplash(c.game);
    if (n.enabled) head += branding::generateWebLaunch(n, c.project.string(), out.string(), g);
    std::error_code ec;
    if (fs::exists(out / "index.html", ec)) {
        std::string html = readText(out / "index.html");
        if (html.find("manifest.webmanifest") == std::string::npos && replaceOnce(html, "</head>", head + "</head>"))
            std::ofstream(out / "index.html", std::ios::binary) << html;
    }
    std::ofstream(out / "branding-head.html", std::ios::binary) << head;   // for custom pages / when index.html comes later
    g.files.push_back((out / "branding-head.html").string());
    return finish(g, info, n.enabled ? "иконки web + PWA manifest + заставка" : "иконки web + PWA manifest");
}

std::vector<std::string> brandLinux(const BrandingContext& c, const fs::path& out, InfoFn info) {
    branding::Generated g;
    branding::generateLinux(iconSpec(c, info), c.project.string(), out.string(), meta(c), g);
    return finish(g, info, "иконки Linux (hicolor + .desktop)");
}

} // namespace rts::build
