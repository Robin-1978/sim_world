#pragma once

#include "content/content_pack.hpp"
#include "core/types.hpp"

#include <cstdint>

namespace tide_marks {

// Computes the next observable simulation boundary. It is deliberately a
// pure planner: advancing time and applying domain rules remain WorldService
// responsibilities.
class SimulationScheduler {
public:
    [[nodiscard]] static std::int64_t nextBoundary(const WorldState& state,
                                                   const WorldContentPack& content,
                                                   std::int64_t target);
};

}  // namespace tide_marks
