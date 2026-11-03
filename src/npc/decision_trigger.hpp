#pragma once

#include <string_view>

namespace tide_marks {

// Decision channels are Runtime protocol, not world content. Keeping their
// identifiers in one place prevents story/action strings from leaking into
// provider and WorldService policy code.
struct DecisionTrigger {
    static constexpr std::string_view PlayerDialogue = "player_dialogue";
    static constexpr std::string_view AutonomousIntent = "npc.autonomous_intent";

    [[nodiscard]] static constexpr bool isPlayerDialogue(std::string_view value) {
        return value == PlayerDialogue;
    }

    [[nodiscard]] static constexpr bool isAutonomous(std::string_view value) {
        return !value.empty() && !isPlayerDialogue(value);
    }
};

}  // namespace tide_marks
