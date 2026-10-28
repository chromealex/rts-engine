// Float math for rendering/tools ONLY. Never use inside the simulation (see engine/core/Fixed.h).
#pragma once
#include <cmath>

namespace rts::render {

struct float2 { float x = 0, y = 0; };
struct float3 {
    float x = 0, y = 0, z = 0;
    float3 operator+(float3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    float3 operator-(float3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    float3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float3 operator-() const { return {-x, -y, -z}; }
    float3& operator+=(float3 o) { x += o.x; y += o.y; z += o.z; return *this; }
};
struct float4 { float x = 0, y = 0, z = 0, w = 0; };

inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(float3 a, float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(float3 a) { return std::sqrt(dot(a, a)); }
inline float3 normalize(float3 a) { float l = length(a); return l > 1e-8f ? a * (1.0f / l) : float3{0, 1, 0}; }
inline float3 lerp(float3 a, float3 b, float t) { return a + (b - a) * t; }
inline float3 mul(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float3 min3(float3 a, float3 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z}; }
inline float3 max3(float3 a, float3 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z}; }

// Unit quaternion (x, y, z, w); rotation of v is q * v * q^-1.
struct quat {
    float x = 0, y = 0, z = 0, w = 1;
    quat operator*(const quat& b) const {
        return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
    }
    static quat axisAngle(float3 axis, float a) {
        float3 n = normalize(axis); float s = std::sin(a * 0.5f);
        return {n.x * s, n.y * s, n.z * s, std::cos(a * 0.5f)};
    }
    float3 rotate(float3 v) const {
        float3 u{x, y, z};
        float3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }
};
inline float dot(const quat& a, const quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline quat qnormalize(quat q) {
    float l = std::sqrt(dot(q, q));
    return l > 1e-8f ? quat{q.x / l, q.y / l, q.z / l, q.w / l} : quat{};
}
// Normalized lerp along the shortest arc: cheap and good enough for animation keys and blending.
inline quat nlerp(quat a, quat b, float t) {
    if (dot(a, b) < 0) b = {-b.x, -b.y, -b.z, -b.w};
    return qnormalize(quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}
inline quat slerp(quat a, quat b, float t) {
    float d = dot(a, b);
    if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f) return nlerp(a, b, t);
    float th = std::acos(d), s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

// Column-major 4x4: m[col*4 + row]
struct mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static mat4 identity() { return {}; }
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }

    mat4 operator*(const mat4& b) const {
        mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += at(rr, k) * b.at(k, c);
                r.at(rr, c) = s;
            }
        return r;
    }
    float4 operator*(const float4& v) const {
        return {at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z + at(0, 3) * v.w,
                at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z + at(1, 3) * v.w,
                at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z + at(2, 3) * v.w,
                at(3, 0) * v.x + at(3, 1) * v.y + at(3, 2) * v.z + at(3, 3) * v.w};
    }
    float3 transformPoint(float3 p) const { float4 r = *this * float4{p.x, p.y, p.z, 1}; return {r.x, r.y, r.z}; }

    static mat4 translate(float3 t) { mat4 r; r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z; return r; }
    static mat4 scale(float3 s) { mat4 r; r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z; return r; }
    static mat4 rotateY(float a) {
        mat4 r; float c = std::cos(a), s = std::sin(a);
        r.at(0, 0) = c; r.at(0, 2) = s; r.at(2, 0) = -s; r.at(2, 2) = c; return r;
    }
    static mat4 lookAt(float3 eye, float3 target, float3 up) {
        float3 f = normalize(target - eye), s = normalize(cross(f, up)), u = cross(s, f);
        mat4 r;
        r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z; r.at(0, 3) = -dot(s, eye);
        r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z; r.at(1, 3) = -dot(u, eye);
        r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = dot(f, eye);
        return r;
    }
    // Right-handed perspective; zeroToOne selects the clip depth range of the backend.
    static mat4 perspective(float fovyRad, float aspect, float zn, float zf, bool zeroToOne) {
        mat4 r; float t = 1.0f / std::tan(fovyRad * 0.5f);
        r.m[0] = t / aspect; r.m[5] = t; r.m[10] = 0; r.m[15] = 0;
        r.at(3, 2) = -1;
        if (zeroToOne) { r.at(2, 2) = zf / (zn - zf); r.at(2, 3) = zn * zf / (zn - zf); }
        else { r.at(2, 2) = (zf + zn) / (zn - zf); r.at(2, 3) = 2 * zf * zn / (zn - zf); }
        return r;
    }
    static mat4 ortho(float l, float rgt, float b, float t, float zn, float zf, bool zeroToOne) {
        mat4 r;
        r.at(0, 0) = 2 / (rgt - l); r.at(1, 1) = 2 / (t - b);
        r.at(0, 3) = -(rgt + l) / (rgt - l); r.at(1, 3) = -(t + b) / (t - b);
        if (zeroToOne) { r.at(2, 2) = -1 / (zf - zn); r.at(2, 3) = -zn / (zf - zn); }
        else { r.at(2, 2) = -2 / (zf - zn); r.at(2, 3) = -(zf + zn) / (zf - zn); }
        return r;
    }
    static mat4 rotateX(float a) {
        mat4 r; float c = std::cos(a), s = std::sin(a);
        r.at(1, 1) = c; r.at(1, 2) = -s; r.at(2, 1) = s; r.at(2, 2) = c; return r;
    }
    static mat4 rotateZ(float a) {
        mat4 r; float c = std::cos(a), s = std::sin(a);
        r.at(0, 0) = c; r.at(0, 1) = -s; r.at(1, 0) = s; r.at(1, 1) = c; return r;
    }
    static mat4 rotate(const quat& q) {
        mat4 r;
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        r.at(0, 0) = 1 - 2 * (yy + zz); r.at(0, 1) = 2 * (xy - wz); r.at(0, 2) = 2 * (xz + wy);
        r.at(1, 0) = 2 * (xy + wz); r.at(1, 1) = 1 - 2 * (xx + zz); r.at(1, 2) = 2 * (yz - wx);
        r.at(2, 0) = 2 * (xz - wy); r.at(2, 1) = 2 * (yz + wx); r.at(2, 2) = 1 - 2 * (xx + yy);
        return r;
    }
    // translate * rotate * scale (glTF node order)
    static mat4 trs(float3 t, const quat& q, float3 s) {
        mat4 r = rotate(q);
        for (int i = 0; i < 3; ++i) { r.at(i, 0) *= s.x; r.at(i, 1) *= s.y; r.at(i, 2) *= s.z; }
        r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
        return r;
    }
    float3 transformDir(float3 v) const { float4 r = *this * float4{v.x, v.y, v.z, 0}; return {r.x, r.y, r.z}; }
    mat4 inverse() const;
    // inverse-transpose of the upper 3x3 (normals under non-uniform scale), translation cleared
    mat4 normalMatrix() const {
        mat4 r = inverse(), t;
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t.at(i, j) = r.at(j, i);
        return t;
    }
};

inline mat4 mat4::inverse() const {
    const float* a = m;
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    mat4 r;
    if (std::fabs(det) < 1e-12f) return r;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
    return r;
}

// RTS camera orbiting a ground target point.
struct RTSCamera {
    float3 target{0, 0, 0};
    float distance = 30.0f;
    float pitch = 0.95f;   // radians from horizontal
    float yaw = 0.0f;      // radians around Y
    float fovY = 0.75f;
    float minDistance = 6.0f, maxDistance = 120.0f;

    float3 eye() const {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        return target + float3{std::sin(yaw) * cp, sp, std::cos(yaw) * cp} * distance;
    }
    mat4 view() const { return mat4::lookAt(eye(), target, {0, 1, 0}); }
    mat4 proj(float aspect, bool zeroToOne) const { return mat4::perspective(fovY, aspect, 0.5f, 1000.0f, zeroToOne); }

    // Screen (logical points) -> world ray. Returns false if degenerate.
    bool screenRay(float sx, float sy, float w, float h, float3& origin, float3& dir) const {
        if (w <= 0 || h <= 0) return false;
        float nx = sx / w * 2 - 1, ny = 1 - sy / h * 2;
        mat4 inv = (proj(w / h, false) * view()).inverse();
        float4 a = inv * float4{nx, ny, -1, 1}, b = inv * float4{nx, ny, 1, 1};
        float3 pa{a.x / a.w, a.y / a.w, a.z / a.w}, pb{b.x / b.w, b.y / b.w, b.z / b.w};
        origin = pa; dir = normalize(pb - pa);
        return true;
    }
    // Intersection with the plane y = height.
    bool screenToGround(float sx, float sy, float w, float h, float3& out, float height = 0) const {
        float3 o, d;
        if (!screenRay(sx, sy, w, h, o, d) || std::fabs(d.y) < 1e-6f) return false;
        float t = (height - o.y) / d.y;
        if (t < 0) return false;
        out = o + d * t;
        return true;
    }
    // World -> screen (logical points). Returns false if behind the camera.
    bool worldToScreen(float3 p, float w, float h, float2& out) const {
        float4 c = (proj(w / h, false) * view()) * float4{p.x, p.y, p.z, 1};
        if (c.w <= 0.0001f) return false;
        out = {(c.x / c.w * 0.5f + 0.5f) * w, (1 - (c.y / c.w * 0.5f + 0.5f)) * h};
        return true;
    }
};

} // namespace rts::render
