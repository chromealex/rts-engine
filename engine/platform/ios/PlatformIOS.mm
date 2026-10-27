// iOS: UIKit + CAMetalLayer + CADisplayLink + Metal backend. Objective-C++ with ARC.
#include "../PlatformCommon.h"
#include "../../gfx/Gfx.h"

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <chrono>
#include <memory>

using namespace rts;
using namespace rts::platform;
using rts::platform::detail::state;

static int g_argc = 0;
static char** g_argv = nullptr;
static std::unique_ptr<gfx::Device> g_device;
static std::chrono::steady_clock::time_point g_last;

@interface RTSMetalView : UIView
@end
@implementation RTSMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
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
- (void)applicationWillResignActive:(UIApplication*)a { Event e; e.type = EventType::FocusLost; detail::dispatch(e); }
- (void)applicationDidBecomeActive:(UIApplication*)a { Event e; e.type = EventType::FocusGained; detail::dispatch(e); }
- (void)applicationWillTerminate:(UIApplication*)a { if (state().cb.shutdown) state().cb.shutdown(); g_device.reset(); }
@end

namespace rts::platform {

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
