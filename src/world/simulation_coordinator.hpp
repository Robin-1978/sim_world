#pragma once

#include "core/types.hpp"

#include <cstdint>

namespace tide_marks {

class WorldService;

// Coordinates time-boundary execution without owning world facts. The
// coordinator is deliberately a thin application layer: WorldService remains
// the single fact and transaction boundary, while domain systems continue to
// implement their own rules behind its private callbacks.
class SimulationCoordinator {
public:
    explicit SimulationCoordinator(WorldService& world) : world_(world) {}

    void advanceMinutes(std::int64_t minutes);
    void advanceTo(GameTime target);

private:
    WorldService& world_;
};

}  // namespace tide_marks
