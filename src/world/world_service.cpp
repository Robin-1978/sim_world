#include "world/world_service.hpp"
#include "world/contract_system.hpp"
#include "world/npc_simulation_system.hpp"
#include "world/behavior_execution_system.hpp"
#include "world/command_dispatcher.hpp"
#include "world/world_intelligence_system.hpp"
#include "world/world_evolution_system.hpp"
#include "world/security_system.hpp"
#include "world/cognition_system.hpp"
#include "world/stimulus_system.hpp"
#include "world/entity_projection_system.hpp"
#include "world/simulation_coordinator.hpp"
#include "world/schedule_system.hpp"
#include "world/world_command_system.hpp"
#include "world/resource_system.hpp"
#include "world/social_system.hpp"
#include "world/meta_action_executor.hpp"
#include "world/world_state_json.hpp"
#include "npc/drive_system.hpp"
#include "world/ids.hpp"
#include "world/meta_actions.hpp"
#include "provider/provider_profile.hpp"
#include "provider/decision_context_projector.hpp"
#include "evolution/evolution.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <exception>
#include <limits>
#include <set>
#include <thread>

namespace tide_marks {

WorldService::TransactionScope::TransactionScope(WorldService& service)
    : service_(service), uncaughtAtConstruction_(std::uncaught_exceptions()) {
    service_.beginTransaction();
}

WorldService::TransactionScope::~TransactionScope() {
    if (!active_) return;
    if (std::uncaught_exceptions() > uncaughtAtConstruction_) service_.rollbackTransaction();
    else service_.commitTransaction();
}

void WorldService::TransactionScope::rollback() {
    if (!active_) return;
    active_ = false;
    service_.rollbackTransaction();
}

void WorldService::TransactionScope::markChanged() { service_.markTransactionChanged(); }

void WorldService::beginTransaction() {
    transactionManager_.begin(state_);
}

void WorldService::commitTransaction() {
    transactionManager_.commit(state_, eventBus_, [this] { rebuildEntityRegistry(); });
}

void WorldService::markTransactionChanged() {
    transactionManager_.markChanged(state_);
}

void WorldService::rollbackTransaction() {
    transactionManager_.rollback(state_, [this] { rebuildEntityRegistry(); });
}

WorldService::WorldService(std::unique_ptr<IProvider> provider,
                           std::unique_ptr<IWorldPersistence> persistence)
    : provider_(provider ? std::move(provider) : makeDefaultRuntimeProvider()),
      intelligence_(std::make_unique<IntelligenceCoordinator>(*provider_)),
      evolution_(std::make_unique<EvolutionQueue>(*provider_)),
      persistence_(persistence ? std::move(persistence) : std::make_unique<JsonFilePersistence>()) {}

WorldService WorldService::load(std::string contentRoot, std::unique_ptr<IProvider> provider) {
    return fromContent(ContentPackLoader::load(contentRoot), std::move(provider));
}

WorldService WorldService::fromContent(const WorldContentPack& content,
                                       std::unique_ptr<IProvider> provider) {
    WorldService service(std::move(provider));
    service.baseContent_ = content;
    service.content_ = content;
    const auto* instance = content.instance ? &*content.instance : nullptr;
    service.state_.worldId = instance != nullptr && !instance->worldId.empty() ? instance->worldId : content.worldId;
    service.state_.instanceId = instance != nullptr ? instance->id : "default";
    service.state_.contentVersion = content.contentVersion;
    service.state_.now = instance != nullptr && instance->startTime ? *instance->startTime : content.startTime;
    for (const auto& [id, resource] : content.resources) {
        int initial = resource.initial;
        if (instance != nullptr) {
            if (const auto override = instance->resourceInitial.find(id); override != instance->resourceInitial.end()) {
                initial = override->second;
            }
        }
        service.state_.resources[id] = ResourceState{initial, 0};
    }
    service.state_.entities[kPlayerId] = WorldEntityState{kPlayerId, "actor", EntityLifecycle::Active, Json::object()};
    service.state_.inventories.try_emplace(kPlayerId);
    for (const auto& spatial : content.spatialEntities) {
        service.state_.entities[spatialEntityId(spatial.id)] =
            WorldEntityState{spatialEntityId(spatial.id), spatial.kind, EntityLifecycle::Active, Json::object()};
    }
    for (const auto& link : content.links) {
        service.state_.entities[linkEntityId(link.id)] =
            WorldEntityState{linkEntityId(link.id), link.kind, EntityLifecycle::Active, Json::object()};
        service.state_.links[link.id] = RuntimeLinkState{link.link.traversable, link.link.locked};
    }
    for (const auto& authored : content.npcs) {
        NpcState npc;
        npc.definitionId = authored.definition.id;
        npc.location = authored.initialLocation;
        npc.scheduleLabel = authored.initialScheduleLabel;
        npc.goalStage = authored.initialGoalStage;
        npc.strategy = authored.initialStrategy;
        npc.relationships[kPlayerId] = RelationshipAxes{};
        for (const auto& [peer, trust] : authored.initialPeerTrust) {
            npc.relationships[peer].trust = trust;
        }
        if (instance != nullptr) {
            if (const auto override = instance->npcs.find(authored.definition.id); override != instance->npcs.end()) {
                if (!override->second.location.empty()) npc.location = override->second.location;
                if (!override->second.scheduleLabel.empty()) npc.scheduleLabel = override->second.scheduleLabel;
                npc.goalStage = override->second.goalStage;
                if (!override->second.strategy.empty()) npc.strategy = override->second.strategy;
                if (!override->second.peerTrust.empty()) {
                    for (const auto& [peer, trust] : override->second.peerTrust) {
                        npc.relationships[peer].trust = trust;
                    }
                }
            }
        }
        DriveSystem::initialize(npc, authored.needs, service.state_.now);
        npc.lastDriveTick = service.state_.now;
        npc.memories.push_back(Memory{"authored", "Initial content pack memory.", service.state_.now, 1.0});
        service.state_.npcs.emplace(authored.definition.id, std::move(npc));
        service.state_.entities[npcEntityId(authored.definition.id)] =
            WorldEntityState{npcEntityId(authored.definition.id), "npc", EntityLifecycle::Active, Json::object()};
        service.state_.inventories.try_emplace(npcEntityId(authored.definition.id));
    }
    for (const auto& authored : content.socialGroups) {
        SocialGroupState group;
        group.definitionId = authored.id;
        group.metrics = authored.metrics;
        group.members = authored.initialMembers;
        group.relationships = authored.initialRelationships;
        service.state_.socialGroups.emplace(authored.id, std::move(group));
    }
    if (instance != nullptr) {
        for (const auto& [actor, resources] : instance->inventoryInitial) {
            const auto inventoryId = actor == kPlayerId ? kPlayerId : npcEntityId(actor);
            auto& inventory = service.state_.inventories[inventoryId];
            for (const auto& [resource, amount] : resources) inventory[resource] = amount;
        }
    }
    service.rebuildEntityRegistry();
    Json ids = Json::array();
    for (const auto& authored : content.npcs) ids.push_back(authored.definition.id);
    service.addEvent("world.initialized", kWorldActor, content.id, "World content pack loaded",
                     Json{{"content_pack", content.id}, {"content_version", content.version},
                          {"instance", instance != nullptr ? instance->id : "default"}, {"npcs", ids}});
    service.replayBaseline_ = service.snapshotJson();
    service.replayBaselineEvents_ = service.state_.events;
    return service;
}

void WorldService::rebuildEntityRegistry() {
    EntityProjectionSystem(*this).rebuild();
}

void WorldService::syncNpcEntityState() {
    EntityProjectionSystem(*this).syncNpc();
}

bool WorldService::hasNpc(const std::string& npcId) const {
    if (state_.npcs.find(npcId) == state_.npcs.end()) return false;
    const auto entity = state_.entities.find(npcEntityId(npcId));
    return entity == state_.entities.end() || entity->second.lifecycle == EntityLifecycle::Active;
}

DecisionContext WorldService::decisionContext(const std::string& npcId) const {
    const auto promptVersion = content_.contentVersion + ":" + content_.id;
    const auto worldRevision = state_.revision + (transactionManager_.active() ? 1 : 0);
    return DecisionContextProjector::build(state_, content_, npcId, worldRevision, promptVersion);
}

void WorldService::advanceMinutes(std::int64_t minutes) {
    SimulationCoordinator(*this).advanceMinutes(minutes);
}

void WorldService::advanceTo(GameTime target) {
    SimulationCoordinator(*this).advanceTo(target);
}

void WorldService::processHourBoundary(int day, int hour, const std::set<std::string>& dueNpcIds) {
    ScheduleSystem(*this).processHourBoundary(day, hour, dueNpcIds);
}

void WorldService::processActivity(const ActivityContent& activity) {
    NpcSimulationSystem(*this).processActivity(activity);
}

void WorldService::tickProcesses(const std::set<std::string>& dueNpcIds) {
    NpcSimulationSystem(*this).tickProcesses(dueNpcIds);
}

void WorldService::resumeSuspendedProcesses(const std::set<std::string>& dueNpcIds) {
    NpcSimulationSystem(*this).resumeSuspendedProcesses(dueNpcIds);
}

void WorldService::releaseReservations(ProcessExecution& process) {
    NpcSimulationSystem(*this).releaseReservations(process);
}

void WorldService::suspendProcess(const std::string& npcId, ProcessExecution& process,
                                  const std::string& reason, Json eventData) {
    NpcSimulationSystem(*this).suspendProcess(npcId, process, reason, std::move(eventData));
}

std::optional<std::vector<std::string>> WorldService::planResourcePreemption(
    const std::string& requesterId, const std::map<std::string, int>& requirements,
    int requesterPriority) {
    return NpcSimulationSystem(*this).planResourcePreemption(
        requesterId, requirements, requesterPriority);
}

void WorldService::cancelProcess(const std::string& npcId, ProcessExecution& process,
                                 const std::string& reason) {
    NpcSimulationSystem(*this).cancelProcess(npcId, process, reason);
}

void WorldService::completeProcess(const std::string& npcId, ProcessExecution process) {
    NpcSimulationSystem(*this).completeProcess(npcId, std::move(process));
}

void WorldService::processAutonomousTurns(const std::set<std::string>& dueNpcIds) {
    NpcSimulationSystem(*this).processAutonomousTurns(dueNpcIds);
}

void WorldService::processDayBoundary(int day, const std::set<std::string>& dueNpcIds) {
    NpcSimulationSystem(*this).processDayBoundary(day, dueNpcIds);
}

void WorldService::reviseNpcStrategies(int day) {
    NpcSimulationSystem(*this).reviseNpcStrategies(day);
}

void WorldService::applyPromiseDeadline() {
    ContractSystem(*this).applyPromiseDeadline();
}

void WorldService::applyExchangeDeadlines() {
    ContractSystem(*this).applyExchangeDeadlines();
}

CommandResult WorldService::proposeExchange(const std::string& proposer,
                                            const std::string& counterparty,
                                            std::map<std::string, int> offered,
                                            std::map<std::string, int> requested,
                                            int dueInMinutes, const std::string& actor,
                                            const std::string& turnId) {
    return ContractSystem(*this).proposeExchange(proposer, counterparty, std::move(offered),
                                                 std::move(requested), dueInMinutes, actor, turnId);
}

CommandResult WorldService::decideExchange(const std::string& contractId,
                                           ExchangeContractStatus decision,
                                           const std::string& actor,
                                           const std::string& turnId,
                                           const std::string& reason) {
    return ContractSystem(*this).decideExchange(contractId, decision, actor, turnId, reason);
}

CommandResult WorldService::settleExchange(const std::string& contractId,
                                           const std::string& actor,
                                           const std::string& turnId) {
    return ContractSystem(*this).settleExchange(contractId, actor, turnId);
}

CommandResult WorldService::reject(std::string code, std::string message) const {
    return CommandResult{false, std::move(code), std::move(message), std::nullopt};
}

CommandResult WorldService::submit(const Command& command) {
    return CommandDispatcher(*this).submit(command);
}

CommandResult WorldService::createPromise(const std::string& from, const std::string& to,
                                           const std::string& description, int dueInDays,
                                           const std::string& turnId) {
    return ContractSystem(*this).createPromise(from, to, description, dueInDays, turnId);
}

CommandResult WorldService::decidePromise(const std::string& promiseId, PromiseStatus decision,
                                          const std::string& actor, const std::string& turnId,
                                          const std::string& detail) {
    return ContractSystem(*this).decidePromise(promiseId, decision, actor, turnId, detail);
}

CommandResult WorldService::createEntity(const std::string& id, const std::string& archetypeId,
                                         Json metadata, const std::string& turnId) {
    return WorldCommandSystem(*this).createEntity(id, archetypeId, std::move(metadata), turnId);
}

CommandResult WorldService::setEntityLifecycle(const std::string& id, EntityLifecycle lifecycle,
                                               const std::string& turnId) {
    return WorldCommandSystem(*this).setEntityLifecycle(id, lifecycle, turnId);
}

CommandResult WorldService::setLinkState(const std::string& linkId, bool traversable, bool locked,
                                         const std::string& turnId) {
    return WorldCommandSystem(*this).setLinkState(linkId, traversable, locked, turnId);
}

CommandResult WorldService::transferResource(const std::string& source, const std::string& target,
                                             const std::string& resource, int amount,
                                             const std::string& actor, const std::string& turnId) {
    return WorldCommandSystem(*this).transferResource(source, target, resource, amount, actor, turnId);
}

CommandResult WorldService::tradeResources(const std::string& seller, const std::string& buyer,
                                           const std::string& sellerResource, int sellerAmount,
                                           const std::string& buyerResource, int buyerAmount,
                                           const std::string& actor, const std::string& turnId) {
    return WorldCommandSystem(*this).tradeResources(seller, buyer, sellerResource, sellerAmount,
                                                    buyerResource, buyerAmount, actor, turnId);
}

CommandResult WorldService::adjustRelationship(const std::string& npcId, const std::string& target,
                                               RelationshipAxes delta, const std::string& actor,
                                               const std::string& turnId, const std::string& reason) {
    return WorldCommandSystem(*this).adjustRelationship(npcId, target, delta, actor, turnId, reason);
}

CommandResult WorldService::adjustGroupMetric(const std::string& groupId, const std::string& metric,
                                              double delta, const std::string& actor,
                                              const std::string& turnId, const std::string& reason) {
    return WorldCommandSystem(*this).adjustGroupMetric(groupId, metric, delta, actor, turnId, reason);
}

CommandResult WorldService::setGroupMembership(const std::string& groupId, const std::string& npcId,
                                               const std::string& role, bool active,
                                               const std::string& actor, const std::string& turnId) {
    return WorldCommandSystem(*this).setGroupMembership(groupId, npcId, role, active, actor, turnId);
}

CommandResult WorldService::moveNpc(const std::string& npcId, const std::string& destination,
                                    const std::string& actor, const std::string& turnId) {
    return WorldCommandSystem(*this).moveNpc(npcId, destination, actor, turnId);
}

CommandResult WorldService::activateOverlay(const EvolutionProposal& proposal,
                                            const EvolutionSandboxResult& sandbox) {
    return WorldEvolutionSystem(*this).activateOverlay(proposal, sandbox);
}

CommandResult WorldService::rollbackOverlay(const std::string& proposalId) {
    return WorldEvolutionSystem(*this).rollbackOverlay(proposalId);
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

DialogueTurn WorldService::converse(const std::string& npcId, const std::string& playerInput,
                                    const std::string& turnId) {
    return WorldIntelligenceSystem(*this).converse(npcId, playerInput, turnId);
}

DialogueTurn WorldService::deliberate(const std::string& npcId, const std::string& trigger,
                                      const std::string& turnId) {
    return WorldIntelligenceSystem(*this).deliberate(npcId, trigger, turnId);
}

ProviderResponse WorldService::requestEvolutionProposal(const std::string& evolutionLevel,
                                                         const std::string& trigger,
                                                         const Json& validationFeedback) const {
    return WorldEvolutionSystem::requestProposal(*this, evolutionLevel, trigger, validationFeedback);
}

TurnRequest WorldService::makeTurnRequest(const std::string& npcId, const std::string& input,
                                          const std::string& turnId, const std::string& channelId,
                                          const std::string& trigger) {
    return WorldIntelligenceSystem(*this).makeTurnRequest(npcId, input, turnId, channelId, trigger);
}

DialogueTurn WorldService::executeTurn(const std::string& npcId, const std::string& input,
                                       const std::string& turnId, const std::string& channelId,
                                       const std::string& trigger) {
    return WorldIntelligenceSystem(*this).executeTurn(npcId, input, turnId, channelId, trigger);
}

DialogueTurn WorldService::commitTurn(const TurnRequest& request, ProviderResponse response) {
    return WorldIntelligenceSystem(*this).commitTurn(request, std::move(response));
}

void WorldService::enqueueAutonomousTurn(const std::string& npcId, const std::string& trigger,
                                         const std::string& channelId) {
    WorldIntelligenceSystem(*this).enqueueAutonomousTurn(npcId, trigger, channelId);
}

void WorldService::scheduleAutomaticEvolution() {
    // MockProvider is an explicit deterministic test adapter and has no
    // evolution implementation. Do not enqueue failed mock work into a
    // partitioned offline simulation; product providers remain fail-closed
    // and are scheduled from the configured policy below.
    if (!evolution_ || provider_->name() == "mock" || !content_.evolutionPolicy.enabled || state_.now.minute <= 0 ||
        state_.now.minute % kMinutesPerDay != 0) {
        return;
    }
    const int day = state_.now.day();
    const auto schedule = [this, day](const std::string& level, int intervalDays) {
        if (intervalDays <= 0 || day % intervalDays != 0) return;
        const std::string key = "day:" + std::to_string(day) + ":" + level;
        if (content_.evolutionPolicy.coalescePending && pendingEvolutionKeys_.contains(key)) return;
        const std::string trigger = "day_boundary:" + std::to_string(day);
        const auto context = WorldEvolutionSystem::proposalContext(*this, level, trigger, Json::object());
        if (context.empty()) return;
        EvolutionRequest request;
        request.requestId = "evolution:auto:" + level + ":" + std::to_string(day) + ":" +
                            std::to_string(state_.revision) + ":attempt:0";
        request.key = key;
        request.evolutionLevel = level;
        request.trigger = trigger;
        request.baseContentVersion = content_.contentVersion;
        request.baseWorldRevision = state_.revision;
        request.createdAt = state_.now;
        request.deadline = GameTime{state_.now.minute + content_.evolutionPolicy.maxProposalAgeMinutes};
        request.coalescePending = content_.evolutionPolicy.coalescePending;
        request.context = context;
        const auto submitted = evolution_->submit(std::move(request));
        if (!submitted.accepted) return;
        pendingEvolutionKeys_.insert(key);
    };
    schedule("level1", content_.evolutionPolicy.level1IntervalDays);
    schedule("level2", content_.evolutionPolicy.level2IntervalDays);
}

std::size_t WorldService::pumpAsyncTurns() {
    return WorldIntelligenceSystem(*this).pumpAsyncTurns() +
           WorldEvolutionSystem(*this).pumpAsyncProposals();
}

std::size_t WorldService::pendingAsyncTurns() const {
    return WorldIntelligenceSystem::pendingAsyncTurns(*this) +
           (evolution_ ? evolution_->pending() : 0);
}

std::size_t WorldService::waitForAsyncTurns(std::int64_t timeoutMilliseconds) {
    if (timeoutMilliseconds <= 0) return pumpAsyncTurns();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMilliseconds);
    std::size_t consumed = 0;
    while (pendingAsyncTurns() > 0 && std::chrono::steady_clock::now() < deadline) {
        consumed += pumpAsyncTurns();
        if (pendingAsyncTurns() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    consumed += pumpAsyncTurns();
    return consumed;
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


std::string summarize(const WorldState& state) {
    std::ostringstream output;
    output << state.now.toString();
    for (const auto& [id, value] : state.resources) output << "; " << id << '=' << value.amount;
    output << "; revision=" << state.revision;
    for (const auto& [id, npc] : state.npcs) {
        output << "; " << id << "@" << npc.location << ":stage=" << npc.goalStage;
        const auto highest = std::max_element(npc.needs.begin(), npc.needs.end(),
                                              [](const auto& left, const auto& right) {
                                                  if (left.second.deficit == right.second.deficit) {
                                                      return left.first > right.first;
                                                  }
                                                  return left.second.deficit < right.second.deficit;
                                              });
        if (highest != npc.needs.end()) output << ":need=" << highest->first << '=' << highest->second.deficit;
    }
    return output.str();
}


}  // namespace tide_marks
