// Components of the agent-mech mechanics (RtsMech*.cpp). Optional: an object carries them only when
// its game uses the mechanic, and they are hashed only when present.
#pragma once
#include <vector>
#include "../../engine/core/ECS.h"
#include "../../engine/core/Fixed.h"

namespace rts::game {

// Stance (Age of Empires, Warcraft): how an idle unit reacts to enemies.
struct Stance { uint8_t mode = 0; Vec3 home{}; bool homeSet = false; };
// Retreating to the nearest own retreat point: faster, takes less damage, does not fight.
struct Retreating { Entity to = kNullEntity; Vec3 point{}; };
// A transport (or garrisoned structure) and its passengers, in boarding order.
struct Transport { std::vector<Entity> cargo; int used = 0; Vec3 unloadAt{}; bool unloading = false, unloadAtPoint = false; uint32_t nextUnload = 0; };
// A passenger inside a transport (flag kUnitInCargo): off the map, moves with it.
struct Boarded { Entity carrier = kNullEntity; bool fires = false; };

} // namespace rts::game
