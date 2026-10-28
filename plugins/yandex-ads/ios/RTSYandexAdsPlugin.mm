// Yandex Mobile Ads SDK 8 (iOS) → the engine's "ads" protocol (docs/plugins.md).
// The SDK is written in Swift and keeps YMA-prefixed Objective-C names. Confirmed in Yandex docs:
// YMAYandexAds setters/initializeSDK, YMAAdRequest initWithAdUnitID:, loaders'
// loadAdWithRequest:completionHandler:, YMABannerAdSize stickyWithContainerWidth:,
// YMABannerAdView initWithAdSize: / loadAdWithRequest:. UNVERIFIED (derived from the Swift API by
// the Swift→ObjC naming rules): showFromViewController:, delegate selectors below. If Xcode
// disagrees, check YandexMobileAds-Swift.h in the pod.
#import <YandexMobileAds/YandexMobileAds.h>
#import <UIKit/UIKit.h>
#import "engine/platform/apple/RTSPlugin.h"

@interface RTSYandexAdsPlugin : NSObject <RTSPlugin, YMAInterstitialAdDelegate, YMARewardedAdDelegate, YMABannerAdViewDelegate>
@end

@implementation RTSYandexAdsPlugin {
    NSDictionary* _config;
    YMAInterstitialAdLoader* _interstitialLoader;
    YMARewardedAdLoader* _rewardedLoader;
    YMAInterstitialAd* _interstitial;
    YMARewardedAd* _rewarded;
    YMABannerAdView* _banner;
    BOOL _bannerTop, _earned;
    RTSPluginReply* _showReply;
    NSString* _showFormat;
}

- (void)call:(NSString*)method args:(NSDictionary*)args reply:(RTSPluginReply*)reply {
    if ([method isEqualToString:@"init"]) {
        _config = args;
        [YMAYandexAds initializeSDK];
        [self load:@"interstitial"];
        [self load:@"rewarded"];
        [reply ok:nil];
    } else if ([method isEqualToString:@"consent"]) {
        [YMAYandexAds setUserConsent:[args[@"personalized"] boolValue]];
        [YMAYandexAds setAgeRestricted:[args[@"ageRestricted"] boolValue]];
        [reply ok:nil];
    } else if ([method isEqualToString:@"load"]) {
        [self load:args[@"format"]];
        [reply ok:nil];
    } else if ([method isEqualToString:@"show"]) {
        [self show:args[@"format"] reply:reply];
    } else if ([method isEqualToString:@"banner"]) {
        [self banner:[args[@"show"] boolValue] top:[args[@"position"] isEqual:@"top"] reply:reply];
    } else if ([method isEqualToString:@"lifecycle"]) {
        [reply ok:nil];
    } else {
        [reply fail:[@"yandex-ads: unknown method " stringByAppendingString:method]];
    }
}

- (NSString*)unit:(NSString*)format { id u = _config[format]; return [u isKindOfClass:NSString.class] ? u : @""; }
- (void)event:(NSString*)ev format:(NSString*)f { RTSPluginEmit(@"yandex-ads", ev, @{@"format": f ?: @""}); }

- (void)load:(NSString*)format {
    NSString* unit = [self unit:format];
    if (!unit.length) return;
    YMAAdRequest* req = [[YMAAdRequest alloc] initWithAdUnitID:unit];
    if ([format isEqualToString:@"interstitial"]) {
        if (!_interstitialLoader) _interstitialLoader = [YMAInterstitialAdLoader new];
        [_interstitialLoader loadAdWithRequest:req completionHandler:^(YMAInterstitialAd* ad, NSError* error) {
            dispatch_async(dispatch_get_main_queue(), ^{ self->_interstitial = ad; [self event:ad ? @"loaded" : @"failed" format:format]; });
        }];
    } else if ([format isEqualToString:@"rewarded"]) {
        if (!_rewardedLoader) _rewardedLoader = [YMARewardedAdLoader new];
        [_rewardedLoader loadAdWithRequest:req completionHandler:^(YMARewardedAd* ad, NSError* error) {
            dispatch_async(dispatch_get_main_queue(), ^{ self->_rewarded = ad; [self event:ad ? @"loaded" : @"failed" format:format]; });
        }];
    }
}

- (void)show:(NSString*)format reply:(RTSPluginReply*)reply {
    UIViewController* vc = RTSPluginPresentingViewController();
    if (_showReply) { [reply fail:@"another ad is on screen"]; return; }
    _earned = NO;
    if ([format isEqualToString:@"interstitial"] && _interstitial) {
        YMAInterstitialAd* ad = _interstitial; _interstitial = nil;
        _showReply = reply; _showFormat = format;
        ad.delegate = self;
        RTSPluginEmit(@"yandex-ads", @"app.pause", nil);
        [ad showFromViewController:vc];
    } else if ([format isEqualToString:@"rewarded"] && _rewarded) {
        YMARewardedAd* ad = _rewarded; _rewarded = nil;
        _showReply = reply; _showFormat = format;
        ad.delegate = self;
        RTSPluginEmit(@"yandex-ads", @"app.pause", nil);
        [ad showFromViewController:vc];
    } else {
        [self load:format];
        [reply fail:@"not loaded"];
    }
}

- (void)finishShow:(BOOL)shown error:(NSString*)error {
    RTSPluginReply* r = _showReply;
    NSString* f = _showFormat;
    _showReply = nil;
    RTSPluginEmit(@"yandex-ads", @"app.resume", nil);
    NSMutableDictionary* res = [@{@"shown": @(shown), @"rewarded": @(_earned)} mutableCopy];
    if (error) res[@"error"] = error;
    [r ok:res];
    [self load:f];
}

// YMAInterstitialAdDelegate
- (void)interstitialAdDidShow:(YMAInterstitialAd*)ad { [self event:@"opened" format:_showFormat]; }
- (void)interstitialAdDidDismiss:(YMAInterstitialAd*)ad { [self event:@"closed" format:_showFormat]; [self finishShow:YES error:nil]; }
- (void)interstitialAd:(YMAInterstitialAd*)ad didFailToShowWithError:(NSError*)error { [self finishShow:NO error:error.localizedDescription]; }
// YMARewardedAdDelegate
- (void)rewardedAdDidShow:(YMARewardedAd*)ad { [self event:@"opened" format:_showFormat]; }
- (void)rewardedAdDidDismiss:(YMARewardedAd*)ad { [self event:@"closed" format:_showFormat]; [self finishShow:YES error:nil]; }
- (void)rewardedAd:(YMARewardedAd*)ad didFailToShowWithError:(NSError*)error { [self finishShow:NO error:error.localizedDescription]; }
- (void)rewardedAd:(YMARewardedAd*)ad didReward:(id<YMAReward>)reward { _earned = YES; [self event:@"rewarded" format:_showFormat]; }

- (void)banner:(BOOL)show top:(BOOL)top reply:(RTSPluginReply*)reply {
    if (!show) {
        [_banner removeFromSuperview];
        _banner = nil;
        RTSPluginEmit(@"yandex-ads", @"app.banner", @{@"height": @0, @"position": top ? @"top" : @"bottom"});
        [reply ok:nil];
        return;
    }
    NSString* unit = [self unit:@"banner"];
    if (!unit.length) { [reply fail:@"yandex-ads: no \"banner\" ad unit in game.json"]; return; }
    if (!_banner) {
        UIViewController* vc = RTSPluginPresentingViewController();
        CGFloat width = CGRectGetWidth(UIEdgeInsetsInsetRect(vc.view.bounds, vc.view.safeAreaInsets));
        _banner = [[YMABannerAdView alloc] initWithAdSize:[YMABannerAdSize stickyWithContainerWidth:width]];
        _banner.delegate = self;
        _bannerTop = top;
        _banner.translatesAutoresizingMaskIntoConstraints = NO;
        [vc.view addSubview:_banner];
        UILayoutGuide* g = vc.view.safeAreaLayoutGuide;
        [NSLayoutConstraint activateConstraints:@[
            [_banner.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
            top ? [_banner.topAnchor constraintEqualToAnchor:g.topAnchor] : [_banner.bottomAnchor constraintEqualToAnchor:g.bottomAnchor]]];
        [_banner loadAdWithRequest:[[YMAAdRequest alloc] initWithAdUnitID:unit]];
    }
    _banner.hidden = NO;
    [reply ok:nil];
}

// YMABannerAdViewDelegate
- (void)bannerAdViewDidLoad:(YMABannerAdView*)adView {
    [adView layoutIfNeeded];
    RTSPluginEmit(@"yandex-ads", @"app.banner", @{@"height": @(adView.bounds.size.height), @"position": _bannerTop ? @"top" : @"bottom"});
    [self event:@"loaded" format:@"banner"];
}
- (void)bannerAdViewDidFailLoading:(YMABannerAdView*)adView error:(NSError*)error { [self event:@"failed" format:@"banner"]; }

@end
