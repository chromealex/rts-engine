// Terrain shader (RtsTerrainRender): the lit shader of Shaders.h (sun + soft shadows RTS_*_SHADOW +
// point lights / clouds RTS_*_LIGHTS + fog + ACES) with terrain-only inputs. Three quality tiers,
// selected by prepending "#define TERRAIN_TIER n" (GLSL after the backend's #version line, MSL
// before the includes):
//   0 Low     vertex colours only - exactly the classic look (weak Mali / Adreno / PowerVR)
//   1 Medium  + splat textures (4 layers, per-cell weights) and a cliff texture on walls / slopes
//   2 High    + normal maps per layer and triplanar cliffs
//
// Vertex: pos, nrm, col (UByte4N), morph (Float2: x = height on the next coarser LOD; CDLOD morph).
//   col.a == 1: static colour. col.a = m/255 with m in 1..15: the colour is the average of the cells
//   around the vertex's grid corner selected by the bit mask m (bit i: cell (X-1 + (i&1), Y-1 + (i>>1)));
//   the vertex shader adds the dynamic tint of those cells (resource fields, creep) read from
//   tex3 - so geometry never changes during a match.
//
//   U0 (per chunk): vec4 lod (x morph start, y morph end (camera distance), z map w, w map h)
//                   vec4 mode (x specular, y splat on, z normal maps on, w vision fog on)
//                   vec4 scale (world units per repeat of layers 0..3)
//                   vec4 cliff (x cliff scale, y splat strength, z slope start, w slope end (1 - n.y))
//                   vec4 fog (x alpha explored, y alpha unexplored); vec4 tint[4] (layer tints, w: has normal map)
//                   vec4 cliffTint (rgb, w: has normal map)
//   U1 / U2 / tex0 / tex1: as the lit shader (Renderer3D::bindLitFrame)
//   tex2 cell colour (RGBA8, W x H, vertex stage)   tex3 dynamic (r field mix, g ore 0/0.5/1, b creep; vertex stage)
//   tex4 splat weights (RGBA8 per cell, linear)     tex5..8 layer albedo   tex9..12 layer normals
//   tex13 cliff albedo  tex14 cliff normal          tex15 vision (R8 per cell: 0 unexplored, .5 explored, 1 visible)
#pragma once
#include "../../../engine/render/Shaders.h"

namespace rts::game::shaders {

#define RTS_TERRAIN_GLSL_U0 \
    "layout(std140) uniform U0 { vec4 t_lod; vec4 t_mode; vec4 t_scale; vec4 t_cliff; vec4 t_fog; vec4 t_tint[4]; vec4 t_cliffTint; };\n"

inline const char* kTerrainVS =
    RTS_TERRAIN_GLSL_U0 RTS_GLSL_FRAME
    R"(
uniform highp sampler2D tex2;
uniform highp sampler2D tex3;
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec2 a_morph;
out vec3 v_nrm;
out vec3 v_wpos;
out vec3 v_col;
out vec3 v_dyn;
void main() {
    vec3 p = a_pos;
    float dist = length(u_camPos.xyz - p);
    float k = clamp((dist - t_lod.x) / max(t_lod.y - t_lod.x, 0.001), 0.0, 1.0);
    p.y = mix(p.y, a_morph.x, k);
    vec3 dyn = vec3(0.0);
    int mask = int(a_col.a * 255.0 + 0.5);
    if (mask > 0 && mask < 16) {
        ivec2 X = ivec2(floor(a_pos.xz + 0.5));
        ivec2 hi = ivec2(t_lod.zw) - 1;
        float n = 0.0;
        for (int i = 0; i < 4; ++i) {
            if ((mask & (1 << i)) == 0) continue;
            ivec2 cell = clamp(X + ivec2(i & 1, i >> 1) - 1, ivec2(0), hi);
            vec4 d = texelFetch(tex3, cell, 0);
            vec3 cc = texelFetch(tex2, cell, 0).rgb;
            vec3 cp = cc;
            if (d.r > 0.0) {
                vec3 ore = d.g < 0.25 ? vec3(0.86, 0.62, 0.20) : d.g < 0.75 ? vec3(0.30, 0.78, 0.86) : vec3(0.80, 0.36, 0.76);
                cp = cp * (1.0 - d.r) + ore * d.r;
            }
            if (d.b > 0.5) cp = cp * 0.45 + vec3(0.36, 0.16, 0.34) * 0.55;
            dyn += cp - cc;
            n += 1.0;
        }
        dyn /= max(n, 1.0);
    }
    v_wpos = p;
    v_nrm = a_nrm;
    v_col = a_col.rgb;
    v_dyn = dyn;
    gl_Position = u_viewProj * vec4(p, 1.0);
}
)";

inline const char* kTerrainFS =
    RTS_TERRAIN_GLSL_U0 RTS_GLSL_FRAME RTS_GLSL_SHADOW RTS_GLSL_LIGHTS
    R"(
uniform sampler2D tex4;
uniform sampler2D tex5;
uniform sampler2D tex6;
uniform sampler2D tex7;
uniform sampler2D tex8;
uniform sampler2D tex9;
uniform sampler2D tex10;
uniform sampler2D tex11;
uniform sampler2D tex12;
uniform sampler2D tex13;
uniform sampler2D tex14;
uniform mediump sampler2D tex15;
in vec3 v_nrm;
in vec3 v_wpos;
in vec3 v_col;
in vec3 v_dyn;
out vec4 o_color;

#if TERRAIN_TIER >= 2
vec3 unpackN(vec4 t) { return t.xyz * 2.0 - 1.0; }
// normal map on a surface whose texture is projected on the xz plane (Gram-Schmidt tangent frame)
vec3 planarNormal(vec3 n, vec3 tn) {
    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - n * n.x);
    vec3 B = cross(T, n);
    return normalize(T * tn.x - B * tn.y + n * tn.z);
}
#endif

void main() {
    vec3 n = normalize(v_nrm);
    vec3 albedoS = v_col;
#if TERRAIN_TIER >= 1
    if (t_mode.y > 0.5) {
        vec4 w = texture(tex4, v_wpos.xz / t_lod.zw);
        w /= max(w.r + w.g + w.b + w.a, 0.0001);
        vec2 p = v_wpos.xz;
        vec3 s = vec3(0.0);
        if (w.r > 0.002) s += w.r * texture(tex5, p / t_scale.x).rgb * t_tint[0].rgb;
        if (w.g > 0.002) s += w.g * texture(tex6, p / t_scale.y).rgb * t_tint[1].rgb;
        if (w.b > 0.002) s += w.b * texture(tex7, p / t_scale.z).rgb * t_tint[2].rgb;
        if (w.a > 0.002) s += w.a * texture(tex8, p / t_scale.w).rgb * t_tint[3].rgb;
#if TERRAIN_TIER >= 2
        if (t_mode.z > 0.5) {
            vec3 tn = vec3(0.0, 0.0, 0.001);
            if (w.r > 0.002) tn += w.r * mix(vec3(0.0, 0.0, 1.0), unpackN(texture(tex9, p / t_scale.x)), t_tint[0].w);
            if (w.g > 0.002) tn += w.g * mix(vec3(0.0, 0.0, 1.0), unpackN(texture(tex10, p / t_scale.y)), t_tint[1].w);
            if (w.b > 0.002) tn += w.b * mix(vec3(0.0, 0.0, 1.0), unpackN(texture(tex11, p / t_scale.z)), t_tint[2].w);
            if (w.a > 0.002) tn += w.a * mix(vec3(0.0, 0.0, 1.0), unpackN(texture(tex12, p / t_scale.w)), t_tint[3].w);
            n = planarNormal(n, normalize(tn));
        }
#endif
        float slope = 1.0 - abs(normalize(v_nrm).y);
        float cw = smoothstep(t_cliff.z, t_cliff.w, slope);
        if (cw > 0.002) {
            vec3 an = abs(normalize(v_nrm));
            vec3 q = v_wpos / t_cliff.x;
#if TERRAIN_TIER >= 2
            // triplanar: three projections blended by the normal
            vec3 bw = pow(an, vec3(4.0));
            bw /= bw.x + bw.y + bw.z;
            vec3 c = texture(tex13, q.zy).rgb * bw.x + texture(tex13, q.xz).rgb * bw.y + texture(tex13, q.xy).rgb * bw.z;
            if (t_cliffTint.w > 0.5) { // UDN-blended triplanar normal
                vec3 nn = normalize(v_nrm);
                vec3 tx = unpackN(texture(tex14, q.zy)), ty = unpackN(texture(tex14, q.xz)), tz = unpackN(texture(tex14, q.xy));
                vec3 wn = normalize(vec3(0.0, tx.y, tx.x) * bw.x + vec3(ty.x, 0.0, ty.y) * bw.y + vec3(tz.x, tz.y, 0.0) * bw.z + nn);
                n = normalize(mix(n, wn, cw));
            }
#else
            // one projection along the dominant axis
            vec2 uv = an.x > an.z ? (an.y > an.x ? q.xz : q.zy) : (an.y > an.z ? q.xz : q.xy);
            vec3 c = texture(tex13, uv).rgb;
#endif
            s = mix(s, c * t_cliffTint.rgb, cw);
        }
        albedoS = mix(v_col, s, t_cliff.y);
    }
#endif
    vec3 alb = albedoS + v_dyn;
    vec3 base = alb * alb;                                // ~sRGB -> linear
    vec3 L = normalize(u_sunDir.xyz);
    vec3 V = normalize(u_camPos.xyz - v_wpos);
    float ndl = max(dot(n, L), 0.0);
    float sh = ndl > 0.0 ? shadowAt(v_wpos, n) : 1.0;
    sh *= cloudShade(v_wpos);
    vec3 sun = u_sunColor.rgb * u_sunColor.rgb * u_sunDir.w;
    vec3 sky = u_skyColor.rgb * u_skyColor.rgb, ground = u_groundColor.rgb * u_groundColor.rgb;
    vec3 amb = mix(ground, sky, n.y * 0.5 + 0.5);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 40.0) * t_mode.x;
    float rim = pow(1.0 - max(dot(n, V), 0.0), 3.0) * 0.25;
    vec3 col = base * (amb + sun * ndl * sh) + sun * spec * sh + amb * rim * base;
    col += base * dynamicLight(v_wpos, n, V, t_mode.x);
    float dist = length(u_camPos.xyz - v_wpos);
    col = mix(col, u_fog.rgb * u_fog.rgb, 1.0 - exp(-dist * u_fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0); // ACES fit
    vec3 outc = sqrt(col);
    if (t_mode.w > 0.5) { // fog of war of heightfield maps (the flat fog sheet would cut the relief)
        vec2 uv = v_wpos.xz / t_lod.zw;
        if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) {
            float v = texture(tex15, uv).r;
            float a = v > 0.75 ? 0.0 : v > 0.25 ? t_fog.x : t_fog.y;
            outc = mix(outc, vec3(8.0, 10.0, 14.0) / 255.0, a);
        }
    }
    o_color = vec4(outc, 1.0);
}
)";

inline const char* kTerrainMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct TU0 { float4 lod; float4 mode; float4 scale; float4 cliff; float4 fog; float4 tint[4]; float4 cliffTint; };\n"
    RTS_MSL_FRAME RTS_MSL_SHADOW RTS_MSL_LIGHTS
    R"(
struct VIn { float3 pos [[attribute(0)]]; float3 nrm [[attribute(1)]]; float4 col [[attribute(2)]]; float2 morph [[attribute(3)]]; };
struct VOut { float4 pos [[position]]; float3 nrm; float3 wpos; float3 col; float3 dyn; };
vertex VOut vs_main(VIn in [[stage_in]], constant TU0& u [[buffer(1)]], constant U1& f [[buffer(2)]],
                    texture2d<float> tex2 [[texture(2)]], texture2d<float> tex3 [[texture(3)]]) {
    VOut o;
    float3 p = in.pos;
    float dist = length(f.camPos.xyz - p);
    float k = clamp((dist - u.lod.x) / max(u.lod.y - u.lod.x, 0.001), 0.0, 1.0);
    p.y = mix(p.y, in.morph.x, k);
    float3 dyn = float3(0.0);
    int mask = int(in.col.a * 255.0 + 0.5);
    if (mask > 0 && mask < 16) {
        int2 X = int2(floor(in.pos.xz + 0.5));
        int2 hi = int2(u.lod.zw) - 1;
        float n = 0.0;
        for (int i = 0; i < 4; ++i) {
            if ((mask & (1 << i)) == 0) continue;
            int2 cell = clamp(X + int2(i & 1, i >> 1) - 1, int2(0), hi);
            float4 d = tex3.read(uint2(cell));
            float3 cc = tex2.read(uint2(cell)).rgb;
            float3 cp = cc;
            if (d.r > 0.0) {
                float3 ore = d.g < 0.25 ? float3(0.86, 0.62, 0.20) : d.g < 0.75 ? float3(0.30, 0.78, 0.86) : float3(0.80, 0.36, 0.76);
                cp = cp * (1.0 - d.r) + ore * d.r;
            }
            if (d.b > 0.5) cp = cp * 0.45 + float3(0.36, 0.16, 0.34) * 0.55;
            dyn += cp - cc;
            n += 1.0;
        }
        dyn /= max(n, 1.0);
    }
    o.wpos = p;
    o.nrm = in.nrm;
    o.col = in.col.rgb;
    o.dyn = dyn;
    o.pos = f.viewProj * float4(p, 1.0);
    return o;
}
#if TERRAIN_TIER >= 2
static float3 unpackN(float4 t) { return t.xyz * 2.0 - 1.0; }
static float3 planarNormal(float3 n, float3 tn) {
    float3 T = normalize(float3(1.0, 0.0, 0.0) - n * n.x);
    float3 B = cross(T, n);
    return normalize(T * tn.x - B * tn.y + n * tn.z);
}
#endif
fragment float4 fs_main(VOut in [[stage_in]], constant TU0& u [[buffer(1)]], constant U1& f [[buffer(2)]],
                        depth2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],
                        constant LightU& l [[buffer(3)]], texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]],
                        texture2d<float> tex4 [[texture(4)]], sampler s4 [[sampler(4)]],
                        texture2d<float> tex5 [[texture(5)]], sampler s5 [[sampler(5)]],
                        texture2d<float> tex6 [[texture(6)]], sampler s6 [[sampler(6)]],
                        texture2d<float> tex7 [[texture(7)]], sampler s7 [[sampler(7)]],
                        texture2d<float> tex8 [[texture(8)]], sampler s8 [[sampler(8)]],
                        texture2d<float> tex9 [[texture(9)]], sampler s9 [[sampler(9)]],
                        texture2d<float> tex10 [[texture(10)]], sampler s10 [[sampler(10)]],
                        texture2d<float> tex11 [[texture(11)]], sampler s11 [[sampler(11)]],
                        texture2d<float> tex12 [[texture(12)]], sampler s12 [[sampler(12)]],
                        texture2d<float> tex13 [[texture(13)]], sampler s13 [[sampler(13)]],
                        texture2d<float> tex14 [[texture(14)]], sampler s14 [[sampler(14)]],
                        texture2d<float> tex15 [[texture(15)]], sampler s15 [[sampler(15)]]) {
    float3 n = normalize(in.nrm);
    float3 albedoS = in.col;
#if TERRAIN_TIER >= 1
    if (u.mode.y > 0.5) {
        float4 w = tex4.sample(s4, in.wpos.xz / u.lod.zw);
        w /= max(w.r + w.g + w.b + w.a, 0.0001);
        float2 p = in.wpos.xz;
        float3 s = float3(0.0);
        if (w.r > 0.002) s += w.r * tex5.sample(s5, p / u.scale.x).rgb * u.tint[0].rgb;
        if (w.g > 0.002) s += w.g * tex6.sample(s6, p / u.scale.y).rgb * u.tint[1].rgb;
        if (w.b > 0.002) s += w.b * tex7.sample(s7, p / u.scale.z).rgb * u.tint[2].rgb;
        if (w.a > 0.002) s += w.a * tex8.sample(s8, p / u.scale.w).rgb * u.tint[3].rgb;
#if TERRAIN_TIER >= 2
        if (u.mode.z > 0.5) {
            float3 tn = float3(0.0, 0.0, 0.001);
            if (w.r > 0.002) tn += w.r * mix(float3(0.0, 0.0, 1.0), unpackN(tex9.sample(s9, p / u.scale.x)), u.tint[0].w);
            if (w.g > 0.002) tn += w.g * mix(float3(0.0, 0.0, 1.0), unpackN(tex10.sample(s10, p / u.scale.y)), u.tint[1].w);
            if (w.b > 0.002) tn += w.b * mix(float3(0.0, 0.0, 1.0), unpackN(tex11.sample(s11, p / u.scale.z)), u.tint[2].w);
            if (w.a > 0.002) tn += w.a * mix(float3(0.0, 0.0, 1.0), unpackN(tex12.sample(s12, p / u.scale.w)), u.tint[3].w);
            n = planarNormal(n, normalize(tn));
        }
#endif
        float slope = 1.0 - abs(normalize(in.nrm).y);
        float cw = smoothstep(u.cliff.z, u.cliff.w, slope);
        if (cw > 0.002) {
            float3 an = abs(normalize(in.nrm));
            float3 q = in.wpos / u.cliff.x;
#if TERRAIN_TIER >= 2
            float3 bw = pow(an, float3(4.0));
            bw /= bw.x + bw.y + bw.z;
            float3 c = tex13.sample(s13, q.zy).rgb * bw.x + tex13.sample(s13, q.xz).rgb * bw.y + tex13.sample(s13, q.xy).rgb * bw.z;
            if (u.cliffTint.w > 0.5) {
                float3 nn = normalize(in.nrm);
                float3 tx = unpackN(tex14.sample(s14, q.zy)), ty = unpackN(tex14.sample(s14, q.xz)), tz = unpackN(tex14.sample(s14, q.xy));
                float3 wn = normalize(float3(0.0, tx.y, tx.x) * bw.x + float3(ty.x, 0.0, ty.y) * bw.y + float3(tz.x, tz.y, 0.0) * bw.z + nn);
                n = normalize(mix(n, wn, cw));
            }
#else
            float2 uv = an.x > an.z ? (an.y > an.x ? q.xz : q.zy) : (an.y > an.z ? q.xz : q.xy);
            float3 c = tex13.sample(s13, uv).rgb;
#endif
            s = mix(s, c * u.cliffTint.rgb, cw);
        }
        albedoS = mix(in.col, s, u.cliff.y);
    }
#endif
    float3 alb = albedoS + in.dyn;
    float3 base = alb * alb;
    float3 L = normalize(f.sunDir.xyz);
    float3 V = normalize(f.camPos.xyz - in.wpos);
    float ndl = max(dot(n, L), 0.0);
    float sh = ndl > 0.0 ? shadowAt(f, in.wpos, n, in.pos.xy, tex0, s0) : 1.0;
    sh *= cloudShade(l, in.wpos);
    float3 sun = f.sunColor.rgb * f.sunColor.rgb * f.sunDir.w;
    float3 amb = mix(f.groundColor.rgb * f.groundColor.rgb, f.skyColor.rgb * f.skyColor.rgb, n.y * 0.5 + 0.5);
    float3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 40.0) * u.mode.x;
    float rim = pow(1.0 - max(dot(n, V), 0.0), 3.0) * 0.25;
    float3 col = base * (amb + sun * ndl * sh) + sun * spec * sh + amb * rim * base;
    col += base * dynamicLight(l, tex1, s1, in.wpos, n, V, u.mode.x);
    float dist = length(f.camPos.xyz - in.wpos);
    col = mix(col, f.fog.rgb * f.fog.rgb, 1.0 - exp(-dist * f.fog.w));
    col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);
    float3 outc = sqrt(col);
    if (u.mode.w > 0.5) {
        float2 uv = in.wpos.xz / u.lod.zw;
        if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) {
            float v = tex15.sample(s15, uv).r;
            float a = v > 0.75 ? 0.0 : v > 0.25 ? u.fog.x : u.fog.y;
            outc = mix(outc, float3(8.0, 10.0, 14.0) / 255.0, a);
        }
    }
    return float4(outc, 1.0);
}
)";

} // namespace rts::game::shaders
