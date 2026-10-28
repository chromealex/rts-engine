// ironSource / Unity LevelPlay SDK 9 (iOS, ad-unit API) → the engine's "ads" protocol (docs/plugins.md).
// Ads are created after LevelPlay init succeeds; didRewardAd may arrive after didCloseAd, so a
// rewarded show waits a moment for it before replying.
#import <IronSource/IronSource.h>
#import <UIKit/UIKit.h>
#import "engine/platform/apple/RTSPlugin.h"

@interface RTSLevelPlayPlugin : NSObject <RTSPlugin, LPMInterstitialAdDelegate, LPMRewardedAdDelegate, LPMBannerAdViewDelegate>
@end

@implementation RTSLevelPlayPlugin {
    NSDictionary* _config;
    BOOL _ready, _bannerTop, _earned, _closed;
    LPMInterstitialAd* _interstitial;
    LPMRewardedAd* _rewarded;
    LPMBannerAdView* _banner;
    CGFloat _bannerHeight;
    RTSPluginReply* _showReply;
    NSString* _showFormat;
}

- (void)call:(NSString*)method args:(NSDictionary*)args reply:(RTSPluginReply*)reply {
    if ([method isEqualToString:@"init"]) {
        [self initSdk:args];
        [reply ok:nil];
    } else if ([method isEqualToString:@"consent"]) {
        [LPMPrivacySettings setGDPRConsent:[args[@"personalized"] boolValue]];
        [reply ok:nil];
    } else if ([method isEqualToString:@"load"]) {
        [self load:args[@"format"]];
        [reply ok:nil];
    } else if ([method isEqualToString:@"show"]) {
        [self show:args[@"format"] placement:args[@"placement"] reply:reply];
    } else if ([method isEqualToString:@"banner"]) {
        [self banner:[args[@"show"] boolValue] top:[args[@"position"] isEqual:@"top"] reply:reply];
    } else if ([method isEqualToString:@"lifecycle"]) {
        [reply ok:nil];
    } else {
        [reply fail:[@"ironsource: unknown method " stringByAppendingString:method]];
    }
}

- (void)initSdk:(NSDictionary*)args {
    _config = args;
    NSString* key = [args[@"appKey"] isKindOfClass:NSString.class] ? args[@"appKey"] : @"";
    if (!key.length) { RTSPluginEmit(@"ironsource", @"app.log", @"ironsource: no \"appKey\" in game.json"); return; }
    LPMInitRequest* request = [[[LPMInitRequestBuilder alloc] initWithAppKey:key] build];
    [LevelPlay initWithRequest:request completion:^(LPMConfiguration* config, NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (error) { RTSPluginEmit(@"ironsource", @"app.log", [@"ironsource: init failed: " stringByAppendingString:error.localizedDescription]); return; }
            self->_ready = YES;
            [self load:@"interstitial"];
            [self load:@"rewarded"];
        });
    }];
}

- (NSString*)unit:(NSString*)format { id u = _config[format]; return [u isKindOfClass:NSString.class] ? u : @""; }
- (void)event:(NSString*)ev format:(NSString*)f { RTSPluginEmit(@"ironsource", ev, @{@"format": f ?: @""}); }
- (NSString*)formatOfUnit:(NSString*)unit {
    if ([unit isEqualToString:[self unit:@"banner"]]) return @"banner";
    return [unit isEqualToString:[self unit:@"rewarded"]] ? @"rewarded" : @"interstitial";
}
- (NSString*)formatOfInfo:(LPMAdInfo*)info { return [self formatOfUnit:info.adUnitId]; }

- (void)load:(NSString*)format {
    NSString* unit = [self unit:format];
    if (!_ready || !unit.length) return;
    if ([format isEqualToString:@"interstitial"]) {
        if (!_interstitial) { _interstitial = [[LPMInterstitialAd alloc] initWithAdUnitId:unit]; _interstitial.delegate = self; }
        [_interstitial loadAd];
    } else if ([format isEqualToString:@"rewarded"]) {
        if (!_rewarded) { _rewarded = [[LPMRewardedAd alloc] initWithAdUnitId:unit]; _rewarded.delegate = self; }
        [_rewarded loadAd];
    }
}

- (void)show:(NSString*)format placement:(NSString*)placement reply:(RTSPluginReply*)reply {
    if (_showReply) { [reply fail:@"another ad is on screen"]; return; }
    BOOL isInter = [format isEqualToString:@"interstitial"], isRew = [format isEqualToString:@"rewarded"];
    BOOL ready = (isInter && [_interstitial isAdReady]) || (isRew && [_rewarded isAdReady]);
    if (!ready) { [self load:format]; [reply fail:@"not loaded"]; return; }
    NSString* where = [placement isKindOfClass:NSString.class] && placement.length ? placement : nil;
    _showReply = reply; _showFormat = format; _earned = NO; _closed = NO;
    RTSPluginEmit(@"ironsource", @"app.pause", nil);
    UIViewController* vc = RTSPluginPresentingViewController();
    if (isInter) [_interstitial showAdWithViewController:vc placementName:where];
    else [_rewarded showAdWithViewController:vc placementName:where];
}

- (void)finishShow:(BOOL)shown error:(NSString*)error {
    RTSPluginReply* r = _showReply;
    NSString* f = _showFormat;
    _showReply = nil;
    if (!r) return;
    RTSPluginEmit(@"ironsource", @"app.resume", nil);
    NSMutableDictionary* res = [@{@"shown": @(shown), @"rewarded": @([f isEqualToString:@"rewarded"] && _earned)} mutableCopy];
    if (error) res[@"error"] = error;
    [r ok:res];
    [self load:f];
}

// Shared by LPMInterstitialAdDelegate, LPMRewardedAdDelegate and LPMBannerAdViewDelegate
- (void)didLoadAdWithAdInfo:(LPMAdInfo*)adInfo {
    NSString* f = [self formatOfInfo:adInfo];
    if ([f isEqualToString:@"banner"])
        RTSPluginEmit(@"ironsource", @"app.banner", @{@"height": @(_bannerHeight), @"position": _bannerTop ? @"top" : @"bottom"});
    [self event:@"loaded" format:f];
}
- (void)didFailToLoadAdWithAdUnitId:(NSString*)adUnitId error:(NSError*)error { [self event:@"failed" format:[self formatOfUnit:adUnitId]]; }
- (void)didChangeAdInfo:(LPMAdInfo*)adInfo {}
- (void)didDisplayAdWithAdInfo:(LPMAdInfo*)adInfo {
    NSString* f = [self formatOfInfo:adInfo];
    if (![f isEqualToString:@"banner"]) [self event:@"opened" format:f];
}
- (void)didFailToDisplayAdWithAdInfo:(LPMAdInfo*)adInfo error:(NSError*)error {
    if (![[self formatOfInfo:adInfo] isEqualToString:@"banner"]) [self finishShow:NO error:error.localizedDescription];
}
- (void)didClickAdWithAdInfo:(LPMAdInfo*)adInfo {}
- (void)didCloseAdWithAdInfo:(LPMAdInfo*)adInfo {
    NSString* f = [self formatOfInfo:adInfo];
    [self event:@"closed" format:f];
    _closed = YES;
    if (![f isEqualToString:@"rewarded"] || _earned) { [self finishShow:YES error:nil]; return; }
    RTSPluginReply* pending = _showReply;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (self->_showReply == pending) [self finishShow:YES error:nil];
    });
}
- (void)didRewardAdWithAdInfo:(LPMAdInfo*)adInfo reward:(LPMReward*)reward {
    _earned = YES;
    [self event:@"rewarded" format:@"rewarded"];
    if (_closed) [self finishShow:YES error:nil];
}
- (void)didLeaveAppWithAdInfo:(LPMAdInfo*)adInfo {}
- (void)didExpandAdWithAdInfo:(LPMAdInfo*)adInfo {}
- (void)didCollapseAdWithAdInfo:(LPMAdInfo*)adInfo {}

- (void)banner:(BOOL)show top:(BOOL)top reply:(RTSPluginReply*)reply {
    if (!show) {
        [_banner destroy];
        [_banner removeFromSuperview];
        _banner = nil;
        RTSPluginEmit(@"ironsource", @"app.banner", @{@"height": @0, @"position": top ? @"top" : @"bottom"});
        [reply ok:nil];
        return;
    }
    NSString* unit = [self unit:@"banner"];
    if (!unit.length) { [reply fail:@"ironsource: no \"banner\" ad unit in game.json"]; return; }
    if (!_ready) { [reply fail:@"ironsource: SDK not initialized yet"]; return; }
    if (!_banner) {
        UIViewController* vc = RTSPluginPresentingViewController();
        LPMAdSize* size = [LPMAdSize createAdaptiveAdSize] ?: [LPMAdSize bannerSize];
        LPMBannerAdViewConfigBuilder* builder = [LPMBannerAdViewConfigBuilder new];
        [builder setWithAdSize:size];
        _banner = [[LPMBannerAdView alloc] initWithAdUnitId:unit config:[builder build]];
        [_banner setDelegate:self];
        _bannerTop = top;
        _bannerHeight = size.height;
        _banner.translatesAutoresizingMaskIntoConstraints = NO;
        [vc.view addSubview:_banner];
        UILayoutGuide* g = vc.view.safeAreaLayoutGuide;
        [NSLayoutConstraint activateConstraints:@[
            [_banner.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
            [_banner.widthAnchor constraintEqualToConstant:size.width],
            [_banner.heightAnchor constraintEqualToConstant:size.height],
            top ? [_banner.topAnchor constraintEqualToAnchor:g.topAnchor] : [_banner.bottomAnchor constraintEqualToAnchor:g.bottomAnchor]]];
        [_banner loadAdWithViewController:vc];
    }
    _banner.hidden = NO;
    [reply ok:nil];
}

@end
