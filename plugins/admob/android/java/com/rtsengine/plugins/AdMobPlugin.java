// Google Mobile Ads SDK (AdMob) → the engine's "ads" protocol (docs/plugins.md).
// Config (game.json "plugins" → "admob"): appId (manifest), interstitial / rewarded / banner ad unit
// ids. Google test ids: interstitial ca-app-pub-3940256099942544/1033173712,
// rewarded .../5224354917, banner .../9214589741, app ca-app-pub-3940256099942544~3347511713.
package com.rtsengine.plugins;

import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;

import com.google.ads.mediation.admob.AdMobAdapter;
import com.google.android.gms.ads.AdError;
import com.google.android.gms.ads.AdListener;
import com.google.android.gms.ads.AdRequest;
import com.google.android.gms.ads.AdSize;
import com.google.android.gms.ads.AdView;
import com.google.android.gms.ads.FullScreenContentCallback;
import com.google.android.gms.ads.LoadAdError;
import com.google.android.gms.ads.MobileAds;
import com.google.android.gms.ads.RequestConfiguration;
import com.google.android.gms.ads.interstitial.InterstitialAd;
import com.google.android.gms.ads.interstitial.InterstitialAdLoadCallback;
import com.google.android.gms.ads.rewarded.RewardedAd;
import com.google.android.gms.ads.rewarded.RewardedAdLoadCallback;
import com.rtsengine.platform.RtsPlugin;

import org.json.JSONObject;

public class AdMobPlugin extends RtsPlugin {
    private JSONObject config = new JSONObject();
    private boolean personalized = true;
    private InterstitialAd interstitial;
    private RewardedAd rewarded;
    private AdView banner;

    @Override
    public void call(String method, JSONObject args, Reply reply) {
        switch (method) {
            case "init":
                config = args;
                // initialize off the main thread (Google's recommendation), then preload on it
                new Thread(() -> MobileAds.initialize(activity, status -> activity.runOnUiThread(() -> {
                    load("interstitial");
                    load("rewarded");
                }))).start();
                reply.ok();
                return;
            case "consent":
                personalized = args.optBoolean("personalized", false);
                MobileAds.setRequestConfiguration(MobileAds.getRequestConfiguration().toBuilder()
                    .setTagForChildDirectedTreatment(args.optBoolean("ageRestricted", false)
                        ? RequestConfiguration.TAG_FOR_CHILD_DIRECTED_TREATMENT_TRUE
                        : RequestConfiguration.TAG_FOR_CHILD_DIRECTED_TREATMENT_UNSPECIFIED)
                    .build());
                reply.ok();
                return;
            case "load": load(args.optString("format")); reply.ok(); return;
            case "show": show(args.optString("format"), reply); return;
            case "banner": banner(args.optBoolean("show", true), "top".equals(args.optString("position")), reply); return;
            case "lifecycle": reply.ok(); return;
            default: reply.fail("admob: unknown method " + method);
        }
    }

    private String unit(String format) { return config.optString(format, ""); }

    private AdRequest request() {
        AdRequest.Builder b = new AdRequest.Builder();
        if (!personalized) { Bundle extras = new Bundle(); extras.putString("npa", "1"); b.addNetworkExtrasBundle(AdMobAdapter.class, extras); }
        return b.build();
    }

    private JSONObject ev(String format) {
        JSONObject o = new JSONObject();
        try { o.put("format", format); } catch (Exception ignored) {}
        return o;
    }

    private void load(String format) {
        String id = unit(format);
        if (id.isEmpty()) return;
        if (format.equals("interstitial")) {
            InterstitialAd.load(activity, id, request(), new InterstitialAdLoadCallback() {
                @Override public void onAdLoaded(InterstitialAd ad) { interstitial = ad; emit("loaded", ev(format)); }
                @Override public void onAdFailedToLoad(LoadAdError e) { emit("failed", ev(format)); }
            });
        } else if (format.equals("rewarded")) {
            RewardedAd.load(activity, id, request(), new RewardedAdLoadCallback() {
                @Override public void onAdLoaded(RewardedAd ad) { rewarded = ad; emit("loaded", ev(format)); }
                @Override public void onAdFailedToLoad(LoadAdError e) { emit("failed", ev(format)); }
            });
        }
    }

    private void show(String format, Reply reply) {
        final boolean[] earned = {false};
        FullScreenContentCallback cb = new FullScreenContentCallback() {
            @Override public void onAdShowedFullScreenContent() { emit("opened", ev(format)); }
            @Override public void onAdFailedToShowFullScreenContent(AdError e) { done(reply, false, false, e.getMessage()); }
            @Override public void onAdDismissedFullScreenContent() { emit("closed", ev(format)); done(reply, true, earned[0], null); }
        };
        if (format.equals("interstitial")) {
            InterstitialAd ad = interstitial;
            interstitial = null;
            if (ad == null) { load(format); reply.fail("not loaded"); return; }
            emit("app.pause", null);
            ad.setFullScreenContentCallback(cb);
            ad.show(activity);
        } else if (format.equals("rewarded")) {
            RewardedAd ad = rewarded;
            rewarded = null;
            if (ad == null) { load(format); reply.fail("not loaded"); return; }
            emit("app.pause", null);
            ad.setFullScreenContentCallback(cb);
            ad.show(activity, item -> { earned[0] = true; emit("rewarded", ev(format)); });
        } else {
            reply.fail("admob: unknown format " + format);
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
        if (id.isEmpty()) { reply.fail("admob: no \"banner\" ad unit in game.json"); return; }
        if (banner == null) {
            float density = activity.getResources().getDisplayMetrics().density;
            int widthDp = Math.round(activity.getResources().getDisplayMetrics().widthPixels / density);
            AdSize size = AdSize.getCurrentOrientationAnchoredAdaptiveBannerAdSize(activity, widthDp);
            banner = new AdView(activity);
            banner.setAdUnitId(id);
            banner.setAdSize(size);
            banner.setAdListener(new AdListener() {
                @Override public void onAdLoaded() { emitBanner(size.getHeightInPixels(activity), top); emit("loaded", ev("banner")); }
                @Override public void onAdFailedToLoad(LoadAdError e) { emit("failed", ev("banner")); }
            });
            activity.addContentView(banner, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, (top ? Gravity.TOP : Gravity.BOTTOM) | Gravity.CENTER_HORIZONTAL));
            banner.loadAd(request());
        }
        banner.setVisibility(View.VISIBLE);
        reply.ok();
    }

    private void emitBanner(int px, boolean top) {
        JSONObject o = new JSONObject();
        try { o.put("height", px / activity.getResources().getDisplayMetrics().density).put("position", top ? "top" : "bottom"); } catch (Exception ignored) {}
        emit("app.banner", o);
    }

    @Override public void onPause() { if (banner != null) banner.pause(); }
    @Override public void onResume() { if (banner != null) banner.resume(); }
    @Override public void onDestroy() { if (banner != null) banner.destroy(); }
}
