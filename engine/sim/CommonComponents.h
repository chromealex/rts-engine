// Components shared by all gameplay modules.
#pragma once
#include <cstdint>
#include "../core/Fixed.h"

namespace rts {

struct Transform {
    Vec3 pos{};
    Vec3 prevPos{};   // position at the previous tick, for render interpolation
    Fixed yaw{};      // facing, in "turns" [0,1) to avoid trig in the sim
};

struct Owner {
    uint8_t player = 0; // 0 = neutral
};

} // namespace rts
