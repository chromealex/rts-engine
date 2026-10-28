// Sprite / tile shaders (SpriteBatch), GLSL ES 3.00 bodies and MSL, same layout as Shaders.h.
//
//   vertex: pos (float3), uv (float2), color (ubyte4n, multiplies), team (ubyte4n: rgb team colour,
//           a = strength of the team remap)
//   U0:     mat4 viewProj; vec4 params (x alpha cutoff)
//   tex0:   RGBA sheet / atlas
//   U2 + tex1: dynamic lights (LightShaders.h, bound by the 2D view): day/night tint and the light
//           map at the sprite's world position. Unbound: sprites are drawn as before.
//
// Team colour: pixels painted in shades of magenta (r == b, g low) are replaced by the team colour
// times their brightness, like the remap ranges of classic palette games. One texture fetch, no
// branches besides the alpha test: cheap on weak mobile GPUs.
//
// Lit variant (kSpriteLit*, games with "sprites": {"lit": true}): the same colour times the light
// at the sprite relative to a fixed daylight reference, so a sprite in full default sun keeps its
// painted colours and darkens in shadows, at night or tints with point lights like the terrain.
//   vertex: + rpos (float4): xyz receive point (on the upright shadow caster quad, or the ground
//           for decals and tiles), w > 0: upright (offset towards the sun by w + filter size)
//   U0:     mat4 viewProj; vec4 params (x alpha cutoff, y receive sun shadows); vec4 ref (rgb ref light)
//   U1 / U2 / tex0 / tex1: as the lit shader (Renderer3D::bindLitFrame); tex2: sheet
// Shadow caster (kSpriteCaster*): alpha-tested quads into the sun shadow map (depth only).
#pragma once
#include "Shaders.h"

namespace rts::render::shaders {

inline const char* kSpriteVS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; vec4 u_params; };
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec4 a_team;
out vec2 v_uv;
out vec4 v_col;
out vec4 v_team;
out vec3 v_wpos;
void main() { v_uv = a_uv; v_col = a_col; v_team = a_team; v_wpos = a_pos; gl_Position = u_viewProj * vec4(a_pos, 1.0); }
)";

inline const char* kSpriteFS = RTS_GLSL_LIGHTS_AT("U2", "tex1") R"(
layout(std140) uniform U0 { mat4 u_viewProj; vec4 u_params; };
uniform sampler2D tex0;
in vec2 v_uv;
in vec4 v_col;
in vec4 v_team;
in vec3 v_wpos;
out vec4 o_color;
void main() {
    vec4 t = texture(tex0, v_uv);
    if (t.a < u_params.x) discard;
    float k = clamp((min(t.r, t.b) - t.g) * 4.0, 0.0, 1.0) * clamp(1.0 - abs(t.r - t.b) * 4.0, 0.0, 1.0) * v_team.a;
    vec3 c = mix(t.rgb, v_team.rgb * max(t.r, t.b), k);
    c *= unlitTint() + mapLight(v_wpos) * 0.8;   // lights (LightShaders.h)
    o_color = vec4(c, t.a) * v_col;
}
)";

inline const char* kSpriteMSL = "#include <metal_stdlib>\nusing namespace metal;\n" RTS_MSL_LIGHTS R"(
struct U0 { float4x4 viewProj; float4 params; };
struct VIn { float3 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; float4 team [[attribute(3)]]; };
struct VOut { float4 pos [[position]]; float2 uv; float4 col; float4 team; float3 wpos; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) {
    VOut o; o.pos = u.viewProj * float4(in.pos, 1.0); o.uv = in.uv; o.col = in.col; o.team = in.team; o.wpos = in.pos; return o;
}
fragment float4 fs_main(VOut in [[stage_in]], constant U0& u [[buffer(1)]],
                        texture2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],
                        constant LightU& l [[buffer(3)]], texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]]) {
    float4 t = tex0.sample(s0, in.uv);
    if (t.a < u.params.x) discard_fragment();
    float k = clamp((min(t.r, t.b) - t.g) * 4.0, 0.0, 1.0) * clamp(1.0 - abs(t.r - t.b) * 4.0, 0.0, 1.0) * in.team.a;
    float3 c = mix(t.rgb, in.team.rgb * max(t.r, t.b), k);
    c *= unlitTint(l) + mapLight(l, tex1, s1, in.wpos) * 0.8;
    return float4(c, t.a) * in.col;
}
)";

#define RTS_GLSL_SPRITE_LIT_U0 "layout(std140) uniform U0 { mat4 u_spriteVP; vec4 u_sp; vec4 u_ref; };\n"

inline const char* kSpriteLitVS =
    RTS_GLSL_SPRITE_LIT_U0
    R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec4 a_team;
layout(location = 4) in vec4 a_rpos;
out vec2 v_uv;
out vec4 v_col;
out vec4 v_team;
out vec4 v_rpos;
void main() { v_uv = a_uv; v_col = a_col; v_team = a_team; v_rpos = a_rpos; gl_Position = u_spriteVP * vec4(a_pos, 1.0); }
)";

inline const char* kSpriteLitFS =
    RTS_GLSL_SPRITE_LIT_U0 RTS_GLSL_FRAME RTS_GLSL_SHADOW RTS_GLSL_LIGHTS
    R"(
uniform sampler2D tex2;
in vec2 v_uv;
in vec4 v_col;
in vec4 v_team;
in vec4 v_rpos;
out vec4 o_color;
void main() {
    vec4 t = texture(tex2, v_uv);
    if (t.a < u_sp.x) discard;
    float k = clamp((min(t.r, t.b) - t.g) * 4.0, 0.0, 1.0) * clamp(1.0 - abs(t.r - t.b) * 4.0, 0.0, 1.0) * v_team.a;
    vec3 c = mix(t.rgb, v_team.rgb * max(t.r, t.b), k);
    vec3 L = normalize(u_sunDir.xyz);
    bool upright = v_rpos.w > 0.0;
    // upright sprites: receive at their caster quad, moved towards the sun past the filter footprint
    // (never shadowed by their own caster); ground decals / tiles: like the terrain
    vec3 n = upright ? L : vec3(0.0, 1.0, 0.0);
    vec3 p = v_rpos.xyz + (upright ? L * (v_rpos.w + u_quality.w * 3.0) : vec3(0.0));
    float sh = u_sp.y > 0.5 ? shadowAt(p, n) : 1.0;
    sh *= cloudShade(p);
    vec3 sun = u_sunColor.rgb * u_sunColor.rgb * u_sunDir.w;
    vec3 light = u_skyColor.rgb * u_skyColor.rgb + sun * (max(L.y, 0.0) * sh);
    light += dynamicLight(p, vec3(0.0, 1.0, 0.0), normalize(u_camPos.xyz - p), 0.0);
    c *= min(sqrt(light / u_ref.rgb), vec3(1.6));   // linear ratio -> display (~sqrt)
    o_color = vec4(c, t.a) * v_col;
}
)";

inline const char* kSpriteLitMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct SU0 { float4x4 viewProj; float4 sp; float4 ref; };\n"
    RTS_MSL_FRAME RTS_MSL_SHADOW RTS_MSL_LIGHTS
    R"(
struct VIn { float3 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; float4 team [[attribute(3)]]; float4 rpos [[attribute(4)]]; };
struct VOut { float4 pos [[position]]; float2 uv; float4 col; float4 team; float4 rpos; };
vertex VOut vs_main(VIn in [[stage_in]], constant SU0& u [[buffer(1)]]) {
    VOut o; o.pos = u.viewProj * float4(in.pos, 1.0); o.uv = in.uv; o.col = in.col; o.team = in.team; o.rpos = in.rpos; return o;
}
fragment float4 fs_main(VOut in [[stage_in]], constant SU0& u [[buffer(1)]], constant U1& f [[buffer(2)]],
                        depth2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],
                        constant LightU& l [[buffer(3)]], texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]],
                        texture2d<float> tex2 [[texture(2)]], sampler s2 [[sampler(2)]]) {
    float4 t = tex2.sample(s2, in.uv);
    if (t.a < u.sp.x) discard_fragment();
    float k = clamp((min(t.r, t.b) - t.g) * 4.0, 0.0, 1.0) * clamp(1.0 - abs(t.r - t.b) * 4.0, 0.0, 1.0) * in.team.a;
    float3 c = mix(t.rgb, in.team.rgb * max(t.r, t.b), k);
    float3 L = normalize(f.sunDir.xyz);
    bool upright = in.rpos.w > 0.0;
    float3 n = upright ? L : float3(0.0, 1.0, 0.0);
    float3 p = in.rpos.xyz + (upright ? L * (in.rpos.w + f.quality.w * 3.0) : float3(0.0));
    float sh = u.sp.y > 0.5 ? shadowAt(f, p, n, in.pos.xy, tex0, s0) : 1.0;
    sh *= cloudShade(l, p);
    float3 sun = f.sunColor.rgb * f.sunColor.rgb * f.sunDir.w;
    float3 light = f.skyColor.rgb * f.skyColor.rgb + sun * (max(L.y, 0.0) * sh);
    light += dynamicLight(l, tex1, s1, p, float3(0.0, 1.0, 0.0), normalize(f.camPos.xyz - p), 0.0);
    c *= min(sqrt(light / u.ref.rgb), float3(1.6));
    return float4(c, t.a) * in.col;
}
)";

// Shadow caster: depth only, alpha-tested against the sheet (same vertex layout as the sprites).
inline const char* kSpriteCasterVS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; vec4 u_params; };
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main() { v_uv = a_uv; gl_Position = u_viewProj * vec4(a_pos, 1.0); }
)";

inline const char* kSpriteCasterFS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; vec4 u_params; };
uniform sampler2D tex0;
in vec2 v_uv;
void main() { if (texture(tex0, v_uv).a < u_params.x) discard; }
)";

inline const char* kSpriteCasterMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct U0 { float4x4 viewProj; float4 params; };
struct VIn { float3 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) {
    VOut o; o.pos = u.viewProj * float4(in.pos, 1.0); o.uv = in.uv; return o;
}
fragment void fs_main(VOut in [[stage_in]], constant U0& u [[buffer(1)]], texture2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    if (tex0.sample(s0, in.uv).a < u.params.x) discard_fragment();
}
)";

} // namespace rts::render::shaders
