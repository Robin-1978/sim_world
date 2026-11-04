#pragma once

namespace tide_marks {

class WorldService;

// Rebuilds the query/render projection from committed world facts. It never
// owns entity state and cannot be used as a second write path.
class EntityProjectionSystem {
public:
    explicit EntityProjectionSystem(WorldService& world) : world_(world) {}

    void rebuild();
    void syncNpc();

private:
    WorldService& world_;
};

}  // namespace tide_marks
