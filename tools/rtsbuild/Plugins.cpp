#include "Plugins.h"
#include "engine/pack/FileCopy.h"

#include "engine/platform/PluginJson.h"
#include "engine/platform/Plugins.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace rts::build {
namespace {
namespace json = rts::plugins::json;

std::string readFile(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
void writeFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << s;
}

struct Loaded {
    std::string name;
    fs::path dir;
    json::Value manifest;  // plugin.json
    json::Value config;    // game.json config, resolved for the platform
};

// Per-target cache: loadPlugins() result → the parsed data the writers need.
std::map<std::string, std::vector<Loaded>>& cache() { static std::map<std::string, std::vector<Loaded>> c; return c; }
const std::vector<Loaded>& loaded(const PluginSet& s) { return cache()[s.platform]; }

json::Value section(const Loaded& l, const std::string& platform) { // copy: small, avoids dangling refs
    const json::Value* v = l.manifest.find(platform);
    return v ? *v : json::Value();
}

// ${key} / ${a.b} from the plugin's config, else from plugin.json "defaults"; ${env.NAME} is an
// environment variable. Unknown keys stay visible so the mistake is obvious.
std::string expand(const std::string& s, const Loaded& l, int depth = 0) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        size_t a = s.find("${", i);
        if (a == std::string::npos) { out += s.substr(i); break; }
        size_t b = s.find('}', a);
        if (b == std::string::npos) { out += s.substr(i); break; }
        out += s.substr(i, a - i);
        std::string key = s.substr(a + 2, b - a - 2);
        auto lookup = [&](const json::Value& root) {
            const json::Value* v = &root;
            std::stringstream ks(key);
            for (std::string part; v && std::getline(ks, part, '.');) v = v->find(part);
            return v && !v->isNull() && !v->isObject() && !v->isArray() ? v : nullptr;
        };
        const json::Value* v = lookup(l.config);
        if (!v && depth < 4) v = lookup(l.manifest["defaults"]);
        if (key.rfind("env.", 0) == 0) {
            const char* e = std::getenv(key.c_str() + 4);
            if (e && *e) out += e;
            else std::cerr << "    [plugins] " << l.name << ": environment variable " << key.substr(4) << " is not set\n";
        } else if (v) {
            out += depth < 4 ? expand(v->str(), l, depth + 1) : v->str();
        } else {
            std::cerr << "    [plugins] " << l.name << ": ${" << key << "} is not set in game.json \"plugins\" → \"" << l.name << "\"\n";
            out += "${" + key + "}";
        }
        i = b + 1;
    }
    return out;
}

// Paths of SDKs installed outside the repo: a clear error instead of a compiler/linker one.
std::string requireFile(const std::string& path, const Loaded& l) {
    if (path.find("${") == std::string::npos && fs::exists(path)) return fs::path(path).generic_string();
    std::cout << "ОШИБКА: плагин " << l.name << ": не найден " << path << "\n";
    if (const json::Value* hint = l.manifest.find("sdkHint")) std::cout << "    " << hint->str() << "\n";
    std::exit(1);
}

std::vector<std::string> strings(const json::Value& v) {
    std::vector<std::string> r;
    if (v.isArray()) for (auto& s : v.arr) r.push_back(s.str());
    else if (!v.isNull()) r.push_back(v.str());
    return r;
}

// "src/*.cpp" style patterns (one '*' in the file name) relative to the plugin folder.
std::vector<fs::path> globFiles(const fs::path& base, const std::string& pattern) {
    std::vector<fs::path> r;
    fs::path p = base / pattern;
    std::string fname = p.filename().string();
    if (fname.find('*') == std::string::npos) { if (fs::exists(p)) r.push_back(p); return r; }
    std::string rx = std::regex_replace(fname, std::regex(R"([.^$|()\[\]{}+?\\])"), R"(\$&)");
    rx = std::regex_replace(rx, std::regex(R"(\*)"), ".*");
    std::regex re(rx);
    std::error_code ec;
    for (auto& e : fs::directory_iterator(p.parent_path(), ec))
        if (e.is_regular_file() && std::regex_match(e.path().filename().string(), re)) r.push_back(e.path());
    std::sort(r.begin(), r.end());
    return r;
}

std::string cmakeQuote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\' || c == '$' || c == ';') o += '\\'; o += c; }
    return o + "\"";
}
} // namespace

PluginSet loadPlugins(const fs::path& engine, const fs::path& project, const std::string& gameJsonText,
                      const std::string& platform, const std::string& override) {
    PluginSet set;
    set.platform = platform;
    std::vector<Loaded>& out = cache()[platform];
    out.clear();
    json::Value game = json::parseOr(json::stripComments(gameJsonText), json::Value::object());
    const json::Value& cfg = game["plugins"];
    std::vector<std::string> names;
    if (!override.empty()) {
        std::stringstream ss(override);
        for (std::string n; std::getline(ss, n, ',');) if (!n.empty()) names.push_back(n);
    } else {
        for (auto& [k, _] : cfg.obj) names.push_back(k);
    }
    for (auto& name : names) {
        if (name.rfind("mock-", 0) == 0) { set.mocks = true; continue; } // engine's desktop mocks, compiled in on request
        fs::path dir;
        for (const fs::path& base : {project / "plugins", engine / "plugins"})
            if (fs::exists(base / name / "plugin.json")) { dir = base / name; break; }
        if (dir.empty()) { set.notes.push_back(name + ": plugin.json not found in plugins/ — skipped"); continue; }
        Loaded l;
        l.name = name;
        l.dir = dir;
        std::string err;
        if (!json::parse(json::stripComments(readFile(dir / "plugin.json")), l.manifest, &err)) {
            set.notes.push_back(name + ": plugin.json: " + err + " — skipped");
            continue;
        }
        l.config = json::parseOr(rts::plugins::detail::resolvePlatformValues(json::dump(cfg[name]), platform), json::Value::object());
        bool hasPlatform = !section(l, platform).isNull() || !l.manifest["cpp"].isNull();
        if (!hasPlatform) { set.notes.push_back(name + ": no " + platform + " part — not included"); continue; }
        set.names.push_back(name);
        set.dirs.push_back(dir);
        set.notes.push_back(name + " (" + dir.string() + ")");
        out.push_back(std::move(l));
    }
    return set;
}

fs::path writePluginsCMake(const PluginSet& set, const fs::path& dir) {
    std::vector<std::string> sources, frameworks, plist, skAdNetworks, includes, libraries;
    std::string classes; // Info.plist RTSPlugins: name → Objective-C class
    for (const Loaded& l : loaded(set)) {
        std::vector<std::string> patterns = strings(l.manifest["cpp"]["sources"]);
        const json::Value native = section(l, set.platform);
        if (set.platform != "android" && set.platform != "web") { // native code: Apple, Windows, Linux
            for (auto& s : strings(native["sources"])) patterns.push_back(s);
            // third-party SDKs the developer installs (e.g. Steamworks): must exist at build time
            for (auto& d : strings(native["includeDirs"])) includes.push_back(requireFile(expand(d, l), l));
            for (auto& d : strings(native["libraries"])) libraries.push_back(requireFile(expand(d, l), l));
            for (auto& d : strings(native["runtime"])) requireFile(expand(d, l), l);
        }
        if (set.platform == "ios" || set.platform == "macos") {
            for (auto& f : strings(native["frameworks"])) frameworks.push_back(f);
            for (auto& p : strings(native["plist"])) plist.push_back(expand(p, l));
            for (auto& id : strings(native["skAdNetworks"]))
                if (std::find(skAdNetworks.begin(), skAdNetworks.end(), id) == skAdNetworks.end()) skAdNetworks.push_back(id);
            for (auto& [k, v] : native["classes"].obj) classes += "<key>" + k + "</key><string>" + v.str() + "</string>";
        }
        for (auto& pat : patterns)
            for (const fs::path& f : globFiles(l.dir, pat)) {
                fs::path rel = fs::path("plugins") / l.name / fs::relative(f, l.dir);
                fs::create_directories((dir / rel).parent_path());
                rts::pack::copyFileReplaceOrThrow(f, dir / rel);
                sources.push_back("${CMAKE_CURRENT_LIST_DIR}/" + rel.generic_string());
            }
    }
    // Top-level Info.plist keys must be unique: the first plugin wins, the others are reported.
    std::string plistXml;
    std::set<std::string> keys;
    std::regex keyRx(R"(^\s*<key>([^<]+)</key>)");
    for (auto& p : plist) {
        std::smatch m;
        if (std::regex_search(p, m, keyRx) && !keys.insert(m[1].str()).second) {
            std::cerr << "    [plugins] Info.plist key " << m[1].str() << " is set by two plugins; keeping the first\n";
            continue;
        }
        plistXml += p;
    }
    if (!skAdNetworks.empty()) {
        plistXml += "<key>SKAdNetworkItems</key><array>";
        for (auto& id : skAdNetworks) plistXml += "<dict><key>SKAdNetworkIdentifier</key><string>" + id + ".skadnetwork</string></dict>";
        plistXml += "</array>";
    }
    if (!classes.empty()) plistXml += "<key>RTSPlugins</key><dict>" + classes + "</dict>";
    std::string text = "# Generated by rtsbuild from plugins/*/plugin.json — do not edit.\n";
    text += "set(RTS_PLUGIN_NAMES";
    for (auto& n : set.names) text += " " + cmakeQuote(n);
    text += ")\nset(RTS_PLUGIN_SOURCES";
    for (auto& s : sources) text += "\n    \"" + s + "\"";
    text += ")\nset(RTS_PLUGIN_FRAMEWORKS";
    for (auto& f : frameworks) text += " " + cmakeQuote(f);
    text += ")\nset(RTS_PLUGIN_INCLUDE_DIRS";
    for (auto& d : includes) text += " " + cmakeQuote(d);
    text += ")\nset(RTS_PLUGIN_LIBRARIES";
    for (auto& d : libraries) text += " " + cmakeQuote(d);
    text += ")\nset(RTS_PLUGIN_RUNTIME_FILES";
    for (auto& f : runtimeFiles(set)) text += " " + cmakeQuote(f.generic_string());
    text += ")\nset(RTS_PLUGIN_PLIST_XML " + cmakeQuote(plistXml) + ")\n";
    // desktop mocks only when the game asked for them: otherwise not even compiled into the app
    text += std::string("set(RTS_MOCK_PLUGINS ") + (set.mocks ? "ON" : "OFF") + ")\n";
    fs::path file = dir / "plugins.cmake";
    writeFile(file, text);
    return file;
}

std::vector<fs::path> runtimeFiles(const PluginSet& set) {
    std::vector<fs::path> files;
    if (set.platform == "android" || set.platform == "web") return files;
    for (const Loaded& l : loaded(set))
        for (auto& f : strings(section(l, set.platform)["runtime"])) files.push_back(requireFile(expand(f, l), l));
    return files;
}

std::map<std::string, std::string> androidPlaceholders(const PluginSet& set) {
    std::string deps, repos, root, app, activity;
    std::set<std::string> seenRepo;
    for (const Loaded& l : loaded(set)) {
        const json::Value a = section(l, "android");
        for (auto& d : strings(a["platformDependencies"])) deps += "    implementation platform('" + expand(d, l) + "')\n";
        for (auto& d : strings(a["dependencies"])) deps += "    implementation '" + expand(d, l) + "'\n";
        for (auto& r : strings(a["repositories"])) if (seenRepo.insert(r).second) repos += "        maven { url '" + expand(r, l) + "' }\n";
        for (auto& m : strings(a["manifest"]["root"])) root += "    " + expand(m, l) + "\n";
        for (auto& m : strings(a["manifest"]["application"])) app += "        " + expand(m, l) + "\n";
        for (auto& m : strings(a["manifest"]["activity"])) activity += "            " + expand(m, l) + "\n";
        for (auto& [name, cls] : a["classes"].obj)
            app += "        <meta-data android:name=\"rts.plugin." + name + "\" android:value=\"" + cls.str() + "\" />\n";
    }
    return {{"@PLUGIN_DEPENDENCIES@", deps}, {"@PLUGIN_REPOSITORIES@", repos}, {"@PLUGIN_MANIFEST_ROOT@", root},
            {"@PLUGIN_MANIFEST_APPLICATION@", app}, {"@PLUGIN_MANIFEST_ACTIVITY@", activity}};
}

void copyAndroidJava(const PluginSet& set, const fs::path& javaRoot) {
    for (const Loaded& l : loaded(set)) {
        std::string src = section(l, "android")["java"].str();
        if (src.empty() || !fs::exists(l.dir / src)) continue;
        // per file: MinGW's recursive fs::copy fails into an existing folder
        const fs::path from = l.dir / src;
        fs::create_directories(javaRoot);
        for (auto it = fs::recursive_directory_iterator(from); it != fs::recursive_directory_iterator(); ++it) {
            const fs::path to = javaRoot / fs::relative(it->path(), from);
            if (it->is_directory()) fs::create_directories(to);
            else rts::pack::copyFileReplaceOrThrow(it->path(), to);
        }
    }
}

std::string podfile(const PluginSet& set, const std::string& iosVersion) {
    std::string pods;
    for (const Loaded& l : loaded(set))
        for (auto& p : strings(section(l, "ios")["pods"])) pods += "  " + expand(p, l) + "\n";
    if (pods.empty()) return "";
    return "# Generated by rtsbuild for the plugins in game.json.\nplatform :ios, '" + iosVersion + "'\n"
           "project 'RTSEngine.xcodeproj'\n\ntarget 'rts_client' do\n  use_frameworks! :linkage => :static\n" + pods + "end\n";
}

void installWebPlugins(const PluginSet& set, const fs::path& outDir) {
    std::string tags;
    std::set<std::string> seen;
    for (const Loaded& l : loaded(set)) {
        const json::Value w = section(l, "web");
        for (auto& s : strings(w["scripts"])) if (seen.insert(s).second) tags += "<script src=\"" + expand(s, l) + "\"></script>";
        for (auto& pat : strings(w["js"]))
            for (const fs::path& f : globFiles(l.dir, pat)) {
                fs::path rel = fs::path("plugins") / l.name / f.filename();
                fs::create_directories((outDir / rel).parent_path());
                rts::pack::copyFileReplaceOrThrow(f, outDir / rel);
                tags += "<script src=\"" + rel.generic_string() + "\"></script>";
            }
        // the plugin's config, so JS can read it before init (e.g. to load an SDK early)
        tags += "<script>RTSPlugins.config=RTSPlugins.config||{};RTSPlugins.config[" + json::quote(l.name) + "]=" + json::dump(l.config) + ";</script>";
    }
    fs::path html = outDir / "index.html";
    std::string t = readFile(html);
    std::smatch m;
    std::regex marker(R"(<meta[^>]*rts-plugin-scripts[^>]*>)");
    if (!std::regex_search(t, m, marker)) {
        if (!tags.empty()) std::cerr << "    [plugins] index.html has no rts-plugin-scripts marker: web plugins not inserted\n";
        return;
    }
    size_t at = size_t(m.position(0) + m.length(0));
    writeFile(html, t.substr(0, at) + tags + t.substr(at));
}

} // namespace rts::build
