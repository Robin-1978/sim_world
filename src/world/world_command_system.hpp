#pragma once

#include "core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace tide_marks {

class WorldService;
struct CommandResult;
struct RelationshipAxes;
enum class EntityLifecycle;

// Coordinates public world commands without owning facts. Every operation
// uses WorldService's single transaction, event and WorldState boundary.
class WorldCommandSystem {
public:
    explicit WorldCommandSystem(WorldService& world) : world_(world) {}

    CommandResult createEntity(const std::string& id, const std::string& archetypeId,
                               Json metadata, const std::string& turnId);
    CommandResult setEntityLifecycle(const std::string& id, EntityLifecycle lifecycle,
                                     const std::string& turnId);
    CommandResult setLinkState(const std::string& linkId, bool traversable, bool locked,
                               const std::string& turnId);
    CommandResult transferResource(const std::string& source, const std::string& target,
                                   const std::string& resource, int amount,
                                   const std::string& actor, const std::string& turnId);
    CommandResult tradeResources(const std::string& seller, const std::string& buyer,
                                 const std::string& sellerResource, int sellerAmount,
                                 const std::string& buyerResource, int buyerAmount,
                                 const std::string& actor, const std::string& turnId);
    CommandResult adjustRelationship(const std::string& npcId, const std::string& target,
                                     RelationshipAxes delta, const std::string& actor,
                                     const std::string& turnId, const std::string& reason);
    CommandResult adjustGroupMetric(const std::string& groupId, const std::string& metric,
                                    double delta, const std::string& actor,
                                    const std::string& turnId, const std::string& reason);
    CommandResult setGroupMembership(const std::string& groupId, const std::string& npcId,
                                     const std::string& role, bool active,
                                     const std::string& actor, const std::string& turnId);
    CommandResult moveNpc(const std::string& npcId, const std::string& destination,
                          const std::string& actor, const std::string& turnId);
    [[nodiscard]] std::optional<std::vector<std::string>> planRoute(const std::string& from,
                                                                     const std::string& to) const;

private:
    friend class WorldService;
    [[nodiscard]] static std::optional<std::vector<std::string>> planRoute(
        const WorldService& world, const std::string& from, const std::string& to);
    WorldService& world_;
};

}  // namespace tide_marks
