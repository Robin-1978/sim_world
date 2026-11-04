#include "world/world_command_system.hpp"

#include "navigation/path_planner.hpp"
#include "world/ids.hpp"
#include "world/resource_system.hpp"
#include "world/social_system.hpp"
#include "world/world_service.hpp"

#include <algorithm>

namespace tide_marks {

CommandResult WorldCommandSystem::createEntity(const std::string& id, const std::string& archetypeId,
                                               Json metadata, const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    if (id.empty() || archetypeId.empty()) return world_.reject("invalid_entity", "Entity id and archetype are required.");
    if (world_.state_.entities.contains(id)) return world_.reject("duplicate_entity", "That entity id already exists.");
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) return world_.reject("duplicate_turn", "The turn was already applied.");
    const auto archetype = std::find_if(world_.content_.entityArchetypes.begin(), world_.content_.entityArchetypes.end(),
                                        [&archetypeId](const auto& item) { return item.id == archetypeId; });
    if (archetype == world_.content_.entityArchetypes.end()) return world_.reject("unknown_archetype", "The entity archetype is not registered.");
    Json merged = archetype->defaultMetadata;
    for (auto it = metadata.begin(); it != metadata.end(); ++it) merged[it.key()] = it.value();
    world_.state_.entities.emplace(id, WorldEntityState{id, archetype->kind, EntityLifecycle::Active, std::move(merged)});
    world_.state_.inventories.try_emplace(id);
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent(
        "entity.created", kWorldActor, id, "Runtime entity created",
        Json{{"entity_id", id}, {"archetype", archetypeId}, {"kind", archetype->kind},
             {"metadata", world_.state_.entities.at(id).metadata}});
    return CommandResult{true, "ok", id, event};
}

CommandResult WorldCommandSystem::setEntityLifecycle(const std::string& id, EntityLifecycle lifecycle,
                                                     const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    auto found = world_.state_.entities.find(id);
    if (found == world_.state_.entities.end()) return world_.reject("unknown_entity", "That entity does not exist.");
    if (found->second.lifecycle == EntityLifecycle::Destroyed) return world_.reject("destroyed_entity", "A destroyed entity cannot be revived.");
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) return world_.reject("duplicate_turn", "The turn was already applied.");
    found->second.lifecycle = lifecycle;
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent("entity.lifecycle", kWorldActor, id, "Runtime entity lifecycle changed",
                                       Json{{"entity_id", id}, {"lifecycle", toString(lifecycle)}});
    return CommandResult{true, "ok", toString(lifecycle), event};
}

CommandResult WorldCommandSystem::setLinkState(const std::string& linkId, bool traversable, bool locked,
                                               const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    const auto entity = world_.state_.entities.find(linkEntityId(linkId));
    if (entity == world_.state_.entities.end()) return world_.reject("unknown_link", "That link does not exist.");
    if (entity->second.lifecycle != EntityLifecycle::Active) return world_.reject("inactive_link", "The link is not active.");
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) return world_.reject("duplicate_turn", "The turn was already applied.");
    world_.state_.links[linkId] = RuntimeLinkState{traversable, locked};
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent("link.state", kWorldActor, linkEntityId(linkId), "Traversal link state changed",
                                       Json{{"link_id", linkId}, {"traversable", traversable}, {"locked", locked}});
    return CommandResult{true, "ok", linkId, event};
}

CommandResult WorldCommandSystem::transferResource(const std::string& source, const std::string& target,
                                                  const std::string& resource, int amount,
                                                  const std::string& actor, const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) return world_.reject("duplicate_turn", "The transfer was already applied.");
    const auto sourceId = ResourceSystem::resolveParty(world_.state_, source);
    const auto targetId = ResourceSystem::resolveParty(world_.state_, target);
    if (actor != kWorldActor && actor != kPlayerId && actor != sourceId && actor != targetId &&
        actor != source && actor != target) return world_.reject("unauthorized", "The actor is not a party to this transfer.");
    const auto result = ResourceSystem::transfer(world_.state_, world_.content_, source, target, resource, amount);
    if (!result.accepted) return world_.reject(result.code, result.message);
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent(
        "resource.transferred", actor, targetId, "A registered resource transfer changed ownership quantities",
        Json{{"resource", resource}, {"amount", amount}, {"source", sourceId}, {"target", targetId},
             {"source_before", result.sourceBefore}, {"source_after", result.sourceAfter},
             {"target_before", result.targetBefore}, {"target_after", result.targetAfter}});
    return CommandResult{true, "ok", resource, event};
}

CommandResult WorldCommandSystem::tradeResources(const std::string& seller, const std::string& buyer,
                                                const std::string& sellerResource, int sellerAmount,
                                                const std::string& buyerResource, int buyerAmount,
                                                const std::string& actor, const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    if (sellerAmount <= 0 || buyerAmount <= 0 || sellerResource.empty() || buyerResource.empty()) return world_.reject("invalid_trade", "A trade needs two positive resource quantities.");
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) return world_.reject("duplicate_turn", "The trade was already applied.");
    if (actor != kWorldActor && actor != kPlayerId && actor != seller && actor != buyer) return world_.reject("unauthorized", "The actor is not a party to this trade.");
    const auto first = world_.transferResource(seller, buyer, sellerResource, sellerAmount, actor);
    if (!first.accepted) return world_.reject(first.code, first.message);
    const auto second = world_.transferResource(buyer, seller, buyerResource, buyerAmount, actor);
    if (!second.accepted) {
        transaction.rollback();
        return world_.reject(second.code, second.message);
    }
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent("trade.settled", actor, seller,
                                       "Two parties exchanged configured resources atomically",
                                       Json{{"seller", seller}, {"buyer", buyer}, {"seller_resource", sellerResource},
                                            {"seller_amount", sellerAmount}, {"buyer_resource", buyerResource},
                                            {"buyer_amount", buyerAmount}});
    return CommandResult{true, "ok", "trade", event};
}

CommandResult WorldCommandSystem::adjustRelationship(const std::string& npcId, const std::string& target,
                                                    RelationshipAxes delta, const std::string& actor,
                                                    const std::string& turnId, const std::string& reason) {
    WorldService::TransactionScope transaction(world_);
    if (actor != kWorldActor && actor != kPlayerId && actor != npcId) return world_.reject("unauthorized", "Only the actor or world authority may change this relationship.");
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) return world_.reject("duplicate_turn", "The relationship change was already applied.");
    const auto result = SocialSystem::adjustRelationship(world_.state_, npcId, target, delta);
    if (!result.accepted) return world_.reject(result.code, result.message);
    ++world_.state_.npcs.at(npcId).stateVersion;
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent("relationship.adjusted", actor, target,
                                       reason.empty() ? "A relationship changed" : reason,
                                       Json{{"npc_id", npcId}, {"target", target}, {"trust", delta.trust},
                                            {"respect", delta.respect}, {"fear", delta.fear}, {"owed", delta.owed}});
    return CommandResult{true, "ok", target, event};
}

CommandResult WorldCommandSystem::adjustGroupMetric(const std::string& groupId, const std::string& metric,
                                                   double delta, const std::string& actor,
                                                   const std::string& turnId, const std::string& reason) {
    WorldService::TransactionScope transaction(world_);
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) return world_.reject("duplicate_turn", "The group metric change was already applied.");
    const auto result = SocialSystem::adjustGroupMetric(world_.state_, groupId, metric, delta);
    if (!result.accepted) return world_.reject(result.code, result.message);
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent("social.group_metric", actor, groupId,
                                       reason.empty() ? "A social group metric changed" : reason,
                                       Json{{"group_id", groupId}, {"metric", metric}, {"before", result.before},
                                            {"after", result.after}, {"delta", delta}});
    return CommandResult{true, "ok", metric, event};
}

CommandResult WorldCommandSystem::setGroupMembership(const std::string& groupId, const std::string& npcId,
                                                    const std::string& role, bool active,
                                                    const std::string& actor, const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) return world_.reject("duplicate_turn", "The membership change was already applied.");
    const auto result = SocialSystem::setMembership(world_.state_, groupId, npcId, role, active);
    if (!result.accepted) return world_.reject(result.code, result.message);
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent(active ? "social.member_joined" : "social.member_left", actor, groupId,
                                       active ? "An NPC joined a social group" : "An NPC left a social group",
                                       Json{{"group_id", groupId}, {"npc_id", npcId}, {"role", role}});
    return CommandResult{true, "ok", groupId, event};
}

CommandResult WorldCommandSystem::moveNpc(const std::string& npcId, const std::string& destination,
                                          const std::string& actor, const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) return world_.reject("duplicate_turn", "The movement was already applied.");
    const auto npcFound = world_.state_.npcs.find(npcId);
    if (npcFound == world_.state_.npcs.end()) return world_.reject("unknown_npc", "That NPC does not exist.");
    if (actor != kWorldActor && actor != npcId && actor != kPlayerId) return world_.reject("unauthorized", "Only the NPC or an authorized world actor can move it.");
    const auto destinationEntity = world_.state_.entities.find(spatialEntityId(destination));
    if (destinationEntity == world_.state_.entities.end() || destinationEntity->second.lifecycle != EntityLifecycle::Active) return world_.reject("unknown_destination", "The destination is not an active spatial entity.");
    if (npcFound->second.location == destination) return CommandResult{true, "ok", destination, std::nullopt};
    const auto route = planRoute(world_, npcFound->second.location, destination);
    if (!route) return world_.reject("no_route", "No active topology route connects the NPC to the destination.");
    const std::string previous = npcFound->second.location;
    npcFound->second.location = destination;
    ++npcFound->second.stateVersion;
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto event = world_.addEvent("entity.moved", actor, npcEntityId(npcId), "An NPC arrived at an authored location",
                                       Json{{"npc_id", npcId}, {"from", previous}, {"to", destination}, {"route", *route}});
    world_.syncNpcEntityState();
    return CommandResult{true, "ok", destination, event};
}

std::optional<std::vector<std::string>> WorldCommandSystem::planRoute(const std::string& from,
                                                                       const std::string& to) const {
    return planRoute(world_, from, to);
}

std::optional<std::vector<std::string>> WorldCommandSystem::planRoute(const WorldService& world,
                                                                       const std::string& from,
                                                                       const std::string& to) {
    GlobalPlanner planner;
    for (const auto& spatial : world.content_.spatialEntities) planner.addNode(spatial.id);
    for (const auto& authored : world.content_.links) {
        const auto entity = world.state_.entities.find(linkEntityId(authored.id));
        const auto runtime = world.state_.links.find(authored.id);
        if (entity != world.state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        if (runtime != world.state_.links.end() && (!runtime->second.traversable || runtime->second.locked)) continue;
        for (std::size_t left = 0; left < authored.link.endpoints.size(); ++left) {
            for (std::size_t right = left + 1; right < authored.link.endpoints.size(); ++right) {
                planner.addEdge(authored.link.endpoints[left], authored.link.endpoints[right], 1.0, true);
            }
        }
    }
    return planner.plan(from, to);
}

}  // namespace tide_marks
