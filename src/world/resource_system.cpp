#include "world/resource_system.hpp"

#include "world/ids.hpp"

#include <algorithm>

namespace tide_marks {

int ResourceSystem::amount(const WorldState& state, const std::string& resource) {
    const auto found = state.resources.find(resource);
    return found == state.resources.end() ? 0 : found->second.amount;
}

int ResourceSystem::available(const WorldState& state, const std::string& resource) {
    const auto found = state.resources.find(resource);
    return found == state.resources.end() ? 0 : found->second.amount - found->second.reserved;
}

std::string ResourceSystem::resolveParty(const WorldState& state, const std::string& party) {
    if (party == "world" || party == kWorldActor) return kWorldActor;
    if (party == kPlayerId && state.entities.contains(kPlayerId)) return kPlayerId;
    if (state.entities.contains(party)) return party;
    if (state.npcs.contains(party)) return npcEntityId(party);
    return {};
}

bool ResourceSystem::activeParty(const WorldState& state, const std::string& party) {
    if (party == kWorldActor) return true;
    const auto found = state.entities.find(party);
    return found != state.entities.end() && found->second.lifecycle == EntityLifecycle::Active;
}

int ResourceSystem::balance(const WorldState& state, const std::string& party,
                            const std::string& resource) {
    if (party == kWorldActor) return available(state, resource);
    const auto inventory = state.inventories.find(party);
    if (inventory == state.inventories.end()) return 0;
    const auto quantity = inventory->second.find(resource);
    return quantity == inventory->second.end() ? 0 : quantity->second;
}

ResourceSystem::TransferResult ResourceSystem::transfer(WorldState& state,
                                                        const WorldContentPack& content,
                                                        const std::string& source,
                                                        const std::string& target,
                                                        const std::string& resource,
                                                        int quantity) {
    TransferResult result;
    if (quantity <= 0 || !content.resources.contains(resource)) {
        result.code = "invalid_resource_transfer";
        result.message = "A transfer needs a known resource and a positive amount.";
        return result;
    }
    result.sourceId = resolveParty(state, source);
    result.targetId = resolveParty(state, target);
    if (result.sourceId.empty() || result.targetId.empty()) {
        result.code = "unknown_transfer_party";
        result.message = "Both transfer parties must be active entities or the world pool.";
        return result;
    }
    if (result.sourceId == result.targetId) {
        result.code = "invalid_resource_transfer";
        result.message = "A resource cannot be transferred to itself.";
        return result;
    }
    if (!activeParty(state, result.sourceId) || !activeParty(state, result.targetId)) {
        result.code = "inactive_transfer_party";
        result.message = "A transfer party is not active.";
        return result;
    }
    result.sourceBefore = balance(state, result.sourceId, resource);
    result.targetBefore = balance(state, result.targetId, resource);
    if (result.sourceBefore < quantity) {
        result.code = "insufficient_resource";
        result.message = "The source does not have enough unreserved resource.";
        return result;
    }
    if (result.targetId == kWorldActor && amount(state, resource) + quantity > content.resources.at(resource).maximum) {
        result.code = "resource_capacity";
        result.message = "The destination resource pool has no capacity.";
        return result;
    }
    if (result.sourceId == kWorldActor) state.resources[resource].amount -= quantity;
    else state.inventories[result.sourceId][resource] -= quantity;
    if (result.targetId == kWorldActor) state.resources[resource].amount += quantity;
    else state.inventories[result.targetId][resource] += quantity;
    result.sourceAfter = balance(state, result.sourceId, resource);
    result.targetAfter = balance(state, result.targetId, resource);
    result.accepted = true;
    result.code = "ok";
    result.message = resource;
    return result;
}

bool ResourceSystem::consume(WorldState& state, const WorldContentPack& content,
                              const std::string& resource, int quantity, bool reserved) {
    if (quantity <= 0 || !content.resources.contains(resource)) return false;
    if (reserved) {
        const auto found = state.resources.find(resource);
        if (found == state.resources.end() || found->second.reserved < quantity) return false;
    } else if (available(state, resource) < quantity) {
        return false;
    }
    auto& value = state.resources[resource];
    value.amount -= quantity;
    if (reserved) value.reserved -= quantity;
    return true;
}

bool ResourceSystem::produce(WorldState& state, const WorldContentPack& content,
                              const std::string& resource, int quantity) {
    if (quantity <= 0) return false;
    const auto definition = content.resources.find(resource);
    if (definition == content.resources.end() || amount(state, resource) + quantity > definition->second.maximum) return false;
    state.resources[resource].amount += quantity;
    return true;
}

bool ResourceSystem::reserve(WorldState& state, const WorldContentPack& content,
                              const std::string& resource, int quantity) {
    if (quantity <= 0 || !content.resources.contains(resource) || available(state, resource) < quantity) return false;
    state.resources[resource].reserved += quantity;
    return true;
}

bool ResourceSystem::release(WorldState& state, const WorldContentPack& content,
                              const std::string& resource, int quantity) {
    if (quantity <= 0 || !content.resources.contains(resource)) return false;
    const auto found = state.resources.find(resource);
    if (found == state.resources.end()) return false;
    found->second.reserved = std::max(0, found->second.reserved - quantity);
    return true;
}

}  // namespace tide_marks
