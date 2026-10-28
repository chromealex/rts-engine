// Desktop stand-ins for store and ad SDKs, so purchase and ad flows can be built and tested without
// a phone. The bridge registers them on Windows/macOS/Linux (and so in tests); never in mobile/web apps.
//
//   RTS_MOCK_STORE = purchased (default) | pending | cancelled | failed   — outcome of purchase()
//   RTS_MOCK_ADS   = ok (default) | noreward | fail                        — outcome of show()
#include "MockPlugins.h"

#include "engine/platform/PluginJson.h"
#include "engine/platform/Plugins.h"

#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

namespace rts::services {
namespace {
namespace json = plugins::json;

std::string env(const char* name, const char* def) {
    const char* v = std::getenv(name);
    return v && *v ? v : def;
}

class MockStore : public plugins::NativePlugin {
public:
    void call(const std::string& method, const std::string& args, plugins::Reply reply) override {
        json::Value a = json::parseOr(args);
        if (method == "init") { reply.ok(); return; }
        if (method == "products") {
            json::Value list = json::Value::array();
            int n = 0;
            for (auto& p : a["products"].arr) {
                const int64_t micros = 990000 + 1000000 * int64_t(n++); // 0.99, 1.99, ...
                json::Value o = json::Value::object();
                o.set("id", json::Value::string(p["id"].str()))
                 .set("title", json::Value::string("Mock " + p["id"].str()))
                 .set("description", json::Value::string("Test product (mock store)"))
                 .set("price", json::Value::string("$" + std::to_string(micros / 1000000) + "." + std::to_string(micros / 10000 % 100)))
                 .set("priceMicros", json::Value::number(micros))
                 .set("currency", json::Value::string("USD"))
                 .set("type", json::Value::string(p["type"].str("consumable")));
                list.push(std::move(o));
            }
            reply.ok(json::dump(list));
            return;
        }
        if (method == "purchase") {
            std::string outcome = env("RTS_MOCK_STORE", "purchased");
            if (outcome == "cancelled") { reply.fail("cancelled"); return; }
            if (outcome == "failed") { reply.fail("mock store: payment declined"); return; }
            json::Value p = json::Value::object();
            std::string token;
            { std::lock_guard<std::mutex> l(m_); token = "mock-" + std::to_string(++seq_); }
            p.set("productId", json::Value::string(a["id"].str()))
             .set("state", json::Value::string(outcome == "pending" ? "pending" : "purchased"))
             .set("token", json::Value::string(token))
             .set("orderId", json::Value::string("MOCK." + token));
            if (outcome != "pending") remember(a["id"].str(), a["type"].str(), token);
            reply.ok(json::dump(p));
            if (outcome == "pending") { // the "bank" confirms a moment later, like a real pending payment
                p.set("state", json::Value::string("purchased"));
                remember(a["id"].str(), a["type"].str(), token);
                plugins::emit("mock-store", "purchase", json::dump(p));
            }
            return;
        }
        if (method == "restore") {
            json::Value list = json::Value::array();
            std::lock_guard<std::mutex> l(m_);
            for (auto& [id, token] : owned_) {
                json::Value p = json::Value::object();
                p.set("productId", json::Value::string(id)).set("state", json::Value::string("restored")).set("token", json::Value::string(token));
                list.push(std::move(p));
            }
            reply.ok(json::dump(list));
            return;
        }
        if (method == "finish") {
            std::lock_guard<std::mutex> l(m_);
            if (a["consumable"].boolean(true)) owned_.erase({a["id"].str(), a["token"].str()});
            reply.ok();
            return;
        }
        reply.fail("mock-store: unknown method " + method);
    }
private:
    void remember(const std::string& id, const std::string& type, const std::string& token) {
        std::lock_guard<std::mutex> l(m_);
        (void)type; // unfinished consumables are restored too, like on real stores
        owned_.insert({id, token});
    }
    std::mutex m_;
    std::set<std::pair<std::string, std::string>> owned_;
    int seq_ = 0;
};

class MockAds : public plugins::NativePlugin {
public:
    void call(const std::string& method, const std::string& args, plugins::Reply reply) override {
        json::Value a = json::parseOr(args);
        std::string format = a["format"].str();
        json::Value ev = json::Value::object();
        ev.set("format", json::Value::string(format));
        if (method == "init" || method == "consent") { reply.ok(); return; }
        if (method == "load") { plugins::emit("mock-ads", "loaded", json::dump(ev)); reply.ok(); return; }
        if (method == "banner") { reply.ok(); return; }
        if (method == "show") {
            std::string outcome = env("RTS_MOCK_ADS", "ok");
            if (outcome == "fail") { reply.fail("mock ads: no fill"); return; }
            plugins::emit("mock-ads", "app.pause", "{}");
            plugins::emit("mock-ads", "opened", json::dump(ev));
            plugins::emit("mock-ads", "closed", json::dump(ev));
            plugins::emit("mock-ads", "app.resume", "{}");
            json::Value r = json::Value::object();
            r.set("shown", json::Value::boolean_(true)).set("rewarded", json::Value::boolean_(format == "rewarded" && outcome != "noreward"));
            reply.ok(json::dump(r));
            return;
        }
        reply.fail("mock-ads: unknown method " + method);
    }
};
} // namespace

void registerMockPlugins() {
    plugins::registerPlugin("mock-store", std::make_shared<MockStore>());
    plugins::registerPlugin("mock-ads", std::make_shared<MockAds>());
}

} // namespace rts::services
