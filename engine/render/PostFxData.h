// Post-processing effects as data: the "postfx" block of game.json (and of a map, which overrides
// it), the catalog of effects with their parameters, presets, and a frame-cost estimate per device
// class. GPU-free (links into the editor core, rtsmcp and tests); the renderer is PostFx.h.
//
// Format (game.json, the same block in a map overrides it):
//   "postfx": {
//     "enabled": true,                       // false switches the whole chain off
//     "replace": false,                      // map only: true ignores the game's effects
//     "effects": [
//       {"type": "bloom", "intensity": 0.8, "threshold": 0.7, "minQuality": "medium"},
//       {"type": "vignette", "intensity": 0.3},
//       {"type": "lut", "texture": "postfx/warm.png"}
//     ]
//   }
// One effect per type (a later one with the same type replaces an earlier one; a map effect replaces
// the game's effect of that type, "enabled": false on it switches that effect off for the map).
// Effects are applied in a fixed order (fxaa -> pixelate -> chromaticAberration -> sharpen ->
// tiltShift -> bloom -> sunShafts -> colorGrade -> lut -> posterize -> vignette -> grain -> fade ->
// letterbox), whatever the order in the list. Every effect has "enabled" and "minQuality"
// (low | medium | high: the effect is skipped on lower post-processing tiers; the default depends on
// how heavy the effect is). The tier is chosen per GPU (old Android: low), RTS_POSTFX=off|low|medium|high.
// The whole post chain costs nothing when no effect is active (the scene renders straight to the screen).
#pragma once
#include <string>
#include <vector>
#include "engine/data/Json.h"

namespace rts::render {

enum class PostFxQuality { Off = 0, Low = 1, Medium = 2, High = 3 };
enum class PostFxCostClass { Cheap, Medium, Heavy };

enum class PostFxType {
    Fxaa, Pixelate, ChromaticAberration, Sharpen, TiltShift, Bloom, SunShafts,
    ColorGrade, Lut, Posterize, Vignette, Grain, Fade, Letterbox, Count
};

enum class PostFxParamKind { Number, Color, Bool, Text };

struct PostFxParamDef {
    const char* name;
    PostFxParamKind kind;
    float def[3];          // Number/Bool: def[0]; Color: rgb
    float lo, hi;          // range (Number and every Color channel)
    const char* help;      // English, shown in MCP schemas
    const char* label;     // Russian, editor window
};

struct PostFxTypeDef {
    PostFxType type;
    const char* id;                 // "bloom"
    const char* label;              // Russian title for the editor
    const char* help;               // English one-liner (MCP)
    PostFxCostClass cost;
    PostFxQuality defaultMinQuality;
    std::vector<PostFxParamDef> params;
};

const std::vector<PostFxTypeDef>& postFxCatalog();
const PostFxTypeDef* postFxType(const std::string& id);
const PostFxTypeDef& postFxType(PostFxType t);
const char* postFxQualityName(PostFxQuality q);
bool parsePostFxQuality(const std::string& s, PostFxQuality& out);
const char* postFxCostName(PostFxCostClass c);

// One resolved effect. Values are indexed like the type's params; a Color takes 3 consecutive floats.
struct PostFxEffect {
    PostFxType type = PostFxType::ColorGrade;
    bool enabled = true;
    PostFxQuality minQuality = PostFxQuality::Low;
    std::vector<float> values;      // flattened params
    std::string text;               // Text param (lut texture path)
    float get(const char* name) const;
    const float* color(const char* name) const;   // 3 floats
};

struct PostFxSettings {
    bool enabled = true;
    std::vector<PostFxEffect> effects;   // at most one per type, catalog order
    const PostFxEffect* find(PostFxType t) const;
    // Effects that run on the given tier.
    std::vector<const PostFxEffect*> activeAt(PostFxQuality q) const;
    bool anyActive(PostFxQuality q) const { return !activeAt(q).empty(); }
};

// game.json block, then the map's block (either may be null). Problems (unknown type / parameter,
// out-of-range values are clamped) are appended to `issues` when given; the result is always usable.
PostFxSettings parsePostFx(const Json& game, const Json& map, std::vector<std::string>* issues = nullptr);
// One effect object -> effect. Throws std::runtime_error with a readable message in strict mode
// (MCP validation), otherwise reports to `issues`.
PostFxEffect parsePostFxEffect(const Json& e, bool strict, std::vector<std::string>* issues = nullptr);
// Effect -> JSON with only the values that differ from the defaults (plus "type").
Json postFxEffectJson(const PostFxEffect& e);
PostFxEffect defaultPostFxEffect(PostFxType t);

// ---- presets ("cinematic", "vivid", ...): a ready list of effects
struct PostFxPreset { const char* id; const char* label; const char* help; const char* effectsJson; };
const std::vector<PostFxPreset>& postFxPresets();
const PostFxPreset* postFxPreset(const std::string& id);

// ---- frame cost
// Work is counted in "units": one full-resolution full-screen texture read per pixel. Extra passes
// add a fixed cost each (render target switch: tile store/load on mobile GPUs). The same plans are
// used by the renderer (PostFx.cpp), so the estimate follows what is really drawn.
struct PostFxDeviceProfile {
    const char* id;          // weakPhone | midPhone | desktop
    const char* label;       // "weak phone (Mali-T720 / Adreno 306, 1280x720)"
    int width, height;       // typical render size
    float msPerMpixUnit;     // ms per megapixel per unit
    float msPerPass;
    PostFxQuality tier;      // the post-processing tier such a device gets by default
};
const std::vector<PostFxDeviceProfile>& postFxProfiles();

struct PostFxWork { float units = 0; int passes = 0; };
// Work of one effect at a tier (zero when the effect doesn't run there). `forceTier` ignores minQuality.
PostFxWork postFxEffectWork(const PostFxEffect& e, PostFxQuality tier, bool ignoreMinQuality = false);
PostFxWork postFxBaseWork();              // scene to texture + final composite (paid once when any effect runs)
float postFxMs(const PostFxWork& w, const PostFxDeviceProfile& p);

// Plans shared with the renderer.
struct BloomPlan { int startDiv = 2; int levels = 4; };          // first level = size / startDiv
BloomPlan bloomPlan(PostFxQuality tier);
struct BlurPlan { int div = 2; int passes = 2; };               // tilt-shift blur
BlurPlan tiltBlurPlan(PostFxQuality tier);
struct ShaftsPlan { int div = 4; int samples = 16; };
ShaftsPlan shaftsPlan(PostFxQuality tier);

// Cost report for MCP / the editor: per effect and in total for each device profile, plus warnings
// (heavy effects, effects forced onto weak devices, a chain over budget).
Json postFxCostReport(const PostFxSettings& s);
// Warnings for one effect only (MCP add / set): heavy, or heavy forced onto low tiers.
std::vector<std::string> postFxEffectWarnings(const PostFxEffect& e);

} // namespace rts::render
