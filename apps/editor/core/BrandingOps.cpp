#include "BrandingOps.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "tools/branding/Icons.h"
#include "tools/branding/Splash.h"

namespace fs = std::filesystem;

namespace rts::editor {

namespace {

Json strings(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (auto& s : v) a.push(Json::string(s));
    return a;
}

void needProject(const Workspace& ws) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open: call open_game first");
}

// Shallow merge of objects (null values remove keys), used by branding_set with "merge": true.
Json merged(const Json& base, const Json& patch) {
    if (!base.isObject() || !patch.isObject()) return patch;
    Json out = base;
    for (auto& [k, v] : patch.members()) {
        if (v.isNull()) out.erase(k);
        else out.set(k, out[k].isObject() && v.isObject() ? merged(out[k], v) : v);
    }
    return out;
}

// Validates both blocks against the files of the project. Throws on format errors.
Json check(const Json& manifest, const std::string& dir) {
    std::vector<std::string> warnings;
    branding::IconSpec icons = branding::parseIcons(manifest);
    if (icons.master.empty() && fs::exists(fs::path(dir) / "icon.png")) icons.master = "icon.png";
    for (auto& w : branding::validateIcons(icons, dir)) warnings.push_back(w);
    splash::Spec sp = splash::parseSpec(manifest["splash"], &warnings);
    for (auto& img : sp.images())
        if (!fs::exists(fs::path(dir) / img)) warnings.push_back("splash: image not found: " + img);
    branding::NativeSplash n = branding::nativeSplash(manifest);
    Json r = Json::object();
    Json res = Json::object();
    res.set("iconMaster", Json::string(icons.master.empty() ? "(built-in default icon)" : icons.master));
    res.set("splashEnabled", Json::fromBool(sp.enabled));
    if (sp.enabled) {
        res.set("splashLayers", Json::number(int64_t(sp.layers.size())));
        res.set("splashSequenceSeconds", Json::number(double(sp.sequenceLength()), 2));
        res.set("splashMinimumSeconds", Json::number(double(sp.minimumTime()), 2));
        res.set("splashImages", strings(sp.images()));
        res.set("nativeLaunchImage", Json::string(n.image));
        res.set("nativeLaunchBackground", Json::string(splash::colorHex(n.background)));
    }
    r.set("resolved", res);
    r.set("warnings", strings(warnings));
    return r;
}

} // namespace

std::string base64Encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size()) v |= uint32_t(uint8_t(in[i + 1])) << 8;
        if (i + 2 < in.size()) v |= uint32_t(uint8_t(in[i + 2]));
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? t[v & 63] : '=';
    }
    return out;
}

void registerBrandingOps(Workspace& ws) {
    Workspace* w = &ws;
    auto add = [&ws](const char* name, const char* desc, const char* schema, Workspace::OpFn fn) {
        ws.registerOp({name, desc, Json::parse(schema), std::move(fn), false});
    };
    add("branding_get",
        "Game icons and splash screen: game.json 'icons' (master image >= 1024 px + per-platform overrides: ios, macos, android "
        "{legacy, round, foreground, background, monochrome}, windows, web, linux; padding/background/cornerRadius for opaque icons) "
        "and 'splash' (background, image/frames/sheet, fit, logos with animation keyframes or fadeIn/fadeOut/pulse/rotate/slide/zoom, "
        "minSeconds, maxSeconds, loop, skippable, progress bar, native launch screen). Returns both blocks, what they resolve to and warnings. "
        "Format: rules_schema, section 'Icons and splash'.",
        "{}", [w](const Json&) {
            needProject(*w);
            Json r = check(w->project.manifest, w->project.dir);
            r.set("icons", w->project.manifest["icons"]);
            if (w->project.manifest.has("icon")) r.set("icon", w->project.manifest["icon"]);
            r.set("splash", w->project.manifest["splash"]);
            return r;
        });
    add("branding_set",
        "Set game.json 'icons' and/or 'splash' (validated before saving; images are project-relative paths). Pass null to remove a block; "
        "merge=true merges objects into the existing blocks (null values remove keys). Check the result with icons_preview / splash_preview.",
        R"({"properties": {"icons": {"description": "icons block (object) or null"}, "splash": {"description": "splash block (object) or null"},
            "merge": {"type": "boolean"}}})",
        [w](const Json& p) {
            needProject(*w);
            if (!p.has("icons") && !p.has("splash")) throw std::runtime_error("nothing to set: pass 'icons' and/or 'splash'");
            Json m = w->project.manifest;
            const bool merge = p.getBool("merge");
            for (const char* key : {"icons", "splash"}) {
                if (!p.has(key)) continue;
                const Json& v = p[key];
                if (!v.isNull() && !v.isObject()) throw std::runtime_error(std::string(key) + ": expected an object or null");
                if (v.isNull()) m.erase(key);
                else m.set(key, merge ? merged(m[key], v) : v);
            }
            Json r = check(m, w->project.dir);   // throws on format errors: nothing saved
            w->project.manifest = m;
            std::string err;
            if (!w->project.saveManifest(&err)) throw std::runtime_error(err);
            w->note({Change::Project, 0, 0, -1, -1, -1, "иконки и заставка"});
            r.set("ok", Json::fromBool(true));
            r.set("summary", Json::string("game.json: icons / splash updated"));
            return r;
        });
    add("icons_preview",
        "Render every icon the build generates (iOS, macOS, Android legacy/round/adaptive with circle and squircle masks, Windows .ico sizes, "
        "web favicon/apple-touch/PWA/maskable, Linux hicolor) at real pixel size into one PNG contact sheet you can look at.",
        "{}", [w](const Json&) {
            needProject(*w);
            std::vector<std::string> warnings;
            branding::IconSpec s = branding::parseIcons(w->project.manifest);
            if (s.master.empty() && fs::exists(fs::path(w->project.dir) / "icon.png")) s.master = "icon.png";
            for (auto& x : branding::validateIcons(s, w->project.dir)) warnings.push_back(x);
            branding::Image sheet = branding::iconsContactSheet(s, w->project.dir, &warnings);
            Json r = Json::object();
            r.set("width", Json::number(int64_t(sheet.width)));
            r.set("height", Json::number(int64_t(sheet.height)));
            r.set("warnings", strings(warnings));
            r.set("png_base64", Json::string(base64Encode(branding::encodePng(sheet))));
            return r;
        });
    add("splash_preview",
        "Render the splash screen at a few moments of its timeline (default: 5 frames over the logo sequence) into a PNG strip, "
        "with the progress bar as if loading took the minimum time. width/height of each frame in pixels (default 320x180, portrait games 180x320).",
        R"({"properties": {"times": {"type": "array", "items": {"type": "number"}, "description": "seconds"},
            "width": {"type": "integer"}, "height": {"type": "integer"}}})",
        [w](const Json& p) {
            needProject(*w);
            std::vector<std::string> warnings;
            splash::Spec s = splash::parseSpec(w->project.manifest["splash"], &warnings);
            if (!s.enabled) throw std::runtime_error("game.json has no 'splash' block (set one with branding_set)");
            const bool portrait = w->project.manifest.getStr("orientation") == "portrait";
            int W = int(std::clamp<int64_t>(p.getInt("width", portrait ? 180 : 320), 32, 1920));
            int H = int(std::clamp<int64_t>(p.getInt("height", portrait ? 320 : 180), 32, 1920));
            std::vector<float> times;
            for (auto& t : p["times"].items()) times.push_back(float(t.toDouble(0)));
            if (times.empty()) {
                float len = std::max(2.0f, std::max(s.sequenceLength(), s.minimumTime()));
                for (int i = 0; i < 5; ++i) times.push_back(len * float(i) / 4.0f * 0.98f);
            }
            if (times.size() > 12) times.resize(12);
            branding::Image strip = branding::splashStrip(s, w->project.dir, times, W, H, &warnings);
            Json r = Json::object();
            Json ts = Json::array();
            for (float t : times) ts.push(Json::number(double(t), 2));
            r.set("times", ts);
            r.set("minimumSeconds", Json::number(double(s.minimumTime()), 2));
            r.set("warnings", strings(warnings));
            r.set("png_base64", Json::string(base64Encode(branding::encodePng(strip))));
            return r;
        });
}

} // namespace rts::editor
