#include "world/entity_projection_system.hpp"

#include "world/world_service.hpp"

namespace tide_marks {

void EntityProjectionSystem::rebuild() {
    world_.entities_ = EntityRegistry{};
    const auto playerState = world_.state_.entities.find(kPlayerId);
    if (playerState == world_.state_.entities.end() || playerState->second.lifecycle != EntityLifecycle::Destroyed) {
        world_.entities_.create(EntityRecord{kPlayerId, kPlayerId, playerState == world_.state_.entities.end() ||
                                                   playerState->second.lifecycle == EntityLifecycle::Active});
    }
    if (world_.entities_.contains(kPlayerId)) world_.entities_.set<TransformComponent>(kPlayerId, TransformComponent{0.0, 0.0, 0.0, 0.0});
    const auto defaultLocation = world_.content_.spatialEntities.empty() ? std::string{} : world_.content_.spatialEntities.front().id;
    const auto regionId = world_.state_.worldId.empty() ? world_.content_.worldId : world_.state_.worldId;
    if (world_.entities_.contains(kPlayerId)) {
        world_.entities_.set<LocationComponent>(kPlayerId, LocationComponent{defaultLocation, regionId});
        InventoryComponent inventory;
        if (const auto found = world_.state_.inventories.find(kPlayerId); found != world_.state_.inventories.end()) {
            inventory.quantities = found->second;
        }
        world_.entities_.set<InventoryComponent>(kPlayerId, std::move(inventory));
    }
    for (const auto& authored : world_.content_.spatialEntities) {
        const auto entity = spatialEntityId(authored.id);
        const auto stateEntity = world_.state_.entities.find(entity);
        if (stateEntity != world_.state_.entities.end() && stateEntity->second.lifecycle == EntityLifecycle::Destroyed) continue;
        const bool active = stateEntity == world_.state_.entities.end() || stateEntity->second.lifecycle == EntityLifecycle::Active;
        if (!world_.entities_.create(EntityRecord{entity, authored.kind, active})) continue;
        world_.entities_.set<SpatialComponent>(entity, authored.spatial);
        world_.entities_.set<LocationComponent>(entity, LocationComponent{authored.id, regionId});
    }
    for (const auto& authored : world_.content_.links) {
        const auto entity = linkEntityId(authored.id);
        const auto stateEntity = world_.state_.entities.find(entity);
        if (stateEntity != world_.state_.entities.end() && stateEntity->second.lifecycle == EntityLifecycle::Destroyed) continue;
        const bool active = stateEntity == world_.state_.entities.end() || stateEntity->second.lifecycle == EntityLifecycle::Active;
        if (!world_.entities_.create(EntityRecord{entity, authored.kind, active})) continue;
        auto link = authored.link;
        if (const auto runtime = world_.state_.links.find(authored.id); runtime != world_.state_.links.end()) {
            link.traversable = runtime->second.traversable;
            link.locked = runtime->second.locked;
        }
        world_.entities_.set<LinkComponent>(entity, std::move(link));
    }
    for (const auto& [id, npc] : world_.state_.npcs) {
        const auto entity = npcEntityId(id);
        const auto stateEntity = world_.state_.entities.find(entity);
        if (stateEntity != world_.state_.entities.end() && stateEntity->second.lifecycle == EntityLifecycle::Destroyed) continue;
        const bool active = stateEntity == world_.state_.entities.end() || stateEntity->second.lifecycle == EntityLifecycle::Active;
        world_.entities_.create(EntityRecord{entity, "npc", active});
        world_.entities_.set<NpcComponent>(entity, NpcComponent{id, npc.mode});
        world_.entities_.set<LocationComponent>(entity, LocationComponent{npc.location, world_.content_.worldId});
        world_.entities_.set<RenderableComponent>(entity, RenderableComponent{"npc." + id, true});
        InventoryComponent inventory;
        if (const auto found = world_.state_.inventories.find(entity); found != world_.state_.inventories.end()) {
            inventory.quantities = found->second;
        }
        world_.entities_.set<InventoryComponent>(entity, std::move(inventory));
    }
    for (const auto& [id, runtimeEntity] : world_.state_.entities) {
        if (runtimeEntity.lifecycle == EntityLifecycle::Destroyed || world_.entities_.contains(id)) continue;
        if (!world_.entities_.create(EntityRecord{id, runtimeEntity.kind, runtimeEntity.lifecycle == EntityLifecycle::Active})) continue;
    }
}

void EntityProjectionSystem::syncNpc() {
    for (const auto& [id, npc] : world_.state_.npcs) {
        const auto entity = npcEntityId(id);
        world_.entities_.set<NpcComponent>(entity, NpcComponent{id, npc.mode});
        world_.entities_.set<LocationComponent>(entity, LocationComponent{npc.location, world_.state_.worldId});
    }
}


}  // namespace tide_marks
