#include "world/world_bootstrap.hpp"

#include "npc/drive_system.hpp"
#include "world/ids.hpp"

namespace tide_marks {

WorldState WorldBootstrap::createState(const WorldContentPack& content) {
    WorldState state;
    const auto* instance = content.instance ? &*content.instance : nullptr;
    state.worldId = instance != nullptr && !instance->worldId.empty() ? instance->worldId : content.worldId;
    state.instanceId = instance != nullptr ? instance->id : "default";
    state.contentVersion = content.contentVersion;
    state.now = instance != nullptr && instance->startTime ? *instance->startTime : content.startTime;

    for (const auto& [id, resource] : content.resources) {
        int initial = resource.initial;
        if (instance != nullptr) {
            if (const auto override = instance->resourceInitial.find(id); override != instance->resourceInitial.end()) {
                initial = override->second;
            }
        }
        state.resources[id] = ResourceState{initial, 0};
    }

    state.entities[kPlayerId] = WorldEntityState{kPlayerId, "actor", EntityLifecycle::Active, Json::object()};
    state.inventories.try_emplace(kPlayerId);
    for (const auto& spatial : content.spatialEntities) {
        state.entities[spatialEntityId(spatial.id)] =
            WorldEntityState{spatialEntityId(spatial.id), spatial.kind, EntityLifecycle::Active, Json::object()};
    }
    for (const auto& link : content.links) {
        state.entities[linkEntityId(link.id)] =
            WorldEntityState{linkEntityId(link.id), link.kind, EntityLifecycle::Active, Json::object()};
        state.links[link.id] = RuntimeLinkState{link.link.traversable, link.link.locked};
    }

    for (const auto& authored : content.npcs) {
        NpcState npc;
        npc.definitionId = authored.definition.id;
        npc.location = authored.initialLocation;
        npc.scheduleLabel = authored.initialScheduleLabel;
        npc.goalStage = authored.initialGoalStage;
        npc.strategy = authored.initialStrategy;
        npc.relationships[kPlayerId] = RelationshipAxes{};
        for (const auto& [peer, trust] : authored.initialPeerTrust) {
            npc.relationships[peer].trust = trust;
        }
        if (instance != nullptr) {
            if (const auto override = instance->npcs.find(authored.definition.id); override != instance->npcs.end()) {
                if (!override->second.location.empty()) npc.location = override->second.location;
                if (!override->second.scheduleLabel.empty()) npc.scheduleLabel = override->second.scheduleLabel;
                npc.goalStage = override->second.goalStage;
                if (!override->second.strategy.empty()) npc.strategy = override->second.strategy;
                for (const auto& [peer, trust] : override->second.peerTrust) {
                    npc.relationships[peer].trust = trust;
                }
            }
        }
        DriveSystem::initialize(npc, authored.needs, state.now);
        npc.lastDriveTick = state.now;
        npc.memories.push_back(Memory{"authored", "Initial content pack memory.", state.now, 1.0});
        state.npcs.emplace(authored.definition.id, std::move(npc));
        state.entities[npcEntityId(authored.definition.id)] =
            WorldEntityState{npcEntityId(authored.definition.id), "npc", EntityLifecycle::Active, Json::object()};
        state.inventories.try_emplace(npcEntityId(authored.definition.id));
    }

    for (const auto& authored : content.socialGroups) {
        SocialGroupState group;
        group.definitionId = authored.id;
        group.metrics = authored.metrics;
        group.members = authored.initialMembers;
        group.relationships = authored.initialRelationships;
        state.socialGroups.emplace(authored.id, std::move(group));
    }

    if (instance != nullptr) {
        for (const auto& [actor, resources] : instance->inventoryInitial) {
            const auto inventoryId = actor == kPlayerId ? kPlayerId : npcEntityId(actor);
            auto& inventory = state.inventories[inventoryId];
            for (const auto& [resource, amount] : resources) inventory[resource] = amount;
        }
    }
    return state;
}

}  // namespace tide_marks
