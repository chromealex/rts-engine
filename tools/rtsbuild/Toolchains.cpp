// Toolchain detection (see Toolchains.h). Installers are in ToolchainInstall.cpp.
#include "tools/rtsbuild/Toolchains.h"
#include "tools/rtsbuild/ToolchainUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rts::build {

using namespace tc;

// ------------------------------------------------------------------ environment
std::string ToolEnv::var(const std::string& name) const {
    if (auto it = vars.find(name); it != vars.end()) return it->second;
    if (!inherit) return {};
    const char* v = std::getenv(name.c_str());
    return v ? v : "";
}

ToolEnv ToolEnv::current() {
    ToolEnv e;
#if defined(_WIN32)
    e.host = "windows";
#elif defined(__APPLE__)
    e.host = "macos";
#else
    e.host = "linux";
#endif
    std::string h = e.var("HOME");
    if (h.empty()) h = e.var("USERPROFILE");
    e.home = h;
    if (e.host != "windows")
        e.systemDirs = {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/opt/local/bin", "/snap/bin", "/Applications/CMake.app/Contents/bin"};
    return e;
}

fs::path toolchainsDir(const ToolEnv& env) {
    if (std::string d = env.var("RTS_TOOLCHAINS_DIR"); !d.empty()) return d;
    return env.home / ".rtsengine" / "toolchains";
}

namespace tc {

bool there(const fs::path& p) { std::error_code ec; return !p.empty() && fs::exists(p, ec); }
bool isFile(const fs::path& p) { std::error_code ec; return !p.empty() && fs::is_regular_file(p, ec); }

std::string readFile(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

std::vector<fs::path> subdirs(const fs::path& dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) v.push_back(it->path());
    // newest version last: numeric compare of dotted parts ("27.0.12077973" > "25.2.9519653")
    auto parts = [](const std::string& s) {
        std::vector<long> r;
        std::string cur;
        for (char c : s + ".") {
            if (c >= '0' && c <= '9') cur += c;
            else if (!cur.empty()) { r.push_back(std::atol(cur.c_str())); cur.clear(); }
        }
        return r;
    };
    std::sort(v.begin(), v.end(), [&](const fs::path& a, const fs::path& b) {
        auto pa = parts(a.filename().string()), pb = parts(b.filename().string());
        return pa != pb ? pa < pb : a.filename().string() < b.filename().string();
    });
    return v;
}

std::string capture(const std::string& cmd) {
#if defined(_WIN32)
    FILE* p = _popen(sysCmd(cmd + " 2>&1").c_str(), "r");
#else
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
#endif
    if (!p) return {};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    return out;
}

std::string shq(const fs::path& p) {
#if defined(_WIN32)
    return "\"" + p.string() + "\"";
#else
    std::string s = p.string(), o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}

std::string firstLine(const std::string& s) {
    size_t e = s.find_first_of("\r\n");
    return e == std::string::npos ? s : s.substr(0, e);
}

char pathSep(const ToolEnv& env) { return env.host == "windows" ? ';' : ':'; }

std::vector<fs::path> extraDirs(const ToolEnv& env) {
    const fs::path tcd = toolchainsDir(env);
    const bool win = env.host == "windows";
    std::vector<fs::path> d = {
        tcd / "py" / (win ? "Scripts" : "bin"), tcd / "cmake" / "bin", tcd / "cmake" / "CMake.app" / "Contents" / "bin", tcd / "ninja",
    };
    for (auto& g : subdirs(tcd)) if (g.filename().string().rfind("gradle-", 0) == 0) d.push_back(g / "bin");
    for (auto& s : env.systemDirs) d.push_back(s);
    if (!env.home.empty()) {
        d.push_back(env.home / ".local" / "bin");
        for (auto& p : subdirs(env.home / "Library" / "Python")) d.push_back(p / "bin");   // pip --user on macOS
    }
    if (win) {
        for (const char* v : {"ProgramFiles", "ProgramFiles(x86)"})
            if (std::string pf = env.var(v); !pf.empty()) { d.push_back(fs::path(pf) / "CMake" / "bin"); d.push_back(fs::path(pf) / "Git" / "cmd"); }
        if (std::string la = env.var("LOCALAPPDATA"); !la.empty())
            for (auto& p : subdirs(fs::path(la) / "Programs" / "Python")) { d.push_back(p); d.push_back(p / "Scripts"); }
        if (std::string ad = env.var("APPDATA"); !ad.empty())
            for (auto& p : subdirs(fs::path(ad) / "Python")) d.push_back(p / "Scripts");
    }
    return d;
}

fs::path exeIn(const fs::path& dir, const std::string& name, const ToolEnv& env) {
    if (dir.empty()) return {};
    if (env.host == "windows") {
        for (const char* ext : {".exe", ".bat", ".cmd", ""}) if (isFile(dir / (name + ext))) return dir / (name + ext);
        return {};
    }
    return isFile(dir / name) ? dir / name : fs::path();
}

} // namespace tc

fs::path findExecutable(const std::string& name, const ToolEnv& env) {
    std::stringstream ss(env.var("PATH"));
    for (std::string dir; std::getline(ss, dir, pathSep(env));) {
        // Windows: the Store "python.exe" alias opens the Store instead of running Python
        if (dir.empty() || (env.host == "windows" && dir.find("WindowsApps") != std::string::npos)) continue;
        if (fs::path p = exeIn(dir, name, env); !p.empty()) return p;
    }
    for (auto& dir : extraDirs(env)) if (fs::path p = exeIn(dir, name, env); !p.empty()) return p;
    return {};
}

int javaMajorFromRelease(const std::string& text) {
    size_t a = text.find("JAVA_VERSION=\"");
    if (a == std::string::npos) return 0;
    std::string v = text.substr(a + 14);
    int major = std::atoi(v.c_str());
    if (major == 1) major = std::atoi(v.c_str() + 2);   // 1.8.0
    return major;
}

std::string machineLine(const ToolchainStatus& st) {
    std::string s = "RTS_MISSING_TOOLCHAIN " + st.target + ":";
    auto m = st.missing();
    for (size_t i = 0; i < m.size(); ++i) s += (i ? "," : " ") + m[i];
    return s;
}

// ------------------------------------------------------------------ status
std::vector<std::string> ToolchainStatus::missing() const {
    std::vector<std::string> m;
    for (auto& r : reqs) if (r.required && !r.found) m.push_back(r.id);
    return m;
}
const Requirement* ToolchainStatus::find(const std::string& id) const {
    for (auto& r : reqs) if (r.id == id) return &r;
    return nullptr;
}

Json ToolchainStatus::toJson() const {
    Json j = Json::object();
    j.set("target", Json::string(target));
    j.set("host", Json::string(host));
    j.set("hostSupported", Json::fromBool(hostSupported));
    if (!hostNote.empty()) j.set("hostNote", Json::string(hostNote));
    j.set("ok", Json::fromBool(ok()));
    Json miss = Json::array();
    for (auto& m : missing()) miss.push(Json::string(m));
    j.set("missing", std::move(miss));
    Json arr = Json::array();
    for (auto& r : reqs) {
        Json e = Json::object();
        e.set("id", Json::string(r.id));
        e.set("name", Json::string(r.name));
        e.set("required", Json::fromBool(r.required));
        e.set("found", Json::fromBool(r.found));
        e.set("autoInstall", Json::fromBool(r.autoInstall));
        for (auto& [k, v] : std::vector<std::pair<const char*, const std::string*>>{
                 {"path", &r.path}, {"version", &r.version}, {"detail", &r.detail}, {"install", &r.install}, {"size", &r.size}, {"manual", &r.manual}})
            if (!v->empty()) e.set(k, Json::string(*v));
        arr.push(std::move(e));
    }
    j.set("requirements", std::move(arr));
    return j;
}

std::string ToolchainStatus::table() const {
    std::string s = "Инструменты для " + target + " (хост " + host + "): ";
    if (!hostSupported) s += hostNote + "\n";
    else if (ok()) s += "всё готово\n";
    else {
        s += "не хватает:";
        for (auto& m : missing()) s += " " + m;
        s += "\n";
    }
    for (auto& r : reqs) {
        s += r.found ? "  [есть] " : r.required ? "  [НЕТ]  " : "  [—]    ";
        s += r.name + (r.required ? "" : " (необязательно)");
        if (r.found) s += (r.version.empty() ? "" : "  " + r.version) + "  " + r.path;
        else {
            if (!r.detail.empty()) s += " — " + r.detail;
            if (r.autoInstall) s += "\n           установка: " + r.install + (r.size.empty() ? "" : " (" + r.size + ")");
            else if (!r.manual.empty()) s += "\n           вручную: " + r.manual;
        }
        s += "\n";
    }
    return s;
}

// ------------------------------------------------------------------ requirements
namespace {

Requirement req(const char* id, const char* name) {
    Requirement r;
    r.id = id;
    r.name = name;
    return r;
}

void foundAt(Requirement& r, const fs::path& p, const std::string& version = "") {
    r.found = true;
    r.path = p.string();
    r.version = version;
}

std::string toolVersion(const fs::path& exe, const ToolEnv& env, const char* flag = "--version") {
    if (!env.probe || exe.empty()) return {};
    std::string out = firstLine(capture(shq(exe) + " " + flag));
    // the last word that starts with a digit: "cmake version 3.28.3", "g++ (Ubuntu 13.2.0-23) 13.2.0", "emcc (...) 3.1.6 ()"
    std::string best, word;
    for (char c : out + " ") {
        if (c == ' ' || c == '\t') { if (!word.empty() && std::isdigit(static_cast<unsigned char>(word[0]))) best = word; word.clear(); }
        else word += c;
    }
    return best.empty() ? out : best;
}

bool macCltInstalled(const ToolEnv& env) {
    if (there("/Library/Developer/CommandLineTools/usr/bin/clang")) return true;
    if (!env.probe) return false;
    std::string p = firstLine(capture("/usr/bin/xcode-select -p"));
    return !p.empty() && p[0] == '/' && there(p);
}

Requirement reqCmake(ToolchainStatus& st, const ToolEnv& env) {
    Requirement r = req("cmake", "CMake");
    r.autoInstall = true;
    r.install = "pip (cmake + ninja) в " + (toolchainsDir(env) / "py").string() + ", без pip — архив с GitHub";
    r.size = "≈ 50 МБ";
    if (fs::path p = findExecutable("cmake", env); !p.empty()) { foundAt(r, p, toolVersion(p, env)); st.binDirs.push_back(p.parent_path()); }
    else r.detail = "не установлен";
    return r;
}

Requirement reqNinja(ToolchainStatus& st, const ToolEnv& env) {
    Requirement r = req("ninja", "Ninja");
    r.required = false;
    r.autoInstall = true;
    r.install = "вместе с CMake (pip)";
    r.size = "≈ 1 МБ";
    if (fs::path p = findExecutable("ninja", env); !p.empty()) { foundAt(r, p, toolVersion(p, env)); st.binDirs.push_back(p.parent_path()); }
    else r.detail = "не найден (сборка пойдёт без него, медленнее)";
    return r;
}

Requirement reqPython(ToolchainStatus& st, const ToolEnv& env) {
    Requirement r = req("python3", "Python 3");
    r.manual = env.host == "macos" ? "xcode-select --install (Command Line Tools содержат python3) или https://www.python.org/downloads/"
             : env.host == "windows" ? "https://www.python.org/downloads/ (отметьте «Add python.exe to PATH») или winget install Python.Python.3.12"
                                     : "sudo apt install python3 python3-venv (или пакетный менеджер вашего дистрибутива)";
    fs::path p = findExecutable(env.host == "windows" ? "python" : "python3", env);
    if (p.empty() && env.host == "windows") p = findExecutable("py", env);
    // /usr/bin/python3 on a Mac without Command Line Tools is a stub that opens an install dialog
    if (!p.empty() && env.host == "macos" && p == fs::path("/usr/bin/python3") && !macCltInstalled(env)) {
        r.detail = "/usr/bin/python3 есть, но Command Line Tools не установлены";
        r.autoInstall = true;
        r.install = "xcode-select --install (откроется установщик Apple)";
        r.size = "≈ 700 МБ";
        return r;
    }
    if (!p.empty()) { foundAt(r, p, toolVersion(p, env)); st.binDirs.push_back(p.parent_path()); }
    else r.detail = "не найден (нужен emsdk)";
    return r;
}

// The engine is C++20 (lambda captures of structured bindings need clang 16+): older
// Emscripten (Ubuntu's 3.1.6) fails on it.
constexpr const char* kMinEmscripten = "3.1.40";
bool emTooOld(const std::string& ver) {
    auto parts = [](const std::string& v) {
        std::vector<int> r;
        std::string cur;
        for (char c : v + ".") {
            if (std::isdigit(static_cast<unsigned char>(c))) cur += c;
            else if (!cur.empty()) { r.push_back(std::atoi(cur.c_str())); cur.clear(); if (c != '.') break; }
            else break;
        }
        return r;
    };
    auto have = parts(ver);
    return !have.empty() && have < parts(kMinEmscripten);   // unknown version: trust it
}

// An activated emsdk: emcc exists and the config written by `emsdk activate` is there.
bool emsdkReady(const fs::path& dir, const ToolEnv& env) {
    return !exeIn(dir / "upstream" / "emscripten", "emcc", env).empty() && isFile(dir / ".emscripten");
}

Requirement reqEmsdk(ToolchainStatus& st, const ToolEnv& env) {
    Requirement r = req("emsdk", "Emscripten (emsdk)");
    r.autoInstall = true;
    r.install = "git clone emsdk в " + (toolchainsDir(env) / "emsdk").string() + ", emsdk install latest, emsdk activate latest";
    r.size = "≈ 400 МБ загрузки, ≈ 1.5 ГБ на диске";
    std::vector<fs::path> dirs;
    if (std::string e = env.var("EMSDK"); !e.empty()) dirs.push_back(e);
    dirs.push_back(toolchainsDir(env) / "emsdk");
    if (!env.home.empty()) dirs.push_back(env.home / "emsdk");
    for (auto& d : dirs) {
        if (!emsdkReady(d, env)) {
            if (isFile(d / "emsdk.py") && r.detail.empty()) r.detail = "emsdk есть в " + d.string() + ", но не установлен (emsdk install/activate latest)";
            continue;
        }
        std::string ver = readFile(d / "upstream" / "emscripten" / "emscripten-version.txt");
        ver.erase(std::remove_if(ver.begin(), ver.end(), [](char c) { return c == '"' || c == '\n' || c == '\r'; }), ver.end());
        if (emTooOld(ver)) { r.detail = "в " + d.string() + " Emscripten " + ver + " — слишком старый (нужен " + kMinEmscripten + "+)"; continue; }
        foundAt(r, d, ver);
        st.binDirs.push_back(d);
        st.binDirs.push_back(d / "upstream" / "emscripten");
        auto nodes = subdirs(d / "node");
        if (!nodes.empty()) st.binDirs.push_back(env.host == "windows" ? nodes.back() / "bin" : nodes.back() / "bin");
        st.env["EMSDK"] = d.string();
        st.env["EM_CONFIG"] = (d / ".emscripten").string();
        // emsdk ships its own python on macOS / Windows; emcc prefers EMSDK_PYTHON
        for (auto& py : subdirs(d / "python")) {
            fs::path exe = env.host == "windows" ? py / "python.exe" : py / "bin" / "python3";
            if (isFile(exe)) st.env["EMSDK_PYTHON"] = exe.string();
        }
        return r;
    }
    // Homebrew / distro emscripten: emcmake on PATH manages its own config
    if (fs::path p = findExecutable("emcmake", env); !p.empty()) {
        std::string ver = toolVersion(findExecutable("emcc", env), env);
        if (!emTooOld(ver)) {
            foundAt(r, p.parent_path(), ver);
            st.binDirs.push_back(p.parent_path());
            return r;
        }
        r.detail = "найден " + p.parent_path().string() + "/emcc " + ver + " — слишком старый (нужен " + kMinEmscripten + "+), будет установлен emsdk";
        return r;
    }
    if (r.detail.empty()) r.detail = "не установлен";
    return r;
}

// ---- Android
constexpr const char* kAndroidPlatform = "platforms;android-35";   // platform/android/template: compileSdk 35
constexpr const char* kBuildTools = "build-tools;35.0.0";          // AGP 8.7 default
constexpr const char* kNdk = "ndk;27.0.12077973";                  // AGP 8.7 default NDK
constexpr const char* kSdkCmake = "cmake;3.22.1";                  // AGP default CMake

fs::path jdkHome(const fs::path& dir) {
    if (isFile(dir / "release") && (isFile(dir / "bin" / "java") || isFile(dir / "bin" / "java.exe"))) return dir;
    if (isFile(dir / "Contents" / "Home" / "release")) return dir / "Contents" / "Home";
    return {};
}

Requirement reqJdk(ToolchainStatus& st, const ToolEnv& env) {
    Requirement r = req("jdk", "JDK 17+ (Java)");
    r.autoInstall = true;
    r.install = "Temurin JDK 17 (Adoptium) в " + (toolchainsDir(env) / "jdk-17").string();
    r.size = "≈ 190 МБ";
    std::vector<fs::path> cands;
    if (std::string jh = env.var("JAVA_HOME"); !jh.empty()) cands.push_back(jh);
    cands.push_back(toolchainsDir(env) / "jdk-17");
    if (env.host == "macos" && env.probe) {
        std::string p = firstLine(capture("/usr/libexec/java_home -v 17+"));
        if (!p.empty() && p[0] == '/') cands.push_back(p);
    }
    if (env.host == "macos") {
        cands.push_back("/Applications/Android Studio.app/Contents/jbr");
        for (auto& d : subdirs("/Library/Java/JavaVirtualMachines")) cands.push_back(d);
    } else if (env.host == "windows") {
        for (const char* v : {"ProgramFiles", "LOCALAPPDATA"})
            if (std::string pf = env.var(v); !pf.empty()) cands.push_back(fs::path(pf) / "Android" / "Android Studio" / "jbr");
        if (std::string pf = env.var("ProgramFiles"); !pf.empty())
            for (const char* vendor : {"Eclipse Adoptium", "Java", "Microsoft", "Zulu"})
                for (auto& d : subdirs(fs::path(pf) / vendor)) cands.push_back(d);
    } else {
        if (!env.home.empty()) cands.push_back(env.home / "android-studio" / "jbr");
        if (!env.systemDirs.empty()) for (auto& d : subdirs("/usr/lib/jvm")) cands.push_back(d);
    }
    if (fs::path java = findExecutable("java", env); !java.empty()) {
        std::error_code ec;
        fs::path real = fs::canonical(java, ec);
        if (!ec) cands.push_back(real.parent_path().parent_path());
    }
    int bestTooNew = 0;
    for (auto& c : cands) {
        fs::path h = jdkHome(c);
        if (h.empty()) continue;
        int major = javaMajorFromRelease(readFile(h / "release"));
        if (major >= 17 && major <= 23) {   // Gradle 8.10 / AGP 8.7 run on 17..23
            foundAt(r, h, std::to_string(major));
            st.binDirs.push_back(h / "bin");
            st.env["JAVA_HOME"] = h.string();
            return r;
        }
        if (major > 23) bestTooNew = major;
    }
    r.detail = bestTooNew ? "найден только JDK " + std::to_string(bestTooNew) + " (Gradle 8.10 работает с 17–23)" : "не найден";
    return r;
}

fs::path androidSdkRoot(const ToolEnv& env) {
    std::vector<fs::path> c;
    for (const char* v : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) if (std::string e = env.var(v); !e.empty()) c.push_back(e);
    c.push_back(toolchainsDir(env) / "android-sdk");
    if (!env.home.empty()) for (const char* rel : {"Library/Android/sdk", "Android/Sdk", "Android/sdk"}) c.push_back(env.home / rel);
    if (std::string la = env.var("LOCALAPPDATA"); !la.empty()) c.push_back(fs::path(la) / "Android" / "Sdk");
    for (auto& p : c)
        if (there(p / "platforms") || there(p / "cmdline-tools") || there(p / "platform-tools") || there(p / "ndk")) return p;
    return {};
}

void androidReqs(ToolchainStatus& st, const ToolEnv& env) {
    fs::path sdk = androidSdkRoot(env);
    const std::string ours = (toolchainsDir(env) / "android-sdk").string();
    Requirement s = req("android-sdk", "Android SDK (platform 35, build-tools, platform-tools)");
    s.autoInstall = true;
    s.install = "Android command line tools + sdkmanager: platform-tools, " + std::string(kAndroidPlatform) + ", " + kBuildTools +
                " в " + (sdk.empty() ? ours : sdk.string()) + " (лицензии принимаются автоматически)";
    s.size = "≈ 300 МБ";
    Requirement n = req("android-ndk", "Android NDK");
    n.autoInstall = true;
    n.install = std::string("sdkmanager \"") + kNdk + "\"";
    n.size = "≈ 700 МБ загрузки, ≈ 2 ГБ на диске";
    Requirement c = req("android-cmake", "CMake для Android (SDK)");
    c.autoInstall = true;
    c.install = std::string("sdkmanager \"") + kSdkCmake + "\"";
    c.size = "≈ 30 МБ";
    if (sdk.empty()) {
        s.detail = n.detail = c.detail = "Android SDK не найден (ANDROID_HOME)";
    } else {
        std::vector<std::string> miss;
        if (!there(sdk / "platforms" / "android-35")) miss.push_back(kAndroidPlatform);
        if (subdirs(sdk / "build-tools").empty()) miss.push_back(kBuildTools);
        if (!there(sdk / "platform-tools")) miss.push_back("platform-tools");
        if (miss.empty()) foundAt(s, sdk);
        else { s.detail = "в " + sdk.string() + " нет:"; for (auto& m : miss) s.detail += " " + m; }
        auto ndks = subdirs(sdk / "ndk");
        if (!ndks.empty()) foundAt(n, ndks.back(), ndks.back().filename().string()); else n.detail = "нет в " + sdk.string();
        auto cms = subdirs(sdk / "cmake");
        if (!cms.empty()) foundAt(c, cms.back(), cms.back().filename().string()); else c.detail = "нет в " + sdk.string();
        st.env["ANDROID_HOME"] = sdk.string();
        st.binDirs.push_back(sdk / "platform-tools");
    }
    st.reqs.push_back(s);
    st.reqs.push_back(n);
    st.reqs.push_back(c);
    Requirement g = req("gradle", "Gradle");
    g.autoInstall = true;
    g.install = "Gradle 8.10.2 (services.gradle.org) в " + (toolchainsDir(env) / "gradle-8.10.2").string();
    g.size = "≈ 130 МБ";
    if (fs::path p = findExecutable("gradle", env); !p.empty()) {
        std::string dir = p.parent_path().parent_path().filename().string();
        foundAt(g, p, dir.rfind("gradle-", 0) == 0 ? dir.substr(7) : "");
        st.binDirs.push_back(p.parent_path());
    } else g.detail = "не найден";
    st.reqs.push_back(g);
}

// ---- Apple
Requirement reqClt(ToolchainStatus&, const ToolEnv& env) {
    Requirement r = req("xcode-clt", "Xcode Command Line Tools (clang)");
    r.autoInstall = true;
    r.install = "xcode-select --install (откроется установщик Apple, нужно подтвердить)";
    r.size = "≈ 700 МБ";
    if (macCltInstalled(env)) foundAt(r, env.probe ? firstLine(capture("/usr/bin/xcode-select -p")) : "/Library/Developer/CommandLineTools");
    else r.detail = "не установлены";
    return r;
}

Requirement reqXcode(ToolchainStatus&, const ToolEnv& env) {
    Requirement r = req("xcode", "Xcode (iOS SDK)");
    r.manual = "установите Xcode из App Store, затем: sudo xcode-select -s /Applications/Xcode.app";
    std::string sel = env.probe ? firstLine(capture("/usr/bin/xcode-select -p")) : "";
    if (sel.find(".app/") != std::string::npos && there(sel)) {
        foundAt(r, sel, env.probe ? firstLine(capture("/usr/bin/xcodebuild -version")) : "");
        return r;
    }
    r.detail = there("/Applications/Xcode.app") ? "Xcode есть, но не выбран: sudo xcode-select -s /Applications/Xcode.app" : "не установлен";
    return r;
}

// ---- Windows / Linux compilers
Requirement reqMsvc(ToolchainStatus& st, const ToolEnv& env) {
    Requirement r = req("msvc", "Visual Studio 2022 / Build Tools (C++)");
    r.manual = "winget install Microsoft.VisualStudio.2022.BuildTools --override \"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive\" "
               "или https://visualstudio.microsoft.com/visual-cpp-build-tools/ (нагрузка «Разработка классических приложений на C++»)";
    std::string pf = env.var("ProgramFiles(x86)");
    fs::path vswhere = fs::path(pf.empty() ? "C:\\Program Files (x86)" : pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    if (env.probe && isFile(vswhere)) {
        std::string p = firstLine(capture(shq(vswhere) + " -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"));
        if (!p.empty() && there(p)) {
            foundAt(r, p, firstLine(capture(shq(vswhere) + " -latest -products * -property catalog_productDisplayVersion")));
            // VS ships cmake + ninja: usable when no other cmake is installed
            fs::path vsCmake = fs::path(p) / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake";
            st.binDirs.push_back(vsCmake / "CMake" / "bin");
            st.binDirs.push_back(vsCmake / "Ninja");
            return r;
        }
    }
    r.detail = "не найден (vswhere)";
    return r;
}

Requirement reqCxx(ToolchainStatus& st, const ToolEnv& env) {
    Requirement r = req("cxx", "Компилятор C++ (g++ / clang++)");
    r.manual = "sudo apt install build-essential libx11-dev libgl-dev libasound2-dev (или аналог в вашем дистрибутиве)";
    for (const char* c : {"c++", "g++", "clang++"})
        if (fs::path p = findExecutable(c, env); !p.empty()) { foundAt(r, p, toolVersion(p, env)); st.binDirs.push_back(p.parent_path()); return r; }
    r.detail = "не найден";
    return r;
}

} // namespace

ToolchainStatus checkToolchain(const std::string& target, const ToolEnv& env) {
    ToolchainStatus st;
    st.target = target;
    st.host = env.host;
    if (target == "web") {
        st.reqs.push_back(reqCmake(st, env));
        st.reqs.push_back(reqPython(st, env));
        Requirement em = reqEmsdk(st, env);
        // Homebrew emscripten carries its own python; emsdk needs one
        if (em.found && st.env.count("EMSDK_PYTHON")) st.reqs.back().required = false;
        st.reqs.push_back(em);
        st.reqs.push_back(reqNinja(st, env));
    } else if (target == "android") {
        st.reqs.push_back(reqJdk(st, env));
        androidReqs(st, env);
    } else if (target == "macos" || target == "ios") {
        if (env.host != "macos") { st.hostSupported = false; st.hostNote = target == "ios" ? "проект iOS собирается только на Mac" : "macOS собирается только на Mac"; return st; }
        st.reqs.push_back(target == "ios" ? reqXcode(st, env) : reqClt(st, env));
        st.reqs.push_back(reqCmake(st, env));
        st.reqs.push_back(reqNinja(st, env));
    } else if (target == "windows") {
        if (env.host == "windows") {
            Requirement vs = reqMsvc(st, env);
            st.reqs.push_back(vs);
            st.reqs.push_back(reqCmake(st, env));
        } else {
            Requirement m = req("mingw", "MinGW-w64 (кросс-компилятор для Windows)");
            m.required = false;
            m.manual = env.host == "macos" ? "brew install mingw-w64" : "sudo apt install mingw-w64";
            for (const char* c : {"x86_64-w64-mingw32-g++", "x86_64-w64-mingw32-clang++"})
                if (fs::path p = findExecutable(c, env); !p.empty()) { foundAt(m, p); st.binDirs.push_back(p.parent_path()); break; }
            if (!m.found) m.detail = "не найден — будут только исходники + build.bat";
            st.reqs.push_back(m);
            if (m.found) st.reqs.push_back(reqCmake(st, env));
        }
    } else if (target == "linux") {
        if (env.host != "linux") { st.hostSupported = false; st.hostNote = "Linux собирается только на Linux"; return st; }
        st.reqs.push_back(reqCxx(st, env));
        st.reqs.push_back(reqCmake(st, env));
        st.reqs.push_back(reqNinja(st, env));
    } else {
        st.hostSupported = false;
        st.hostNote = "неизвестная платформа: " + target;
    }
    // de-duplicate, keep order
    std::vector<fs::path> dirs;
    for (auto& d : st.binDirs) if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
    st.binDirs = dirs;
    return st;
}

void tc::setEnvVar(const std::string& k, const std::string& v) {
#if defined(_WIN32)
    _putenv_s(k.c_str(), v.c_str());
#else
    setenv(k.c_str(), v.c_str(), 1);
#endif
}

void applyToolchainEnv(const ToolchainStatus& st) {
    const ToolEnv env = ToolEnv::current();
    std::string path = env.var("PATH");
    std::string pre;
    for (auto& d : st.binDirs) {
        if (!there(d)) continue;
        std::string s = d.string();
        if ((pathSep(env) + path + pathSep(env)).find(pathSep(env) + s + pathSep(env)) != std::string::npos) continue;
        pre += s + pathSep(env);
    }
    if (!pre.empty()) setEnvVar("PATH", pre + path);
    for (auto& [k, v] : st.env) setEnvVar(k, v);
}

} // namespace rts::build
