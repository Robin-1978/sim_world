#include "world/meta_actions.hpp"

#include <map>

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
        "trade.execute",
        "exchange.create",
        "exchange.transition",
        "exchange.settle",
        "group.metric_adjust",
        "group.membership_adjust",
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

MetaActionRegistry::SecurityOperatorKind MetaActionRegistry::securityOperatorKind(
    const std::string& operation) {
    static const std::map<std::string, SecurityOperatorKind> operators{
        {"conflict.attack", SecurityOperatorKind::Conflict},
        {"conflict.sabotage", SecurityOperatorKind::Conflict},
        {"security.threaten", SecurityOperatorKind::Threaten},
        {"security.defend", SecurityOperatorKind::Defend},
        {"security.evacuate", SecurityOperatorKind::Evacuate},
        {"security.repair", SecurityOperatorKind::Repair},
    };
    const auto found = operators.find(operation);
    return found == operators.end() ? SecurityOperatorKind::Unknown : found->second;
}

}  // namespace tide_marks
