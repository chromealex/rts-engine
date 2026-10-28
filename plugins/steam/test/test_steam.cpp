// Purchase flow of the steam plugin against fake_steam.cpp: products, restore, purchase through the
// overlay, finish (consume), DLC, cancelled checkout. Built and run by run.sh.
#include "engine/platform/Plugins.h"
#include "engine/services/Store.h"
#include <cstdio>
#include <ctime>
#include <string>
using namespace rts;
extern "C" { void fake_overlay(bool); void fake_completePurchase(); void fake_buyDlc(); int fake_itemCount(); void fake_addItem(int); }
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static void frames(int n) { for (int i = 0; i < n; ++i) plugins::pump(); }
int main() {
    fake_addItem(100); // an unfinished purchase from an earlier session
    plugins::configureGame(R"({ "plugins": { "steam": { "appId": 480, "items": { "gems_100": 100, "no_ads": 200 }, "dlc": { "expansion": 777 } } },
      "store": { "products": [ {"id":"gems_100","type":"consumable"}, {"id":"no_ads","type":"nonConsumable"}, {"id":"expansion","type":"nonConsumable"}, {"id":"missing","type":"consumable"} ] } })");
    CHECK(store::available() && store::provider() == "steam");
    std::vector<store::Product> prods; bool pok = false;
    store::products([&](bool ok, const std::vector<store::Product>& v, const std::string& e) { pok = ok; prods = v; if (!ok) std::printf("products: %s\n", e.c_str()); });
    frames(5);
    CHECK(pok && prods.size() == 3);
    for (auto& p : prods) std::printf("  %s | %s | %s | %lld %s\n", p.id.c_str(), p.title.c_str(), p.price.c_str(), (long long)p.priceMicros, p.currency.c_str());
    std::vector<store::Purchase> restored;
    store::restore([&](bool ok, const std::vector<store::Purchase>& v, const std::string&) { CHECK(ok); restored = v; });
    frames(5);
    CHECK(restored.size() == 1 && restored[0].productId == "gems_100" && restored[0].token == "1000");
    // purchase: overlay opens, user pays, overlay closes → new instance found
    store::Purchase got; bool done = false;
    store::purchase("gems_100", [&](const store::Purchase& p) { got = p; done = true; });
    frames(3);
    std::vector<std::string> evs;
    plugins::addListener("steam", [&](const std::string& ev, const std::string& d) { evs.push_back(ev + d); });
    fake_overlay(true); frames(2);
    fake_completePurchase(); fake_overlay(false); frames(5);
    CHECK(done && got.state == store::PurchaseState::Purchased && got.productId == "gems_100" && got.token == "1001" && got.orderId == "555");
    std::printf("  purchase: %s token %s order %s error '%s'\n", got.productId.c_str(), got.token.c_str(), got.orderId.c_str(), got.error.c_str());
    CHECK(evs.size() >= 2 && evs[0].rfind("app.pause", 0) == 0);
    // finish consumes
    bool fin = false;
    store::finish(got, [&](bool ok, const std::string& e) { fin = ok; if (!ok) std::printf("finish: %s\n", e.c_str()); });
    frames(4);
    CHECK(fin && fake_itemCount() == 1);
    // DLC purchase through the overlay store page
    store::Purchase dlc; bool dd = false;
    store::purchase("expansion", [&](const store::Purchase& p) { dlc = p; dd = true; });
    frames(2); fake_buyDlc(); frames(3);
    CHECK(dd && dlc.state == store::PurchaseState::Purchased && dlc.token == "dlc-777");
    // cancelled checkout: overlay closes without a grant → "cancelled" after a few seconds
    store::Purchase c; bool cd = false;
    store::purchase("no_ads", [&](const store::Purchase& p) { c = p; cd = true; });
    frames(2); fake_overlay(true); frames(1); fake_overlay(false);
    for (int i = 0; i < 1000 && !cd; ++i) { frames(1); struct timespec t{0, 10000000}; nanosleep(&t, nullptr); }
    CHECK(cd && c.state != store::PurchaseState::Purchased);
    std::printf("  cancelled: state %d error '%s'\n", int(c.state), c.error.c_str());
    std::printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
