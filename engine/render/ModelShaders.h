// Model shaders (GLSL ES 3.00 + MSL), compiled as permutations: the renderer prepends
//   #define SKINNED      GPU skinning, 4 influences, joint palette in U3
//   #define NORMALMAP    tangent-space normal map (tex3) with vertex tangents
//   #define ALPHAMASK    alpha test against the material cutoff
//   #define RTS_Q 0|1|2  material quality: 0 Blinn-Phong, factors only (weak GPUs)
//                        1 GGX + metallic-roughness / emissive textures + normal maps
//                        2 + occlusion texture
// Uniforms (std140 / MSL layouts match):
//   U0 per draw:  mat4 model; mat4 normalMat; vec4 tint (rgb multiply, a opacity);
//                 vec4 team (rgb team colour, a hit flash); vec4 draw (x receive shadow)
//   U1 per frame: as the lit shader (Shaders.h)
//   U2 material:  vec4 base; vec4 emissive (rgb, linear, premultiplied by strength);
//                 vec4 pbr (metallic, roughness, normalScale, occlusionStrength);
//                 vec4 misc (alphaCutoff, teamMode, teamStrength, useVertexColor);
//                 vec4 uv0 (offset.xy, scale.xy); vec4 uv1 (rotation, unlit, -, -)
//   U3 skin:      vec4 joints[240] (80 joints as 3 rows of a 3x4 matrix)
// Textures: tex0 shadow map, tex1 base colour (sRGB), tex2 metallic-roughness (G rough, B metal),
//           tex3 normal, tex4 occlusion (R), tex5 emissive (sRGB), tex6 team mask (R)
// Dynamic lights (LightShaders.h): U4 light block, tex7 light map.
#pragma once
#include "Shaders.h"

namespace rts::render::shaders {

#define RTS_GLSL_MODEL_BLOCKS \
    "layout(std140) uniform U0 { mat4 u_model; mat4 u_normalMat; vec4 u_tint; vec4 u_team; vec4 u_draw; };\n" \
    RTS_GLSL_FRAME \
    "layout(std140) uniform U2 { vec4 m_base; vec4 m_emissive; vec4 m_pbr; vec4 m_misc; vec4 m_uv0; vec4 m_uv1; };\n" \
    "#if defined(SKINNED)\n" \
    "layout(std140) uniform U3 { vec4 u_joints[240]; };\n" \
    "#endif\n"

#define RTS_MSL_MODEL_BLOCKS \
    "#include <metal_stdlib>\nusing namespace metal;\n" \
    "struct MU0 { float4x4 model; float4x4 normalMat; float4 tint; float4 team; float4 draw; };\n" \
    RTS_MSL_FRAME \
    "struct MU2 { float4 base; float4 emissive; float4 pbr; float4 misc; float4 uv0; float4 uv1; };\n" \
    "struct MU3 { float4 joints[240]; };\n"

// ---------------------------------------------------------------- GLSL

inline const char* kModelVS =
    RTS_GLSL_MODEL_BLOCKS
    R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_tan;
layout(location = 4) in vec4 a_col;
#if defined(SKINNED)
layout(location = 5) in vec4 a_joints;
layout(location = 6) in vec4 a_weights;
#endif
out vec3 v_wpos;
out vec3 v_nrm;
out vec2 v_uv;
out vec4 v_col;
#if defined(NORMALMAP)
out vec4 v_tan;
#endif
void main() {
    vec4 p = vec4(a_pos, 1.0);
    vec3 n = a_nrm;
    vec3 t = a_tan.xyz;
#if defined(SKINNED)
    vec4 w = a_weights / max(dot(a_weights, vec4(1.0)), 1e-5);
    ivec4 j = ivec4(a_joints + 0.5) * 3;
    vec4 r0 = u_joints[j.x] * w.x + u_joints[j.y] * w.y + u_joints[j.z] * w.z + u_joints[j.w] * w.w;
    vec4 r1 = u_joints[j.x + 1] * w.x + u_joints[j.y + 1] * w.y + u_joints[j.z + 1] * w.z + u_joints[j.w + 1] * w.w;
    vec4 r2 = u_joints[j.x + 2] * w.x + u_joints[j.y + 2] * w.y + u_joints[j.z + 2] * w.z + u_joints[j.w + 2] * w.w;
    p = vec4(dot(r0, p), dot(r1, p), dot(r2, p), 1.0);
    n = vec3(dot(r0.xyz, n), dot(r1.xyz, n), dot(r2.xyz, n));
    t = vec3(dot(r0.xyz, t), dot(r1.xyz, t), dot(r2.xyz, t));
#endif
    vec4 wp = u_model * p;
    v_wpos = wp.xyz;
    v_nrm = mat3(u_normalMat) * n;
#if defined(NORMALMAP)
    v_tan = vec4(mat3(u_model) * t, a_tan.w);
#endif
    // KHR_texture_transform: offset * rotation * scale
    float c = cos(m_uv1.x), s = sin(m_uv1.x);
    vec2 uv = a_uv * m_uv0.zw;
    v_uv = vec2(c * uv.x + s * uv.y, -s * uv.x + c * uv.y) + m_uv0.xy;
    v_col = a_col;
    gl_Position = u_viewProj * wp;
}
)";

inline const char* kModelFS =
    RTS_GLSL_MODEL_BLOCKS RTS_GLSL_SHADOW RTS_GLSL_LIGHTS_AT("U4", "tex7")
    R"(
uniform mediump sampler2D tex1;
uniform mediump sampler2D tex2;
uniform mediump sampler2D tex3;
uniform mediump sampler2D tex4;
uniform mediump sampler2D tex5;
uniform mediump sampler2D tex6;
in vec3 v_wpos;
in vec3 v_nrm;
in vec2 v_uv;
in vec4 v_col;
#if defined(NORMALMAP)
in vec4 v_tan;
#endif
out vec4 o_color;

vec3 toLinear(vec3 c) { return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878); }
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    vec4 tb = texture(tex1, v_uv);
    vec4 base = m_base * vec4(toLinear(tb.rgb), tb.a);
    if (m_misc.w > 0.5) base *= v_col;
    int teamMode = int(m_misc.y + 0.5);
    if (teamMode == 1) {
        base.rgb = mix(base.rgb, base.rgb * u_team.rgb, m_misc.z);
    } else if (teamMode > 1) {
        float m = 0.0, k = 1.0;
        if (teamMode == 2) { m = texture(tex6, v_uv).r; k = min(0.5 + luma(base.rgb) * 1.5, 1.0); }
        else if (teamMode == 3) { m = 1.0 - tb.a; base.a = m_base.a; }
        else { m = 1.0 - v_col.a; base.a = m_base.a * tb.a; }
        base.rgb = mix(base.rgb, u_team.rgb * k, clamp(m * m_misc.z, 0.0, 1.0));
    }
    base.rgb *= u_tint.rgb;
#if defined(ALPHAMASK)
    if (base.a < m_misc.x) discard;
    base.a = 1.0;
#endif
    vec3 n = normalize(v_nrm);
    if (!gl_FrontFacing) n = -n;
#if defined(NORMALMAP) && RTS_Q >= 1
    {
        vec3 t = normalize(v_tan.xyz - n * dot(n, v_tan.xyz));
        vec3 b = cross(n, t) * v_tan.w;
        vec3 tn = texture(tex3, v_uv).xyz * 2.0 - 1.0;
        tn.xy *= m_pbr.z;
        n = normalize(t * tn.x + b * tn.y + n * tn.z);
    }
#endif
    float metallic = m_pbr.x, rough = m_pbr.y, ao = 1.0;
    vec3 emis = m_emissive.rgb;
#if RTS_Q >= 1
    vec4 mr = texture(tex2, v_uv);
    rough *= mr.g;
    metallic *= mr.b;
    emis *= toLinear(texture(tex5, v_uv).rgb);
#endif
#if RTS_Q >= 2
    ao = mix(1.0, texture(tex4, v_uv).r, m_pbr.w);
#endif
    rough = clamp(rough, 0.045, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    vec3 L = normalize(u_sunDir.xyz);
    vec3 V = normalize(u_camPos.xyz - v_wpos);
    float ndl = max(dot(n, L), 0.0);
    float ndv = max(dot(n, V), 1e-4);
    float sh = (ndl > 0.0 && u_draw.x > 0.5) ? shadowAt(v_wpos, normalize(v_nrm)) : 1.0;
    vec3 sun = u_sunColor.rgb * u_sunColor.rgb * u_sunDir.w;
    vec3 sky = u_skyColor.rgb * u_skyColor.rgb, ground = u_groundColor.rgb * u_groundColor.rgb;
    vec3 amb = mix(ground, sky, n.y * 0.5 + 0.5);
    vec3 diff = base.rgb * (1.0 - metallic);
    vec3 F0 = mix(vec3(0.04), base.rgb, metallic);
    vec3 H = normalize(L + V);
    float ndh = max(dot(n, H), 0.0);
#if RTS_Q == 0
    float a2 = rough * rough * rough * rough;
    float e = clamp(2.0 / a2 - 2.0, 1.0, 512.0);
    vec3 spec = F0 * pow(ndh, e) * (e + 8.0) * 0.125 * ndl;
    vec3 ambSpec = F0 * amb * (1.0 - rough) * 0.5;
#else
    float a = rough * rough, a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d);
    float k = (rough + 1.0) * (rough + 1.0) * 0.125;
    float vis = 0.25 / ((ndl * (1.0 - k) + k) * (ndv * (1.0 - k) + k));
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(L, H), 0.0), 5.0);
    vec3 spec = D * vis * F * ndl * 3.14159265;   // lights use the "no 1/pi" convention of the lit shader
    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022), c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    vec3 R = reflect(-V, n);
    vec3 env = mix(mix(ground, sky, clamp(R.y * 0.5 + 0.5, 0.0, 1.0)), amb, rough);
    vec3 ambSpec = env * (F0 * AB.x + AB.y);
#endif
    vec3 col = diff * (amb * ao + sun * ndl * sh) + sun * spec * sh + ambSpec * ao;
    col += amb * base.rgb * pow(1.0 - ndv, 3.0) * 0.25;   // rim, keeps silhouettes readable from the RTS camera
    col += diff * dynamicLight(v_wpos, n, V, (1.0 - rough) * 0.5);   // point lights (Lights.h)
    if (m_uv1.y > 0.5) col = base.rgb;
    col += emis * (1.0 + nightGlow()) + u_team.a * vec3(1.0, 0.85, 0.7);
    float dist = length(u_camPos.xyz - v_wpos);
    col = mix(col, u_fog.rgb * u_fog.rgb, 1.0 - exp(-dist * u_fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    o_color = vec4(sqrt(col), base.a * u_tint.a);
}
)";

inline const char* kModelShadowVS =
    RTS_GLSL_MODEL_BLOCKS
    R"(
layout(location = 0) in vec3 a_pos;
layout(location = 2) in vec2 a_uv;
#if defined(SKINNED)
layout(location = 5) in vec4 a_joints;
layout(location = 6) in vec4 a_weights;
#endif
out vec2 v_uv;
void main() {
    vec4 p = vec4(a_pos, 1.0);
#if defined(SKINNED)
    vec4 w = a_weights / max(dot(a_weights, vec4(1.0)), 1e-5);
    ivec4 j = ivec4(a_joints + 0.5) * 3;
    vec4 r0 = u_joints[j.x] * w.x + u_joints[j.y] * w.y + u_joints[j.z] * w.z + u_joints[j.w] * w.w;
    vec4 r1 = u_joints[j.x + 1] * w.x + u_joints[j.y + 1] * w.y + u_joints[j.z + 1] * w.z + u_joints[j.w + 1] * w.w;
    vec4 r2 = u_joints[j.x + 2] * w.x + u_joints[j.y + 2] * w.y + u_joints[j.z + 2] * w.z + u_joints[j.w + 2] * w.w;
    p = vec4(dot(r0, p), dot(r1, p), dot(r2, p), 1.0);
#endif
    float c = cos(m_uv1.x), s = sin(m_uv1.x);
    vec2 uv = a_uv * m_uv0.zw;
    v_uv = vec2(c * uv.x + s * uv.y, -s * uv.x + c * uv.y) + m_uv0.xy;
    gl_Position = u_lightViewProj * (u_model * p);
}
)";

inline const char* kModelShadowFS =
    RTS_GLSL_MODEL_BLOCKS
    R"(
uniform mediump sampler2D tex1;
in vec2 v_uv;
void main() {
#if defined(ALPHAMASK)
    if (m_base.a * texture(tex1, v_uv).a < m_misc.x) discard;
#endif
}
)";

// ---------------------------------------------------------------- MSL

inline const char* kModelMSL =
    RTS_MSL_MODEL_BLOCKS RTS_MSL_SHADOW RTS_MSL_LIGHTS
    R"(
struct VIn {
    float3 pos [[attribute(0)]];
    float3 nrm [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float4 tan [[attribute(3)]];
    float4 col [[attribute(4)]];
#if defined(SKINNED)
    uchar4 joints [[attribute(5)]];
    float4 weights [[attribute(6)]];
#endif
};
struct VOut { float4 pos [[position]]; float3 wpos; float3 nrm; float2 uv; float4 col; float4 tan; };

static float2 uvTransform(constant MU2& m, float2 uv0) {
    float c = cos(m.uv1.x), s = sin(m.uv1.x);
    float2 uv = uv0 * m.uv0.zw;
    return float2(c * uv.x + s * uv.y, -s * uv.x + c * uv.y) + m.uv0.xy;
}

vertex VOut vs_main(VIn in [[stage_in]], constant MU0& u [[buffer(1)]], constant U1& f [[buffer(2)]], constant MU2& m [[buffer(3)]]
#if defined(SKINNED)
                    , constant MU3& sk [[buffer(4)]]
#endif
                    ) {
    float4 p = float4(in.pos, 1.0);
    float3 n = in.nrm, t = in.tan.xyz;
#if defined(SKINNED)
    float4 w = in.weights / max(dot(in.weights, float4(1.0)), 1e-5);
    int4 j = int4(in.joints) * 3;
    float4 r0 = sk.joints[j.x] * w.x + sk.joints[j.y] * w.y + sk.joints[j.z] * w.z + sk.joints[j.w] * w.w;
    float4 r1 = sk.joints[j.x + 1] * w.x + sk.joints[j.y + 1] * w.y + sk.joints[j.z + 1] * w.z + sk.joints[j.w + 1] * w.w;
    float4 r2 = sk.joints[j.x + 2] * w.x + sk.joints[j.y + 2] * w.y + sk.joints[j.z + 2] * w.z + sk.joints[j.w + 2] * w.w;
    p = float4(dot(r0, p), dot(r1, p), dot(r2, p), 1.0);
    n = float3(dot(r0.xyz, n), dot(r1.xyz, n), dot(r2.xyz, n));
    t = float3(dot(r0.xyz, t), dot(r1.xyz, t), dot(r2.xyz, t));
#endif
    VOut o;
    float4 wp = u.model * p;
    o.wpos = wp.xyz;
    o.nrm = (u.normalMat * float4(n, 0.0)).xyz;
    o.tan = float4((u.model * float4(t, 0.0)).xyz, in.tan.w);
    o.uv = uvTransform(m, in.uv);
    o.col = in.col;
    o.pos = f.viewProj * wp;
    return o;
}

static float3 toLinear(float3 c) { return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878); }
static float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

fragment float4 fs_main(VOut in [[stage_in]], bool frontFacing [[front_facing]],
                        constant MU0& u [[buffer(1)]], constant U1& f [[buffer(2)]], constant MU2& m [[buffer(3)]],
                        depth2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],
                        texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]],
                        texture2d<float> tex2 [[texture(2)]], sampler s2 [[sampler(2)]],
                        texture2d<float> tex3 [[texture(3)]], sampler s3 [[sampler(3)]],
                        texture2d<float> tex4 [[texture(4)]], sampler s4 [[sampler(4)]],
                        texture2d<float> tex5 [[texture(5)]], sampler s5 [[sampler(5)]],
                        texture2d<float> tex6 [[texture(6)]], sampler s6 [[sampler(6)]],
                        constant LightU& lu [[buffer(5)]], texture2d<float> tex7 [[texture(7)]], sampler s7 [[sampler(7)]]) {
    float4 tb = tex1.sample(s1, in.uv);
    float4 base = m.base * float4(toLinear(tb.rgb), tb.a);
    if (m.misc.w > 0.5) base *= in.col;
    int teamMode = int(m.misc.y + 0.5);
    if (teamMode == 1) {
        base.rgb = mix(base.rgb, base.rgb * u.team.rgb, m.misc.z);
    } else if (teamMode > 1) {
        float tm = 0.0, k = 1.0;
        if (teamMode == 2) { tm = tex6.sample(s6, in.uv).r; k = min(0.5 + luma(base.rgb) * 1.5, 1.0); }
        else if (teamMode == 3) { tm = 1.0 - tb.a; base.a = m.base.a; }
        else { tm = 1.0 - in.col.a; base.a = m.base.a * tb.a; }
        base.rgb = mix(base.rgb, u.team.rgb * k, clamp(tm * m.misc.z, 0.0, 1.0));
    }
    base.rgb *= u.tint.rgb;
#if defined(ALPHAMASK)
    if (base.a < m.misc.x) discard_fragment();
    base.a = 1.0;
#endif
    float3 n = normalize(in.nrm);
    if (!frontFacing) n = -n;
#if defined(NORMALMAP) && RTS_Q >= 1
    {
        float3 t = normalize(in.tan.xyz - n * dot(n, in.tan.xyz));
        float3 b = cross(n, t) * in.tan.w;
        float3 tn = tex3.sample(s3, in.uv).xyz * 2.0 - 1.0;
        tn.xy *= m.pbr.z;
        n = normalize(t * tn.x + b * tn.y + n * tn.z);
    }
#endif
    float metallic = m.pbr.x, rough = m.pbr.y, ao = 1.0;
    float3 emis = m.emissive.rgb;
#if RTS_Q >= 1
    float4 mr = tex2.sample(s2, in.uv);
    rough *= mr.g;
    metallic *= mr.b;
    emis *= toLinear(tex5.sample(s5, in.uv).rgb);
#endif
#if RTS_Q >= 2
    ao = mix(1.0, tex4.sample(s4, in.uv).r, m.pbr.w);
#endif
    rough = clamp(rough, 0.045, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    float3 L = normalize(f.sunDir.xyz);
    float3 V = normalize(f.camPos.xyz - in.wpos);
    float ndl = max(dot(n, L), 0.0);
    float ndv = max(dot(n, V), 1e-4);
    float sh = (ndl > 0.0 && u.draw.x > 0.5) ? shadowAt(f, in.wpos, normalize(in.nrm), in.pos.xy, tex0, s0) : 1.0;
    float3 sun = f.sunColor.rgb * f.sunColor.rgb * f.sunDir.w;
    float3 sky = f.skyColor.rgb * f.skyColor.rgb, ground = f.groundColor.rgb * f.groundColor.rgb;
    float3 amb = mix(ground, sky, n.y * 0.5 + 0.5);
    float3 diff = base.rgb * (1.0 - metallic);
    float3 F0 = mix(float3(0.04), base.rgb, metallic);
    float3 H = normalize(L + V);
    float ndh = max(dot(n, H), 0.0);
#if RTS_Q == 0
    float a2 = rough * rough * rough * rough;
    float e = clamp(2.0 / a2 - 2.0, 1.0, 512.0);
    float3 spec = F0 * pow(ndh, e) * (e + 8.0) * 0.125 * ndl;
    float3 ambSpec = F0 * amb * (1.0 - rough) * 0.5;
#else
    float a = rough * rough, a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d);
    float k = (rough + 1.0) * (rough + 1.0) * 0.125;
    float vis = 0.25 / ((ndl * (1.0 - k) + k) * (ndv * (1.0 - k) + k));
    float3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(L, H), 0.0), 5.0);
    float3 spec = D * vis * F * ndl * 3.14159265;
    float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022), c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    float3 R = reflect(-V, n);
    float3 env = mix(mix(ground, sky, clamp(R.y * 0.5 + 0.5, 0.0, 1.0)), amb, rough);
    float3 ambSpec = env * (F0 * AB.x + AB.y);
#endif
    float3 col = diff * (amb * ao + sun * ndl * sh) + sun * spec * sh + ambSpec * ao;
    col += amb * base.rgb * pow(1.0 - ndv, 3.0) * 0.25;
    col += diff * dynamicLight(lu, tex7, s7, in.wpos, n, V, (1.0 - rough) * 0.5);
    if (m.uv1.y > 0.5) col = base.rgb;
    col += emis * (1.0 + lu.info.w) + u.team.a * float3(1.0, 0.85, 0.7);
    float dist = length(f.camPos.xyz - in.wpos);
    col = mix(col, f.fog.rgb * f.fog.rgb, 1.0 - exp(-dist * f.fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    return float4(sqrt(col), base.a * u.tint.a);
}
)";

inline const char* kModelShadowMSL =
    RTS_MSL_MODEL_BLOCKS
    R"(
struct VIn {
    float3 pos [[attribute(0)]];
    float2 uv [[attribute(2)]];
#if defined(SKINNED)
    uchar4 joints [[attribute(5)]];
    float4 weights [[attribute(6)]];
#endif
};
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vs_main(VIn in [[stage_in]], constant MU0& u [[buffer(1)]], constant U1& f [[buffer(2)]], constant MU2& m [[buffer(3)]]
#if defined(SKINNED)
                    , constant MU3& sk [[buffer(4)]]
#endif
                    ) {
    float4 p = float4(in.pos, 1.0);
#if defined(SKINNED)
    float4 w = in.weights / max(dot(in.weights, float4(1.0)), 1e-5);
    int4 j = int4(in.joints) * 3;
    float4 r0 = sk.joints[j.x] * w.x + sk.joints[j.y] * w.y + sk.joints[j.z] * w.z + sk.joints[j.w] * w.w;
    float4 r1 = sk.joints[j.x + 1] * w.x + sk.joints[j.y + 1] * w.y + sk.joints[j.z + 1] * w.z + sk.joints[j.w + 1] * w.w;
    float4 r2 = sk.joints[j.x + 2] * w.x + sk.joints[j.y + 2] * w.y + sk.joints[j.z + 2] * w.z + sk.joints[j.w + 2] * w.w;
    p = float4(dot(r0, p), dot(r1, p), dot(r2, p), 1.0);
#endif
    VOut o;
    float c = cos(m.uv1.x), s = sin(m.uv1.x);
    float2 uv = in.uv * m.uv0.zw;
    o.uv = float2(c * uv.x + s * uv.y, -s * uv.x + c * uv.y) + m.uv0.xy;
    o.pos = f.lightViewProj * (u.model * p);
    return o;
}
#if defined(ALPHAMASK)
fragment void fs_main(VOut in [[stage_in]], constant MU2& m [[buffer(3)]],
                      texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]]) {
    if (m.base.a * tex1.sample(s1, in.uv).a < m.misc.x) discard_fragment();
}
#endif
)";

} // namespace rts::render::shaders
