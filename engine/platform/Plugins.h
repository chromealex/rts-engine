// Native plugins: platform SDKs (payments, ads, analytics, ...) behind one asynchronous bridge.
//
// A plugin is a name plus methods. It can be written in C++ (any platform), Java (Android),
// Objective-C (iOS/macOS) or JavaScript (web); the game calls it the same way everywhere:
//
//   plugins::call("yandex-ads", "show", R"({"format":"rewarded"})", [](const plugins::Result& r) {
//       if (r.ok) ... r.data is the JSON reply ...
//   });
//
// Messages are JSON text (UTF-8), so the bridge is identical for every language. Replies and
// events always arrive on the game thread from pump() (the platform calls it every frame), never
// inside call() and never on an SDK's own thread.
//
// Configuration comes from game.json → "plugins": { "<name>": { ...config... } }. Values may be
// per platform: {"android": "...", "ios": "...", "web": "..."} is replaced by the current
// platform's value. The bridge sends the config to each plugin once, as method "init", before the
// first other call. Packaging (sources, Gradle/CocoaPods dependencies, manifest/plist entries)
// is described by plugins/<name>/plugin.json and applied by rtsbuild — see docs/plugins.md.
//
// Standard plugin protocols ("store", "ads") and their C++ front-ends: engine/services.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rts::plugins {

struct Result {
    bool ok = false;
    std::string data = "null"; // JSON reply when ok
    std::string error;         // readable message when !ok ("cancelled" for user cancellations)
};
using Callback = std::function<void(const Result&)>;
using EventHandler = std::function<void(const std::string& event, const std::string& dataJson)>;

// Is the plugin built into this app for this platform?
bool available(const std::string& plugin);
// Calls a method. `cb` (optional) gets exactly one Result on the game thread.
void call(const std::string& plugin, const std::string& method, const std::string& argsJson = "{}", Callback cb = {});
// Events pushed by a plugin (e.g. a purchase finished while the app was in the background).
// Returns an id for removeListener. Listeners of all plugins: plugin == "*".
int addListener(const std::string& plugin, EventHandler handler);
void removeListener(int id);

// game.json is read automatically on first use; these override it (tests, a launcher with several
// games). configure() replaces only the "plugins" block, configureGame() the whole manifest.
void configure(const std::string& pluginsObjectJson);
void configureGame(const std::string& gameJsonText);
// A top-level game.json block ("store", "ads", ...) with per-platform values resolved; "null" if absent.
std::string gameSection(const std::string& key);
std::string platformName(); // "android", "ios", "web", "macos", "windows", "linux"
// The config the plugin receives in "init" (per-platform values resolved), "{}" if none.
std::string configFor(const std::string& plugin);
// Plugins listed in game.json "plugins", in file order.
std::vector<std::string> configured();

// Delivers queued replies and events. Platforms call it once per frame; tests call it directly.
void pump();

// ---------------------------------------------------------------- writing plugins in C++
// Thread-safe handle a plugin uses to answer one call (once) — from any thread, any time later.
class Reply {
public:
    void ok(const std::string& dataJson = "{}") const;
    void fail(const std::string& error) const;
    explicit operator bool() const { return id_ != 0; }
private:
    friend struct Bridge;
    uint64_t id_ = 0;
};

class NativePlugin {
public:
    virtual ~NativePlugin() = default;
    // method == "init" is the first call, with the game.json config as args.
    virtual void call(const std::string& method, const std::string& argsJson, Reply reply) = 0;
    // Called every frame on the game thread (from pump()) once the plugin got "init": run SDK
    // callbacks here (e.g. SteamAPI_RunCallbacks).
    virtual void update() {}
};

// Registers a C++ plugin under `name` (replaces an earlier one). Thread-safe.
void registerPlugin(const std::string& name, std::shared_ptr<NativePlugin> plugin);
void unregisterPlugin(const std::string& name);
// A plugin (any language, any thread) sends an event to the game. Events "app.pause" and
// "app.resume" are also turned into FocusLost/FocusGained platform events (an ad or a store dialog
// covers the game: pause the match, mute audio); "app.log" prints its data to the engine log;
// "app.banner" {"height": points, "position": "top"|"bottom"} adds an ad banner to safeAreaInsets().
void emit(const std::string& plugin, const std::string& event, const std::string& dataJson = "{}");

// Static registration in a C++ plugin's .cpp:  RTS_PLUGIN("my-sdk", MySdkPlugin)
#define RTS_PLUGIN(name, Class) \
    static const bool rtsPluginRegistered_##Class = (::rts::plugins::registerPlugin(name, std::make_shared<Class>()), true)

// ---------------------------------------------------------------- platform backends
namespace detail {
// Java (Android), Objective-C (Apple) and JavaScript (web) plugins are reached through the
// platform's backend; C++ plugins win when both exist.
struct Backend {
    virtual ~Backend() = default;
    virtual bool available(const std::string& plugin) = 0;
    // Must answer through completeCall(callId, ...) exactly once (any thread).
    virtual void call(const std::string& plugin, const std::string& method, const std::string& argsJson, uint64_t callId) = 0;
};
void setBackend(Backend* backend); // set by the platform at startup (not owned)
// Returns the text of game.json ("" if unavailable); the platform installs it.
void setConfigLoader(std::function<std::string()> loader);
// Calls `method` on every configured, available plugin without a callback. The platform sends
// "lifecycle" {"event": "ready"} on the first interactive frames (Yandex Games moderation needs it).
void broadcast(const std::string& method, const std::string& argsJson = "{}");
void completeCall(uint64_t callId, bool ok, const std::string& dataOrError);
// Turns {"android": x, "ios": y, ...} into the value for `platformName`, recursively.
std::string resolvePlatformValues(const std::string& json, const std::string& platformName);
} // namespace detail

} // namespace rts::plugins
