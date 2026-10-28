// Native plugins in packaged builds (platforms agent; hooks in main.cpp are one-liners).
//
// Plugins are folders with a plugin.json, searched in <project>/plugins/<name>, then
// <engine>/plugins/<name>. The game picks them in game.json "plugins" (or --plugins a,b,c).
// plugin.json (all sections optional; strings in manifest/plist/podfile entries may use
// ${key} / ${a.b} = the plugin's game.json config resolved for the target platform):
//
//   { "name": "admob", "provides": ["ads"],
//     "cpp":     { "sources": ["src/*.cpp"] },                       // every platform
//     "android": { "java": "android/java", "classes": { "admob": "com.rtsengine.plugins.AdMobPlugin" },
//                  "dependencies": ["com.google.android.gms:play-services-ads:25.5.0"],
//                  "platformDependencies": ["ru.rustore.sdk:bom:2026.10.01"],
//                  "repositories": ["https://maven.example.com/repo"],
//                  "manifest": { "root": ["<uses-permission .../>"], "application": ["<meta-data .../>"],
//                                "activity": ["<intent-filter>...</intent-filter>"] } },
//     "ios":     { "sources": ["ios/*.m"], "classes": { "admob": "RTSAdMobPlugin" },
//                  "pods": ["pod 'Google-Mobile-Ads-SDK', '~> 13.11'"], "frameworks": ["StoreKit"],
//                  "plist": ["<key>GADApplicationIdentifier</key><string>${appId}</string>"],
//                  "skAdNetworks": ["cstr6suwn9"] },   // merged into one SKAdNetworkItems list
//     "macos":   { same keys as ios },
//     "windows" / "linux" / "macos": { "sources": ["src/*.cpp"],            // desktop C++ SDKs
//                  "includeDirs": ["${sdk}/public"], "libraries": ["${sdk}/.../steam_api64.lib"],
//                  "runtime": ["${sdk}/.../steam_api64.dll"] },           // copied next to the executable
//     "defaults": { "sdk": "${env.STEAMWORKS_SDK}" },  // config fallbacks; ${env.X} = environment
//     "sdkHint": "how to get the SDK (printed when a path above is missing)",
//     "web":     { "scripts": ["/sdk.js"], "js": ["web/admob.js"] } }
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace rts::build {

struct PluginSet {
    std::string platform;                     // android | ios | macos | web | windows | linux
    std::vector<std::string> names;           // plugins included for this platform
    std::vector<std::filesystem::path> dirs;  // their folders
    std::vector<std::string> notes;           // human-readable report lines
    bool mocks = false;                       // game lists mock-store / mock-ads → compile them in
};

// gameJsonText: game.json; override: comma list from --plugins ("" = game.json "plugins").
PluginSet loadPlugins(const std::filesystem::path& engine, const std::filesystem::path& project,
                      const std::string& gameJsonText, const std::string& platform, const std::string& override);

// Copies C++ (and iOS/macOS Objective-C) sources to <dir>/plugins/<name>/ and writes
// <dir>/plugins.cmake (RTS_PLUGIN_SOURCES, _FRAMEWORKS, _PLIST_XML, _INCLUDE_DIRS, _LIBRARIES, _RUNTIME_FILES).
// Returns the plugins.cmake path (written even when empty, so stale plugins never leak in).
std::filesystem::path writePluginsCMake(const PluginSet& set, const std::filesystem::path& dir);

// Desktop: shared libraries the plugins need next to the executable (also in plugins.cmake).
std::vector<std::filesystem::path> runtimeFiles(const PluginSet& set);

// Android Gradle template: values for @PLUGIN_DEPENDENCIES@, @PLUGIN_REPOSITORIES@,
// @PLUGIN_MANIFEST_ROOT@, @PLUGIN_MANIFEST_APPLICATION@, @PLUGIN_MANIFEST_ACTIVITY@.
std::map<std::string, std::string> androidPlaceholders(const PluginSet& set);
void copyAndroidJava(const PluginSet& set, const std::filesystem::path& javaRoot);

// iOS: Podfile text for target rts_client ("" when no plugin needs CocoaPods).
std::string podfile(const PluginSet& set, const std::string& iosVersion);

// Web: copies plugin scripts to <out>/plugins/ and inserts <script> tags into index.html.
void installWebPlugins(const PluginSet& set, const std::filesystem::path& outDir);

} // namespace rts::build
