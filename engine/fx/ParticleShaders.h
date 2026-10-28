// Particle shaders (GLSL ES 3.00 body without #version, and MSL). Kept apart from
// engine/render/Shaders.h on purpose: particles only need the RHI.
//
// Built for the weakest GPUs we ship on (ES 3.0 Mali-T6xx / Adreno 3xx / PowerVR Rogue):
//   * quads are expanded on the CPU: no instancing, no geometry/compute, no transform feedback;
//   * fog is evaluated per vertex, the fragment shader is one texture fetch and one multiply,
//     in mediump/lowp (half on Metal) — particles are fill-rate bound, every ALU op counts;
//   * no depth texture reads (no soft particles), no derivatives, no discard (keeps early-Z on
//     tile-based GPUs).
//
// U0 (per draw): mat4 viewProj; vec4 fog (rgb display-space colour, w density); params.z: 1 = multiply blend (fog fades to white);
//                vec4 camPos (xyz); vec4 params (x: 1 = additive blend -> fog fades alpha instead of colour;
//                y: 1 = premultiplied colour -> the fog colour is premultiplied too)
// Vertex: a_pos (float3), a_uv (float2), a_col (ubyte4 normalized)
#pragma once

namespace rts::fx::shaders {

inline const char* kParticleVS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; vec4 u_fog; vec4 u_camPos; vec4 u_params; };
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
out mediump vec2 v_uv;
out lowp vec4 v_col;
void main() {
    gl_Position = u_viewProj * vec4(a_pos, 1.0);
    float f = 1.0 - exp(-length(a_pos - u_camPos.xyz) * u_fog.w);
    vec4 c = a_col;
    c.rgb = mix(c.rgb, mix(u_fog.rgb * mix(1.0, c.a, u_params.y), vec3(1.0), u_params.z), f * (1.0 - u_params.x));
    c.a *= 1.0 - f * u_params.x;
    v_col = c;
    v_uv = a_uv;
}
)";

inline const char* kParticleFS = R"(
precision mediump float;
uniform lowp sampler2D tex0;
in mediump vec2 v_uv;
in lowp vec4 v_col;
out lowp vec4 o_color;
void main() { o_color = texture(tex0, v_uv) * v_col; }
)";

// Multiply blend: transparent texels must leave the background unchanged (colour -> white by alpha).
inline const char* kParticleMulFS = R"(
precision mediump float;
uniform lowp sampler2D tex0;
in mediump vec2 v_uv;
in lowp vec4 v_col;
out lowp vec4 o_color;
void main() { lowp vec4 c = texture(tex0, v_uv) * v_col; o_color = vec4(mix(vec3(1.0), c.rgb, c.a), c.a); }
)";

inline const char* kParticleMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct U0 { float4x4 viewProj; float4 fog; float4 camPos; float4 params; };
struct VIn { float3 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float2 uv; half4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) {
    VOut o;
    o.pos = u.viewProj * float4(in.pos, 1.0);
    float f = 1.0 - exp(-length(in.pos - u.camPos.xyz) * u.fog.w);
    float4 c = in.col;
    c.rgb = mix(c.rgb, mix(u.fog.rgb * mix(1.0, c.a, u.params.y), float3(1.0), u.params.z), f * (1.0 - u.params.x));
    c.a *= 1.0 - f * u.params.x;
    o.col = half4(c);
    o.uv = in.uv;
    return o;
}
fragment half4 fs_main(VOut in [[stage_in]], texture2d<half> tex0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    return tex0.sample(s0, in.uv) * in.col;
}
)";

// Same vertex stage, multiply fragment stage (see kParticleMulFS).
inline const char* kParticleMulMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct U0 { float4x4 viewProj; float4 fog; float4 camPos; float4 params; };
struct VIn { float3 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float2 uv; half4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) {
    VOut o;
    o.pos = u.viewProj * float4(in.pos, 1.0);
    float f = 1.0 - exp(-length(in.pos - u.camPos.xyz) * u.fog.w);
    float4 c = in.col;
    c.rgb = mix(c.rgb, mix(u.fog.rgb * mix(1.0, c.a, u.params.y), float3(1.0), u.params.z), f * (1.0 - u.params.x));
    c.a *= 1.0 - f * u.params.x;
    o.col = half4(c);
    o.uv = in.uv;
    return o;
}
fragment half4 fs_main(VOut in [[stage_in]], texture2d<half> tex0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    half4 c = tex0.sample(s0, in.uv) * in.col;
    return half4(mix(half3(1.0h), c.rgb, c.a), c.a);
}
)";

} // namespace rts::fx::shaders
