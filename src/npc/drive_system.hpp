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
    double expectedRelief{0.0};
    double actualRelief{0.0};
    double expectationBefore{0.0};
    double expectationAfter{0.0};
    double sensitivityBefore{1.0};
    double sensitivityAfter{1.0};
    bool adapted{false};
};

// Stable decision-facing projection of a need. urgency is normalised to
// [0, 1], effectiveWeight combines authored priority and sensitivity, and
// utilityContribution is comparable inside one NPC's authored content pack.
struct DriveEvaluation {
    double urgency{0.0};
    double effectiveWeight{0.0};
    double expectedRelief{0.0};
    double confidence{1.0};
    double utilityContribution{0.0};
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
        GameTime now,
        const DriveAdaptationPolicy& policy);
    [[nodiscard]] static DriveEvaluation evaluate(
        const NeedContent& definition,
        const NeedState& state,
        const std::string& actionId);
    [[nodiscard]] static double drive(const NeedContent& definition, const NeedState& state);
};

}  // namespace tide_marks
