// iOS: UIKit + CAMetalLayer + CADisplayLink + Metal backend. Objective-C++ with ARC.
#include "../PlatformCommon.h"
#include "../../gfx/Gfx.h"

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <chrono>
#include <memory>

using namespace rts;
using namespace rts::platform;
using rts::platform::detail::state;

static int g_argc = 0;
static char** g_argv = nullptr;
static std::unique_ptr<gfx::Device> g_device;
static std::chrono::steady_clock::time_point g_last;

// The Metal view doubles as the text input client: becoming first responder shows the keyboard,
// UIKeyInput delivers typed text (any language) and Backspace.
@interface RTSMetalView : UIView <UIKeyInput>
@property(nonatomic) UITextAutocorrectionType autocorrectionType;
@property(nonatomic) UITextAutocapitalizationType autocapitalizationType;
@property(nonatomic) UITextSpellCheckingType spellCheckingType;
@property(nonatomic) UITextSmartQuotesType smartQuotesType;
@property(nonatomic) UITextSmartDashesType smartDashesType;
@property(nonatomic) UIReturnKeyType returnKeyType;
@property(nonatomic) UIKeyboardType keyboardType;
@end
static __weak RTSMetalView* g_view = nil;
static bool g_closingKeyboard = false; // resignFirstResponder initiated by stopTextInput()

@implementation RTSMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        // game text (names, chat): no autocorrect surprises, "Done" closes the keyboard
        _autocorrectionType = UITextAutocorrectionTypeNo;
        _autocapitalizationType = UITextAutocapitalizationTypeNone;
        _spellCheckingType = UITextSpellCheckingTypeNo;
        _smartQuotesType = UITextSmartQuotesTypeNo;
        _smartDashesType = UITextSmartDashesTypeNo;
        _returnKeyType = UIReturnKeyDone;
        _keyboardType = UIKeyboardTypeDefault;
        NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
        [nc addObserver:self selector:@selector(keyboardFrame:) name:UIKeyboardWillChangeFrameNotification object:nil];
        [nc addObserver:self selector:@selector(keyboardHide:) name:UIKeyboardWillHideNotification object:nil];
    }
    return self;
}
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (BOOL)canBecomeFirstResponder { return state().textInput; }
- (BOOL)hasText { return YES; } // keep Backspace enabled: the text lives in the game UI, not here
- (void)insertText:(NSString*)text { detail::dispatchText(text.UTF8String ? text.UTF8String : ""); }
- (void)deleteBackward { detail::dispatchKeyPress(Key::Backspace); }
- (BOOL)resignFirstResponder {
    BOOL r = [super resignFirstResponder];
    if (r && !g_closingKeyboard) detail::textInputEndedByUser(); // iPad "hide keyboard" key
    return r;
}
- (void)keyboardFrame:(NSNotification*)n {
    CGRect screenFrame = [n.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    UIScreen* screen = self.window ? self.window.screen : UIScreen.mainScreen;
    CGRect f = [self convertRect:screenFrame fromCoordinateSpace:screen.coordinateSpace];
    detail::setKeyboardHeight(float(std::max<CGFloat>(0, CGRectGetMaxY(self.bounds) - CGRectGetMinY(f))));
}
- (void)keyboardHide:(NSNotification*)n { detail::setKeyboardHeight(0); }
- (void)layoutSubviews {
    [super layoutSubviews];
    auto& S = state();
    CGFloat scale = self.window ? self.window.screen.nativeScale : UIScreen.mainScreen.nativeScale;
    self.contentScaleFactor = scale;
    ((CAMetalLayer*)self.layer).contentsScale = scale;
    S.winW = float(self.bounds.size.width); S.winH = float(self.bounds.size.height);
    S.scale = float(scale);
    S.fbW = int(self.bounds.size.width * scale); S.fbH = int(self.bounds.size.height * scale);
    Event e; e.type = EventType::Resized; detail::dispatch(e);
}
- (void)touches:(NSSet<UITouch*>*)touches type:(EventType)t {
    for (UITouch* touch in touches) {
        CGPoint p = [touch locationInView:self];
        Event e; e.type = t; e.x = float(p.x); e.y = float(p.y);
        e.touchId = (int64_t)(intptr_t)(__bridge void*)touch;
        detail::dispatch(e);
    }
}
- (void)touchesBegan:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { [self touches:t type:EventType::TouchBegin]; }
- (void)touchesMoved:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { [self touches:t type:EventType::TouchMove]; }
- (void)touchesEnded:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { [self touches:t type:EventType::TouchEnd]; }
- (void)touchesCancelled:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { [self touches:t type:EventType::TouchCancel]; }
@end

@interface RTSViewController : UIViewController
@property(strong) CADisplayLink* link;
@end
@implementation RTSViewController
- (void)loadView {
    RTSMetalView* v = [[RTSMetalView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    v.multipleTouchEnabled = YES;
    g_view = v;
    self.view = v;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    [self.view layoutIfNeeded];
    g_device = gfx::createMetalDevice((__bridge void*)self.view.layer);
    if (state().cb.init) state().cb.init(*g_device);
    g_last = std::chrono::steady_clock::now();
    self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick)];
    self.link.preferredFrameRateRange = CAFrameRateRangeMake(30, 120, 60);
    [self.link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (void)tick {
    auto& S = state();
    if (S.fbW <= 0 || S.fbH <= 0) return;
    @autoreleasepool {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - g_last).count();
        if (dt > rts::platform::detail::FrameClock::kMaxDt) dt = rts::platform::detail::FrameClock::kMaxDt; // after sleep/background
        g_last = now;
        g_device->beginFrame(S.fbW, S.fbH);
        if (S.cb.frame) S.cb.frame(*g_device, dt);
        g_device->endFrame();
    }
}
@end

@interface RTSAppDelegate : UIResponder <UIApplicationDelegate>
@property(strong) UIWindow* window;
@end
@implementation RTSAppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [RTSViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
// iOS kills apps that submit GPU work in the background, so the display link stops while inactive.
- (void)applicationWillResignActive:(UIApplication*)a {
    ((RTSViewController*)self.window.rootViewController).link.paused = YES;
    Event e; e.type = EventType::FocusLost; detail::dispatch(e);
}
- (void)applicationDidBecomeActive:(UIApplication*)a {
    g_last = std::chrono::steady_clock::now(); // no catch-up for the time spent in background
    ((RTSViewController*)self.window.rootViewController).link.paused = NO;
    Event e; e.type = EventType::FocusGained; detail::dispatch(e);
}
- (void)applicationWillTerminate:(UIApplication*)a { if (state().cb.shutdown) state().cb.shutdown(); g_device.reset(); }
@end

namespace rts::platform {

namespace detail {
void showSoftKeyboard(bool show, const TextInputArea&) {
    RTSMetalView* v = g_view;
    if (!v) return;
    if (show) {
        if (!v.isFirstResponder) [v becomeFirstResponder];
    } else if (v.isFirstResponder) {
        g_closingKeyboard = true;
        [v resignFirstResponder];
        g_closingKeyboard = false;
    }
}
} // namespace detail

const char* name() { return "ios"; }
void setTitle(const std::string&) {}
std::string clipboardText() {
    NSString* s = UIPasteboard.generalPasteboard.string;
    return s ? std::string(s.UTF8String) : std::string();
}
void setClipboardText(const std::string& s) { UIPasteboard.generalPasteboard.string = [NSString stringWithUTF8String:s.c_str()]; }

int run(const AppConfig& cfg, AppCallbacks cb) {
    auto& S = state();
    S.cfg = cfg; S.cb = std::move(cb);
    S.touch = true;
    @autoreleasepool {
        S.contentRoot = std::string([[NSBundle mainBundle] resourcePath].UTF8String) + "/content";
        NSArray* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        if (docs.count) S.userDir = [docs[0] UTF8String];
        return UIApplicationMain(g_argc, g_argv, nil, NSStringFromClass([RTSAppDelegate class]));
    }
}

} // namespace rts::platform

int main(int argc, char** argv) { g_argc = argc; g_argv = argv; return rtsMain(argc, argv); }
