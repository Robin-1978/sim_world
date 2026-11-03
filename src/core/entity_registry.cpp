#include "core/entity_registry.hpp"

#include <utility>

namespace tide_marks {

bool EntityRegistry::create(EntityRecord record) {
    if (record.id.empty() || records_.contains(record.id)) return false;
    components_.try_emplace(record.id);
    records_.emplace(record.id, std::move(record));
    return true;
}

bool EntityRegistry::destroy(const EntityId& id) {
    if (records_.erase(id) == 0) return false;
    components_.erase(id);
    return true;
}

bool EntityRegistry::setActive(const EntityId& id, bool active) {
    const auto found = records_.find(id);
    if (found == records_.end()) return false;
    found->second.active = active;
    return true;
}

bool EntityRegistry::contains(const EntityId& id) const { return records_.contains(id); }

bool EntityRegistry::isActive(const EntityId& id) const {
    const auto found = records_.find(id);
    return found != records_.end() && found->second.active;
}

std::optional<EntityRecord> EntityRegistry::record(const EntityId& id) const {
    const auto found = records_.find(id);
    return found == records_.end() ? std::nullopt : std::optional<EntityRecord>(found->second);
}

std::vector<EntityRecord> EntityRegistry::all() const {
    std::vector<EntityRecord> result;
    for (const auto& [id, record] : records_) {
        (void)id;
        result.push_back(record);
    }
    return result;
}

std::size_t EntityRegistry::componentCount(const EntityId& id) const {
    const auto found = components_.find(id);
    return found == components_.end() ? 0 : found->second.size();
}

}  // namespace tide_marks
