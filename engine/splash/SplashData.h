// Splash / loading screen: the game.json "splash" block and its timeline (CPU only, no GPU).
// The client draws it through SplashScreen (engine/splash/SplashScreen.h); rtsbuild and the editor
// use the same evaluation for previews and for the native launch screens. Format: docs/game-format.md
// "Icons and splash".
//
//   "splash": {
//     "background": "#101418",
//     "image": "branding/splash.png", "fit": "cover", "scale": 1,          // base layer (optional)
//     "logos": [                                                             // stacked layers, in order
//       {"image": "branding/studio.png", "start": 0, "duration": 2.2, "scale": 0.35,
//        "animation": {"fadeIn": 0.4, "fadeOut": 0.4, "pulse": {"amount": 0.04, "period": 1.6}}},
//       {"sheet": "branding/logo_sheet.png", "frame": [256, 256], "count": 8, "fps": 12, "start": 2.0,
//        "animation": [{"t": 0, "opacity": 0, "scale": 0.8}, {"t": 0.5, "opacity": 1, "scale": 1, "ease": "out"}]}],
//     "minSeconds": 3, "maxSeconds": 6, "loop": false, "skippable": true, "fadeOut": 0.4,
//     "progress": {"show": true, "color": "#e0b040", "track": "#ffffff30", "position": "bottom", "width": 0.4, "height": 6}
//   }
//
// Times are seconds from the start of the splash; sizes are fractions of the screen (fit) or of the
// image's own size (fit "center"); positions are fractions of the screen.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "engine/data/Json.h"

namespace rts::splash {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    uint32_t rgba8() const;   // 0xAABBGGRR (little-endian RGBA bytes)
};
// "#rgb", "#rgba", "#rrggbb", "#rrggbbaa" or [r, g, b(, a)] in 0..1. False on anything else.
bool parseColor(const Json& j, Color& out);
bool parseColor(const std::string& s, Color& out);
std::string colorHex(const Color& c, bool withAlpha = false);   // "#rrggbb" / "#rrggbbaa"

enum class Fit { Contain, Cover, Center, Stretch };
enum class Ease { Linear, In, Out, InOut };

// A keyframe sets any of these properties; missing ones are interpolated from the neighbours that
// set them (a property no keyframe sets keeps its default).
struct Keyframe {
    float t = 0;
    float value[5] = {1, 1, 0, 0, 0};   // opacity, scale, x, y (screen fractions), rotate (degrees)
    bool has[5] = {false, false, false, false, false};
    Ease ease = Ease::Linear;           // easing of the segment that ends at this key
};
enum Prop { kOpacity = 0, kScale, kX, kY, kRotate, kPropCount };

// Shorthand effects, combined with the keyframes (multiply opacity/scale, add offsets/rotation).
struct Effects {
    float fadeIn = 0, fadeOut = 0;          // seconds at the start / end of the layer's duration
    float pulseAmount = 0, pulsePeriod = 1; // scale *= 1 + amount * sin(2 pi t / period)
    float rotateSpeed = 0;                  // degrees per second
    float slideFromX = 0, slideFromY = 0, slideTime = 0; // starts offset by (x, y), eases out to 0
    float zoomFrom = 1, zoomTime = 0;       // starts at scale zoomFrom, eases out to 1
};

struct Layer {
    std::vector<std::string> images;    // one = static; several = animation frames ("frames")
    std::string sheet;                  // sprite sheet (frames in rows, left to right)
    int frameW = 0, frameH = 0, frameCount = 0;
    float fps = 12;
    bool loopFrames = true;
    Fit fit = Fit::Contain;
    float scale = 1;                    // contain/cover/stretch: of the fitted size; center: of the image size
    float anchorX = 0.5f, anchorY = 0.5f; // centre of the layer on screen (fractions)
    float start = 0, duration = 0;      // seconds; duration 0 = until the splash ends
    std::vector<Keyframe> keys;
    Effects fx;
    bool hasImage() const { return !images.empty() || !sheet.empty(); }
    int frames() const;                 // number of animation frames
};

struct ProgressStyle {
    bool show = true;
    Color color{0.88f, 0.69f, 0.25f, 1}, track{1, 1, 1, 0.18f};
    std::string position = "bottom";    // bottom | top | center
    float width = 0.4f;                 // fraction of the screen width
    float height = 6;                   // logical points
    float offset = 0.08f;               // fraction of the screen height from the edge (bottom/top)
};

struct Spec {
    bool enabled = false;               // false: no "splash" block (the client shows the background only while loading)
    Color background{0.06f, 0.07f, 0.08f, 1};
    std::vector<Layer> layers;          // base image first, then logos in order
    float minSeconds = -1;              // -1: until the logo sequence has played once
    float maxSeconds = 0;               // 0: no cap on the minimum
    bool loop = false;                  // repeat the logo sequence while loading takes longer
    bool skippable = true;              // a tap / click / key ends the minimum time
    bool waitForContent = false;        // split builds: download every pack chunk before leaving
    float fadeOut = 0.4f;               // crossfade to the menu, seconds
    ProgressStyle progress;

    float sequenceLength() const;       // end of the last finite layer (0 when every layer is endless)
    float minimumTime() const;          // what the splash waits for once loading is done
    std::vector<std::string> images() const;   // every image file it uses (base pack, validation)
};

// Parses game.json["splash"]. Throws std::runtime_error with a readable message ("splash.logos[1]:
// unknown fit 'x'"); warnings get non-fatal notes.
Spec parseSpec(const Json& splash, std::vector<std::string>* warnings = nullptr);

// One quad to draw: centre and size in pixels of the target, frame of the layer, rotation, opacity.
struct DrawItem {
    int layer = 0, frame = 0;
    float cx = 0, cy = 0, w = 0, h = 0;
    float rotation = 0;   // radians, clockwise on screen
    float opacity = 1;
};
struct ImageSize { int w = 0, h = 0; };   // of ONE frame of each layer (0 = not loaded: skipped)

// Timeline at time t (seconds on the splash timeline) on a W x H pixel target. `pixelScale`
// converts image pixels for fit "center" (pass the dpi scale).
std::vector<DrawItem> evaluate(const Spec& s, const std::vector<ImageSize>& sizes, float t, float W, float H, float pixelScale = 1);
// Advances the timeline by dt; with "loop" it wraps to the start of the logo sequence while
// loading is still running (the real elapsed time is what readyToLeave looks at).
float advanceTimeline(const Spec& s, float timeline, float dt, bool stillLoading);
// Progress bar in pixels (track rectangle); false when hidden.
bool progressRect(const Spec& s, float W, float H, float pixelScale, float& x, float& y, float& w, float& h);

// Layer properties at local time (seconds since layer start); exposed for tests.
void layerState(const Layer& l, float local, float out[kPropCount]);

// When the client may leave the splash: loading done, minimum time reached (or skipped).
bool readyToLeave(const Spec& s, float elapsed, bool loaded, bool skipped);

} // namespace rts::splash
