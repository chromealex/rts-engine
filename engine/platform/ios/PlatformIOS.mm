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
static int g_pointerButton = 0;

static uint8_t modsOf(UIKeyModifierFlags f) {
    return uint8_t(((f & UIKeyModifierShift) ? kModShift : 0) | ((f & UIKeyModifierControl) ? kModCtrl : 0) |
                   ((f & UIKeyModifierAlternate) ? kModAlt : 0) | ((f & UIKeyModifierCommand) ? kModSuper : 0));
}

// USB HID usage (UIKey.keyCode) -> engine key.
static Key keyFromHID(UIKeyboardHIDUsage u) {
    if (u >= UIKeyboardHIDUsageKeyboardA && u <= UIKeyboardHIDUsageKeyboardZ) return Key(int(Key::A) + int(u - UIKeyboardHIDUsageKeyboardA));
    if (u >= UIKeyboardHIDUsageKeyboard1 && u <= UIKeyboardHIDUsageKeyboard9) return Key(int(Key::Num1) + int(u - UIKeyboardHIDUsageKeyboard1));
    if (u >= UIKeyboardHIDUsageKeyboardF1 && u <= UIKeyboardHIDUsageKeyboardF12) return Key(int(Key::F1) + int(u - UIKeyboardHIDUsageKeyboardF1));
    switch (u) {
    case UIKeyboardHIDUsageKeyboard0: return Key::Num0;
    case UIKeyboardHIDUsageKeyboardReturnOrEnter: case UIKeyboardHIDUsageKeypadEnter: return Key::Enter;
    case UIKeyboardHIDUsageKeyboardEscape: return Key::Escape;
    case UIKeyboardHIDUsageKeyboardDeleteOrBackspace: return Key::Backspace;
    case UIKeyboardHIDUsageKeyboardDeleteForward: return Key::Delete;
    case UIKeyboardHIDUsageKeyboardTab: return Key::Tab;
    case UIKeyboardHIDUsageKeyboardSpacebar: return Key::Space;
    case UIKeyboardHIDUsageKeyboardInsert: return Key::Insert;
    case UIKeyboardHIDUsageKeyboardLeftArrow: return Key::Left;
    case UIKeyboardHIDUsageKeyboardRightArrow: return Key::Right;
    case UIKeyboardHIDUsageKeyboardUpArrow: return Key::Up;
    case UIKeyboardHIDUsageKeyboardDownArrow: return Key::Down;
    case UIKeyboardHIDUsageKeyboardHome: return Key::Home;
    case UIKeyboardHIDUsageKeyboardEnd: return Key::End;
    case UIKeyboardHIDUsageKeyboardPageUp: return Key::PageUp;
    case UIKeyboardHIDUsageKeyboardPageDown: return Key::PageDown;
    case UIKeyboardHIDUsageKeyboardLeftShift: return Key::LeftShift;
    case UIKeyboardHIDUsageKeyboardRightShift: return Key::RightShift;
    case UIKeyboardHIDUsageKeyboardLeftControl: return Key::LeftCtrl;
    case UIKeyboardHIDUsageKeyboardRightControl: return Key::RightCtrl;
    case UIKeyboardHIDUsageKeyboardLeftAlt: return Key::LeftAlt;
    case UIKeyboardHIDUsageKeyboardRightAlt: return Key::RightAlt;
    case UIKeyboardHIDUsageKeyboardLeftGUI: return Key::LeftSuper;
    case UIKeyboardHIDUsageKeyboardRightGUI: return Key::RightSuper;
    case UIKeyboardHIDUsageKeyboardHyphen: return Key::Minus;
    case UIKeyboardHIDUsageKeyboardEqualSign: return Key::Equal;
    case UIKeyboardHIDUsageKeyboardOpenBracket: return Key::LeftBracket;
    case UIKeyboardHIDUsageKeyboardCloseBracket: return Key::RightBracket;
    case UIKeyboardHIDUsageKeyboardSemicolon: return Key::Semicolon;
    case UIKeyboardHIDUsageKeyboardQuote: return Key::Apostrophe;
    case UIKeyboardHIDUsageKeyboardComma: return Key::Comma;
    case UIKeyboardHIDUsageKeyboardPeriod: return Key::Period;
    case UIKeyboardHIDUsageKeyboardSlash: return Key::Slash;
    case UIKeyboardHIDUsageKeyboardBackslash: return Key::Backslash;
    case UIKeyboardHIDUsageKeyboardGraveAccentAndTilde: return Key::GraveAccent;
    default: return Key::Unknown;
    }
}
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
    UIEdgeInsets in = self.safeAreaInsets;
    S.safe = {float(in.left), float(in.top), float(in.right), float(in.bottom)};
    Event e; e.type = EventType::Resized; detail::dispatch(e);
}
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    UIEdgeInsets in = self.safeAreaInsets;
    detail::setSafeArea({float(in.left), float(in.top), float(in.right), float(in.bottom)});
}
// Fingers are touches. A mouse or trackpad on iPad (UIApplicationSupportsIndirectInputEvents) is
// an "indirect pointer" touch: it becomes MouseDown/Move/Up with the real button, so right-click
// orders and box selection work like on desktop.
- (void)touches:(NSSet<UITouch*>*)touches type:(EventType)t event:(UIEvent*)ev {
    for (UITouch* touch in touches) {
        CGPoint p = [touch locationInView:self];
        Event e; e.x = float(p.x); e.y = float(p.y);
        if (touch.type == UITouchTypeIndirectPointer) {
            if (t == EventType::TouchBegin) g_pointerButton = (ev.buttonMask & UIEventButtonMaskSecondary) ? 1 : 0;
            e.button = g_pointerButton;
            e.mods = modsOf(ev.modifierFlags);
            e.type = t == EventType::TouchBegin ? EventType::MouseDown : t == EventType::TouchMove ? EventType::MouseMove : EventType::MouseUp;
        } else {
            e.type = t;
            e.touchId = (int64_t)(intptr_t)(__bridge void*)touch;
        }
        detail::dispatch(e);
    }
}
- (void)touchesBegan:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { [self touches:t type:EventType::TouchBegin event:e]; }
- (void)touchesMoved:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { [self touches:t type:EventType::TouchMove event:e]; }
- (void)touchesEnded:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { [self touches:t type:EventType::TouchEnd event:e]; }
- (void)touchesCancelled:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { [self touches:t type:EventType::TouchCancel event:e]; }
- (void)hover:(UIHoverGestureRecognizer*)g {
    CGPoint p = [g locationInView:self];
    Event e; e.type = EventType::MouseMove; e.x = float(p.x); e.y = float(p.y);
    detail::dispatch(e);
}
// Two-finger trackpad scroll / mouse wheel; allowedTouchTypes is empty so finger pans are untouched.
- (void)scroll:(UIPanGestureRecognizer*)g {
    CGPoint d = [g translationInView:self];
    [g setTranslation:CGPointZero inView:self];
    CGPoint p = [g locationInView:self];
    Event e; e.type = EventType::MouseWheel; e.x = float(p.x); e.y = float(p.y);
    e.wheelX = float(d.x) / 40.0f; e.wheelY = float(d.y) / 40.0f; // ~one notch per 40 pt, like the desktop backends
    if (e.wheelX != 0 || e.wheelY != 0) detail::dispatch(e);
}
@end

@interface RTSViewController : UIViewController
@property(strong) CADisplayLink* link;
@end
@implementation RTSViewController
- (void)loadView {
    RTSMetalView* v = [[RTSMetalView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    v.multipleTouchEnabled = YES;
    UIHoverGestureRecognizer* hover = [[UIHoverGestureRecognizer alloc] initWithTarget:v action:@selector(hover:)];
    [v addGestureRecognizer:hover];
    UIPanGestureRecognizer* scroll = [[UIPanGestureRecognizer alloc] initWithTarget:v action:@selector(scroll:)];
    scroll.allowedScrollTypesMask = UIScrollTypeMaskAll;
    scroll.allowedTouchTypes = @[];
    [v addGestureRecognizer:scroll];
    g_view = v;
    self.view = v;
}
// Hardware keyboard (iPad Magic Keyboard, Bluetooth). The controller is the first responder while
// no text field is active — it isn't a UIKeyInput, so it never brings up the on-screen keyboard.
- (BOOL)canBecomeFirstResponder { return YES; }
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; if (!state().textInput) [self becomeFirstResponder]; }
- (void)presses:(NSSet<UIPress*>*)presses down:(BOOL)down event:(UIPressesEvent*)ev {
    for (UIPress* p in presses) {
        UIKey* k = p.key;
        if (!k) continue;
        Event e; e.type = down ? EventType::KeyDown : EventType::KeyUp;
        e.key = keyFromHID(k.keyCode); e.mods = modsOf(k.modifierFlags);
        if (state().textInput) {
            // the text field (RTSMetalView/UIKeyInput) already received text, Backspace and Return
            if (e.key == Key::Backspace || e.key == Key::Enter || (e.key >= Key::A && e.key <= Key::Num9) || e.key == Key::Space) continue;
        }
        if (e.key != Key::Unknown) detail::dispatch(e);
        if (down && !state().textInput && !(k.modifierFlags & (UIKeyModifierCommand | UIKeyModifierControl))) {
            // printable characters only: arrows and function keys use private-use code points
            NSString* chars = k.characters;
            for (NSUInteger i = 0; i < chars.length; ++i) {
                unichar c = [chars characterAtIndex:i];
                if (c < 32 || c == 127 || (c >= 0xF700 && c <= 0xF8FF)) chars = nil;
                if (!chars) break;
            }
            if (chars.length) detail::dispatchText(chars.UTF8String);
        }
    }
}
- (void)pressesBegan:(NSSet<UIPress*>*)p withEvent:(UIPressesEvent*)e { [self presses:p down:YES event:e]; }
- (void)pressesEnded:(NSSet<UIPress*>*)p withEvent:(UIPressesEvent*)e { [self presses:p down:NO event:e]; }
- (void)pressesCancelled:(NSSet<UIPress*>*)p withEvent:(UIPressesEvent*)e { [self presses:p down:NO event:e]; }
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
        [v.window.rootViewController becomeFirstResponder]; // hardware keyboard keeps working
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
