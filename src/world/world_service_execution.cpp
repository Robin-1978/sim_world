#include "world/world_service.hpp"

#include "world/behavior_execution_system.hpp"
#include "world/cognition_system.hpp"
#include "evolution/evolution.hpp"
#include "world/resource_system.hpp"
#include "world/security_system.hpp"
#include "world/stimulus_system.hpp"
#include "world/world_command_system.hpp"
#include "world/world_evolution_system.hpp"

#include <algorithm>

namespace tide_marks {

CommandResult WorldService::activateOverlay(const EvolutionProposal& proposal,
                                            const EvolutionSandboxResult& sandbox) {
    return WorldEvolutionSystem(*this).activateOverlay(proposal, sandbox);
}

CommandResult WorldService::rollbackOverlay(const std::string& proposalId) {
    return WorldEvolutionSystem(*this).rollbackOverlay(proposalId);
}

CommandResult WorldService::transitionInnovationHypothesis(const std::string& npcId,
                                                           const std::string& proposalId,
                                                           const std::string& nextStatus,
                                                           const std::string& actor) {
    return WorldEvolutionSystem(*this).transitionInnovationHypothesis(npcId, proposalId, nextStatus, actor);
}

CommandResult WorldService::startInnovationAdoption(const std::string& npcId, const std::string& proposalId) {
    return WorldEvolutionSystem(*this).startAdoption(npcId, proposalId);
}

CommandResult WorldService::transitionInnovationAdoption(const std::string& npcId,
                                                         const std::string& proposalId,
                                                         InnovationAdoptionStatus status,
                                                         const std::string& reason) {
    return WorldEvolutionSystem(*this).transitionAdoption(npcId, proposalId, status, reason);
}

CommandResult WorldService::assertKnownFact(const std::string& npcId, const std::string& subject,
                                            const std::string& predicate, const std::string& value,
                                            std::uint64_t sourceSequence, const std::string& scope,
                                            double confidence, const std::string& actor,
                                            const std::string& turnId) {
    return CognitionSystem(*this).assertKnownFact(npcId, subject, predicate, value, sourceSequence,
                                                   scope, confidence, actor, turnId);
}

CommandResult WorldService::recordExperience(const std::string& npcId, const std::string& domain,
                                             const std::string& trigger, const std::string& action,
                                             const std::string& outcome, std::uint64_t sourceSequence,
                                             double confidence, bool personal, const std::string& actor,
                                             const std::string& turnId) {
    return CognitionSystem(*this).recordExperience(npcId, domain, trigger, action, outcome, sourceSequence,
                                                   confidence, personal, actor, turnId);
}

CommandResult WorldService::applySecurityOperator(const std::string& operation, const std::string& actor,
                                                  const std::string& target, const Json& args,
                                                  const std::string& turnId) {
    return SecuritySystem(*this).applyOperator(operation, actor, target, args, turnId);
}

std::optional<std::vector<std::string>> WorldService::planRoute(const std::string& from,
                                                                const std::string& to) const {
    return WorldCommandSystem::planRoute(*this, from, to);
}

int WorldService::resourceValue(const std::string& resource) const {
    return ResourceSystem::amount(state_, resource);
}

int WorldService::availableResourceValue(const std::string& resource) const {
    return ResourceSystem::available(state_, resource);
}

void WorldService::setResource(const std::string& resource, int value) {
    const auto definition = content_.resources.find(resource);
    if (definition == content_.resources.end()) return;
    auto& state = state_.resources[resource];
    state.amount = std::clamp(value, definition->second.minimum, definition->second.maximum);
    state.reserved = std::min(state.reserved, state.amount);
}

const NpcContent* WorldService::authoredNpc(const std::string& npcId) const {
    const auto found = std::find_if(content_.npcs.begin(), content_.npcs.end(),
                                    [&npcId](const auto& item) { return item.definition.id == npcId; });
    return found == content_.npcs.end() ? nullptr : &*found;
}

const BehaviorRecipeContent* WorldService::recipeForAction(const std::string& actionId) {
    return BehaviorExecutionSystem(*this).recipeForAction(actionId);
}

bool WorldService::evaluateBehaviorCondition(const std::string& npcId,
                                             const BehaviorNodeContent& node) {
    return BehaviorExecutionSystem(*this).evaluateBehaviorCondition(npcId, node);
}

bool WorldService::applyMetaAction(const std::string& npcId, const std::string& actionId,
                                   const BehaviorNodeContent& node,
                                   const std::map<std::string, int>* reservedResources,
                                   const Json* parameters) {
    return BehaviorExecutionSystem(*this).applyMetaAction(npcId, actionId, node, reservedResources, parameters);
}

bool WorldService::applyActionEffects(const std::string& npcId, const std::string& actionId) {
    return BehaviorExecutionSystem(*this).applyActionEffects(npcId, actionId);
}

bool WorldService::applyActionEffects(const std::string& npcId, const std::string& actionId,
                                      const std::map<std::string, int>* reservedResources,
                                      const Json* parameters) {
    return BehaviorExecutionSystem(*this).applyActionEffects(npcId, actionId, reservedResources, parameters);
}

void WorldService::applyNeedRelief(const std::string& npcId, const std::string& actionId) {
    BehaviorExecutionSystem(*this).applyNeedRelief(npcId, actionId);
}

WorldEvent WorldService::addEvent(std::string type, std::string actor, std::string target,
                                  std::string detail, Json data) {
    return eventJournal_.append(state_, transactionManager_, eventBus_, content_.contentVersion,
                                std::move(type), std::move(actor), std::move(target), std::move(detail),
                                std::move(data), [this](const WorldEvent& event) { StimulusSystem(*this).project(event); });
}

}  // namespace tide_marks
