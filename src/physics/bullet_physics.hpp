#pragma once

#include "physics/physics_world.hpp"

#include <memory>

namespace tide_marks {

// Bullet is hidden behind this factory. Callers depend only on IPhysicsWorld.
std::unique_ptr<IPhysicsWorld> makeBulletPhysicsWorld();

}  // namespace tide_marks
