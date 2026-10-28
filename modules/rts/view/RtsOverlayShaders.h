// Shader of the overlay batch renderer (RtsOverlays): unlit coloured triangles with an optional
// texture (icons / decals resolved by TextureCache; untextured geometry samples a 1x1 white texture).
// Cheap on purpose (one fetch, one multiply, no discard): overlays may cover much of the screen on
// weak mobile GPUs. Pipelines: alpha / additive blending x depth-tested / always on top; no depth write.
//
// U0 (per draw): mat4 viewProj
// Vertex: a_pos (float3), a_uv (float2), a_col (ubyte4 normalized)
#pragma once

namespace rts::game::shaders {

inline const char* kOverlayVS = R"(
layout(std140) uniform U0 { mat4 u_viewProj; };
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
out mediump vec2 v_uv;
out lowp vec4 v_col;
void main() {
    gl_Position = u_viewProj * vec4(a_pos, 1.0);
    v_uv = a_uv;
    v_col = a_col;
}
)";

inline const char* kOverlayFS = R"(
precision mediump float;
uniform lowp sampler2D tex0;
in mediump vec2 v_uv;
in lowp vec4 v_col;
out lowp vec4 o_color;
void main() { o_color = texture(tex0, v_uv) * v_col; }
)";

inline const char* kOverlayMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct U0 { float4x4 viewProj; };
struct VIn { float3 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float2 uv; half4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) {
    VOut o;
    o.pos = u.viewProj * float4(in.pos, 1.0);
    o.uv = in.uv;
    o.col = half4(in.col);
    return o;
}
fragment half4 fs_main(VOut in [[stage_in]], texture2d<half> tex0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    return tex0.sample(s0, in.uv) * in.col;
}
)";

} // namespace rts::game::shaders
