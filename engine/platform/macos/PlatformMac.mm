// macOS: Cocoa window + CAMetalLayer + Metal backend. Objective-C++ with ARC.
#include "../PlatformCommon.h"
#include "../../gfx/Gfx.h"

#include <cstring>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include <chrono>
#include <memory>

using namespace rts;
using namespace rts::platform;
using rts::platform::detail::state;

static std::unique_ptr<gfx::Device> g_device;
static NSWindow* g_window = nil;
// Strong owner of the app delegate: NSApp.delegate is weak, and once the timer is invalidated
// nothing else retains it (a freed delegate crashed in -tick when the window closed).
static id g_delegate = nil;
static std::chrono::steady_clock::time_point g_last;

static uint8_t modsFrom(NSEventModifierFlags f) {
    uint8_t m = 0;
    if (f & NSEventModifierFlagShift) m |= kModShift;
    if (f & NSEventModifierFlagControl) m |= kModCtrl;
    if (f & NSEventModifierFlagOption) m |= kModAlt;
    if (f & NSEventModifierFlagCommand) m |= kModSuper;
    return m;
}

static Key mapKeyCode(unsigned short k) {
    // Carbon virtual key codes (kVK_*)
    static const struct { unsigned short code; Key key; } table[] = {
        {0x00, Key::A}, {0x0B, Key::B}, {0x08, Key::C}, {0x02, Key::D}, {0x0E, Key::E}, {0x03, Key::F}, {0x05, Key::G},
        {0x04, Key::H}, {0x22, Key::I}, {0x26, Key::J}, {0x28, Key::K}, {0x25, Key::L}, {0x2E, Key::M}, {0x2D, Key::N},
        {0x1F, Key::O}, {0x23, Key::P}, {0x0C, Key::Q}, {0x0F, Key::R}, {0x01, Key::S}, {0x11, Key::T}, {0x20, Key::U},
        {0x09, Key::V}, {0x0D, Key::W}, {0x07, Key::X}, {0x10, Key::Y}, {0x06, Key::Z},
        {0x1D, Key::Num0}, {0x12, Key::Num1}, {0x13, Key::Num2}, {0x14, Key::Num3}, {0x15, Key::Num4},
        {0x17, Key::Num5}, {0x16, Key::Num6}, {0x1A, Key::Num7}, {0x1C, Key::Num8}, {0x19, Key::Num9},
        {0x35, Key::Escape}, {0x24, Key::Enter}, {0x30, Key::Tab}, {0x33, Key::Backspace}, {0x75, Key::Delete},
        {0x31, Key::Space}, {0x7B, Key::Left}, {0x7C, Key::Right}, {0x7E, Key::Up}, {0x7D, Key::Down},
        {0x73, Key::Home}, {0x77, Key::End}, {0x74, Key::PageUp}, {0x79, Key::PageDown},
        {0x38, Key::LeftShift}, {0x3C, Key::RightShift}, {0x3B, Key::LeftCtrl}, {0x3E, Key::RightCtrl},
        {0x3A, Key::LeftAlt}, {0x3D, Key::RightAlt}, {0x37, Key::LeftSuper}, {0x36, Key::RightSuper},
        {0x7A, Key::F1}, {0x78, Key::F2}, {0x63, Key::F3}, {0x76, Key::F4}, {0x60, Key::F5}, {0x61, Key::F6},
        {0x62, Key::F7}, {0x64, Key::F8}, {0x65, Key::F9}, {0x6D, Key::F10}, {0x67, Key::F11}, {0x6F, Key::F12},
        {0x1B, Key::Minus}, {0x18, Key::Equal}, {0x21, Key::LeftBracket}, {0x1E, Key::RightBracket},
        {0x29, Key::Semicolon}, {0x27, Key::Apostrophe}, {0x2B, Key::Comma}, {0x2F, Key::Period},
        {0x2C, Key::Slash}, {0x2A, Key::Backslash}, {0x32, Key::GraveAccent},
    };
    for (auto& e : table) if (e.code == k) return e.key;
    return Key::Unknown;
}

static void updateSizes(NSView* view) {
    auto& S = state();
    NSRect b = view.bounds;
    CGFloat scale = view.window ? view.window.backingScaleFactor : 1.0;
    if (!S.cfg.highDpi) scale = 1.0;
    S.winW = float(b.size.width); S.winH = float(b.size.height);
    S.scale = float(scale);
    S.fbW = int(b.size.width * scale); S.fbH = int(b.size.height * scale);
    ((CAMetalLayer*)view.layer).contentsScale = scale;
}

@interface RTSView : NSView
@end

@implementation RTSView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
- (instancetype)initWithFrame:(NSRect)r {
    if ((self = [super initWithFrame:r])) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
        NSTrackingArea* ta = [[NSTrackingArea alloc] initWithRect:NSZeroRect
            options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil];
        [self addTrackingArea:ta];
    }
    return self;
}
- (void)viewDidChangeBackingProperties { updateSizes(self); }
- (void)setFrameSize:(NSSize)s {
    [super setFrameSize:s];
    updateSizes(self);
    Event e; e.type = EventType::Resized; detail::dispatch(e);
}
- (void)mouse:(NSEvent*)ev type:(EventType)t button:(int)b {
    NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];
    Event e; e.type = t; e.x = float(p.x); e.y = float(p.y); e.button = b; e.mods = modsFrom(ev.modifierFlags);
    detail::dispatch(e);
}
- (void)mouseDown:(NSEvent*)e { [self mouse:e type:EventType::MouseDown button:0]; }
- (void)mouseUp:(NSEvent*)e { [self mouse:e type:EventType::MouseUp button:0]; }
- (void)rightMouseDown:(NSEvent*)e { [self mouse:e type:EventType::MouseDown button:1]; }
- (void)rightMouseUp:(NSEvent*)e { [self mouse:e type:EventType::MouseUp button:1]; }
- (void)otherMouseDown:(NSEvent*)e { [self mouse:e type:EventType::MouseDown button:2]; }
- (void)otherMouseUp:(NSEvent*)e { [self mouse:e type:EventType::MouseUp button:2]; }
- (void)mouseMoved:(NSEvent*)e { [self mouse:e type:EventType::MouseMove button:0]; }
- (void)mouseDragged:(NSEvent*)e { [self mouse:e type:EventType::MouseMove button:0]; }
- (void)rightMouseDragged:(NSEvent*)e { [self mouse:e type:EventType::MouseMove button:1]; }
- (void)otherMouseDragged:(NSEvent*)e { [self mouse:e type:EventType::MouseMove button:2]; }
- (void)scrollWheel:(NSEvent*)ev {
    NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];
    Event e; e.type = EventType::MouseWheel; e.x = float(p.x); e.y = float(p.y);
    float k = ev.hasPreciseScrollingDeltas ? 0.1f : 1.0f;
    e.wheelX = float(ev.scrollingDeltaX) * k; e.wheelY = float(ev.scrollingDeltaY) * k;
    e.mods = modsFrom(ev.modifierFlags);
    detail::dispatch(e);
}
- (void)magnifyWithEvent:(NSEvent*)ev { // trackpad pinch -> wheel zoom
    NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];
    Event e; e.type = EventType::MouseWheel; e.x = float(p.x); e.y = float(p.y); e.wheelY = float(ev.magnification) * 10.0f;
    detail::dispatch(e);
}
- (void)keyDown:(NSEvent*)ev {
    Event e; e.type = EventType::KeyDown; e.key = mapKeyCode(ev.keyCode); e.mods = modsFrom(ev.modifierFlags); e.repeat = ev.isARepeat;
    detail::dispatch(e);
    if (ev.modifierFlags & NSEventModifierFlagCommand) return;
    NSString* chars = ev.characters;
    for (NSUInteger i = 0; i < chars.length; ++i) {
        unichar c = [chars characterAtIndex:i];
        if (c < 32 || c == 127 || (c >= 0xF700 && c <= 0xF8FF)) continue; // control / function keys
        Event ce; ce.type = EventType::Char; ce.codepoint = c; detail::dispatch(ce);
    }
}
- (void)keyUp:(NSEvent*)ev {
    Event e; e.type = EventType::KeyUp; e.key = mapKeyCode(ev.keyCode); e.mods = modsFrom(ev.modifierFlags);
    detail::dispatch(e);
}
- (void)flagsChanged:(NSEvent*)ev {
    Key k = mapKeyCode(ev.keyCode);
    if (k == Key::Unknown) return;
    NSEventModifierFlags f = ev.modifierFlags;
    bool down = false;
    switch (k) {
        case Key::LeftShift: case Key::RightShift: down = f & NSEventModifierFlagShift; break;
        case Key::LeftCtrl: case Key::RightCtrl: down = f & NSEventModifierFlagControl; break;
        case Key::LeftAlt: case Key::RightAlt: down = f & NSEventModifierFlagOption; break;
        case Key::LeftSuper: case Key::RightSuper: down = f & NSEventModifierFlagCommand; break;
        default: return;
    }
    Event e; e.type = down ? EventType::KeyDown : EventType::KeyUp; e.key = k; e.mods = modsFrom(f);
    detail::dispatch(e);
}
@end

@interface RTSAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) RTSView* view;
@property(strong) NSTimer* timer;
@end

@implementation RTSAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    auto& S = state();
    NSRect frame = NSMakeRect(0, 0, S.cfg.width, S.cfg.height);
    g_window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    g_window.title = [NSString stringWithUTF8String:S.cfg.title.c_str()];
    g_window.delegate = self;
    g_window.releasedWhenClosed = NO; // we keep using it until terminate (no ARC here)
    g_window.acceptsMouseMovedEvents = YES;
    // shown until the first frame is presented (games: the splash background, see AppConfig::clearColor)
    g_window.backgroundColor = [NSColor colorWithSRGBRed:S.cfg.clearColor[0] green:S.cfg.clearColor[1] blue:S.cfg.clearColor[2] alpha:1];
    self.view = [[RTSView alloc] initWithFrame:frame];
    g_window.contentView = self.view;
    [g_window center];
    updateSizes(self.view);
    g_device = gfx::createMetalDevice((__bridge void*)self.view.layer);
    if (S.cb.init) S.cb.init(*g_device);   // before the window becomes key: no events reach an uninitialised app
    [g_window makeKeyAndOrderFront:nil];
    [g_window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];
    updateSizes(self.view);
    g_last = std::chrono::steady_clock::now();
    self.timer = [NSTimer timerWithTimeInterval:1.0 / 120.0 target:self selector:@selector(tick) userInfo:nil repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSRunLoopCommonModes];
}
- (void)tick {
    auto& S = state();
    if (S.quit || !g_window) { [self.timer invalidate]; [NSApp terminate:nil]; return; }
    if (S.fbW <= 0 || S.fbH <= 0 || g_window.isMiniaturized || !g_window.visible) return;
    @autoreleasepool {
        // RTS_SCREENSHOT / RTS_EXIT_AFTER_FRAMES (same hooks as Linux/Win32): fixed 1/60 s steps,
        // capture frame N through the device, write it and quit once the GPU delivered it.
        static const detail::CaptureRequest capture = detail::captureRequestFromEnv();
        static long frameNo = 0;
        static bool armed = false;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - g_last).count();
        if (dt > rts::platform::detail::FrameClock::kMaxDt) dt = rts::platform::detail::FrameClock::kMaxDt; // after sleep/background
        rts::platform::pollContent(); // pack chunk downloads
        if (capture.active()) dt = 1.0 / 60.0;
        g_last = now;
        if (capture.active() && !armed && ++frameNo >= capture.frames) {
            armed = true;
            std::string path = capture.path;
            g_device->captureNextFrame([path](int w, int h, std::vector<uint8_t> px) {
                if (!path.empty() && w > 0 && !px.empty()) {
                    std::vector<uint8_t> flipped(px.size()); // capture is top-down, writePPM wants bottom-up
                    size_t row = size_t(w) * 4;
                    for (int y = 0; y < h; ++y) std::memcpy(&flipped[size_t(h - 1 - y) * row], &px[size_t(y) * row], row);
                    detail::writePPM(path, w, h, flipped.data());
                }
                state().quit = true;
            });
        }
        g_device->beginFrame(S.fbW, S.fbH);
        if (S.cb.frame) S.cb.frame(*g_device, dt);
        g_device->endFrame();
    }
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { return YES; }
- (BOOL)windowShouldClose:(NSWindow*)w {
    Event e; e.type = EventType::QuitRequested; detail::dispatch(e);
    return YES;
}
- (void)windowWillClose:(NSNotification*)n {
    // Stop rendering before the layer goes away; terminate follows (last window closed).
    [self.timer invalidate];
    state().quit = true;
}
// Fullscreen on notched MacBooks: the menu-bar strip around the camera is not usable.
static void updateMacSafeArea() {
    NSEdgeInsets in = g_window.contentView.safeAreaInsets;
    detail::setSafeArea({float(in.left), float(in.top), float(in.right), float(in.bottom)});
}
- (void)windowDidEnterFullScreen:(NSNotification*)n { state().fullscreen = true; updateMacSafeArea(); }
- (void)windowDidExitFullScreen:(NSNotification*)n { state().fullscreen = false; updateMacSafeArea(); }
- (void)windowDidBecomeKey:(NSNotification*)n { Event e; e.type = EventType::FocusGained; detail::dispatch(e); }
- (void)windowDidResignKey:(NSNotification*)n { Event e; e.type = EventType::FocusLost; detail::dispatch(e); }
- (void)applicationWillTerminate:(NSNotification*)n {
    [self.timer invalidate];
    static bool done = false; // terminate may arrive twice (window close + menu)
    if (done) return;
    done = true;
    if (state().cb.shutdown) state().cb.shutdown();
    g_device.reset();
}
@end

namespace rts::platform {

namespace detail {
void applyFullscreen(bool on) {
    if (!g_window) return;
    bool now = (g_window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (on != now) [g_window toggleFullScreen:nil]; // native Spaces fullscreen; delegate updates state
}
} // namespace detail

const char* name() { return "macos"; }
void setTitle(const std::string& t) { if (g_window) g_window.title = [NSString stringWithUTF8String:t.c_str()]; }
std::string clipboardText() {
    NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    return s ? std::string(s.UTF8String) : std::string();
}
void setClipboardText(const std::string& s) {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:[NSString stringWithUTF8String:s.c_str()] forType:NSPasteboardTypeString];
}

int run(const AppConfig& cfg, AppCallbacks cb) {
    auto& S = state();
    S.cfg = cfg; S.cb = std::move(cb);
    @autoreleasepool {
        // content: env override -> app bundle Resources/content -> ./content
        NSString* res = [[NSBundle mainBundle] resourcePath];
        NSString* bundled = [res stringByAppendingPathComponent:@"content"];
        if (const char* env = getenv("RTS_CONTENT")) S.contentRoot = env;
        else if ([[NSFileManager defaultManager] fileExistsAtPath:bundled]) S.contentRoot = bundled.UTF8String;
        else S.contentRoot = "content";
        NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
        if (dirs.count) S.userDir = std::string([dirs[0] UTF8String]) + "/" + cfg.title;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        // Minimal menu so Cmd+Q works.
        NSMenu* bar = [NSMenu new];
        NSMenuItem* appItem = [NSMenuItem new];
        [bar addItem:appItem];
        NSMenu* appMenu = [NSMenu new];
        [appMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"]];
        appItem.submenu = appMenu;
        NSApp.mainMenu = bar;
        RTSAppDelegate* del = [RTSAppDelegate new];
        g_delegate = del;
        NSApp.delegate = del;
        [NSApp run];
    }
    return 0;
}

} // namespace rts::platform

int main(int argc, char** argv) { return rtsMain(argc, argv); }
