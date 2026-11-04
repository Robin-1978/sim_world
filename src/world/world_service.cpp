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
#include "world/resource_system.hpp"
#include "world/social_system.hpp"
#include "world/meta_action_executor.hpp"
#include "world/world_state_json.hpp"
#include "npc/drive_system.hpp"
#include "npc/state_machine.hpp"
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

namespace {
void applyNpcSignal(NpcState& npc, NpcSignal signal) {
    npc.mode = toString(transition(npcModeFromString(npc.mode), signal));
}

Json replayFactJson(const WorldState& state, std::size_t eventSequence) {
    Json json = worldStateToJson(state);
    json.erase("events");
    json["event_sequence"] = eventSequence;
    return json;
}

std::string stateDigest(const Json& state) {
    std::uint64_t hash = 1469598103934665603ULL;
    const std::string canonical = state.dump();
    for (const unsigned char byte : canonical) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream digest;
    digest << "fnv1a64:" << std::hex << hash;
    return digest.str();
}

void validateEventHistory(const WorldState& state) {
    std::map<std::string, std::uint64_t> correlationRevisions;
    std::uint64_t previousRevision = 0;
    for (std::size_t index = 0; index < state.events.size(); ++index) {
        const auto& event = state.events[index];
        if (event.schemaVersion != kWorldEventSchemaVersion || event.sequence != index + 1 ||
            event.eventId != "event:" + std::to_string(event.sequence)) {
            throw std::runtime_error("snapshot contains an invalid event identity");
        }
        if (event.contentVersion.empty() ||
            event.simulationVersion != kSimulationVersion) {
            throw std::runtime_error("snapshot event has incomplete version provenance");
        }
        if (event.worldRevision == 0 || event.worldRevision > state.revision ||
            event.worldRevision < previousRevision) {
            throw std::runtime_error("snapshot event has an invalid world revision");
        }
        previousRevision = event.worldRevision;
        const std::string prefix = "correlation:";
        if (!event.correlationId.starts_with(prefix) || event.correlationId.size() == prefix.size()) {
            throw std::runtime_error("snapshot event has an invalid correlation id");
        }
        const auto [revision, inserted] =
            correlationRevisions.emplace(event.correlationId, event.worldRevision);
        if (!inserted && revision->second != event.worldRevision) {
            throw std::runtime_error("snapshot transaction combines events from different world revisions");
        }
        if (event.causationSequence == 0) continue;
        if (event.causationSequence >= event.sequence) {
            throw std::runtime_error("snapshot event causation does not precede its effect");
        }
        const auto& cause = state.events.at(static_cast<std::size_t>(event.causationSequence - 1));
        if (cause.correlationId != event.correlationId) {
            throw std::runtime_error("snapshot event causation crosses correlation chains");
        }
    }
}
}

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
    (void)day;
    const int minuteOfDay = hour * static_cast<int>(kMinutesPerHour);
    for (auto& [id, npc] : state_.npcs) {
        if (!dueNpcIds.contains(id)) continue;
        const auto* authored = authoredNpc(id);
        if (authored != nullptr) {
            const GameTime previous = npc.lastDriveTick.minute >= 0
                                           ? npc.lastDriveTick
                                           : GameTime{state_.now.minute - kMinutesPerHour};
            const auto changes = DriveSystem::advance(npc, authored->needs, previous, state_.now);
            Json changed = Json::array();
            for (const auto& change : changes) {
                changed.push_back(Json{{"need_id", change.needId}, {"before", change.before}, {"after", change.after}});
                if (change.becameCritical) {
                    addEvent("need.critical", id, change.needId, "A need crossed its critical threshold",
                             Json{{"need_id", change.needId}, {"before", change.before},
                                  {"deficit", change.after}, {"rule", "drive.time_decay"},
                                  {"from_minute", previous.minute}, {"to_minute", state_.now.minute}});
                }
            }
            if (!changed.empty()) {
                addEvent("need.deprived", id, "self", "Need deficits advanced with world time",
                         Json{{"changes", changed}, {"rule", "drive.time_decay"},
                              {"from_minute", previous.minute}, {"to_minute", state_.now.minute}});
            }
            npc.lastDriveTick = state_.now;
        }
        const auto entity = state_.entities.find(npcEntityId(id));
        if (entity != state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        std::string scheduledLocation = authored != nullptr ? authored->definition.home : npc.location;
        if (authored != nullptr) {
            for (const auto& slot : authored->schedule) {
                const bool within = slot.startMinute < slot.endMinute
                                        ? minuteOfDay >= slot.startMinute && minuteOfDay < slot.endMinute
                                        : (slot.startMinute == slot.endMinute || minuteOfDay >= slot.startMinute || minuteOfDay < slot.endMinute);
                if (within) {
                    scheduledLocation = slot.location;
                    break;
                }
            }
        }
        const auto previousLocation = npc.location;
        if (npc.location != scheduledLocation) {
            ++npc.stateVersion;
            addEvent("npc.schedule_applied", id, scheduledLocation,
                     "The authored baseline schedule placed the NPC at its scheduled location",
                     Json{{"from", previousLocation}, {"to", scheduledLocation},
                          {"rule", "content.schedule"}, {"minute_of_day", minuteOfDay}});
        }
        npc.location = scheduledLocation;
        npc.scheduleLabel = scheduledLocation;
        if (!npc.activity.process || npc.activity.process->status != ProcessStatus::Running) {
            const bool atHome = authored != nullptr && npc.location == authored->definition.home;
            applyNpcSignal(npc, atHome ? NpcSignal::AtHome : NpcSignal::AtWork);
        }
    }
    for (const auto& activity : content_.activities) {
        if (activity.trigger != "hour" || activity.hour != hour) continue;
        if (!activity.conditionResource.empty() && resourceValue(activity.conditionResource) >= activity.conditionBelow) continue;
        processActivity(activity);
    }
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
    TransactionScope transaction(*this);
    if (id.empty() || archetypeId.empty()) return reject("invalid_entity", "Entity id and archetype are required.");
    if (state_.entities.contains(id)) return reject("duplicate_entity", "That entity id already exists.");
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The turn was already applied.");
    const auto archetype = std::find_if(content_.entityArchetypes.begin(), content_.entityArchetypes.end(),
                                        [&archetypeId](const auto& item) { return item.id == archetypeId; });
    if (archetype == content_.entityArchetypes.end()) return reject("unknown_archetype", "The entity archetype is not registered.");
    Json merged = archetype->defaultMetadata;
    for (auto it = metadata.begin(); it != metadata.end(); ++it) merged[it.key()] = it.value();
    state_.entities.emplace(id, WorldEntityState{id, archetype->kind, EntityLifecycle::Active, std::move(merged)});
    state_.inventories.try_emplace(id);
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "entity.created", kWorldActor, id, "Runtime entity created",
        Json{{"entity_id", id}, {"archetype", archetypeId}, {"kind", archetype->kind},
             {"metadata", state_.entities.at(id).metadata}});
    return CommandResult{true, "ok", id, event};
}

CommandResult WorldService::setEntityLifecycle(const std::string& id, EntityLifecycle lifecycle,
                                               const std::string& turnId) {
    TransactionScope transaction(*this);
    auto found = state_.entities.find(id);
    if (found == state_.entities.end()) return reject("unknown_entity", "That entity does not exist.");
    if (found->second.lifecycle == EntityLifecycle::Destroyed) return reject("destroyed_entity", "A destroyed entity cannot be revived.");
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The turn was already applied.");
    found->second.lifecycle = lifecycle;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent("entity.lifecycle", kWorldActor, id, "Runtime entity lifecycle changed",
                                Json{{"entity_id", id}, {"lifecycle", toString(lifecycle)}});
    return CommandResult{true, "ok", toString(lifecycle), event};
}

CommandResult WorldService::setLinkState(const std::string& linkId, bool traversable, bool locked,
                                         const std::string& turnId) {
    TransactionScope transaction(*this);
    const auto entity = state_.entities.find(linkEntityId(linkId));
    if (entity == state_.entities.end()) return reject("unknown_link", "That link does not exist.");
    if (entity->second.lifecycle != EntityLifecycle::Active) return reject("inactive_link", "The link is not active.");
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The turn was already applied.");
    state_.links[linkId] = RuntimeLinkState{traversable, locked};
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent("link.state", kWorldActor, linkEntityId(linkId), "Traversal link state changed",
                                Json{{"link_id", linkId}, {"traversable", traversable}, {"locked", locked}});
    return CommandResult{true, "ok", linkId, event};
}

CommandResult WorldService::transferResource(const std::string& source, const std::string& target,
                                             const std::string& resource, int amount,
                                             const std::string& actor, const std::string& turnId) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The transfer was already applied.");
    }
    const auto sourceId = ResourceSystem::resolveParty(state_, source);
    const auto targetId = ResourceSystem::resolveParty(state_, target);
    if (actor != kWorldActor && actor != kPlayerId && actor != sourceId && actor != targetId &&
        actor != source && actor != target) {
        return reject("unauthorized", "The actor is not a party to this transfer.");
    }
    const auto result = ResourceSystem::transfer(state_, content_, source, target, resource, amount);
    if (!result.accepted) return reject(result.code, result.message);
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "resource.transferred", actor, targetId, "A registered resource transfer changed ownership quantities",
        Json{{"resource", resource}, {"amount", amount}, {"source", sourceId}, {"target", targetId},
             {"source_before", result.sourceBefore}, {"source_after", result.sourceAfter},
             {"target_before", result.targetBefore}, {"target_after", result.targetAfter}});
    return CommandResult{true, "ok", resource, event};
}

CommandResult WorldService::tradeResources(const std::string& seller, const std::string& buyer,
                                           const std::string& sellerResource, int sellerAmount,
                                           const std::string& buyerResource, int buyerAmount,
                                           const std::string& actor, const std::string& turnId) {
    TransactionScope transaction(*this);
    if (sellerAmount <= 0 || buyerAmount <= 0 || sellerResource.empty() || buyerResource.empty()) {
        return reject("invalid_trade", "A trade needs two positive resource quantities.");
    }
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The trade was already applied.");
    }
    if (actor != kWorldActor && actor != kPlayerId && actor != seller && actor != buyer) {
        return reject("unauthorized", "The actor is not a party to this trade.");
    }
    const auto first = transferResource(seller, buyer, sellerResource, sellerAmount, actor);
    if (!first.accepted) return reject(first.code, first.message);
    const auto second = transferResource(buyer, seller, buyerResource, buyerAmount, actor);
    if (!second.accepted) {
        transaction.rollback();
        return reject(second.code, second.message);
    }
    if (!turnId.empty()) {
        state_.processedTurnIds.insert(turnId);
    }
    const auto event = addEvent("trade.settled", actor, seller,
                                "Two parties exchanged configured resources atomically",
                                Json{{"seller", seller}, {"buyer", buyer}, {"seller_resource", sellerResource},
                                     {"seller_amount", sellerAmount}, {"buyer_resource", buyerResource},
                                     {"buyer_amount", buyerAmount}});
    return CommandResult{true, "ok", "trade", event};
}

CommandResult WorldService::adjustRelationship(const std::string& npcId, const std::string& target,
                                               RelationshipAxes delta, const std::string& actor,
                                               const std::string& turnId, const std::string& reason) {
    TransactionScope transaction(*this);
    if (actor != kWorldActor && actor != kPlayerId && actor != npcId) {
        return reject("unauthorized", "Only the actor or world authority may change this relationship.");
    }
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The relationship change was already applied.");
    }
    const auto result = SocialSystem::adjustRelationship(state_, npcId, target, delta);
    if (!result.accepted) return reject(result.code, result.message);
    ++state_.npcs.at(npcId).stateVersion;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent("relationship.adjusted", actor, target,
                                reason.empty() ? "A relationship changed" : reason,
                                Json{{"npc_id", npcId}, {"target", target}, {"trust", delta.trust},
                                     {"respect", delta.respect}, {"fear", delta.fear}, {"owed", delta.owed}});
    return CommandResult{true, "ok", target, event};
}

CommandResult WorldService::adjustGroupMetric(const std::string& groupId, const std::string& metric,
                                              double delta, const std::string& actor,
                                              const std::string& turnId, const std::string& reason) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The group metric change was already applied.");
    const auto result = SocialSystem::adjustGroupMetric(state_, groupId, metric, delta);
    if (!result.accepted) return reject(result.code, result.message);
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "social.group_metric", actor, groupId, reason.empty() ? "A social group metric changed" : reason,
        Json{{"group_id", groupId}, {"metric", metric}, {"before", result.before},
             {"after", result.after}, {"delta", delta}});
    return CommandResult{true, "ok", metric, event};
}

CommandResult WorldService::setGroupMembership(const std::string& groupId, const std::string& npcId,
                                               const std::string& role, bool active,
                                               const std::string& actor, const std::string& turnId) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The membership change was already applied.");
    const auto result = SocialSystem::setMembership(state_, groupId, npcId, role, active);
    if (!result.accepted) return reject(result.code, result.message);
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        active ? "social.member_joined" : "social.member_left", actor, groupId,
        active ? "An NPC joined a social group" : "An NPC left a social group",
        Json{{"group_id", groupId}, {"npc_id", npcId}, {"role", role}});
    return CommandResult{true, "ok", groupId, event};
}

CommandResult WorldService::moveNpc(const std::string& npcId, const std::string& destination,
                                    const std::string& actor, const std::string& turnId) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The movement was already applied.");
    }
    const auto npcFound = state_.npcs.find(npcId);
    if (npcFound == state_.npcs.end()) return reject("unknown_npc", "That NPC does not exist.");
    if (actor != kWorldActor && actor != npcId && actor != kPlayerId) {
        return reject("unauthorized", "Only the NPC or an authorized world actor can move it.");
    }
    const auto destinationEntity = state_.entities.find(spatialEntityId(destination));
    if (destinationEntity == state_.entities.end() ||
        destinationEntity->second.lifecycle != EntityLifecycle::Active) {
        return reject("unknown_destination", "The destination is not an active spatial entity.");
    }
    if (npcFound->second.location == destination) return CommandResult{true, "ok", destination, std::nullopt};
    if (!planRoute(npcFound->second.location, destination).has_value()) {
        return reject("no_route", "No active topology route connects the NPC to the destination.");
    }
    const std::string previous = npcFound->second.location;
    npcFound->second.location = destination;
    ++npcFound->second.stateVersion;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "entity.moved", actor, npcEntityId(npcId), "An NPC arrived at an authored location",
        Json{{"npc_id", npcId}, {"from", previous}, {"to", destination},
             {"route", planRoute(previous, destination).value_or(std::vector<std::string>{})}});
    syncNpcEntityState();
    return CommandResult{true, "ok", destination, event};
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
    GlobalPlanner planner;
    for (const auto& spatial : content_.spatialEntities) planner.addNode(spatial.id);
    for (const auto& authored : content_.links) {
        const auto entity = state_.entities.find(linkEntityId(authored.id));
        const auto runtime = state_.links.find(authored.id);
        if (entity != state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        if (runtime != state_.links.end() && (!runtime->second.traversable || runtime->second.locked)) continue;
        for (std::size_t left = 0; left < authored.link.endpoints.size(); ++left) {
            for (std::size_t right = left + 1; right < authored.link.endpoints.size(); ++right) {
                planner.addEdge(authored.link.endpoints[left], authored.link.endpoints[right], 1.0, true);
            }
        }
    }
    return planner.plan(from, to);
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

std::size_t WorldService::pumpAsyncTurns() {
    return WorldIntelligenceSystem(*this).pumpAsyncTurns();
}

std::size_t WorldService::pendingAsyncTurns() const {
    return WorldIntelligenceSystem::pendingAsyncTurns(*this);
}

std::size_t WorldService::waitForAsyncTurns(std::int64_t timeoutMilliseconds) {
    return WorldIntelligenceSystem(*this).waitForAsyncTurns(timeoutMilliseconds);
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

Json WorldService::snapshotJson() const { return worldStateToJson(state_); }

void WorldService::saveSnapshot(const std::string& path) const {
    persistence_->saveSnapshot(path, snapshotJson());
}

void WorldService::writeReplay(const std::string& path) const {
    std::vector<Json> lines;
    Json baseline = replayBaseline_.is_object() && !replayBaseline_.empty() ? replayBaseline_ : snapshotJson();
    const auto baselineRevision = baseline.at("world_revision").get<std::uint64_t>();
    Json baselineFact = baseline;
    baselineFact.erase("events");
    const Json currentFact = replayFactJson(state_, state_.events.size());
    const auto patch = Json::diff(baselineFact, currentFact);
    const bool hasChanges = !patch.empty() || state_.events.size() > replayBaselineEvents_.size();
    lines.push_back(Json{{"kind", "replay_header"}, {"schema_version", 2}, {"state", baseline},
                         {"transaction_count", hasChanges ? 1U : 0U}});
    if (hasChanges) {
        Json events = Json::array();
        for (std::size_t index = replayBaselineEvents_.size(); index < state_.events.size(); ++index) {
            events.push_back(worldEventToJson(state_.events[index]));
        }
        lines.push_back(Json{{"kind", "transaction"}, {"base_world_revision", baselineRevision},
                             {"world_revision", state_.revision}, {"patch", patch}, {"events", events},
                             {"state_digest", stateDigest(currentFact)}});
    }
    persistence_->saveReplay(path, lines);
}

void WorldService::saveDurableCommit(const std::string& path) const {
    std::vector<Json> lines;
    Json baseline = replayBaseline_.is_object() && !replayBaseline_.empty() ? replayBaseline_ : snapshotJson();
    const auto baselineRevision = baseline.at("world_revision").get<std::uint64_t>();
    Json baselineFact = baseline;
    baselineFact.erase("events");
    const Json currentFact = replayFactJson(state_, state_.events.size());
    const auto patch = Json::diff(baselineFact, currentFact);
    const bool hasChanges = !patch.empty() || state_.events.size() > replayBaselineEvents_.size();
    lines.push_back(Json{{"kind", "replay_header"}, {"schema_version", 2}, {"state", baseline},
                         {"transaction_count", hasChanges ? 1U : 0U}});
    if (hasChanges) {
        Json events = Json::array();
        for (std::size_t index = replayBaselineEvents_.size(); index < state_.events.size(); ++index) {
            events.push_back(worldEventToJson(state_.events[index]));
        }
        lines.push_back(Json{{"kind", "transaction"}, {"base_world_revision", baselineRevision},
                             {"world_revision", state_.revision}, {"patch", patch}, {"events", events},
                             {"state_digest", stateDigest(currentFact)}});
    }
    persistence_->saveDurableCommit(path, snapshotJson(), lines);
}

void WorldService::configureDurableCheckpoint(DurableCheckpointPolicy policy) {
    if (policy.mode == DurableCheckpointMode::Disabled) {
        durableCheckpointPolicy_ = std::move(policy);
        nextDurableCheckpointMinute_ = 0;
        return;
    }
    if (policy.path.empty()) throw std::invalid_argument("durable checkpoint policy requires a path");
    if (policy.mode == DurableCheckpointMode::PeriodicMinutes && policy.intervalMinutes <= 0) {
        throw std::invalid_argument("periodic durable checkpoint interval must be positive");
    }
    durableCheckpointPolicy_ = std::move(policy);
    if (durableCheckpointPolicy_.mode == DurableCheckpointMode::PeriodicMinutes) {
        nextDurableCheckpointMinute_ = state_.now.minute + durableCheckpointPolicy_.intervalMinutes;
    } else {
        nextDurableCheckpointMinute_ = 0;
    }
}

void WorldService::maybeSaveDurableCheckpoint() {
    if (durableCheckpointPolicy_.mode == DurableCheckpointMode::Disabled ||
        durableCheckpointPolicy_.path.empty()) return;
    bool due = false;
    if (durableCheckpointPolicy_.mode == DurableCheckpointMode::PeriodicMinutes) {
        due = state_.now.minute >= nextDurableCheckpointMinute_;
    } else if (durableCheckpointPolicy_.mode == DurableCheckpointMode::DayBoundary) {
        due = state_.now.minute > 0 && state_.now.minute % kMinutesPerDay == 0 &&
              state_.revision != lastDurableCheckpointRevision_;
    }
    if (!due) return;
    saveDurableCommit(durableCheckpointPolicy_.path);
    lastDurableCheckpointRevision_ = state_.revision;
    if (durableCheckpointPolicy_.mode == DurableCheckpointMode::PeriodicMinutes) {
        do {
            nextDurableCheckpointMinute_ += durableCheckpointPolicy_.intervalMinutes;
        } while (nextDurableCheckpointMinute_ <= state_.now.minute);
    }
}

WorldService WorldService::fromContentSnapshot(const WorldContentPack& content, const Json& json,
                                                     std::unique_ptr<IProvider> provider) {
    WorldService service(std::move(provider));
    service.baseContent_ = content;
    service.content_ = content;
    const auto& snapshot = json.contains("state") ? json.at("state") : json;
    if (!snapshot.is_object() || !snapshot.contains("save_schema_version") ||
        !snapshot.at("save_schema_version").is_number_integer() ||
        snapshot.at("save_schema_version").get<int>() != kSnapshotSchemaVersion) {
        throw std::runtime_error("unsupported snapshot schema; expected save_schema_version=" +
                                 std::to_string(kSnapshotSchemaVersion));
    }
    if (!snapshot.contains("simulation_version") || !snapshot.at("simulation_version").is_string() ||
        snapshot.at("simulation_version") != kSimulationVersion) {
        throw std::runtime_error("unsupported snapshot simulation version; expected " +
                                 std::string{kSimulationVersion});
    }
    service.state_ = deserializeWorldState(snapshot);
    if (snapshot.value("event_sequence", static_cast<std::size_t>(0)) != service.state_.events.size()) {
        throw std::runtime_error("snapshot event_sequence does not match its event history");
    }
    validateEventHistory(service.state_);
    if (!service.state_.activeOverlays.empty()) {
        auto rebuilt = content;
        for (const auto& overlay : service.state_.activeOverlays) {
            EvolutionProposal replay{overlay.proposalId, "declarative_capability", "snapshot",
                                     overlay.baseContentVersion, 0, overlay.operations,
                                     EvolutionProposalStatus::Approved, {}};
            const auto applied = EvolutionManager::applyOverlay(rebuilt, replay);
            if (!applied.valid) {
                throw std::runtime_error("cannot restore overlay " + overlay.proposalId + ": " + applied.message);
            }
            if (rebuilt.contentVersion != overlay.overlayVersion) {
                throw std::runtime_error("restored overlay version does not match its activation record: " +
                                         overlay.proposalId);
            }
        }
        service.content_ = std::move(rebuilt);
    }
    for (const auto& [id, npc] : service.state_.npcs) {
        if (npc.definitionId.empty() || service.authoredNpc(npc.definitionId) == nullptr) {
            throw std::runtime_error("snapshot references unknown npc definition: " + npc.definitionId);
        }
    }
    for (const auto& [id, group] : service.state_.socialGroups) {
        if (std::none_of(content.socialGroups.begin(), content.socialGroups.end(),
                         [&id](const auto& defined) { return defined.id == id; })) {
            throw std::runtime_error("snapshot references unknown social group: " + id);
        }
        for (const auto& [npcId, role] : group.members) {
            (void)role;
            if (!service.state_.npcs.contains(npcId)) {
                throw std::runtime_error("social group snapshot references unknown NPC: " + id + "/" + npcId);
            }
        }
    }
    const std::string expectedInstanceId = service.content_.instance ? service.content_.instance->id : "default";
    if (service.state_.instanceId.empty() || service.state_.instanceId != expectedInstanceId) {
        throw std::runtime_error("snapshot instance " + service.state_.instanceId +
                                 " does not match loaded instance " + expectedInstanceId);
    }
    const std::string expectedWorldId = service.content_.instance && !service.content_.instance->worldId.empty()
                                            ? service.content_.instance->worldId
                                            : service.content_.worldId;
    if (service.state_.worldId.empty() || service.state_.worldId != expectedWorldId) {
        throw std::runtime_error("snapshot world " + service.state_.worldId +
                                 " does not match loaded world " + expectedWorldId);
    }
    if (service.state_.contentVersion.empty() || service.state_.contentVersion != service.content_.contentVersion) {
        throw std::runtime_error("snapshot content version " + service.state_.contentVersion +
                                 " does not match loaded content " + service.content_.contentVersion);
    }
    service.rebuildEntityRegistry();
    service.replayBaseline_ = service.snapshotJson();
    service.replayBaselineEvents_ = service.state_.events;
    return service;
}

WorldService WorldService::fromSnapshot(const Json& json, std::string contentRoot,
                                        std::unique_ptr<IProvider> provider) {
    const auto content = ContentPackLoader::load(contentRoot);
    return fromContentSnapshot(content, json, std::move(provider));
}

WorldService WorldService::replay(const std::string& path, std::string contentRoot,
                                  std::unique_ptr<IProvider> provider) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read replay: " + path);
    std::vector<Json> documents;
    std::ostringstream whole;
    whole << input.rdbuf();
    const auto bytes = whole.str();
    if (!bytes.empty()) {
        try {
            const auto artifact = Json::parse(bytes);
            if (artifact.is_object() && artifact.value("kind", "") == "durable_commit") {
                if (!artifact.contains("replay") || !artifact.at("replay").is_array()) {
                    throw std::runtime_error("durable commit has no replay package");
                }
                for (const auto& item : artifact.at("replay")) documents.push_back(item);
            }
        } catch (const nlohmann::json::parse_error&) {
            // A normal Replay Package is JSONL, so parsing the complete file
            // as one JSON document is expected to fail. Parse its records
            // below instead.
        }
    }
    if (documents.empty()) {
        std::istringstream lines(bytes);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty()) continue;
            documents.push_back(Json::parse(line));
        }
    }
    Json current;
    std::size_t expectedTransactions = 0;
    std::size_t appliedTransactions = 0;
    for (const auto& json : documents) {
        const auto kind = json.value("kind", "");
        if (kind == "replay_header") {
            if (json.value("schema_version", 0) != 2 || !json.contains("state")) {
                throw std::runtime_error("unsupported replay header");
            }
            current = json.at("state");
            expectedTransactions = json.value("transaction_count", 0U);
            if (!current.is_object() || !current.contains("events")) {
                throw std::runtime_error("replay baseline must contain an event history");
            }
            continue;
        }
        if (kind != "transaction") continue;
        if (current.empty()) throw std::runtime_error("replay transaction precedes its header");
        if (!json.contains("patch") || !json.at("patch").is_array() ||
            !json.contains("events") || !json.at("events").is_array()) {
            throw std::runtime_error("replay transaction is incomplete");
        }
        const auto baseRevision = current.at("world_revision").get<std::uint64_t>();
        if (json.at("base_world_revision").get<std::uint64_t>() != baseRevision) {
            throw std::runtime_error("replay transaction base revision does not match reconstructed state");
        }
        Json fact = current;
        fact.erase("events");
        fact = fact.patch(json.at("patch"));
        if (fact.at("world_revision").get<std::uint64_t>() !=
            json.at("world_revision").get<std::uint64_t>()) {
            throw std::runtime_error("replay transaction patch produced the wrong world revision");
        }
        if (json.contains("state_digest") &&
            json.at("state_digest").get<std::string>() != stateDigest(fact)) {
            throw std::runtime_error("replay transaction state digest mismatch");
        }
        auto events = current.at("events");
        for (const auto& event : json.at("events")) events.push_back(event);
        fact["events"] = std::move(events);
        current = std::move(fact);
        ++appliedTransactions;
    }
    if (current.empty()) throw std::runtime_error("replay has no header");
    if (appliedTransactions != expectedTransactions) {
        throw std::runtime_error("replay transaction count mismatch");
    }
    return fromSnapshot(current, std::move(contentRoot), std::move(provider));
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
