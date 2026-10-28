// In-app purchases, one API for every store: App Store, Google Play, RuStore, Yandex Games, and a
// mock store on desktop builds. The store is chosen per platform from game.json:
//
//   "store": {
//     "providers": { "android": ["googleplay"], "ios": ["appstore"], "web": ["yandex-games"] },
//     "products": [
//       { "id": "gems_100", "type": "consumable" },
//       { "id": "no_ads",   "type": "nonConsumable", "ids": { "ios": "com.studio.game.noads" } }
//     ]
//   }
//
// "providers" is optional (default order: appstore, googleplay, rustore, yandex-games, mock-store —
// the first one built into the app wins). "ids" maps a product to a store-specific id.
//
// Purchase flow: purchase() → grant the item → finish() (consumes / acknowledges it). Purchases
// that complete later or after a restart (pending payments, crash mid-purchase, "Ask to Buy") come
// through onPurchase(); call restore() at startup to get them and the owned non-consumables.
// Never grant twice: remember Purchase::token of what you have granted.
//
// Callbacks run on the game thread (plugins::pump). Plugin protocol "store": docs/plugins.md.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rts::store {

enum class ProductType : uint8_t { Consumable, NonConsumable, Subscription };

struct Product {
    std::string id;              // the game's id (from game.json)
    std::string title, description;
    std::string price;           // localized, ready to show: "99 ₽", "$0.99"
    int64_t priceMicros = 0;     // 0.99 → 990000
    std::string currency;        // ISO 4217: "RUB", "USD"; Yandex Games: "YAN"
    ProductType type = ProductType::Consumable;
};

enum class PurchaseState : uint8_t { Purchased, Pending, Cancelled, Failed };

struct Purchase {
    std::string productId;       // the game's id
    PurchaseState state = PurchaseState::Failed;
    std::string token;           // unique per purchase: use it to never grant twice
    std::string orderId;
    std::string error;           // Failed: readable reason
    std::string receipt;         // provider data (JSON) for server-side validation
    bool restored = false;       // came from restore() / a previous session
};

bool available();                // a store is built into this app for this platform
std::string provider();          // "googleplay", "appstore", "rustore", "yandex-games", "mock-store", ""

// Catalog with localized prices. ids empty = all products from game.json.
void products(std::function<void(bool ok, const std::vector<Product>& products, const std::string& error)> cb,
              const std::vector<std::string>& ids = {});
// Shows the store's payment sheet. The callback gets the final state (Pending: wait for onPurchase).
void purchase(const std::string& productId, std::function<void(const Purchase&)> cb);
// Owned non-consumables / active subscriptions and purchases that were never finished.
void restore(std::function<void(bool ok, const std::vector<Purchase>& purchases, const std::string& error)> cb);
// After the item was granted: consumes consumables (can be bought again), acknowledges the rest.
void finish(const Purchase& p, std::function<void(bool ok, const std::string& error)> cb = {});
// Purchases that finish outside purchase(): pending → purchased, completed after a restart.
void onPurchase(std::function<void(const Purchase&)> handler);

ProductType typeOf(const std::string& productId); // from game.json; Consumable if unknown

} // namespace rts::store
