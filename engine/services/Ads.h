// Ads, one API for every network: Yandex Mobile Ads, Google AdMob, Yandex Games (web), and mock ads
// on desktop builds. The network is chosen per platform from game.json:
//
//   "ads": { "providers": { "android": ["yandex-ads", "admob"], "ios": ["admob"], "web": ["yandex-games"] } }
//
// Ad unit ids live in the plugin's own config ("plugins" → "yandex-ads" → "rewarded": {...}).
// "providers" is optional (default order: yandex-ads, admob, yandex-games, mock-ads).
//
// Rules the stores and networks check: show interstitials only at natural pauses (between rounds,
// after "Next"), pause the game and mute audio while an ad is up (FocusLost arrives automatically
// when the provider reports it), grant rewards only when ShowResult::rewarded is true.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace rts::ads {

enum class Format : uint8_t { Interstitial, Rewarded, Banner };

struct ShowResult {
    bool shown = false;      // the ad was on screen
    bool rewarded = false;   // Rewarded: the user watched enough to earn the reward
    std::string error;       // why nothing was shown ("not loaded", "frequency limit", network error)
};

bool available();
std::string provider();          // "yandex-ads", "admob", "yandex-games", "mock-ads", ""

// Starts loading an ad so show() is instant later (providers also preload after init).
void preload(Format f);
// Last known state: a "loaded" event arrived and the ad was not shown yet.
bool ready(Format f);
// Shows an interstitial or rewarded ad; the callback runs when it is closed (or failed).
// `placement` names the spot for statistics ("level_end", "double_reward").
void show(Format f, std::function<void(const ShowResult&)> cb, const std::string& placement = "");
// Sticky banner at the bottom (or top) edge; its height arrives as safe-area-like info via events.
void showBanner(bool top = false);
void hideBanner();

// Interstitials and banners off (the player bought "no ads"); rewarded ads stay available.
void setEnabled(bool enabled);
bool enabled();
// Privacy: personalised ads allowed (GDPR consent), child-directed / age-restricted audience.
void setConsent(bool personalized, bool ageRestricted = false);

} // namespace rts::ads
