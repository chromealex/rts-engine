// Shader code of the dynamic lights (see Lights.h), kept apart from Shaders.h so the lit shader only
// needs two insertion points: the declarations macro and one line in main().
//
// U2: vec4 map (origin.xz, 1/extent, enabled); vec4 info (x per-pixel count, y map range, z flipY);
//     vec4 pos[8] (xyz, radius); vec4 col[8] (rgb * intensity)
// tex1: light map (RGBA8, light / range).
#pragma once

namespace rts::render::shaders {

#define RTS_GLSL_LIGHTS \
    "layout(std140) uniform U2 { vec4 l_map; vec4 l_info; vec4 l_pos[8]; vec4 l_col[8]; };\n" \
    "uniform sampler2D tex1;\n" \
    "vec3 dynamicLight(vec3 wpos, vec3 n, vec3 V, float specK) {\n" \
    "    vec3 c = vec3(0.0);\n" \
    "    if (l_map.w > 0.5) {\n" \
    "        vec2 uv = (wpos.xz - l_map.xy) * l_map.z;\n" \
    "        vec2 e = min(uv, 1.0 - uv);\n" \
    "        float fade = clamp(min(e.x, e.y) * 16.0, 0.0, 1.0);\n" \
    "        if (l_info.z > 0.5) uv.y = 1.0 - uv.y;\n" \
    "        c = texture(tex1, uv).rgb * (l_info.y * fade * (0.4 + 0.3 * max(n.y, 0.0)));\n" \
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

#define RTS_MSL_LIGHTS \
    "struct U2 { float4 map; float4 info; float4 pos[8]; float4 col[8]; };\n" \
    "static float3 dynamicLight(constant U2& l, texture2d<float> lm, sampler ls, float3 wpos, float3 n, float3 V, float specK) {\n" \
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

} // namespace rts::render::shaders
