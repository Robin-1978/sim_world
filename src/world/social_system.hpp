#pragma once

#include "core/types.hpp"

#include <string>

namespace tide_marks {

class SocialSystem {
public:
    struct RelationshipResult {
        bool accepted{false};
        std::string code;
        std::string message;
        RelationshipAxes before;
        RelationshipAxes after;
    };
    struct GroupMetricResult {
        bool accepted{false};
        std::string code;
        std::string message;
        double before{0.0};
        double after{0.0};
    };
    struct MembershipResult {
        bool accepted{false};
        std::string code;
        std::string message;
    };

    [[nodiscard]] static RelationshipResult adjustRelationship(WorldState& state,
                                                                 const std::string& npcId,
                                                                 const std::string& target,
                                                                 const RelationshipAxes& delta);
    [[nodiscard]] static GroupMetricResult adjustGroupMetric(WorldState& state,
                                                               const std::string& groupId,
                                                               const std::string& metric,
                                                               double delta);
    [[nodiscard]] static MembershipResult setMembership(WorldState& state,
                                                         const std::string& groupId,
                                                         const std::string& npcId,
                                                         const std::string& role,
                                                         bool active);
};

}  // namespace tide_marks
