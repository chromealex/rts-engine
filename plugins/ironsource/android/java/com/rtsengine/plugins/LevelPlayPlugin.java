// ironSource / Unity LevelPlay SDK 9 (ad-unit API) → the engine's "ads" protocol (docs/plugins.md).
// Ads are created after LevelPlay.init succeeds (LevelPlay requirement); privacy flags are set
// before init. API per developers.is.com "LevelPlay SDK 9 Android" integration guides.
package com.rtsengine.plugins;

import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;

import com.rtsengine.platform.RtsPlugin;
import com.unity3d.mediation.LevelPlay;
import com.unity3d.mediation.LevelPlayAdError;
import com.unity3d.mediation.LevelPlayAdInfo;
import com.unity3d.mediation.LevelPlayAdSize;
import com.unity3d.mediation.LevelPlayConfiguration;
import com.unity3d.mediation.LevelPlayInitError;
import com.unity3d.mediation.LevelPlayInitListener;
import com.unity3d.mediation.LevelPlayInitRequest;
import com.unity3d.mediation.LevelPlayPrivacySettings;
import com.unity3d.mediation.banner.LevelPlayBannerAdView;
import com.unity3d.mediation.banner.LevelPlayBannerAdViewListener;
import com.unity3d.mediation.interstitial.LevelPlayInterstitialAd;
import com.unity3d.mediation.interstitial.LevelPlayInterstitialAdListener;
import com.unity3d.mediation.rewarded.LevelPlayReward;
import com.unity3d.mediation.rewarded.LevelPlayRewardedAd;
import com.unity3d.mediation.rewarded.LevelPlayRewardedAdListener;

import org.json.JSONObject;

public class LevelPlayPlugin extends RtsPlugin {
    private JSONObject config = new JSONObject();
    private boolean ready;
    private LevelPlayInterstitialAd interstitial;
    private LevelPlayRewardedAd rewarded;
    private LevelPlayBannerAdView banner;
    private Reply showReply;
    private String showFormat;
    private boolean earned, closed;
    private static final long REWARD_GRACE_MS = 1500;

    @Override
    public void call(String method, JSONObject args, Reply reply) {
        switch (method) {
            case "init": init(args); reply.ok(); return;
            case "consent":
                LevelPlayPrivacySettings.setGDPRConsent(args.optBoolean("personalized", false));
                if (args.has("ageRestricted")) LevelPlayPrivacySettings.setCOPPA(args.optBoolean("ageRestricted", false));
                reply.ok();
                return;
            case "load": load(args.optString("format")); reply.ok(); return;
            case "show": show(args.optString("format"), args.optString("placement", ""), reply); return;
            case "banner": banner(args.optBoolean("show", true), "top".equals(args.optString("position")), reply); return;
            case "lifecycle": reply.ok(); return;
            default: reply.fail("ironsource: unknown method " + method);
        }
    }

    private void init(JSONObject args) {
        config = args;
        String key = args.optString("appKey", "");
        if (key.isEmpty()) { emit("app.log", "ironsource: no \"appKey\" in game.json"); return; }
        LevelPlay.init(activity, new LevelPlayInitRequest.Builder(key).build(), new LevelPlayInitListener() {
            @Override public void onInitSuccess(LevelPlayConfiguration configuration) {
                activity.runOnUiThread(() -> { ready = true; load("interstitial"); load("rewarded"); });
            }
            @Override public void onInitFailed(LevelPlayInitError error) {
                emit("app.log", "ironsource: init failed: " + error.getErrorMessage());
            }
        });
    }

    private String unit(String format) { return config.optString(format, ""); }

    private JSONObject ev(String format) {
        JSONObject o = new JSONObject();
        try { o.put("format", format); } catch (Exception ignored) {}
        return o;
    }

    private void load(String format) {
        if (!ready || unit(format).isEmpty()) return;
        if (format.equals("interstitial")) {
            if (interstitial == null) {
                interstitial = new LevelPlayInterstitialAd(unit(format));
                interstitial.setListener(new LevelPlayInterstitialAdListener() {
                    @Override public void onAdLoaded(LevelPlayAdInfo info) { emit("loaded", ev("interstitial")); }
                    @Override public void onAdLoadFailed(LevelPlayAdError error) { emit("failed", ev("interstitial")); }
                    @Override public void onAdDisplayed(LevelPlayAdInfo info) { emit("opened", ev("interstitial")); }
                    @Override public void onAdDisplayFailed(LevelPlayAdError error, LevelPlayAdInfo info) { finish(false, error.getErrorMessage()); }
                    @Override public void onAdClicked(LevelPlayAdInfo info) {}
                    @Override public void onAdClosed(LevelPlayAdInfo info) { emit("closed", ev("interstitial")); finish(true, null); }
                    @Override public void onAdInfoChanged(LevelPlayAdInfo info) {}
                });
            }
            interstitial.loadAd();
        } else if (format.equals("rewarded")) {
            if (rewarded == null) {
                rewarded = new LevelPlayRewardedAd(unit(format));
                rewarded.setListener(new LevelPlayRewardedAdListener() {
                    @Override public void onAdLoaded(LevelPlayAdInfo info) { emit("loaded", ev("rewarded")); }
                    @Override public void onAdLoadFailed(LevelPlayAdError error) { emit("failed", ev("rewarded")); }
                    @Override public void onAdDisplayed(LevelPlayAdInfo info) { emit("opened", ev("rewarded")); }
                    @Override public void onAdDisplayFailed(LevelPlayAdError error, LevelPlayAdInfo info) { finish(false, error.getErrorMessage()); }
                    @Override public void onAdClicked(LevelPlayAdInfo info) {}
                    @Override public void onAdClosed(LevelPlayAdInfo info) {
                        emit("closed", ev("rewarded"));
                        // onAdRewarded may arrive after onAdClosed (LevelPlay docs): give it a moment
                        closed = true;
                        if (earned) finish(true, null);
                        else {
                            Reply pending = showReply;
                            new Handler(Looper.getMainLooper()).postDelayed(() -> { if (showReply == pending) finish(true, null); }, REWARD_GRACE_MS);
                        }
                    }
                    @Override public void onAdInfoChanged(LevelPlayAdInfo info) {}
                    @Override public void onAdRewarded(LevelPlayReward reward, LevelPlayAdInfo info) {
                        earned = true;
                        emit("rewarded", ev("rewarded"));
                        if (closed) finish(true, null);
                    }
                });
            }
            rewarded.loadAd();
        }
    }

    private void show(String format, String placement, Reply reply) {
        if (showReply != null) { reply.fail("another ad is on screen"); return; }
        boolean isReady = format.equals("interstitial") ? interstitial != null && interstitial.isAdReady()
                        : format.equals("rewarded") && rewarded != null && rewarded.isAdReady();
        if (!isReady) { load(format); reply.fail("not loaded"); return; }
        showReply = reply;
        showFormat = format;
        earned = false;
        closed = false;
        emit("app.pause", null);
        String where = placement.isEmpty() ? null : placement;
        if (format.equals("interstitial")) interstitial.showAd(activity, where);
        else rewarded.showAd(activity, where);
    }

    private void finish(boolean shown, String error) {
        Reply r = showReply;
        String f = showFormat;
        showReply = null;
        if (r == null) return;
        emit("app.resume", null);
        JSONObject res = new JSONObject();
        try { res.put("shown", shown).put("rewarded", "rewarded".equals(f) && earned); if (error != null) res.put("error", error); } catch (Exception ignored) {}
        r.ok(res);
        load(f); // ad objects are reusable: load the next one
    }

    private void banner(boolean show, boolean top, Reply reply) {
        if (!show) {
            if (banner != null) { banner.destroy(); ((android.view.ViewGroup) banner.getParent()).removeView(banner); banner = null; }
            emitBanner(0, top);
            reply.ok();
            return;
        }
        if (unit("banner").isEmpty()) { reply.fail("ironsource: no \"banner\" ad unit in game.json"); return; }
        if (!ready) { reply.fail("ironsource: SDK not initialized yet"); return; }
        if (banner == null) {
            LevelPlayAdSize size = LevelPlayAdSize.createAdaptiveAdSize(activity);
            if (size == null) size = LevelPlayAdSize.BANNER;
            final int heightDp = size.getHeight();
            LevelPlayBannerAdView.Config cfg = new LevelPlayBannerAdView.Config.Builder().setAdSize(size).build();
            banner = new LevelPlayBannerAdView(activity, unit("banner"), cfg);
            banner.setBannerListener(new LevelPlayBannerAdViewListener() {
                @Override public void onAdLoaded(LevelPlayAdInfo info) { emitBanner(heightDp, top); emit("loaded", ev("banner")); }
                @Override public void onAdLoadFailed(LevelPlayAdError error) { emit("failed", ev("banner")); }
                @Override public void onAdDisplayed(LevelPlayAdInfo info) {}
                @Override public void onAdDisplayFailed(LevelPlayAdInfo info, LevelPlayAdError error) {}
                @Override public void onAdClicked(LevelPlayAdInfo info) {}
                @Override public void onAdExpanded(LevelPlayAdInfo info) {}
                @Override public void onAdCollapsed(LevelPlayAdInfo info) {}
                @Override public void onAdLeftApplication(LevelPlayAdInfo info) {}
            });
            float density = activity.getResources().getDisplayMetrics().density;
            activity.addContentView(banner, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                Math.round(heightDp * density), (top ? Gravity.TOP : Gravity.BOTTOM) | Gravity.CENTER_HORIZONTAL));
            banner.loadAd();
        }
        banner.setVisibility(View.VISIBLE);
        reply.ok();
    }

    private void emitBanner(int heightDp, boolean top) {
        JSONObject o = new JSONObject();
        try { o.put("height", heightDp).put("position", top ? "top" : "bottom"); } catch (Exception ignored) {}
        emit("app.banner", o);
    }

    @Override public void onDestroy() { if (banner != null) banner.destroy(); }
}
