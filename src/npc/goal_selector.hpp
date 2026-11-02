#pragma once

#include "core/types.hpp"

#include <vector>

namespace tide_marks {

struct GoalChoice {
    std::string actionId;
    double score{0.0};
    std::string reason;
};

class GoalSelector {
public:
    [[nodiscard]] static std::vector<GoalChoice> rank(const NpcState& npc,
                                                       const VillageState& village,
                                                       GameTime now);
};

}  // namespace tide_marks
