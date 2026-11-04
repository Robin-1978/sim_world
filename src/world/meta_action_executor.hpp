#pragma once

#include "content/content_pack.hpp"
#include "npc/behavior_recipe.hpp"
#include "world/meta_actions.hpp"

#include <functional>
#include <map>
#include <string>

namespace tide_marks {

// The executor contains the finite C++ implementations of registered
// operators. WorldService injects callbacks for higher-level commands so the
// executor can never open a second transaction or publish an event itself.
class MetaActionExecutor {
public:
    struct Callbacks {
        std::function<void(const std::string&, const std::string&, const std::string&, const std::string&, Json)> emit;
        std::function<bool(const Json&)> transfer;
        std::function<bool(const Json&)> move;
        std::function<bool(const Json&)> security;
        std::function<bool(const Json&)> knownFact;
        std::function<bool(const Json&)> experience;
        std::function<bool(const Json&)> promiseCreate;
        std::function<bool(const Json&)> promiseTransition;
        std::function<bool(const Json&)> trade;
        std::function<bool(const Json&)> exchangeCreate;
        std::function<bool(const Json&)> exchangeTransition;
        std::function<bool(const Json&)> exchangeSettle;
        std::function<bool(const Json&)> groupMetric;
        std::function<bool(const Json&)> groupMembership;
        std::function<void(const std::string&)> needRelief;
    };

    [[nodiscard]] static bool execute(const WorldContentPack& content, WorldState& state,
                                      const std::string& npcId, const std::string& actionId,
                                      const MetaActionDefinition& definition,
                                      const BehaviorNodeContent& node,
                                      const std::map<std::string, int>* reservedResources,
                                      const Json* parameters, const Callbacks& callbacks);
};

}  // namespace tide_marks
