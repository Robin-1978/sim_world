#include "npc/goal_selector.hpp"

#include <algorithm>

namespace tide_marks {

std::vector<GoalChoice> GoalSelector::rank(const NpcState& npc, const VillageState& village, GameTime now) {
    std::vector<GoalChoice> choices;
    if (npc.definition.id == "mira") {
        choices.push_back({"offer_ration", village.grain < 80 ? 0.95 : 0.55,
                           village.grain < 80 ? "grain pressure" : "keep the ledger fair"});
        choices.push_back({"protect_secret", npc.pressure + 0.35, "protect a family member"});
        choices.push_back({"explain_ration", 0.35 + (now.hour() >= 6 && now.hour() < 16 ? 0.2 : 0.0), "answer a public question"});
    } else if (npc.definition.id == "lorn") {
        choices.push_back({"repair_ferry", (100.0 - village.boatIntegrity) / 100.0 + npc.pressure, "keep the crossing safe"});
        choices.push_back({"offer_crossing", 0.55 + (now.hour() >= 6 && now.hour() < 18 ? 0.2 : 0.0), "serve passengers"});
        choices.push_back({"ask_debt", 0.3 + npc.pressure, "repay a debt without abandoning work"});
    } else if (npc.definition.id == "eve") {
        choices.push_back({"share_record", village.recordsIntegrity > 80 ? 0.7 : 0.35, "preserve a verified record"});
        choices.push_back({"withhold_record", npc.pressure + 0.3, "protect the archive"});
        choices.push_back({"reveal_clue", npc.goalStage > 0 ? 0.8 : 0.25, "follow the missing name"});
    }
    std::sort(choices.begin(), choices.end(), [](const auto& left, const auto& right) {
        if (left.score == right.score) return left.actionId < right.actionId;
        return left.score > right.score;
    });
    return choices;
}

}  // namespace tide_marks
