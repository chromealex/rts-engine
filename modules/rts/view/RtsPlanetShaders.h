// Planet shaders (RtsPlanetRender): cube-sphere terrain + ocean, atmosphere shell, sky (starfield + star
// glow). GLSL ES 3.00 bodies (backend adds #version) and MSL with the same uniform layouts.
//
// Terrain ("#define PLANET_TIER n" prepended: 0 vertex colours, 1+ triplanar splat + slope cliff texture):
//   vertex: pos (chunk-relative), nrm, col (UByte4N rgb, a unused), morph (Float3: position on the coarser
//           CDLOD grid, chunk-relative), uv (fog-of-war atlas uv), splat (UByte4N layer weights)
//   U0: mat4 model (chunk -> view world), vec4 morph (x start, y end camera distance, z ocean radius,
//       w mode 0 terrain / 1 ocean), vec4 body (xyz centre in view world, w radius), vec4 opts (x specular,
//       y splat on, z fog of war on, w haze strength), vec4 scale (4 layers), vec4 tint[4], vec4 cliff
//       (x scale, y splat strength, z slope start, w slope end), vec4 cliffTint, vec4 water (rgba),
//       vec4 fog (x explored, y unexplored, zw atlas size in cells), vec4 fogColor (rgb, w hard cells),
//       vec4 atmo (rgb haze colour, w night ambient), vec4 sun (xyz direction to the star, w shadows on)
//   U1 / U2 / tex0 / tex1: Renderer3D::bindLitFrame (sun = direction to the star)
//   tex2 vision atlas (R8: 3 x 2 faces of N x N cells)   tex5..8 layer albedo   tex13 cliff albedo
#pragma once
#include "../../../engine/render/Shaders.h"

namespace rts::game::shaders {

#define RTS_PLANET_GLSL_U0 \
    "layout(std140) uniform U0 { mat4 p_model; vec4 p_morph; vec4 p_body; vec4 p_opts; vec4 p_scale; vec4 p_tint[4];\n" \
    "  vec4 p_cliff; vec4 p_cliffTint; vec4 p_water; vec4 p_fog; vec4 p_fogColor; vec4 p_atmo; vec4 p_sun; };\n"
#define RTS_PLANET_MSL_U0 \
    "struct PU0 { float4x4 model; float4 morph; float4 body; float4 opts; float4 scale; float4 tint[4];\n" \
    "  float4 cliff; float4 cliffTint; float4 water; float4 fog; float4 fogColor; float4 atmo; float4 sun; };\n"

inline const char* kPlanetVS =
    RTS_PLANET_GLSL_U0 RTS_GLSL_FRAME
    R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec3 a_morph;
layout(location = 4) in vec2 a_uv;
layout(location = 5) in vec4 a_splat;
out vec3 v_nrm;
out vec3 v_wpos;
out vec3 v_col;
out vec2 v_uv;
out vec4 v_splat;
void main() {
    vec3 wp = (p_model * vec4(a_pos, 1.0)).xyz;
    vec3 wm = (p_model * vec4(a_morph, 1.0)).xyz;
    float dist = length(u_camPos.xyz - wp);
    float k = clamp((dist - p_morph.x) / max(p_morph.y - p_morph.x, 0.001), 0.0, 1.0);
    wp = mix(wp, wm, k);
    vec3 n = mat3(p_model) * a_nrm;
    if (p_morph.w > 0.5) { // ocean: the same chunk projected onto the sea sphere
        n = normalize(wp - p_body.xyz);
        wp = p_body.xyz + n * p_morph.z;
    }
    v_wpos = wp;
    v_nrm = n;
    v_col = a_col.rgb;
    v_uv = a_uv;
    v_splat = a_splat;
    gl_Position = u_viewProj * vec4(wp, 1.0);
}
)";

inline const char* kPlanetFS =
    RTS_PLANET_GLSL_U0 RTS_GLSL_FRAME RTS_GLSL_SHADOW RTS_GLSL_LIGHTS
    R"(
uniform mediump sampler2D tex2;
uniform sampler2D tex5;
uniform sampler2D tex6;
uniform sampler2D tex7;
uniform sampler2D tex8;
uniform sampler2D tex13;
in vec3 v_nrm;
in vec3 v_wpos;
in vec3 v_col;
in vec2 v_uv;
in vec4 v_splat;
out vec4 o_color;

#if PLANET_TIER >= 1
vec3 triplanar(sampler2D t, vec3 q, vec3 bw) { return texture(t, q.zy).rgb * bw.x + texture(t, q.xz).rgb * bw.y + texture(t, q.xy).rgb * bw.z; }
#endif

void main() {
    vec3 n = normalize(v_nrm);
    vec3 up = normalize(v_wpos - p_body.xyz);
    bool ocean = p_morph.w > 0.5;
    vec3 albedoS = ocean ? p_water.rgb : v_col;
#if PLANET_TIER >= 1
    if (!ocean && p_opts.y > 0.5) {
        vec3 q = v_wpos - p_body.xyz;            // body-local (rotation of the view frame only)
        vec3 bw = pow(abs(n), vec3(4.0));
        bw /= bw.x + bw.y + bw.z;
        vec4 w = v_splat / max(v_splat.r + v_splat.g + v_splat.b + v_splat.a, 0.0001);
        vec3 s = vec3(0.0);
        if (w.r > 0.002) s += w.r * triplanar(tex5, q / p_scale.x, bw) * p_tint[0].rgb;
        if (w.g > 0.002) s += w.g * triplanar(tex6, q / p_scale.y, bw) * p_tint[1].rgb;
        if (w.b > 0.002) s += w.b * triplanar(tex7, q / p_scale.z, bw) * p_tint[2].rgb;
        if (w.a > 0.002) s += w.a * triplanar(tex8, q / p_scale.w, bw) * p_tint[3].rgb;
        float cw = smoothstep(p_cliff.z, p_cliff.w, 1.0 - dot(n, up));
        if (cw > 0.002) s = mix(s, triplanar(tex13, q / p_cliff.x, bw) * p_cliffTint.rgb, cw);
        albedoS = mix(v_col, s, p_cliff.y);
    }
#endif
    vec3 base = albedoS * albedoS;
    vec3 L = normalize(p_sun.xyz);
    vec3 V = normalize(u_camPos.xyz - v_wpos);
    float ndl = max(dot(n, L), 0.0);
    float sh = ndl > 0.0 && p_sun.w > 0.5 ? shadowAt(v_wpos, n) : 1.0;
    sh *= cloudShade(v_wpos);
    float day = mix(p_atmo.w, 1.0, smoothstep(-0.25, 0.3, dot(up, L)));   // ambient fades on the night side
    vec3 sun = u_sunColor.rgb * u_sunColor.rgb * u_sunDir.w;
    vec3 amb = mix(u_groundColor.rgb * u_groundColor.rgb, u_skyColor.rgb * u_skyColor.rgb, dot(n, up) * 0.5 + 0.5) * day;
    vec3 H = normalize(L + V);
    float specK = ocean ? 0.9 : p_opts.x;
    float spec = pow(max(dot(n, H), 0.0), ocean ? 80.0 : 40.0) * specK;
    float rim = pow(1.0 - max(dot(n, V), 0.0), 3.0) * 0.25;
    vec3 col = base * (amb + sun * ndl * sh) + sun * spec * sh + amb * rim * base;
    col += base * dynamicLight(v_wpos, n, V, specK);
    float dist = length(u_camPos.xyz - v_wpos);
    col = mix(col, u_fog.rgb * u_fog.rgb, 1.0 - exp(-dist * u_fog.w));
    col = mix(col, p_atmo.rgb * p_atmo.rgb * day, (1.0 - exp(-dist * p_opts.w)) * 0.6);   // aerial haze
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    vec3 outc = sqrt(col);
    float alpha = ocean ? mix(p_water.a, 1.0, pow(1.0 - max(dot(n, V), 0.0), 4.0)) : 1.0;
    if (p_opts.z > 0.5) {
        vec2 uv = v_uv;
        if (p_fogColor.w > 0.5) uv = (floor(uv * p_fog.zw) + 0.5) / p_fog.zw;
        float v = texture(tex2, uv).r;
        float a = v < 0.5 ? mix(p_fog.y, p_fog.x, v * 2.0) : mix(p_fog.x, 0.0, (v - 0.5) * 2.0);
        outc = mix(outc, p_fogColor.rgb, a);
        alpha = mix(alpha, 1.0, a);
    }
    o_color = vec4(outc, alpha);
}
)";

inline const char* kPlanetMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n" RTS_PLANET_MSL_U0 RTS_MSL_FRAME RTS_MSL_SHADOW RTS_MSL_LIGHTS
    R"(
struct VIn { float3 pos [[attribute(0)]]; float3 nrm [[attribute(1)]]; float4 col [[attribute(2)]]; float3 morph [[attribute(3)]];
             float2 uv [[attribute(4)]]; float4 splat [[attribute(5)]]; };
struct VOut { float4 pos [[position]]; float3 nrm; float3 wpos; float3 col; float2 uv; float4 splat; };
vertex VOut vs_main(VIn in [[stage_in]], constant PU0& u [[buffer(1)]], constant U1& f [[buffer(2)]]) {
    VOut o;
    float3 wp = (u.model * float4(in.pos, 1.0)).xyz;
    float3 wm = (u.model * float4(in.morph, 1.0)).xyz;
    float dist = length(f.camPos.xyz - wp);
    float k = clamp((dist - u.morph.x) / max(u.morph.y - u.morph.x, 0.001), 0.0, 1.0);
    wp = mix(wp, wm, k);
    float3 n = (u.model * float4(in.nrm, 0.0)).xyz;
    if (u.morph.w > 0.5) {
        n = normalize(wp - u.body.xyz);
        wp = u.body.xyz + n * u.morph.z;
    }
    o.wpos = wp;
    o.nrm = n;
    o.col = in.col.rgb;
    o.uv = in.uv;
    o.splat = in.splat;
    o.pos = f.viewProj * float4(wp, 1.0);
    return o;
}
#if PLANET_TIER >= 1
static float3 triplanar(texture2d<float> t, sampler s, float3 q, float3 bw) {
    return t.sample(s, q.zy).rgb * bw.x + t.sample(s, q.xz).rgb * bw.y + t.sample(s, q.xy).rgb * bw.z;
}
#endif
fragment float4 fs_main(VOut in [[stage_in]], constant PU0& u [[buffer(1)]], constant U1& f [[buffer(2)]],
                        depth2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],
                        constant LightU& l [[buffer(3)]], texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]],
                        texture2d<float> tex2 [[texture(2)]], sampler s2 [[sampler(2)]],
                        texture2d<float> tex5 [[texture(5)]], sampler s5 [[sampler(5)]],
                        texture2d<float> tex6 [[texture(6)]], sampler s6 [[sampler(6)]],
                        texture2d<float> tex7 [[texture(7)]], sampler s7 [[sampler(7)]],
                        texture2d<float> tex8 [[texture(8)]], sampler s8 [[sampler(8)]],
                        texture2d<float> tex13 [[texture(13)]], sampler s13 [[sampler(13)]]) {
    float3 n = normalize(in.nrm);
    float3 up = normalize(in.wpos - u.body.xyz);
    bool ocean = u.morph.w > 0.5;
    float3 albedoS = ocean ? u.water.rgb : in.col;
#if PLANET_TIER >= 1
    if (!ocean && u.opts.y > 0.5) {
        float3 q = in.wpos - u.body.xyz;
        float3 bw = pow(abs(n), float3(4.0));
        bw /= bw.x + bw.y + bw.z;
        float4 w = in.splat / max(in.splat.r + in.splat.g + in.splat.b + in.splat.a, 0.0001);
        float3 s = float3(0.0);
        if (w.r > 0.002) s += w.r * triplanar(tex5, s5, q / u.scale.x, bw) * u.tint[0].rgb;
        if (w.g > 0.002) s += w.g * triplanar(tex6, s6, q / u.scale.y, bw) * u.tint[1].rgb;
        if (w.b > 0.002) s += w.b * triplanar(tex7, s7, q / u.scale.z, bw) * u.tint[2].rgb;
        if (w.a > 0.002) s += w.a * triplanar(tex8, s8, q / u.scale.w, bw) * u.tint[3].rgb;
        float cw = smoothstep(u.cliff.z, u.cliff.w, 1.0 - dot(n, up));
        if (cw > 0.002) s = mix(s, triplanar(tex13, s13, q / u.cliff.x, bw) * u.cliffTint.rgb, cw);
        albedoS = mix(in.col, s, u.cliff.y);
    }
#endif
    float3 base = albedoS * albedoS;
    float3 L = normalize(u.sun.xyz);
    float3 V = normalize(f.camPos.xyz - in.wpos);
    float ndl = max(dot(n, L), 0.0);
    float sh = ndl > 0.0 && u.sun.w > 0.5 ? shadowAt(f, in.wpos, n, in.pos.xy, tex0, s0) : 1.0;
    sh *= cloudShade(l, in.wpos);
    float day = mix(u.atmo.w, 1.0, smoothstep(-0.25, 0.3, dot(up, L)));
    float3 sun = f.sunColor.rgb * f.sunColor.rgb * f.sunDir.w;
    float3 amb = mix(f.groundColor.rgb * f.groundColor.rgb, f.skyColor.rgb * f.skyColor.rgb, dot(n, up) * 0.5 + 0.5) * day;
    float3 H = normalize(L + V);
    float specK = ocean ? 0.9 : u.opts.x;
    float spec = pow(max(dot(n, H), 0.0), ocean ? 80.0 : 40.0) * specK;
    float rim = pow(1.0 - max(dot(n, V), 0.0), 3.0) * 0.25;
    float3 col = base * (amb + sun * ndl * sh) + sun * spec * sh + amb * rim * base;
    col += base * dynamicLight(l, tex1, s1, in.wpos, n, V, specK);
    float dist = length(f.camPos.xyz - in.wpos);
    col = mix(col, f.fog.rgb * f.fog.rgb, 1.0 - exp(-dist * f.fog.w));
    col = mix(col, u.atmo.rgb * u.atmo.rgb * day, (1.0 - exp(-dist * u.opts.w)) * 0.6);
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    float3 outc = sqrt(col);
    float alpha = ocean ? mix(u.water.a, 1.0, pow(1.0 - max(dot(n, V), 0.0), 4.0)) : 1.0;
    if (u.opts.z > 0.5) {
        float2 uv = in.uv;
        if (u.fogColor.w > 0.5) uv = (floor(uv * u.fog.zw) + 0.5) / u.fog.zw;
        float v = tex2.sample(s2, uv).r;
        float a = v < 0.5 ? mix(u.fog.y, u.fog.x, v * 2.0) : mix(u.fog.x, 0.0, (v - 0.5) * 2.0);
        outc = mix(outc, u.fogColor.rgb, a);
        alpha = mix(alpha, 1.0, a);
    }
    return float4(outc, alpha);
}
)";

// ---- sky: one fullscreen triangle behind everything (starfield + glow of the star), and the
// atmosphere shell (a sphere mesh drawn with additive blending, analytic rim scattering).
//   U0 sky:  mat4 invViewProj; vec4 eye (xyz, w star count scale); vec4 star (xyz direction to the star, w glow size);
//            vec4 starColor (rgb, w intensity)
//   U0 atmo: mat4 model (unit sphere -> world); vec4 body (xyz centre, w planet radius); vec4 shell (x outer radius,
//            y strength); vec4 color (rgb); vec4 sun (xyz direction to the star)
#define RTS_SKY_GLSL_U0 "layout(std140) uniform U0 { mat4 s_inv; vec4 s_eye; vec4 s_star; vec4 s_starColor; };\n"
#define RTS_ATMO_GLSL_U0 "layout(std140) uniform U0 { mat4 a_model; vec4 a_body; vec4 a_shell; vec4 a_color; vec4 a_sun; };\n"

inline const char* kSkyVS =
    RTS_SKY_GLSL_U0
    R"(
out vec2 v_ndc;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    v_ndc = p;
    gl_Position = vec4(p, 0.9999, 1.0);
}
)";

inline const char* kSkyFS =
    RTS_SKY_GLSL_U0
    R"(
in vec2 v_ndc;
out vec4 o_color;
float hash3(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
vec3 starLayer(vec3 d, float scale, float density) {
    vec3 q = d * scale;
    vec3 c = floor(q);
    float h = hash3(c);
    if (h > density) return vec3(0.0);
    vec3 sp = c + 0.5 + (vec3(hash3(c + 1.7), hash3(c + 3.1), hash3(c + 5.3)) - 0.5) * 0.7;
    float r = length(q - sp);
    float b = smoothstep(0.12, 0.0, r) * (0.4 + 0.6 * hash3(c + 9.0));
    vec3 tint = mix(vec3(0.75, 0.82, 1.0), vec3(1.0, 0.85, 0.7), hash3(c + 11.0));
    return tint * b;
}
void main() {
    vec4 a = s_inv * vec4(v_ndc, -1.0, 1.0), b = s_inv * vec4(v_ndc, 1.0, 1.0);
    vec3 d = normalize(b.xyz / b.w - a.xyz / a.w);
    vec3 col = vec3(0.004, 0.006, 0.014) + vec3(0.02, 0.018, 0.03) * pow(1.0 - abs(d.y), 6.0);
    col += starLayer(d, 90.0, 0.08 * s_eye.w);
    col += starLayer(d, 180.0, 0.05 * s_eye.w) * 0.7;
    float g = max(dot(d, normalize(s_star.xyz)), 0.0);
    col += s_starColor.rgb * s_starColor.w * (pow(g, s_star.w) * 0.6 + pow(g, s_star.w * 12.0) * 2.0);
    o_color = vec4(sqrt(clamp(col, 0.0, 1.0)), 1.0);
}
)";

inline const char* kSkyMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct SU0 { float4x4 inv; float4 eye; float4 star; float4 starColor; };\n"
    R"(
struct VOut { float4 pos [[position]]; float2 ndc; };
vertex VOut vs_main(uint vid [[vertex_id]], constant SU0& u [[buffer(1)]]) {
    VOut o;
    float2 p = float2(float((vid << 1) & 2), float(vid & 2)) * 2.0 - 1.0;
    o.ndc = p;
    o.pos = float4(p, 0.9999, 1.0);
    return o;
}
static float hash3(float3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
static float3 starLayer(float3 d, float scale, float density) {
    float3 q = d * scale;
    float3 c = floor(q);
    float h = hash3(c);
    if (h > density) return float3(0.0);
    float3 sp = c + 0.5 + (float3(hash3(c + 1.7), hash3(c + 3.1), hash3(c + 5.3)) - 0.5) * 0.7;
    float r = length(q - sp);
    float b = smoothstep(0.12, 0.0, r) * (0.4 + 0.6 * hash3(c + 9.0));
    float3 tint = mix(float3(0.75, 0.82, 1.0), float3(1.0, 0.85, 0.7), hash3(c + 11.0));
    return tint * b;
}
fragment float4 fs_main(VOut in [[stage_in]], constant SU0& u [[buffer(1)]]) {
    float4 a = u.inv * float4(in.ndc, -1.0, 1.0), b = u.inv * float4(in.ndc, 1.0, 1.0);
    float3 d = normalize(b.xyz / b.w - a.xyz / a.w);
    float3 col = float3(0.004, 0.006, 0.014) + float3(0.02, 0.018, 0.03) * pow(1.0 - abs(d.y), 6.0);
    col += starLayer(d, 90.0, 0.08 * u.eye.w);
    col += starLayer(d, 180.0, 0.05 * u.eye.w) * 0.7;
    float g = max(dot(d, normalize(u.star.xyz)), 0.0);
    col += u.starColor.rgb * u.starColor.w * (pow(g, u.star.w) * 0.6 + pow(g, u.star.w * 12.0) * 2.0);
    return float4(sqrt(clamp(col, 0.0, 1.0)), 1.0);
}
)";

inline const char* kAtmoVS =
    RTS_ATMO_GLSL_U0 RTS_GLSL_FRAME
    R"(
layout(location = 0) in vec3 a_pos;
out vec3 v_wpos;
void main() {
    vec4 wp = a_model * vec4(a_pos, 1.0);
    v_wpos = wp.xyz;
    gl_Position = u_viewProj * wp;
}
)";

// Optical depth of the view ray through the shell, minus the part hidden by the planet; lit by the
// star (forward scattering towards the star, a thin bright terminator ring).
inline const char* kAtmoFS =
    RTS_ATMO_GLSL_U0 RTS_GLSL_FRAME
    R"(
in vec3 v_wpos;
out vec4 o_color;
void main() {
    vec3 eye = u_camPos.xyz;
    vec3 d = normalize(v_wpos - eye);
    vec3 oc = eye - a_body.xyz;
    float R = a_shell.x, r = a_body.w;
    float b = dot(oc, d), c = dot(oc, oc) - R * R, disc = b * b - c;
    if (disc <= 0.0) discard;
    float sq = sqrt(disc);
    float t0 = max(-b - sq, 0.0), t1 = -b + sq;
    float cp = dot(oc, oc) - r * r, dp = b * b - cp;
    if (dp > 0.0) { float tp = -b - sqrt(dp); if (tp > 0.0) t1 = min(t1, tp); }   // ground hides the far side
    float len = max(t1 - t0, 0.0) / (2.0 * sqrt(max(R * R - r * r, 1e-4)));      // 0..~1 through the limb
    vec3 mid = oc + d * (0.5 * (t0 + t1));
    vec3 up = normalize(mid);
    vec3 L = normalize(a_sun.xyz);
    float lit = smoothstep(-0.35, 0.25, dot(up, L));
    float fwd = pow(max(dot(d, L), 0.0), 6.0);
    float k = clamp(len, 0.0, 1.0);
    vec3 col = a_color.rgb * (k * k * 1.4 + k * 0.25) * lit * a_shell.y + vec3(1.0, 0.75, 0.5) * fwd * k * lit * 0.5 * a_shell.y;
    o_color = vec4(col, 1.0);
}
)";

inline const char* kAtmoMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct AU0 { float4x4 model; float4 body; float4 shell; float4 color; float4 sun; };\n" RTS_MSL_FRAME
    R"(
struct VIn { float3 pos [[attribute(0)]]; };
struct VOut { float4 pos [[position]]; float3 wpos; };
vertex VOut vs_main(VIn in [[stage_in]], constant AU0& u [[buffer(1)]], constant U1& f [[buffer(2)]]) {
    VOut o;
    float4 wp = u.model * float4(in.pos, 1.0);
    o.wpos = wp.xyz;
    o.pos = f.viewProj * wp;
    return o;
}
fragment float4 fs_main(VOut in [[stage_in]], constant AU0& u [[buffer(1)]], constant U1& f [[buffer(2)]]) {
    float3 eye = f.camPos.xyz;
    float3 d = normalize(in.wpos - eye);
    float3 oc = eye - u.body.xyz;
    float R = u.shell.x, r = u.body.w;
    float b = dot(oc, d), c = dot(oc, oc) - R * R, disc = b * b - c;
    if (disc <= 0.0) discard_fragment();
    float sq = sqrt(disc);
    float t0 = max(-b - sq, 0.0), t1 = -b + sq;
    float cp = dot(oc, oc) - r * r, dp = b * b - cp;
    if (dp > 0.0) { float tp = -b - sqrt(dp); if (tp > 0.0) t1 = min(t1, tp); }
    float len = max(t1 - t0, 0.0) / (2.0 * sqrt(max(R * R - r * r, 1e-4)));
    float3 mid = oc + d * (0.5 * (t0 + t1));
    float3 up = normalize(mid);
    float3 L = normalize(u.sun.xyz);
    float lit = smoothstep(-0.35, 0.25, dot(up, L));
    float fwd = pow(max(dot(d, L), 0.0), 6.0);
    float k = clamp(len, 0.0, 1.0);
    float3 col = u.color.rgb * (k * k * 1.4 + k * 0.25) * lit * u.shell.y + float3(1.0, 0.75, 0.5) * fwd * k * lit * 0.5 * u.shell.y;
    return float4(col, 1.0);
}
)";

} // namespace rts::game::shaders
