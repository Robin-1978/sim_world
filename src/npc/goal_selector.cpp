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
        const double drive = DriveSystem::drive(need, current);
        for (const auto& [actionId, satisfier] : need.satisfiers) {
            const auto learned = current.expectedRelief.find(actionId);
            const double expected = learned == current.expectedRelief.end() ? satisfier.relief : learned->second;
            const double score = drive * std::max(0.0, expected);
            auto found = ranked.find(actionId);
            if (found == ranked.end() || score > found->second.score) {
                ranked[actionId] = GoalChoice{actionId, score, need.domain + ":" + need.id};
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
