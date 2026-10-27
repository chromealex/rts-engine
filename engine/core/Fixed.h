// Deterministic fixed-point arithmetic.
// Q47.16 stored in int64. All gameplay math MUST use these types:
// float/double give different results across CPUs/compilers/WASM and break lockstep.
#pragma once
#include <cstdint>
#include <compare>
#include <string>

namespace rts {

namespace detail {
// Portable signed 64x64 -> 128 multiply, then shift right by `shift` with round-half-up.
// Rounding (instead of truncation) keeps per-tick accumulations like growth*dt unbiased.
// Works on MSVC (no __int128), GCC, Clang, Emscripten; both paths are bit-identical.
inline int64_t mulShift(int64_t a, int64_t b, int shift) {
    const uint64_t half = uint64_t(1) << (shift - 1);
#if defined(__SIZEOF_INT128__) && !defined(RTS_FORCE_PORTABLE_MUL)
    __extension__ typedef __int128 i128;
    i128 p = static_cast<i128>(a) * b + static_cast<i128>(half);
    return static_cast<int64_t>(p >> shift);
#else
    const bool neg = (a < 0) != (b < 0);
    uint64_t ua = a < 0 ? 0ull - static_cast<uint64_t>(a) : static_cast<uint64_t>(a);
    uint64_t ub = b < 0 ? 0ull - static_cast<uint64_t>(b) : static_cast<uint64_t>(b);
    uint64_t aLo = ua & 0xffffffffull, aHi = ua >> 32;
    uint64_t bLo = ub & 0xffffffffull, bHi = ub >> 32;
    uint64_t ll = aLo * bLo, lh = aLo * bHi, hl = aHi * bLo, hh = aHi * bHi;
    uint64_t mid = (ll >> 32) + (lh & 0xffffffffull) + (hl & 0xffffffffull);
    uint64_t lo = (ll & 0xffffffffull) | (mid << 32);
    uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    if (neg) { // two's complement negate 128-bit
        lo = ~lo + 1; hi = ~hi + (lo == 0 ? 1 : 0);
    }
    { uint64_t nlo = lo + half; hi += (nlo < lo) ? 1 : 0; lo = nlo; } // + half with carry
    // arithmetic shift of (hi:lo) right by shift (0 < shift < 64)
    uint64_t r = (lo >> shift) | (hi << (64 - shift));
    return static_cast<int64_t>(r);
#endif
}
} // namespace detail

struct Fixed {
    static constexpr int kFracBits = 16;
    static constexpr int64_t kOne = int64_t(1) << kFracBits;

    int64_t raw = 0;

    constexpr Fixed() = default;
    static constexpr Fixed fromRaw(int64_t r) { Fixed f; f.raw = r; return f; }
    static constexpr Fixed fromInt(int64_t i) { return fromRaw(i * kOne); }
    // num/den evaluated with integer math only (safe for data loading and constants)
    static constexpr Fixed fromRatio(int64_t num, int64_t den) {
        if (den < 0) { num = -num; den = -den; }
        const int64_t n = num * kOne;
        return fromRaw(n >= 0 ? (n + den / 2) / den : -((-n + den / 2) / den)); // round to nearest
    }
    // Parse decimal string ("1.25", "-3", "0.5") without touching floating point.
    static Fixed parse(const std::string& s, bool* ok = nullptr);

    constexpr int64_t toInt() const { return raw >= 0 ? raw >> kFracBits : -((-raw) >> kFracBits); } // truncate toward 0
    constexpr int64_t floorInt() const { return raw >> kFracBits; }
    // For rendering/debug only. Never feed back into the simulation.
    double toDouble() const { return static_cast<double>(raw) / static_cast<double>(kOne); }
    std::string toString() const;

    constexpr Fixed operator-() const { return fromRaw(-raw); }
    constexpr Fixed operator+(Fixed o) const { return fromRaw(raw + o.raw); }
    constexpr Fixed operator-(Fixed o) const { return fromRaw(raw - o.raw); }
    Fixed operator*(Fixed o) const { return fromRaw(detail::mulShift(raw, o.raw, kFracBits)); }
    Fixed operator/(Fixed o) const;
    constexpr Fixed operator*(int64_t i) const { return fromRaw(raw * i); }
    constexpr Fixed operator/(int64_t i) const { return fromRaw(raw / i); }

    Fixed& operator+=(Fixed o) { raw += o.raw; return *this; }
    Fixed& operator-=(Fixed o) { raw -= o.raw; return *this; }
    Fixed& operator*=(Fixed o) { *this = *this * o; return *this; }
    Fixed& operator/=(Fixed o) { *this = *this / o; return *this; }

    constexpr auto operator<=>(const Fixed&) const = default;
};

constexpr Fixed operator""_fx(unsigned long long v) { return Fixed::fromInt(static_cast<int64_t>(v)); }

inline constexpr Fixed fxAbs(Fixed a) { return a.raw < 0 ? -a : a; }
inline constexpr Fixed fxMin(Fixed a, Fixed b) { return a < b ? a : b; }
inline constexpr Fixed fxMax(Fixed a, Fixed b) { return a < b ? b : a; }
inline constexpr Fixed fxClamp(Fixed v, Fixed lo, Fixed hi) { return fxMin(fxMax(v, lo), hi); }
Fixed fxSqrt(Fixed a);

struct Vec2 {
    Fixed x, y;
    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(Fixed s) const { return {x * s, y * s}; }
    Vec2 operator/(Fixed s) const { return {x / s, y / s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Fixed dot(Vec2 o) const { return x * o.x + y * o.y; }
    Fixed lengthSq() const { return dot(*this); }
    Fixed length() const { return fxSqrt(lengthSq()); }
    constexpr bool operator==(const Vec2&) const = default;
};

// Simulation space is 3D: X/Z ground plane, Y up (height for terrain, air units, projectiles).
struct Vec3 {
    Fixed x, y, z;
    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(Fixed s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(Fixed s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    Fixed dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    Fixed lengthSq() const { return dot(*this); }
    Fixed length() const { return fxSqrt(lengthSq()); }
    Vec2 xz() const { return {x, z}; }
    constexpr bool operator==(const Vec3&) const = default;
};

// Move `from` toward `to` by at most `step`. Returns true if destination reached.
bool moveTowards(Vec3& from, Vec3 to, Fixed step);

} // namespace rts
