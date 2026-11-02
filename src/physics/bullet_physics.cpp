#include "physics/bullet_physics.hpp"

#include <btBulletDynamicsCommon.h>

#include <map>
#include <memory>
#include <string>

namespace tide_marks {

namespace {

class BulletPhysicsWorld final : public IPhysicsWorld {
public:
    BulletPhysicsWorld()
        : collisionConfiguration_(std::make_unique<btDefaultCollisionConfiguration>()),
          dispatcher_(std::make_unique<btCollisionDispatcher>(collisionConfiguration_.get())),
          broadphase_(std::make_unique<btDbvtBroadphase>()),
          solver_(std::make_unique<btSequentialImpulseConstraintSolver>()),
          world_(std::make_unique<btDiscreteDynamicsWorld>(dispatcher_.get(), broadphase_.get(), solver_.get(),
                                                           collisionConfiguration_.get())) {
        world_->setGravity(btVector3(0.0, 0.0, 0.0));
    }

    ~BulletPhysicsWorld() override {
        for (auto& [id, body] : bodies_) {
            world_->removeRigidBody(body.body.get());
            delete static_cast<std::string*>(body.body->getUserPointer());
            delete body.motion;
            delete body.shape;
        }
    }

    void addBody(PhysicsBody body) override {
        auto shape = new btSphereShape(static_cast<btScalar>(body.radius));
        btTransform transform;
        transform.setIdentity();
        transform.setOrigin(btVector3(static_cast<btScalar>(body.x), 0.0, static_cast<btScalar>(body.y)));
        auto* motion = new btDefaultMotionState(transform);
        const btScalar mass = body.dynamic ? 1.0 : 0.0;
        btVector3 inertia(0.0, 0.0, 0.0);
        if (mass > 0.0) shape->calculateLocalInertia(mass, inertia);
        btRigidBody::btRigidBodyConstructionInfo info(mass, motion, shape, inertia);
        auto rigid = std::make_unique<btRigidBody>(info);
        rigid->setUserPointer(new std::string(body.id));
        world_->addRigidBody(rigid.get());
        bodies_[body.id] = Body{std::move(rigid), motion, shape};
    }

    void applyImpulse(const std::string& bodyId, double dx, double dy) override {
        const auto found = bodies_.find(bodyId);
        if (found == bodies_.end()) return;
        found->second.body->applyCentralImpulse(btVector3(static_cast<btScalar>(dx), 0.0, static_cast<btScalar>(dy)));
    }

    void step(double fixedSeconds) override {
        if (fixedSeconds <= 0.0) return;
        world_->stepSimulation(static_cast<btScalar>(fixedSeconds), 1, static_cast<btScalar>(fixedSeconds));
        const int manifolds = dispatcher_->getNumManifolds();
        for (int index = 0; index < manifolds; ++index) {
            const auto* manifold = dispatcher_->getManifoldByIndexInternal(index);
            if (manifold->getNumContacts() == 0) continue;
            const auto* first = static_cast<const btRigidBody*>(manifold->getBody0());
            const auto* second = static_cast<const btRigidBody*>(manifold->getBody1());
            const auto* firstId = static_cast<const std::string*>(first->getUserPointer());
            const auto* secondId = static_cast<const std::string*>(second->getUserPointer());
            if (firstId != nullptr && secondId != nullptr) events_.push_back({"collision", *firstId, *secondId});
        }
    }

    [[nodiscard]] std::vector<PhysicsEvent> drainEvents() override {
        auto result = std::move(events_);
        events_.clear();
        return result;
    }

private:
    struct Body {
        std::unique_ptr<btRigidBody> body;
        btMotionState* motion;
        btCollisionShape* shape;
    };
    std::unique_ptr<btDefaultCollisionConfiguration> collisionConfiguration_;
    std::unique_ptr<btCollisionDispatcher> dispatcher_;
    std::unique_ptr<btBroadphaseInterface> broadphase_;
    std::unique_ptr<btSequentialImpulseConstraintSolver> solver_;
    std::unique_ptr<btDiscreteDynamicsWorld> world_;
    std::map<std::string, Body> bodies_;
    std::vector<PhysicsEvent> events_;
};

}  // namespace

std::unique_ptr<IPhysicsWorld> makeBulletPhysicsWorld() {
    return std::make_unique<BulletPhysicsWorld>();
}

}  // namespace tide_marks
