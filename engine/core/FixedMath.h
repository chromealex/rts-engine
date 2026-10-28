// Deterministic fixed-point trigonometry (simulation-safe). Angles are in TURNS (1.0 = 360 degrees,
// 0.25 = 90 degrees) so no irrational constant enters the simulation. Tables were generated offline
// (see the comment in FixedMath.cpp) and are embedded as integers: identical on every platform.
#pragma once
#include "Fixed.h"

namespace rts {

Fixed fxSinTurns(Fixed turns);           // |error| <= 2 raw units (Q16)
Fixed fxCosTurns(Fixed turns);
Fixed fxTanTurns(Fixed turns);           // undefined at +-0.25 (returns a large value of the right sign)
Fixed fxAtan2Turns(Fixed y, Fixed x);    // (-0.5, 0.5], atan2(0,0) = 0; CORDIC, |error| <= 2 raw units
Fixed fxAsinTurns(Fixed v);              // [-0.25, 0.25], v clamped to [-1, 1]
Fixed fxAcosTurns(Fixed v);              // [0, 0.5]

// Unit vector helpers (fixed-point, deterministic).
Vec3 fxNormalize(Vec3 v);                // zero vector -> zero vector
Vec3 fxCross(Vec3 a, Vec3 b);

} // namespace rts
