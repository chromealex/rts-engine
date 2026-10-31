#include "tools/branding/Icons.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace rts::branding {

namespace {

[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }
float num(const Json& j, float def) { return j.isNumber() ? float(j.toDouble(def)) : def; }

void parseLookInto(const Json& j, IconLook& l, const std::string& where) {
    if (j.isNull()) return;
    if (j.isString()) { l.image = j.str(); return; }
    if (!j.isObject()) fail(where, "expected an image path or an object {\"image\", \"background\", \"padding\", ...}");
    l.image = j.getStr("image", l.image);
    if (j.has("background")) {
        if (!splash::parseColor(j["background"], l.background)) fail(where + ".background", "expected a colour like \"#1b2a3a\"");
        l.hasBackground = true;
    }
    l.padding = std::clamp(num(j["padding"], l.padding), 0.0f, 0.45f);
    l.margin = std::clamp(num(j["margin"], l.margin), 0.0f, 0.45f);
    l.cornerRadius = std::clamp(num(j["cornerRadius"], l.cornerRadius), 0.0f, 0.5f);
    if (j.has("circle")) l.circle = j.getBool("circle");
}

std::string pathOf(const std::string& dir, const std::string& rel) { return (fs::path(dir) / rel).string(); }

// The opaque background a look ends up with (its own, else the image's edge colour).
Color backgroundFor(const IconLook& l, const Image& src) { return l.hasBackground ? l.background : edgeColor(src); }

struct Sources {   // decoded images by project path ("" = master), cached for one generation run
    const IconSpec& spec;
    std::string dir;
    std::vector<std::string>* warnings;
    std::map<std::string, Image> cache;
    const Image& get(const std::string& path) {
        std::string p = path.empty() ? spec.master : path;
        auto it = cache.find(p);
        if (it != cache.end()) return it->second;
        return cache[p] = loadIconSource(dir, p, warnings);
    }
};

void save(const std::string& path, const Image& img, Generated& g) {
    std::string err;
    if (writePng(path, img, &err)) g.files.push_back(path);
    else g.warnings.push_back(err);
}
void saveText(const std::string& path, const std::string& text, Generated& g) {
    std::string err;
    if (writeFile(path, text, &err)) g.files.push_back(path);
    else g.warnings.push_back(err);
}

struct Density { const char* name; float scale; };
const Density kDensities[] = {{"mdpi", 1}, {"hdpi", 1.5f}, {"xhdpi", 2}, {"xxhdpi", 3}, {"xxxhdpi", 4}};

// Android adaptive layers: 108dp squares (density px), foreground inset so the art stays in the
// 66dp safe zone (logos) or fills the 72dp viewport (full-bleed art).
float foregroundPadding(const AndroidIcons& a, const Image& master) {
    if (a.foregroundPadding >= 0) return a.foregroundPadding;
    return cornersTransparent(master) ? 0.19f : 1.0f / 6.0f;
}
Image adaptiveForeground(Sources& src, const AndroidIcons& a, int px) {
    IconLook l;   // a given foreground is the whole 108dp layer unless foregroundPadding says otherwise
    l.padding = a.foreground.empty() ? foregroundPadding(a, src.get("")) : std::max(0.0f, a.foregroundPadding);
    return renderIcon(src.get(a.foreground), px, l);
}
Image adaptiveBackground(Sources& src, const AndroidIcons& a, int px) {
    if (!a.backgroundImage.empty()) return fitCover(src.get(a.backgroundImage), px, px);
    return makeImage(px, px, a.hasBackgroundColor ? a.backgroundColor : edgeColor(src.get("")));
}
Image monochromeLayer(Sources& src, const AndroidIcons& a, int px) {
    IconLook l;
    l.padding = foregroundPadding(a, src.get(a.monochrome));
    Image m = renderIcon(src.get(a.monochrome), px, l);
    for (size_t i = 0; i < m.rgba.size(); i += 4) m.rgba[i] = m.rgba[i + 1] = m.rgba[i + 2] = 255;   // alpha mask, any colour
    return m;
}
// What a launcher shows: background + foreground, the central 72dp of the 108dp layers.
Image adaptiveComposite(Sources& src, const AndroidIcons& a, int px) {
    int layer = int(std::lround(px * 1.5));
    Image c = adaptiveBackground(src, a, layer);
    blit(c, adaptiveForeground(src, a, layer), 0, 0);
    Image out = makeImage(px, px);
    int o = (layer - px) / 2;
    for (int y = 0; y < px; ++y) std::copy_n(c.px(o, o + y), size_t(px) * 4, out.px(0, y));
    return out;
}

} // namespace

const std::vector<IosIcon>& iosIconSet() {
    static const std::vector<IosIcon> v = {
        {"iphone", "20x20", "2x", 40}, {"iphone", "20x20", "3x", 60}, {"iphone", "29x29", "2x", 58}, {"iphone", "29x29", "3x", 87},
        {"iphone", "40x40", "2x", 80}, {"iphone", "40x40", "3x", 120}, {"iphone", "60x60", "2x", 120}, {"iphone", "60x60", "3x", 180},
        {"ipad", "20x20", "1x", 20}, {"ipad", "20x20", "2x", 40}, {"ipad", "29x29", "1x", 29}, {"ipad", "29x29", "2x", 58},
        {"ipad", "40x40", "1x", 40}, {"ipad", "40x40", "2x", 80}, {"ipad", "76x76", "1x", 76}, {"ipad", "76x76", "2x", 152},
        {"ipad", "83.5x83.5", "2x", 167}, {"ios-marketing", "1024x1024", "1x", 1024},
    };
    return v;
}
const std::vector<int>& windowsIcoSizes() { static const std::vector<int> v = {16, 20, 24, 32, 40, 48, 64, 256}; return v; }
const std::vector<int>& linuxIconSizes() { static const std::vector<int> v = {16, 22, 24, 32, 48, 64, 128, 256, 512}; return v; }

IconSpec parseIcons(const Json& game) {
    IconSpec s;
    const Json& j = game["icons"];
    if (!j.isNull() && !j.isObject() && !j.isString()) fail("icons", "expected an object (or an image path)");
    s.master = j.isString() ? j.str() : j.getStr("default", game.getStr("icon"));
    // defaults of the icons that must be opaque squares
    IconLook opaque;
    opaque.opaque = true;
    if (j.isObject()) {
        if (j.has("background")) {
            if (!splash::parseColor(j["background"], opaque.background)) fail("icons.background", "expected a colour like \"#1b2a3a\"");
            opaque.hasBackground = true;
        }
        opaque.padding = std::clamp(num(j["padding"], 0), 0.0f, 0.45f);
        opaque.cornerRadius = std::clamp(num(j["cornerRadius"], 0), 0.0f, 0.5f);
    }
    auto block = [&](const char* k) -> const Json& { return j.isObject() ? j[k] : j["-"]; };

    s.ios = opaque;
    s.ios.cornerRadius = 0;   // the system draws the rounded shape
    parseLookInto(block("ios"), s.ios, "icons.ios");
    s.ios.opaque = true;

    s.macos.margin = 100.0f / 1024.0f;   // Big Sur grid: 824 px body on a 1024 canvas, radius ~185
    s.macos.cornerRadius = 0.225f;
    parseLookInto(block("macos"), s.macos, "icons.macos");

    parseLookInto(block("windows"), s.windows, "icons.windows");
    parseLookInto(block("linux"), s.linux, "icons.linux");
    const Json& web = block("web");
    parseLookInto(web.isObject() || web.isString() ? web : Json(), s.web, "icons.web");
    s.appleTouch = opaque;
    s.appleTouch.image = s.web.image;
    s.webMaskable = opaque;
    s.webMaskable.image = s.web.image;
    s.webMaskable.padding = std::max(opaque.padding, 0.1f);   // maskable safe zone: inner 80% circle
    if (web.isObject()) {
        parseLookInto(web["appleTouch"], s.appleTouch, "icons.web.appleTouch");
        parseLookInto(web["maskable"], s.webMaskable, "icons.web.maskable");
    }
    s.appleTouch.opaque = s.webMaskable.opaque = true;

    AndroidIcons& a = s.android;
    const Json& an = block("android");
    if (!an.isNull() && !an.isObject()) fail("icons.android", "expected an object {\"legacy\", \"foreground\", \"background\", ...}");
    if (an.has("background")) {
        const Json& b = an["background"];
        if (b.isString() && !b.str().empty() && b.str()[0] != '#') a.backgroundImage = b.str();
        else if (splash::parseColor(b, a.backgroundColor)) a.hasBackgroundColor = true;
        else fail("icons.android.background", "expected a colour (\"#1b2a3a\") or an image path");
    } else if (opaque.hasBackground) {
        a.backgroundColor = opaque.background;
        a.hasBackgroundColor = true;
    }
    a.foreground = an.getStr("foreground");
    a.monochrome = an.getStr("monochrome");
    a.foregroundPadding = an.has("foregroundPadding") ? std::clamp(num(an["foregroundPadding"], 0), 0.0f, 0.45f) : -1.0f;
    a.legacy = opaque;
    a.legacy.opaque = false;
    a.legacy.hasBackground = true;
    a.legacy.background = a.hasBackgroundColor ? a.backgroundColor : opaque.background;
    if (!a.hasBackgroundColor && !opaque.hasBackground) a.legacy.hasBackground = false;
    a.legacy.margin = 0.04f;
    if (a.legacy.cornerRadius <= 0) a.legacy.cornerRadius = 0.12f;
    parseLookInto(an["legacy"], a.legacy, "icons.android.legacy");
    a.round = a.legacy;
    a.round.circle = true;
    parseLookInto(an["round"], a.round, "icons.android.round");
    return s;
}

namespace {
// The RTSEngine mark in a 1000 x 1000 frame (tools/branding/logo/make_logo.py has the same polygons).
struct Pt { float x, y; };
const Pt kTop[] = {{500, 94}, {373, 179}, {373, 476}, {500, 397}, {627, 476}, {627, 179}};
const Pt kRight[] = {{567, 741}, {853, 907}, {977, 833}, {977, 719}, {693, 561}, {693, 663}};
const Pt kDiamond[] = {{500, 438}, {350, 588}, {500, 738}, {650, 588}};

bool insidePoly(const Pt* p, int n, float x, float y, bool mirror) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = mirror ? 1000 - p[i].x : p[i].x, xj = mirror ? 1000 - p[j].x : p[j].x;
        if ((p[i].y > y) != (p[j].y > y) && x < (xj - xi) * (y - p[i].y) / (p[j].y - p[i].y) + xi) in = !in;
    }
    return in;
}
} // namespace

// The engine's default icon: the dark mark with the orange diamond on white (the master; platform
// looks add their own margins and corners).
Image defaultIcon(int size) {
    Image img = makeImage(size, size);
    const float inset = 0.1f, k = (1 - 2 * inset) * size / 1000.0f, o = inset * size;
    const int ss = 4;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int ink = 0, orange = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const float u = (x + (sx + 0.5f) / ss - o) / k, v = (y + (sy + 0.5f) / ss - o) / k;
                    if (insidePoly(kDiamond, 4, u, v, false)) ++orange;
                    else if (insidePoly(kTop, 6, u, v, false) || insidePoly(kRight, 6, u, v, false) || insidePoly(kRight, 6, u, v, true)) ++ink;
                }
            const float n = float(ss * ss), wi = ink / n, wo = orange / n, ww = 1 - wi - wo;
            uint8_t* p = img.px(x, y);
            p[0] = uint8_t(ww * 255 + wi * 21 + wo * 254 + 0.5f);
            p[1] = uint8_t(ww * 255 + wi * 21 + wo * 111 + 0.5f);
            p[2] = uint8_t(ww * 255 + wi * 21 + wo * 3 + 0.5f);
            p[3] = 255;
        }
    return img;
}

Image loadIconSource(const std::string& dir, const std::string& path, std::vector<std::string>* warnings) {
    if (path.empty()) return defaultIcon(1024);
    Image img;
    std::string err;
    if (!loadImageFile(pathOf(dir, path), img, &err)) {
        if (warnings) warnings->push_back("icons: " + err + " — using the built-in default icon");
        return defaultIcon(1024);
    }
    return img;
}

Image renderIcon(const Image& src, int size, const IconLook& l) {
    int m = int(std::lround(size * l.margin));
    int body = std::max(1, size - 2 * m);
    Image b = makeImage(body, body, l.hasBackground ? l.background : Color{0, 0, 0, 0});
    int inner = std::max(1, int(std::lround(body * (1 - 2 * l.padding))));
    Image art = fitContain(src, inner, inner);
    blit(b, art, (body - art.width) / 2, (body - art.height) / 2);
    if (l.circle) maskCircle(b);
    else if (l.cornerRadius > 0) maskRoundedRect(b, l.cornerRadius * body);
    Image out = makeImage(size, size);
    blit(out, b, m, m);
    if (l.opaque) flatten(out, backgroundFor(l, src));
    return out;
}

std::vector<std::string> validateIcons(const IconSpec& s, const std::string& dir) {
    std::vector<std::string> w;
    if (s.master.empty()) w.push_back("icons: no master image (icons.default) — the built-in default icon is used");
    std::map<std::string, std::string> used;   // path -> first key using it
    auto use = [&](const std::string& p, const std::string& key) { if (!p.empty() && !used.count(p)) used[p] = key; };
    use(s.master, "icons.default");
    use(s.ios.image, "icons.ios");
    use(s.macos.image, "icons.macos");
    use(s.windows.image, "icons.windows");
    use(s.web.image, "icons.web");
    use(s.appleTouch.image, "icons.web.appleTouch");
    use(s.webMaskable.image, "icons.web.maskable");
    use(s.linux.image, "icons.linux");
    use(s.android.legacy.image, "icons.android.legacy");
    use(s.android.round.image, "icons.android.round");
    use(s.android.foreground, "icons.android.foreground");
    use(s.android.backgroundImage, "icons.android.background");
    use(s.android.monochrome, "icons.android.monochrome");
    for (auto& [p, key] : used) {
        Image img;
        std::string err;
        if (!loadImageFile(pathOf(dir, p), img, &err)) { w.push_back(key + ": " + err); continue; }
        if (img.width != img.height) w.push_back(key + ": " + p + " is " + std::to_string(img.width) + "x" + std::to_string(img.height) + ", icons must be square (it is fitted with transparent bars)");
        int need = key == "icons.android.foreground" || key == "icons.android.background" || key == "icons.android.monochrome" ? 432 : 1024;
        if (std::min(img.width, img.height) < need)
            w.push_back(key + ": " + p + " is " + std::to_string(img.width) + "x" + std::to_string(img.height) + ", at least " + std::to_string(need) + "x" + std::to_string(need) + " is needed for sharp large icons");
    }
    // iOS (and apple-touch) forbid alpha: say what it is flattened on
    Image ios;
    std::string err;
    std::string iosPath = s.ios.image.empty() ? s.master : s.ios.image;
    if (!iosPath.empty() && loadImageFile(pathOf(dir, iosPath), ios, &err) && hasAlpha(ios))
        w.push_back("icons.ios: " + iosPath + " has transparency; iOS icons must be opaque — flattened on " +
                    splash::colorHex(backgroundFor(s.ios, ios)) + (s.ios.hasBackground ? "" : " (its edge colour; set icons.background to choose)"));
    return w;
}

// ------------------------------------------------------------------ generators
void generateIOS(const IconSpec& s, const std::string& dir, const std::string& xcassets, Generated& g) {
    Sources src{s, dir, &g.warnings, {}};
    const fs::path set = fs::path(xcassets) / "AppIcon.appiconset";
    std::set<int> done;
    std::string images;
    for (auto& e : iosIconSet()) {
        std::string file = "icon_" + std::to_string(e.pixels) + ".png";
        if (done.insert(e.pixels).second) save((set / file).string(), renderIcon(src.get(s.ios.image), e.pixels, s.ios), g);
        if (!images.empty()) images += ",\n";
        images += std::string("    { \"filename\": \"") + file + "\", \"idiom\": \"" + e.idiom + "\", \"scale\": \"" + e.scale + "\", \"size\": \"" + e.size + "\" }";
    }
    saveText((set / "Contents.json").string(), "{\n  \"images\": [\n" + images + "\n  ],\n  \"info\": { \"author\": \"rtsbuild\", \"version\": 1 }\n}\n", g);
    saveText((fs::path(xcassets) / "Contents.json").string(), "{\n  \"info\": { \"author\": \"rtsbuild\", \"version\": 1 }\n}\n", g);
}

void generateMac(const IconSpec& s, const std::string& dir, const std::string& genDir, Generated& g) {
    Sources src{s, dir, &g.warnings, {}};
    const fs::path set = fs::path(genDir) / "AppIcon.iconset";
    std::map<int, Image> bySize;
    for (int px : {16, 32, 64, 128, 256, 512, 1024}) bySize[px] = renderIcon(src.get(s.macos.image), px, s.macos);
    for (int base : {16, 32, 128, 256, 512}) {
        std::string n = std::to_string(base) + "x" + std::to_string(base);
        save((set / ("icon_" + n + ".png")).string(), bySize[base], g);
        save((set / ("icon_" + n + "@2x.png")).string(), bySize[base * 2], g);
    }
    // OSType -> pixels; PNG payloads (macOS 10.7+)
    std::vector<std::pair<std::string, Image>> e = {
        {"icp4", bySize[16]}, {"icp5", bySize[32]}, {"ic11", bySize[32]}, {"ic12", bySize[64]}, {"ic07", bySize[128]},
        {"ic13", bySize[256]}, {"ic08", bySize[256]}, {"ic14", bySize[512]}, {"ic09", bySize[512]}, {"ic10", bySize[1024]}};
    saveText((fs::path(genDir) / "AppIcon.icns").string(), encodeIcns(e), g);
}

void generateAndroid(const IconSpec& s, const std::string& dir, const std::string& resDir, const std::string& playStore, Generated& g) {
    Sources src{s, dir, &g.warnings, {}};
    const AndroidIcons& a = s.android;
    const fs::path res(resDir);
    for (auto& d : kDensities) {
        fs::path mip = res / (std::string("mipmap-") + d.name);
        int legacy = int(std::lround(48 * d.scale)), layer = int(std::lround(108 * d.scale));
        save((mip / "ic_launcher.png").string(), renderIcon(src.get(a.legacy.image), legacy, a.legacy), g);
        save((mip / "ic_launcher_round.png").string(), renderIcon(src.get(a.round.image), legacy, a.round), g);
        save((mip / "ic_launcher_foreground.png").string(), adaptiveForeground(src, a, layer), g);
        if (!a.backgroundImage.empty()) save((mip / "ic_launcher_background.png").string(), adaptiveBackground(src, a, layer), g);
        if (!a.monochrome.empty()) save((mip / "ic_launcher_monochrome.png").string(), monochromeLayer(src, a, layer), g);
    }
    Color bg = a.hasBackgroundColor ? a.backgroundColor : edgeColor(src.get(""));
    saveText((res / "values" / "ic_launcher_background.xml").string(),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n    <color name=\"ic_launcher_background\">" + splash::colorHex(bg) + "</color>\n</resources>\n", g);
    std::string adaptive = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<adaptive-icon xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
                           "    <background android:drawable=\"" + std::string(a.backgroundImage.empty() ? "@color/ic_launcher_background" : "@mipmap/ic_launcher_background") + "\" />\n"
                           "    <foreground android:drawable=\"@mipmap/ic_launcher_foreground\" />\n" +
                           (a.monochrome.empty() ? std::string() : "    <monochrome android:drawable=\"@mipmap/ic_launcher_monochrome\" />\n") + "</adaptive-icon>\n";
    saveText((res / "mipmap-anydpi-v26" / "ic_launcher.xml").string(), adaptive, g);
    saveText((res / "mipmap-anydpi-v26" / "ic_launcher_round.xml").string(), adaptive, g);
    // Play Store listing: 512x512 full square, Google Play applies its own mask
    if (!playStore.empty()) {
        IconLook store;
        store.opaque = true;
        store.background = bg;
        store.hasBackground = true;
        save(playStore, a.legacy.image.empty() ? adaptiveComposite(src, a, 512) : renderIcon(src.get(a.legacy.image), 512, store), g);
    }
}

void generateWindows(const IconSpec& s, const std::string& dir, const std::string& genDir, Generated& g) {
    Sources src{s, dir, &g.warnings, {}};
    std::vector<Image> imgs;
    for (int px : windowsIcoSizes()) imgs.push_back(renderIcon(src.get(s.windows.image), px, s.windows));
    saveText((fs::path(genDir) / "app.ico").string(), encodeIco(imgs), g);
    // resource id 1: PlatformWin32 loads it as the window class icon; Explorer shows the first group
    saveText((fs::path(genDir) / "app.rc").string(), "// Generated by rtsbuild: application icon\n1 ICON \"app.ico\"\n", g);
}

void generateWeb(const IconSpec& s, const std::string& dir, const std::string& outDir, const GameMeta& m, Generated& g) {
    Sources src{s, dir, &g.warnings, {}};
    const fs::path out(outDir);
    std::vector<Image> fav;
    for (int px : {16, 32, 48}) fav.push_back(renderIcon(src.get(s.web.image), px, s.web));
    saveText((out / "favicon.ico").string(), encodeIco(fav), g);
    save((out / "favicon-32.png").string(), fav[1], g);
    save((out / "apple-touch-icon.png").string(), renderIcon(src.get(s.appleTouch.image), 180, s.appleTouch), g);
    for (int px : {192, 512}) {
        save((out / "icons" / ("icon-" + std::to_string(px) + ".png")).string(), renderIcon(src.get(s.web.image), px, s.web), g);
        save((out / "icons" / ("maskable-" + std::to_string(px) + ".png")).string(), renderIcon(src.get(s.webMaskable.image), px, s.webMaskable), g);
    }
    Json man = Json::object();
    man.set("name", Json::string(m.name));
    man.set("short_name", Json::string(m.name.size() > 12 ? m.name.substr(0, 12) : m.name));
    man.set("id", Json::string(m.id));
    man.set("start_url", Json::string("./"));
    man.set("scope", Json::string("./"));
    man.set("display", Json::string("fullscreen"));
    man.set("orientation", Json::string(m.orientation == "any" ? "any" : m.orientation));
    std::string bg = m.backgroundColor.empty() ? "#0f1214" : m.backgroundColor;
    man.set("background_color", Json::string(bg));
    man.set("theme_color", Json::string(m.themeColor.empty() ? bg : m.themeColor));
    man.set("categories", [] { Json c = Json::array(); c.push(Json::string("games")); return c; }());
    Json icons = Json::array();
    for (int px : {192, 512})
        for (const char* kind : {"any", "maskable"}) {
            Json i = Json::object();
            i.set("src", Json::string(std::string("icons/") + (std::string(kind) == "any" ? "icon-" : "maskable-") + std::to_string(px) + ".png"));
            i.set("sizes", Json::string(std::to_string(px) + "x" + std::to_string(px)));
            i.set("type", Json::string("image/png"));
            i.set("purpose", Json::string(kind));
            icons.push(std::move(i));
        }
    man.set("icons", std::move(icons));
    saveText((out / "manifest.webmanifest").string(), man.dump(2) + "\n", g);
}

std::string webIconHead(const GameMeta& m) {
    std::string theme = !m.themeColor.empty() ? m.themeColor : !m.backgroundColor.empty() ? m.backgroundColor : "#0f1214";
    return "<link rel=\"icon\" href=\"favicon.ico\" sizes=\"any\">\n<link rel=\"icon\" type=\"image/png\" sizes=\"32x32\" href=\"favicon-32.png\">\n"
           "<link rel=\"apple-touch-icon\" href=\"apple-touch-icon.png\">\n<link rel=\"manifest\" href=\"manifest.webmanifest\">\n"
           "<meta name=\"theme-color\" content=\"" + theme + "\">\n";
}

void generateLinux(const IconSpec& s, const std::string& dir, const std::string& outDir, const GameMeta& m, Generated& g) {
    Sources src{s, dir, &g.warnings, {}};
    const fs::path out(outDir);
    for (int px : linuxIconSizes()) {
        std::string n = std::to_string(px) + "x" + std::to_string(px);
        save((out / "share" / "icons" / "hicolor" / n / "apps" / (m.exe + ".png")).string(), renderIcon(src.get(s.linux.image), px, s.linux), g);
    }
    saveText((out / "share" / "applications" / (m.exe + ".desktop")).string(),
             "[Desktop Entry]\nType=Application\nName=" + m.name + "\nExec=" + m.exe + "\nIcon=" + m.exe +
                 "\nCategories=Game;StrategyGame;\nTerminal=false\n", g);
}

// ------------------------------------------------------------------ previews
std::vector<IconPreview> previewIcons(const IconSpec& s, const std::string& dir, std::vector<std::string>* warnings) {
    Sources src{s, dir, warnings, {}};
    std::vector<IconPreview> v;
    auto add = [&](const char* platform, const std::string& label, Image img) { int n = img.width; v.push_back({platform, label, std::move(img), n}); };
    for (int px : {20, 29, 40, 58, 60, 76, 80, 87, 120, 152, 167, 180}) add("iOS", std::to_string(px), renderIcon(src.get(s.ios.image), px, s.ios));
    {   // what the home screen shows: the system's continuous-corner mask
        Image home = renderIcon(src.get(s.ios.image), 180, s.ios);
        maskRoundedRect(home, 180 * 0.2237f);
        add("iOS", "180 HOME", std::move(home));
    }
    for (int px : {16, 32, 64, 128, 256}) add("macOS", std::to_string(px), renderIcon(src.get(s.macos.image), px, s.macos));
    for (auto& d : kDensities) {
        int px = int(std::lround(48 * d.scale));
        add("Android", std::to_string(px), renderIcon(src.get(s.android.legacy.image), px, s.android.legacy));
    }
    add("Android", "ROUND 192", renderIcon(src.get(s.android.round.image), 192, s.android.round));
    {
        Image c = adaptiveComposite(src, s.android, 192);
        Image circle = c, squircle = c, rounded = c;
        maskCircle(circle);
        maskSquircle(squircle);
        maskRoundedRect(rounded, 192 * 0.16f);
        add("Android", "ADAPT CIRCLE", std::move(circle));
        add("Android", "ADAPT SQUIRCLE", std::move(squircle));
        add("Android", "ADAPT ROUNDED", std::move(rounded));
    }
    for (int px : windowsIcoSizes()) add("Windows", std::to_string(px), renderIcon(src.get(s.windows.image), px, s.windows));
    add("Web", "FAV 16", renderIcon(src.get(s.web.image), 16, s.web));
    add("Web", "FAV 32", renderIcon(src.get(s.web.image), 32, s.web));
    add("Web", "APPLE 180", renderIcon(src.get(s.appleTouch.image), 180, s.appleTouch));
    add("Web", "PWA 192", renderIcon(src.get(s.web.image), 192, s.web));
    {
        Image mk = renderIcon(src.get(s.webMaskable.image), 192, s.webMaskable);
        maskCircle(mk);
        add("Web", "MASKABLE 192", std::move(mk));
    }
    for (int px : {16, 22, 24, 32, 48, 64, 128, 256}) add("Linux", std::to_string(px), renderIcon(src.get(s.linux.image), px, s.linux));
    return v;
}

Image iconsContactSheet(const IconSpec& s, const std::string& dir, std::vector<std::string>* warnings) {
    std::vector<IconPreview> all = previewIcons(s, dir, warnings);
    const int pad = 10, label = 12, rowLabelW = 80;
    // layout rows by platform, wrap at maxW
    const int maxW = 1600;
    struct Placed { const IconPreview* p; int x, y; };
    std::vector<Placed> placed;
    std::vector<std::pair<std::string, int>> rowTitles;
    int y = pad, sheetW = 0;
    std::string current;
    int x = 0, rowH = 0;
    for (auto& p : all) {
        if (p.platform != current) {
            if (!current.empty()) y += rowH + label + pad * 2;
            current = p.platform;
            rowTitles.push_back({current, y});
            x = rowLabelW;
            rowH = 0;
        }
        int w = std::max(p.size, textWidth(p.label) + 2);
        if (x + w > maxW) { y += rowH + label + pad; x = rowLabelW; rowH = 0; }
        placed.push_back({&p, x + (w - p.size) / 2, y});
        x += w + pad;
        rowH = std::max(rowH, p.size);
        sheetW = std::max(sheetW, x);
    }
    y += rowH + label + pad * 2;
    // checkerboard shows transparency
    Image sheet = makeImage(sheetW + pad, y, {0.16f, 0.17f, 0.19f, 1});
    for (int yy = 0; yy < sheet.height; ++yy)
        for (int xx = 0; xx < sheet.width; ++xx)
            if (((xx / 8) + (yy / 8)) % 2) { uint8_t* q = sheet.px(xx, yy); q[0] = 46; q[1] = 48; q[2] = 53; }
    for (auto& [t, ty] : rowTitles) drawText(sheet, pad, ty + 2, t, {1, 0.85f, 0.45f, 1}, 1);
    for (auto& pl : placed) {
        blit(sheet, pl.p->image, pl.x, pl.y);
        int lw = textWidth(pl.p->label);
        drawText(sheet, pl.x + (pl.p->size - lw) / 2, pl.y + pl.p->size + 3, pl.p->label, {0.85f, 0.88f, 0.92f, 1}, 1);
    }
    flatten(sheet, {0, 0, 0, 1});
    return sheet;
}

} // namespace rts::branding
