#include "world/world_service.hpp"
#include "world/world_state_json.hpp"
#include "npc/drive_system.hpp"
#include "npc/state_machine.hpp"
#include "world/ids.hpp"
#include "evolution/evolution.hpp"
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <exception>
#include <set>

namespace tide_marks {

namespace {
void applyRelationshipDelta(RelationshipAxes& axes, const RelationshipDelta& delta) {
    axes.trust = std::clamp(axes.trust + delta.trust, 0.0, 1.0);
    axes.respect = std::clamp(axes.respect + delta.respect, 0.0, 1.0);
    axes.fear = std::clamp(axes.fear + delta.fear, 0.0, 1.0);
    axes.owed = std::clamp(axes.owed + delta.owed, 0.0, 1.0);
}

void applyNpcSignal(NpcState& npc, NpcSignal signal) {
    npc.mode = toString(transition(npcModeFromString(npc.mode), signal));
}

Json mergedBehaviorArgs(const MetaActionDefinition& definition, const BehaviorNodeContent& node) {
    Json result = definition.defaultArgs;
    for (auto it = node.args.begin(); it != node.args.end(); ++it) result[it.key()] = it.value();
    return result;
}

Json quantityMapJson(const std::map<std::string, int>& quantities) {
    Json result = Json::object();
    for (const auto& [resource, amount] : quantities) result[resource] = amount;
    return result;
}

std::string canonicalActorId(const WorldState& state, const std::string& value) {
    if (value == kWorldActor || value == kPlayerId) return value;
    if (state.npcs.contains(value)) return value;
    if (value.starts_with("npc:")) {
        const auto npcId = value.substr(4);
        if (state.npcs.contains(npcId)) return npcId;
    }
    return {};
}

bool actorIsActive(const WorldState& state, const std::string& actorId) {
    if (actorId == kWorldActor) return true;
    const auto entityId = actorId == kPlayerId ? actorId : npcEntityId(actorId);
    const auto entity = state.entities.find(entityId);
    return entity != state.entities.end() && entity->second.lifecycle == EntityLifecycle::Active;
}

WorldState transactionalBackup(const WorldState& state, std::size_t& eventCount) {
    eventCount = state.events.size();
    // Build the rollback image field-by-field so the immutable event history
    // is never copied into every minute transaction.  A default WorldState
    // copy would still copy the complete JSON event log before clearing it,
    // turning a long offline advance into O(history^2) work.
    WorldState backup;
    backup.worldId = state.worldId;
    backup.instanceId = state.instanceId;
    backup.contentVersion = state.contentVersion;
    backup.now = state.now;
    backup.revision = state.revision;
    backup.resources = state.resources;
    backup.inventories = state.inventories;
    backup.security = state.security;
    backup.activeOverlays = state.activeOverlays;
    backup.entities = state.entities;
    backup.links = state.links;
    backup.npcs = state.npcs;
    backup.socialGroups = state.socialGroups;
    backup.promises = state.promises;
    backup.exchangeContracts = state.exchangeContracts;
    backup.processedTurnIds = state.processedTurnIds;
    return backup;
}

void restoreTransactionalBackup(WorldState& state, WorldState backup,
                                std::size_t eventCount) {
    std::vector<WorldEvent> committed;
    const auto end = std::min(eventCount, state.events.size());
    committed.assign(state.events.begin(), state.events.begin() + static_cast<std::ptrdiff_t>(end));
    state = std::move(backup);
    state.events = std::move(committed);
}
}

class WorldService::TransactionScope {
public:
    explicit TransactionScope(WorldService& service)
        : service_(service), uncaughtAtConstruction_(std::uncaught_exceptions()) {
        service_.beginTransaction();
    }

    ~TransactionScope() {
        if (!active_) return;
        if (std::uncaught_exceptions() > uncaughtAtConstruction_) service_.rollbackTransaction();
        else service_.commitTransaction();
    }

    // Used when a validated recipe reports failure after staging a partial
    // change. No caller-visible fact may survive a failed recipe.
    void rollback() {
        if (!active_) return;
        active_ = false;
        service_.rollbackTransaction();
    }

    TransactionScope(const TransactionScope&) = delete;
    TransactionScope& operator=(const TransactionScope&) = delete;

private:
    WorldService& service_;
    int uncaughtAtConstruction_;
    bool active_{true};
};

void WorldService::beginTransaction() {
    TransactionFrame frame;
    frame.backup = transactionalBackup(state_, frame.backupEventCount);
    frame.backupDeferredEventCount = deferredEvents_.size();
    transactionFrames_.push_back(std::move(frame));
}

void WorldService::commitTransaction() {
    if (transactionFrames_.empty()) throw std::logic_error("transaction commit without an active scope");
    transactionFrames_.pop_back();
    if (!transactionFrames_.empty()) return;
    rebuildEntityRegistry();
    auto events = std::move(deferredEvents_);
    deferredEvents_.clear();
    for (const auto& event : events) eventBus_.publish(event);
    eventBus_.dispatch();
}

void WorldService::rollbackTransaction() {
    if (transactionFrames_.empty()) throw std::logic_error("transaction rollback without an active scope");
    auto frame = std::move(transactionFrames_.back());
    transactionFrames_.pop_back();
    restoreTransactionalBackup(state_, std::move(frame.backup), frame.backupEventCount);
    deferredEvents_.resize(std::min(frame.backupDeferredEventCount, deferredEvents_.size()));
    rebuildEntityRegistry();
}

namespace {

std::string join(const std::vector<std::string>& values, std::string_view separator) {
    std::ostringstream output;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) output << separator;
        output << values[i];
    }
    return output.str();
}

}  // namespace
WorldService::WorldService(std::unique_ptr<IProvider> provider)
    : provider_(provider ? std::move(provider) : std::make_unique<MockProvider>()),
      turnQueue_(std::make_unique<TurnQueue>(*provider_)) {}

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
    return service;
}

void WorldService::rebuildEntityRegistry() {
    entities_ = EntityRegistry{};
    const auto playerState = state_.entities.find(kPlayerId);
    if (playerState == state_.entities.end() || playerState->second.lifecycle != EntityLifecycle::Destroyed) {
        entities_.create(EntityRecord{kPlayerId, kPlayerId, playerState == state_.entities.end() ||
                                                   playerState->second.lifecycle == EntityLifecycle::Active});
    }
    if (entities_.contains(kPlayerId)) entities_.set<TransformComponent>(kPlayerId, TransformComponent{0.0, 0.0, 0.0, 0.0});
    const auto defaultLocation = content_.spatialEntities.empty() ? std::string{} : content_.spatialEntities.front().id;
    const auto regionId = state_.worldId.empty() ? content_.worldId : state_.worldId;
    if (entities_.contains(kPlayerId)) {
        entities_.set<LocationComponent>(kPlayerId, LocationComponent{defaultLocation, regionId});
        InventoryComponent inventory;
        if (const auto found = state_.inventories.find(kPlayerId); found != state_.inventories.end()) {
            inventory.quantities = found->second;
        }
        entities_.set<InventoryComponent>(kPlayerId, std::move(inventory));
    }
    for (const auto& authored : content_.spatialEntities) {
        const auto entity = spatialEntityId(authored.id);
        const auto stateEntity = state_.entities.find(entity);
        if (stateEntity != state_.entities.end() && stateEntity->second.lifecycle == EntityLifecycle::Destroyed) continue;
        const bool active = stateEntity == state_.entities.end() || stateEntity->second.lifecycle == EntityLifecycle::Active;
        if (!entities_.create(EntityRecord{entity, authored.kind, active})) continue;
        entities_.set<SpatialComponent>(entity, authored.spatial);
        entities_.set<LocationComponent>(entity, LocationComponent{authored.id, regionId});
    }
    for (const auto& authored : content_.links) {
        const auto entity = linkEntityId(authored.id);
        const auto stateEntity = state_.entities.find(entity);
        if (stateEntity != state_.entities.end() && stateEntity->second.lifecycle == EntityLifecycle::Destroyed) continue;
        const bool active = stateEntity == state_.entities.end() || stateEntity->second.lifecycle == EntityLifecycle::Active;
        if (!entities_.create(EntityRecord{entity, authored.kind, active})) continue;
        auto link = authored.link;
        if (const auto runtime = state_.links.find(authored.id); runtime != state_.links.end()) {
            link.traversable = runtime->second.traversable;
            link.locked = runtime->second.locked;
        }
        entities_.set<LinkComponent>(entity, std::move(link));
    }
    for (const auto& [id, npc] : state_.npcs) {
        const auto entity = npcEntityId(id);
        const auto stateEntity = state_.entities.find(entity);
        if (stateEntity != state_.entities.end() && stateEntity->second.lifecycle == EntityLifecycle::Destroyed) continue;
        const bool active = stateEntity == state_.entities.end() || stateEntity->second.lifecycle == EntityLifecycle::Active;
        entities_.create(EntityRecord{entity, "npc", active});
        entities_.set<NpcComponent>(entity, NpcComponent{id, npc.mode});
        entities_.set<LocationComponent>(entity, LocationComponent{npc.location, content_.worldId});
        entities_.set<RenderableComponent>(entity, RenderableComponent{"npc." + id, true});
        InventoryComponent inventory;
        if (const auto found = state_.inventories.find(entity); found != state_.inventories.end()) {
            inventory.quantities = found->second;
        }
        entities_.set<InventoryComponent>(entity, std::move(inventory));
    }
    for (const auto& [id, runtimeEntity] : state_.entities) {
        if (runtimeEntity.lifecycle == EntityLifecycle::Destroyed || entities_.contains(id)) continue;
        if (!entities_.create(EntityRecord{id, runtimeEntity.kind, runtimeEntity.lifecycle == EntityLifecycle::Active})) continue;
    }
}

void WorldService::syncNpcEntityState() {
    for (const auto& [id, npc] : state_.npcs) {
        const auto entity = npcEntityId(id);
        entities_.set<NpcComponent>(entity, NpcComponent{id, npc.mode});
        entities_.set<LocationComponent>(entity, LocationComponent{npc.location, state_.worldId});
    }
}

bool WorldService::hasNpc(const std::string& npcId) const {
    if (state_.npcs.find(npcId) == state_.npcs.end()) return false;
    const auto entity = state_.entities.find(npcEntityId(npcId));
    return entity == state_.entities.end() || entity->second.lifecycle == EntityLifecycle::Active;
}

DecisionContext WorldService::decisionContext(const std::string& npcId) const {
    if (!hasNpc(npcId)) return {};
    const auto found = state_.npcs.find(npcId);
    if (found == state_.npcs.end()) return {};
    const auto& npc = found->second;
    const auto* authoredContent = authoredNpc(npcId);
    if (authoredContent == nullptr) return {};
    const auto& authoredDefinition = authoredContent->definition;
    DecisionContext context;
    context.npcId = npcId;
    context.npcName = authoredDefinition.name;
    context.contentVersion = content_.contentVersion;
    context.promptVersion = content_.contentVersion + ":" + content_.id;
    context.now = state_.now;
    context.worldRevision = state_.revision;
    context.npcStateVersion = npc.stateVersion;
    std::ostringstream resources;
    for (const auto& [id, resourceDefinition] : content_.resources) {
        resources << id << '=' << resourceValue(id) << ' ' << resourceDefinition.unit << ';';
    }
    context.perception = "At " + npc.location + ", it is " + state_.now.toString() +
                         ". Public resources: " + resources.str();
    for (const auto& stimulus : npc.stimuli) {
        if (!stimulus.active || state_.now.minute >= stimulus.expiresAt.minute) continue;
        context.stimuli.push_back(stimulus);
    }
    std::sort(context.stimuli.begin(), context.stimuli.end(), [](const auto& left, const auto& right) {
        if (left.effectiveSalience == right.effectiveSalience) return left.id < right.id;
        return left.effectiveSalience > right.effectiveSalience;
    });
    std::ostringstream stimuli;
    for (const auto& stimulus : context.stimuli) {
        stimuli << stimulus.category << " from " << stimulus.sourceEvent << " by " << stimulus.actor
                << " salience=" << stimulus.effectiveSalience << " valence=" << stimulus.valence
                << " sequence=" << stimulus.sourceSequence << ';';
    }
    if (stimuli.str().empty()) stimuli << "none beyond the current public state;";
    context.externalStimuli = stimuli.str();
    for (const auto& [resourceId, resource] : state_.resources) {
        if (resource.amount - resource.reserved > 0) continue;
        context.constraints.push_back(StructuralConstraintView{
            "resource:" + resourceId + ":unavailable", "resource", resourceId,
            "world_resource_rule", "No unreserved units are available.", true});
    }
    for (const auto& link : content_.links) {
        const auto runtime = state_.links.find(link.id);
        if (runtime == state_.links.end() || (runtime->second.traversable && !runtime->second.locked)) continue;
        context.constraints.push_back(StructuralConstraintView{
            linkEntityId(link.id) + ":blocked", "topology", join(link.link.endpoints, ","),
            "authored_link_state", runtime->second.locked ? "The link is locked." : "The link is closed.", true});
    }
    context.constraints.push_back(StructuralConstraintView{
        "actions:authored_vocabulary", "capability", npcId, "content_action_whitelist",
        "Only the supplied authored action ids may be proposed.", true});
    std::ostringstream constraints;
    for (const auto& constraint : context.constraints) {
        constraints << constraint.id << " [" << (constraint.hard ? "hard" : "soft") << "] "
                    << constraint.reason << ';';
    }
    context.structuralConstraints = constraints.str();
    std::vector<std::string> memories;
    for (const auto& memory : npc.memories) memories.push_back(memory.text);
    context.memories = join(memories, " | ");
    std::vector<std::string> knowledge;
    for (const auto& fact : npc.knowledge) {
        knowledge.push_back(fact.subject + " " + fact.predicate + " " + fact.value +
                           " [confidence=" + std::to_string(fact.confidence) + ", scope=" + fact.scope + "]");
    }
    context.knowledge = join(knowledge, " | ");
    std::vector<std::string> experiences;
    for (const auto& experience : npc.experiences) {
        experiences.push_back(experience.domain + ": when " + experience.trigger + ", " +
                              experience.action + " -> " + experience.outcome +
                              " [confidence=" + std::to_string(experience.confidence) + "]");
    }
    context.experiences = join(experiences, " | ");
    context.character = authoredDefinition.name + ": " + authoredDefinition.role + "; public goal=" +
                        authoredDefinition.publicGoal + "; hidden goal=" + authoredDefinition.hiddenGoal +
                        "; boundary=" + authoredDefinition.boundary + "; strategy=" + npc.strategy;
    for (const auto& [needId, need] : npc.needs) {
        context.character += "; need(" + needId + ")=" + std::to_string(need.deficit);
    }
    for (const auto& [peer, relation] : npc.relationships) {
        context.character += "; relationship(" + peer + ")=trust:" + std::to_string(relation.trust) +
                             ",respect:" + std::to_string(relation.respect) +
                             ",fear:" + std::to_string(relation.fear) +
                             ",owed:" + std::to_string(relation.owed);
    }
    if (npc.activity.goal) {
        context.currentActivity = Json{{"status", toString(npc.activity.status)},
                                       {"goal_id", npc.activity.goal->id},
                                       {"intent", npc.activity.goal->intent},
                                       {"action_id", npc.activity.goal->target},
                                       {"reason", npc.activity.goal->reason},
                                       {"deadline", npc.activity.goal->deadline.toString()}};
    }
    if (npc.activity.process) {
        context.currentActivity["process_id"] = npc.activity.process->id;
        context.currentActivity["recipe_id"] = npc.activity.process->recipeId;
        context.currentActivity["plan_id"] = npc.activity.process->planId;
        context.currentActivity["progress"] = npc.activity.process->progress;
        context.currentActivity["behavior_status"] = npc.activity.process->behaviorStatus;
        context.currentActivity["completes_at"] = npc.activity.process->completesAt.toString();
    }
    for (const auto& [groupId, group] : state_.socialGroups) {
        const auto membership = group.members.find(npcId);
        if (membership == group.members.end()) continue;
        context.character += "; membership(" + groupId + ")=" + membership->second;
        for (const auto& [metric, value] : group.metrics) {
            context.character += "; group_metric(" + groupId + ":" + metric + ")=" + std::to_string(value);
        }
    }
    std::vector<NeedContent> needs;
    {
            context.candidates = authoredContent->actions;
            needs = authoredContent->needs;
            for (const auto& needDefinition : needs) {
                const auto state = npc.needs.find(needDefinition.id);
                if (state != npc.needs.end()) {
                    const double currentDrive = DriveSystem::drive(needDefinition, state->second);
                    context.internalDrives += (context.internalDrives.empty() ? "" : "; ") + needDefinition.id +
                                              "[" + needDefinition.domain + "] deficit=" +
                                              std::to_string(state->second.deficit) + " drive=" +
                                              std::to_string(currentDrive);
                    context.character += "; drive(" + needDefinition.id + ")=" +
                                         std::to_string(currentDrive);
                    context.drives.push_back(DriveView{needDefinition.id, needDefinition.domain, needDefinition.dimension,
                                                       state->second.deficit, currentDrive,
                                                       state->second.sensitivity, state->second.evidenceConfidence,
                                                       state->second.deficit >= needDefinition.criticalThreshold,
                                                       state->second.expectedRelief});
                }
            }
    }
    // Utility ranking chooses the long-term intent deterministically. The
    // provider may still choose any authored candidate after seeing this list.
    const auto ranked = GoalSelector::rank(npc, needs);
    std::vector<ActionCandidate> ordered;
    for (const auto& choice : ranked) {
        const auto candidate = std::find_if(context.candidates.begin(), context.candidates.end(),
                                            [&choice](const auto& item) { return item.actionId == choice.actionId; });
        if (candidate != context.candidates.end()) ordered.push_back(*candidate);
    }
    for (const auto& candidate : context.candidates) {
        if (std::none_of(ordered.begin(), ordered.end(), [&candidate](const auto& item) {
                return item.actionId == candidate.actionId;
            })) {
            ordered.push_back(candidate);
        }
    }
    context.candidates = std::move(ordered);
    return context;
}

void WorldService::advanceMinutes(std::int64_t minutes) {
    if (minutes <= 0) return;
    // Consume completed provider work at the world boundary. The worker never
    // runs while a WorldState transaction is open.
    pumpAsyncTurns();
    const auto target = state_.now.minute + minutes;
    while (state_.now.minute < target) {
        // A time step is one state transition. A long offline jump is a
        // sequence of committed steps, not one unbounded transaction; this
        // keeps event publication and observation aligned with the step that
        // produced the fact.
        TransactionScope transaction(*this);
        ++state_.now.minute;
        ++simulationMetrics_.simulatedMinutes;
        expireStimuli();
        std::set<std::string> dueNpcIds;
        for (auto& [id, npc] : state_.npcs) {
            const auto* authored = authoredNpc(id);
            const auto tier = authored != nullptr ? authored->simulationTier : std::string("near");
            const int interval = tier == "far" ? content_.simulationPolicy.farTickMinutes
                                                : content_.simulationPolicy.nearTickMinutes;
            if (state_.now.minute % interval != 0) {
                ++simulationMetrics_.skippedNpcTicks;
                continue;
            }
            dueNpcIds.insert(id);
            if (tier == "far") ++simulationMetrics_.farNpcTicks;
            else ++simulationMetrics_.nearNpcTicks;
            npc.lastSimulationTick = state_.now;
        }
        resumeSuspendedProcesses(dueNpcIds);
        tickProcesses(dueNpcIds);
        if (state_.now.minute % kMinutesPerHour == 0) {
            processHourBoundary(state_.now.day(), state_.now.hour(), dueNpcIds);
        }
        if (state_.now.minute % kMinutesPerDay == 0) processDayBoundary(state_.now.day(), dueNpcIds);
        applyPromiseDeadline();
        applyExchangeDeadlines();
    }
    pumpAsyncTurns();
    syncNpcEntityState();
}

void WorldService::advanceTo(GameTime target) {
    if (target.minute <= state_.now.minute) return;
    advanceMinutes(target.minute - state_.now.minute);
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
        if (npc.location != scheduledLocation) ++npc.stateVersion;
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
    processAutonomousTurns(dueNpcIds);
}

void WorldService::processActivity(const ActivityContent& activity) {
    std::map<std::string, int> required;
    for (const auto& effect : activity.effects) {
        if (effect.operation == "resource.consume" || effect.operation == "resource.reserve") {
            required[effect.resource] += effect.amount;
        }
    }
    for (const auto& [resource, amount] : required) {
        if (availableResourceValue(resource) < amount) return;
    }

    TransactionScope activityTransaction(*this);
    Json changes = Json::object();
    for (const auto& effect : activity.effects) {
        if (effect.operation == "meta_action") {
            BehaviorNodeContent node;
            node.type = "meta_action";
            node.id = activity.id + ":" + effect.metaActionId;
            node.name = effect.metaActionId;
            node.args = effect.args;
            if (!applyMetaAction(activity.actor, effect.metaActionId, node, nullptr)) {
                activityTransaction.rollback();
                addEvent("activity.failed", activity.actor, activity.target,
                         "A scheduled activity was rejected by a registered meta action",
                         Json{{"activity_id", activity.id}, {"meta_action", effect.metaActionId}});
                return;
            }
            continue;
        }
        if (effect.operation == "need.relieve") {
            if (activity.actor != kWorldActor) applyNeedRelief(activity.actor, effect.effectAction);
            continue;
        }
        if (effect.operation == "link.set_state") {
            state_.links[effect.linkId] = RuntimeLinkState{effect.traversable, effect.locked};
            changes[effect.linkId] = Json{{"traversable", effect.traversable}, {"locked", effect.locked}};
            continue;
        }
        const int before = resourceValue(effect.resource);
        if (effect.operation == "resource.consume") setResource(effect.resource, before - effect.amount);
        if (effect.operation == "resource.produce") setResource(effect.resource, before + effect.amount);
        if (effect.operation == "resource.reserve") state_.resources[effect.resource].reserved += effect.amount;
        if (effect.operation == "resource.release") {
            auto& resource = state_.resources[effect.resource];
            resource.reserved = std::max(0, resource.reserved - effect.amount);
        }
        if (effect.operation != "resource.release") {
            changes[effect.resource] = Json{{"before", before}, {"after", resourceValue(effect.resource)}};
        }
    }
    addEvent(activity.eventType, activity.actor, activity.target, activity.detail,
             Json{{"activity_id", activity.id}, {"changes", changes}});
}

void WorldService::tickProcesses(const std::set<std::string>& dueNpcIds) {
    // Completion may run a nested recipe savepoint. A failed recipe restores
    // WorldState and invalidates references/iterators into the NPC map, so
    // collect durable work items before invoking code that may roll back.
    std::vector<std::pair<std::string, ProcessExecution>> due;
    for (const auto& [npcId, npc] : state_.npcs) {
        if (!dueNpcIds.contains(npcId)) continue;
        if (npc.activity.status != ActivityStatus::Active ||
            !npc.activity.process || npc.activity.process->status != ProcessStatus::Running) {
            continue;
        }
        auto process = *npc.activity.process;
        const auto duration = std::max<std::int64_t>(1, process.completesAt.minute - process.startedAt.minute);
        process.progress = std::clamp(static_cast<double>(state_.now.minute - process.startedAt.minute) /
                                          static_cast<double>(duration),
                                      0.0, 1.0);
        double interruptSalience = 0.0;
        for (const auto& stimulus : npc.stimuli) {
            if (!stimulus.active || stimulus.observedAt.minute <= process.startedAt.minute) continue;
            interruptSalience = std::max(interruptSalience, stimulus.effectiveSalience);
        }
        if (process.interruptible &&
            interruptSalience >= std::max(process.interruptSalienceThreshold,
                                          content_.npcDeliberation.minInterruptSalience)) {
            state_.npcs.at(npcId).activity.process->progress = process.progress;
            suspendProcess(npcId, state_.npcs.at(npcId).activity.process.value(),
                           "new_high_salience_stimulus");
            continue;
        }
        if (state_.now.minute < process.completesAt.minute) continue;
        due.emplace_back(npcId, process);
    }
    for (const auto& [npcId, process] : due) {
        completeProcess(npcId, process);
    }
}

void WorldService::resumeSuspendedProcesses(const std::set<std::string>& dueNpcIds) {
    for (auto& [npcId, npc] : state_.npcs) {
        if (!dueNpcIds.contains(npcId)) continue;
        if (npc.activity.status != ActivityStatus::Suspended || !npc.activity.process) continue;
        auto& process = *npc.activity.process;
        double highestSalience = 0.0;
        for (const auto& stimulus : npc.stimuli) {
            if (stimulus.active && state_.now.minute < stimulus.expiresAt.minute) {
                highestSalience = std::max(highestSalience, stimulus.effectiveSalience);
            }
        }
        if (highestSalience >= std::max(process.interruptSalienceThreshold,
                                        content_.npcDeliberation.minInterruptSalience)) continue;
        bool resourcesAvailable = true;
        for (const auto& [resource, amount] : process.requiredResources) {
            if (availableResourceValue(resource) < amount) {
                resourcesAvailable = false;
                break;
            }
        }
        if (!resourcesAvailable) continue;
        for (const auto& [resource, amount] : process.requiredResources) {
            state_.resources[resource].reserved += amount;
            process.reservedResources[resource] = amount;
        }
        const std::string previousReason = process.interruptionReason;
        if (process.suspendedAt.minute >= 0) {
            process.completesAt.minute += state_.now.minute - process.suspendedAt.minute;
        }
        process.suspendedAt = GameTime{-1};
        process.interruptionReason.clear();
        process.behaviorStatus = "running";
        setActivityStatus(npc.activity, ActivityStatus::Active);
        applyNpcSignal(npc, NpcSignal::StartProcess);
        addEvent("npc.process_resumed", npcId, process.actionId,
                 "A suspended process resumed after its interruption cleared",
                 Json{{"process_id", process.id}, {"action_id", process.actionId},
                      {"previous_reason", previousReason},
                      {"completes_at", process.completesAt.minute}});
    }
}

void WorldService::releaseReservations(ProcessExecution& process) {
    for (const auto& [resource, amount] : process.reservedResources) {
        auto found = state_.resources.find(resource);
        if (found != state_.resources.end()) found->second.reserved = std::max(0, found->second.reserved - amount);
    }
    process.reservedResources.clear();
}

void WorldService::suspendProcess(const std::string& npcId, ProcessExecution& process,
                                  const std::string& reason, Json eventData) {
    releaseReservations(process);
    process.status = ProcessStatus::Suspended;
    process.behaviorStatus = "suspended";
    process.suspendedAt = state_.now;
    process.interruptionReason = reason;
    if (auto found = state_.npcs.find(npcId); found != state_.npcs.end() &&
        found->second.activity.goal && found->second.activity.goal->id == process.goalId) {
        setActivityStatus(found->second.activity, ActivityStatus::Suspended);
        applyNpcSignal(found->second, NpcSignal::CancelProcess);
    }
    eventData["process_id"] = process.id;
    eventData["action_id"] = process.actionId;
    eventData["reason"] = reason;
    eventData["progress"] = process.progress;
    addEvent("npc.process_suspended", npcId, process.actionId,
             reason == "resource_preempted"
                 ? "A lower-priority process released reservations for a higher-priority process"
                 : "A process was suspended by a high-salience stimulus",
             std::move(eventData));
}

std::optional<std::vector<std::string>> WorldService::planResourcePreemption(
    const std::string& requesterId, const std::map<std::string, int>& requirements,
    int requesterPriority) const {
    std::map<std::string, int> remaining;
    for (const auto& [resource, amount] : requirements) {
        remaining[resource] = std::max(0, amount - availableResourceValue(resource));
    }
    const bool alreadyAvailable = std::all_of(remaining.begin(), remaining.end(),
                                              [](const auto& entry) { return entry.second == 0; });
    if (alreadyAvailable) return std::vector<std::string>{};

    struct Candidate {
        const std::string* npcId;
        int priority;
    };
    std::vector<Candidate> candidates;
    for (const auto& [npcId, npc] : state_.npcs) {
        if (npcId == requesterId || !npc.activity.process ||
            npc.activity.process->status != ProcessStatus::Running ||
            !npc.activity.process->interruptible ||
            npc.activity.process->priority >= requesterPriority) {
            continue;
        }
        const bool ownsNeededReservation = std::any_of(
            remaining.begin(), remaining.end(), [&npc](const auto& entry) {
                const auto reserved = npc.activity.process->reservedResources.find(entry.first);
                return entry.second > 0 && reserved != npc.activity.process->reservedResources.end() &&
                       reserved->second > 0;
            });
        if (ownsNeededReservation) candidates.push_back(Candidate{&npcId, npc.activity.process->priority});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.priority != right.priority) return left.priority < right.priority;
        return *left.npcId < *right.npcId;
    });

    std::vector<std::string> victims;
    for (const auto& candidate : candidates) {
        const auto& process = state_.npcs.at(*candidate.npcId).activity.process.value();
        bool contributes = false;
        for (const auto& [resource, amount] : process.reservedResources) {
            auto need = remaining.find(resource);
            if (need == remaining.end() || need->second <= 0 || amount <= 0) continue;
            contributes = true;
            need->second = std::max(0, need->second - amount);
        }
        if (contributes) victims.push_back(*candidate.npcId);
        if (std::all_of(remaining.begin(), remaining.end(),
                        [](const auto& entry) { return entry.second == 0; })) {
            return victims;
        }
    }
    return std::nullopt;
}

void WorldService::cancelProcess(const std::string& npcId, ProcessExecution& process,
                                 const std::string& reason) {
    releaseReservations(process);
    process.behaviorStatus = "cancelled";
    process.status = ProcessStatus::Cancelled;
    process.interruptionReason = reason;
    if (auto found = state_.npcs.find(npcId); found != state_.npcs.end() && found->second.activity.goal &&
        found->second.activity.goal->id == process.goalId) {
        setActivityStatus(found->second.activity, ActivityStatus::Cancelled);
        applyNpcSignal(found->second, NpcSignal::CancelProcess);
    }
    addEvent("npc.process_interrupted", npcId, process.actionId,
             "A committed process was interrupted before completion",
             Json{{"process_id", process.id}, {"action_id", process.actionId},
                  {"reason", reason}, {"progress", process.progress}, {"status", toString(process.status)}});
}

void WorldService::completeProcess(const std::string& npcId, ProcessExecution process) {
    if (!state_.npcs.contains(npcId)) return;
    const bool applied = applyActionEffects(npcId, process.recipeId.empty() ? process.actionId : process.recipeId,
                                            &process.reservedResources);
    process.progress = 1.0;
    process.status = applied ? ProcessStatus::Completed : ProcessStatus::Failed;
    process.behaviorStatus = applied ? "success" : "failure";
    if (!applied) releaseReservations(process);
    addEvent(applied ? "npc.process_completed" : "npc.process_failed", npcId, process.actionId,
             applied ? "A committed process completed and its effects were applied"
                     : "A committed process could not satisfy its completion preconditions",
             Json{{"process_id", process.id}, {"action_id", process.actionId},
                  {"started_at", process.startedAt.minute}, {"completed_at", state_.now.minute},
                  {"progress", process.progress}, {"status", toString(process.status)}});
    // A failed recipe restores a nested savepoint and invalidates iterators
    // into WorldState. Reacquire the NPC after the recipe has returned.
    auto found = state_.npcs.find(npcId);
    if (found == state_.npcs.end()) return;
    if (found->second.activity.process && found->second.activity.process->id == process.id) {
        found->second.activity.process = process;
        setActivityStatus(found->second.activity,
                          applied ? ActivityStatus::Completed : ActivityStatus::Failed);
    }
    applyNpcSignal(found->second, NpcSignal::FinishProcess);
    const auto* authored = authoredNpc(npcId);
    applyNpcSignal(found->second, authored != nullptr && found->second.location == authored->definition.home
                                      ? NpcSignal::AtHome
                                      : NpcSignal::AtWork);
    found->second.activity = NpcActivity{};
    ++found->second.stateVersion;
}

void WorldService::processAutonomousTurns(const std::set<std::string>& dueNpcIds) {
    if (!content_.npcDeliberation.enabled) return;
    pumpAsyncTurns();
    for (auto& [npcId, npc] : state_.npcs) {
        if (!dueNpcIds.contains(npcId)) continue;
        if (!hasNpc(npcId) || npc.activity.status == ActivityStatus::Active) continue;
        if (npc.lastAutonomousTurn.minute >= 0 &&
            state_.now.minute - npc.lastAutonomousTurn.minute < content_.npcDeliberation.cooldownMinutes) {
            continue;
        }
        const auto context = decisionContext(npcId);
        double highestStimulus = 0.0;
        for (const auto& stimulus : context.stimuli) {
            highestStimulus = std::max(highestStimulus, stimulus.effectiveSalience);
        }
        double highestDrive = 0.0;
        for (const auto& drive : context.drives) highestDrive = std::max(highestDrive, drive.drive);
        if (highestStimulus < content_.npcDeliberation.minStimulusSalience &&
            highestDrive < content_.npcDeliberation.minDrivePressure) {
            continue;
        }
        // The request captures the current decision context and returns to the
        // main thread later. Marking the scheduling time prevents an hourly
        // boundary from enqueueing the same NPC repeatedly while the model is
        // still thinking.
        enqueueAutonomousTurn(npcId, "high_salience_context");
    }
}

void WorldService::processDayBoundary(int day, const std::set<std::string>& dueNpcIds) {
    for (const auto& activity : content_.activities) {
        if (activity.trigger != "day") continue;
        if (!activity.conditionResource.empty() && resourceValue(activity.conditionResource) >= activity.conditionBelow) continue;
        processActivity(activity);
    }
    for (auto& [id, npc] : state_.npcs) {
        if (!dueNpcIds.contains(id)) continue;
        const auto entity = state_.entities.find(npcEntityId(id));
        if (entity != state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        ++npc.stateVersion;
        npc.memories.push_back(Memory{"episode", "A day passed while the world kept working.", state_.now, 1.0});
    }
    reviseNpcStrategies(day);
    Json resources = Json::object();
    for (const auto& [id, resource] : state_.resources) {
        resources[id] = Json{{"amount", resource.amount}, {"reserved", resource.reserved}};
    }
    addEvent("life.day", kWorldActor, "instance", "The world completes another day",
             Json{{"day", day}, {"resources", resources}});
}

void WorldService::reviseNpcStrategies(int day) {
    for (const auto& rule : content_.strategyRules) {
        auto found = state_.npcs.find(rule.npcId);
        if (found == state_.npcs.end() || found->second.strategy == rule.strategy) continue;
        const auto trust = found->second.relationships.find(rule.peerId);
        if (trust == found->second.relationships.end() || trust->second.trust < rule.minPeerTrust) continue;
        auto& npc = found->second;
        npc.strategy = rule.strategy;
        npc.goalStage = std::max(npc.goalStage, rule.goalStage);
        npc.memories.push_back(Memory{"plan", rule.memory, state_.now, trust->second.trust});
        ++npc.stateVersion;
        addEvent(rule.eventType, rule.npcId, rule.peerId, rule.memory,
                 Json{{"strategy", npc.strategy}, {"rule", rule.id}, {"peer_trust", trust->second.trust}, {"day", day}});
    }
}

void WorldService::applyPromiseDeadline() {
    for (auto& [id, promise] : state_.promises) {
        if ((promise.status == PromiseStatus::Accepted || promise.status == PromiseStatus::Renegotiated) &&
            state_.now.minute >= promise.due.minute) {
            promise.status = PromiseStatus::Broken;
            ++promise.version;
            auto from = state_.npcs.find(promise.from);
            auto to = state_.npcs.find(promise.to);
            if (from != state_.npcs.end()) {
                applyRelationshipDelta(from->second.relationships[promise.to], content_.promisePolicy.brokenPromiser);
                ++from->second.stateVersion;
            }
            if (to != state_.npcs.end()) {
                applyRelationshipDelta(to->second.relationships[promise.from], content_.promisePolicy.brokenPromisee);
                ++to->second.stateVersion;
            }
            addEvent("promise.broken", promise.from, promise.to, "A promise reached its deadline", Json{{"promise_id", id}});
        }
    }
}

void WorldService::applyExchangeDeadlines() {
    for (auto& [id, contract] : state_.exchangeContracts) {
        if ((contract.status != ExchangeContractStatus::Proposed &&
             contract.status != ExchangeContractStatus::Accepted) ||
            state_.now.minute < contract.due.minute) {
            continue;
        }
        contract.status = ExchangeContractStatus::Expired;
        ++contract.version;
        contract.reason = "deadline_reached";
        addEvent("exchange.expired", kWorldActor, id,
                 "An exchange contract reached its deadline before settlement",
                 Json{{"contract_id", id}, {"proposer", contract.proposer},
                      {"counterparty", contract.counterparty}, {"due", contract.due.minute}});
    }
}

CommandResult WorldService::proposeExchange(const std::string& proposer,
                                            const std::string& counterparty,
                                            std::map<std::string, int> offered,
                                            std::map<std::string, int> requested,
                                            int dueInMinutes, const std::string& actor,
                                            const std::string& turnId) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The exchange proposal was already applied.");
    }
    const auto proposerId = canonicalActorId(state_, proposer);
    const auto counterpartyId = canonicalActorId(state_, counterparty);
    const auto actorId = canonicalActorId(state_, actor);
    if (proposerId.empty() || counterpartyId.empty() || proposerId == counterpartyId) {
        return reject("unknown_exchange_party", "An exchange needs two distinct active parties.");
    }
    if (actorId != kWorldActor && actorId != proposerId) {
        return reject("unauthorized", "Only the proposer or world authority may create this exchange.");
    }
    if (!actorIsActive(state_, proposerId) || !actorIsActive(state_, counterpartyId)) {
        return reject("inactive_exchange_party", "Both exchange parties must be active.");
    }
    if (dueInMinutes <= 0) {
        return reject("invalid_exchange_deadline", "An exchange deadline must be positive.");
    }
    const auto validTerms = [this](const std::map<std::string, int>& terms) {
        return std::all_of(terms.begin(), terms.end(), [this](const auto& term) {
            return content_.resources.contains(term.first) && term.second > 0;
        });
    };
    if (offered.empty() || requested.empty() || !validTerms(offered) || !validTerms(requested)) {
        return reject("invalid_exchange_terms",
                      "An exchange needs positive terms on both sides using known resources.");
    }
    std::size_t sequence = state_.exchangeContracts.size() + 1;
    std::string id = "exchange-" + std::to_string(sequence);
    while (state_.exchangeContracts.contains(id)) {
        id = "exchange-" + std::to_string(++sequence);
    }
    ExchangeContract contract{id, proposerId, counterpartyId, std::move(offered), std::move(requested),
                              state_.now, GameTime{state_.now.minute + dueInMinutes}, GameTime{-1},
                              ExchangeContractStatus::Proposed, 0, "awaiting_counterparty"};
    state_.exchangeContracts.emplace(id, contract);
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "exchange.proposed", proposerId, counterpartyId,
        "An actor proposed an atomic two-party exchange",
        Json{{"contract_id", id}, {"offered", quantityMapJson(contract.offered)},
             {"requested", quantityMapJson(contract.requested)}, {"due", contract.due.minute}});
    return CommandResult{true, "ok", id, event};
}

CommandResult WorldService::decideExchange(const std::string& contractId,
                                           ExchangeContractStatus decision,
                                           const std::string& actor,
                                           const std::string& turnId,
                                           const std::string& reason) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The exchange transition was already applied.");
    }
    const auto found = state_.exchangeContracts.find(contractId);
    if (found == state_.exchangeContracts.end()) return reject("unknown_exchange", "That exchange does not exist.");
    auto& contract = found->second;
    const auto proposer = contract.proposer;
    const auto counterparty = contract.counterparty;
    if (state_.now.minute >= contract.due.minute || contract.status == ExchangeContractStatus::Expired) {
        return reject("exchange_expired", "The exchange deadline has passed.");
    }
    const auto actorId = canonicalActorId(state_, actor);
    if (actorId.empty()) return reject("unauthorized", "The actor is not a party to this exchange.");
    const bool authority = actorId == kWorldActor;
    const bool isProposer = actorId == contract.proposer;
    const bool isCounterparty = actorId == contract.counterparty;
    if (!authority && !isProposer && !isCounterparty) {
        return reject("unauthorized", "The actor is not a party to this exchange.");
    }
    if ((decision == ExchangeContractStatus::Accepted || decision == ExchangeContractStatus::Rejected) &&
        !authority && !isCounterparty) {
        return reject("counterparty_required", "Only the counterparty may accept or reject an exchange.");
    }
    if ((decision == ExchangeContractStatus::Accepted || decision == ExchangeContractStatus::Rejected) &&
        (!actorIsActive(state_, contract.proposer) || !actorIsActive(state_, contract.counterparty))) {
        return reject("inactive_exchange_party", "Both exchange parties must be active to decide a proposal.");
    }
    if (decision == ExchangeContractStatus::Cancelled && !authority &&
        !actorIsActive(state_, actorId)) {
        return reject("inactive_exchange_party", "An inactive exchange party cannot cancel a contract.");
    }
    if (decision == ExchangeContractStatus::Cancelled && !authority &&
        contract.status != ExchangeContractStatus::Proposed &&
        contract.status != ExchangeContractStatus::Accepted) {
        return reject("invalid_exchange_transition", "Only a live exchange can be cancelled.");
    }
    if (decision == ExchangeContractStatus::Accepted && contract.status == ExchangeContractStatus::Proposed) {
        contract.status = ExchangeContractStatus::Accepted;
    } else if (decision == ExchangeContractStatus::Rejected && contract.status == ExchangeContractStatus::Proposed) {
        contract.status = ExchangeContractStatus::Rejected;
    } else if (decision == ExchangeContractStatus::Cancelled &&
               (contract.status == ExchangeContractStatus::Proposed ||
                contract.status == ExchangeContractStatus::Accepted)) {
        contract.status = ExchangeContractStatus::Cancelled;
    } else {
        return reject("invalid_exchange_transition", "The requested exchange transition is not legal.");
    }
    contract.reason = reason;
    ++contract.version;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto eventType = decision == ExchangeContractStatus::Accepted ? "exchange.accepted"
                          : decision == ExchangeContractStatus::Rejected ? "exchange.rejected"
                                                                          : "exchange.cancelled";
    const auto event = addEvent(eventType, actorId, contractId,
                                reason.empty() ? "An exchange lifecycle state changed" : reason,
                                Json{{"contract_id", contractId}, {"status", toString(contract.status)}});
    return CommandResult{true, "ok", contractId, event};
}

CommandResult WorldService::settleExchange(const std::string& contractId,
                                           const std::string& actor,
                                           const std::string& turnId) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The exchange settlement was already applied.");
    }
    const auto found = state_.exchangeContracts.find(contractId);
    if (found == state_.exchangeContracts.end()) return reject("unknown_exchange", "That exchange does not exist.");
    auto& contract = found->second;
    const auto proposer = contract.proposer;
    const auto counterparty = contract.counterparty;
    if (contract.status != ExchangeContractStatus::Accepted) {
        return reject("invalid_exchange_transition", "Only an accepted exchange can settle.");
    }
    if (state_.now.minute >= contract.due.minute) {
        return reject("exchange_expired", "The exchange deadline has passed.");
    }
    const auto actorId = canonicalActorId(state_, actor);
    if (actorId.empty()) return reject("unauthorized", "The actor is not a party to this exchange.");
    if (actorId != kWorldActor && actorId != contract.proposer && actorId != contract.counterparty) {
        return reject("unauthorized", "The actor is not a party to this exchange.");
    }
    if (!actorIsActive(state_, proposer) || !actorIsActive(state_, counterparty)) {
        return reject("inactive_exchange_party", "Both exchange parties must be active to settle.");
    }
    for (const auto& [resource, amount] : contract.offered) {
        const auto transfer = transferResource(proposer, counterparty, resource, amount, actorId);
        if (!transfer.accepted) {
            transaction.rollback();
            addEvent("exchange.settlement_rejected", actorId, contractId,
                     "An accepted exchange could not deliver both sides",
                     Json{{"contract_id", contractId}, {"code", transfer.code},
                          {"message", transfer.message}, {"proposer", proposer},
                          {"counterparty", counterparty}});
            return reject(transfer.code, transfer.message);
        }
    }
    for (const auto& [resource, amount] : contract.requested) {
        const auto transfer = transferResource(counterparty, proposer, resource, amount, actorId);
        if (!transfer.accepted) {
            transaction.rollback();
            addEvent("exchange.settlement_rejected", actorId, contractId,
                     "An accepted exchange could not deliver both sides",
                     Json{{"contract_id", contractId}, {"code", transfer.code},
                          {"message", transfer.message}, {"proposer", proposer},
                          {"counterparty", counterparty}});
            return reject(transfer.code, transfer.message);
        }
    }
    contract.status = ExchangeContractStatus::Settled;
    contract.settledAt = state_.now;
    contract.reason = "settled";
    ++contract.version;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "exchange.settled", actorId, contractId,
        "Both parties delivered their contractual resources atomically",
        Json{{"contract_id", contractId}, {"proposer", contract.proposer},
             {"counterparty", contract.counterparty}, {"offered", quantityMapJson(contract.offered)},
             {"requested", quantityMapJson(contract.requested)}});
    return CommandResult{true, "ok", contractId, event};
}

CommandResult WorldService::reject(std::string code, std::string message) const {
    return CommandResult{false, std::move(code), std::move(message), std::nullopt};
}

CommandResult WorldService::submit(const Command& command) {
    if (command.expectedWorldRevision != 0 && command.expectedWorldRevision != state_.revision) {
        return reject("state_conflict", "The world changed before this command was committed.");
    }
    if (command.expectedNpcStateVersion != 0 && hasNpc(command.actor) &&
        command.expectedNpcStateVersion != state_.npcs.at(command.actor).stateVersion) {
        return reject("state_conflict", "The NPC changed before this command was committed.");
    }
    if (!command.turnId.empty() && state_.processedTurnIds.contains(command.turnId)) {
        return reject("duplicate_turn", "The turn was already applied.");
    }
    if (command.kind == "advance") {
        std::int64_t minutes = 0;
        try {
            minutes = std::stoll(command.detail);
        } catch (...) {
            return reject("invalid_duration", "Advance duration must be minutes.");
        }
        if (minutes <= 0) return reject("invalid_duration", "Advance duration must be positive minutes.");
        advanceMinutes(minutes);
        if (!command.turnId.empty()) {
            TransactionScope transaction(*this);
            state_.processedTurnIds.insert(command.turnId);
        }
        return CommandResult{true, "ok", "World advanced.", std::nullopt};
    }
    if (command.kind == "promise_accept" || command.kind == "promise_refuse" ||
        command.kind == "promise_renegotiate" || command.kind == "promise_fulfill" || command.kind == "promise_break") {
        PromiseStatus status = PromiseStatus::Accepted;
        if (command.kind == "promise_refuse") status = PromiseStatus::Refused;
        if (command.kind == "promise_renegotiate") status = PromiseStatus::Renegotiated;
        if (command.kind == "promise_fulfill") status = PromiseStatus::Fulfilled;
        if (command.kind == "promise_break") status = PromiseStatus::Broken;
        return decidePromise(command.promiseId, status, command.actor, command.turnId, command.detail);
    }
    if (command.kind == "entity_create") {
        return createEntity(command.target, command.detail, Json::object(), command.turnId);
    }
    if (command.kind == "entity_activate" || command.kind == "entity_dormant" || command.kind == "entity_destroy") {
        const auto lifecycle = command.kind == "entity_activate" ? EntityLifecycle::Active
                              : command.kind == "entity_dormant" ? EntityLifecycle::Dormant
                                                                  : EntityLifecycle::Destroyed;
        return setEntityLifecycle(command.target, lifecycle, command.turnId);
    }
    if (command.kind == "link_open" || command.kind == "link_close" ||
        command.kind == "link_lock" || command.kind == "link_unlock") {
        const auto state = state_.links.find(command.target);
        if (state == state_.links.end()) return reject("unknown_link", "That link does not exist.");
        const bool traversable = command.kind == "link_open" ? true
                              : command.kind == "link_close" ? false : state->second.traversable;
        const bool locked = command.kind == "link_lock" ? true
                          : command.kind == "link_unlock" ? false : state->second.locked;
        return setLinkState(command.target, traversable, locked, command.turnId);
    }
    if (command.kind == "resource_transfer") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto source = args.value("source", command.actor);
            const auto target = args.value("target", command.target);
            const auto resource = args.value("resource", std::string{});
            const auto amount = args.value("amount", 0);
            return transferResource(source, target, resource, amount, command.actor, command.turnId);
        } catch (...) {
            return reject("invalid_resource_transfer", "Resource transfer detail must be valid JSON.");
        }
    }
    if (command.kind == "npc_move") {
        return moveNpc(command.actor, command.target, command.actor, command.turnId);
    }
    if (command.kind == "security") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto operation = args.value("operation", std::string{});
            const auto target = args.value("target", command.target);
            if (operation.empty()) return reject("invalid_security_operator", "Security command needs an operation.");
            return applySecurityOperator(operation, command.actor, target, args, command.turnId);
        } catch (...) {
            return reject("invalid_security_operator", "Security command detail must be valid JSON.");
        }
    }
    if (command.kind == "knowledge_assert" || command.kind == "experience_record") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto npcId = args.value("npc", command.target);
            const auto sourceSequence = args.value("source_sequence", 0ULL);
            const auto confidence = args.value("confidence", 0.0);
            if (command.kind == "knowledge_assert") {
                return assertKnownFact(npcId, args.value("subject", std::string{}),
                                       args.value("predicate", std::string{}),
                                       args.value("value", std::string{}), sourceSequence,
                                       args.value("scope", "direct"), confidence,
                                       command.actor, command.turnId);
            }
            return recordExperience(npcId, args.value("domain", std::string{}),
                                    args.value("trigger", std::string{}),
                                    args.value("action", std::string{}),
                                    args.value("outcome", std::string{}), sourceSequence,
                                    confidence, args.value("personal", true),
                                    command.actor, command.turnId);
        } catch (...) {
            return reject("invalid_knowledge_action", "Knowledge detail must be valid JSON.");
        }
    }
    if (command.kind == "relationship_adjust") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto delta = RelationshipAxes{args.value("trust", 0.0), args.value("respect", 0.0),
                                                args.value("fear", 0.0), args.value("owed", 0.0)};
            return adjustRelationship(args.value("npc", command.actor), args.value("target", command.target),
                                      delta, command.actor, command.turnId, args.value("reason", ""));
        } catch (...) {
            return reject("invalid_relationship", "Relationship detail must be valid JSON.");
        }
    }
    if (command.kind == "group_metric") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            return adjustGroupMetric(args.value("group", command.target), args.value("metric", ""),
                                     args.value("delta", 0.0), command.actor, command.turnId,
                                     args.value("reason", ""));
        } catch (...) {
            return reject("invalid_group_metric", "Group metric detail must be valid JSON.");
        }
    }
    if (command.kind == "group_membership") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            return setGroupMembership(args.value("group", command.target), args.value("npc", command.actor),
                                      args.value("role", "member"), args.value("active", true),
                                      command.actor, command.turnId);
        } catch (...) {
            return reject("invalid_group_membership", "Group membership detail must be valid JSON.");
        }
    }
    if (command.kind == "trade") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            return tradeResources(args.value("seller", command.actor), args.value("buyer", command.target),
                                  args.value("seller_resource", ""), args.value("seller_amount", 0),
                                  args.value("buyer_resource", ""), args.value("buyer_amount", 0),
                                  command.actor, command.turnId);
        } catch (...) {
            return reject("invalid_trade", "Trade detail must be valid JSON.");
        }
    }
    if (command.kind == "exchange_propose") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto parseTerms = [](const Json& value) {
                std::map<std::string, int> terms;
                if (!value.is_object()) throw std::runtime_error("exchange terms must be objects");
                for (auto it = value.begin(); it != value.end(); ++it) {
                    if (!it.value().is_number_integer()) throw std::runtime_error("exchange quantities must be integers");
                    terms[it.key()] = it.value().get<int>();
                }
                return terms;
            };
            const auto proposer = args.value("proposer", command.actor);
            return proposeExchange(proposer, args.value("counterparty", command.target),
                                   parseTerms(args.value("offered", Json::object())),
                                   parseTerms(args.value("requested", Json::object())),
                                   args.value("due_minutes", 0), command.actor, command.turnId);
        } catch (...) {
            return reject("invalid_exchange", "Exchange proposal detail must contain valid JSON terms.");
        }
    }
    if (command.kind == "exchange_transition") {
        try {
            const Json args = command.detail.empty() ? Json::object() : Json::parse(command.detail);
            const auto status = exchangeContractStatusFromString(args.value("status", ""));
            if (status == ExchangeContractStatus::Proposed || status == ExchangeContractStatus::Settled ||
                status == ExchangeContractStatus::Expired) {
                return reject("invalid_exchange_transition", "Use accept, reject or cancel for a transition.");
            }
            return decideExchange(args.value("contract_id", command.target), status, command.actor,
                                  command.turnId, args.value("reason", ""));
        } catch (...) {
            return reject("invalid_exchange", "Exchange transition detail must be valid JSON.");
        }
    }
    if (command.kind == "exchange_settle") {
        return settleExchange(command.target, command.actor, command.turnId);
    }
    return reject("unknown_command", "The command is not part of the registered action vocabulary.");
}

CommandResult WorldService::createPromise(const std::string& from, const std::string& to,
                                           const std::string& description, int dueInDays,
                                           const std::string& turnId) {
    TransactionScope transaction(*this);
    const bool validFrom = from == kPlayerId || hasNpc(from);
    const bool validTo = to == kPlayerId || hasNpc(to);
    if (!validFrom || !validTo || from == to) return reject("unknown_actor", "Both promise parties must be distinct.");
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The turn was already applied.");
    const std::string id = "promise-" + std::to_string(state_.promises.size() + 1);
    Promise promise{id, from, to, description,
                    GameTime{state_.now.minute + static_cast<std::int64_t>(std::max(1, dueInDays)) * kMinutesPerDay},
                    PromiseStatus::Proposed, 0};
    state_.promises.emplace(id, promise);
    if (hasNpc(from)) state_.npcs.at(from).openPromises.push_back(id);
    if (hasNpc(to)) state_.npcs.at(to).openPromises.push_back(id);
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent("promise.created", from, to, description,
                                Json{{"promise_id", id}, {"due", promise.due.minute}});
    return CommandResult{true, "ok", id, event};
}

CommandResult WorldService::decidePromise(const std::string& promiseId, PromiseStatus decision,
                                          const std::string& actor, const std::string& turnId,
                                          const std::string& detail) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The turn was already applied.");
    auto found = state_.promises.find(promiseId);
    if (found == state_.promises.end()) return reject("unknown_promise", "That promise does not exist.");
    auto& promise = found->second;
    if (actor != kPlayerId && actor != promise.from && actor != promise.to) {
        return reject("unauthorized", "Only a promise party can change it.");
    }
    if (promise.status == PromiseStatus::Broken || promise.status == PromiseStatus::Refused || promise.status == PromiseStatus::Fulfilled) {
        return reject("stale_promise", "That promise is already closed.");
    }
    if (decision == PromiseStatus::Renegotiated) {
        promise.due.minute += content_.promisePolicy.renegotiationExtensionMinutes;
        promise.status = PromiseStatus::Renegotiated;
    } else {
        promise.status = decision;
    }
    ++promise.version;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    if (auto npc = state_.npcs.find(actor); npc != state_.npcs.end()) {
        ++npc->second.stateVersion;
        npc->second.memories.push_back(Memory{"promise", detail.empty() ? "A promise changed status." : detail, state_.now, 1.0});
        const auto counterparty = actor == promise.from ? promise.to : promise.from;
        if (decision == PromiseStatus::Refused || decision == PromiseStatus::Broken) {
            applyRelationshipDelta(npc->second.relationships[counterparty], content_.promisePolicy.refusedActor);
        } else {
            applyRelationshipDelta(npc->second.relationships[counterparty], content_.promisePolicy.acceptedActor);
        }
    }
    const auto event = addEvent(
        "promise." + toString(decision), actor, promiseId, detail.empty() ? "Promise status changed" : detail,
        Json{{"promise_id", promiseId}, {"status", toString(decision)}, {"version", promise.version}});
    return CommandResult{true, "ok", toString(decision), event};
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
    if (amount <= 0 || !content_.resources.contains(resource)) {
        return reject("invalid_resource_transfer", "A transfer needs a known resource and a positive amount.");
    }
    const auto resolve = [this](const std::string& value) -> std::string {
        if (value == "world" || value == kWorldActor) return kWorldActor;
        if (value == kPlayerId && state_.entities.contains(kPlayerId)) return kPlayerId;
        if (state_.entities.contains(value)) return value;
        if (state_.npcs.contains(value)) return npcEntityId(value);
        return {};
    };
    const auto sourceId = resolve(source);
    const auto targetId = resolve(target);
    if (sourceId.empty() || targetId.empty()) return reject("unknown_transfer_party", "Both transfer parties must be active entities or the world pool.");
    if (sourceId == targetId) return reject("invalid_resource_transfer", "A resource cannot be transferred to itself.");
    if (actor != kWorldActor && actor != kPlayerId && actor != sourceId && actor != targetId &&
        actor != source && actor != target) {
        return reject("unauthorized", "The actor is not a party to this transfer.");
    }
    const auto activeEntity = [this](const std::string& id) {
        if (id == kWorldActor) return true;
        const auto found = state_.entities.find(id);
        return found != state_.entities.end() && found->second.lifecycle == EntityLifecycle::Active;
    };
    if (!activeEntity(sourceId) || !activeEntity(targetId)) return reject("inactive_transfer_party", "A transfer party is not active.");
    const auto balance = [this, &resource](const std::string& id) {
        if (id == kWorldActor) return availableResourceValue(resource);
        const auto inventory = state_.inventories.find(id);
        if (inventory == state_.inventories.end()) return 0;
        const auto quantity = inventory->second.find(resource);
        return quantity == inventory->second.end() ? 0 : quantity->second;
    };
    if (balance(sourceId) < amount) return reject("insufficient_resource", "The source does not have enough unreserved resource.");
    if (targetId == kWorldActor) {
        const auto& definition = content_.resources.at(resource);
        if (resourceValue(resource) + amount > definition.maximum) {
            return reject("resource_capacity", "The destination resource pool has no capacity.");
        }
    }
    const int sourceBefore = balance(sourceId);
    const int targetBefore = balance(targetId);
    if (sourceId == kWorldActor) {
        state_.resources[resource].amount -= amount;
    } else {
        state_.inventories[sourceId][resource] -= amount;
    }
    if (targetId == kWorldActor) {
        state_.resources[resource].amount += amount;
    } else {
        state_.inventories[targetId][resource] += amount;
    }
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "resource.transferred", actor, targetId, "A registered resource transfer changed ownership quantities",
        Json{{"resource", resource}, {"amount", amount}, {"source", sourceId}, {"target", targetId},
             {"source_before", sourceBefore}, {"source_after", balance(sourceId)},
             {"target_before", targetBefore}, {"target_after", balance(targetId)}});
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
    auto npc = state_.npcs.find(npcId);
    if (npc == state_.npcs.end()) return reject("unknown_npc", "The relationship actor does not exist.");
    if (target.empty() || target == npcId ||
        (!state_.npcs.contains(target) && target != kPlayerId && !state_.socialGroups.contains(target))) {
        return reject("unknown_relationship_target", "The relationship target does not exist.");
    }
    if (actor != kWorldActor && actor != kPlayerId && actor != npcId) {
        return reject("unauthorized", "Only the actor or world authority may change this relationship.");
    }
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The relationship change was already applied.");
    }
    applyRelationshipDelta(npc->second.relationships[target],
                           RelationshipDelta{delta.trust, delta.respect, delta.fear, delta.owed});
    ++npc->second.stateVersion;
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
    if (metric.empty() || !std::isfinite(delta)) return reject("invalid_group_metric", "A group metric needs a finite delta.");
    auto group = state_.socialGroups.find(groupId);
    if (group == state_.socialGroups.end()) return reject("unknown_social_group", "The social group does not exist.");
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The group metric change was already applied.");
    const double before = group->second.metrics[metric];
    group->second.metrics[metric] += delta;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "social.group_metric", actor, groupId, reason.empty() ? "A social group metric changed" : reason,
        Json{{"group_id", groupId}, {"metric", metric}, {"before", before},
             {"after", group->second.metrics[metric]}, {"delta", delta}});
    return CommandResult{true, "ok", metric, event};
}

CommandResult WorldService::setGroupMembership(const std::string& groupId, const std::string& npcId,
                                               const std::string& role, bool active,
                                               const std::string& actor, const std::string& turnId) {
    TransactionScope transaction(*this);
    auto group = state_.socialGroups.find(groupId);
    if (group == state_.socialGroups.end()) return reject("unknown_social_group", "The social group does not exist.");
    if (!state_.npcs.contains(npcId)) return reject("unknown_npc", "The group member does not exist.");
    if (active && role.empty()) return reject("invalid_group_role", "An active membership needs a role.");
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The membership change was already applied.");
    if (active) group->second.members[npcId] = role;
    else group->second.members.erase(npcId);
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
    TransactionScope transaction(*this);
    if (proposal.status != EvolutionProposalStatus::Approved) {
        return reject("overlay_not_approved", "Only an approved evolution proposal can activate an overlay.");
    }
    if (!sandbox.passed || sandbox.proposalId != proposal.id || sandbox.overlayVersion.empty()) {
        return reject("overlay_sandbox_required", "A passing sandbox result for this proposal is required.");
    }
    if (sandbox.baseContentVersion != proposal.baseContentVersion ||
        sandbox.baseRevision != state_.revision || proposal.baseWorldRevision != state_.revision) {
        return reject("overlay_sandbox_stale",
                      "The sandbox and proposal must target the current content version and world revision.");
    }
    if (std::any_of(state_.activeOverlays.begin(), state_.activeOverlays.end(),
                    [&proposal](const auto& active) { return active.proposalId == proposal.id; })) {
        return reject("overlay_already_active", "That evolution overlay is already active.");
    }
    auto candidate = content_;
    const auto applied = EvolutionManager::applyOverlay(candidate, proposal);
    if (!applied.valid) return reject(applied.code, applied.message);
    if (candidate.contentVersion != sandbox.overlayVersion) {
        return reject("overlay_version_mismatch", "The sandbox result does not match the activated content version.");
    }
    const auto previousVersion = content_.contentVersion;
    content_ = std::move(candidate);
    state_.contentVersion = content_.contentVersion;
    state_.activeOverlays.push_back(
        ActiveOverlayState{proposal.id, proposal.baseContentVersion, content_.contentVersion, proposal.operations});
    const auto event = addEvent(
        "evolution.overlay_activated", "developer", proposal.id, "An approved content overlay became active",
        Json{{"proposal_id", proposal.id}, {"overlay_version", content_.contentVersion},
             {"previous_content_version", previousVersion},
             {"base_content_version", proposal.baseContentVersion},
             {"active_overlay_ids", overlayIds()}});
    rebuildEntityRegistry();
    return CommandResult{true, "ok", content_.contentVersion, event};
}

std::vector<std::string> WorldService::overlayIds() const {
    std::vector<std::string> ids;
    ids.reserve(state_.activeOverlays.size());
    for (const auto& overlay : state_.activeOverlays) ids.push_back(overlay.proposalId);
    return ids;
}

CommandResult WorldService::rollbackOverlay(const std::string& proposalId) {
    TransactionScope transaction(*this);
    const auto active = std::find_if(state_.activeOverlays.begin(), state_.activeOverlays.end(),
                                     [&proposalId](const auto& item) { return item.proposalId == proposalId; });
    if (active == state_.activeOverlays.end()) return reject("overlay_not_active", "That overlay is not active.");
    if (std::next(active) != state_.activeOverlays.end()) {
        return reject("overlay_not_top", "Roll back newer overlays before removing an older overlay.");
    }
    const auto previousVersion = content_.contentVersion;
    content_ = baseContent_;
    for (auto record = state_.activeOverlays.begin(); record != active; ++record) {
        EvolutionProposal replay{record->proposalId, "declarative_capability", "snapshot",
                                 record->baseContentVersion, 0, record->operations,
                                 EvolutionProposalStatus::Approved, {}};
        const auto applied = EvolutionManager::applyOverlay(content_, replay);
        if (!applied.valid) return reject(applied.code, applied.message);
        if (content_.contentVersion != record->overlayVersion) {
            return reject("overlay_rebuild_version_mismatch",
                          "The persisted overlay stack did not rebuild its recorded version.");
        }
    }
    state_.contentVersion = content_.contentVersion;
    state_.activeOverlays.erase(active);
    const auto event = addEvent(
        "evolution.overlay_rolled_back", "developer", proposalId, "An active content overlay was rolled back",
        Json{{"proposal_id", proposalId}, {"from_version", previousVersion},
             {"to_version", content_.contentVersion}, {"active_overlay_ids", overlayIds()}});
    rebuildEntityRegistry();
    return CommandResult{true, "ok", content_.contentVersion, event};
}

CommandResult WorldService::assertKnownFact(const std::string& npcId, const std::string& subject,
                                            const std::string& predicate, const std::string& value,
                                            std::uint64_t sourceSequence, const std::string& scope,
                                            double confidence, const std::string& actor,
                                            const std::string& turnId) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The knowledge fact was already applied.");
    }
    auto npc = state_.npcs.find(npcId);
    if (npc == state_.npcs.end()) return reject("unknown_npc", "Knowledge must belong to an existing NPC.");
    if (actor != kWorldActor && actor != npcId && actor != kPlayerId) {
        return reject("unauthorized", "The actor cannot write this NPC's knowledge.");
    }
    if (subject.empty() || predicate.empty() || value.empty()) {
        return reject("invalid_knowledge", "Knowledge needs subject, predicate and value.");
    }
    if (confidence < 0.0 || confidence > 1.0) return reject("invalid_knowledge", "Knowledge confidence must be between 0 and 1.");
    const auto source = std::find_if(state_.events.begin(), state_.events.end(), [sourceSequence](const auto& event) {
        return event.sequence == sourceSequence;
    });
    if (source == state_.events.end()) return reject("unknown_knowledge_source", "Knowledge must cite a committed event.");
    const bool participant = source->actor == npcId || source->target == npcId || source->target == npcEntityId(npcId);
    if (scope == "direct" && !participant) {
        return reject("source_not_observed", "An NPC cannot claim direct knowledge of an unrelated event.");
    }
    if (scope != "direct" && scope != "witness" && scope != "testimony" && scope != "inference") {
        return reject("invalid_knowledge_scope", "Knowledge scope is not registered.");
    }
    const auto existing = std::find_if(npc->second.knowledge.begin(), npc->second.knowledge.end(),
                                       [&subject, &predicate](const auto& fact) {
                                           return fact.subject == subject && fact.predicate == predicate;
                                       });
    const std::string id = "knowledge-" + std::to_string(state_.events.size() + 1);
    KnowledgeFact fact{id, subject, predicate, value, sourceSequence, source->type, state_.now,
                       confidence, scope, true};
    if (existing != npc->second.knowledge.end()) *existing = fact;
    else npc->second.knowledge.push_back(fact);
    ++npc->second.stateVersion;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "knowledge.asserted", actor, npcId, "An NPC recorded a sourced belief",
        Json{{"knowledge_id", id}, {"subject", subject}, {"predicate", predicate},
             {"value", value}, {"source_sequence", sourceSequence}, {"source_type", source->type},
             {"scope", scope}, {"confidence", confidence}});
    return CommandResult{true, "ok", id, event};
}

CommandResult WorldService::recordExperience(const std::string& npcId, const std::string& domain,
                                             const std::string& trigger, const std::string& action,
                                             const std::string& outcome, std::uint64_t sourceSequence,
                                             double confidence, bool personal, const std::string& actor,
                                             const std::string& turnId) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The experience was already applied.");
    }
    auto npc = state_.npcs.find(npcId);
    if (npc == state_.npcs.end()) return reject("unknown_npc", "Experience must belong to an existing NPC.");
    if (actor != kWorldActor && actor != npcId && actor != kPlayerId) {
        return reject("unauthorized", "The actor cannot write this NPC's experience.");
    }
    if (domain.empty() || trigger.empty() || action.empty() || outcome.empty()) {
        return reject("invalid_experience", "Experience needs domain, trigger, action and outcome.");
    }
    if (confidence < 0.0 || confidence > 1.0) return reject("invalid_experience", "Experience confidence must be between 0 and 1.");
    const auto source = std::find_if(state_.events.begin(), state_.events.end(), [sourceSequence](const auto& event) {
        return event.sequence == sourceSequence;
    });
    if (source == state_.events.end()) return reject("unknown_experience_source", "Experience must cite a committed event.");
    const auto existing = std::find_if(npc->second.experiences.begin(), npc->second.experiences.end(),
                                       [&domain, &trigger, &action](const auto& experience) {
                                           return experience.domain == domain && experience.trigger == trigger &&
                                                  experience.action == action;
                                       });
    if (existing != npc->second.experiences.end()) {
        const double samples = static_cast<double>(existing->sourceSequences.size() + 1);
        existing->confidence = std::clamp(existing->confidence + (confidence - existing->confidence) / samples,
                                           0.0, 1.0);
        existing->outcome = outcome;
        existing->lastUsed = state_.now;
        if (std::find(existing->sourceSequences.begin(), existing->sourceSequences.end(), sourceSequence) ==
            existing->sourceSequences.end()) {
            existing->sourceSequences.push_back(sourceSequence);
        }
    } else {
        Experience experience{"experience-" + std::to_string(state_.events.size() + 1), domain, trigger,
                               action, outcome, {sourceSequence}, state_.now, state_.now, confidence, personal};
        npc->second.experiences.push_back(std::move(experience));
    }
    ++npc->second.stateVersion;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto event = addEvent(
        "experience.recorded", actor, npcId, "An outcome was abstracted into reusable experience",
        Json{{"domain", domain}, {"trigger", trigger}, {"action", action},
             {"outcome", outcome}, {"source_sequence", sourceSequence},
             {"confidence", confidence}, {"personal", personal}});
    return CommandResult{true, "ok", domain, event};
}

CommandResult WorldService::applySecurityOperator(const std::string& operation, const std::string& actor,
                                                  const std::string& target, const Json& args,
                                                  const std::string& turnId) {
    TransactionScope transaction(*this);
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        return reject("duplicate_turn", "The security action was already applied.");
    }
    const auto resolve = [this](const std::string& value) -> std::string {
        if (value == "world" || value == kWorldActor) return kWorldActor;
        if (state_.entities.contains(value)) return value;
        if (state_.npcs.contains(value)) return npcEntityId(value);
        return {};
    };
    const auto actorEntity = resolve(actor);
    const auto requestedTarget = target.empty() ? actor : target;
    const auto targetEntity = resolve(requestedTarget == "self" ? actor : requestedTarget);
    if (actorEntity.empty() || targetEntity.empty() ||
        (actorEntity == targetEntity &&
         (operation == "conflict.attack" || operation == "conflict.sabotage" || operation == "security.threaten"))) {
        return reject("invalid_security_target", "Security actions need distinct active actor and target entities.");
    }
    const auto active = [this](const std::string& id) {
        if (id == kWorldActor) return true;
        const auto found = state_.entities.find(id);
        return found != state_.entities.end() && found->second.lifecycle == EntityLifecycle::Active;
    };
    if (!active(actorEntity) || !active(targetEntity)) return reject("inactive_security_target", "The security target is not active.");
    if (operation == "conflict.attack" || operation == "conflict.sabotage" || operation == "security.threaten") {
        if (actorEntity == kWorldActor) return reject("invalid_security_actor", "A conflict action needs a concrete actor.");
    }
    auto& security = state_.security[targetEntity];
    const auto bounded = [](double value) { return std::clamp(value, 0.0, 1.0); };
    const double integrityBefore = security.integrity;
    const double availabilityBefore = security.availability;
    const double threatBefore = security.threat;
    const double exposureBefore = security.exposure;
    const bool evacuatedBefore = security.evacuated;
    if (operation == "conflict.attack" || operation == "conflict.sabotage") {
        const double defaultDamage = operation == "conflict.attack" ? 0.15 : 0.10;
        const double damage = bounded(args.value("integrity_loss", args.value("damage", defaultDamage)));
        const double availabilityLoss = bounded(args.value("availability_loss", damage));
        const double threatGain = bounded(args.value("threat_gain", 0.7));
        security.integrity = bounded(security.integrity - damage);
        security.availability = bounded(security.availability - availabilityLoss);
        security.threat = bounded(security.threat + threatGain);
        security.exposure = bounded(security.exposure + args.value("exposure_gain", 0.4));
    } else if (operation == "security.threaten") {
        const double threatGain = bounded(args.value("threat_gain", 0.55));
        security.threat = bounded(security.threat + threatGain);
        security.exposure = bounded(security.exposure + args.value("exposure_gain", 0.25));
    } else if (operation == "security.defend") {
        security.threat = bounded(security.threat - args.value("threat_reduction", 0.35));
        security.exposure = bounded(security.exposure - args.value("exposure_reduction", 0.25));
    } else if (operation == "security.evacuate") {
        security.evacuated = true;
        security.exposure = bounded(security.exposure - args.value("exposure_reduction", 0.6));
    } else if (operation == "security.repair") {
        const auto resource = args.value("resource", "");
        const int cost = args.value("amount", 0);
        if (!resource.empty() && (cost <= 0 || availableResourceValue(resource) < cost)) {
            return reject("insufficient_resource", "Security repair lacks its authored repair resource.");
        }
        if (!resource.empty()) state_.resources[resource].amount -= cost;
        security.integrity = bounded(security.integrity + args.value("integrity_restore", 0.2));
        security.availability = bounded(security.availability + args.value("availability_restore", 0.2));
        security.threat = bounded(security.threat - args.value("threat_reduction", 0.2));
        security.evacuated = false;
    } else {
        return reject("unknown_security_operator", "The security operator is not registered.");
    }
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    const auto eventActor = actorEntity.starts_with("npc:") ? actorEntity.substr(4) : actorEntity;
    const auto eventTarget = targetEntity.starts_with("npc:") ? targetEntity.substr(4) : targetEntity;
    const auto event = addEvent(
        operation, eventActor, eventTarget, "A registered security operator changed target security facts",
        Json{{"operation", operation}, {"actor_entity", actorEntity}, {"target_entity", targetEntity},
             {"integrity_before", integrityBefore}, {"integrity_after", security.integrity},
             {"availability_before", availabilityBefore}, {"availability_after", security.availability},
             {"threat_before", threatBefore}, {"threat_after", security.threat},
             {"exposure_before", exposureBefore}, {"exposure_after", security.exposure},
             {"evacuated_before", evacuatedBefore}, {"evacuated_after", security.evacuated}});
    return CommandResult{true, "ok", operation, event};
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
    return executeTurn(npcId, playerInput, turnId, content_.conversationChannelId, "player_input");
}

DialogueTurn WorldService::deliberate(const std::string& npcId, const std::string& trigger,
                                      const std::string& turnId) {
    return executeTurn(npcId, {}, turnId, content_.autonomousChannelId, trigger);
}

TurnRequest WorldService::makeTurnRequest(const std::string& npcId, const std::string& input,
                                          const std::string& turnId, const std::string& channelId,
                                          const std::string& trigger) const {
    TurnRequest request;
    request.turnId = turnId;
    request.npcId = npcId;
    request.channelId = channelId;
    request.trigger = trigger;
    request.playerInput = input;
    request.contentVersion = content_.contentVersion;
    request.worldRevision = state_.revision;
    request.npcStateVersion = state_.npcs.at(npcId).stateVersion;
    request.createdAt = state_.now;
    const auto channel = content_.decisionChannels.find(channelId);
    request.context = decisionContext(npcId);
    request.context.channel = channelId;
    request.context.providerPolicy = channel->second.providerPolicy;
    request.context.trigger = trigger;
    request.context.promptVersion =
        "tide-marks-intent-v2:" + content_.contentVersion + ":" + content_.id;
    request.promptVersion = request.context.promptVersion;
    request.coalescePending = !channel->second.conversation &&
                              content_.npcDeliberation.coalescePending;
    request.deadline = GameTime{state_.now.minute + content_.npcDeliberation.maxDecisionAgeMinutes};
    if (channel->second.conversation && channel->second.classifySpeechAct) {
        // Semantic interpretation belongs exclusively to the provider. Runtime
        // supplies the content-authored speech-act vocabulary and validates the
        // returned identifier; it never inspects player text for keywords.
        request.context.speechAct.clear();
        request.context.worldActionAllowed = false;
        request.context.speechActClassificationRequired = true;
        for (const auto& rule : content_.conversationPolicy.rules) {
            request.context.speechActs.push_back(
                SpeechActPermissionView{toString(rule.act), rule.description, rule.worldActionAllowed});
        }
    } else {
        request.context.speechAct = channel->second.defaultSpeechAct;
        request.context.worldActionAllowed = channel->second.defaultWorldActionAllowed;
    }
    return request;
}

DialogueTurn WorldService::executeTurn(const std::string& npcId, const std::string& input,
                                       const std::string& turnId, const std::string& channelId,
                                       const std::string& trigger) {
    DialogueTurn result;
    result.npcId = npcId;
    result.provider = provider_->name();
    if (!hasNpc(npcId)) {
        result.error = "unknown_npc";
        return result;
    }
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) {
        result.error = "duplicate_turn";
        return result;
    }
    if (!content_.decisionChannels.contains(channelId)) {
        result.error = "unknown_decision_channel";
        return result;
    }
    auto request = makeTurnRequest(npcId, input, turnId, channelId, trigger);
    auto response = provider_->decide(request.context, input);
    return commitTurn(request, std::move(response));
}

DialogueTurn WorldService::commitTurn(const TurnRequest& request, ProviderResponse response) {
    TransactionScope transaction(*this);
    DialogueTurn result;
    result.npcId = request.npcId;
    result.provider = provider_->name();
    if (!hasNpc(request.npcId)) {
        result.error = "unknown_npc";
        return result;
    }
    if (!request.turnId.empty() && state_.processedTurnIds.contains(request.turnId)) {
        result.error = "duplicate_turn";
        return result;
    }
    if (request.deadline.minute > 0 && state_.now.minute > request.deadline.minute) {
        result.error = "decision_expired";
        return result;
    }
    const auto channel = content_.decisionChannels.find(request.channelId);
    if (channel == content_.decisionChannels.end()) {
        result.error = "unknown_decision_channel";
        return result;
    }
    const auto& npc = state_.npcs.at(request.npcId);
    if (request.contentVersion != content_.contentVersion ||
        request.promptVersion != request.context.promptVersion ||
        state_.revision != request.worldRevision || npc.stateVersion != request.npcStateVersion) {
        result.error = "stale_intent_proposal";
        return result;
    }
    const auto& context = request.context;
    if (!response.ok) {
        if (channel->second.conversation) {
            // Conversation semantics must come from the configured intelligence
            // provider. Guessing intent from tokens would produce a second,
            // ungoverned semantic authority inside Runtime.
            result.error = response.error.empty() ? "provider_unavailable" : response.error;
            return result;
        }
        const std::string providerError = response.error;
        response = MockProvider{}.decide(context, request.playerInput);
        result.provider += "+deterministic_fallback";
        result.error = "provider_fallback:" + providerError;
    }
    const std::string effectiveSpeechAct = channel->second.conversation
                                               ? (response.proposal.speechAct.empty()
                                                      ? request.context.speechAct
                                                      : response.proposal.speechAct)
                                               : request.context.speechAct;
    const auto parsedSpeechAct = speechActFromString(effectiveSpeechAct);
    if (channel->second.conversation && context.speechActClassificationRequired &&
        parsedSpeechAct == SpeechAct::Unknown) {
        result.error = "invalid_speech_act";
        return result;
    }
    const bool worldActionAllowed =
        channel->second.conversation && context.speechActClassificationRequired
            ? permitsWorldAction(parsedSpeechAct, content_.conversationPolicy)
            : channel->second.defaultWorldActionAllowed;
    // Autonomous channels must produce an action. Conversation channels may
    // validly refuse or answer without one, so their empty action_id is kept.
    if (response.ok && !channel->second.conversation && worldActionAllowed &&
        response.proposal.actionId.empty()) {
        response = MockProvider{}.decide(context, request.playerInput);
        result.provider += "+proposal_fallback";
        result.error = "provider_fallback:world_action_required";
    }
    if (response.proposal.worldRevision != context.worldRevision ||
        response.proposal.npcStateVersion != context.npcStateVersion) {
        result.error = "stale_intent_proposal";
        return result;
    }
    if (response.proposal.confidence < 0.0 || response.proposal.confidence > 1.0) {
        result.error = "invalid_intent_confidence";
        return result;
    }
    for (const auto& stimulusId : response.proposal.citedStimulusIds) {
        const auto foundStimulus = std::find_if(context.stimuli.begin(), context.stimuli.end(),
                                                [&stimulusId](const auto& stimulus) { return stimulus.id == stimulusId; });
        if (foundStimulus == context.stimuli.end()) {
            result.error = "unknown_stimulus_citation";
            return result;
        }
    }
    for (const auto& [needId, expected] : response.proposal.expectedRelief) {
        if (expected < 0.0 || expected > 1.0 || !state_.npcs.at(request.npcId).needs.contains(needId)) {
            result.error = "invalid_expected_relief";
            return result;
        }
    }
    if (response.proposal.actionId.empty()) {
        if (worldActionAllowed && response.proposal.worldActionRequested) {
            result.error = "world_action_required";
            return result;
        }
        auto& mutableNpc = state_.npcs.at(request.npcId);
        if (!request.turnId.empty()) state_.processedTurnIds.insert(request.turnId);
        ++mutableNpc.stateVersion;
        mutableNpc.memories.push_back(Memory{channel->second.memoryKind, request.playerInput, state_.now,
                                             response.proposal.confidence});
        const auto target = channel->second.eventTarget == "player" ? kPlayerId :
                            channel->second.eventTarget == "self" ? "self" : channel->second.eventTarget;
        const auto event = addEvent(
            channel->second.eventType, request.npcId, target, response.dialogue,
            Json{{"turn_id", request.turnId}, {"action_id", ""}, {"trigger", request.trigger},
                 {"speech_act", effectiveSpeechAct}, {"world_action_requested", false},
                 {"utterance_understanding", response.proposal.utteranceUnderstanding},
                 {"process_status", "conversation_only"}, {"provider", result.provider},
                 {"context_world_revision", response.proposal.worldRevision},
                 {"context_npc_state_version", response.proposal.npcStateVersion}});
        result.accepted = true;
        result.intent = response.proposal.intent;
        result.motivation = response.proposal.rationale;
        result.dialogue = response.dialogue;
        result.event = event;
        syncNpcEntityState();
        return result;
    }
    if (!worldActionAllowed) {
        result.error = "speech_act_disallows_world_action";
        return result;
    }
    const auto validation = ActionValidator::validate(context, response.proposal.actionId);
    if (!validation.valid) {
        result.error = validation.code;
        return result;
    }
    const auto candidate = std::find_if(context.candidates.begin(), context.candidates.end(),
                                        [&response](const auto& item) {
                                            return item.actionId == response.proposal.actionId;
                                        });
    if (candidate == context.candidates.end()) {
        result.error = "unknown_action_id";
        return result;
    }
    auto& mutableNpc = state_.npcs.at(request.npcId);
    if (mutableNpc.activity.process) {
        double incomingSalience = channel->second.conversation ? 1.0 : 0.0;
        for (const auto& stimulus : context.stimuli) {
            incomingSalience = std::max(incomingSalience, stimulus.effectiveSalience);
        }
        if (!mutableNpc.activity.process->interruptible ||
            incomingSalience < mutableNpc.activity.process->interruptSalienceThreshold) {
            result.error = "npc_busy";
            return result;
        }
        cancelProcess(request.npcId, *mutableNpc.activity.process, request.trigger);
        mutableNpc.activity = NpcActivity{};
    }
    std::map<std::string, int> reservations = candidate->reservationMode == "input_resources"
                                                   ? candidate->reservations
                                                   : std::map<std::string, int>{};
    const std::string processId =
        "process:" + request.npcId + ":" +
        (request.turnId.empty() ? std::to_string(state_.events.size() + 1) : request.turnId);
    if (candidate->preemptionPolicy == "lower_priority") {
        // Plan all suspensions before mutating state, then apply one
        // deterministic arbitration decision. This prevents an impossible
        // request from leaving some victims suspended.
        const auto victims = planResourcePreemption(request.npcId, reservations, candidate->priority);
        if (!victims) {
            transaction.rollback();
            result.error = "insufficient_resource";
            return result;
        }
        for (const auto& victimId : *victims) {
            auto& victim = state_.npcs.at(victimId);
            suspendProcess(victimId, *victim.activity.process, "resource_preempted",
                           Json{{"preempted_by", request.npcId},
                                {"preempting_process_id", processId},
                                {"preempting_priority", candidate->priority}});
        }
    }
    for (const auto& [resource, amount] : reservations) {
        if (availableResourceValue(resource) < amount) {
            transaction.rollback();
            result.error = "insufficient_resource";
            return result;
        }
    }
    if (candidate->durationMinutes <= 0 && !applyActionEffects(request.npcId, candidate->actionId)) {
        transaction.rollback();
        result.error = "insufficient_resource";
        return result;
    }
    result.accepted = true;
    if (!request.turnId.empty()) state_.processedTurnIds.insert(request.turnId);
    ++mutableNpc.stateVersion;
    if (candidate->durationMinutes > 0) {
        for (const auto& [resource, amount] : reservations) state_.resources[resource].reserved += amount;
        const std::string goalId = "goal:" + request.npcId + ":" + processId.substr(processId.find(':') + 1);
        mutableNpc.activity.goal = GoalState{goalId, response.proposal.intent, candidate->actionId,
                                             response.proposal.confidence, GoalStatus::Active, state_.now,
                                             GameTime{state_.now.minute + candidate->durationMinutes},
                                             response.proposal.rationale};
        ProcessExecution process;
        process.id = processId;
        process.goalId = goalId;
        process.actionId = candidate->actionId;
        process.recipeId = candidate->recipeId;
        process.planId = candidate->planId;
        process.trigger = request.trigger;
        process.startedAt = state_.now;
        process.completesAt = GameTime{state_.now.minute + candidate->durationMinutes};
        process.behaviorNodeId = candidate->planId;
        process.interruptible = candidate->interruptible;
        process.interruptSalienceThreshold = candidate->interruptSalienceThreshold;
        process.reservationMode = candidate->reservationMode;
        process.reservedResources = reservations;
        process.requiredResources = reservations;
        process.priority = candidate->priority;
        process.preemptionPolicy = candidate->preemptionPolicy;
        process.proposalWorldRevision = response.proposal.worldRevision;
        process.proposalNpcStateVersion = response.proposal.npcStateVersion;
        mutableNpc.activity.process = std::move(process);
        setActivityStatus(mutableNpc.activity, ActivityStatus::Active);
        applyNpcSignal(mutableNpc, NpcSignal::StartProcess);
    }
    mutableNpc.memories.push_back(Memory{channel->second.memoryKind,
                                         request.playerInput.empty() ? response.proposal.intent : request.playerInput,
                                         state_.now, 1.0});
    const auto target = channel->second.eventTarget == "player" ? kPlayerId :
                        channel->second.eventTarget == "self" ? "self" : channel->second.eventTarget;
    const auto event = addEvent(
        channel->second.eventType, request.npcId, target, response.dialogue,
        Json{{"turn_id", request.turnId}, {"process_id", processId},
             {"action_id", response.proposal.actionId}, {"trigger", request.trigger},
             {"speech_act", effectiveSpeechAct},
             {"utterance_understanding", response.proposal.utteranceUnderstanding},
             {"duration_minutes", candidate->durationMinutes},
             {"process_status", candidate->durationMinutes > 0 ? "running" : "completed"},
             {"intent", response.proposal.intent}, {"motivation", response.proposal.rationale},
             {"expected_outcome", response.proposal.expectedOutcome},
             {"cited_stimulus_ids", response.proposal.citedStimulusIds},
             {"tradeoffs", response.proposal.tradeoffs},
             {"expected_relief", response.proposal.expectedRelief},
             {"confidence", response.proposal.confidence}, {"provider", result.provider},
             {"context_world_revision", response.proposal.worldRevision},
             {"context_npc_state_version", response.proposal.npcStateVersion}});
    result.actionId = response.proposal.actionId;
    result.intent = response.proposal.intent;
    result.motivation = response.proposal.rationale;
    result.dialogue = response.dialogue;
    result.event = event;
    syncNpcEntityState();
    return result;
}

void WorldService::enqueueAutonomousTurn(const std::string& npcId, const std::string& trigger) {
    if (!hasNpc(npcId)) return;
    const std::string turnId = "autonomous:" + npcId + ":" + std::to_string(state_.now.minute) + ":" +
                               std::to_string(state_.revision);
    if (pendingAsyncTurnIds_.contains(turnId)) return;
    const auto request = makeTurnRequest(npcId, {}, turnId, content_.autonomousChannelId, trigger);
    const auto submitted = turnQueue_->submit(request);
    if (!submitted.accepted) return;
    for (const auto& supersededId : submitted.supersededTurnIds) pendingAsyncTurnIds_.erase(supersededId);
    pendingAsyncTurnIds_.insert(turnId);
    state_.npcs.at(npcId).lastAutonomousTurn = state_.now;
}

std::size_t WorldService::pumpAsyncTurns() {
    if (!turnQueue_) return 0;
    const auto results = turnQueue_->drain();
    std::size_t consumed = 0;
    for (auto& queued : results) {
        ++consumed;
        pendingAsyncTurnIds_.erase(queued.request.turnId);
        const auto turn = commitTurn(queued.request, std::move(queued.response));
        if (turn.accepted) continue;
        // An asynchronous result is observable even when it arrives too late
        // or fails validation. This is an audit fact, not a world mutation.
        TransactionScope transaction(*this);
        const bool expired = turn.error == "stale_intent_proposal" ||
                             turn.error == "decision_expired" ||
                             state_.revision != queued.request.worldRevision ||
                             !hasNpc(queued.request.npcId) ||
                             state_.npcs.at(queued.request.npcId).stateVersion != queued.request.npcStateVersion;
        addEvent(expired ? "npc.intent_expired" : "npc.intent_rejected", queued.request.npcId, "self",
                 expired ? "An asynchronous intent arrived after its decision context expired."
                         : "An asynchronous intent was rejected by the runtime validator.",
                 Json{{"turn_id", queued.request.turnId}, {"channel", queued.request.channelId},
                      {"trigger", queued.request.trigger}, {"error", turn.error},
                      {"request_world_revision", queued.request.worldRevision},
                      {"current_world_revision", state_.revision},
                      {"request_npc_state_version", queued.request.npcStateVersion},
                      {"created_at", queued.request.createdAt.minute},
                      {"deadline", queued.request.deadline.minute},
                      {"current_minute", state_.now.minute},
                      {"provider", queued.provider}});
    }
    return consumed;
}

std::size_t WorldService::pendingAsyncTurns() const {
    return turnQueue_ ? turnQueue_->pending() : 0;
}

int WorldService::resourceValue(const std::string& resource) const {
    const auto found = state_.resources.find(resource);
    return found == state_.resources.end() ? 0 : found->second.amount;
}

int WorldService::availableResourceValue(const std::string& resource) const {
    const auto found = state_.resources.find(resource);
    return found == state_.resources.end() ? 0 : found->second.amount - found->second.reserved;
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

const BehaviorRecipeContent* WorldService::recipeForAction(const std::string& actionId) const {
    const auto found = std::find_if(content_.behaviorRecipes.begin(), content_.behaviorRecipes.end(),
                                    [&actionId](const auto& recipe) { return recipe.id == actionId; });
    return found == content_.behaviorRecipes.end() ? nullptr : &*found;
}

bool WorldService::evaluateBehaviorCondition(const std::string& npcId,
                                             const BehaviorNodeContent& node) const {
    const auto definition = content_.conditions.find(node.name);
    if (definition == content_.conditions.end()) return false;
    Json args = definition->second.args;
    for (auto it = node.args.begin(); it != node.args.end(); ++it) args[it.key()] = it.value();
    const auto& operation = definition->second.operation;
    if (operation == "always") return true;
    if (operation == "resource.available") {
        return availableResourceValue(args.value("resource", "")) >= args.value("minimum", 1);
    }
    if (operation == "resource.below") {
        return resourceValue(args.value("resource", "")) < args.value("below", 1);
    }
    if (operation == "link.traversable" || operation == "link.locked") {
        const auto link = state_.links.find(args.value("link", ""));
        const bool traversable = link == state_.links.end() ||
                                 (link->second.traversable && !link->second.locked);
        return operation == "link.traversable" ? traversable : !traversable;
    }
    if (operation == "need.critical") {
        const auto npc = state_.npcs.find(npcId);
        const auto* authored = authoredNpc(npcId);
        if (npc == state_.npcs.end() || authored == nullptr) return false;
        const auto needId = args.value("need", "");
        const auto need = std::find_if(authored->needs.begin(), authored->needs.end(),
                                       [&needId](const auto& item) { return item.id == needId; });
        const auto stateNeed = npc->second.needs.find(needId);
        return need != authored->needs.end() && stateNeed != npc->second.needs.end() &&
               stateNeed->second.deficit >= need->criticalThreshold;
    }
    if (operation == "npc.at") {
        const auto npc = state_.npcs.find(npcId);
        return npc != state_.npcs.end() && npc->second.location == args.value("location", "");
    }
    if (operation == "time.between") {
        const int minuteOfDay = state_.now.hour() * 60 + state_.now.minuteOfHour();
        return minuteOfDay >= args.value("from_minute", 0) && minuteOfDay < args.value("to_minute", 0);
    }
    return false;
}

bool WorldService::applyMetaAction(const std::string& npcId, const std::string& actionId,
                                   const BehaviorNodeContent& node,
                                   const std::map<std::string, int>* reservedResources) {
    const auto definition = content_.metaActions.find(node.name);
    if (definition == content_.metaActions.end()) return false;
    const Json args = mergedBehaviorArgs(definition->second, node);
    const auto& operation = definition->second.operation;
    if (operation == "resource.consume") {
        const auto resource = args.value("resource", "");
        const int amount = args.value("amount", 0);
        if (amount <= 0 || !content_.resources.contains(resource)) return false;
        const bool reserved = reservedResources != nullptr && reservedResources->contains(resource);
        if (reserved && reservedResources->at(resource) < amount) return false;
        if (!reserved && availableResourceValue(resource) < amount) return false;
        auto& state = state_.resources[resource];
        const int before = state.amount;
        state.amount = std::max(0, state.amount - amount);
        if (reserved) state.reserved = std::max(0, state.reserved - amount);
        addEvent("action.resource", npcId, resource, "A behavior recipe consumed a resource",
                 Json{{"action_id", actionId}, {"resource", resource}, {"before", before},
                      {"after", state.amount}, {"amount", amount}});
        return true;
    }
    if (operation == "resource.produce") {
        const auto resource = args.value("resource", "");
        const int amount = args.value("amount", 0);
        if (amount <= 0 || !content_.resources.contains(resource)) return false;
        const int before = resourceValue(resource);
        setResource(resource, before + amount);
        addEvent("action.resource", npcId, resource, "A behavior recipe produced a resource",
                 Json{{"action_id", actionId}, {"resource", resource}, {"before", before},
                      {"after", resourceValue(resource)}, {"amount", amount}});
        return true;
    }
    if (operation == "resource.reserve") {
        const auto resource = args.value("resource", "");
        const int amount = args.value("amount", 0);
        if (amount <= 0 || availableResourceValue(resource) < amount) return false;
        state_.resources[resource].reserved += amount;
        addEvent("action.reservation", npcId, resource, "A behavior recipe reserved a resource",
                 Json{{"action_id", actionId}, {"resource", resource}, {"amount", amount}});
        return true;
    }
    if (operation == "resource.release") {
        const auto resource = args.value("resource", "");
        const int amount = args.value("amount", 0);
        auto found = state_.resources.find(resource);
        if (amount <= 0 || found == state_.resources.end()) return false;
        found->second.reserved = std::max(0, found->second.reserved - amount);
        addEvent("action.reservation", npcId, resource, "A behavior recipe released a resource reservation",
                 Json{{"action_id", actionId}, {"resource", resource}, {"amount", amount}});
        return true;
    }
    if (operation == "resource.transfer") {
        const auto source = args.value("source", "self");
        const auto target = args.value("target", "world");
        const auto resource = args.value("resource", "");
        const int amount = args.value("amount", 0);
        // Keep actor-facing NPC ids at the command boundary.  The transfer
        // operator resolves them to entity ids internally; passing the
        // canonical id here preserves the authored `self` party for
        // authorization instead of accidentally turning it into an
        // unrelated inventory identifier.
        const auto resolvedSource = source == "self" ? npcId : source;
        const auto resolvedTarget = target == "self" ? npcId : target;
        const auto transfer = transferResource(resolvedSource, resolvedTarget, resource, amount, npcId,
                                               "recipe:" + npcId + ":" + actionId);
        return transfer.accepted;
    }
    if (operation == "movement.traverse" || operation == "movement.arrive") {
        const auto destination = args.value("destination", args.value("location", ""));
        if (destination.empty()) return false;
        const auto moved = moveNpc(npcId, destination, npcId,
                                   "recipe:" + npcId + ":" + actionId + ":movement");
        return moved.accepted;
    }
    if (operation == "conflict.attack" || operation == "conflict.sabotage" ||
        operation == "security.threaten" || operation == "security.defend" ||
        operation == "security.evacuate" || operation == "security.repair") {
        const auto target = args.value("target", "self");
        const auto security = applySecurityOperator(operation, npcId, target, args,
                                                    "recipe:" + npcId + ":" + actionId + ":security");
        return security.accepted;
    }
    if (operation == "memory.append") {
        auto npc = state_.npcs.find(npcId);
        if (npc == state_.npcs.end()) return false;
        const auto text = args.value("text", "");
        if (text.empty()) return false;
        const auto sourceSequence = args.value("source_sequence", 0ULL);
        if (sourceSequence != 0 && std::none_of(state_.events.begin(), state_.events.end(),
                                                [sourceSequence](const auto& event) { return event.sequence == sourceSequence; })) {
            return false;
        }
        npc->second.memories.push_back(Memory{args.value("kind", "episode"), text, state_.now,
                                              args.value("confidence", 1.0)});
        addEvent("memory.appended", npcId, args.value("target", "self"), text,
                 Json{{"action_id", actionId}, {"kind", args.value("kind", "episode")},
                      {"source_sequence", sourceSequence}});
        return true;
    }
    if (operation == "knowledge.assert") {
        const auto result = assertKnownFact(
            npcId, args.value("subject", ""), args.value("predicate", ""), args.value("value", ""),
            args.value("source_sequence", 0ULL), args.value("scope", "direct"),
            args.value("confidence", 0.0), npcId,
            "recipe:" + npcId + ":" + actionId + ":knowledge");
        return result.accepted;
    }
    if (operation == "experience.record") {
        const auto result = recordExperience(
            npcId, args.value("domain", ""), args.value("trigger", ""), args.value("action", actionId),
            args.value("outcome", ""), args.value("source_sequence", 0ULL),
            args.value("confidence", 0.0), args.value("personal", true), npcId,
            "recipe:" + npcId + ":" + actionId + ":experience");
        return result.accepted;
    }
    if (operation == "promise.create") {
        const auto resolveParty = [&npcId](const std::string& value) {
            return value == "self" ? npcId : value;
        };
        const auto created = createPromise(resolveParty(args.value("from", "self")),
                                           resolveParty(args.value("to", "")),
                                           args.value("description", ""), args.value("due_days", 1),
                                           "recipe:" + npcId + ":" + actionId + ":promise");
        return created.accepted;
    }
    if (operation == "promise.transition") {
        const auto status = promiseStatusFromString(args.value("status", "proposed"));
        const auto transitioned = decidePromise(args.value("promise_id", ""), status, npcId,
                                                "recipe:" + npcId + ":" + actionId + ":promise",
                                                args.value("detail", ""));
        return transitioned.accepted;
    }
    if (operation == "production.execute") {
        const auto inputs = args.value("inputs", Json::object());
        const auto outputs = args.value("outputs", Json::object());
        if (!inputs.is_object() || !outputs.is_object() || (inputs.empty() && outputs.empty())) return false;
        for (auto it = inputs.begin(); it != inputs.end(); ++it) {
            if (!it.value().is_number_integer() || it.value().get<int>() <= 0 ||
                availableResourceValue(it.key()) < it.value().get<int>()) return false;
        }
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if (!it.value().is_number_integer() || it.value().get<int>() <= 0 || !content_.resources.contains(it.key()) ||
                resourceValue(it.key()) + it.value().get<int>() > content_.resources.at(it.key()).maximum) return false;
        }
        Json consumed = Json::object();
        Json produced = Json::object();
        for (auto it = inputs.begin(); it != inputs.end(); ++it) {
            const int amount = it.value().get<int>();
            state_.resources[it.key()].amount -= amount;
            consumed[it.key()] = amount;
        }
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            const int amount = it.value().get<int>();
            state_.resources[it.key()].amount += amount;
            produced[it.key()] = amount;
        }
        addEvent("production.executed", npcId, "world", "A registered production process exchanged inputs and outputs",
                 Json{{"action_id", actionId}, {"inputs", consumed}, {"outputs", produced}});
        return true;
    }
    if (operation == "trade.execute") {
        const auto seller = args.value("seller", "self") == "self" ? npcId : args.value("seller", npcId);
        const auto buyer = args.value("buyer", "");
        const auto result = tradeResources(
            seller, buyer, args.value("seller_resource", ""), args.value("seller_amount", 0),
            args.value("buyer_resource", ""), args.value("buyer_amount", 0), npcId,
            "recipe:" + npcId + ":" + actionId);
        return result.accepted;
    }
    if (operation == "exchange.create") {
        const auto parseTerms = [](const Json& value, std::map<std::string, int>& terms) {
            if (!value.is_object()) return false;
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!it.value().is_number_integer()) return false;
                terms[it.key()] = it.value().get<int>();
            }
            return true;
        };
        std::map<std::string, int> offered;
        std::map<std::string, int> requested;
        if (!parseTerms(args.value("offered", Json::object()), offered) ||
            !parseTerms(args.value("requested", Json::object()), requested)) {
            return false;
        }
        const auto resolveParty = [&npcId](const std::string& value) {
            return value == "self" ? npcId : value;
        };
        const auto result = proposeExchange(
            resolveParty(args.value("proposer", "self")),
            resolveParty(args.value("counterparty", "")), std::move(offered), std::move(requested),
            args.value("due_minutes", 0), npcId,
            "recipe:" + npcId + ":" + actionId + ":exchange");
        return result.accepted;
    }
    if (operation == "exchange.transition") {
        const auto status = exchangeContractStatusFromString(args.value("status", ""));
        if (status == ExchangeContractStatus::Proposed || status == ExchangeContractStatus::Settled ||
            status == ExchangeContractStatus::Expired) {
            return false;
        }
        const auto result = decideExchange(
            args.value("contract_id", ""), status, npcId,
            "recipe:" + npcId + ":" + actionId + ":exchange", args.value("reason", ""));
        return result.accepted;
    }
    if (operation == "exchange.settle") {
        const auto result = settleExchange(
            args.value("contract_id", ""), npcId,
            "recipe:" + npcId + ":" + actionId + ":exchange");
        return result.accepted;
    }
    if (operation == "group.metric_adjust") {
        const auto result = adjustGroupMetric(
            args.value("group", ""), args.value("metric", ""), args.value("delta", 0.0), npcId,
            "recipe:" + npcId + ":" + actionId, args.value("reason", ""));
        return result.accepted;
    }
    if (operation == "group.membership_adjust") {
        const auto member = args.value("npc", "self") == "self" ? npcId : args.value("npc", npcId);
        const auto result = setGroupMembership(
            args.value("group", ""), member, args.value("role", "member"), args.value("active", true),
            npcId, "recipe:" + npcId + ":" + actionId);
        return result.accepted;
    }
    if (operation == "need.relieve") {
        const auto sourceAction = args.value("action", actionId);
        applyNeedRelief(npcId, sourceAction);
        return true;
    }
    if (operation == "link.set_state") {
        const auto link = args.value("link", "");
        if (!state_.entities.contains(linkEntityId(link))) return false;
        state_.links[link] = RuntimeLinkState{args.value("traversable", true), args.value("locked", false)};
        addEvent("action.structure", npcId, link, "A behavior recipe changed a traversal link",
                 Json{{"action_id", actionId}, {"link_id", link},
                      {"traversable", args.value("traversable", true)}, {"locked", args.value("locked", false)}});
        return true;
    }
    if (operation == "relationship.adjust") {
        auto npc = state_.npcs.find(npcId);
        if (npc == state_.npcs.end()) return false;
        const auto target = args.value("target", "player");
        if (target.empty() || target == npcId ||
            (!state_.npcs.contains(target) && target != kPlayerId && !state_.socialGroups.contains(target))) {
            return false;
        }
        RelationshipDelta delta;
        delta.trust = args.value("trust", 0.0);
        delta.respect = args.value("respect", 0.0);
        delta.fear = args.value("fear", 0.0);
        delta.owed = args.value("owed", 0.0);
        applyRelationshipDelta(npc->second.relationships[target], delta);
        addEvent("action.relationship", npcId, target, "A behavior recipe changed a relationship",
                 Json{{"action_id", actionId}, {"target", target}, {"trust", delta.trust}, {"respect", delta.respect},
                      {"fear", delta.fear}, {"owed", delta.owed}});
        return true;
    }
    return false;
}

bool WorldService::applyActionEffects(const std::string& npcId, const std::string& actionId) {
    return applyActionEffects(npcId, actionId, nullptr);
}

bool WorldService::applyActionEffects(const std::string& npcId, const std::string& actionId,
                                      const std::map<std::string, int>* reservedResources) {
    const auto* recipe = recipeForAction(actionId);
    if (recipe == nullptr) return false;
    const std::size_t eventCountBefore = state_.events.size();
    // A recipe is one atomic transition. Its conditions and all meta-action
    // effects either commit together or disappear together.
    TransactionScope recipeTransaction(*this);
    const auto result = BehaviorRecipeRuntime::run(
        *recipe,
        [this, &npcId](const BehaviorNodeContent& node) {
            return evaluateBehaviorCondition(npcId, node);
        },
        [this, &npcId, &actionId, reservedResources](const BehaviorNodeContent& node) {
            return applyMetaAction(npcId, actionId, node, reservedResources);
        });
    if (result.status != NodeStatus::Success) {
        recipeTransaction.rollback();
        return false;
    }
    // A successfully committed action is an episode. Record a bounded prior
    // candidate from its own event trail, never from model prose.
    const auto effectEvent = std::find_if(state_.events.rbegin(), state_.events.rend(),
                                          [eventCountBefore](const auto& event) { return event.sequence > eventCountBefore; });
    if (effectEvent != state_.events.rend()) {
        auto npc = state_.npcs.find(npcId);
        if (npc != state_.npcs.end()) {
            bool changed = false;
            const auto existing = std::find_if(npc->second.experiences.begin(), npc->second.experiences.end(),
                                               [&actionId](const auto& experience) {
                                                   return experience.action == actionId && experience.domain == "action";
                                               });
            if (existing == npc->second.experiences.end()) {
                npc->second.experiences.push_back(Experience{
                    "experience:" + npcId + ":" + actionId, "action", "observed_context", actionId,
                    effectEvent->type, {effectEvent->sequence}, state_.now, state_.now, 0.5, true});
                changed = true;
            } else if (std::find(existing->sourceSequences.begin(), existing->sourceSequences.end(),
                                 effectEvent->sequence) == existing->sourceSequences.end()) {
                const double samples = static_cast<double>(existing->sourceSequences.size() + 1);
                existing->confidence = std::clamp(existing->confidence + (1.0 - existing->confidence) / samples,
                                                   0.0, 1.0);
                existing->lastUsed = state_.now;
                existing->sourceSequences.push_back(effectEvent->sequence);
                changed = true;
            }
            if (changed) {
                addEvent("experience.observed", npcId, npcId,
                         "A committed action outcome became reusable NPC experience",
                         Json{{"action_id", actionId}, {"source_sequence", effectEvent->sequence},
                              {"source_type", effectEvent->type}, {"domain", "action"}});
            }
        }
    }
    return result.status == NodeStatus::Success;
}

void WorldService::applyNeedRelief(const std::string& npcId, const std::string& actionId) {
    const auto* authored = authoredNpc(npcId);
    const auto npc = state_.npcs.find(npcId);
    if (authored == nullptr || npc == state_.npcs.end()) return;
    const auto changes = DriveSystem::relieve(npc->second, authored->needs, actionId, state_.now,
                                              content_.driveAdaptation);
    for (const auto& change : changes) {
        npc->second.needs.at(change.needId).lastReliefRevision = state_.revision + 1;
        addEvent("need.relieved", npcId, actionId, "A committed action relieved a need",
                 Json{{"need_id", change.needId}, {"action_id", actionId},
                      {"before", change.before}, {"after", change.after},
                      {"expected_relief", change.expectedRelief}, {"actual_relief", change.actualRelief},
                      {"rule", "drive.committed_action"}, {"source_revision", state_.revision + 1}});
        if (change.adapted) {
            addEvent("drive.adapted", npcId, change.needId, "Predicted need relief was calibrated from its outcome",
                     Json{{"need_id", change.needId}, {"action_id", actionId},
                          {"expectation_before", change.expectationBefore},
                          {"expectation_after", change.expectationAfter},
                          {"actual_relief", change.actualRelief},
                          {"adaptation_version", npc->second.needs.at(change.needId).adaptationVersion},
                          {"rule", "drive.bounded_expectation_adaptation"}});
        }
    }
}

void WorldService::projectStimuli(const WorldEvent& event) {
    const WorldEvent source = event;
    const auto rule = std::find_if(content_.stimulusRules.begin(), content_.stimulusRules.end(),
                                   [&source](const auto& candidate) {
                                       return source.type.starts_with(candidate.eventPrefix);
                                   });
    if (rule == content_.stimulusRules.end()) return;
    for (auto& [npcId, npc] : state_.npcs) {
        const auto entity = state_.entities.find(npcEntityId(npcId));
        if (entity != state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        const bool participant = source.actor == npcId || source.target == npcId;
        const bool visible = rule->visibility == "world" ||
                             (rule->visibility == "participants" && participant) ||
                             (rule->visibility == "actor" && source.actor == npcId) ||
                             (rule->visibility == "target" && source.target == npcId);
        if (!visible) continue;
        auto sensitivity = npc.stimulusSensitivity.find(rule->category);
        if (sensitivity == npc.stimulusSensitivity.end()) {
            sensitivity = npc.stimulusSensitivity.emplace(rule->category, content_.stimulusPolicy.maxSensitivity).first;
        }
        const std::int64_t windowStart = state_.now.minute - content_.stimulusPolicy.repeatWindowMinutes;
        const int repetitions = static_cast<int>(std::count_if(
            npc.stimuli.begin(), npc.stimuli.end(), [&](const auto& prior) {
                return prior.active && prior.category == rule->category && prior.actor == source.actor &&
                       prior.observedAt.minute >= windowStart;
            }));
        std::erase_if(npc.stimuli, [this](const auto& stimulus) {
            return !stimulus.active && state_.now.minute - stimulus.expiresAt.minute > 0;
        });
        while (npc.stimuli.size() >= content_.stimulusPolicy.maxActivePerNpc) {
            auto oldest = std::min_element(npc.stimuli.begin(), npc.stimuli.end(),
                                           [](const auto& left, const auto& right) {
                                               if (left.effectiveSalience == right.effectiveSalience) {
                                                   return left.observedAt.minute < right.observedAt.minute;
                                               }
                                               return left.effectiveSalience < right.effectiveSalience;
                                           });
            if (oldest == npc.stimuli.end()) break;
            npc.stimuli.erase(oldest);
        }
        const double novelty = std::pow(content_.stimulusPolicy.noveltyDecay,
                                        std::max(0, repetitions));
        const double sensitivityBefore = sensitivity->second;
        if (repetitions > 0) {
            sensitivity->second = std::clamp(
                sensitivityBefore - content_.stimulusPolicy.habituationStep * repetitions,
                content_.stimulusPolicy.minSensitivity, content_.stimulusPolicy.maxSensitivity);
            if (std::abs(sensitivity->second - sensitivityBefore) >= 0.000001) {
                addEvent("stimulus.habituated", npcId, rule->category,
                         "Repeated stimuli reduced this character's sensitivity",
                         Json{{"category", rule->category}, {"source_type", source.type},
                              {"repetitions", repetitions}, {"before", sensitivityBefore},
                              {"after", sensitivity->second}, {"rule", rule->id}});
            }
        }
        const double salience = std::clamp(rule->relevance * rule->intensity * rule->urgency *
                                               novelty * rule->sourceConfidence, 0.0, 1.0);
        ExternalStimulus stimulus;
        stimulus.id = "stimulus:" + std::to_string(source.sequence) + ":" + npcId;
        stimulus.sourceEvent = source.type;
        stimulus.sourceSequence = source.sequence;
        stimulus.category = rule->category;
        stimulus.actor = source.actor;
        stimulus.target = source.target;
        stimulus.observedAt = state_.now;
        stimulus.expiresAt = GameTime{state_.now.minute + rule->ttlMinutes};
        stimulus.visibilityScope = rule->visibility;
        stimulus.sourceConfidence = rule->sourceConfidence;
        stimulus.relevance = rule->relevance;
        stimulus.intensity = rule->intensity;
        stimulus.urgency = rule->urgency;
        stimulus.novelty = novelty;
        stimulus.valence = rule->valence;
        stimulus.salience = salience;
        stimulus.effectiveSalience = salience * sensitivity->second;
        npc.stimuli.push_back(std::move(stimulus));
    }
}

void WorldService::expireStimuli() {
    for (auto& [id, npc] : state_.npcs) {
        (void)id;
        for (auto& stimulus : npc.stimuli) {
            if (stimulus.active && state_.now.minute >= stimulus.expiresAt.minute) stimulus.active = false;
        }
    }
}

WorldEvent WorldService::addEvent(std::string type, std::string actor, std::string target,
                                  std::string detail, Json data) {
    ++state_.revision;
    state_.events.push_back(WorldEvent{1, state_.events.size() + 1, state_.now, std::move(type), std::move(actor),
                                       std::move(target), std::move(detail), state_.revision, std::move(data)});
    const WorldEvent committed = state_.events.back();
    if (!transactionFrames_.empty()) {
        deferredEvents_.push_back(committed);
        projectStimuli(committed);
    } else {
        projectStimuli(committed);
        eventBus_.publish(committed);
        eventBus_.dispatch();
    }
    return committed;
}

Json WorldService::snapshotJson() const { return worldStateToJson(state_); }

void WorldService::saveSnapshot(const std::string& path) const {
    const std::string temporaryPath = path + ".tmp";
    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write snapshot: " + path);
    output << snapshotJson().dump(2) << '\n';
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("cannot flush snapshot: " + path);
    }
    output.close();
    std::error_code error;
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("cannot replace snapshot: " + path + ": " + error.message());
    }
}

void WorldService::writeReplay(const std::string& path) const {
    const std::string temporaryPath = path + ".tmp";
    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write replay: " + path);
    output << Json{{"kind", "snapshot"}, {"state", snapshotJson()}}.dump() << '\n';
    for (const auto& event : state_.events) output << Json{{"kind", "event"}, {"event", worldEventToJson(event)}}.dump() << '\n';
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("cannot flush replay: " + path);
    }
    output.close();
    std::error_code error;
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("cannot replace replay: " + path + ": " + error.message());
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
        snapshot.at("save_schema_version").get<int>() != 15) {
        throw std::runtime_error("unsupported snapshot schema; expected save_schema_version=15");
    }
    if (!snapshot.contains("simulation_version") || !snapshot.at("simulation_version").is_string() ||
        snapshot.at("simulation_version") != "adaptive-runtime-p1.15") {
        throw std::runtime_error("unsupported snapshot simulation version; expected adaptive-runtime-p1.15");
    }
    service.state_ = deserializeWorldState(snapshot);
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
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto json = Json::parse(line);
        if (json.value("kind", "") == "snapshot") {
            return fromSnapshot(json.at("state"), std::move(contentRoot), std::move(provider));
        }
    }
    throw std::runtime_error("replay has no snapshot");
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
