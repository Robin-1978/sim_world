#include "world/world_service.hpp"
#include "world/world_state_json.hpp"
#include "npc/drive_system.hpp"
#include "npc/state_machine.hpp"
#include "world/ids.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
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
}

class WorldService::TransactionScope {
public:
    explicit TransactionScope(WorldService& service)
        : service_(service), uncaughtAtConstruction_(std::uncaught_exceptions()) {
        service_.beginTransaction();
    }

    ~TransactionScope() {
        if (std::uncaught_exceptions() > uncaughtAtConstruction_) service_.rollbackTransaction();
        else service_.commitTransaction();
    }

    TransactionScope(const TransactionScope&) = delete;
    TransactionScope& operator=(const TransactionScope&) = delete;

private:
    WorldService& service_;
    int uncaughtAtConstruction_;
};

void WorldService::beginTransaction() {
    if (transactionDepth_++ == 0) {
        transactionBackup_ = state_;
        deferredEvents_.clear();
    }
}

void WorldService::commitTransaction() {
    if (--transactionDepth_ != 0) return;
    rebuildEntityRegistry();
    auto events = std::move(deferredEvents_);
    deferredEvents_.clear();
    for (const auto& event : events) eventBus_.publish(event);
    eventBus_.dispatch();
}

void WorldService::rollbackTransaction() {
    if (--transactionDepth_ != 0) return;
    state_ = std::move(transactionBackup_);
    deferredEvents_.clear();
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
    : provider_(provider ? std::move(provider) : std::make_unique<MockProvider>()) {}

WorldService WorldService::load(std::string contentRoot, std::unique_ptr<IProvider> provider) {
    return fromContent(ContentPackLoader::load(contentRoot), std::move(provider));
}

WorldService WorldService::fromContent(const WorldContentPack& content,
                                       std::unique_ptr<IProvider> provider) {
    WorldService service(std::move(provider));
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
        npc.peerTrust = authored.initialPeerTrust;
        if (instance != nullptr) {
            if (const auto override = instance->npcs.find(authored.definition.id); override != instance->npcs.end()) {
                if (!override->second.location.empty()) npc.location = override->second.location;
                if (!override->second.scheduleLabel.empty()) npc.scheduleLabel = override->second.scheduleLabel;
                npc.goalStage = override->second.goalStage;
                if (!override->second.strategy.empty()) npc.strategy = override->second.strategy;
                if (!override->second.peerTrust.empty()) npc.peerTrust = override->second.peerTrust;
            }
        }
        DriveSystem::initialize(npc, authored.needs, service.state_.now);
        npc.memories.push_back(Memory{"authored", "Initial content pack memory.", service.state_.now, 1.0});
        service.state_.npcs.emplace(authored.definition.id, std::move(npc));
        service.state_.entities[npcEntityId(authored.definition.id)] =
            WorldEntityState{npcEntityId(authored.definition.id), "npc", EntityLifecycle::Active, Json::object()};
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
        entities_.set<InventoryComponent>(kPlayerId, InventoryComponent{});
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
        entities_.set<InventoryComponent>(entity, InventoryComponent{});
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
    context.character = authoredDefinition.name + ": " + authoredDefinition.role + "; public goal=" +
                        authoredDefinition.publicGoal + "; hidden goal=" + authoredDefinition.hiddenGoal +
                        "; boundary=" + authoredDefinition.boundary + "; strategy=" + npc.strategy;
    for (const auto& [needId, need] : npc.needs) {
        context.character += "; need(" + needId + ")=" + std::to_string(need.deficit);
    }
    for (const auto& [peer, trust] : npc.peerTrust) {
        context.character += "; trust(" + peer + ")=" + std::to_string(trust);
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
    const auto target = state_.now.minute + minutes;
    while (state_.now.minute < target) {
        // A time step is one state transition. A long offline jump is a
        // sequence of committed steps, not one unbounded transaction; this
        // keeps event publication and observation aligned with the step that
        // produced the fact.
        TransactionScope transaction(*this);
        ++state_.now.minute;
        expireStimuli();
        resumeSuspendedProcesses();
        tickProcesses();
        if (state_.now.minute % kMinutesPerHour == 0) processHourBoundary(state_.now.day(), state_.now.hour());
        if (state_.now.minute % kMinutesPerDay == 0) processDayBoundary(state_.now.day());
        applyPromiseDeadline();
    }
    syncNpcEntityState();
}

void WorldService::processHourBoundary(int day, int hour) {
    (void)day;
    const int minuteOfDay = hour * static_cast<int>(kMinutesPerHour);
    for (auto& [id, npc] : state_.npcs) {
        const auto* authored = authoredNpc(id);
        if (authored != nullptr) {
            const GameTime previous{state_.now.minute - kMinutesPerHour};
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
                         Json{{"changes", changed}, {"hours", 1}, {"rule", "drive.time_decay"},
                              {"from_minute", previous.minute}, {"to_minute", state_.now.minute}});
            }
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
    processAutonomousTurns();
}

void WorldService::processActivity(const ActivityContent& activity) {
    std::map<std::string, int> required;
    for (const auto& effect : activity.effects) {
        if (effect.operation == "consume_resource" || effect.operation == "reserve_resource") {
            required[effect.resource] += effect.amount;
        }
    }
    for (const auto& [resource, amount] : required) {
        if (availableResourceValue(resource) < amount) return;
    }

    Json changes = Json::object();
    for (const auto& effect : activity.effects) {
        if (effect.operation == "relieve_need") {
            if (activity.actor != kWorldActor) applyNeedRelief(activity.actor, effect.effectAction);
            continue;
        }
        if (effect.operation == "set_link_state") {
            state_.links[effect.linkId] = RuntimeLinkState{effect.traversable, effect.locked};
            changes[effect.linkId] = Json{{"traversable", effect.traversable}, {"locked", effect.locked}};
            continue;
        }
        const int before = resourceValue(effect.resource);
        if (effect.operation == "consume_resource") setResource(effect.resource, before - effect.amount);
        if (effect.operation == "produce_resource") setResource(effect.resource, before + effect.amount);
        if (effect.operation == "reserve_resource") state_.resources[effect.resource].reserved += effect.amount;
        if (effect.operation == "release_resource") {
            auto& resource = state_.resources[effect.resource];
            resource.reserved = std::max(0, resource.reserved - effect.amount);
        }
        if (effect.operation != "release_resource") {
            changes[effect.resource] = Json{{"before", before}, {"after", resourceValue(effect.resource)}};
        }
    }
    addEvent(activity.eventType, activity.actor, activity.target, activity.detail,
             Json{{"activity_id", activity.id}, {"changes", changes}});
}

void WorldService::tickProcesses() {
    for (auto& [npcId, npc] : state_.npcs) {
        if (npc.activity.status != ActivityStatus::Active ||
            !npc.activity.process || npc.activity.process->status != ProcessStatus::Running) {
            continue;
        }
        auto& process = *npc.activity.process;
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
            suspendProcess(npcId, process, "new_high_salience_stimulus");
            continue;
        }
        if (state_.now.minute < process.completesAt.minute) continue;
        const auto completed = *npc.activity.process;
        completeProcess(npcId, completed);
    }
}

void WorldService::resumeSuspendedProcesses() {
    for (auto& [npcId, npc] : state_.npcs) {
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
                                  const std::string& reason) {
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
    addEvent("npc.process_suspended", npcId, process.actionId,
             "A process was suspended by a high-salience stimulus",
             Json{{"process_id", process.id}, {"action_id", process.actionId},
                  {"reason", reason}, {"progress", process.progress}});
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
    auto found = state_.npcs.find(npcId);
    if (found == state_.npcs.end()) return;
    const bool applied = applyActionEffects(npcId, process.actionId, &process.reservedResources);
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

void WorldService::processAutonomousTurns() {
    if (!content_.npcDeliberation.enabled) return;
    for (auto& [npcId, npc] : state_.npcs) {
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
        const auto turn = deliberate(npcId, "high_salience_context");
        if (turn.accepted) npc.lastAutonomousTurn = state_.now;
    }
}

void WorldService::processDayBoundary(int day) {
    for (const auto& activity : content_.activities) {
        if (activity.trigger != "day") continue;
        if (!activity.conditionResource.empty() && resourceValue(activity.conditionResource) >= activity.conditionBelow) continue;
        processActivity(activity);
    }
    for (auto& [id, npc] : state_.npcs) {
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
        const auto trust = found->second.peerTrust.find(rule.peerId);
        if (trust == found->second.peerTrust.end() || trust->second < rule.minPeerTrust) continue;
        auto& npc = found->second;
        npc.strategy = rule.strategy;
        npc.goalStage = std::max(npc.goalStage, rule.goalStage);
        npc.memories.push_back(Memory{"plan", rule.memory, state_.now, trust->second});
        ++npc.stateVersion;
        addEvent(rule.eventType, rule.npcId, rule.peerId, rule.memory,
                 Json{{"strategy", npc.strategy}, {"rule", rule.id}, {"peer_trust", trust->second}, {"day", day}});
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
                applyRelationshipDelta(from->second.playerRelation, content_.promisePolicy.brokenPromiser);
                ++from->second.stateVersion;
            }
            if (to != state_.npcs.end()) {
                applyRelationshipDelta(to->second.playerRelation, content_.promisePolicy.brokenPromisee);
                ++to->second.stateVersion;
            }
            addEvent("promise.broken", promise.from, promise.to, "A promise reached its deadline", Json{{"promise_id", id}});
        }
    }
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
    addEvent("promise.created", from, to, description, Json{{"promise_id", id}, {"due", promise.due.minute}});
    return CommandResult{true, "ok", id, state_.events.back()};
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
        if (decision == PromiseStatus::Refused || decision == PromiseStatus::Broken) {
            applyRelationshipDelta(npc->second.playerRelation, content_.promisePolicy.refusedActor);
        } else {
            applyRelationshipDelta(npc->second.playerRelation, content_.promisePolicy.acceptedActor);
        }
    }
    addEvent("promise." + toString(decision), actor, promiseId, detail.empty() ? "Promise status changed" : detail,
             Json{{"promise_id", promiseId}, {"status", toString(decision)}, {"version", promise.version}});
    return CommandResult{true, "ok", toString(decision), state_.events.back()};
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
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    addEvent("entity.created", kWorldActor, id, "Runtime entity created",
             Json{{"entity_id", id}, {"archetype", archetypeId}, {"kind", archetype->kind},
                  {"metadata", state_.entities.at(id).metadata}});
    return CommandResult{true, "ok", id, state_.events.back()};
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
    addEvent("entity.lifecycle", kWorldActor, id, "Runtime entity lifecycle changed",
             Json{{"entity_id", id}, {"lifecycle", toString(lifecycle)}});
    return CommandResult{true, "ok", toString(lifecycle), state_.events.back()};
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
    addEvent("link.state", kWorldActor, linkEntityId(linkId), "Traversal link state changed",
             Json{{"link_id", linkId}, {"traversable", traversable}, {"locked", locked}});
    return CommandResult{true, "ok", linkId, state_.events.back()};
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
    return executeTurn(npcId, playerInput, turnId, "player_dialogue", "dialogue.turn");
}

DialogueTurn WorldService::deliberate(const std::string& npcId, const std::string& trigger,
                                      const std::string& turnId) {
    return executeTurn(npcId, {}, turnId, trigger, "npc.autonomous_intent");
}

DialogueTurn WorldService::executeTurn(const std::string& npcId, const std::string& input,
                                       const std::string& turnId, const std::string& trigger,
                                       const std::string& eventType) {
    TransactionScope transaction(*this);
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
    const auto context = decisionContext(npcId);
    auto response = provider_->decide(context, input);
    if (!response.ok) {
        // Rule fallback keeps the world playable when the local server is down.
        const std::string providerError = response.error;
        response = MockProvider{}.decide(context, input);
        result.provider += "+fallback";
        result.error = "provider_fallback:" + providerError;
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
        if (expected < 0.0 || expected > 1.0 || !state_.npcs.at(npcId).needs.contains(needId)) {
            result.error = "invalid_expected_relief";
            return result;
        }
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
    auto& npc = state_.npcs.at(npcId);
    if (npc.activity.process) {
        double incomingSalience = trigger == "player_dialogue" ? 1.0 : 0.0;
        for (const auto& stimulus : context.stimuli) {
            incomingSalience = std::max(incomingSalience, stimulus.effectiveSalience);
        }
        if (!npc.activity.process->interruptible ||
            incomingSalience < npc.activity.process->interruptSalienceThreshold) {
            result.error = "npc_busy";
            return result;
        }
        cancelProcess(npcId, *npc.activity.process, trigger);
        npc.activity = NpcActivity{};
    }
    std::map<std::string, int> reservations;
    if (candidate->durationMinutes > 0) {
        for (const auto& activity : content_.activities) {
            for (const auto& effect : activity.effects) {
                if (effect.effectAction == candidate->actionId && effect.operation == "consume_resource") {
                    reservations[effect.resource] += effect.amount;
                }
            }
        }
        for (const auto& [resource, amount] : reservations) {
            if (availableResourceValue(resource) < amount) {
                result.error = "insufficient_resource";
                return result;
            }
        }
    }
    const std::string processId =
        "process:" + npcId + ":" + (turnId.empty() ? std::to_string(state_.events.size() + 1) : turnId);
    if (candidate->durationMinutes <= 0 && !applyActionEffects(npcId, candidate->actionId)) {
        result.error = "insufficient_resource";
        return result;
    }
    result.accepted = true;
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    ++npc.stateVersion;
    if (candidate->durationMinutes > 0) {
        for (const auto& [resource, amount] : reservations) state_.resources[resource].reserved += amount;
        const std::string goalId = "goal:" + npcId + ":" + processId.substr(processId.find(':') + 1);
        npc.activity.goal = GoalState{goalId, response.proposal.intent, candidate->actionId,
                                  response.proposal.confidence, GoalStatus::Active, state_.now,
                                  GameTime{state_.now.minute + candidate->durationMinutes},
                                  response.proposal.rationale};
        ProcessExecution process;
        process.id = processId;
        process.goalId = goalId;
        process.actionId = candidate->actionId;
        process.planId = candidate->planId;
        process.trigger = trigger;
        process.startedAt = state_.now;
        process.completesAt = GameTime{state_.now.minute + candidate->durationMinutes};
        process.behaviorNodeId = candidate->planId;
        process.interruptible = candidate->interruptible;
        process.interruptSalienceThreshold = candidate->interruptSalienceThreshold;
        process.reservedResources = reservations;
        process.requiredResources = reservations;
        process.proposalWorldRevision = response.proposal.worldRevision;
        process.proposalNpcStateVersion = response.proposal.npcStateVersion;
        npc.activity.process = std::move(process);
        setActivityStatus(npc.activity, ActivityStatus::Active);
        applyNpcSignal(npc, NpcSignal::StartProcess);
    }
    npc.memories.push_back(Memory{trigger == "player_dialogue" ? "conversation" : "intent",
                                  input.empty() ? response.proposal.intent : input, state_.now, 1.0});
    addEvent(eventType, npcId, trigger == "player_dialogue" ? kPlayerId : "self", response.dialogue,
             Json{{"turn_id", turnId}, {"process_id", processId}, {"action_id", response.proposal.actionId},
                  {"trigger", trigger}, {"duration_minutes", candidate->durationMinutes},
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
    result.event = state_.events.back();
    syncNpcEntityState();
    return result;
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

bool WorldService::applyActionEffects(const std::string& npcId, const std::string& actionId) {
    return applyActionEffects(npcId, actionId, nullptr);
}

bool WorldService::applyActionEffects(const std::string& npcId, const std::string& actionId,
                                      const std::map<std::string, int>* reservedResources) {
    std::map<std::string, int> required;
    for (const auto& activity : content_.activities) {
        for (const auto& effect : activity.effects) {
            if (effect.effectAction == actionId &&
                (effect.operation == "consume_resource" || effect.operation == "reserve_resource")) {
                required[effect.resource] += effect.amount;
            }
        }
    }
    for (const auto& [resource, amount] : required) {
        if (reservedResources != nullptr) {
            const auto reserved = reservedResources->find(resource);
            if (reserved == reservedResources->end() || reserved->second < amount) return false;
        } else if (availableResourceValue(resource) < amount) {
            return false;
        }
    }
    for (const auto& activity : content_.activities) {
        for (const auto& effect : activity.effects) {
            if (effect.effectAction != actionId) continue;
            if (effect.operation == "relieve_need") {
                applyNeedRelief(npcId, actionId);
                continue;
            }
            if (effect.operation == "set_link_state") {
                state_.links[effect.linkId] = RuntimeLinkState{effect.traversable, effect.locked};
                addEvent("action.structure", npcId, effect.linkId, "Authored action changed a traversal link",
                         Json{{"action_id", actionId}, {"link_id", effect.linkId},
                              {"traversable", effect.traversable}, {"locked", effect.locked}});
                continue;
            }
            const int before = resourceValue(effect.resource);
            if (effect.operation == "consume_resource") {
                setResource(effect.resource, before - effect.amount);
                if (reservedResources != nullptr) {
                    auto& reserved = state_.resources[effect.resource].reserved;
                    reserved = std::max(0, reserved - effect.amount);
                }
            }
            if (effect.operation == "produce_resource") setResource(effect.resource, before + effect.amount);
            if (effect.operation == "reserve_resource") state_.resources[effect.resource].reserved += effect.amount;
            if (effect.operation == "release_resource") {
                auto& resource = state_.resources[effect.resource];
                resource.reserved = std::max(0, resource.reserved - effect.amount);
            }
            if (effect.operation == "consume_resource" || effect.operation == "produce_resource") {
                addEvent("action.resource", npcId, effect.resource, "Authored action changed a resource",
                         Json{{"action_id", actionId}, {"activity_id", activity.id}, {"resource", effect.resource},
                              {"before", before}, {"after", resourceValue(effect.resource)}});
            }
        }
    }
    return true;
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

void WorldService::addEvent(std::string type, std::string actor, std::string target,
                            std::string detail, Json data) {
    ++state_.revision;
    state_.events.push_back(WorldEvent{1, state_.events.size() + 1, state_.now, std::move(type), std::move(actor),
                                       std::move(target), std::move(detail), state_.revision, std::move(data)});
    const WorldEvent committed = state_.events.back();
    if (transactionDepth_ > 0) {
        deferredEvents_.push_back(committed);
        projectStimuli(committed);
    } else {
        projectStimuli(committed);
        eventBus_.publish(committed);
        eventBus_.dispatch();
    }
}

Json WorldService::snapshotJson() const { return worldStateToJson(state_); }

void WorldService::saveSnapshot(const std::string& path) const {
    std::ofstream output(path);
    if (!output) throw std::runtime_error("cannot write snapshot: " + path);
    output << snapshotJson().dump(2) << '\n';
}

void WorldService::writeReplay(const std::string& path) const {
    std::ofstream output(path);
    if (!output) throw std::runtime_error("cannot write replay: " + path);
    output << Json{{"kind", "snapshot"}, {"state", snapshotJson()}}.dump() << '\n';
    for (const auto& event : state_.events) output << Json{{"kind", "event"}, {"event", worldEventToJson(event)}}.dump() << '\n';
}

WorldService WorldService::fromSnapshot(const Json& json, std::string contentRoot,
                                        std::unique_ptr<IProvider> provider) {
    WorldService service(std::move(provider));
    service.content_ = ContentPackLoader::load(contentRoot);
    const auto& snapshot = json.contains("state") ? json.at("state") : json;
    if (!snapshot.is_object() || !snapshot.contains("save_schema_version") ||
        !snapshot.at("save_schema_version").is_number_integer() ||
        snapshot.at("save_schema_version").get<int>() != 10) {
        throw std::runtime_error("unsupported snapshot schema; expected save_schema_version=10");
    }
    if (!snapshot.contains("simulation_version") || !snapshot.at("simulation_version").is_string() ||
        snapshot.at("simulation_version") != "adaptive-runtime-p0.10") {
        throw std::runtime_error("unsupported snapshot simulation version; expected adaptive-runtime-p0.10");
    }
    service.state_ = deserializeWorldState(snapshot);
    for (const auto& [id, npc] : service.state_.npcs) {
        if (npc.definitionId.empty() || service.authoredNpc(npc.definitionId) == nullptr) {
            throw std::runtime_error("snapshot references unknown npc definition: " + npc.definitionId);
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
    if (service.state_.contentVersion.empty() ||
        service.state_.contentVersion != service.content_.contentVersion) {
        throw std::runtime_error("snapshot content version " + service.state_.contentVersion +
                                 " does not match loaded content " + service.content_.contentVersion);
    }
    service.rebuildEntityRegistry();
    return service;
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
