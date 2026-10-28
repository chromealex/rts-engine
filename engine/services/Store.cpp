#include "Store.h"

#include "engine/platform/PluginJson.h"
#include "engine/platform/Plugins.h"

#include <map>

namespace rts::store {
namespace {
namespace json = plugins::json;

const char* typeName(ProductType t) { return t == ProductType::Consumable ? "consumable" : t == ProductType::NonConsumable ? "nonConsumable" : "subscription"; }
ProductType parseType(const std::string& s) { return s == "nonConsumable" || s == "non_consumable" ? ProductType::NonConsumable : s == "subscription" ? ProductType::Subscription : ProductType::Consumable; }

struct Def { std::string id, storeId; ProductType type = ProductType::Consumable; };

struct State {
    bool loaded = false;
    std::string provider;
    std::vector<Def> defs;
    std::function<void(const Purchase&)> handler;
    int listener = 0;
};
State& S() { static State s; return s; }

void load() {
    State& s = S();
    if (s.loaded) return;
    s.loaded = true;
    json::Value cfg = json::parseOr(plugins::gameSection("store"), json::Value::object());
    for (auto& p : cfg["products"].arr) {
        Def d;
        d.id = p["id"].str();
        if (d.id.empty()) continue;
        d.type = parseType(p["type"].str("consumable"));
        d.storeId = p["ids"].isNull() ? d.id : p["ids"].str(d.id); // ids already resolved per platform
        s.defs.push_back(d);
    }
    std::vector<std::string> order;
    const json::Value& prov = cfg["providers"];
    if (prov.isArray()) for (auto& v : prov.arr) order.push_back(v.str());
    else if (!prov.isNull()) order.push_back(prov.str());
    if (order.empty()) order = {"appstore", "googleplay", "rustore", "yandex-games", "mock-store"};
    for (auto& name : order) if (plugins::available(name)) { s.provider = name; break; }
}

const Def* byGameId(const std::string& id) { for (auto& d : S().defs) if (d.id == id) return &d; return nullptr; }
const Def* byStoreId(const std::string& id) { for (auto& d : S().defs) if (d.storeId == id) return &d; return nullptr; }
std::string gameId(const std::string& storeId) { const Def* d = byStoreId(storeId); return d ? d->id : storeId; }
std::string storeId(const std::string& gameId) { const Def* d = byGameId(gameId); return d ? d->storeId : gameId; }

PurchaseState parseState(const std::string& s) {
    if (s == "purchased" || s == "restored") return PurchaseState::Purchased;
    if (s == "pending") return PurchaseState::Pending;
    if (s == "cancelled" || s == "canceled") return PurchaseState::Cancelled;
    return PurchaseState::Failed;
}

Purchase toPurchase(const json::Value& v) {
    Purchase p;
    p.productId = gameId(v["productId"].str());
    p.state = parseState(v["state"].str("purchased"));
    p.token = v["token"].str();
    p.orderId = v["orderId"].str();
    p.error = v["error"].str();
    p.restored = v["state"].str() == "restored" || v["restored"].boolean();
    const json::Value& r = v["receipt"];
    p.receipt = r.isNull() ? "" : (r.type == json::Value::Type::String ? r.s : json::dump(r));
    return p;
}

void ensureListener() {
    State& s = S();
    if (s.listener || s.provider.empty()) return;
    s.listener = plugins::addListener(s.provider, [](const std::string& ev, const std::string& data) {
        if (ev != "purchase" || !S().handler) return;
        S().handler(toPurchase(json::parseOr(data)));
    });
}
} // namespace

bool available() { load(); return !S().provider.empty(); }
std::string provider() { load(); return S().provider; }
ProductType typeOf(const std::string& id) { load(); const Def* d = byGameId(id); return d ? d->type : ProductType::Consumable; }

void products(std::function<void(bool, const std::vector<Product>&, const std::string&)> cb, const std::vector<std::string>& ids) {
    load();
    if (!available()) { if (cb) cb(false, {}, "no store on this platform"); return; }
    json::Value args = json::Value::object();
    json::Value list = json::Value::array();
    if (ids.empty()) for (auto& d : S().defs) {
        json::Value p = json::Value::object();
        p.set("id", json::Value::string(d.storeId)).set("type", json::Value::string(typeName(d.type)));
        list.push(std::move(p));
    }
    else for (auto& id : ids) {
        json::Value p = json::Value::object();
        p.set("id", json::Value::string(storeId(id))).set("type", json::Value::string(typeName(typeOf(id))));
        list.push(std::move(p));
    }
    args.set("products", std::move(list));
    plugins::call(S().provider, "products", json::dump(args), [cb](const plugins::Result& r) {
        if (!cb) return;
        if (!r.ok) { cb(false, {}, r.error); return; }
        std::vector<Product> out;
        json::Value v = json::parseOr(r.data, json::Value::array());
        for (auto& p : v.arr) {
            Product pr;
            pr.id = gameId(p["id"].str());
            pr.title = p["title"].str();
            pr.description = p["description"].str();
            pr.price = p["price"].str();
            pr.priceMicros = p["priceMicros"].i64();
            pr.currency = p["currency"].str();
            pr.type = p["type"].isNull() ? typeOf(pr.id) : parseType(p["type"].str());
            out.push_back(pr);
        }
        cb(true, out, "");
    });
}

void purchase(const std::string& productId, std::function<void(const Purchase&)> cb) {
    load();
    ensureListener();
    if (!available()) { Purchase p; p.productId = productId; p.error = "no store on this platform"; if (cb) cb(p); return; }
    json::Value args = json::Value::object();
    args.set("id", json::Value::string(storeId(productId))).set("type", json::Value::string(typeName(typeOf(productId))));
    plugins::call(S().provider, "purchase", json::dump(args), [cb, productId](const plugins::Result& r) {
        if (!cb) return;
        Purchase p;
        if (r.ok) p = toPurchase(json::parseOr(r.data));
        else { p.state = r.error == "cancelled" ? PurchaseState::Cancelled : PurchaseState::Failed; p.error = r.error; }
        if (p.productId.empty()) p.productId = productId;
        cb(p);
    });
}

void restore(std::function<void(bool, const std::vector<Purchase>&, const std::string&)> cb) {
    load();
    ensureListener();
    if (!available()) { if (cb) cb(false, {}, "no store on this platform"); return; }
    plugins::call(S().provider, "restore", "{}", [cb](const plugins::Result& r) {
        if (!cb) return;
        if (!r.ok) { cb(false, {}, r.error); return; }
        std::vector<Purchase> out;
        for (auto& v : json::parseOr(r.data, json::Value::array()).arr) {
            Purchase p = toPurchase(v);
            p.restored = true;
            out.push_back(p);
        }
        cb(true, out, "");
    });
}

void finish(const Purchase& p, std::function<void(bool, const std::string&)> cb) {
    load();
    if (!available()) { if (cb) cb(false, "no store on this platform"); return; }
    json::Value args = json::Value::object();
    args.set("id", json::Value::string(storeId(p.productId)))
        .set("token", json::Value::string(p.token))
        .set("consumable", json::Value::boolean_(typeOf(p.productId) == ProductType::Consumable));
    plugins::call(S().provider, "finish", json::dump(args), [cb](const plugins::Result& r) { if (cb) cb(r.ok, r.error); });
}

void onPurchase(std::function<void(const Purchase&)> handler) {
    load();
    S().handler = std::move(handler);
    ensureListener();
}

} // namespace rts::store
