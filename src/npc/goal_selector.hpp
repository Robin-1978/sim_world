#pragma once

#include "content/content_pack.hpp"
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
    [[nodiscard]] static std::vector<GoalChoice> rank(
        const NpcState& npc,
        const std::vector<NeedContent>& needs);
};

}  // namespace tide_marks
