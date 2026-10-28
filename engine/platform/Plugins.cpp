#include "Plugins.h"
#include "PluginJson.h"
#if RTS_WITH_MOCK_PLUGINS
#include "engine/services/MockPlugins.h"
#endif
#include <cstdlib>

#include <cstdio>
#include <map>
#include <mutex>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace rts::plugins {
namespace {

#if defined(__ANDROID__)
constexpr const char* kPlatform = "android";
#elif defined(__EMSCRIPTEN__)
constexpr const char* kPlatform = "web";
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
constexpr const char* kPlatform = "ios";
#else
constexpr const char* kPlatform = "macos";
#endif
#elif defined(_WIN32)
constexpr const char* kPlatform = "windows";
#else
constexpr const char* kPlatform = "linux";
#endif

bool isPlatformKey(const std::string& k) {
    return k == "android" || k == "ios" || k == "web" || k == "macos" || k == "windows" || k == "linux" || k == "desktop" || k == "default";
}

// {"android": a, "ios": b, "default": c} → value for this platform ("desktop" covers windows/macos/linux).
json::Value resolve(const json::Value& v, const std::string& platform) {
    if (v.isObject() && !v.obj.empty()) {
        bool allPlatform = true;
        for (auto& [k, _] : v.obj) allPlatform = allPlatform && isPlatformKey(k);
        if (allPlatform) {
            if (const json::Value* p = v.find(platform)) return resolve(*p, platform);
            bool desktop = platform == "windows" || platform == "macos" || platform == "linux";
            if (const json::Value* d = desktop ? v.find("desktop") : nullptr) return resolve(*d, platform);
            if (const json::Value* d = v.find("default")) return resolve(*d, platform);
            return json::Value();
        }
        json::Value o = json::Value::object();
        for (auto& [k, item] : v.obj) o.obj.emplace_back(k, resolve(item, platform));
        return o;
    }
    if (v.isArray()) {
        json::Value a = json::Value::array();
        for (auto& item : v.arr) a.arr.push_back(resolve(item, platform));
        return a;
    }
    return v;
}

// Desktop mocks (mock-store / mock-ads) exist only where they were asked for: listed in game.json
// "plugins" or RTS_MOCK_PLUGINS=1 in the environment. Packaged builds compile them out entirely
// unless the game lists them (rtsbuild → plugins.cmake → RTS_MOCK_PLUGINS).
void syncMocks(const json::Value& config) {
#if RTS_WITH_MOCK_PLUGINS
    const char* env = std::getenv("RTS_MOCK_PLUGINS");
    const bool all = env && std::string(env) == "1";
    for (const char* name : {"mock-store", "mock-ads"})
        if (all || config.find(name)) services::registerMockPlugin(name);
#else
    (void)config;
#endif
}

struct Pending { std::string plugin; Callback cb; };
struct Delivery {
    enum Kind { Reply, Event } kind = Reply;
    uint64_t id = 0;     // Reply
    bool ok = false;
    std::string plugin, event, data;
};

} // namespace

// Process-wide bridge state (platform state, like platform::detail::State).
struct Bridge {
    std::mutex m;                                              // guards everything below
    std::map<std::string, std::shared_ptr<NativePlugin>> native;
    std::map<uint64_t, Pending> pending;
    std::vector<Delivery> queue;
    std::map<int, std::pair<std::string, EventHandler>> listeners;
    std::map<std::string, bool> initSent;                       // plugin → "init" already sent
    detail::Backend* backend = nullptr;
    json::Value config = json::Value::object();
    json::Value gameJson = json::Value::object();
    bool configured = false;
    uint64_t nextId = 1;
    int nextListener = 1;
    std::function<std::string()> configLoader;

    static Bridge& get() { static Bridge b; return b; }

    void deliver(Delivery d) { std::lock_guard<std::mutex> l(m); queue.push_back(std::move(d)); }

    void ensureConfig() {
        std::function<std::string()> loader;
        {
            std::lock_guard<std::mutex> l(m);
            if (configured) return;
            configured = true;
            loader = configLoader;
        }
        if (!loader) { syncMocks(json::Value::object()); return; }
        std::string text = loader();
        if (text.empty()) { syncMocks(json::Value::object()); return; }
        json::Value game;
        std::string err;
        if (!json::parse(json::stripComments(text), game, &err)) { std::fprintf(stderr, "[plugins] game.json: %s\n", err.c_str()); return; }
        json::Value cfg;
        {
            std::lock_guard<std::mutex> l(m);
            gameJson = game;
            if (const json::Value* p = game.find("plugins")) {
                if (p->isObject()) config = *p;
                else std::fprintf(stderr, "[plugins] game.json \"plugins\" must be an object\n");
            }
            cfg = config;
        }
        syncMocks(cfg);
    }

    // Starts a call on the plugin; "init" with the game.json config goes first, once per plugin.
    void dispatch(const std::string& plugin, const std::string& method, const std::string& args, uint64_t id) {
        std::shared_ptr<NativePlugin> cpp;
        detail::Backend* be = nullptr;
        bool needInit = false;
        std::string initArgs;
        {
            std::lock_guard<std::mutex> l(m);
            auto it = native.find(plugin);
            if (it != native.end()) cpp = it->second;
            else be = backend;
            if (method != "init" && !initSent[plugin]) {
                needInit = true;
                const json::Value* c = config.find(plugin);
                initArgs = c ? json::dump(resolve(*c, kPlatform)) : "{}";
            }
        }
        const bool reachable = cpp || (be && be->available(plugin));
        auto send = [&](const std::string& meth, const std::string& a, uint64_t callId) {
            if (cpp) { Reply r; r.id_ = callId; cpp->call(meth, a, r); }
            else if (reachable) be->call(plugin, meth, a, callId);
            else detail::completeCall(callId, false, "plugin '" + plugin + "' is not available on " + kPlatform);
        };
        if (needInit && reachable) {
            { std::lock_guard<std::mutex> l(m); needInit = !initSent[plugin]; initSent[plugin] = true; } // first caller wins
        }
        if (needInit && reachable) {
            uint64_t initId;
            { std::lock_guard<std::mutex> l(m); initId = nextId++; pending[initId] = {plugin, [plugin](const Result& r) {
                if (!r.ok) std::fprintf(stderr, "[plugins] %s init failed: %s\n", plugin.c_str(), r.error.c_str());
            }}; }
            send("init", initArgs, initId);
        }
        send(method, args, id);
    }
};

// ---------------------------------------------------------------- public API
bool available(const std::string& plugin) {
    Bridge& b = Bridge::get();
    b.ensureConfig();
    detail::Backend* be;
    {
        std::lock_guard<std::mutex> l(b.m);
        if (b.native.count(plugin)) return true;
        be = b.backend;
    }
    return be && be->available(plugin);
}

void call(const std::string& plugin, const std::string& method, const std::string& argsJson, Callback cb) {
    Bridge& b = Bridge::get();
    b.ensureConfig();
    uint64_t id;
    {
        std::lock_guard<std::mutex> l(b.m);
        id = b.nextId++;
        b.pending[id] = {plugin, std::move(cb)};
    }
    b.dispatch(plugin, method, argsJson.empty() ? "{}" : argsJson, id);
}

int addListener(const std::string& plugin, EventHandler handler) {
    Bridge& b = Bridge::get();
    std::lock_guard<std::mutex> l(b.m);
    int id = b.nextListener++;
    b.listeners[id] = {plugin, std::move(handler)};
    return id;
}
void removeListener(int id) {
    Bridge& b = Bridge::get();
    std::lock_guard<std::mutex> l(b.m);
    b.listeners.erase(id);
}

void configure(const std::string& pluginsObjectJson) {
    Bridge& b = Bridge::get();
    json::Value v = json::parseOr(pluginsObjectJson, json::Value::object());
    json::Value cfg = v.isObject() ? std::move(v) : json::Value::object();
    {
        std::lock_guard<std::mutex> l(b.m);
        b.config = cfg;
        b.configured = true;
        b.initSent.clear(); // new config → plugins get "init" again before their next call
    }
    syncMocks(cfg);
}

std::string gameSection(const std::string& key) {
    Bridge& b = Bridge::get();
    b.ensureConfig();
    std::lock_guard<std::mutex> l(b.m);
    const json::Value* v = b.gameJson.find(key);
    return v ? json::dump(resolve(*v, kPlatform)) : "null";
}

void configureGame(const std::string& gameJsonText) {
    Bridge& b = Bridge::get();
    json::Value game = json::parseOr(json::stripComments(gameJsonText), json::Value::object());
    const json::Value* p = game.find("plugins");
    json::Value cfg = p && p->isObject() ? *p : json::Value::object();
    {
        std::lock_guard<std::mutex> l(b.m);
        b.gameJson = game;
        b.config = cfg;
        b.configured = true;
        b.initSent.clear();
    }
    syncMocks(cfg);
}

std::string platformName() { return kPlatform; }

std::string configFor(const std::string& plugin) {
    Bridge& b = Bridge::get();
    b.ensureConfig();
    std::lock_guard<std::mutex> l(b.m);
    const json::Value* c = b.config.find(plugin);
    return c ? json::dump(resolve(*c, kPlatform)) : "{}";
}

std::vector<std::string> configured() {
    Bridge& b = Bridge::get();
    b.ensureConfig();
    std::lock_guard<std::mutex> l(b.m);
    std::vector<std::string> out;
    for (auto& [k, _] : b.config.obj) out.push_back(k);
    return out;
}

void pump() {
    Bridge& b = Bridge::get();
    std::vector<Delivery> q;
    { std::lock_guard<std::mutex> l(b.m); q.swap(b.queue); }
    for (auto& d : q) {
        if (d.kind == Delivery::Reply) {
            Callback cb;
            {
                std::lock_guard<std::mutex> l(b.m);
                auto it = b.pending.find(d.id);
                if (it == b.pending.end()) continue; // answered twice: first answer wins
                cb = std::move(it->second.cb);
                b.pending.erase(it);
            }
            if (!cb) continue;
            Result r;
            r.ok = d.ok;
            if (d.ok) r.data = d.data.empty() ? "null" : d.data;
            else r.error = d.data;
            cb(r);
        } else {
            std::vector<EventHandler> hs;
            {
                std::lock_guard<std::mutex> l(b.m);
                for (auto& [id, lh] : b.listeners) if (lh.first == d.plugin || lh.first == "*") hs.push_back(lh.second);
            }
            for (auto& h : hs) h(d.event, d.data.empty() ? "{}" : d.data);
        }
    }
}

// ---------------------------------------------------------------- C++ plugins
void Reply::ok(const std::string& dataJson) const { if (id_) detail::completeCall(id_, true, dataJson.empty() ? "null" : dataJson); }
void Reply::fail(const std::string& error) const { if (id_) detail::completeCall(id_, false, error); }

void registerPlugin(const std::string& name, std::shared_ptr<NativePlugin> plugin) {
    Bridge& b = Bridge::get();
    std::lock_guard<std::mutex> l(b.m);
    b.native[name] = std::move(plugin);
    b.initSent[name] = false;
}
void unregisterPlugin(const std::string& name) {
    Bridge& b = Bridge::get();
    std::lock_guard<std::mutex> l(b.m);
    b.native.erase(name);
}

void emit(const std::string& plugin, const std::string& event, const std::string& dataJson) {
    Delivery d;
    d.kind = Delivery::Event;
    d.plugin = plugin; d.event = event; d.data = dataJson;
    Bridge::get().deliver(std::move(d));
}

namespace detail {
void setBackend(Backend* backend) {
    Bridge& b = Bridge::get();
    std::lock_guard<std::mutex> l(b.m);
    b.backend = backend;
}
void setConfigLoader(std::function<std::string()> loader) {
    Bridge& b = Bridge::get();
    std::lock_guard<std::mutex> l(b.m);
    b.configLoader = std::move(loader);
}
void completeCall(uint64_t callId, bool ok, const std::string& dataOrError) {
    Delivery d;
    d.kind = Delivery::Reply;
    d.id = callId; d.ok = ok; d.data = dataOrError;
    Bridge::get().deliver(std::move(d));
}
std::string resolvePlatformValues(const std::string& text, const std::string& platformName) {
    return json::dump(resolve(json::parseOr(text, json::Value()), platformName));
}
void broadcast(const std::string& method, const std::string& argsJson) {
    for (const std::string& p : configured())
        if (available(p)) call(p, method, argsJson); // no callback: plugins without the method just fail quietly
}
} // namespace detail

} // namespace rts::plugins
