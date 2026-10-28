// Yandex Mobile Ads SDK 8 → the engine's "ads" protocol (docs/plugins.md). API checked against the
// official samples (github.com/yandexmobile/yandex-ads-sdk-android, mobileads 8.6.0).
// Config (game.json "plugins" → "yandex-ads"): interstitial / rewarded / banner ad unit ids
// ("demo-interstitial-yandex", "demo-rewarded-yandex", "demo-banner-yandex" for testing).
package com.rtsengine.plugins;

import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;

import com.rtsengine.platform.RtsPlugin;
import com.yandex.mobile.ads.banner.BannerAdEventListener;
import com.yandex.mobile.ads.banner.BannerAdSize;
import com.yandex.mobile.ads.banner.BannerAdView;
import com.yandex.mobile.ads.common.AdError;
import com.yandex.mobile.ads.common.AdRequest;
import com.yandex.mobile.ads.common.AdRequestError;
import com.yandex.mobile.ads.common.ImpressionData;
import com.yandex.mobile.ads.common.YandexAds;
import com.yandex.mobile.ads.interstitial.InterstitialAd;
import com.yandex.mobile.ads.interstitial.InterstitialAdEventListener;
import com.yandex.mobile.ads.interstitial.InterstitialAdLoadListener;
import com.yandex.mobile.ads.interstitial.InterstitialAdLoader;
import com.yandex.mobile.ads.rewarded.Reward;
import com.yandex.mobile.ads.rewarded.RewardedAd;
import com.yandex.mobile.ads.rewarded.RewardedAdEventListener;
import com.yandex.mobile.ads.rewarded.RewardedAdLoadListener;
import com.yandex.mobile.ads.rewarded.RewardedAdLoader;

import org.json.JSONObject;

public class YandexAdsPlugin extends RtsPlugin {
    private JSONObject config = new JSONObject();
    private InterstitialAdLoader interstitialLoader;
    private RewardedAdLoader rewardedLoader;
    private InterstitialAd interstitial;
    private RewardedAd rewarded;
    private BannerAdView banner;

    @Override
    public void call(String method, JSONObject args, Reply reply) {
        switch (method) {
            case "init":
                config = args;
                YandexAds.initialize(activity, () -> { load("interstitial"); load("rewarded"); });
                reply.ok();
                return;
            case "consent":
                YandexAds.setUserConsent(args.optBoolean("personalized", false));
                YandexAds.setAgeRestricted(args.optBoolean("ageRestricted", false));
                reply.ok();
                return;
            case "load": load(args.optString("format")); reply.ok(); return;
            case "show": show(args.optString("format"), reply); return;
            case "banner": banner(args.optBoolean("show", true), "top".equals(args.optString("position")), reply); return;
            case "lifecycle": reply.ok(); return;
            default: reply.fail("yandex-ads: unknown method " + method);
        }
    }

    private String unit(String format) { return config.optString(format, ""); }

    private JSONObject ev(String format) {
        JSONObject o = new JSONObject();
        try { o.put("format", format); } catch (Exception ignored) {}
        return o;
    }

    private void load(String format) {
        String id = unit(format);
        if (id.isEmpty()) return;
        AdRequest req = new AdRequest.Builder(id).build();
        if (format.equals("interstitial")) {
            if (interstitialLoader == null) interstitialLoader = new InterstitialAdLoader(activity);
            interstitialLoader.loadAd(req, new InterstitialAdLoadListener() {
                @Override public void onAdLoaded(InterstitialAd ad) { interstitial = ad; emit("loaded", ev(format)); }
                @Override public void onAdFailedToLoad(AdRequestError e) { emit("failed", ev(format)); }
            });
        } else if (format.equals("rewarded")) {
            if (rewardedLoader == null) rewardedLoader = new RewardedAdLoader(activity);
            rewardedLoader.loadAd(req, new RewardedAdLoadListener() {
                @Override public void onAdLoaded(RewardedAd ad) { rewarded = ad; emit("loaded", ev(format)); }
                @Override public void onAdFailedToLoad(AdRequestError e) { emit("failed", ev(format)); }
            });
        }
    }

    private void show(String format, Reply reply) {
        if (format.equals("interstitial")) {
            InterstitialAd ad = interstitial;
            interstitial = null;
            if (ad == null) { load(format); reply.fail("not loaded"); return; }
            emit("app.pause", null);
            ad.setAdEventListener(new InterstitialAdEventListener() {
                @Override public void onAdShown() { emit("opened", ev(format)); }
                @Override public void onAdFailedToShow(AdError e) { done(reply, false, false, e.getDescription()); }
                @Override public void onAdDismissed() { emit("closed", ev(format)); done(reply, true, false, null); }
                @Override public void onAdClicked() {}
                @Override public void onAdImpression(ImpressionData d) {}
            });
            ad.show(activity);
        } else if (format.equals("rewarded")) {
            RewardedAd ad = rewarded;
            rewarded = null;
            if (ad == null) { load(format); reply.fail("not loaded"); return; }
            final boolean[] earned = {false};
            emit("app.pause", null);
            ad.setAdEventListener(new RewardedAdEventListener() {
                @Override public void onAdShown() { emit("opened", ev(format)); }
                @Override public void onAdFailedToShow(AdError e) { done(reply, false, false, e.getDescription()); }
                @Override public void onAdDismissed() { emit("closed", ev(format)); done(reply, true, earned[0], null); }
                @Override public void onRewarded(Reward r) { earned[0] = true; emit("rewarded", ev(format)); }
                @Override public void onAdClicked() {}
                @Override public void onAdImpression(ImpressionData d) {}
            });
            ad.show(activity);
        } else {
            reply.fail("yandex-ads: unknown format " + format);
        }
    }

    private void done(Reply reply, boolean shown, boolean rewardedOk, String error) {
        emit("app.resume", null);
        JSONObject r = new JSONObject();
        try { r.put("shown", shown).put("rewarded", rewardedOk); if (error != null) r.put("error", error); } catch (Exception ignored) {}
        reply.ok(r);
    }

    private void banner(boolean show, boolean top, Reply reply) {
        if (!show) {
            if (banner != null) { banner.destroy(); ((android.view.ViewGroup) banner.getParent()).removeView(banner); banner = null; }
            emitBanner(0, top);
            reply.ok();
            return;
        }
        String id = unit("banner");
        if (id.isEmpty()) { reply.fail("yandex-ads: no \"banner\" ad unit in game.json"); return; }
        if (banner == null) {
            float density = activity.getResources().getDisplayMetrics().density;
            int widthDp = Math.round(activity.getResources().getDisplayMetrics().widthPixels / density);
            BannerAdSize size = BannerAdSize.sticky(activity, widthDp);
            banner = new BannerAdView(activity);
            banner.setAdSize(size);
            banner.setBannerAdEventListener(new BannerAdEventListener() {
                @Override public void onAdLoaded() { emitBanner(size.getHeightInPixels(activity), top); emit("loaded", ev("banner")); }
                @Override public void onAdFailedToLoad(AdRequestError e) { emit("failed", ev("banner")); }
                @Override public void onAdClicked() {}
                @Override public void onImpression(ImpressionData d) {}
            });
            activity.addContentView(banner, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, (top ? Gravity.TOP : Gravity.BOTTOM) | Gravity.CENTER_HORIZONTAL));
            banner.loadAd(new AdRequest.Builder(id).build());
        }
        banner.setVisibility(View.VISIBLE);
        reply.ok();
    }

    /** The game keeps its UI out of the banner: height (logical points) goes into safeAreaInsets(). */
    private void emitBanner(int px, boolean top) {
        JSONObject o = new JSONObject();
        try { o.put("height", px / activity.getResources().getDisplayMetrics().density).put("position", top ? "top" : "bottom"); } catch (Exception ignored) {}
        emit("app.banner", o);
    }

    @Override public void onDestroy() { if (banner != null) banner.destroy(); }
}
