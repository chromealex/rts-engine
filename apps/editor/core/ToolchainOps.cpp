#include "ToolchainOps.h"

#include <cctype>
#include <stdexcept>

namespace rts::editor {

namespace {
std::string word(const Json& p, const char* key) {
    std::string v = p.getStr(key);
    for (char c : v) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) throw std::runtime_error(std::string("bad ") + key + ": " + v);
    return v;
}
} // namespace

void registerToolchainOps(Workspace& ws) {
    Workspace* w = &ws;
    auto add = [&ws](const char* name, const char* desc, const char* schema, Workspace::OpFn fn) {
        ws.registerOp({name, desc, Json::parse(schema), std::move(fn), false});
    };
    add("toolchain_check",
        "What building for a target needs on this machine (cmake, Emscripten, JDK 17, Android SDK/NDK, Gradle, Xcode, Visual Studio, ...): "
        "per requirement id, name, found, path, version, why it is missing, whether it installs automatically, what the installer does and "
        "the download size. 'missing' lists required ids; 'hostSupported' false = this OS can't build the target at all.",
        R"({"properties": {"target": {"type": "string", "enum": ["web", "android", "macos", "ios", "windows", "linux"]}}, "required": ["target"]})",
        [w](const Json& p) { return w->toolchainCheck(word(p, "target")); });
    add("toolchain_install",
        "Install what a target needs (or one requirement id from toolchain_check: emsdk, cmake, jdk, android-sdk, android-ndk, android-cmake, "
        "gradle, xcode-clt) into ~/.rtsengine/toolchains, in the background with progress in the build log. Poll build_status, then call "
        "toolchain_check again. Downloads can be large (Emscripten ~400 MB, Android NDK ~700 MB).",
        R"({"properties": {"target": {"type": "string"}, "id": {"type": "string"}}})", [w](const Json& p) {
            std::string what = p.has("id") ? word(p, "id") : word(p, "target");
            if (what.empty()) throw std::runtime_error("give 'target' or 'id'");
            w->startTool("--install-toolchain " + what, "toolchain-" + what, false);
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", Json::string("Установка инструментов для " + what + " запущена (build_status — ход установки)"));
            return r;
        });
}

} // namespace rts::editor
