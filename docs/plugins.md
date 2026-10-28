# Native plugins: payments, ads, any platform SDK

Platform SDKs (stores, ad networks, analytics, ...) plug into the engine through one asynchronous
bridge, `engine/platform/Plugins.h`. A plugin is a name plus methods, written in the platform's own
language:

| Platform | Language | Base / registration |
|---|---|---|
| Android | Java (calling Java/Kotlin SDKs) | `extends com.rtsengine.platform.RtsPlugin`, manifest `<meta-data android:name="rts.plugin.NAME">` |
| iOS, macOS | Objective-C++ (`.mm`) | `<RTSPlugin>` (`engine/platform/apple/RTSPlugin.h`), Info.plist `RTSPlugins` |
| Web | JavaScript | `RTSPlugins.register(name, { method(args) { ... } })` |
| any (incl. desktop) | C++ | `plugins::NativePlugin` + `RTS_PLUGIN("name", Class)` |

rtsbuild writes the registration entries from `plugin.json`; plugin code never touches the engine.

Games normally don't call plugins directly: they use the standard services in `engine/services`,
which pick the provider for the current platform from `game.json`:

- `rts::store` (`Store.h`) — products with localized prices, purchase, restore, finish.
- `rts::ads` (`Ads.h`) — interstitial, rewarded, sticky banner, consent, "no ads" switch.

Desktop builds get `mock-store` and `mock-ads` (C++, `engine/services/MockPlugins.cpp`) so the whole
purchase and ad flow can be built and tested without a phone: `RTS_MOCK_STORE=purchased|pending|
cancelled|failed`, `RTS_MOCK_ADS=ok|noreward|fail`.

## Ready plugins (`plugins/`)

| Plugin | Platforms | Protocol | SDK |
|---|---|---|---|
| `appstore` | iOS, macOS | store | StoreKit (original API: deprecated since iOS 18 but working; StoreKit 2 is Swift-only) |
| `googleplay` | Android | store | Google Play Billing 9.1 |
| `rustore` | Android | store | RuStore Pay SDK 11 (BOM 2026.10.01). The old `billingclient` stopped working on 1 Aug 2026 |
| `yandex-games` | Web | store + ads | Yandex Games SDK (`/sdk.js`) |
| `yandex-ads` | Android, iOS | ads | Yandex Mobile Ads 8.6 |
| `admob` | Android, iOS | ads | Google Mobile Ads (Android 25.5, iOS 13.11) |

## game.json

```jsonc
"plugins": {                                   // which plugins go into the build, and their config
  "googleplay": {},
  "rustore":    { "consoleAppId": "2063512345", "scheme": "ironandgold" },
  "appstore":   {},
  "admob":      { "appId":    { "android": "ca-app-pub-...~...", "ios": "ca-app-pub-...~..." },
                  "rewarded": { "android": "ca-app-pub-.../...", "ios": "ca-app-pub-.../..." } },
  "yandex-ads": { "interstitial": "R-M-123456-1", "rewarded": "R-M-123456-2", "banner": "R-M-123456-3" },
  "yandex-games": { "signed": false }
},
"store": {
  "providers": { "android": ["rustore", "googleplay"], "ios": ["appstore"], "web": ["yandex-games"], "desktop": ["mock-store"] },
  "products": [ { "id": "gems_100", "type": "consumable" },
                { "id": "no_ads", "type": "nonConsumable", "ids": { "ios": "com.studio.game.noads" } } ]
},
"ads": { "providers": { "android": ["yandex-ads", "admob"], "ios": ["admob"], "web": ["yandex-games"] } }
```

- Any value may be per platform: `{"android": …, "ios": …, "web": …, "desktop": …, "default": …}`
  is replaced by the current platform's value everywhere (plugin config, store, ads).
- `providers`: the first one built into the app wins. Without it: store = appstore, googleplay,
  rustore, yandex-games, mock-store; ads = yandex-ads, admob, yandex-games, mock-ads.
- **Store policies**: an app published on Google Play must not contain another payment system, and a
  RuStore build should not need Google Play. Build them separately:
  `rtsbuild --target android --plugins googleplay,admob` and `--plugins rustore,yandex-ads`
  (`--plugins` replaces the game.json list for that build).

Test ids: Yandex `demo-interstitial-yandex`, `demo-rewarded-yandex`, `demo-banner-yandex`;
Google — https://developers.google.com/admob/android/test-ads (Android) and /ios/test-ads.

## Using the services (C++)

```cpp
#include "engine/services/Store.h"
#include "engine/services/Ads.h"

// at startup: grant what was paid but not granted (crash mid-purchase, pending payments)
store::onPurchase([](const store::Purchase& p) { if (p.state == store::PurchaseState::Purchased) grant(p); });
store::restore([](bool ok, const std::vector<store::Purchase>& list, const std::string&) { for (auto& p : list) grant(p); });

store::products([](bool ok, const std::vector<store::Product>& items, const std::string& err) { /* show prices */ });
store::purchase("gems_100", [](const store::Purchase& p) {
    if (p.state == store::PurchaseState::Purchased) grant(p);   // Pending: wait for onPurchase
});
void grant(const store::Purchase& p) {
    if (alreadyGranted(p.token)) return;                         // purchases may arrive twice
    giveItem(p.productId); saveGranted(p.token);
    store::finish(p);                                            // consume / acknowledge only after granting
}

ads::show(ads::Format::Rewarded, [](const ads::ShowResult& r) { if (r.rewarded) doubleTheLoot(); }, "level_end");
ads::showBanner();                    // its height is added to platform::safeAreaInsets()
ads::setEnabled(!ownsNoAds);          // "no ads" purchase: interstitials and banners off, rewarded stays
```

All callbacks run on the game thread. While an ad or a payment sheet covers the game the provider
sends `app.pause`/`app.resume`, which arrive as `FocusLost`/`FocusGained` — the same events as
switching apps, so the game pauses and mutes in one place.

## Protocols

All messages are JSON. Errors are failed replies; `"cancelled"` means the user backed out.

**store** (`appstore`, `googleplay`, `rustore`, `yandex-games`, `mock-store`)

| Method | Args | Reply |
|---|---|---|
| `init` | the plugin's game.json config | `{}` |
| `products` | `{"products": [{"id", "type"}]}` | `[{"id", "title", "description", "price", "priceMicros", "currency", "type"}]` |
| `purchase` | `{"id", "type"}` | purchase |
| `restore` | `{}` | `[purchase]`: owned non-consumables / subscriptions + never-finished purchases |
| `finish` | `{"id", "token", "consumable"}` | `{}`: consume (consumable) or acknowledge |
| event `purchase` | purchase | a purchase finished outside `purchase()` |

purchase = `{"productId", "state": "purchased|pending|restored", "token", "orderId", "receipt": {...}}`;
`token` is unique per purchase; `receipt` carries provider data for server-side validation
(Google `originalJson` + `signature`, Apple `appStoreReceipt`, RuStore `invoiceId`, Yandex `signature`).

**ads** (`yandex-ads`, `admob`, `yandex-games`, `mock-ads`)

| Method | Args | Reply |
|---|---|---|
| `init` | config | `{}` (starts preloading) |
| `load` | `{"format": "interstitial|rewarded"}` | `{}` |
| `show` | `{"format", "placement"}` | `{"shown", "rewarded", "error"?}` when the ad is closed |
| `banner` | `{"show", "position": "top|bottom"}` | `{}` |
| `consent` | `{"personalized", "ageRestricted"}` | `{}` |
| events | `loaded`, `failed`, `opened`, `closed`, `rewarded` with `{"format"}` | |

**app events** any plugin may send: `app.pause`, `app.resume`, `app.banner` `{"height", "position"}`
(logical points, added to the safe area), `app.log` (text to the engine log). The engine calls
`lifecycle` `{"event": "ready"}` on every configured plugin once the first screen is interactive
(Yandex Games moderation requires `LoadingAPI.ready()` then).

## Writing a plugin

Folder `plugins/<name>/` (engine-wide) or `games/<game>/plugins/<name>/` (one game), with
`plugin.json` and the sources:

```jsonc
{
  "name": "my-sdk", "provides": ["ads"],
  "cpp":     { "sources": ["src/*.cpp"] },                                    // every platform
  "android": { "java": "android/java",                                        // copied to app/src/main/java
               "classes": { "my-sdk": "com.studio.plugins.MySdkPlugin" },
               "dependencies": ["com.example:sdk:1.2.3"], "platformDependencies": ["com.example:bom:1.0"],
               "repositories": ["https://maven.example.com"],
               "manifest": { "root": ["<uses-permission .../>"], "application": ["<meta-data ... android:value=\"${appKey}\"/>"],
                             "activity": ["<intent-filter>...</intent-filter>"] } },
  "ios":     { "sources": ["ios/*.mm"], "classes": { "my-sdk": "RTSMySdkPlugin" },
               "pods": ["pod 'MySdk', '~> 2.0'"], "frameworks": ["StoreKit"],
               "plist": ["<key>MySdkKey</key><string>${appKey}</string>"] },
  "macos":   { same keys as ios },
  "web":     { "scripts": ["https://cdn.example.com/sdk.js"], "js": ["web/my-sdk.js"] }
}
```

`${key}` / `${a.b}` in manifest, plist, pod and script strings are filled from the plugin's
game.json config for that platform (rtsbuild warns about missing keys). rtsbuild then:

- Android — Java sources, Gradle dependencies/repositories, manifest entries (incl. the
  `rts.plugin.*` registry); `RtsActivity` forwards `onResume/onPause/onDestroy/onNewIntent/
  onActivityResult` to plugins (the activity is `singleTask`, payment deep links come back through
  `onNewIntent`).
- iOS — sources and frameworks via `plugins.cmake`, Info.plist keys, a Podfile for `rts_client`
  and `pod install` (open `RTSEngine.xcworkspace` afterwards).
- Web — copies the scripts and inserts `<script>` tags at `<meta name="rts-plugin-scripts">` in the
  shell, plus `RTSPlugins.config[name]`.
- C++ sources are compiled into the game executable itself (not a static library), so
  `RTS_PLUGIN(...)` self-registration works.

Rules for plugin code: answer every call exactly once (the reply objects ignore a second answer);
call SDKs on their required thread (Java plugins run on the UI thread, Objective-C on the main
thread); never block; send `app.pause`/`app.resume` around anything that covers the game.

## Verified / not verified

- ✅ Bridge, services, mocks: unit tests (`tests/tests.cpp`), Linux and Windows (MinGW/Wine).
- ✅ Web bridge in Chromium (calls, config, events, errors, `LoadingAPI.ready`); `yandex-games`
  protocol test against a fake SDK (`node plugins/yandex-games/web/test.mjs`).
- ✅ rtsbuild output: Android manifest/Gradle/Java, web script tags.
- ✅ Java plugins compile (javac against API stubs written from the SDKs' bytecode / official samples).
- ⏳ Real SDKs: Gradle/CocoaPods builds and device runs — CI job `plugins`; Objective-C plugins not
  compiled here (no Xcode). `yandex-ads` iOS: some Objective-C selectors are derived from the Swift
  API (marked in the source).
