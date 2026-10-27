#include "Fixed.h"
#include <cstdio>

namespace rts {

Fixed Fixed::operator/(Fixed o) const {
    if (o.raw == 0) return fromRaw(raw >= 0 ? INT64_MAX : INT64_MIN); // saturate; caller bug
    // Integer-only long division that never needs 128-bit types:
    // a/b = q + r/b  ->  (q << 16) + (r << 16) / b. Both terms truncate toward zero.
    const int64_t q = raw / o.raw;
    const int64_t r = raw % o.raw;
    return fromRaw(q * kOne + (r * kOne) / o.raw);
}

static uint64_t isqrt64(uint64_t v) {
    uint64_t res = 0;
    uint64_t bit = uint64_t(1) << 62;
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return res;
}

Fixed fxSqrt(Fixed a) {
    if (a.raw <= 0) return Fixed{};
    const uint64_t r = static_cast<uint64_t>(a.raw);
    if (r < (uint64_t(1) << 47))
        return Fixed::fromRaw(static_cast<int64_t>(isqrt64(r << Fixed::kFracBits)));
    // Very large values: lose 8 bits of precision but stay deterministic.
    return Fixed::fromRaw(static_cast<int64_t>(isqrt64(r) << (Fixed::kFracBits / 2)));
}

Fixed Fixed::parse(const std::string& s, bool* ok) {
    size_t i = 0;
    bool neg = false, good = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
    int64_t ip = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ip = ip * 10 + (s[i] - '0'); ++i; good = true; }
    int64_t frac = 0, scale = 1;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (scale < 1000000000) { frac = frac * 10 + (s[i] - '0'); scale *= 10; }
            ++i; good = true;
        }
    }
    if (i != s.size()) good = false; // exponents etc. are not supported in gameplay data
    if (ok) *ok = good;
    int64_t raw = ip * kOne + (frac * kOne + scale / 2) / scale; // round to nearest
    return fromRaw(neg ? -raw : raw);
}

std::string Fixed::toString() const {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", toDouble());
    return buf;
}

bool moveTowards(Vec3& from, Vec3 to, Fixed step) {
    Vec3 d = to - from;
    Fixed dist = d.length();
    if (dist <= step || dist.raw == 0) { from = to; return true; }
    from += d * (step / dist);
    return false;
}

} // namespace rts
