// Shaders of the post-processing chain (PostFx.cpp). Each program is written once in a GLSL/MSL
// common subset and wrapped per backend by postfx::buildProgram():
//   * types vec2/vec3/vec4 (MSL: #define to float2..4), no ## pasting (old GLES drivers);
//   * uniforms: one block U0 of vec4 fields, used by name (GLSL block members; MSL locals copied from u);
//   * textures: TEX0(uv) .. TEX4(uv); the output: RETURN(color);
//   * helper functions start with PF_FN (MSL: static);
//   * `s` = screen uv of the pixel (0..1, y up); TUV(p) -> texture uv of a render target
//     (Metal stores targets top-down: px.z = 1 flips v); TDIR(d) flips a direction the same way.
// The vertex stage is a full-screen triangle (one Float2 attribute).
#pragma once

namespace rts::render::postfx {

// Bloom bright pass + 4-tap downsample. px.xy = source texel * tap spread; p0 = threshold, knee.
inline const char* kPrefilter = R"(
    vec2 uv = TUV(s);
    vec2 o = px.xy;
    vec3 c = (TEX0(uv + vec2(-o.x, -o.y)).rgb + TEX0(uv + vec2(o.x, -o.y)).rgb +
              TEX0(uv + vec2(-o.x, o.y)).rgb + TEX0(uv + vec2(o.x, o.y)).rgb) * 0.25;
    float br = max(c.r, max(c.g, c.b));
    float k = max(p0.x * p0.y, 0.0001);
    float soft = clamp(br - p0.x + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k + 0.0001);
    float w = max(soft, br - p0.x) / max(br, 0.0001);
    RETURN(vec4(c * w, 1.0));
)";

// Dual-filter downsample (centre x4 + 4 diagonals). px.xy = source texel.
inline const char* kDown = R"(
    vec2 uv = TUV(s);
    vec2 o = px.xy;
    vec3 c = TEX0(uv).rgb * 4.0 + TEX0(uv + vec2(-o.x, -o.y)).rgb + TEX0(uv + vec2(o.x, -o.y)).rgb +
             TEX0(uv + vec2(-o.x, o.y)).rgb + TEX0(uv + vec2(o.x, o.y)).rgb;
    RETURN(vec4(c * 0.125, 1.0));
)";

// Dual-filter upsample of the lower level (tex0) added to this level's downsample (tex1).
// px.xy = texel of tex0; p0.x = weight of the lower level (scatter).
inline const char* kUp = R"(
    vec2 uv = TUV(s);
    vec2 o = px.xy;
    vec3 c = TEX0(uv + vec2(-2.0 * o.x, 0.0)).rgb + TEX0(uv + vec2(2.0 * o.x, 0.0)).rgb +
             TEX0(uv + vec2(0.0, -2.0 * o.y)).rgb + TEX0(uv + vec2(0.0, 2.0 * o.y)).rgb +
             (TEX0(uv + vec2(-o.x, o.y)).rgb + TEX0(uv + vec2(o.x, o.y)).rgb +
              TEX0(uv + vec2(-o.x, -o.y)).rgb + TEX0(uv + vec2(o.x, -o.y)).rgb) * 2.0;
    RETURN(vec4(c * (p0.x / 12.0) + TEX1(uv).rgb, 1.0));
)";

// Separable 9-tap gaussian through 5 bilinear taps. px.xy = source texel; p0.xy = direction * spread.
inline const char* kBlur = R"(
    vec2 uv = TUV(s);
    vec2 d = px.xy * p0.xy;
    vec3 c = TEX0(uv).rgb * 0.2270270 +
             (TEX0(uv + d * 1.3846154).rgb + TEX0(uv - d * 1.3846154).rgb) * 0.3162162 +
             (TEX0(uv + d * 3.2307692).rgb + TEX0(uv - d * 3.2307692).rgb) * 0.0702703;
    RETURN(vec4(c, 1.0));
)";

// Sun shafts mask: bright pixels near the sun. px.xy = source texel * spread; p0.x = threshold;
// sun.xy = sun position (texture uv), sun.w = aspect.
inline const char* kShaftsPre = R"(
    vec2 uv = TUV(s);
    vec2 o = px.xy;
    vec3 c = (TEX0(uv + vec2(-o.x, -o.y)).rgb + TEX0(uv + vec2(o.x, -o.y)).rgb +
              TEX0(uv + vec2(-o.x, o.y)).rgb + TEX0(uv + vec2(o.x, o.y)).rgb) * 0.25;
    float l = max(c.r, max(c.g, c.b));
    float m = smoothstep(p0.x, p0.x + 0.15, l);
    vec2 d = uv - sun.xy;
    d.x *= sun.w;
    float fall = clamp(1.0 - length(d) * 0.45, 0.0, 1.0);
    RETURN(vec4(c * (m * fall), 1.0));
)";

// Radial blur towards the sun. p0 = samples, length (0..1), decay, weight; sun.xy = sun (texture uv).
inline const char* kShafts = R"(
    vec2 uv = TUV(s);
    vec2 delta = (uv - sun.xy) * (p0.y / p0.x);
    vec3 acc = vec3(0.0);
    float illum = 1.0;
    vec2 q = uv;
    for (int i = 0; i < 64; i++) {
        if (float(i) >= p0.x) break;
        q -= delta;
        acc += TEX0(q).rgb * illum;
        illum *= p0.z;
    }
    RETURN(vec4(acc * (p0.w / p0.x), 1.0));
)";

// FXAA (console-style, 5 taps + early out, 9 at most). px.xy = texel; p0.x = subpixel softening.
inline const char* kFxaaHelpers = R"(
PF_FN float pfLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
)";
inline const char* kFxaa = R"(
    vec2 uv = TUV(s);
    vec2 o = px.xy;
    vec3 cM = TEX0(uv).rgb;
    float lNW = pfLuma(TEX0(uv + vec2(-o.x, -o.y)).rgb), lNE = pfLuma(TEX0(uv + vec2(o.x, -o.y)).rgb);
    float lSW = pfLuma(TEX0(uv + vec2(-o.x, o.y)).rgb), lSE = pfLuma(TEX0(uv + vec2(o.x, o.y)).rgb);
    float lM = pfLuma(cM);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) RETURN(vec4(cM, 1.0));
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * o;
    vec3 a = 0.5 * (TEX0(uv + dir * (1.0 / 3.0 - 0.5)).rgb + TEX0(uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (TEX0(uv - dir * 0.5).rgb + TEX0(uv + dir * 0.5).rgb);
    float lB = pfLuma(b);
    vec3 r = (lB < lMin || lB > lMax) ? a : b;
    RETURN(vec4(mix(r, cM, (1.0 - p0.x) * 0.5), 1.0));
)";

// Final composite. PF_* defines pick the effects (one program per combination, built on demand).
//   px      texel x, y, flip, grain seed     grade0  -, contrast, saturation, posterize levels
//   grade1  colour multiplier rgb, pixel size lift    lift rgb, vignette intensity
//   gamma   1/gamma rgb, vignette softness   gain    gain rgb, grain intensity
//   filt    -, -, -, grain size              vig     vignette colour rgb, aberration
//   bloom   bloom tint rgb, sharpen amount   shafts  shafts colour rgb, LUT intensity
//   tilt    focus, width, falloff, -          fade    colour rgb, amount
//   lut     size N, flipY, -, -               lbox    colour rgb, bars height
// Textures: 0 scene, 1 bloom, 2 tilt-shift blur, 3 sun shafts, 4 LUT.
inline const char* kCompositeHelpers = R"(
PF_FN float pfHash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
)";
inline const char* kComposite = R"(
    vec2 sp = s;
#ifdef PF_PIXELATE
    {
        vec2 cell = px.xy * grade1.w;
        sp = (floor(sp / cell) + 0.5) * cell;
    }
#endif
    vec2 uv = TUV(sp);
#ifdef PF_CA
    vec2 cd = TDIR((sp - 0.5) * (vig.w * 0.012));
    vec3 col = vec3(TEX0(uv + cd).r, TEX0(uv).g, TEX0(uv - cd).b);
#else
    vec3 col = TEX0(uv).rgb;
#endif
#ifdef PF_SHARPEN
    {
        vec2 o = px.xy * 0.66;
        vec3 n = (TEX0(uv + o).rgb + TEX0(uv - o).rgb) * 0.5;
        col = max(col + (col - n) * (bloom.w * 1.5), vec3(0.0));
    }
#endif
#ifdef PF_TILT
    {
        float dy = abs(sp.y - tilt.x);
        float m = smoothstep(tilt.y * 0.5, tilt.y * 0.5 + tilt.z + 0.0001, dy);
        col = mix(col, TEX2(uv).rgb, m);
    }
#endif
#ifdef PF_BLOOM
    col += TEX1(uv).rgb * bloom.rgb;
#endif
#ifdef PF_SHAFTS
    col += TEX3(uv).rgb * shafts.rgb;
#endif
#ifdef PF_GRADE
    col *= grade1.rgb;
    col = gain.rgb * (col + lift.rgb * (vec3(1.0) - col));
    col = pow(max(col, vec3(0.0)), gamma.rgb);
    col = (col - vec3(0.5)) * grade0.y + vec3(0.5);
    col = mix(vec3(dot(col, vec3(0.2126, 0.7152, 0.0722))), col, grade0.z);
    col = clamp(col, vec3(0.0), vec3(1.0));
#endif
#ifdef PF_LUT
    {
        vec3 c = clamp(col, vec3(0.0), vec3(1.0));
        float n = lut.x;
        float bz = c.b * (n - 1.0);
        float s0 = floor(bz);
        float s1 = min(s0 + 1.0, n - 1.0);
        float x = (c.r * (n - 1.0) + 0.5) / (n * n);
        float y = (c.g * (n - 1.0) + 0.5) / n;
        if (lut.y > 0.5) y = 1.0 - y;
        vec3 la = TEX4(vec2(x + s0 / n, y)).rgb;
        vec3 lb = TEX4(vec2(x + s1 / n, y)).rgb;
        col = mix(col, mix(la, lb, bz - s0), shafts.w);
    }
#endif
#ifdef PF_POSTERIZE
    col = floor(col * grade0.w + vec3(0.5)) / grade0.w;
#endif
#ifdef PF_VIGNETTE
    {
        float aspect = px.y / px.x;
        vec2 d = (s - vec2(0.5)) * vec2(aspect, 1.0);
        float r = length(d) / length(vec2(aspect, 1.0) * 0.5);
        float m = smoothstep(mix(0.85, 0.0, gamma.w), 1.0, r);
        col = mix(col, vig.rgb, m * lift.w);
    }
#endif
#ifdef PF_GRAIN
    {
        vec2 g = floor(s / (px.xy * filt.w));
        col += vec3((pfHash(g + px.w * vec2(17.0, 31.0)) - 0.5) * gain.w);
    }
#endif
#ifdef PF_FADE
    col = mix(col, fade.rgb, fade.w);
#endif
#ifdef PF_LETTERBOX
    if (s.y < lbox.w * 0.5 || s.y > 1.0 - lbox.w * 0.5) col = lbox.rgb;
#endif
    col += vec3((pfHash(s * 1931.0 + px.w) - 0.5) / 255.0);   // dither: no banding in gradients / vignette
    RETURN(vec4(clamp(col, vec3(0.0), vec3(1.0)), 1.0));
)";

} // namespace rts::render::postfx
