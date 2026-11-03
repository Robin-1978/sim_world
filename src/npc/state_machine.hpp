#pragma once

#include <string>

namespace tide_marks {

enum class NpcMode { AtHome, Working, Executing, Traveling, InConversation, Interrupted, Unavailable };
enum class NpcSignal {
    AtHome,
    AtWork,
    StartProcess,
    FinishProcess,
    StartTravel,
    StartConversation,
    EndConversation,
    Interrupt,
    CancelProcess,
    Resume,
    Disable
};

std::string toString(NpcMode mode);
NpcMode npcModeFromString(const std::string& value);
NpcMode transition(NpcMode current, NpcSignal signal);

}  // namespace tide_marks
