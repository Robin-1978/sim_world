#pragma once

#include <any>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace tide_marks {

using EntityId = std::string;

struct EntityRecord {
    EntityId id;
    std::string kind;
    bool active{true};
};

// The registry knows only entity identity and type-erased component storage.
// A feature adds a component type without editing this class.
class EntityRegistry {
public:
    bool create(EntityRecord record);
    bool destroy(const EntityId& id);
    bool setActive(const EntityId& id, bool active);
    [[nodiscard]] bool contains(const EntityId& id) const;
    [[nodiscard]] bool isActive(const EntityId& id) const;
    [[nodiscard]] std::optional<EntityRecord> record(const EntityId& id) const;
    [[nodiscard]] std::vector<EntityRecord> all() const;
    [[nodiscard]] std::size_t componentCount(const EntityId& id) const;

    template <typename Component>
    bool set(const EntityId& id, Component component) {
        if (!contains(id)) return false;
        components_[id][std::type_index(typeid(Component))] = std::move(component);
        return true;
    }

    template <typename Component>
    [[nodiscard]] const Component* get(const EntityId& id) const {
        const auto entity = components_.find(id);
        if (entity == components_.end()) return nullptr;
        const auto component = entity->second.find(std::type_index(typeid(Component)));
        if (component == entity->second.end()) return nullptr;
        return std::any_cast<Component>(&component->second);
    }

    template <typename Component>
    bool remove(const EntityId& id) {
        const auto entity = components_.find(id);
        if (entity == components_.end()) return false;
        return entity->second.erase(std::type_index(typeid(Component))) != 0;
    }

private:
    std::map<EntityId, EntityRecord> records_;
    std::map<EntityId, std::map<std::type_index, std::any>> components_;
};

}  // namespace tide_marks
