// AppLovin MAX SDK 13 → the engine's "ads" protocol (docs/plugins.md). API checked against the
// official demo (github.com/AppLovin/AppLovin-MAX-SDK-Android) and AppLovin's React Native/Flutter
// bridges. The SDK key is passed in code (no manifest entry since SDK 12).
package com.rtsengine.plugins;

import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;

import com.applovin.mediation.MaxAd;
import com.applovin.mediation.MaxAdFormat;
import com.applovin.mediation.MaxAdListener;
import com.applovin.mediation.MaxAdViewAdListener;
import com.applovin.mediation.MaxError;
import com.applovin.mediation.MaxReward;
import com.applovin.mediation.MaxRewardedAdListener;
import com.applovin.mediation.ads.MaxAdView;
import com.applovin.mediation.ads.MaxInterstitialAd;
import com.applovin.mediation.ads.MaxRewardedAd;
import com.applovin.sdk.AppLovinMediationProvider;
import com.applovin.sdk.AppLovinPrivacySettings;
import com.applovin.sdk.AppLovinSdk;
import com.applovin.sdk.AppLovinSdkInitializationConfiguration;
import com.rtsengine.platform.RtsPlugin;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

public class AppLovinMaxPlugin extends RtsPlugin {
    private JSONObject config = new JSONObject();
    private boolean ready;
    private MaxInterstitialAd interstitial;
    private MaxRewardedAd rewarded;
    private MaxAdView banner;
    private Reply showReply;
    private String showFormat;
    private boolean earned;

    @Override
    public void call(String method, JSONObject args, Reply reply) {
        switch (method) {
            case "init": init(args); reply.ok(); return;
            case "consent":
                AppLovinPrivacySettings.setHasUserConsent(args.optBoolean("personalized", false), activity);
                reply.ok();
                return;
            case "load": load(args.optString("format")); reply.ok(); return;
            case "show": show(args.optString("format"), args.optString("placement", ""), reply); return;
            case "banner": banner(args.optBoolean("show", true), "top".equals(args.optString("position")), reply); return;
            case "lifecycle": reply.ok(); return;
            default: reply.fail("applovin: unknown method " + method);
        }
    }

    private void init(JSONObject args) {
        config = args;
        String key = args.optString("sdkKey", "");
        if (key.isEmpty()) { emit("app.log", "applovin: no \"sdkKey\" in game.json"); return; }
        AppLovinSdkInitializationConfiguration.Builder b = AppLovinSdkInitializationConfiguration.builder(key, activity);
        b.setMediationProvider(AppLovinMediationProvider.MAX);
        JSONArray devices = args.optJSONArray("testDevices");
        if (devices != null) {
            List<String> ids = new ArrayList<>();
            for (int i = 0; i < devices.length(); ++i) ids.add(devices.optString(i));
            b.setTestDeviceAdvertisingIds(ids);
        }
        AppLovinSdk.getInstance(activity).initialize(b.build(), sdkConfig -> activity.runOnUiThread(() -> {
            ready = true;
            load("interstitial");
            load("rewarded");
        }));
    }

    private String unit(String format) { return config.optString(format, ""); }

    private JSONObject ev(String format) {
        JSONObject o = new JSONObject();
        try { o.put("format", format); } catch (Exception ignored) {}
        return o;
    }

    // One listener for both full-screen formats; rewards come through onUserRewarded.
    private final MaxRewardedAdListener listener = new MaxRewardedAdListener() {
        @Override public void onAdLoaded(MaxAd ad) { emit("loaded", ev(formatOf(ad))); }
        @Override public void onAdLoadFailed(String adUnitId, MaxError error) { emit("failed", ev(formatOfUnit(adUnitId))); }
        @Override public void onAdDisplayed(MaxAd ad) { emit("opened", ev(formatOf(ad))); }
        @Override public void onAdHidden(MaxAd ad) { emit("closed", ev(formatOf(ad))); finish(true, null); load(formatOf(ad)); }
        @Override public void onAdClicked(MaxAd ad) {}
        @Override public void onAdDisplayFailed(MaxAd ad, MaxError error) { finish(false, error.getMessage()); load(formatOf(ad)); }
        @Override public void onUserRewarded(MaxAd ad, MaxReward reward) { earned = true; emit("rewarded", ev("rewarded")); }
    };

    private String formatOf(MaxAd ad) { return ad.getFormat() == MaxAdFormat.REWARDED ? "rewarded" : "interstitial"; }
    private String formatOfUnit(String unit) { return unit.equals(unit("rewarded")) ? "rewarded" : "interstitial"; }

    private void load(String format) {
        if (!ready || unit(format).isEmpty()) return;
        if (format.equals("interstitial")) {
            if (interstitial == null) { interstitial = new MaxInterstitialAd(unit(format), activity); interstitial.setListener(listener); }
            interstitial.loadAd();
        } else if (format.equals("rewarded")) {
            if (rewarded == null) { rewarded = MaxRewardedAd.getInstance(unit(format), activity); rewarded.setListener(listener); }
            rewarded.loadAd();
        }
    }

    private void show(String format, String placement, Reply reply) {
        if (showReply != null) { reply.fail("another ad is on screen"); return; }
        boolean isReady = format.equals("interstitial") ? interstitial != null && interstitial.isReady()
                        : format.equals("rewarded") && rewarded != null && rewarded.isReady();
        if (!isReady) { load(format); reply.fail("not loaded"); return; }
        showReply = reply;
        showFormat = format;
        earned = false;
        emit("app.pause", null);
        String where = placement.isEmpty() ? null : placement;
        if (format.equals("interstitial")) interstitial.showAd(where, activity);
        else rewarded.showAd(where, activity);
    }

    private void finish(boolean shown, String error) {
        Reply r = showReply;
        showReply = null;
        if (r == null) return;
        emit("app.resume", null);
        JSONObject res = new JSONObject();
        try { res.put("shown", shown).put("rewarded", "rewarded".equals(showFormat) && earned); if (error != null) res.put("error", error); } catch (Exception ignored) {}
        r.ok(res);
    }

    private void banner(boolean show, boolean top, Reply reply) {
        if (!show) {
            if (banner != null) { banner.stopAutoRefresh(); banner.destroy(); ((android.view.ViewGroup) banner.getParent()).removeView(banner); banner = null; }
            emitBanner(0, top);
            reply.ok();
            return;
        }
        if (unit("banner").isEmpty()) { reply.fail("applovin: no \"banner\" ad unit in game.json"); return; }
        if (!ready) { reply.fail("applovin: SDK not initialized yet"); return; }
        if (banner == null) {
            float density = activity.getResources().getDisplayMetrics().density;
            int heightDp = MaxAdFormat.BANNER.getAdaptiveSize(activity).getHeight();
            banner = new MaxAdView(unit("banner"), activity);
            banner.setBackgroundColor(Color.BLACK); // AppLovin: full width with a background colour
            banner.setListener(new MaxAdViewAdListener() {
                @Override public void onAdLoaded(MaxAd ad) { emitBanner(heightDp, top); emit("loaded", ev("banner")); }
                @Override public void onAdLoadFailed(String adUnitId, MaxError error) { emit("failed", ev("banner")); }
                @Override public void onAdDisplayed(MaxAd ad) {}
                @Override public void onAdHidden(MaxAd ad) {}
                @Override public void onAdClicked(MaxAd ad) {}
                @Override public void onAdDisplayFailed(MaxAd ad, MaxError error) {}
                @Override public void onAdExpanded(MaxAd ad) {}
                @Override public void onAdCollapsed(MaxAd ad) {}
            });
            activity.addContentView(banner, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                Math.round(heightDp * density), top ? Gravity.TOP : Gravity.BOTTOM));
            banner.loadAd();
        }
        banner.setVisibility(View.VISIBLE);
        banner.startAutoRefresh();
        reply.ok();
    }

    private void emitBanner(int heightDp, boolean top) {
        JSONObject o = new JSONObject();
        try { o.put("height", heightDp).put("position", top ? "top" : "bottom"); } catch (Exception ignored) {}
        emit("app.banner", o);
    }

    @Override public void onDestroy() { if (banner != null) banner.destroy(); }
}
