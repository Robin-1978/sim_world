#pragma once

#include <string>
#include <vector>

namespace tide_marks {

struct PhysicsBody {
    std::string id;
    double x{0.0};
    double y{0.0};
    double radius{0.5};
    bool dynamic{true};
};

struct PhysicsEvent {
    std::string type;
    std::string bodyA;
    std::string bodyB;
};

class IPhysicsWorld {
public:
    virtual ~IPhysicsWorld() = default;
    virtual void addBody(PhysicsBody body) = 0;
    virtual void applyImpulse(const std::string& bodyId, double dx, double dy) = 0;
    virtual void step(double fixedSeconds) = 0;
    [[nodiscard]] virtual std::vector<PhysicsEvent> drainEvents() = 0;
};

// Deterministic fallback used by headless builds and replay tests. The
// optional Bullet adapter implements the same interface when engine deps are
// enabled, so gameplay code never includes Bullet headers.
class DeterministicPhysicsWorld final : public IPhysicsWorld {
public:
    void addBody(PhysicsBody body) override;
    void applyImpulse(const std::string& bodyId, double dx, double dy) override;
    void step(double fixedSeconds) override;
    [[nodiscard]] std::vector<PhysicsEvent> drainEvents() override;

private:
    std::vector<PhysicsBody> bodies_;
    std::vector<PhysicsEvent> events_;
};

}  // namespace tide_marks
