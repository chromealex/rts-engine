// Model shaders (GLSL ES 3.00 + MSL), compiled as permutations: the renderer prepends
//   #define RTS_INST n    instances per draw (1, or 32 for instanced batches)
//   #define SKINNED      GPU skinning from a joint palette in U3 (80 joints, 4 influences)
//   #define BAKED        GPU skinning from baked clips: joint matrices in tex7, rows per instance
//   #define MORPH        morph targets: deltas in tex8, weights in U4
//   #define NORMALMAP    tangent-space normal map (tex3) with vertex tangents
//   #define ALPHAMASK    alpha test against the material cutoff
//   #define OUTLINE      selection outline: hull extruded along normals, flat colour
//   #define RTS_Q 0|1|2  material quality: 0 Blinn-Phong, factors only (weak GPUs)
//                        1 GGX + metallic-roughness / emissive textures + normal maps
//                        2 + occlusion texture
// Uniforms (std140 / MSL layouts match):
//   U0 per draw:  Inst inst[RTS_INST]: mat4 model; vec4 tint (rgb multiply, a opacity);
//                 vec4 team (rgb team colour / outline colour, a hit flash);
//                 vec4 draw (x receive shadow, y dissolve 0..1, z outline width);
//                 vec4 anim (x baked row A, y baked row B, z A->B mix)
//   U1 per frame: as the lit shader (Shaders.h)
//   U2 material:  vec4 base; vec4 emissive (rgb, linear, premultiplied by strength);
//                 vec4 pbr (metallic, roughness, normalScale, occlusionStrength);
//                 vec4 misc (alphaCutoff, teamMode, teamStrength, useVertexColor);
//                 vec4 uv0 (offset.xy, scale.xy); vec4 uv1 (rotation, unlit, -, -)
//   U3 skin:      SKINNED vec4 joints[240] (3 rows of a 3x4 matrix per joint);
//                 BAKED   vec4 palette[20] (palette slot -> skin joint, 4 per vec4)
//   U4 morph:     vec4 w[2] (8 weights); vec4 info (first texel, first vertex, vertex count, targets);
//                 vec4 info2 (texture width)
// Textures: tex0 shadow map, tex1 base colour (sRGB), tex2 metallic-roughness (G rough, B metal),
//           tex3 normal, tex4 occlusion (R), tex5 emissive (sRGB), tex6 team mask (R),
//           tex7 baked joints (RGBA32F), tex8 morph deltas (RGBA32F)
// Dynamic lights (LightShaders.h): U5 light block, tex9 light map.
#pragma once
#include "Shaders.h"

namespace rts::render::shaders {

#define RTS_GLSL_MODEL_BLOCKS \
    "#if !defined(RTS_INST)\n#define RTS_INST 1\n#endif\n" \
    "struct Inst { mat4 model; vec4 tint; vec4 team; vec4 draw; vec4 anim; };\n" \
    "layout(std140) uniform U0 { Inst u_inst[RTS_INST]; };\n" \
    RTS_GLSL_FRAME \
    "layout(std140) uniform U2 { vec4 m_base; vec4 m_emissive; vec4 m_pbr; vec4 m_misc; vec4 m_uv0; vec4 m_uv1; };\n" \
    "#if defined(SKINNED)\n" \
    "layout(std140) uniform U3 { vec4 u_joints[240]; };\n" \
    "#elif defined(BAKED)\n" \
    "layout(std140) uniform U3 { vec4 u_palette[20]; };\n" \
    "#endif\n" \
    "#if defined(MORPH)\n" \
    "layout(std140) uniform U4 { vec4 u_morphW[2]; vec4 u_morphInfo; vec4 u_morphInfo2; };\n" \
    "#endif\n"

// Vertex deformation shared by the colour and shadow vertex shaders: morph, then skin.
#define RTS_GLSL_MODEL_DEFORM \
    "#if defined(BAKED)\n" \
    "uniform highp sampler2D tex7;\n" \
    "vec4 boneRow(int row, int joint, int r) { return texelFetch(tex7, ivec2(joint * 3 + r, row), 0); }\n" \
    "#endif\n" \
    "#if defined(MORPH)\n" \
    "uniform highp sampler2D tex8;\n" \
    "vec4 morphTexel(int i) { int w = int(u_morphInfo2.x); return texelFetch(tex8, ivec2(i % w, i / w), 0); }\n" \
    "#endif\n" \
    "void deform(Inst I, inout vec4 p, inout vec3 n, inout vec3 t) {\n" \
    "#if defined(MORPH)\n" \
    "    int vid = gl_VertexID - int(u_morphInfo.y), cnt = int(u_morphInfo.z), nt = int(u_morphInfo.w), base = int(u_morphInfo.x);\n" \
    "    for (int k = 0; k < 8; ++k) {\n" \
    "        if (k >= nt) break;\n" \
    "        float wt = u_morphW[k / 4][k % 4];\n" \
    "        if (wt == 0.0) continue;\n" \
    "        int i = base + (k * cnt + vid) * 2;\n" \
    "        p.xyz += wt * morphTexel(i).xyz;\n" \
    "        n += wt * morphTexel(i + 1).xyz;\n" \
    "    }\n" \
    "#endif\n" \
    "#if defined(SKINNED) || defined(BAKED)\n" \
    "    vec4 w = a_weights / max(dot(a_weights, vec4(1.0)), 1e-5);\n" \
    "    vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0);\n" \
    "    for (int k = 0; k < 4; ++k) {\n" \
    "        if (w[k] <= 0.0) continue;\n" \
    "        int slot = int(a_joints[k] + 0.5);\n" \
    "#if defined(SKINNED)\n" \
    "        r0 += u_joints[slot * 3] * w[k]; r1 += u_joints[slot * 3 + 1] * w[k]; r2 += u_joints[slot * 3 + 2] * w[k];\n" \
    "#else\n" \
    "        int joint = int(u_palette[slot / 4][slot % 4] + 0.5);\n" \
    "        int ra = int(I.anim.x + 0.5), rb = int(I.anim.y + 0.5);\n" \
    "        r0 += mix(boneRow(ra, joint, 0), boneRow(rb, joint, 0), I.anim.z) * w[k];\n" \
    "        r1 += mix(boneRow(ra, joint, 1), boneRow(rb, joint, 1), I.anim.z) * w[k];\n" \
    "        r2 += mix(boneRow(ra, joint, 2), boneRow(rb, joint, 2), I.anim.z) * w[k];\n" \
    "#endif\n" \
    "    }\n" \
    "    p = vec4(dot(r0, p), dot(r1, p), dot(r2, p), 1.0);\n" \
    "    n = vec3(dot(r0.xyz, n), dot(r1.xyz, n), dot(r2.xyz, n));\n" \
    "    t = vec3(dot(r0.xyz, t), dot(r1.xyz, t), dot(r2.xyz, t));\n" \
    "#endif\n" \
    "}\n" \
    "vec2 uvTransform(vec2 uv0) {\n" \
    "    float c = cos(m_uv1.x), s = sin(m_uv1.x);\n" \
    "    vec2 uv = uv0 * m_uv0.zw;\n" \
    "    return vec2(c * uv.x + s * uv.y, -s * uv.x + c * uv.y) + m_uv0.xy;\n" \
    "}\n"

#define RTS_GLSL_MODEL_ATTRIBS \
    "layout(location = 0) in vec3 a_pos;\n" \
    "layout(location = 1) in vec3 a_nrm;\n" \
    "layout(location = 2) in vec2 a_uv;\n" \
    "layout(location = 3) in vec4 a_tan;\n" \
    "layout(location = 4) in vec4 a_col;\n" \
    "#if defined(SKINNED) || defined(BAKED)\n" \
    "layout(location = 5) in vec4 a_joints;\n" \
    "layout(location = 6) in vec4 a_weights;\n" \
    "#endif\n" \
    "#if RTS_INST > 1\n#define INSTANCE u_inst[gl_InstanceID]\n#else\n#define INSTANCE u_inst[0]\n#endif\n"

// ---------------------------------------------------------------- GLSL

inline const char* kModelVS =
    RTS_GLSL_MODEL_BLOCKS RTS_GLSL_MODEL_ATTRIBS RTS_GLSL_MODEL_DEFORM
    R"(
out vec3 v_wpos;
out vec3 v_opos;
out vec3 v_nrm;
out vec2 v_uv;
out vec4 v_col;
out vec4 v_tan;
flat out vec4 v_tint;
flat out vec4 v_team;
flat out vec4 v_draw;
void main() {
    Inst I = INSTANCE;
    vec4 p = vec4(a_pos, 1.0);
    vec3 n = a_nrm, t = a_tan.xyz;
    deform(I, p, n, t);
    v_opos = p.xyz;
    // normal matrix = cofactor of the upper 3x3 (inverse transpose up to scale)
    vec3 c0 = I.model[0].xyz, c1 = I.model[1].xyz, c2 = I.model[2].xyz;
    mat3 cof = mat3(cross(c1, c2), cross(c2, c0), cross(c0, c1));
    float sgn = dot(c0, cross(c1, c2)) < 0.0 ? -1.0 : 1.0;
    vec4 wp = I.model * p;
    v_nrm = cof * n * sgn;
#if defined(OUTLINE)
    wp.xyz += normalize(v_nrm) * I.draw.z;
#endif
    v_wpos = wp.xyz;
    v_tan = vec4(mat3(I.model) * t, a_tan.w);
    v_uv = uvTransform(a_uv);
    v_col = a_col;
    v_tint = I.tint; v_team = I.team; v_draw = I.draw;
    gl_Position = u_viewProj * wp;
}
)";

inline const char* kModelFS =
    RTS_GLSL_MODEL_BLOCKS RTS_GLSL_SHADOW RTS_GLSL_LIGHTS_AT("U5", "tex9")
    R"(
uniform mediump sampler2D tex1;
uniform mediump sampler2D tex2;
uniform mediump sampler2D tex3;
uniform mediump sampler2D tex4;
uniform mediump sampler2D tex5;
uniform mediump sampler2D tex6;
in vec3 v_wpos;
in vec3 v_opos;
in vec3 v_nrm;
in vec2 v_uv;
in vec4 v_col;
in vec4 v_tan;
flat in vec4 v_tint;
flat in vec4 v_team;
flat in vec4 v_draw;
out vec4 o_color;

vec3 toLinear(vec3 c) { return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878); }
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float hash3(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float noise3(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1, 0, 0)), f.x), mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), f.x), mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

void main() {
#if defined(OUTLINE)
    o_color = vec4(sqrt(v_team.rgb), v_tint.a);
#else
    float edge = 0.0;
    if (v_draw.y > 0.0) { // dissolve (spawn / death / construction), noise fixed to the object
        float nz = noise3(v_opos * 7.0) * 0.7 + noise3(v_opos * 19.0) * 0.3;
        if (nz < v_draw.y) discard;
        edge = 1.0 - smoothstep(0.0, 0.06, nz - v_draw.y);
    }
    vec4 tb = texture(tex1, v_uv);
    vec4 base = m_base * vec4(toLinear(tb.rgb), tb.a);
    if (m_misc.w > 0.5) base *= v_col;
    int teamMode = int(m_misc.y + 0.5);
    if (teamMode == 1) {
        base.rgb = mix(base.rgb, base.rgb * v_team.rgb, m_misc.z);
    } else if (teamMode > 1) {
        float m = 0.0, k = 1.0;
        if (teamMode == 2) { m = texture(tex6, v_uv).r; k = min(0.5 + luma(base.rgb) * 1.5, 1.0); }
        else if (teamMode == 3) { m = 1.0 - tb.a; base.a = m_base.a; }
        else { m = 1.0 - v_col.a; base.a = m_base.a * tb.a; }
        base.rgb = mix(base.rgb, v_team.rgb * k, clamp(m * m_misc.z, 0.0, 1.0));
    }
    base.rgb *= v_tint.rgb;
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
    float sh = (ndl > 0.0 && v_draw.x > 0.5) ? shadowAt(v_wpos, normalize(v_nrm)) : 1.0;
    sh *= cloudShade(v_wpos);
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
    { vec3 dd, ds; dynLight(v_wpos, n, V, rough, dd, ds); col += diff * dd + (F0 + 0.15 * (1.0 - rough) * (1.0 - metallic)) * ds; }   // point lights + light reflections (Lights.h)
    if (m_uv1.y > 0.5) col = base.rgb;
    col += emis * (1.0 + nightGlow()) + v_team.a * vec3(1.0, 0.85, 0.7) + edge * vec3(1.0, 0.45, 0.12) * 3.0;
    float dist = length(u_camPos.xyz - v_wpos);
    col = mix(col, u_fog.rgb * u_fog.rgb, 1.0 - exp(-dist * u_fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    o_color = vec4(sqrt(col), base.a * v_tint.a);
#endif
}
)";

inline const char* kModelShadowVS =
    RTS_GLSL_MODEL_BLOCKS RTS_GLSL_MODEL_ATTRIBS RTS_GLSL_MODEL_DEFORM
    R"(
out vec2 v_uv;
void main() {
    Inst I = INSTANCE;
    vec4 p = vec4(a_pos, 1.0);
    vec3 n = a_nrm, t = a_tan.xyz;
    deform(I, p, n, t);
    v_uv = uvTransform(a_uv);
    gl_Position = u_lightViewProj * (I.model * p);
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

#define RTS_MSL_MODEL_BLOCKS \
    "#include <metal_stdlib>\nusing namespace metal;\n" \
    "#if !defined(RTS_INST)\n#define RTS_INST 1\n#endif\n" \
    "struct Inst { float4x4 model; float4 tint; float4 team; float4 draw; float4 anim; };\n" \
    "struct MU0 { Inst inst[RTS_INST]; };\n" \
    RTS_MSL_FRAME \
    "struct MU2 { float4 base; float4 emissive; float4 pbr; float4 misc; float4 uv0; float4 uv1; };\n" \
    "struct MU3 { float4 v[240]; };\n" \
    "struct MU4 { float4 w[2]; float4 info; float4 info2; };\n"

// Vertex inputs, argument list and deformation shared by the colour and shadow vertex functions.
#define RTS_MSL_MODEL_VERTEX \
    "struct VIn {\n" \
    "    float3 pos [[attribute(0)]];\n" \
    "    float3 nrm [[attribute(1)]];\n" \
    "    float2 uv [[attribute(2)]];\n" \
    "    float4 tan [[attribute(3)]];\n" \
    "    float4 col [[attribute(4)]];\n" \
    "#if defined(SKINNED) || defined(BAKED)\n" \
    "    uchar4 joints [[attribute(5)]];\n" \
    "    float4 weights [[attribute(6)]];\n" \
    "#endif\n" \
    "};\n" \
    "#if defined(SKINNED) || defined(BAKED)\n#define SKIN_ARG , constant MU3& sk [[buffer(4)]]\n#else\n#define SKIN_ARG\n#endif\n" \
    "#if defined(BAKED)\n#define BAKED_ARG , texture2d<float> tex7 [[texture(7)]]\n#else\n#define BAKED_ARG\n#endif\n" \
    "#if defined(MORPH)\n#define MORPH_ARG , constant MU4& mo [[buffer(5)]], texture2d<float> tex8 [[texture(8)]]\n#else\n#define MORPH_ARG\n#endif\n" \
    "#define VERTEX_ARGS VIn in [[stage_in]], uint iid [[instance_id]], uint vid [[vertex_id]], constant MU0& u [[buffer(1)]], " \
    "constant U1& f [[buffer(2)]], constant MU2& m [[buffer(3)]] SKIN_ARG BAKED_ARG MORPH_ARG\n" \
    "#if defined(SKINNED) || defined(BAKED)\n#define SKIN_PASS , sk\n#else\n#define SKIN_PASS\n#endif\n" \
    "#if defined(BAKED)\n#define BAKED_PASS , tex7\n#else\n#define BAKED_PASS\n#endif\n" \
    "#if defined(MORPH)\n#define MORPH_PASS , mo, tex8\n#else\n#define MORPH_PASS\n#endif\n" \
    "#define DEFORM_PARAMS VIn in, uint vid, Inst I SKIN_ARG_P BAKED_ARG_P MORPH_ARG_P\n" \
    "#if defined(SKINNED) || defined(BAKED)\n#define SKIN_ARG_P , constant MU3& sk\n#else\n#define SKIN_ARG_P\n#endif\n" \
    "#if defined(BAKED)\n#define BAKED_ARG_P , texture2d<float> tex7\n#else\n#define BAKED_ARG_P\n#endif\n" \
    "#if defined(MORPH)\n#define MORPH_ARG_P , constant MU4& mo, texture2d<float> tex8\n#else\n#define MORPH_ARG_P\n#endif\n" \
    "struct Deformed { float4 p; float3 n; float3 t; };\n" \
    "static Deformed deform(DEFORM_PARAMS) {\n" \
    "    Deformed d; d.p = float4(in.pos, 1.0); d.n = in.nrm; d.t = in.tan.xyz;\n" \
    "#if defined(MORPH)\n" \
    "    int w = int(mo.info2.x), cnt = int(mo.info.z), nt = int(mo.info.w), base = int(mo.info.x);\n" \
    "    int lv = int(vid) - int(mo.info.y);\n" \
    "    for (int k = 0; k < 8; ++k) {\n" \
    "        if (k >= nt) break;\n" \
    "        float wt = mo.w[k / 4][k % 4];\n" \
    "        if (wt == 0.0) continue;\n" \
    "        int i = base + (k * cnt + lv) * 2;\n" \
    "        d.p.xyz += wt * tex8.read(uint2(i % w, i / w)).xyz;\n" \
    "        d.n += wt * tex8.read(uint2((i + 1) % w, (i + 1) / w)).xyz;\n" \
    "    }\n" \
    "#endif\n" \
    "#if defined(SKINNED) || defined(BAKED)\n" \
    "    float4 wv = in.weights / max(dot(in.weights, float4(1.0)), 1e-5);\n" \
    "    float4 r0 = float4(0.0), r1 = float4(0.0), r2 = float4(0.0);\n" \
    "    for (int k = 0; k < 4; ++k) {\n" \
    "        if (wv[k] <= 0.0) continue;\n" \
    "        int slot = int(in.joints[k]);\n" \
    "#if defined(SKINNED)\n" \
    "        r0 += sk.v[slot * 3] * wv[k]; r1 += sk.v[slot * 3 + 1] * wv[k]; r2 += sk.v[slot * 3 + 2] * wv[k];\n" \
    "#else\n" \
    "        uint joint = uint(sk.v[slot / 4][slot % 4] + 0.5);\n" \
    "        uint ra = uint(I.anim.x + 0.5), rb = uint(I.anim.y + 0.5);\n" \
    "        r0 += mix(tex7.read(uint2(joint * 3, ra)), tex7.read(uint2(joint * 3, rb)), I.anim.z) * wv[k];\n" \
    "        r1 += mix(tex7.read(uint2(joint * 3 + 1, ra)), tex7.read(uint2(joint * 3 + 1, rb)), I.anim.z) * wv[k];\n" \
    "        r2 += mix(tex7.read(uint2(joint * 3 + 2, ra)), tex7.read(uint2(joint * 3 + 2, rb)), I.anim.z) * wv[k];\n" \
    "#endif\n" \
    "    }\n" \
    "    d.p = float4(dot(r0, d.p), dot(r1, d.p), dot(r2, d.p), 1.0);\n" \
    "    d.n = float3(dot(r0.xyz, d.n), dot(r1.xyz, d.n), dot(r2.xyz, d.n));\n" \
    "    d.t = float3(dot(r0.xyz, d.t), dot(r1.xyz, d.t), dot(r2.xyz, d.t));\n" \
    "#endif\n" \
    "    return d;\n" \
    "}\n" \
    "static float2 uvTransform(constant MU2& m, float2 uv0) {\n" \
    "    float c = cos(m.uv1.x), s = sin(m.uv1.x);\n" \
    "    float2 uv = uv0 * m.uv0.zw;\n" \
    "    return float2(c * uv.x + s * uv.y, -s * uv.x + c * uv.y) + m.uv0.xy;\n" \
    "}\n" \
    "#if RTS_INST > 1\n#define INSTANCE u.inst[iid]\n#else\n#define INSTANCE u.inst[0]\n#endif\n"

inline const char* kModelMSL =
    RTS_MSL_MODEL_BLOCKS RTS_MSL_SHADOW RTS_MSL_LIGHTS RTS_MSL_MODEL_VERTEX
    R"(
struct VOut {
    float4 pos [[position]];
    float3 wpos; float3 opos; float3 nrm; float2 uv; float4 col; float4 tan;
    float4 tint [[flat]]; float4 team [[flat]]; float4 draw [[flat]];
};

vertex VOut vs_main(VERTEX_ARGS) {
    Inst I = INSTANCE;
    Deformed d = deform(in, vid, I SKIN_PASS BAKED_PASS MORPH_PASS);
    VOut o;
    float3 c0 = I.model[0].xyz, c1 = I.model[1].xyz, c2 = I.model[2].xyz;
    float3x3 cof = float3x3(cross(c1, c2), cross(c2, c0), cross(c0, c1));
    float sgn = dot(c0, cross(c1, c2)) < 0.0 ? -1.0 : 1.0;
    float4 wp = I.model * d.p;
    o.opos = d.p.xyz;
    o.nrm = cof * d.n * sgn;
#if defined(OUTLINE)
    wp.xyz += normalize(o.nrm) * I.draw.z;
#endif
    o.wpos = wp.xyz;
    o.tan = float4((I.model * float4(d.t, 0.0)).xyz, in.tan.w);
    o.uv = uvTransform(m, in.uv);
    o.col = in.col;
    o.tint = I.tint; o.team = I.team; o.draw = I.draw;
    o.pos = f.viewProj * wp;
    return o;
}

static float3 toLinear(float3 c) { return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878); }
static float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
static float hash3(float3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
static float noise3(float3 x) {
    float3 i = floor(x), fr = fract(x);
    fr = fr * fr * (3.0 - 2.0 * fr);
    return mix(mix(mix(hash3(i), hash3(i + float3(1, 0, 0)), fr.x), mix(hash3(i + float3(0, 1, 0)), hash3(i + float3(1, 1, 0)), fr.x), fr.y),
               mix(mix(hash3(i + float3(0, 0, 1)), hash3(i + float3(1, 0, 1)), fr.x), mix(hash3(i + float3(0, 1, 1)), hash3(i + float3(1, 1, 1)), fr.x), fr.y), fr.z);
}

fragment float4 fs_main(VOut in [[stage_in]], bool frontFacing [[front_facing]],
                        constant U1& f [[buffer(2)]], constant MU2& m [[buffer(3)]],
                        depth2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],
                        texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]],
                        texture2d<float> tex2 [[texture(2)]], sampler s2 [[sampler(2)]],
                        texture2d<float> tex3 [[texture(3)]], sampler s3 [[sampler(3)]],
                        texture2d<float> tex4 [[texture(4)]], sampler s4 [[sampler(4)]],
                        texture2d<float> tex5 [[texture(5)]], sampler s5 [[sampler(5)]],
                        texture2d<float> tex6 [[texture(6)]], sampler s6 [[sampler(6)]],
                        constant LightU& lu [[buffer(6)]], texture2d<float> tex9 [[texture(9)]], sampler s9 [[sampler(9)]]) {
#if defined(OUTLINE)
    return float4(sqrt(in.team.rgb), in.tint.a);
#else
    float edge = 0.0;
    if (in.draw.y > 0.0) {
        float nz = noise3(in.opos * 7.0) * 0.7 + noise3(in.opos * 19.0) * 0.3;
        if (nz < in.draw.y) discard_fragment();
        edge = 1.0 - smoothstep(0.0, 0.06, nz - in.draw.y);
    }
    float4 tb = tex1.sample(s1, in.uv);
    float4 base = m.base * float4(toLinear(tb.rgb), tb.a);
    if (m.misc.w > 0.5) base *= in.col;
    int teamMode = int(m.misc.y + 0.5);
    if (teamMode == 1) {
        base.rgb = mix(base.rgb, base.rgb * in.team.rgb, m.misc.z);
    } else if (teamMode > 1) {
        float tm = 0.0, k = 1.0;
        if (teamMode == 2) { tm = tex6.sample(s6, in.uv).r; k = min(0.5 + luma(base.rgb) * 1.5, 1.0); }
        else if (teamMode == 3) { tm = 1.0 - tb.a; base.a = m.base.a; }
        else { tm = 1.0 - in.col.a; base.a = m.base.a * tb.a; }
        base.rgb = mix(base.rgb, in.team.rgb * k, clamp(tm * m.misc.z, 0.0, 1.0));
    }
    base.rgb *= in.tint.rgb;
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
    float sh = (ndl > 0.0 && in.draw.x > 0.5) ? shadowAt(f, in.wpos, normalize(in.nrm), in.pos.xy, tex0, s0) : 1.0;
    sh *= cloudShade(lu, in.wpos);
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
    { float3 dd, ds; dynLight(lu, tex9, s9, in.wpos, n, V, rough, dd, ds); col += diff * dd + (F0 + 0.15 * (1.0 - rough) * (1.0 - metallic)) * ds; }
    if (m.uv1.y > 0.5) col = base.rgb;
    col += emis * (1.0 + lu.info.w) + in.team.a * float3(1.0, 0.85, 0.7) + edge * float3(1.0, 0.45, 0.12) * 3.0;
    float dist = length(f.camPos.xyz - in.wpos);
    col = mix(col, f.fog.rgb * f.fog.rgb, 1.0 - exp(-dist * f.fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    return float4(sqrt(col), base.a * in.tint.a);
#endif
}
)";

inline const char* kModelShadowMSL =
    RTS_MSL_MODEL_BLOCKS RTS_MSL_MODEL_VERTEX
    R"(
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vs_main(VERTEX_ARGS) {
    Inst I = INSTANCE;
    Deformed d = deform(in, vid, I SKIN_PASS BAKED_PASS MORPH_PASS);
    VOut o;
    o.uv = uvTransform(m, in.uv);
    o.pos = f.lightViewProj * (I.model * d.p);
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
