#include "npc/action_validator.hpp"

namespace tide_marks {

ValidationResult ActionValidator::validate(const DecisionContext& context, const std::string& actionId) {
    for (const auto& candidate : context.candidates) {
        if (candidate.actionId == actionId) return {true, "ok", "Candidate action is allowed."};
    }
    return {false, "unknown_action_id", "The provider selected an action outside the authored candidate set."};
}

}  // namespace tide_marks
