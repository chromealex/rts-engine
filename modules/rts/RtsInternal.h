// Small helpers shared by the RTS module implementation files (not part of the public API).
#pragma once
#include "RtsModule.h"

namespace rts::game::detail {

inline const Fixed kHalf = Fixed::fromRatio(1, 2);
inline const Fixed kOne = Fixed::fromInt(1);

inline Fixed dist2D(Vec3 a, Vec3 b) { return (a - b).xz().length(); }
inline Vec3 flat(Vec3 v) { v.y = Fixed{}; return v; }
inline bool canHit(const WeaponDef& w, uint8_t layer) { return (w.targets & layer) != 0; }
inline Fixed maxWeaponRange(const UnitType& t) {
    Fixed r{};
    for (auto& w : t.weapons) r = fxMax(r, w.range);
    return r;
}
inline Fixed secondsToTicks(Fixed s, uint32_t rate) { return s * int64_t(rate); }

} // namespace rts::game::detail
