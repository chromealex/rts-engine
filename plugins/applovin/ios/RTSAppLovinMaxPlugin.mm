// AppLovin MAX SDK 13 (iOS) → the engine's "ads" protocol (docs/plugins.md).
// Init via ALSdkInitializationConfiguration (SDK key in code, no Info.plist entry since SDK 12).
#import <AppLovinSDK/AppLovinSDK.h>
#import <UIKit/UIKit.h>
#import "engine/platform/apple/RTSPlugin.h"

@interface RTSAppLovinMaxPlugin : NSObject <RTSPlugin, MAAdDelegate, MARewardedAdDelegate, MAAdViewAdDelegate>
@end

@implementation RTSAppLovinMaxPlugin {
    NSDictionary* _config;
    BOOL _ready, _bannerTop, _earned;
    MAInterstitialAd* _interstitial;
    MARewardedAd* _rewarded;
    MAAdView* _banner;
    RTSPluginReply* _showReply;
    NSString* _showFormat;
}

- (void)call:(NSString*)method args:(NSDictionary*)args reply:(RTSPluginReply*)reply {
    if ([method isEqualToString:@"init"]) {
        [self initSdk:args];
        [reply ok:nil];
    } else if ([method isEqualToString:@"consent"]) {
        [ALPrivacySettings setHasUserConsent:[args[@"personalized"] boolValue]];
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
        [reply fail:[@"applovin: unknown method " stringByAppendingString:method]];
    }
}

- (void)initSdk:(NSDictionary*)args {
    _config = args;
    NSString* key = [args[@"sdkKey"] isKindOfClass:NSString.class] ? args[@"sdkKey"] : @"";
    if (!key.length) { RTSPluginEmit(@"applovin", @"app.log", @"applovin: no \"sdkKey\" in game.json"); return; }
    NSArray* devices = [args[@"testDevices"] isKindOfClass:NSArray.class] ? args[@"testDevices"] : nil;
    ALSdkInitializationConfiguration* cfg = [ALSdkInitializationConfiguration configurationWithSdkKey:key
        builderBlock:^(ALSdkInitializationConfigurationBuilder* b) {
            b.mediationProvider = ALMediationProviderMAX;
            if (devices) b.testDeviceAdvertisingIdentifiers = devices;
        }];
    [[ALSdk shared] initializeWithConfiguration:cfg completionHandler:^(ALSdkConfiguration* sdkConfig) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_ready = YES;
            [self load:@"interstitial"];
            [self load:@"rewarded"];
        });
    }];
}

- (NSString*)unit:(NSString*)format { id u = _config[format]; return [u isKindOfClass:NSString.class] ? u : @""; }
- (void)event:(NSString*)ev format:(NSString*)f { RTSPluginEmit(@"applovin", ev, @{@"format": f ?: @""}); }
- (NSString*)formatOf:(MAAd*)ad { return ad.format == MAAdFormat.rewarded ? @"rewarded" : @"interstitial"; }

- (void)load:(NSString*)format {
    NSString* unit = [self unit:format];
    if (!_ready || !unit.length) return;
    if ([format isEqualToString:@"interstitial"]) {
        if (!_interstitial) { _interstitial = [[MAInterstitialAd alloc] initWithAdUnitIdentifier:unit]; _interstitial.delegate = self; }
        [_interstitial loadAd];
    } else if ([format isEqualToString:@"rewarded"]) {
        if (!_rewarded) { _rewarded = [MARewardedAd sharedWithAdUnitIdentifier:unit]; _rewarded.delegate = self; }
        [_rewarded loadAd];
    }
}

- (void)show:(NSString*)format placement:(NSString*)placement reply:(RTSPluginReply*)reply {
    if (_showReply) { [reply fail:@"another ad is on screen"]; return; }
    BOOL isInter = [format isEqualToString:@"interstitial"], isRew = [format isEqualToString:@"rewarded"];
    BOOL ready = (isInter && _interstitial.isReady) || (isRew && _rewarded.isReady);
    if (!ready) { [self load:format]; [reply fail:@"not loaded"]; return; }
    NSString* where = [placement isKindOfClass:NSString.class] && placement.length ? placement : nil;
    _showReply = reply; _showFormat = format; _earned = NO;
    RTSPluginEmit(@"applovin", @"app.pause", nil);
    if (isInter) [_interstitial showAdForPlacement:where];
    else [_rewarded showAdForPlacement:where];
}

- (void)finishShow:(BOOL)shown error:(NSString*)error {
    RTSPluginReply* r = _showReply;
    NSString* f = _showFormat;
    _showReply = nil;
    if (!r) return;
    RTSPluginEmit(@"applovin", @"app.resume", nil);
    NSMutableDictionary* res = [@{@"shown": @(shown), @"rewarded": @([f isEqualToString:@"rewarded"] && _earned)} mutableCopy];
    if (error) res[@"error"] = error;
    [r ok:res];
    [self load:f];
}

// MAAdDelegate (full-screen ads and, through MAAdViewAdDelegate, the banner)
- (void)didLoadAd:(MAAd*)ad {
    if (ad.format.isAdViewAd) {
        RTSPluginEmit(@"applovin", @"app.banner", @{@"height": @(_banner.bounds.size.height), @"position": _bannerTop ? @"top" : @"bottom"});
        [self event:@"loaded" format:@"banner"];
    } else {
        [self event:@"loaded" format:[self formatOf:ad]];
    }
}
- (void)didFailToLoadAdForAdUnitIdentifier:(NSString*)adUnitIdentifier withError:(MAError*)error {
    NSString* f = [adUnitIdentifier isEqualToString:[self unit:@"banner"]] ? @"banner"
                : [adUnitIdentifier isEqualToString:[self unit:@"rewarded"]] ? @"rewarded" : @"interstitial";
    [self event:@"failed" format:f];
}
- (void)didDisplayAd:(MAAd*)ad { if (!ad.format.isAdViewAd) [self event:@"opened" format:[self formatOf:ad]]; }
- (void)didHideAd:(MAAd*)ad { if (!ad.format.isAdViewAd) { [self event:@"closed" format:[self formatOf:ad]]; [self finishShow:YES error:nil]; } }
- (void)didClickAd:(MAAd*)ad {}
- (void)didFailToDisplayAd:(MAAd*)ad withError:(MAError*)error { [self finishShow:NO error:error.message]; }
// MARewardedAdDelegate
- (void)didRewardUserForAd:(MAAd*)ad withReward:(MAReward*)reward { _earned = YES; [self event:@"rewarded" format:@"rewarded"]; }
// MAAdViewAdDelegate
- (void)didExpandAd:(MAAd*)ad {}
- (void)didCollapseAd:(MAAd*)ad {}

- (void)banner:(BOOL)show top:(BOOL)top reply:(RTSPluginReply*)reply {
    if (!show) {
        [_banner stopAutoRefresh];
        [_banner removeFromSuperview];
        _banner = nil;
        RTSPluginEmit(@"applovin", @"app.banner", @{@"height": @0, @"position": top ? @"top" : @"bottom"});
        [reply ok:nil];
        return;
    }
    NSString* unit = [self unit:@"banner"];
    if (!unit.length) { [reply fail:@"applovin: no \"banner\" ad unit in game.json"]; return; }
    if (!_ready) { [reply fail:@"applovin: SDK not initialized yet"]; return; }
    if (!_banner) {
        UIViewController* vc = RTSPluginPresentingViewController();
        CGFloat width = CGRectGetWidth(UIEdgeInsetsInsetRect(vc.view.bounds, vc.view.safeAreaInsets));
        CGFloat height = [MAAdFormat.banner adaptiveSizeForWidth:width].height;
        _banner = [[MAAdView alloc] initWithAdUnitIdentifier:unit];
        _banner.delegate = self;
        _banner.backgroundColor = UIColor.blackColor;
        _bannerTop = top;
        _banner.translatesAutoresizingMaskIntoConstraints = NO;
        [vc.view addSubview:_banner];
        UILayoutGuide* g = vc.view.safeAreaLayoutGuide;
        [NSLayoutConstraint activateConstraints:@[
            [_banner.leadingAnchor constraintEqualToAnchor:g.leadingAnchor],
            [_banner.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],
            [_banner.heightAnchor constraintEqualToConstant:height],
            top ? [_banner.topAnchor constraintEqualToAnchor:g.topAnchor] : [_banner.bottomAnchor constraintEqualToAnchor:g.bottomAnchor]]];
        [_banner loadAd];
    }
    _banner.hidden = NO;
    [_banner startAutoRefresh];
    [reply ok:nil];
}

@end
