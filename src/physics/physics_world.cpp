#include "physics/physics_world.hpp"

#include <cmath>
#include <utility>

namespace tide_marks {

void DeterministicPhysicsWorld::addBody(PhysicsBody body) {
    bodies_.push_back(std::move(body));
}

void DeterministicPhysicsWorld::applyImpulse(const std::string& bodyId, double dx, double dy) {
    for (auto& body : bodies_) {
        if (body.id == bodyId && body.dynamic) {
            body.x += dx;
            body.y += dy;
        }
    }
}

void DeterministicPhysicsWorld::step(double fixedSeconds) {
    if (fixedSeconds <= 0.0) return;
    for (std::size_t left = 0; left < bodies_.size(); ++left) {
        for (std::size_t right = left + 1; right < bodies_.size(); ++right) {
            const auto& a = bodies_[left];
            const auto& b = bodies_[right];
            const double dx = a.x - b.x;
            const double dy = a.y - b.y;
            const double radius = a.radius + b.radius;
            if (dx * dx + dy * dy <= radius * radius) {
                events_.push_back(PhysicsEvent{"collision", a.id, b.id});
            }
        }
    }
}

std::vector<PhysicsEvent> DeterministicPhysicsWorld::drainEvents() {
    auto result = std::move(events_);
    events_.clear();
    return result;
}

}  // namespace tide_marks
