// Built-in shaders, written once per shading language. GLSL bodies are GLSL ES 3.00 without the
// #version line (the GL backend prepends "#version 300 es" or "#version 330 core").
//
// Lit pipeline (single pass, forward, mobile friendly):
//   sun (Lambert + Blinn specular) x shadow map with hardware PCF
//   + hemisphere ambient (sky/ground) + rim + distance fog + ACES tonemap, lighting in linear space.
#pragma once
#include "LightShaders.h"   // RTS_GLSL_LIGHTS / RTS_MSL_LIGHTS: dynamic point lights (U2, tex1)

namespace rts::render::shaders {

// Uniform blocks shared by the lit and shadow shaders (std140 / MSL layouts match):
//   U0 (per draw):  mat4 model; vec4 color; vec4 params(x unlit, y specular, z receiveShadow, w emissive)
//   U1 (per frame): mat4 viewProj; mat4 lightViewProj; vec4 sunDir(xyz, w intensity); vec4 sunColor;
//                   vec4 skyColor; vec4 groundColor; vec4 fog(rgb, density); vec4 camPos;
//                   vec4 shadow(x uvFlipY, y depthScale, z depthOffset, w texel); vec4 quality(x taps, y enabled, z radius uv, w normal offset)
#define RTS_GLSL_FRAME \
    "layout(std140) uniform U1 { mat4 u_viewProj; mat4 u_lightViewProj; vec4 u_sunDir; vec4 u_sunColor;\n" \
    "  vec4 u_skyColor; vec4 u_groundColor; vec4 u_fog; vec4 u_camPos; vec4 u_shadow; vec4 u_quality; };\n"
#define RTS_GLSL_BLOCKS \
    "layout(std140) uniform U0 { mat4 u_model; vec4 u_color; vec4 u_params; };\n" RTS_GLSL_FRAME

#define RTS_MSL_FRAME \
    "struct U1 { float4x4 viewProj; float4x4 lightViewProj; float4 sunDir; float4 sunColor;\n" \
    "  float4 skyColor; float4 groundColor; float4 fog; float4 camPos; float4 shadow; float4 quality; };\n"
#define RTS_MSL_BLOCKS \
    "struct U0 { float4x4 model; float4 color; float4 params; };\n" RTS_MSL_FRAME


// Shadow lookup shared by every lit shader (needs U1 and the shadow map in slot 0).
#define RTS_GLSL_SHADOW \
    "uniform highp sampler2DShadow tex0;\n" \
    "// Soft shadows: rotated Poisson-disc PCF over hardware 2x2 compare taps.\n" \
    "//   u_quality.x taps (1 / 4 / 8 / 16), u_quality.z filter radius in shadow UV, u_quality.w normal offset (world)\n" \
    "const vec2 kDisk[16] = vec2[16](\n" \
    "    vec2(-0.613, 0.617), vec2(0.170, -0.040), vec2(-0.299, 0.791), vec2(0.645, 0.493),\n" \
    "    vec2(-0.651, 0.717), vec2(0.421, 0.027), vec2(-0.817, -0.271), vec2(-0.705, -0.668),\n" \
    "    vec2(0.977, -0.108), vec2(0.063, -0.233), vec2(0.346, -0.894), vec2(-0.272, 0.416),\n" \
    "    vec2(-0.025, 0.919), vec2(-0.389, -0.144), vec2(0.733, -0.627), vec2(0.282, 0.512));\n" \
    "float shadowAt(vec3 wpos, vec3 n) {\n" \
    "    if (u_quality.y < 0.5) return 1.0;\n" \
    "    vec3 L = normalize(u_sunDir.xyz);\n" \
    "    float k = 1.0 - clamp(dot(n, L), 0.0, 1.0);          // grazing surfaces need the most offset\n" \
    "    vec4 lp = u_lightViewProj * vec4(wpos + n * u_quality.w * (0.35 + k), 1.0);\n" \
    "    vec3 p = lp.xyz / lp.w;\n" \
    "    vec2 uv = p.xy * 0.5 + 0.5;\n" \
    "    if (u_shadow.x > 0.5) uv.y = 1.0 - uv.y;\n" \
    "    float d = p.z * u_shadow.y + u_shadow.z;\n" \
    "    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || d >= 1.0) return 1.0;\n" \
    "    float r = u_quality.z;\n" \
    "    if (u_quality.x < 2.0) return texture(tex0, vec3(uv, d));\n" \
    "    if (u_quality.x < 5.0) { // weak GPUs: fixed 4 taps, each a bilinear 2x2 compare\n" \
    "        float s = texture(tex0, vec3(uv + vec2(-0.5, -0.5) * r, d));\n" \
    "        s += texture(tex0, vec3(uv + vec2( 0.5, -0.5) * r, d));\n" \
    "        s += texture(tex0, vec3(uv + vec2(-0.5,  0.5) * r, d));\n" \
    "        s += texture(tex0, vec3(uv + vec2( 0.5,  0.5) * r, d));\n" \
    "        return s * 0.25;\n" \
    "    }\n" \
    "    // per-pixel rotation (interleaved gradient noise) turns banding into fine grain\n" \
    "    float a = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n" \
    "    vec2 cs = vec2(cos(a), sin(a));\n" \
    "    int n_ = int(u_quality.x);\n" \
    "    float s = 0.0;\n" \
    "    for (int i = 0; i < 16; ++i) {\n" \
    "        if (i >= n_) break;\n" \
    "        vec2 o = kDisk[i];\n" \
    "        o = vec2(o.x * cs.x - o.y * cs.y, o.x * cs.y + o.y * cs.x);\n" \
    "        s += texture(tex0, vec3(uv + o * r, d));\n" \
    "    }\n" \
    "    return s / float(n_);\n" \
    "}\n"

#define RTS_MSL_SHADOW \
    "constant float2 kDisk[16] = {\n" \
    "    float2(-0.613, 0.617), float2(0.170, -0.040), float2(-0.299, 0.791), float2(0.645, 0.493),\n" \
    "    float2(-0.651, 0.717), float2(0.421, 0.027), float2(-0.817, -0.271), float2(-0.705, -0.668),\n" \
    "    float2(0.977, -0.108), float2(0.063, -0.233), float2(0.346, -0.894), float2(-0.272, 0.416),\n" \
    "    float2(-0.025, 0.919), float2(-0.389, -0.144), float2(0.733, -0.627), float2(0.282, 0.512)};\n" \
    "static float shadowAt(constant U1& f, float3 wpos, float3 n, float2 fragCoord, depth2d<float> sm, sampler ss) {\n" \
    "    if (f.quality.y < 0.5) return 1.0;\n" \
    "    float3 L = normalize(f.sunDir.xyz);\n" \
    "    float k = 1.0 - clamp(dot(n, L), 0.0, 1.0);\n" \
    "    float4 lp = f.lightViewProj * float4(wpos + n * f.quality.w * (0.35 + k), 1.0);\n" \
    "    float3 p = lp.xyz / lp.w;\n" \
    "    float2 uv = p.xy * 0.5 + 0.5;\n" \
    "    if (f.shadow.x > 0.5) uv.y = 1.0 - uv.y;\n" \
    "    float d = p.z * f.shadow.y + f.shadow.z;\n" \
    "    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || d >= 1.0) return 1.0;\n" \
    "    float r = f.quality.z;\n" \
    "    if (f.quality.x < 2.0) return sm.sample_compare(ss, uv, d);\n" \
    "    if (f.quality.x < 5.0) {\n" \
    "        float s = sm.sample_compare(ss, uv + float2(-0.5, -0.5) * r, d);\n" \
    "        s += sm.sample_compare(ss, uv + float2( 0.5, -0.5) * r, d);\n" \
    "        s += sm.sample_compare(ss, uv + float2(-0.5,  0.5) * r, d);\n" \
    "        s += sm.sample_compare(ss, uv + float2( 0.5,  0.5) * r, d);\n" \
    "        return s * 0.25;\n" \
    "    }\n" \
    "    float a = 6.2831853 * fract(52.9829189 * fract(dot(fragCoord, float2(0.06711056, 0.00583715))));\n" \
    "    float2 cs = float2(cos(a), sin(a));\n" \
    "    int cnt = int(f.quality.x);\n" \
    "    float s = 0.0;\n" \
    "    for (int i = 0; i < 16; ++i) {\n" \
    "        if (i >= cnt) break;\n" \
    "        float2 o = kDisk[i];\n" \
    "        o = float2(o.x * cs.x - o.y * cs.y, o.x * cs.y + o.y * cs.x);\n" \
    "        s += sm.sample_compare(ss, uv + o * r, d);\n" \
    "    }\n" \
    "    return s / float(cnt);\n" \
    "}\n"

inline const char* kLitVS =
    RTS_GLSL_BLOCKS
    R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec4 a_col;
out vec3 v_nrm;
out vec3 v_wpos;
out vec4 v_col;
void main() {
    vec4 wp = u_model * vec4(a_pos, 1.0);
    v_wpos = wp.xyz;
    v_nrm = mat3(u_model) * a_nrm;
    v_col = a_col;
    gl_Position = u_viewProj * wp;
}
)";

inline const char* kLitFS =
    RTS_GLSL_BLOCKS RTS_GLSL_SHADOW RTS_GLSL_LIGHTS
    R"(
in vec3 v_nrm;
in vec3 v_wpos;
in vec4 v_col;
out vec4 o_color;

float shadowFactor(vec3 n) { return u_params.z < 0.5 ? 1.0 : shadowAt(v_wpos, n); }

void main() {
    vec4 albedo = u_color * v_col;
    vec3 base = albedo.rgb * albedo.rgb;                  // ~sRGB -> linear
    vec3 n = normalize(v_nrm);
    vec3 L = normalize(u_sunDir.xyz);
    vec3 V = normalize(u_camPos.xyz - v_wpos);
    float ndl = max(dot(n, L), 0.0);
    float sh = ndl > 0.0 ? shadowFactor(n) : 1.0;
    vec3 sun = u_sunColor.rgb * u_sunColor.rgb * u_sunDir.w;
    vec3 sky = u_skyColor.rgb * u_skyColor.rgb, ground = u_groundColor.rgb * u_groundColor.rgb;
    vec3 amb = mix(ground, sky, n.y * 0.5 + 0.5);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 40.0) * u_params.y;
    float rim = pow(1.0 - max(dot(n, V), 0.0), 3.0) * 0.25;
    vec3 col = base * (amb + sun * ndl * sh) + sun * spec * sh + amb * rim * base;
    col += base * dynamicLight(v_wpos, n, V, u_params.y);
    col = mix(col, base, u_params.x) + base * u_params.w;
    float dist = length(u_camPos.xyz - v_wpos);
    col = mix(col, u_fog.rgb * u_fog.rgb, 1.0 - exp(-dist * u_fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0); // ACES fit
    o_color = vec4(sqrt(col), albedo.a);
}
)";

inline const char* kLitMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n" RTS_MSL_BLOCKS RTS_MSL_SHADOW RTS_MSL_LIGHTS
    R"(
struct VIn { float3 pos [[attribute(0)]]; float3 nrm [[attribute(1)]]; float4 col [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float3 nrm; float3 wpos; float4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]], constant U1& f [[buffer(2)]]) {
    VOut o;
    float4 wp = u.model * float4(in.pos, 1.0);
    o.wpos = wp.xyz;
    o.nrm = (u.model * float4(in.nrm, 0.0)).xyz;
    o.col = in.col;
    o.pos = f.viewProj * wp;
    return o;
}
static float shadowFactor(constant U0& u, constant U1& f, float3 wpos, float3 n, float2 fragCoord, depth2d<float> sm, sampler ss) {
    return u.params.z < 0.5 ? 1.0 : shadowAt(f, wpos, n, fragCoord, sm, ss);
}
fragment float4 fs_main(VOut in [[stage_in]], constant U0& u [[buffer(1)]], constant U1& f [[buffer(2)]],
                        depth2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],
                        constant U2& l [[buffer(3)]], texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]]) {
    float4 albedo = u.color * in.col;
    float3 base = albedo.rgb * albedo.rgb;
    float3 n = normalize(in.nrm);
    float3 L = normalize(f.sunDir.xyz);
    float3 V = normalize(f.camPos.xyz - in.wpos);
    float ndl = max(dot(n, L), 0.0);
    float sh = ndl > 0.0 ? shadowFactor(u, f, in.wpos, n, in.pos.xy, tex0, s0) : 1.0;
    float3 sun = f.sunColor.rgb * f.sunColor.rgb * f.sunDir.w;
    float3 amb = mix(f.groundColor.rgb * f.groundColor.rgb, f.skyColor.rgb * f.skyColor.rgb, n.y * 0.5 + 0.5);
    float3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 40.0) * u.params.y;
    float rim = pow(1.0 - max(dot(n, V), 0.0), 3.0) * 0.25;
    float3 col = base * (amb + sun * ndl * sh) + sun * spec * sh + amb * rim * base;
    col += base * dynamicLight(l, tex1, s1, in.wpos, n, V, u.params.y);
    col = mix(col, base, u.params.x) + base * u.params.w;
    float dist = length(f.camPos.xyz - in.wpos);
    col = mix(col, f.fog.rgb * f.fog.rgb, 1.0 - exp(-dist * f.fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    return float4(sqrt(col), albedo.a);
}
)";

// Depth-only shadow caster
inline const char* kShadowVS =
    RTS_GLSL_BLOCKS
    R"(
layout(location = 0) in vec3 a_pos;
void main() { gl_Position = u_lightViewProj * (u_model * vec4(a_pos, 1.0)); }
)";
inline const char* kShadowFS = R"(
void main() {}
)";
inline const char* kShadowMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n" RTS_MSL_BLOCKS
    R"(
struct VIn { float3 pos [[attribute(0)]]; };
struct VOut { float4 pos [[position]]; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]], constant U1& f [[buffer(2)]]) {
    VOut o; o.pos = f.lightViewProj * (u.model * float4(in.pos, 1.0)); return o;
}
)";

// ---------------- colored lines / flat triangles (overlays, no lighting) ----------------
inline const char* kColorVS =
    RTS_GLSL_BLOCKS
    R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
out vec4 v_col;
void main() { v_col = a_col; gl_Position = u_viewProj * vec4(a_pos, 1.0); }
)";
inline const char* kColorFS = R"(
in vec4 v_col;
out vec4 o_color;
void main() { o_color = v_col; }
)";
inline const char* kColorMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n" RTS_MSL_BLOCKS
    R"(
struct VIn { float3 pos [[attribute(0)]]; float4 col [[attribute(1)]]; };
struct VOut { float4 pos [[position]]; float4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant U1& f [[buffer(2)]]) {
    VOut o; o.pos = f.viewProj * float4(in.pos, 1.0); o.col = in.col; return o;
}
fragment float4 fs_main(VOut in [[stage_in]]) { return in.col; }
)";

// ---------------- Dear ImGui ----------------
// U0: mat4 proj; vec4 params (x: 1 = texture is single-channel alpha)
inline const char* kImGuiVS = R"(
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(std140) uniform U0 { mat4 u_proj; vec4 u_params; };
out vec2 v_uv;
out vec4 v_col;
void main() { v_uv = a_uv; v_col = a_col; gl_Position = u_proj * vec4(a_pos, 0.0, 1.0); }
)";
inline const char* kImGuiFS = R"(
layout(std140) uniform U0 { mat4 u_proj; vec4 u_params; };
uniform sampler2D tex0;
in vec2 v_uv;
in vec4 v_col;
out vec4 o_color;
void main() {
    vec4 t = texture(tex0, v_uv);
    if (u_params.x > 0.5) t = vec4(1.0, 1.0, 1.0, t.r);
    o_color = v_col * t;
}
)";
inline const char* kImGuiMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct U0 { float4x4 proj; float4 params; };
struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float2 uv; float4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) {
    VOut o; o.pos = u.proj * float4(in.pos, 0.0, 1.0); o.uv = in.uv; o.col = in.col; return o;
}
fragment float4 fs_main(VOut in [[stage_in]], constant U0& u [[buffer(1)]],
                        texture2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    float4 t = tex0.sample(s0, in.uv);
    if (u.params.x > 0.5) t = float4(1.0, 1.0, 1.0, t.r);
    return in.col * t;
}
)";

} // namespace rts::render::shaders
