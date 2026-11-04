#pragma once

namespace tide_marks {

class WorldService;
struct Command;
struct CommandResult;

// Parses the stable Runtime command vocabulary and routes each command to a
// WorldService high-level operation. It owns no state and does not define
// content-specific NPC actions.
class CommandDispatcher {
public:
    explicit CommandDispatcher(WorldService& world) : world_(world) {}

    [[nodiscard]] CommandResult submit(const Command& command);

private:
    WorldService& world_;
};

}  // namespace tide_marks
