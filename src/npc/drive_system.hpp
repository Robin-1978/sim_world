#pragma once

#include "content/content_pack.hpp"
#include "core/types.hpp"

#include <string>
#include <vector>

namespace tide_marks {

struct NeedChange {
    std::string needId;
    double before{0.0};
    double after{0.0};
    bool becameCritical{false};
};

class DriveSystem {
public:
    static void initialize(NpcState& npc, const std::vector<NeedContent>& definitions, GameTime now);
    [[nodiscard]] static std::vector<NeedChange> advance(
        NpcState& npc,
        const std::vector<NeedContent>& definitions,
        GameTime from,
        GameTime to);
    [[nodiscard]] static std::vector<NeedChange> relieve(
        NpcState& npc,
        const std::vector<NeedContent>& definitions,
        const std::string& actionId,
        GameTime now);
    [[nodiscard]] static double drive(const NeedContent& definition, const NeedState& state);
};

}  // namespace tide_marks
