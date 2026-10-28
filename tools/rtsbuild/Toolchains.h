// Toolchains: what each build target needs on this machine (cmake, Emscripten, JDK, Android SDK,
// Xcode, ...), where it was found and how to install what is missing.
//
// Search goes beyond PATH because the editor is often started from Finder / a desktop shortcut
// with a minimal PATH: Homebrew (/opt/homebrew, /usr/local), ~/.local/bin, Python user bin dirs,
// ~/emsdk, $EMSDK, ANDROID_HOME / ANDROID_SDK_ROOT and the usual SDK folders, JAVA_HOME,
// /usr/libexec/java_home, Visual Studio (vswhere) and our own ~/.rtsengine/toolchains/
// (RTS_TOOLCHAINS_DIR overrides it). rtsbuild prepends the found folders to PATH for its children.
//
// Installers put everything under ~/.rtsengine/toolchains (no sudo, no system changes; removing
// that folder removes it all): emsdk (git clone or GitHub zip + emsdk install/activate latest),
// cmake + ninja (pip venv, else GitHub release archives, else Homebrew), Android command line
// tools + sdkmanager packages (licences accepted), Temurin JDK 17, Gradle.
#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "engine/data/Json.h"

namespace rts::build {

namespace fs = std::filesystem;

constexpr int kExitMissingToolchain = 3;   // rtsbuild exit code: a required tool is missing

// The machine as the checks see it. current() reads the real process; tests build a fake one
// (host, home, variables, no system folders, no probing processes).
struct ToolEnv {
    std::string host;                          // macos | linux | windows
    fs::path home;
    std::map<std::string, std::string> vars;   // overrides / the whole environment when !inherit
    bool inherit = true;                       // fall back to getenv
    bool probe = true;                         // may run tools (--version, xcode-select, java_home, vswhere)
    std::vector<fs::path> systemDirs;          // /opt/homebrew/bin, /usr/local/bin, ... (existing or not)
    std::string var(const std::string& name) const;
    static ToolEnv current();
};

struct Requirement {
    std::string id, name;                      // "emsdk", "Emscripten (emsdk)"
    bool required = true, found = false, autoInstall = false;
    std::string path, version, detail;         // detail: what is wrong / what was found
    std::string install;                       // what the installer will do
    std::string size;                          // download size, "≈ 400 МБ"
    std::string manual;                        // how to install by hand (not auto-installable)
};

struct ToolchainStatus {
    std::string target, host;
    bool hostSupported = true;
    std::string hostNote;                      // "macOS собирается только на Mac"
    std::vector<Requirement> reqs;
    std::vector<fs::path> binDirs;             // prepended to PATH for child processes
    std::map<std::string, std::string> env;    // EMSDK, EM_CONFIG, JAVA_HOME, ANDROID_HOME, ...
    std::vector<std::string> missing() const;  // ids of required requirements not found
    bool ok() const { return missing().empty(); }
    const Requirement* find(const std::string& id) const;
    Json toJson() const;
    std::string table() const;                 // human readable, Russian
};

fs::path toolchainsDir(const ToolEnv& env);
fs::path findExecutable(const std::string& name, const ToolEnv& env);   // PATH, then the extra folders
ToolchainStatus checkToolchain(const std::string& target, const ToolEnv& env);
// Puts binDirs in front of PATH and sets env for this process (children inherit it).
void applyToolchainEnv(const ToolchainStatus& st);

using ToolLog = std::function<void(const std::string&)>;
// Installs what `what` needs: a target (web, android, ...) or one requirement id (emsdk, cmake,
// jdk, android-sdk, android-ndk, android-cmake, gradle, xcode-clt). Idempotent.
// Returns an empty string on success, otherwise a readable error.
std::string installToolchain(const std::string& what, const ToolEnv& env, const ToolLog& log);

// Exposed for tests.
int javaMajorFromRelease(const std::string& releaseText);   // JAVA_VERSION="17.0.12" -> 17, "1.8.0" -> 8
std::string machineLine(const ToolchainStatus& st);          // "RTS_MISSING_TOOLCHAIN web: emsdk,cmake"

} // namespace rts::build
