#include "npc/state_machine.hpp"

namespace tide_marks {

std::string toString(NpcMode mode) {
    switch (mode) {
    case NpcMode::AtHome: return "at_home";
    case NpcMode::Working: return "working";
    case NpcMode::Traveling: return "traveling";
    case NpcMode::InConversation: return "in_conversation";
    case NpcMode::Interrupted: return "interrupted";
    case NpcMode::Unavailable: return "unavailable";
    }
    return "working";
}

NpcMode npcModeFromString(const std::string& value) {
    if (value == "at_home") return NpcMode::AtHome;
    if (value == "traveling") return NpcMode::Traveling;
    if (value == "in_conversation") return NpcMode::InConversation;
    if (value == "interrupted") return NpcMode::Interrupted;
    if (value == "unavailable") return NpcMode::Unavailable;
    return NpcMode::Working;
}

NpcMode transition(NpcMode current, NpcSignal signal) {
    if (signal == NpcSignal::Disable) return NpcMode::Unavailable;
    if (signal == NpcSignal::StartConversation && current != NpcMode::Unavailable) return NpcMode::InConversation;
    if (signal == NpcSignal::Interrupt && current != NpcMode::Unavailable) return NpcMode::Interrupted;
    if (signal == NpcSignal::StartTravel && current != NpcMode::Unavailable) return NpcMode::Traveling;
    if (signal == NpcSignal::AtHome && current != NpcMode::InConversation) return NpcMode::AtHome;
    if (signal == NpcSignal::AtWork && current != NpcMode::InConversation) return NpcMode::Working;
    if (signal == NpcSignal::EndConversation && current == NpcMode::InConversation) return NpcMode::Working;
    if (signal == NpcSignal::Resume && current == NpcMode::Interrupted) return NpcMode::Working;
    return current;
}

}  // namespace tide_marks
