// Splash shaders: textured, tinted 2D quads in pixel coordinates (origin top-left). The cheapest
// possible path (one fetch, one multiply) so the loading screen runs on any GPU.
//
//   vertex: pos (float2, pixels), uv (float2), color (ubyte4n, multiplies the texel)
//   U0:     vec4 target (x = width, y = height in pixels)
//   tex0:   RGBA image (straight alpha)
#pragma once

namespace rts::splash::shaders {

inline const char* kSplashVS = R"(
layout(std140) uniform U0 { vec4 u_target; };
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
out vec2 v_uv;
out vec4 v_col;
void main() {
    v_uv = a_uv; v_col = a_col;
    gl_Position = vec4(a_pos.x / u_target.x * 2.0 - 1.0, 1.0 - a_pos.y / u_target.y * 2.0, 0.0, 1.0);
}
)";

inline const char* kSplashFS = R"(
uniform sampler2D tex0;
in vec2 v_uv;
in vec4 v_col;
out vec4 o_color;
void main() { o_color = texture(tex0, v_uv) * v_col; }
)";

inline const char* kSplashMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct U0 { float4 target; };
struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 col [[attribute(2)]]; };
struct VOut { float4 pos [[position]]; float2 uv; float4 col; };
vertex VOut vs_main(VIn in [[stage_in]], constant U0& u [[buffer(1)]]) {
    VOut o;
    o.pos = float4(in.pos.x / u.target.x * 2.0 - 1.0, 1.0 - in.pos.y / u.target.y * 2.0, 0.0, 1.0);
    o.uv = in.uv; o.col = in.col; return o;
}
fragment float4 fs_main(VOut in [[stage_in]], texture2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    return tex0.sample(s0, in.uv) * in.col;
}
)";

} // namespace rts::splash::shaders
