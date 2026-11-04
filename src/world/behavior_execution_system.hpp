#pragma once

#include "content/content_pack.hpp"
#include "core/types.hpp"
#include "npc/behavior_tree.hpp"

#include <map>
#include <string>

namespace tide_marks {

class WorldService;

// Compiles and executes content-authored behavior recipes.  It owns no world
// state and never publishes directly; all cross-domain effects go through the
// WorldService command/event boundary.
class BehaviorExecutionSystem {
public:
    explicit BehaviorExecutionSystem(WorldService& world) : world_(world) {}

    [[nodiscard]] const BehaviorRecipeContent* recipeForAction(const std::string& actionId) const;
    [[nodiscard]] bool evaluateBehaviorCondition(const std::string& npcId,
                                                 const BehaviorNodeContent& node) const;
    [[nodiscard]] bool applyMetaAction(const std::string& npcId, const std::string& actionId,
                                       const BehaviorNodeContent& node,
                                       const std::map<std::string, int>* reservedResources,
                                       const Json* parameters = nullptr);
    [[nodiscard]] bool applyActionEffects(const std::string& npcId, const std::string& actionId);
    [[nodiscard]] bool applyActionEffects(const std::string& npcId, const std::string& actionId,
                                          const std::map<std::string, int>* reservedResources,
                                          const Json* parameters = nullptr);
    void applyNeedRelief(const std::string& npcId, const std::string& actionId);

private:
    friend class WorldService;
    WorldService& world_;
};

}  // namespace tide_marks
