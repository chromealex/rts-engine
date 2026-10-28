// Shader of the ground overlays (RtsGroundFx): the lit shader of Shaders.h (sun + soft shadows
// RTS_*_SHADOW + point lights RTS_*_LIGHTS + fog + ACES) for translucent ground decals, with an
// optional world-space planar/triplanar albedo texture (tex2, resolved by TextureCache: the uv rect is
// applied after fract() so atlas pages work without wrap) and an animated emissive pulse (glowing
// territory borders, dig marks).
//
//   U0 (per draw): vec4 color; vec4 params (x textured, y world units per texture repeat, z time, w pulse);
//                  vec4 params2 (x emissive, y specular, z texture strength, w receive shadow); vec4 rect (u0, v0, du, dv)
//   U1 / U2 / tex0 / tex1: as the lit shader (Renderer3D::bindLitFrame)
#pragma once
#include "../../../engine/render/Shaders.h"

namespace rts::game::shaders {

#define RTS_GROUNDFX_GLSL_U0 "layout(std140) uniform U0 { vec4 u_color; vec4 u_params; vec4 u_params2; vec4 u_rect; };\n"

inline const char* kGroundFxVS =
    RTS_GROUNDFX_GLSL_U0 RTS_GLSL_FRAME
    R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec4 a_col;
out vec3 v_nrm;
out vec3 v_wpos;
out vec4 v_col;
void main() {
    v_wpos = a_pos;
    v_nrm = a_nrm;
    v_col = a_col;
    gl_Position = u_viewProj * vec4(a_pos, 1.0);
}
)";

inline const char* kGroundFxFS =
    RTS_GROUNDFX_GLSL_U0 RTS_GLSL_FRAME RTS_GLSL_SHADOW RTS_GLSL_LIGHTS
    R"(
uniform sampler2D tex2;
in vec3 v_nrm;
in vec3 v_wpos;
in vec4 v_col;
out vec4 o_color;
void main() {
    vec4 albedo = u_color * v_col;
    vec3 n = normalize(v_nrm);
    if (u_params.x > 0.5) {
        vec3 an = abs(n);
        vec2 p = an.y > 0.6 ? v_wpos.xz : (an.x > an.z ? v_wpos.zy : v_wpos.xy);
        vec4 t = texture(tex2, u_rect.xy + fract(p / u_params.y) * u_rect.zw);
        albedo.rgb *= mix(vec3(1.0), t.rgb, u_params2.z);
        albedo.a *= mix(1.0, t.a, u_params2.z);
    }
    vec3 base = albedo.rgb * albedo.rgb;
    vec3 L = normalize(u_sunDir.xyz);
    vec3 V = normalize(u_camPos.xyz - v_wpos);
    float ndl = max(dot(n, L), 0.0);
    float sh = (ndl > 0.0 && u_params2.w > 0.5) ? shadowAt(v_wpos, n) : 1.0;
    vec3 sun = u_sunColor.rgb * u_sunColor.rgb * u_sunDir.w;
    vec3 amb = mix(u_groundColor.rgb * u_groundColor.rgb, u_skyColor.rgb * u_skyColor.rgb, n.y * 0.5 + 0.5);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 40.0) * u_params2.y;
    vec3 col = base * (amb + sun * ndl * sh) + sun * spec * sh;
    col += base * dynamicLight(v_wpos, n, V, u_params2.y);
    float pulse = 1.0 + u_params.w * sin(u_params.z * 2.6 - (v_wpos.x + v_wpos.z) * 0.7);
    col += base * (u_params2.x * pulse);
    float dist = length(u_camPos.xyz - v_wpos);
    col = mix(col, u_fog.rgb * u_fog.rgb, 1.0 - exp(-dist * u_fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    o_color = vec4(sqrt(col), albedo.a);
}
)";

inline const char* kGroundFxMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct GU0 { float4 color; float4 params; float4 params2; float4 rect; };\n"
    RTS_MSL_FRAME RTS_MSL_SHADOW RTS_MSL_LIGHTS
    R"(
struct VIn { float3 pos [[attribute(0)]]; float3 nrm [[attribute(1)]]; float4 col [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float3 nrm; float3 wpos; float4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant GU0& u [[buffer(1)]], constant U1& f [[buffer(2)]]) {
    VOut o;
    o.wpos = in.pos;
    o.nrm = in.nrm;
    o.col = in.col;
    o.pos = f.viewProj * float4(in.pos, 1.0);
    return o;
}
fragment float4 fs_main(VOut in [[stage_in]], constant GU0& u [[buffer(1)]], constant U1& f [[buffer(2)]],
                        depth2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],
                        constant U2& l [[buffer(3)]], texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]],
                        texture2d<float> tex2 [[texture(2)]], sampler s2 [[sampler(2)]]) {
    float4 albedo = u.color * in.col;
    float3 n = normalize(in.nrm);
    if (u.params.x > 0.5) {
        float3 an = abs(n);
        float2 p = an.y > 0.6 ? in.wpos.xz : (an.x > an.z ? in.wpos.zy : in.wpos.xy);
        float4 t = tex2.sample(s2, u.rect.xy + fract(p / u.params.y) * u.rect.zw);
        albedo.rgb *= mix(float3(1.0), t.rgb, u.params2.z);
        albedo.a *= mix(1.0, t.a, u.params2.z);
    }
    float3 base = albedo.rgb * albedo.rgb;
    float3 L = normalize(f.sunDir.xyz);
    float3 V = normalize(f.camPos.xyz - in.wpos);
    float ndl = max(dot(n, L), 0.0);
    float sh = (ndl > 0.0 && u.params2.w > 0.5) ? shadowAt(f, in.wpos, n, in.pos.xy, tex0, s0) : 1.0;
    float3 sun = f.sunColor.rgb * f.sunColor.rgb * f.sunDir.w;
    float3 amb = mix(f.groundColor.rgb * f.groundColor.rgb, f.skyColor.rgb * f.skyColor.rgb, n.y * 0.5 + 0.5);
    float3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 40.0) * u.params2.y;
    float3 col = base * (amb + sun * ndl * sh) + sun * spec * sh;
    col += base * dynamicLight(l, tex1, s1, in.wpos, n, V, u.params2.y);
    float pulse = 1.0 + u.params.w * sin(u.params.z * 2.6 - (in.wpos.x + in.wpos.z) * 0.7);
    col += base * (u.params2.x * pulse);
    float dist = length(f.camPos.xyz - in.wpos);
    col = mix(col, f.fog.rgb * f.fog.rgb, 1.0 - exp(-dist * f.fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    return float4(sqrt(col), albedo.a);
}
)";

} // namespace rts::game::shaders
