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
#pragma once
#include "LightShaders.h"

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

} // namespace rts::render::shaders
