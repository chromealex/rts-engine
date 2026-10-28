// Shader code of the dynamic lights (see Lights.h), kept apart from Shaders.h / ModelShaders.h so
// each lit shader only needs the declarations macro and one line in main().
//
// Light block (slot U2 in the lit shader, U4 in the model shader):
//   vec4 map  (origin.xz, 1/extent, enabled)
//   vec4 info (x per-pixel count, y map range, z flipY, w night emissive boost)
//   vec4 cloud (noise offset.xz, scale, coverage); vec4 sky (x cloud shadow strength)
//   vec4 pos[8] (xyz, radius); vec4 col[8] (rgb * intensity)
// Light map texture: tex1 in the lit shader, tex7 in the model shader (RGBA8, light / range).
#pragma once

namespace rts::render::shaders {

// GLSL: block / sampler names are parameters because each shader has its own slots.
#define RTS_GLSL_LIGHTS_AT(BLOCK, TEX) \
    "layout(std140) uniform " BLOCK " { vec4 l_map; vec4 l_info; vec4 l_cloud; vec4 l_sky; vec4 l_pos[8]; vec4 l_col[8]; };\n" \
    "uniform mediump sampler2D " TEX ";\n" \
    "float nightGlow() { return l_info.w; }\n" \
    "float cloudShade(vec3 p) { /* drifting cloud shadows on the sun: a few sines, no texture */\n" \
    "    if (l_sky.x <= 0.0) return 1.0;\n" \
    "    vec2 q = p.xz * l_cloud.z + l_cloud.xy;\n" \
    "    float v = sin(q.x) * sin(q.y * 1.31) + 0.6 * sin(q.x * 2.13 + q.y * 1.71 + 1.7) * sin(q.y * 2.37 - q.x * 0.93);\n" \
    "    v = v * 0.4 + 0.5;\n" \
    "    return 1.0 - smoothstep(1.0 - l_cloud.w, 1.25 - l_cloud.w, v) * l_sky.x;\n" \
    "}\n" \
    "vec3 dynamicLight(vec3 wpos, vec3 n, vec3 V, float specK) {\n" \
    "    vec3 c = vec3(0.0);\n" \
    "    if (l_map.w > 0.5) { /* ground-level light: weaker than the 3D path so the tiers match */\n" \
    "        vec2 uv = (wpos.xz - l_map.xy) * l_map.z;\n" \
    "        vec2 e = min(uv, 1.0 - uv);\n" \
    "        float fade = clamp(min(e.x, e.y) * 16.0, 0.0, 1.0);\n" \
    "        if (l_info.z > 0.5) uv.y = 1.0 - uv.y;\n" \
    "        c = texture(" TEX ", uv).rgb * (l_info.y * fade * (0.4 + 0.3 * max(n.y, 0.0)));\n" \
    "    }\n" \
    "    int cnt = int(l_info.x);\n" \
    "    for (int i = 0; i < 8; ++i) {\n" \
    "        if (i >= cnt) break;\n" \
    "        vec3 d = l_pos[i].xyz - wpos;\n" \
    "        float d2 = dot(d, d);\n" \
    "        float a = clamp(1.0 - d2 / (l_pos[i].w * l_pos[i].w), 0.0, 1.0);\n" \
    "        a *= a;\n" \
    "        vec3 L = d * inversesqrt(max(d2, 1e-6));\n" \
    "        float ndl = max(dot(n, L), 0.0) * 0.85 + 0.15;\n" \
    "        float sp = pow(max(dot(n, normalize(L + V)), 0.0), 40.0) * specK;\n" \
    "        c += l_col[i].rgb * (a * (ndl + sp));\n" \
    "    }\n" \
    "    return c;\n" \
    "}\n"

#define RTS_GLSL_LIGHTS RTS_GLSL_LIGHTS_AT("U2", "tex1")

// MSL: the block and the texture are function arguments (buffer / texture indices are chosen in
// each fragment function's signature).
#define RTS_MSL_LIGHTS \
    "struct LightU { float4 map; float4 info; float4 cloud; float4 sky; float4 pos[8]; float4 col[8]; };\n" \
    "static float cloudShade(constant LightU& l, float3 p) {\n" \
    "    if (l.sky.x <= 0.0) return 1.0;\n" \
    "    float2 q = p.xz * l.cloud.z + l.cloud.xy;\n" \
    "    float v = sin(q.x) * sin(q.y * 1.31) + 0.6 * sin(q.x * 2.13 + q.y * 1.71 + 1.7) * sin(q.y * 2.37 - q.x * 0.93);\n" \
    "    v = v * 0.4 + 0.5;\n" \
    "    return 1.0 - smoothstep(1.0 - l.cloud.w, 1.25 - l.cloud.w, v) * l.sky.x;\n" \
    "}\n" \
    "static float3 dynamicLight(constant LightU& l, texture2d<float> lm, sampler ls, float3 wpos, float3 n, float3 V, float specK) {\n" \
    "    float3 c = float3(0.0);\n" \
    "    if (l.map.w > 0.5) {\n" \
    "        float2 uv = (wpos.xz - l.map.xy) * l.map.z;\n" \
    "        float2 e = min(uv, 1.0 - uv);\n" \
    "        float fade = clamp(min(e.x, e.y) * 16.0, 0.0, 1.0);\n" \
    "        if (l.info.z > 0.5) uv.y = 1.0 - uv.y;\n" \
    "        c = lm.sample(ls, uv).rgb * (l.info.y * fade * (0.4 + 0.3 * max(n.y, 0.0)));\n" \
    "    }\n" \
    "    int cnt = int(l.info.x);\n" \
    "    for (int i = 0; i < 8; ++i) {\n" \
    "        if (i >= cnt) break;\n" \
    "        float3 d = l.pos[i].xyz - wpos;\n" \
    "        float d2 = dot(d, d);\n" \
    "        float a = clamp(1.0 - d2 / (l.pos[i].w * l.pos[i].w), 0.0, 1.0);\n" \
    "        a *= a;\n" \
    "        float3 L = d * rsqrt(max(d2, 1e-6));\n" \
    "        float ndl = max(dot(n, L), 0.0) * 0.85 + 0.15;\n" \
    "        float sp = pow(max(dot(n, normalize(L + V)), 0.0), 40.0) * specK;\n" \
    "        c += l.col[i].rgb * (a * (ndl + sp));\n" \
    "    }\n" \
    "    return c;\n" \
    "}\n"

// Light map splat: clip-space quads, radial falloff (1 - r^2)^2, additive.
inline const char* kLightMapVS = R"(
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
out vec2 v_uv;
out vec3 v_col;
void main() { v_uv = a_uv; v_col = a_col.rgb; gl_Position = vec4(a_pos, 0.0, 1.0); }
)";
inline const char* kLightMapFS = R"(
in vec2 v_uv;
in vec3 v_col;
out vec4 o_color;
void main() {
    float f = clamp(1.0 - dot(v_uv, v_uv), 0.0, 1.0);
    o_color = vec4(v_col * (f * f), 1.0);
}
)";
inline const char* kLightMapMSL = R"(#include <metal_stdlib>
using namespace metal;
struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float2 uv; float3 col; };
vertex VOut vs_main(VIn in [[stage_in]]) { VOut o; o.pos = float4(in.pos, 0.0, 1.0); o.uv = in.uv; o.col = in.col.rgb; return o; }
fragment float4 fs_main(VOut in [[stage_in]]) {
    float f = clamp(1.0 - dot(in.uv, in.uv), 0.0, 1.0);
    return float4(in.col * (f * f), 1.0);
}
)";

// Halos: camera-facing quads (corners built on the CPU), soft core + wide falloff, additive,
// depth-tested against the scene but not written. Drawn in display space (after tonemapping).
// U0: mat4 viewProj
inline const char* kHaloVS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; };
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
out vec2 v_uv;
out vec4 v_col;
void main() { v_uv = a_uv; v_col = a_col; gl_Position = u_viewProj * vec4(a_pos, 1.0); }
)";
inline const char* kHaloFS = R"(
in vec2 v_uv;
in vec4 v_col;
out vec4 o_color;
void main() {
    float d2 = dot(v_uv, v_uv);
    float f = clamp(1.0 - d2, 0.0, 1.0);
    float g = f * f * 0.55 + exp(-d2 * 18.0) * 0.6;   // wide glow + bright core
    o_color = vec4(v_col.rgb * (g * v_col.a), 1.0);
}
)";
inline const char* kHaloMSL = R"(#include <metal_stdlib>
using namespace metal;
struct HU0 { float4x4 viewProj; };
struct VIn { float3 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float2 uv; float4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant HU0& u [[buffer(1)]]) {
    VOut o; o.pos = u.viewProj * float4(in.pos, 1.0); o.uv = in.uv; o.col = in.col; return o;
}
fragment float4 fs_main(VOut in [[stage_in]]) {
    float d2 = dot(in.uv, in.uv);
    float f = clamp(1.0 - d2, 0.0, 1.0);
    float g = f * f * 0.55 + exp(-d2 * 18.0) * 0.6;
    return float4(in.col.rgb * (g * in.col.a), 1.0);
}
)";

} // namespace rts::render::shaders
