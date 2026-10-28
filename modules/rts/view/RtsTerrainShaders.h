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
    "layout(std140) uniform U0 { vec4 t_lod; vec4 t_mode; vec4 t_scale; vec4 t_cliff; vec4 t_fog; vec4 t_tint[4]; vec4 t_cliffTint; vec4 t_fogColor; vec4 t_fogGrid; vec4 t_detail; vec4 t_rock; vec4 t_blend; };\n"

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
#if TERRAIN_DETAIL
uniform sampler2D tex16;   // detail normals: rg = n.xz, b = cavity, a = macro variation
uniform sampler2D tex17;   // detail splat weights
#endif
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
    vec3 geoN = n;                                        // surface normal for slope tests
#if TERRAIN_DETAIL
    // per-pixel normal of the detail surface (cheap: one fetch, every tier)
    // outside the map (apron, border walls) the clamped edge texels would stretch into streaks: fade to
    // the geometric normal and to a heavily blurred (high mip) edge material within a few cells
    vec2 outD = max(max(-v_wpos.xz, v_wpos.xz - t_lod.zw), vec2(0.0));
    float outF = clamp(max(outD.x, outD.y) * 0.5, 0.0, 1.0);
    float outB = outF * 7.0;
    vec4 dn = texture(tex16, v_wpos.xz * t_detail.xy + t_detail.zw, outB);
    vec2 nxz = dn.rg * 2.0 - 1.0;
    vec3 dN = normalize(vec3(nxz.x, sqrt(max(1.0 - dot(nxz, nxz), 0.0)), nxz.y));
    bool wall = n.y < 0.3 && min(min(v_wpos.x, v_wpos.z), min(t_lod.z - v_wpos.x, t_lod.w - v_wpos.z)) < 0.02;
    if (!wall) n = normalize(mix(dN, n, outF));   // the border walls keep their geometric normal
    dn.b = mix(dn.b, 0.5, outF);
    geoN = n;
    float macroV = (dn.a - 0.5) * t_blend.y;
    float cavity = mix(1.0, 0.55 + 0.9 * dn.b, t_blend.z);
    // automatic rock on steep slopes (vertex-colour tiers and games without a cliff texture)
    float rockW = smoothstep(t_cliff.z, t_cliff.w, 1.0 - n.y) * t_rock.w;
    albedoS = mix(albedoS, t_rock.rgb * (0.85 + 0.3 * dn.a), rockW);
    albedoS *= (1.0 + macroV) * cavity;
#endif
#if TERRAIN_TIER >= 1
    if (t_mode.y > 0.5) {
#if TERRAIN_DETAIL
        vec4 w = texture(tex17, v_wpos.xz * t_detail.xy + t_detail.zw, outB);
#else
        vec4 w = texture(tex4, v_wpos.xz / t_lod.zw);
#endif
        w /= max(w.r + w.g + w.b + w.a, 0.0001);
        vec2 p = v_wpos.xz;
        vec3 s = vec3(0.0);
#if TERRAIN_DETAIL
        // height-based blending: each layer's brightness acts as its height, the transition band keeps
        // the higher material (pebbles over sand) instead of a linear cross-fade
        vec3 c0 = texture(tex5, p / t_scale.x).rgb * t_tint[0].rgb, c1 = texture(tex6, p / t_scale.y).rgb * t_tint[1].rgb;
        vec3 c2 = texture(tex7, p / t_scale.z).rgb * t_tint[2].rgb, c3 = texture(tex8, p / t_scale.w).rgb * t_tint[3].rgb;
#if TERRAIN_TIER >= 2
        // second, rotated sample at 1/3.7 scale against visible tiling
        mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
        vec2 q = rot * p / 3.7;
        c0 = mix(c0, texture(tex5, q / t_scale.x).rgb * t_tint[0].rgb, 0.35);
        c1 = mix(c1, texture(tex6, q / t_scale.y).rgb * t_tint[1].rgb, 0.35);
        c2 = mix(c2, texture(tex7, q / t_scale.z).rgb * t_tint[2].rgb, 0.35);
        c3 = mix(c3, texture(tex8, q / t_scale.w).rgb * t_tint[3].rgb, 0.35);
#endif
        vec4 hb = w + t_blend.x * (vec4(dot(c0, vec3(0.33)), dot(c1, vec3(0.33)), dot(c2, vec3(0.33)), dot(c3, vec3(0.33))) - 0.5);
        float hm = max(max(hb.r, hb.g), max(hb.b, hb.a)) - 0.25 * (1.0 - 0.5 * t_blend.x);
        vec4 bw = max(hb - hm, vec4(0.0)) * step(vec4(0.002), w);
        bw /= max(bw.r + bw.g + bw.b + bw.a, 0.0001);
        w = mix(w, bw, t_blend.x);
        s = w.r * c0 + w.g * c1 + w.b * c2 + w.a * c3;
#else
        if (w.r > 0.002) s += w.r * texture(tex5, p / t_scale.x).rgb * t_tint[0].rgb;
        if (w.g > 0.002) s += w.g * texture(tex6, p / t_scale.y).rgb * t_tint[1].rgb;
        if (w.b > 0.002) s += w.b * texture(tex7, p / t_scale.z).rgb * t_tint[2].rgb;
        if (w.a > 0.002) s += w.a * texture(tex8, p / t_scale.w).rgb * t_tint[3].rgb;
#endif
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
        float slope = 1.0 - abs(geoN.y);
        float cw = smoothstep(t_cliff.z, t_cliff.w, slope);
        if (cw > 0.002) {
            vec3 an = abs(geoN);
            vec3 q = v_wpos / t_cliff.x;
#if TERRAIN_TIER >= 2
            // triplanar: three projections blended by the normal
            vec3 bw = pow(an, vec3(4.0));
            bw /= bw.x + bw.y + bw.z;
            vec3 c = texture(tex13, q.zy).rgb * bw.x + texture(tex13, q.xz).rgb * bw.y + texture(tex13, q.xy).rgb * bw.z;
            if (t_cliffTint.w > 0.5) { // UDN-blended triplanar normal
                vec3 nn = geoN;
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
#if TERRAIN_DETAIL
        s *= (1.0 + macroV) * cavity;
#endif
        albedoS = mix(albedoS, s, t_cliff.y);
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
            if (t_fogColor.w > 0.5) uv = (floor(v_wpos.xz) + 0.5) / t_lod.zw;   // hard cells (RtsFog style)
            float v = texture(tex15, uv).r;  // 0 unexplored, 0.5 explored, 1 visible (faded + blurred by RtsFog)
            float a = v < 0.5 ? mix(t_fog.y, t_fog.x, v * 2.0) : mix(t_fog.x, 0.0, (v - 0.5) * 2.0);
            vec3 fc = t_fogColor.rgb;
            if (t_fogGrid.a > 0.0 && (t_fog.z > 0.5 || a > 0.01)) {
                vec2 fw = fract(v_wpos.xz);
                float g = t_fogGrid.a * (1.0 - smoothstep(0.015, 0.05, min(min(fw.x, 1.0 - fw.x), min(fw.y, 1.0 - fw.y))));
                fc = mix(fc, t_fogGrid.rgb, g / max(a + g, 0.001)); a = max(a, g);
            }
            outc = mix(outc, fc, a);
        }
    }
    o_color = vec4(outc, 1.0);
}
)";

inline const char* kTerrainMSL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct TU0 { float4 lod; float4 mode; float4 scale; float4 cliff; float4 fog; float4 tint[4]; float4 cliffTint; float4 fogColor; float4 fogGrid; float4 detail; float4 rock; float4 blend; };\n"
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
                        texture2d<float> tex15 [[texture(15)]], sampler s15 [[sampler(15)]]
#if TERRAIN_DETAIL
                        , texture2d<float> tex16 [[texture(16)]], texture2d<float> tex17 [[texture(17)]]
#endif
                        ) {
#if TERRAIN_DETAIL
    // Metal has sampler slots 0..15 per stage: the detail textures use constant samplers
    constexpr sampler s16(filter::linear, mip_filter::linear, address::clamp_to_edge);
    constexpr sampler s17(filter::linear, mip_filter::linear, address::clamp_to_edge);
#endif
    float3 n = normalize(in.nrm);
    float3 albedoS = in.col;
    float3 geoN = n;
#if TERRAIN_DETAIL
    float2 outD = max(max(-in.wpos.xz, in.wpos.xz - u.lod.zw), float2(0.0));
    float outF = clamp(max(outD.x, outD.y) * 0.5, 0.0, 1.0);
    float outB = outF * 7.0;
    float4 dn = tex16.sample(s16, in.wpos.xz * u.detail.xy + u.detail.zw, bias(outB));
    float2 nxz = dn.rg * 2.0 - 1.0;
    float3 dN = normalize(float3(nxz.x, sqrt(max(1.0 - dot(nxz, nxz), 0.0)), nxz.y));
    bool wall = n.y < 0.3 && min(min(in.wpos.x, in.wpos.z), min(u.lod.z - in.wpos.x, u.lod.w - in.wpos.z)) < 0.02;
    if (!wall) n = normalize(mix(dN, n, outF));
    dn.b = mix(dn.b, 0.5, outF);
    geoN = n;
    float macroV = (dn.a - 0.5) * u.blend.y;
    float cavity = mix(1.0, 0.55 + 0.9 * dn.b, u.blend.z);
    float rockW = smoothstep(u.cliff.z, u.cliff.w, 1.0 - n.y) * u.rock.w;
    albedoS = mix(albedoS, u.rock.rgb * (0.85 + 0.3 * dn.a), rockW);
    albedoS *= (1.0 + macroV) * cavity;
#endif
#if TERRAIN_TIER >= 1
    if (u.mode.y > 0.5) {
#if TERRAIN_DETAIL
        float4 w = tex17.sample(s17, in.wpos.xz * u.detail.xy + u.detail.zw, bias(outB));
#else
        float4 w = tex4.sample(s4, in.wpos.xz / u.lod.zw);
#endif
        w /= max(w.r + w.g + w.b + w.a, 0.0001);
        float2 p = in.wpos.xz;
        float3 s = float3(0.0);
#if TERRAIN_DETAIL
        float3 c0 = tex5.sample(s5, p / u.scale.x).rgb * u.tint[0].rgb, c1 = tex6.sample(s6, p / u.scale.y).rgb * u.tint[1].rgb;
        float3 c2 = tex7.sample(s7, p / u.scale.z).rgb * u.tint[2].rgb, c3 = tex8.sample(s8, p / u.scale.w).rgb * u.tint[3].rgb;
#if TERRAIN_TIER >= 2
        float2x2 rot = float2x2(float2(0.8, 0.6), float2(-0.6, 0.8));
        float2 q2 = rot * p / 3.7;
        c0 = mix(c0, tex5.sample(s5, q2 / u.scale.x).rgb * u.tint[0].rgb, 0.35);
        c1 = mix(c1, tex6.sample(s6, q2 / u.scale.y).rgb * u.tint[1].rgb, 0.35);
        c2 = mix(c2, tex7.sample(s7, q2 / u.scale.z).rgb * u.tint[2].rgb, 0.35);
        c3 = mix(c3, tex8.sample(s8, q2 / u.scale.w).rgb * u.tint[3].rgb, 0.35);
#endif
        float4 hb = w + u.blend.x * (float4(dot(c0, float3(0.33)), dot(c1, float3(0.33)), dot(c2, float3(0.33)), dot(c3, float3(0.33))) - 0.5);
        float hm = max(max(hb.r, hb.g), max(hb.b, hb.a)) - 0.25 * (1.0 - 0.5 * u.blend.x);
        float4 bw4 = max(hb - hm, float4(0.0)) * step(float4(0.002), w);
        bw4 /= max(bw4.r + bw4.g + bw4.b + bw4.a, 0.0001);
        w = mix(w, bw4, u.blend.x);
        s = w.r * c0 + w.g * c1 + w.b * c2 + w.a * c3;
#else
        if (w.r > 0.002) s += w.r * tex5.sample(s5, p / u.scale.x).rgb * u.tint[0].rgb;
        if (w.g > 0.002) s += w.g * tex6.sample(s6, p / u.scale.y).rgb * u.tint[1].rgb;
        if (w.b > 0.002) s += w.b * tex7.sample(s7, p / u.scale.z).rgb * u.tint[2].rgb;
        if (w.a > 0.002) s += w.a * tex8.sample(s8, p / u.scale.w).rgb * u.tint[3].rgb;
#endif
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
        float slope = 1.0 - abs(geoN.y);
        float cw = smoothstep(u.cliff.z, u.cliff.w, slope);
        if (cw > 0.002) {
            float3 an = abs(geoN);
            float3 q = in.wpos / u.cliff.x;
#if TERRAIN_TIER >= 2
            float3 bw = pow(an, float3(4.0));
            bw /= bw.x + bw.y + bw.z;
            float3 c = tex13.sample(s13, q.zy).rgb * bw.x + tex13.sample(s13, q.xz).rgb * bw.y + tex13.sample(s13, q.xy).rgb * bw.z;
            if (u.cliffTint.w > 0.5) {
                float3 nn = geoN;
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
#if TERRAIN_DETAIL
        s *= (1.0 + macroV) * cavity;
#endif
        albedoS = mix(albedoS, s, u.cliff.y);
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
            if (u.fogColor.w > 0.5) uv = (floor(in.wpos.xz) + 0.5) / u.lod.zw;
            float v = tex15.sample(s15, uv).r;
            float a = v < 0.5 ? mix(u.fog.y, u.fog.x, v * 2.0) : mix(u.fog.x, 0.0, (v - 0.5) * 2.0);
            float3 fc = u.fogColor.rgb;
            if (u.fogGrid.a > 0.0 && (u.fog.z > 0.5 || a > 0.01)) {
                float2 fw = fract(in.wpos.xz);
                float g = u.fogGrid.a * (1.0 - smoothstep(0.015, 0.05, min(min(fw.x, 1.0 - fw.x), min(fw.y, 1.0 - fw.y))));
                fc = mix(fc, u.fogGrid.rgb, g / max(a + g, 0.001)); a = max(a, g);
            }
            outc = mix(outc, fc, a);
        }
    }
    return float4(outc, 1.0);
}
)";

} // namespace rts::game::shaders
