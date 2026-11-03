#include "world/meta_actions.hpp"

#include <algorithm>

namespace tide_marks {

const std::vector<MetaActionSpec>& MetaActionRegistry::all() {
    static const std::vector<MetaActionSpec> specs{
        {"consume_resource", "resource", "Decrease a resource after checking unit, ownership and availability.", true},
        {"produce_resource", "resource", "Increase a resource within its declared capacity.", true},
        {"transfer_resource", "resource", "Move a resource between entity inventories.", false},
        {"reserve_resource", "resource", "Reserve a resource for a pending plan.", true},
        {"release_resource", "resource", "Release a previous reservation.", true},
        {"relieve_need", "need", "Apply a committed action's authored need relief.", false},
        {"adjust_relationship", "relationship", "Adjust one relationship axis with a recorded reason.", false},
        {"move_entity", "movement", "Create a validated movement intent.", false},
        {"arrive_entity", "movement", "Commit an arrival result.", false},
        {"create_promise", "promise", "Create a promise with a due time and parties.", false},
        {"transition_promise", "promise", "Transition a promise through its allowed states.", false},
        {"append_memory", "knowledge", "Append a sourced memory projection.", false},
        {"assert_known_fact", "knowledge", "Record a fact within an observation scope.", false},
        {"set_link_state", "topology", "Change authored link traversal state.", false},
        {"create_entity", "entity", "Create an entity from a registered archetype.", false},
        {"set_entity_lifecycle", "entity", "Change active, dormant, or destroyed lifecycle.", false},
    };
    return specs;
}

bool MetaActionRegistry::contains(const std::string& id) {
    const auto& specs = all();
    return std::any_of(specs.begin(), specs.end(), [&id](const auto& spec) { return spec.id == id; });
}

bool MetaActionRegistry::isImplemented(const std::string& id) {
    const auto& specs = all();
    return std::any_of(specs.begin(), specs.end(), [&id](const auto& spec) {
        return spec.id == id && spec.implemented;
    });
}

}  // namespace tide_marks
