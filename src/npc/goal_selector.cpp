#include "npc/goal_selector.hpp"

#include "npc/drive_system.hpp"

#include <algorithm>
#include <map>

namespace tide_marks {

std::vector<GoalChoice> GoalSelector::rank(const NpcState& npc,
                                           const std::vector<NeedContent>& needs) {
    std::vector<GoalChoice> choices;
    std::map<std::string, GoalChoice> ranked;
    for (const auto& need : needs) {
        const auto state = npc.needs.find(need.id);
        NeedState fallback;
        fallback.deficit = need.initialDeficit;
        fallback.sensitivity = need.weight;
        const NeedState current = state == npc.needs.end() ? fallback : state->second;
        for (const auto& [actionId, satisfier] : need.satisfiers) {
            (void)satisfier;
            const auto evaluation = DriveSystem::evaluate(need, current, actionId);
            auto found = ranked.find(actionId);
            if (found == ranked.end()) {
                found = ranked.emplace(actionId, GoalChoice{actionId, 0.0, {}, {}}).first;
            }
            found->second.score += evaluation.utilityContribution;
            found->second.needContributions[need.id] = evaluation.utilityContribution;
            found->second.reason += (found->second.reason.empty() ? "" : " + ") +
                                    need.domain + ":" + need.id;
        }
    }
    for (const auto& [actionId, choice] : ranked) choices.push_back(choice);
    std::sort(choices.begin(), choices.end(), [](const auto& left, const auto& right) {
        if (left.score == right.score) return left.actionId < right.actionId;
        return left.score > right.score;
    });
    return choices;
}

}  // namespace tide_marks
