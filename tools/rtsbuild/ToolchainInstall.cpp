// Toolchain installers (see Toolchains.h): everything goes under ~/.rtsengine/toolchains, nothing
// needs sudo. Downloads use curl (macOS, Windows 10+, Linux) or PowerShell, show percentages,
// resume nothing but are idempotent: a finished download / install is detected and skipped.
#include "tools/rtsbuild/Toolchains.h"
#include "tools/rtsbuild/ToolchainUtil.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>

namespace rts::build {

using namespace tc;

namespace {

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
constexpr bool kArm = true;
#else
constexpr bool kArm = false;
#endif

constexpr const char* kEmsdkGit = "https://github.com/emscripten-core/emsdk.git";
constexpr const char* kEmsdkZip = "https://github.com/emscripten-core/emsdk/archive/refs/heads/main.zip";
constexpr const char* kCmakeVersion = "3.30.5";
constexpr const char* kNinjaVersion = "1.12.1";
constexpr const char* kGradleVersion = "8.10.2";
constexpr const char* kCmdlineTools = "11076708";   // Android command line tools 12.0
constexpr const char* kNdkPkg = "ndk;27.0.12077973";
constexpr const char* kSdkCmakePkg = "cmake;3.22.1";

int sh(const std::string& cmd, const ToolLog& log) {
    log("$ " + cmd);
    std::fflush(stdout);
    int rc = std::system(sysCmd(cmd).c_str());
#if !defined(_WIN32)
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    std::fflush(stdout);
    return rc;
}

std::string inDir(const fs::path& dir, const std::string& cmd, const ToolEnv& env) {
    return (env.host == "windows" ? "cd /d " : "cd ") + shq(dir) + " && " + cmd;
}

std::string mb(uint64_t b) { return b < 1048576 ? std::to_string((b + 1023) / 1024) + " КБ" : std::to_string((b + 524288) / 1048576) + " МБ"; }

uint64_t fileSize(const fs::path& p) { std::error_code ec; auto s = fs::file_size(p, ec); return ec ? 0 : uint64_t(s); }

// Content-Length of the final response (after redirects), 0 when unknown.
uint64_t remoteSize(const std::string& url, const fs::path& curl) {
    if (curl.empty()) return 0;
    std::string head = capture(shq(curl) + " -sIL --max-time 30 \"" + url + "\"");
    uint64_t len = 0;
    size_t pos = 0;
    while (pos < head.size()) {
        size_t e = head.find('\n', pos);
        std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? head.size() : e + 1;
        std::string low;
        for (char c : line) low += char(std::tolower(static_cast<unsigned char>(c)));
        if (low.rfind("content-length:", 0) == 0) {
            uint64_t v = std::strtoull(line.c_str() + 15, nullptr, 10);
            if (v > 0) len = v;   // the last non-zero one: redirects answer with 0
        }
    }
    return len;
}

std::string download(const std::string& url, const fs::path& dest, const std::string& what, const ToolEnv& env, const ToolLog& log) {
    if (fileSize(dest) > 0) { log("уже скачано: " + dest.string()); return {}; }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path part = dest.string() + ".part";
    fs::remove(part, ec);
    fs::path curl = findExecutable("curl", env);
    std::string cmd;
    if (!curl.empty()) cmd = shq(curl) + " -fL -sS --retry 3 --connect-timeout 30 -o " + shq(part) + " \"" + url + "\"";
    else if (env.host == "windows")
        cmd = "powershell -NoProfile -Command \"$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing -Uri '" + url + "' -OutFile '" + part.string() + "'\"";
    else return "нет curl — установите curl и повторите";
    const uint64_t total = remoteSize(url, curl);
    log("загрузка " + what + ": " + url + (total ? " (" + mb(total) + ")" : ""));
    std::atomic<bool> done{false};
    int rc = 0;
    std::thread t([&] {
        std::fflush(stdout);
        rc = std::system(sysCmd(cmd).c_str());
#if !defined(_WIN32)
        if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
        done = true;
    });
    int lastPct = -1;
    uint64_t lastBytes = 0;
    auto lastPrint = std::chrono::steady_clock::now();
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const uint64_t n = fileSize(part);
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPrint < std::chrono::seconds(1)) continue;
        if (total) {
            int pct = int(n * 100 / total);
            if (pct >= lastPct + 5 || (pct != lastPct && now - lastPrint > std::chrono::seconds(10))) {
                log("  " + what + ": " + std::to_string(pct) + "% (" + mb(n) + " из " + mb(total) + ")");
                lastPct = pct;
                lastPrint = now;
            }
        } else if (n >= lastBytes + (16u << 20)) {
            log("  " + what + ": " + mb(n));
            lastBytes = n;
            lastPrint = now;
        }
    }
    t.join();
    if (rc != 0 || fileSize(part) == 0) {
        fs::remove(part, ec);
        return "не удалось скачать " + url + " (код " + std::to_string(rc) + "). Проверьте подключение к интернету / прокси и повторите.";
    }
    fs::rename(part, dest, ec);
    if (ec) return "не удалось переименовать " + part.string() + ": " + ec.message();
    log("  " + what + ": 100% (" + mb(fileSize(dest)) + ")");
    return {};
}

void makeExecutable(const fs::path& dir) {
#if !defined(_WIN32)
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        std::string parent = p.parent_path().filename().string(), ext = p.extension().string();
        if (parent == "bin" || ext == ".sh" || ext == ".py" || ext.empty())
            fs::permissions(p, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    }
#else
    (void)dir;
#endif
}

// Unpacks into a fresh `dest`; returns the single top-level folder (or dest itself).
std::string extract(const fs::path& archive, const fs::path& dest, fs::path& top, const ToolEnv& env, const ToolLog& log) {
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    const std::string name = archive.filename().string();
    const bool zip = name.size() > 4 && name.substr(name.size() - 4) == ".zip";
    log("распаковка " + name);
    int rc = 1;
    if (!zip) rc = sh("tar -xzf " + shq(archive) + " -C " + shq(dest), log);
    else if (env.host == "windows") {
        rc = sh("tar -xf " + shq(archive) + " -C " + shq(dest), log);   // bsdtar, Windows 10+
        if (rc != 0) rc = sh("powershell -NoProfile -Command \"Expand-Archive -Force -Path '" + archive.string() + "' -DestinationPath '" + dest.string() + "'\"", log);
    } else if (!findExecutable("unzip", env).empty()) rc = sh("unzip -q -o " + shq(archive) + " -d " + shq(dest), log);
    else if (fs::path py = findExecutable("python3", env); !py.empty()) {
        rc = sh(shq(py) + " -m zipfile -e " + shq(archive) + " " + shq(dest), log);
        makeExecutable(dest);   // zipfile drops the executable bits
    } else rc = sh("tar -xf " + shq(archive) + " -C " + shq(dest), log);
    std::vector<fs::path> entries;
    for (auto it = fs::directory_iterator(dest, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) entries.push_back(it->path());
    if (rc != 0 || entries.empty()) {
        fs::remove(archive, ec);   // a broken download must not be reused next time
        return "не удалось распаковать " + archive.string() + " (архив удалён, повторите установку)";
    }
    top = entries.size() == 1 && fs::is_directory(entries[0], ec) ? entries[0] : dest;
    return {};
}

std::string moveInto(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) return "не удалось переместить " + from.string() + " -> " + to.string() + ": " + ec.message();
    return {};
}

// archive URL -> dest folder (its single top folder becomes `dest`)
std::string fetchTree(const std::string& url, const std::string& file, const fs::path& dest, const std::string& what, const ToolEnv& env, const ToolLog& log) {
    const fs::path tcd = toolchainsDir(env);
    const fs::path arc = tcd / "_downloads" / file;
    if (auto e = download(url, arc, what, env, log); !e.empty()) return e;
    fs::path top;
    const fs::path tmp = tcd / "_tmp" / what;
    if (auto e = extract(arc, tmp, top, env, log); !e.empty()) return e;
    if (auto e = moveInto(top, dest); !e.empty()) return e;
    std::error_code ec;
    fs::remove_all(tcd / "_tmp", ec);
    fs::remove(arc, ec);   // keep the disk small: the unpacked tree is what counts
    makeExecutable(dest / "bin");
    return {};
}

std::string osName(const ToolEnv& env) { return env.host == "macos" ? "mac" : env.host == "windows" ? "windows" : "linux"; }

// ------------------------------------------------------------------ cmake + ninja
std::string installCmake(const ToolEnv& env, const ToolLog& log) {
    const fs::path tcd = toolchainsDir(env);
    const bool win = env.host == "windows";
    fs::path py = findExecutable(win ? "python" : "python3", env);
    // RTS_TOOLCHAIN_NO_PIP=1: skip pip (broken / offline index), use the release archives
    if (env.var("RTS_TOOLCHAIN_NO_PIP") == "1") py.clear();
    if (!py.empty() && !(env.host == "macos" && py == fs::path("/usr/bin/python3") && !there("/Library/Developer/CommandLineTools"))) {
        const fs::path venv = tcd / "py", bin = venv / (win ? "Scripts" : "bin");
        fs::path vpy = exeIn(bin, win ? "python" : "python3", env);
        if (vpy.empty() && sh(shq(py) + " -m venv " + shq(venv), log) == 0) vpy = exeIn(bin, win ? "python" : "python3", env);
        if (!vpy.empty() && sh(shq(vpy) + " -m pip install --disable-pip-version-check --upgrade \"cmake>=3.22\" ninja", log) == 0 && !exeIn(bin, "cmake", env).empty()) {
            log("CMake и Ninja: " + bin.string());
            return {};
        }
        log("pip не сработал — пробую архивы с GitHub");
    }
    // official release archives
    std::string cm = std::string("cmake-") + kCmakeVersion + "-";
    std::string file = env.host == "macos" ? cm + "macos-universal.tar.gz"
                     : win ? cm + (kArm ? "windows-arm64.zip" : "windows-x86_64.zip")
                           : cm + (kArm ? "linux-aarch64.tar.gz" : "linux-x86_64.tar.gz");
    std::string err = fetchTree(std::string("https://github.com/Kitware/CMake/releases/download/v") + kCmakeVersion + "/" + file, file, tcd / "cmake", "cmake", env, log);
    if (err.empty()) {
        makeExecutable(tcd / "cmake" / "CMake.app" / "Contents" / "bin");
        std::string nf = env.host == "macos" ? "ninja-mac.zip" : win ? "ninja-win.zip" : kArm ? "ninja-linux-aarch64.zip" : "ninja-linux.zip";
        if (auto e = fetchTree(std::string("https://github.com/ninja-build/ninja/releases/download/v") + kNinjaVersion + "/" + nf, nf, tcd / "ninja", "ninja", env, log); !e.empty())
            log("предупреждение: ninja: " + e);
        makeExecutable(tcd / "ninja");
        return {};
    }
    if (fs::path brew = findExecutable("brew", env); !brew.empty()) {
        log("архив не скачался (" + err + ") — пробую Homebrew");
        if (sh(shq(brew) + " install cmake ninja", log) == 0) return {};
    }
    return "CMake: " + err;
}

// ------------------------------------------------------------------ emsdk
std::string installEmsdk(const ToolEnv& env, const ToolLog& log) {
    const fs::path dir = toolchainsDir(env) / "emsdk";
    const bool win = env.host == "windows";
    fs::path py = findExecutable(win ? "python" : "python3", env);
    if (py.empty() && win) py = findExecutable("py", env);
    if (py.empty()) return "для emsdk нужен Python 3: " + std::string(win ? "https://www.python.org/downloads/" : env.host == "macos" ? "xcode-select --install" : "sudo apt install python3");
    if (!isFile(dir / "emsdk.py")) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir.parent_path(), ec);
        fs::path git = findExecutable("git", env);
        int rc = git.empty() ? 1 : sh(shq(git) + " clone --depth 1 " + kEmsdkGit + " " + shq(dir), log);
        if (rc != 0) {
            if (!git.empty()) log("git clone не удался — скачиваю архив");
            if (auto e = fetchTree(kEmsdkZip, "emsdk-main.zip", dir, "emsdk", env, log); !e.empty()) return e;
        }
        if (!isFile(dir / "emsdk.py")) return "emsdk не скачан: нет " + (dir / "emsdk.py").string();
    }
    setEnvVar("PYTHONUNBUFFERED", "1");   // emsdk's progress in order with its errors when piped into the editor
    if (sh(inDir(dir, shq(py) + " emsdk.py install latest", env), log) != 0)
        return "emsdk install latest не удался (см. журнал выше; нужен доступ к storage.googleapis.com)";
    if (sh(inDir(dir, shq(py) + " emsdk.py activate latest", env), log) != 0) return "emsdk activate latest не удался";
    if (exeIn(dir / "upstream" / "emscripten", "emcc", env).empty()) return "после установки не найден emcc в " + (dir / "upstream" / "emscripten").string();
    log("Emscripten: " + dir.string());
    return {};
}

// ------------------------------------------------------------------ JDK, Gradle, Android SDK
std::string installJdk(const ToolEnv& env, const ToolLog& log) {
    const std::string os = osName(env), arch = kArm ? "aarch64" : "x64";
    const std::string file = "temurin-17-" + os + "-" + arch + (env.host == "windows" ? ".zip" : ".tar.gz");
    const std::string url = "https://api.adoptium.net/v3/binary/latest/17/ga/" + os + "/" + arch + "/jdk/hotspot/normal/eclipse";
    const fs::path dest = toolchainsDir(env) / "jdk-17";
    if (auto e = fetchTree(url, file, dest, "jdk", env, log); !e.empty()) return e;
    makeExecutable(dest / "Contents" / "Home" / "bin");
    if (!isFile(dest / "release") && !isFile(dest / "Contents" / "Home" / "release")) return "JDK распакован, но не найден release-файл в " + dest.string();
    log("JDK 17: " + dest.string());
    return {};
}

std::string installGradle(const ToolEnv& env, const ToolLog& log) {
    const std::string file = std::string("gradle-") + kGradleVersion + "-bin.zip";
    const fs::path dest = toolchainsDir(env) / (std::string("gradle-") + kGradleVersion);
    if (!exeIn(dest / "bin", "gradle", env).empty()) { log("Gradle уже установлен: " + dest.string()); return {}; }
    if (auto e = fetchTree("https://services.gradle.org/distributions/" + file, file, dest, "gradle", env, log); !e.empty()) return e;
    if (exeIn(dest / "bin", "gradle", env).empty()) return "Gradle распакован, но нет " + (dest / "bin" / "gradle").string();
    return {};
}

std::string installAndroid(const std::set<std::string>& ids, const ToolEnv& env, const ToolLog& log) {
    ToolchainStatus st = checkToolchain("android", env);
    if (const Requirement* jdk = st.find("jdk"); jdk && !jdk->found) {
        if (auto e = installJdk(env, log); !e.empty()) return e;
        st = checkToolchain("android", env);
    }
    const Requirement* jdk = st.find("jdk");
    if (!jdk || !jdk->found) return "нет JDK 17+";
    setEnvVar("JAVA_HOME", jdk->path);
    const fs::path tcd = toolchainsDir(env);
    fs::path sdk = st.env.count("ANDROID_HOME") ? fs::path(st.env.at("ANDROID_HOME")) : tcd / "android-sdk";
    const bool win = env.host == "windows";
    fs::path mgr = exeIn(sdk / "cmdline-tools" / "latest" / "bin", "sdkmanager", env);
    if (mgr.empty()) {
        const std::string file = std::string("commandlinetools-") + (env.host == "macos" ? "mac" : win ? "win" : "linux") + "-" + kCmdlineTools + "_latest.zip";
        if (auto e = fetchTree("https://dl.google.com/android/repository/" + file, file, sdk / "cmdline-tools" / "latest", "android-cmdline-tools", env, log); !e.empty()) return e;
        mgr = exeIn(sdk / "cmdline-tools" / "latest" / "bin", "sdkmanager", env);
        if (mgr.empty()) return "не найден sdkmanager после распаковки в " + (sdk / "cmdline-tools" / "latest").string();
    }
    // sdkmanager asks y/N per licence: answer from a file (works in sh and cmd alike)
    const fs::path yes = tcd / "_downloads" / "yes.txt";
    {
        std::error_code ec;
        fs::create_directories(yes.parent_path(), ec);
        std::ofstream f(yes);
        for (int i = 0; i < 100; ++i) f << "y\n";
    }
    const std::string base = shq(mgr) + " --sdk_root=" + shq(sdk);
    if (sh(base + " --licenses < " + shq(yes), log) != 0) log("предупреждение: sdkmanager --licenses вернул ошибку");
    std::vector<std::string> pkgs;
    auto want = [&](const std::string& id) { return ids.count(id) || ids.count("android"); };
    auto need = [&](const std::string& id) { const Requirement* r = st.find(id); return r && !r->found; };
    if (want("android-sdk") && need("android-sdk")) pkgs = {"platform-tools", "platforms;android-35", "build-tools;35.0.0"};
    if (want("android-ndk") && need("android-ndk")) pkgs.push_back(kNdkPkg);
    if (want("android-cmake") && need("android-cmake")) pkgs.push_back(kSdkCmakePkg);
    if (!pkgs.empty()) {
        std::string cmd = base + " --install";
        for (auto& p : pkgs) cmd += " \"" + p + "\"";
        if (sh(cmd + " < " + shq(yes), log) != 0) return "sdkmanager --install не удался (см. журнал выше)";
    }
    log("Android SDK: " + sdk.string());
    return {};
}

std::string installOne(const std::string& id, const ToolEnv& env, const ToolLog& log) {
    if (id == "cmake" || id == "ninja") return installCmake(env, log);
    if (id == "emsdk") return installEmsdk(env, log);
    if (id == "jdk") return installJdk(env, log);
    if (id == "gradle") return installGradle(env, log);
    if (id == "android-sdk" || id == "android-ndk" || id == "android-cmake") return installAndroid({id}, env, log);
    if (id == "xcode-clt" || (id == "python3" && env.host == "macos")) {
        sh("xcode-select --install", log);
        return "запущен установщик Apple (Command Line Tools): завершите его и повторите сборку";
    }
    return "«" + id + "» не устанавливается автоматически";
}

} // namespace

std::string installToolchain(const std::string& what, const ToolEnv& env, const ToolLog& log) {
    static const std::set<std::string> kTargets = {"web", "android", "macos", "ios", "windows", "linux"};
    log("Каталог инструментов: " + toolchainsDir(env).string());
    if (!kTargets.count(what)) {
        std::string e = installOne(what, env, log);
        if (e.empty()) log("установлено: " + what);
        return e;
    }
    ToolchainStatus st = checkToolchain(what, env);
    if (!st.hostSupported) return st.hostNote;
    std::string errors;
    std::set<std::string> done;
    for (const Requirement& r : st.reqs) {
        if (r.found || done.count(r.id)) continue;
        if (!r.required && r.id != "ninja") continue;
        if (!r.autoInstall) { errors += r.name + ": установите вручную — " + r.manual + "\n"; continue; }
        log("==> установка: " + r.name + (r.size.empty() ? "" : " (" + r.size + ")"));
        std::string e;
        if (r.id.rfind("android-", 0) == 0) {
            e = installAndroid({"android"}, env, log);
            done.insert({"android-sdk", "android-ndk", "android-cmake", "jdk"});
        } else {
            e = installOne(r.id, env, log);
            if (r.id == "cmake") done.insert("ninja");
        }
        done.insert(r.id);
        if (!e.empty()) {
            if (r.required) errors += r.name + ": " + e + "\n";
            else log("предупреждение: " + r.name + ": " + e);
        }
        applyToolchainEnv(checkToolchain(what, env));   // later steps see what was just installed
    }
    st = checkToolchain(what, env);
    if (errors.empty() && !st.ok()) errors = "после установки всё ещё не хватает: " + machineLine(st).substr(22);
    if (errors.empty()) log("Готово: всё для " + what + " установлено");
    return errors;
}

} // namespace rts::build
