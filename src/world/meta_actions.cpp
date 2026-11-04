#include "world/meta_actions.hpp"

namespace tide_marks {

const std::set<std::string>& MetaActionRegistry::executableOperators() {
    static const std::set<std::string> operators{
        "resource.consume",
        "resource.produce",
        "resource.reserve",
        "resource.release",
        "resource.transfer",
        "need.relieve",
        "link.set_state",
        "relationship.adjust",
        "movement.traverse",
        "movement.arrive",
        "conflict.attack",
        "conflict.sabotage",
        "security.threaten",
        "security.defend",
        "security.evacuate",
        "security.repair",
        "memory.append",
        "knowledge.assert",
        "experience.record",
        "promise.create",
        "promise.transition",
        "production.execute",
    };
    return operators;
}

bool MetaActionRegistry::isExecutableOperator(const std::string& operation) {
    return executableOperators().contains(operation);
}

bool MetaActionRegistry::isExecutableConditionOperator(const std::string& operation) {
    static const std::set<std::string> operators{
        "always", "resource.available", "resource.below", "link.traversable",
        "link.locked", "need.critical", "npc.at", "time.between",
    };
    return operators.contains(operation);
}

}  // namespace tide_marks
