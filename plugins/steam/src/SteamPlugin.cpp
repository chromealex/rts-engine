// Steam (Steamworks SDK) → the engine's "store" protocol (docs/plugins.md), Windows / Linux / macOS.
//
// In-game purchases go through the Steam Inventory Service, the client-only way to sell on Steam
// (MicroTxn needs your own server and a publisher Web API key):
//   products  ISteamInventory::RequestPrices → GetItemPrice / item definition "name", "description"
//   purchase  StartPurchase → checkout in the Steam overlay → Steam grants a new item instance,
//             found by re-reading the inventory when the overlay closes
//   restore   GetAllItems: every item instance (consumables stay in the inventory until finish()
//             consumes them, so unfinished purchases come back too)
//   finish    ConsumeItem(instance, 1) for consumables; nothing for non-consumables
// DLC can be sold as non-consumables: the overlay store page, BIsDlcInstalled for ownership.
//
// Purchasable consumables must not use "auto_stack" in the item definitions: each purchase must be
// its own item instance, whose id is the purchase token.
#include "engine/platform/PluginJson.h"
#include "engine/platform/Plugins.h"

#include <steam/steam_api.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

namespace json = rts::plugins::json;
using rts::plugins::Reply;
using Clock = std::chrono::steady_clock;

constexpr const char* kName = "steam";

std::string u64(uint64 v) { return std::to_string(static_cast<unsigned long long>(v)); }

std::string formatPrice(uint64 minor, const std::string& currency) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%llu.%02llu %s", static_cast<unsigned long long>(minor / 100),
                  static_cast<unsigned long long>(minor % 100), currency.c_str());
    return buf;
}

// Everything that needs a running Steam client: created after SteamAPI_Init succeeded, so the
// STEAM_CALLBACK members register with an initialized API.
class Session {
public:
    explicit Session(json::Value config) : config_(std::move(config)) {
        SteamInventory()->LoadItemDefinitions();
        refresh(); // learn the items we already own, so purchases show up as new instances
    }

    // ---------------------------------------------------------------- product mapping
    // game.json: "items": {"gems_100": 100}, "dlc": {"expansion": 1234560}; a numeric product id
    // is an item definition id as is.
    SteamItemDef_t itemDef(const std::string& product) const {
        if (const json::Value* v = config_["items"].find(product)) return static_cast<SteamItemDef_t>(v->i64());
        char* end = nullptr;
        long n = std::strtol(product.c_str(), &end, 10);
        return !product.empty() && end && !*end ? static_cast<SteamItemDef_t>(n) : 0;
    }
    AppId_t dlc(const std::string& product) const {
        const json::Value* v = config_["dlc"].find(product);
        return v ? static_cast<AppId_t>(v->i64()) : 0;
    }
    std::string productOf(SteamItemDef_t def) const {
        for (auto& [k, v] : config_["items"].obj) if (v.i64() == def) return k;
        return std::to_string(def);
    }

    // ---------------------------------------------------------------- protocol
    void products(const json::Value& args, Reply reply) {
        productsReq_.push_back({args, reply});
        if (productsReq_.size() > 1) return; // one RequestPrices in flight answers everyone
        pricesCall_.Set(SteamInventory()->RequestPrices(), this, &Session::onPrices);
    }

    void purchase(const json::Value& args, Reply reply) {
        std::string id = args["id"].str();
        if (purchase_.reply) { reply.fail("steam: another purchase is in progress"); return; }
        if (AppId_t app = dlc(id)) {
            if (SteamApps()->BIsDlcInstalled(app)) { reply.ok(json::dump(purchaseJson(id, "dlc-" + u64(app), "", "purchased"))); return; }
            begin(id, reply);
            purchase_.dlc = app;
            SteamFriends()->ActivateGameOverlayToStore(app, k_EOverlayToStoreFlag_AddToCartAndShow);
            return;
        }
        SteamItemDef_t def = itemDef(id);
        if (!def) { reply.fail("steam: no item definition for \"" + id + "\" (game.json plugins → steam → items)"); return; }
        begin(id, reply);
        purchase_.def = def;
        uint32 qty = 1;
        purchaseCall_.Set(SteamInventory()->StartPurchase(&def, &qty, 1), this, &Session::onStartPurchase);
    }

    void restore(Reply reply) {
        restoreReplies_.push_back(reply);
        refresh();
    }

    void finish(const json::Value& args, Reply reply) {
        std::string token = args["token"].str();
        if (args["consumable"].boolean(true) && token.rfind("dlc-", 0) != 0) {
            SteamItemInstanceID_t item = std::strtoull(token.c_str(), nullptr, 10);
            SteamInventoryResult_t h = k_SteamInventoryResultInvalid;
            if (!item || !SteamInventory()->ConsumeItem(&h, item, 1)) { reply.fail("steam: cannot consume item " + token); return; }
            consuming_[h] = reply;
            known_.erase(item);
            return;
        }
        reply.ok();
    }

    // ---------------------------------------------------------------- per frame
    void update() {
        SteamAPI_RunCallbacks();
        if (purchase_.reply && purchase_.overlayClosedAt != Clock::time_point{}) {
            auto waited = Clock::now() - purchase_.overlayClosedAt;
            // the grant can lag the overlay a little: look again, then give up as cancelled
            if (waited > std::chrono::seconds(2 * (purchase_.checks + 1)) && purchase_.checks < 3) { ++purchase_.checks; refresh(); }
            else if (waited > std::chrono::seconds(8)) {
                Reply r = purchase_.reply;
                watchLate_.insert(purchase_.product);
                purchase_ = {};
                r.fail("cancelled");
            }
        }
    }

private:
    struct Purchase {
        Reply reply;
        std::string product;
        SteamItemDef_t def = 0;
        AppId_t dlc = 0;
        uint64 orderId = 0;
        Clock::time_point overlayClosedAt{};
        int checks = 0;
    };

    void begin(const std::string& id, Reply reply) {
        purchase_ = {};
        purchase_.reply = reply;
        purchase_.product = id;
    }

    json::Value purchaseJson(const std::string& product, const std::string& token, const std::string& orderId, const char* state) const {
        json::Value receipt = json::Value::object();
        receipt.set("steamId", json::Value::string(u64(SteamUser()->GetSteamID().ConvertToUint64())))
               .set("appId", json::Value::number(int64_t(SteamUtils()->GetAppID())));
        if (token.rfind("dlc-", 0) != 0) receipt.set("itemId", json::Value::string(token));
        json::Value p = json::Value::object();
        p.set("productId", json::Value::string(product)).set("state", json::Value::string(state))
         .set("token", json::Value::string(token)).set("orderId", json::Value::string(orderId))
         .set("receipt", std::move(receipt));
        return p;
    }

    void refresh() {
        SteamInventoryResult_t h = k_SteamInventoryResultInvalid;
        if (SteamInventory()->GetAllItems(&h)) fullReads_.insert(h);
    }

    void onPrices(SteamInventoryRequestPricesResult_t* r, bool ioFailure) {
        std::vector<std::pair<json::Value, Reply>> reqs;
        reqs.swap(productsReq_);
        if (ioFailure || r->m_result != k_EResultOK) {
            for (auto& [a, reply] : reqs) reply.fail("steam: RequestPrices failed (" + std::to_string(ioFailure ? -1 : int(r->m_result)) + ")");
            return;
        }
        std::string currency(r->m_rgchCurrency, strnlen(r->m_rgchCurrency, sizeof r->m_rgchCurrency));
        for (auto& [args, reply] : reqs) {
            json::Value list = json::Value::array();
            for (auto& want : args["products"].arr) {
                std::string id = want["id"].str();
                json::Value p = json::Value::object();
                p.set("id", json::Value::string(id)).set("type", json::Value::string(want["type"].str()));
                if (AppId_t app = dlc(id)) {
                    p.set("title", json::Value::string(dlcName(app))).set("description", json::Value::string(""))
                     .set("price", json::Value::string("")).set("priceMicros", json::Value::number(int64_t(0)))
                     .set("currency", json::Value::string(currency));
                    list.push(std::move(p));
                    continue;
                }
                SteamItemDef_t def = itemDef(id);
                uint64 price = 0, base = 0;
                if (!def || !SteamInventory()->GetItemPrice(def, &price, &base)) continue; // not for sale
                p.set("title", json::Value::string(defProperty(def, "name")))
                 .set("description", json::Value::string(defProperty(def, "description")))
                 .set("price", json::Value::string(formatPrice(price, currency)))
                 .set("priceMicros", json::Value::number(int64_t(price) * 10000))
                 .set("currency", json::Value::string(currency));
                list.push(std::move(p));
            }
            reply.ok(json::dump(list));
        }
    }

    void onStartPurchase(SteamInventoryStartPurchaseResult_t* r, bool ioFailure) {
        if (!purchase_.reply) return;
        if (ioFailure || r->m_result != k_EResultOK) {
            Reply reply = purchase_.reply;
            purchase_ = {};
            reply.fail("steam: StartPurchase failed (" + std::to_string(ioFailure ? -1 : int(r->m_result)) + ")");
            return;
        }
        purchase_.orderId = r->m_ulOrderID; // checkout is now open in the overlay
    }

    std::string defProperty(SteamItemDef_t def, const char* name) const {
        char buf[1024];
        uint32 size = sizeof buf;
        return SteamInventory()->GetItemDefinitionProperty(def, name, buf, &size) ? std::string(buf) : std::string();
    }
    std::string dlcName(AppId_t app) const {
        for (int i = 0, n = SteamApps()->GetDLCCount(); i < n; ++i) {
            AppId_t id = 0; bool avail = false; char name[256] = {};
            if (SteamApps()->BGetDLCDataByIndex(i, &id, &avail, name, sizeof name) && id == app) return name;
        }
        return {};
    }

    void emitPurchase(const json::Value& p) { rts::plugins::emit(kName, "purchase", json::dump(p)); }

    STEAM_CALLBACK(Session, onResult, SteamInventoryResultReady_t) {
        SteamInventoryResult_t h = pParam->m_handle;
        if (auto c = consuming_.find(h); c != consuming_.end()) {
            Reply reply = c->second;
            consuming_.erase(c);
            if (pParam->m_result == k_EResultOK) reply.ok();
            else reply.fail("steam: ConsumeItem failed (" + std::to_string(int(pParam->m_result)) + ")");
            SteamInventory()->DestroyResult(h);
            return;
        }
        if (!fullReads_.erase(h)) return; // somebody else's result
        std::vector<SteamItemDetails_t> items;
        uint32 count = 0;
        bool ok = pParam->m_result == k_EResultOK && SteamInventory()->GetResultItems(h, nullptr, &count);
        if (ok && count) {
            items.resize(count);
            ok = SteamInventory()->GetResultItems(h, items.data(), &count);
        }
        SteamInventory()->DestroyResult(h);
        std::vector<Reply> restores;
        restores.swap(restoreReplies_);
        if (!ok) {
            for (auto& r : restores) r.fail("steam: GetAllItems failed (" + std::to_string(int(pParam->m_result)) + ")");
            return;
        }
        json::Value owned = json::Value::array();
        std::set<SteamItemInstanceID_t> now;
        for (auto& it : items) {
            if (it.m_unFlags & (k_ESteamItemRemoved | k_ESteamItemConsumed)) continue;
            now.insert(it.m_itemId);
            // every instance is reported (drops and promo items too: the game ignores unknown ids)
            std::string product = productOf(it.m_iDefinition);
            owned.push(purchaseJson(product, u64(it.m_itemId), "", "restored"));
            if (!initialized_ || known_.count(it.m_itemId)) continue;
            // a new instance: the pending purchase, or one that completed late
            if (purchase_.reply && it.m_iDefinition == purchase_.def) {
                Reply reply = purchase_.reply;
                std::string order = purchase_.orderId ? u64(purchase_.orderId) : "";
                purchase_ = {};
                reply.ok(json::dump(purchaseJson(product, u64(it.m_itemId), order, "purchased")));
            } else if (watchLate_.count(product)) {
                emitPurchase(purchaseJson(product, u64(it.m_itemId), "", "purchased"));
            }
        }
        for (auto& [id, app] : config_["dlc"].obj)
            if (SteamApps()->BIsDlcInstalled(AppId_t(app.i64()))) owned.push(purchaseJson(id, "dlc-" + app.str(), "", "restored"));
        known_ = std::move(now);
        initialized_ = true;
        for (auto& r : restores) r.ok(json::dump(owned));
    }

    STEAM_CALLBACK(Session, onOverlay, GameOverlayActivated_t) {
        // the overlay covers the game: pause the match and mute, like an ad or a store dialog
        rts::plugins::emit(kName, pParam->m_bActive ? "app.pause" : "app.resume", "null");
        if (!pParam->m_bActive && purchase_.reply) {
            purchase_.overlayClosedAt = Clock::now();
            purchase_.checks = 0;
            refresh();
        }
    }

    STEAM_CALLBACK(Session, onDlc, DlcInstalled_t) {
        for (auto& [id, app] : config_["dlc"].obj) {
            if (AppId_t(app.i64()) != pParam->m_nAppID) continue;
            json::Value p = purchaseJson(id, "dlc-" + app.str(), "", "purchased");
            if (purchase_.reply && purchase_.dlc == pParam->m_nAppID) {
                Reply reply = purchase_.reply;
                purchase_ = {};
                reply.ok(json::dump(p));
            } else {
                emitPurchase(p);
            }
        }
    }

    json::Value config_;
    CCallResult<Session, SteamInventoryRequestPricesResult_t> pricesCall_;
    CCallResult<Session, SteamInventoryStartPurchaseResult_t> purchaseCall_;
    std::vector<std::pair<json::Value, Reply>> productsReq_;
    std::vector<Reply> restoreReplies_;
    std::set<SteamInventoryResult_t> fullReads_;
    std::map<SteamInventoryResult_t, Reply> consuming_;
    std::set<SteamItemInstanceID_t> known_;
    std::set<std::string> watchLate_; // products whose purchase reply already went out as "cancelled"
    bool initialized_ = false;
    Purchase purchase_;
};

class SteamPlugin final : public rts::plugins::NativePlugin {
public:
    ~SteamPlugin() override {
        if (session_) { session_.reset(); SteamAPI_Shutdown(); }
    }

    void call(const std::string& method, const std::string& argsJson, Reply reply) override {
        json::Value args = json::parseOr(argsJson);
        if (method == "init") { init(args); reply.ok(); return; }
        if (method == "lifecycle") { reply.ok(); return; }
        if (!session_) { reply.fail("steam: " + (error_.empty() ? std::string("not initialized") : error_)); return; }
        if (method == "products") session_->products(args, reply);
        else if (method == "purchase") session_->purchase(args, reply);
        else if (method == "restore") session_->restore(reply);
        else if (method == "finish") session_->finish(args, reply);
        else reply.fail("steam: unknown method " + method);
    }

    void update() override {
        if (session_) session_->update();
    }

private:
    void init(const json::Value& config) {
        if (session_) return;
        AppId_t app = static_cast<AppId_t>(config["appId"].i64());
        // Release builds started outside Steam relaunch through it (and this process must quit).
        if (app && config["restartIfNecessary"].boolean(false) && SteamAPI_RestartAppIfNecessary(app)) {
            rts::plugins::emit(kName, "app.log", json::quote("steam: relaunching through the Steam client"));
            std::exit(0);
        }
        SteamErrMsg err = {};
        if (SteamAPI_InitEx(&err) != k_ESteamAPIInitResult_OK) {
            error_ = std::string("SteamAPI_Init failed: ") + err +
                     " (is Steam running? for development put steam_appid.txt with the app id next to the executable)";
            rts::plugins::emit(kName, "app.log", json::quote("steam: " + error_));
            return;
        }
        session_ = std::make_unique<Session>(config);
    }

    std::unique_ptr<Session> session_;
    std::string error_;
};

} // namespace

RTS_PLUGIN("steam", SteamPlugin);
