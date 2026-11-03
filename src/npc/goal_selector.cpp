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
        const NeedState current = state == npc.needs.end() ? NeedState{need.initialDeficit, GameTime{}, 0}
                                                           : state->second;
        const double drive = DriveSystem::drive(need, current);
        for (const auto& [actionId, relief] : need.satisfiers) {
            const double score = drive * std::max(0.0, relief);
            auto found = ranked.find(actionId);
            if (found == ranked.end() || score > found->second.score) {
                ranked[actionId] = GoalChoice{actionId, score, need.taxonomy + ":" + need.id};
            }
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
