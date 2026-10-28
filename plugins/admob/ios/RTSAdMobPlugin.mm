// Google Mobile Ads SDK (AdMob, iOS) → the engine's "ads" protocol (docs/plugins.md).
// Objective-C names are unchanged in SDK 12–13 (only Swift dropped the GAD prefix).
#import <GoogleMobileAds/GoogleMobileAds.h>
#import <UIKit/UIKit.h>
#import "engine/platform/apple/RTSPlugin.h"

@interface RTSAdMobPlugin : NSObject <RTSPlugin, GADFullScreenContentDelegate, GADBannerViewDelegate>
@end

@implementation RTSAdMobPlugin {
    NSDictionary* _config;
    GADInterstitialAd* _interstitial;
    GADRewardedAd* _rewarded;
    GADBannerView* _banner;
    BOOL _bannerTop, _earned;
    RTSPluginReply* _showReply;
    NSString* _showFormat;
}

- (void)call:(NSString*)method args:(NSDictionary*)args reply:(RTSPluginReply*)reply {
    if ([method isEqualToString:@"init"]) {
        _config = args;
        [GADMobileAds.sharedInstance startWithCompletionHandler:^(GADInitializationStatus* status) {
            dispatch_async(dispatch_get_main_queue(), ^{ [self load:@"interstitial"]; [self load:@"rewarded"]; });
        }];
        [reply ok:nil];
    } else if ([method isEqualToString:@"consent"]) {
        GADMobileAds.sharedInstance.requestConfiguration.tagForChildDirectedTreatment = @([args[@"ageRestricted"] boolValue]);
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
        [reply fail:[@"admob: unknown method " stringByAppendingString:method]];
    }
}

- (NSString*)unit:(NSString*)format { id u = _config[format]; return [u isKindOfClass:NSString.class] ? u : @""; }
- (void)event:(NSString*)ev format:(NSString*)f { RTSPluginEmit(@"admob", ev, @{@"format": f ?: @""}); }

- (void)load:(NSString*)format {
    NSString* unit = [self unit:format];
    if (!unit.length) return;
    if ([format isEqualToString:@"interstitial"]) {
        [GADInterstitialAd loadWithAdUnitID:unit request:[GADRequest request] completionHandler:^(GADInterstitialAd* ad, NSError* error) {
            self->_interstitial = ad;
            [self event:ad ? @"loaded" : @"failed" format:format];
        }];
    } else if ([format isEqualToString:@"rewarded"]) {
        [GADRewardedAd loadWithAdUnitID:unit request:[GADRequest request] completionHandler:^(GADRewardedAd* ad, NSError* error) {
            self->_rewarded = ad;
            [self event:ad ? @"loaded" : @"failed" format:format];
        }];
    }
}

- (void)show:(NSString*)format reply:(RTSPluginReply*)reply {
    UIViewController* vc = RTSPluginPresentingViewController();
    if (_showReply) { [reply fail:@"another ad is on screen"]; return; }
    _earned = NO;
    if ([format isEqualToString:@"interstitial"] && _interstitial) {
        GADInterstitialAd* ad = _interstitial; _interstitial = nil;
        _showReply = reply; _showFormat = format;
        ad.fullScreenContentDelegate = self;
        RTSPluginEmit(@"admob", @"app.pause", nil);
        [ad presentFromRootViewController:vc];
    } else if ([format isEqualToString:@"rewarded"] && _rewarded) {
        GADRewardedAd* ad = _rewarded; _rewarded = nil;
        _showReply = reply; _showFormat = format;
        ad.fullScreenContentDelegate = self;
        RTSPluginEmit(@"admob", @"app.pause", nil);
        [ad presentFromRootViewController:vc userDidEarnRewardHandler:^{
            self->_earned = YES;
            [self event:@"rewarded" format:format];
        }];
    } else {
        [self load:format];
        [reply fail:@"not loaded"];
    }
}

- (void)finishShow:(BOOL)shown error:(NSString*)error {
    RTSPluginReply* r = _showReply;
    NSString* f = _showFormat;
    _showReply = nil;
    RTSPluginEmit(@"admob", @"app.resume", nil);
    NSMutableDictionary* res = [@{@"shown": @(shown), @"rewarded": @(_earned)} mutableCopy];
    if (error) res[@"error"] = error;
    [r ok:res];
    [self load:f]; // next one
}

- (void)adWillPresentFullScreenContent:(id<GADFullScreenPresentingAd>)ad { [self event:@"opened" format:_showFormat]; }
- (void)ad:(id<GADFullScreenPresentingAd>)ad didFailToPresentFullScreenContentWithError:(NSError*)error { [self finishShow:NO error:error.localizedDescription]; }
- (void)adDidDismissFullScreenContent:(id<GADFullScreenPresentingAd>)ad { [self event:@"closed" format:_showFormat]; [self finishShow:YES error:nil]; }

- (void)banner:(BOOL)show top:(BOOL)top reply:(RTSPluginReply*)reply {
    if (!show) {
        [_banner removeFromSuperview];
        _banner = nil;
        RTSPluginEmit(@"admob", @"app.banner", @{@"height": @0, @"position": top ? @"top" : @"bottom"});
        [reply ok:nil];
        return;
    }
    NSString* unit = [self unit:@"banner"];
    if (!unit.length) { [reply fail:@"admob: no \"banner\" ad unit in game.json"]; return; }
    if (!_banner) {
        UIViewController* vc = RTSPluginPresentingViewController();
        CGFloat width = CGRectGetWidth(UIEdgeInsetsInsetRect(vc.view.bounds, vc.view.safeAreaInsets));
        _banner = [[GADBannerView alloc] initWithAdSize:GADCurrentOrientationAnchoredAdaptiveBannerAdSizeWithWidth(width)];
        _banner.adUnitID = unit;
        _banner.rootViewController = vc;
        _banner.delegate = self;
        _bannerTop = top;
        _banner.translatesAutoresizingMaskIntoConstraints = NO;
        [vc.view addSubview:_banner];
        UILayoutGuide* g = vc.view.safeAreaLayoutGuide;
        [NSLayoutConstraint activateConstraints:@[
            [_banner.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
            top ? [_banner.topAnchor constraintEqualToAnchor:g.topAnchor] : [_banner.bottomAnchor constraintEqualToAnchor:g.bottomAnchor]]];
        [_banner loadRequest:[GADRequest request]];
    }
    _banner.hidden = NO;
    [reply ok:nil];
}

- (void)bannerViewDidReceiveAd:(GADBannerView*)bannerView {
    RTSPluginEmit(@"admob", @"app.banner", @{@"height": @(bannerView.bounds.size.height), @"position": _bannerTop ? @"top" : @"bottom"});
    [self event:@"loaded" format:@"banner"];
}
- (void)bannerView:(GADBannerView*)bannerView didFailToReceiveAdWithError:(NSError*)error { [self event:@"failed" format:@"banner"]; }

@end
