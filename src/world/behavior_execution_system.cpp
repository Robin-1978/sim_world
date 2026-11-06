#include "world/behavior_execution_system.hpp"

#include "world/world_service.hpp"
#include "world/meta_action_executor.hpp"
#include "world/resource_system.hpp"
#include "world/social_system.hpp"
#include "npc/drive_system.hpp"

#include <algorithm>

namespace tide_marks {

const BehaviorRecipeContent* BehaviorExecutionSystem::recipeForAction(const std::string& actionId) const {
    const auto found = std::find_if(world_.content_.behaviorRecipes.begin(), world_.content_.behaviorRecipes.end(),
                                    [&actionId](const auto& recipe) { return recipe.id == actionId; });
    return found == world_.content_.behaviorRecipes.end() ? nullptr : &*found;
}

bool BehaviorExecutionSystem::evaluateBehaviorCondition(const std::string& npcId,
                                             const BehaviorNodeContent& node) const {
    const auto definition = world_.content_.conditions.find(node.name);
    if (definition == world_.content_.conditions.end()) return false;
    Json args = definition->second.args;
    for (auto it = node.args.begin(); it != node.args.end(); ++it) args[it.key()] = it.value();
    const auto& operation = definition->second.operation;
    if (operation == "always") return true;
    if (operation == "resource.available") {
        return world_.availableResourceValue(args.value("resource", "")) >= args.value("minimum", 1);
    }
    if (operation == "resource.below") {
        return world_.resourceValue(args.value("resource", "")) < args.value("below", 1);
    }
    if (operation == "link.traversable" || operation == "link.locked") {
        const auto link = world_.state_.links.find(args.value("link", ""));
        const bool traversable = link == world_.state_.links.end() ||
                                 (link->second.traversable && !link->second.locked);
        return operation == "link.traversable" ? traversable : !traversable;
    }
    if (operation == "need.critical") {
        const auto npc = world_.state_.npcs.find(npcId);
        const auto* authored = world_.authoredNpc(npcId);
        if (npc == world_.state_.npcs.end() || authored == nullptr) return false;
        const auto needId = args.value("need", "");
        const auto need = std::find_if(authored->needs.begin(), authored->needs.end(),
                                       [&needId](const auto& item) { return item.id == needId; });
        const auto stateNeed = npc->second.needs.find(needId);
        return need != authored->needs.end() && stateNeed != npc->second.needs.end() &&
               stateNeed->second.deficit >= need->criticalThreshold;
    }
    if (operation == "npc.at") {
        const auto npc = world_.state_.npcs.find(npcId);
        return npc != world_.state_.npcs.end() && npc->second.location == args.value("location", "");
    }
    if (operation == "time.between") {
        const int minuteOfDay = world_.state_.now.hour() * 60 + world_.state_.now.minuteOfHour();
        return minuteOfDay >= args.value("from_minute", 0) && minuteOfDay < args.value("to_minute", 0);
    }
    return false;
}

bool BehaviorExecutionSystem::applyMetaAction(const std::string& npcId, const std::string& actionId,
                                   const BehaviorNodeContent& node,
                                   const std::map<std::string, int>* reservedResources,
                                   const Json* parameters) {
    const auto definition = world_.content_.metaActions.find(node.name);
    if (definition == world_.content_.metaActions.end()) return false;

    MetaActionExecutor::Callbacks callbacks;
    callbacks.emit = [this](const std::string& type, const std::string& actor,
                            const std::string& target, const std::string& detail, Json data) {
        world_.addEvent(type, actor, target, detail, std::move(data));
    };
    callbacks.transfer = [this, &npcId, &actionId](const Json& args) {
        return world_.transferResource(args.value("source", npcId), args.value("target", kWorldActor),
                                args.value("resource", ""), args.value("amount", 0), npcId,
                                "recipe:" + npcId + ":" + actionId).accepted;
    };
    callbacks.move = [this, &npcId, &actionId](const Json& args) {
        return world_.moveNpc(npcId, args.value("destination", ""), npcId,
                       "recipe:" + npcId + ":" + actionId + ":movement").accepted;
    };
    callbacks.security = [this, &npcId, &actionId](const Json& args) {
        return world_.applySecurityOperator(args.value("operation", ""), npcId,
                                     args.value("target", "self"), args.value("args", Json::object()),
                                     "recipe:" + npcId + ":" + actionId + ":security").accepted;
    };
    callbacks.knownFact = [this, &npcId, &actionId](const Json& args) {
        return world_.assertKnownFact(npcId, args.value("subject", ""), args.value("predicate", ""),
                               args.value("value", ""), args.value("source_sequence", 0ULL),
                               args.value("scope", "direct"), args.value("confidence", 0.0), npcId,
                               "recipe:" + npcId + ":" + actionId + ":knowledge").accepted;
    };
    callbacks.experience = [this, &npcId, &actionId](const Json& args) {
        return world_.recordExperience(npcId, args.value("domain", ""), args.value("trigger", ""),
                                args.value("action", actionId), args.value("outcome", ""),
                                args.value("source_sequence", 0ULL), args.value("confidence", 0.0),
                                args.value("personal", true), npcId,
                                "recipe:" + npcId + ":" + actionId + ":experience").accepted;
    };
    callbacks.promiseCreate = [this, &npcId, &actionId](const Json& args) {
        return world_.createPromise(args.value("from", npcId), args.value("to", ""),
                             args.value("description", ""), args.value("due_days", 1),
                             "recipe:" + npcId + ":" + actionId + ":promise").accepted;
    };
    callbacks.promiseTransition = [this, &npcId, &actionId](const Json& args) {
        return world_.decidePromise(args.value("promise_id", ""),
                             promiseStatusFromString(args.value("status", "proposed")), npcId,
                             "recipe:" + npcId + ":" + actionId + ":promise",
                             args.value("detail", "")).accepted;
    };
    callbacks.trade = [this, &npcId, &actionId](const Json& args) {
        return world_.tradeResources(args.value("seller", npcId), args.value("buyer", ""),
                              args.value("seller_resource", ""), args.value("seller_amount", 0),
                              args.value("buyer_resource", ""), args.value("buyer_amount", 0), npcId,
                              "recipe:" + npcId + ":" + actionId).accepted;
    };
    callbacks.exchangeCreate = [this, &npcId, &actionId](const Json& args) {
        return world_.proposeExchange(args.value("proposer", npcId), args.value("counterparty", ""),
                                args.value("offered", std::map<std::string, int>{}),
                                args.value("requested", std::map<std::string, int>{}),
                                args.value("due_minutes", 0), npcId,
                                "recipe:" + npcId + ":" + actionId + ":exchange").accepted;
    };
    callbacks.exchangeTransition = [this, &npcId, &actionId](const Json& args) {
        return world_.decideExchange(args.value("contract_id", ""),
                              exchangeContractStatusFromString(args.value("status", "")), npcId,
                              "recipe:" + npcId + ":" + actionId + ":exchange",
                              args.value("reason", "")).accepted;
    };
    callbacks.exchangeSettle = [this, &npcId, &actionId](const Json& args) {
        return world_.settleExchange(args.value("contract_id", ""), npcId,
                              "recipe:" + npcId + ":" + actionId + ":exchange").accepted;
    };
    callbacks.groupMetric = [this, &npcId, &actionId](const Json& args) {
        return world_.adjustGroupMetric(args.value("group", ""), args.value("metric", ""),
                                 args.value("delta", 0.0), npcId,
                                 "recipe:" + npcId + ":" + actionId,
                                 args.value("reason", "")).accepted;
    };
    callbacks.groupMembership = [this, &npcId, &actionId](const Json& args) {
        return world_.setGroupMembership(args.value("group", ""), args.value("npc", npcId),
                                  args.value("role", "member"), args.value("active", true), npcId,
                                  "recipe:" + npcId + ":" + actionId).accepted;
    };
    callbacks.needRelief = [this, &npcId](const std::string& sourceAction) {
        applyNeedRelief(npcId, sourceAction);
    };
    return MetaActionExecutor::execute(world_.content_, world_.state_, npcId, actionId, definition->second, node,
                                       reservedResources, parameters, callbacks);
}

bool BehaviorExecutionSystem::applyActionEffects(const std::string& npcId, const std::string& actionId) {
    return applyActionEffects(npcId, actionId, nullptr, nullptr);
}

bool BehaviorExecutionSystem::applyActionEffects(const std::string& npcId, const std::string& actionId,
                                      const std::map<std::string, int>* reservedResources,
                                      const Json* parameters) {
    const auto* recipe = recipeForAction(actionId);
    if (recipe == nullptr) return false;
    // This is the atomic fallback used for immediate actions and recipes with
    // dynamic branch state. Linear long-running recipes are advanced by
    // applyBehaviorLeaf so already committed stages are never replayed.
    WorldService::TransactionScope recipeTransaction(world_);
    const auto result = BehaviorRecipeRuntime::run(
        *recipe,
        [this, &npcId](const BehaviorNodeContent& node) {
            return world_.evaluateBehaviorCondition(npcId, node);
        },
        [this, &npcId, &actionId, reservedResources, parameters](const BehaviorNodeContent& node) {
            return world_.applyMetaAction(npcId, actionId, node, reservedResources, parameters);
        });
    if (result.status != NodeStatus::Success) {
        recipeTransaction.rollback();
        return false;
    }
    recordActionExperience(npcId, actionId);
    return result.status == NodeStatus::Success;
}

bool BehaviorExecutionSystem::applyBehaviorLeaf(const std::string& npcId,
                                                const std::string& actionId,
                                                const BehaviorNodeContent& node,
                                                const std::map<std::string, int>* reservedResources,
                                                const Json* parameters) {
    WorldService::TransactionScope leafTransaction(world_);
    bool applied = false;
    if (node.type == "condition") {
        applied = evaluateBehaviorCondition(npcId, node);
    } else if (node.type == "meta_action") {
        applied = applyMetaAction(npcId, actionId, node, reservedResources, parameters);
    }
    if (!applied) leafTransaction.rollback();
    return applied;
}

void BehaviorExecutionSystem::recordActionExperience(const std::string& npcId,
                                                     const std::string& actionId) {
    const auto effectEvent = std::find_if(world_.state_.events.rbegin(), world_.state_.events.rend(),
                                          [&npcId](const auto& event) {
                                              return event.actor == npcId && event.type != "experience.observed" &&
                                                     event.type != "experience.recorded";
                                          });
    if (effectEvent == world_.state_.events.rend()) return;
    const auto sourceSequence = effectEvent->sequence;
    const auto sourceType = effectEvent->type;
    const auto recorded = world_.recordExperience(
        npcId, "action", "observed_context", actionId, sourceType, sourceSequence, 0.5, true, npcId,
        "recipe:" + npcId + ":" + actionId + ":experience:" + std::to_string(sourceSequence));
    if (recorded.accepted) {
        world_.addEvent("experience.observed", npcId, npcId,
                        "A committed action outcome became reusable NPC experience",
                        Json{{"action_id", actionId}, {"source_sequence", sourceSequence},
                             {"source_type", sourceType}, {"domain", "action"},
                             {"recorded_experience", recorded.message}});
    }
}

void BehaviorExecutionSystem::applyNeedRelief(const std::string& npcId, const std::string& actionId) {
    const auto* authored = world_.authoredNpc(npcId);
    const auto npc = world_.state_.npcs.find(npcId);
    if (authored == nullptr || npc == world_.state_.npcs.end()) return;
    const auto changes = DriveSystem::relieve(npc->second, authored->needs, actionId, world_.state_.now,
                                              world_.content_.driveAdaptation);
    for (const auto& change : changes) {
        npc->second.needs.at(change.needId).lastReliefRevision = world_.state_.revision + 1;
        world_.addEvent("need.relieved", npcId, actionId, "A committed action relieved a need",
                 Json{{"need_id", change.needId}, {"action_id", actionId},
                      {"before", change.before}, {"after", change.after},
                      {"expected_relief", change.expectedRelief}, {"actual_relief", change.actualRelief},
                      {"rule", "drive.committed_action"}, {"source_revision", world_.state_.revision + 1}});
        if (change.adapted) {
            world_.addEvent("drive.adapted", npcId, change.needId, "Predicted need relief was calibrated from its outcome",
                     Json{{"need_id", change.needId}, {"action_id", actionId},
                          {"expectation_before", change.expectationBefore},
                          {"expectation_after", change.expectationAfter},
                          {"actual_relief", change.actualRelief},
                          {"adaptation_version", npc->second.needs.at(change.needId).adaptationVersion},
                          {"rule", "drive.bounded_expectation_adaptation"}});
        }
    }
}

}  // namespace tide_marks
