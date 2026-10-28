#include "Ads.h"

#include "engine/platform/PluginJson.h"
#include "engine/platform/Plugins.h"

#include <vector>

namespace rts::ads {
namespace {
namespace json = plugins::json;

const char* name(Format f) { return f == Format::Interstitial ? "interstitial" : f == Format::Rewarded ? "rewarded" : "banner"; }

struct State {
    bool loaded = false;
    std::string provider;
    bool enabled = true;
    bool ready[3] = {};
    int listener = 0;
};
State& S() { static State s; return s; }

void load() {
    State& s = S();
    if (s.loaded) return;
    s.loaded = true;
    json::Value cfg = json::parseOr(plugins::gameSection("ads"), json::Value::object());
    std::vector<std::string> order;
    const json::Value& prov = cfg["providers"];
    if (prov.isArray()) for (auto& v : prov.arr) order.push_back(v.str());
    else if (!prov.isNull()) order.push_back(prov.str());
    if (order.empty()) order = {"yandex-ads", "admob", "applovin", "ironsource", "yandex-games", "mock-ads"};
    for (auto& n : order) if (plugins::available(n)) { s.provider = n; break; }
    if (s.provider.empty()) return;
    s.listener = plugins::addListener(s.provider, [](const std::string& ev, const std::string& data) {
        json::Value v = json::parseOr(data);
        std::string f = v["format"].str();
        int i = f == "interstitial" ? 0 : f == "rewarded" ? 1 : f == "banner" ? 2 : -1;
        if (i < 0) return;
        if (ev == "loaded") S().ready[i] = true;
        else if (ev == "failed" || ev == "opened" || ev == "closed") S().ready[i] = false;
    });
}

json::Value formatArgs(Format f) {
    json::Value a = json::Value::object();
    a.set("format", json::Value::string(name(f)));
    return a;
}
} // namespace

bool available() { load(); return !S().provider.empty(); }
std::string provider() { load(); return S().provider; }
bool ready(Format f) { load(); return S().ready[int(f)]; }
bool enabled() { return S().enabled; }

void setEnabled(bool on) {
    S().enabled = on;
    if (!on) hideBanner();
}

void preload(Format f) {
    if (!available()) return;
    plugins::call(S().provider, "load", json::dump(formatArgs(f)));
}

void show(Format f, std::function<void(const ShowResult&)> cb, const std::string& placement) {
    load();
    ShowResult none;
    if (!available()) none.error = "no ad network on this platform";
    else if (f == Format::Banner) none.error = "use showBanner() for banners";
    else if (f == Format::Interstitial && !S().enabled) none.error = "ads disabled";
    if (!none.error.empty()) { if (cb) cb(none); return; }
    json::Value args = formatArgs(f);
    if (!placement.empty()) args.set("placement", json::Value::string(placement));
    S().ready[int(f)] = false;
    plugins::call(S().provider, "show", json::dump(args), [cb, f](const plugins::Result& r) {
        ShowResult res;
        if (r.ok) {
            json::Value v = json::parseOr(r.data);
            res.shown = v["shown"].boolean(true);
            res.rewarded = f == Format::Rewarded && v["rewarded"].boolean();
            res.error = v["error"].str();
        } else {
            res.error = r.error;
        }
        preload(f); // the next one loads while the game goes on
        if (cb) cb(res);
    });
}

void showBanner(bool top) {
    load();
    if (!available() || !S().enabled) return;
    json::Value a = formatArgs(Format::Banner);
    a.set("show", json::Value::boolean_(true)).set("position", json::Value::string(top ? "top" : "bottom"));
    plugins::call(S().provider, "banner", json::dump(a));
}

void hideBanner() {
    if (!available()) return;
    json::Value a = formatArgs(Format::Banner);
    a.set("show", json::Value::boolean_(false));
    plugins::call(S().provider, "banner", json::dump(a));
}

void setConsent(bool personalized, bool ageRestricted) {
    if (!available()) return;
    json::Value a = json::Value::object();
    a.set("personalized", json::Value::boolean_(personalized)).set("ageRestricted", json::Value::boolean_(ageRestricted));
    plugins::call(S().provider, "consent", json::dump(a));
}

} // namespace rts::ads
