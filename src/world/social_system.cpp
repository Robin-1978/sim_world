#include "world/social_system.hpp"

#include "world/ids.hpp"

#include <algorithm>
#include <cmath>

namespace tide_marks {

namespace {
void applyDelta(RelationshipAxes& axes, const RelationshipAxes& delta) {
    axes.trust = std::clamp(axes.trust + delta.trust, 0.0, 1.0);
    axes.respect = std::clamp(axes.respect + delta.respect, 0.0, 1.0);
    axes.fear = std::clamp(axes.fear + delta.fear, 0.0, 1.0);
    axes.owed = std::clamp(axes.owed + delta.owed, 0.0, 1.0);
}
}

SocialSystem::RelationshipResult SocialSystem::adjustRelationship(WorldState& state,
                                                                    const std::string& npcId,
                                                                    const std::string& target,
                                                                    const RelationshipAxes& delta) {
    RelationshipResult result;
    const auto npc = state.npcs.find(npcId);
    if (npc == state.npcs.end()) {
        result.code = "unknown_npc";
        result.message = "The relationship actor does not exist.";
        return result;
    }
    if (target.empty() || target == npcId ||
        (!state.npcs.contains(target) && target != kPlayerId && !state.socialGroups.contains(target))) {
        result.code = "unknown_relationship_target";
        result.message = "The relationship target does not exist.";
        return result;
    }
    result.before = npc->second.relationships[target];
    result.after = result.before;
    applyDelta(result.after, delta);
    npc->second.relationships[target] = result.after;
    result.accepted = true;
    result.code = "ok";
    result.message = target;
    return result;
}

SocialSystem::GroupMetricResult SocialSystem::adjustGroupMetric(WorldState& state,
                                                                const std::string& groupId,
                                                                const std::string& metric,
                                                                double delta) {
    GroupMetricResult result;
    if (metric.empty() || !std::isfinite(delta)) {
        result.code = "invalid_group_metric";
        result.message = "A group metric needs a finite delta.";
        return result;
    }
    const auto group = state.socialGroups.find(groupId);
    if (group == state.socialGroups.end()) {
        result.code = "unknown_social_group";
        result.message = "The social group does not exist.";
        return result;
    }
    result.before = group->second.metrics[metric];
    result.after = result.before + delta;
    group->second.metrics[metric] = result.after;
    result.accepted = true;
    result.code = "ok";
    result.message = metric;
    return result;
}

SocialSystem::MembershipResult SocialSystem::setMembership(WorldState& state,
                                                            const std::string& groupId,
                                                            const std::string& npcId,
                                                            const std::string& role,
                                                            bool active) {
    MembershipResult result;
    const auto group = state.socialGroups.find(groupId);
    if (group == state.socialGroups.end()) {
        result.code = "unknown_social_group";
        result.message = "The social group does not exist.";
        return result;
    }
    if (!state.npcs.contains(npcId)) {
        result.code = "unknown_npc";
        result.message = "The group member does not exist.";
        return result;
    }
    if (active && role.empty()) {
        result.code = "invalid_group_role";
        result.message = "An active membership needs a role.";
        return result;
    }
    if (active) group->second.members[npcId] = role;
    else group->second.members.erase(npcId);
    result.accepted = true;
    result.code = "ok";
    result.message = groupId;
    return result;
}

}  // namespace tide_marks
