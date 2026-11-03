#include "world/world_service.hpp"
#include "world/world_state_json.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <exception>

namespace tide_marks {

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
    service.state_.worldId = content.worldId;
    service.state_.contentVersion = content.contentVersion;
    service.state_.now = content.startTime;
    service.state_.village = content.initialVillage;
    service.state_.entities["player"] = WorldEntityState{"player", "actor", EntityLifecycle::Active, Json::object()};
    for (const auto& spatial : content.spatialEntities) {
        service.state_.entities["spatial:" + spatial.id] =
            WorldEntityState{"spatial:" + spatial.id, spatial.kind, EntityLifecycle::Active, Json::object()};
    }
    for (const auto& link : content.links) {
        service.state_.entities["link:" + link.id] =
            WorldEntityState{"link:" + link.id, link.kind, EntityLifecycle::Active, Json::object()};
        service.state_.links[link.id] = RuntimeLinkState{link.link.traversable, link.link.locked};
    }
    for (const auto& authored : content.npcs) {
        NpcState npc{authored.definition, authored.initialLocation, authored.initialScheduleLabel,
                     authored.initialGoalStage, authored.initialResource, authored.initialPressure,
                     RelationshipAxes{}, {}, {}, 0, "working", authored.initialStrategy, authored.initialPeerTrust};
        npc.memories.push_back(Memory{"authored", "Initial content pack memory.", service.state_.now, 1.0});
        service.state_.npcs.emplace(authored.definition.id, std::move(npc));
        service.state_.entities["npc:" + authored.definition.id] =
            WorldEntityState{"npc:" + authored.definition.id, "npc", EntityLifecycle::Active, Json::object()};
    }
    service.rebuildEntityRegistry();
    Json ids = Json::array();
    for (const auto& authored : content.npcs) ids.push_back(authored.definition.id);
    service.addEvent("world.initialized", "world", content.id, "World content pack loaded",
                     Json{{"content_pack", content.id}, {"content_version", content.version}, {"npcs", ids}});
    return service;
}

void WorldService::rebuildEntityRegistry() {
    entities_ = EntityRegistry{};
    const auto playerState = state_.entities.find("player");
    if (playerState == state_.entities.end() || playerState->second.lifecycle != EntityLifecycle::Destroyed) {
        entities_.create(EntityRecord{"player", "player", playerState == state_.entities.end() ||
                                                   playerState->second.lifecycle == EntityLifecycle::Active});
    }
    if (entities_.contains("player")) entities_.set<TransformComponent>("player", TransformComponent{0.0, 0.0, 0.0, 0.0});
    const auto defaultLocation = content_.spatialEntities.empty() ? std::string{} : content_.spatialEntities.front().id;
    const auto regionId = state_.worldId.empty() ? content_.worldId : state_.worldId;
    if (entities_.contains("player")) {
        entities_.set<LocationComponent>("player", LocationComponent{defaultLocation, regionId});
        entities_.set<InventoryComponent>("player", InventoryComponent{});
    }
    for (const auto& authored : content_.spatialEntities) {
        const auto entity = "spatial:" + authored.id;
        const auto stateEntity = state_.entities.find(entity);
        if (stateEntity != state_.entities.end() && stateEntity->second.lifecycle == EntityLifecycle::Destroyed) continue;
        const bool active = stateEntity == state_.entities.end() || stateEntity->second.lifecycle == EntityLifecycle::Active;
        if (!entities_.create(EntityRecord{entity, authored.kind, active})) continue;
        entities_.set<SpatialComponent>(entity, authored.spatial);
        entities_.set<LocationComponent>(entity, LocationComponent{authored.id, regionId});
    }
    for (const auto& authored : content_.links) {
        const auto entity = "link:" + authored.id;
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
        const auto entity = "npc:" + id;
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
        const auto entity = "npc:" + id;
        entities_.set<NpcComponent>(entity, NpcComponent{id, npc.mode});
        entities_.set<LocationComponent>(entity, LocationComponent{npc.location, state_.worldId});
    }
}

bool WorldService::hasNpc(const std::string& npcId) const {
    if (state_.npcs.find(npcId) == state_.npcs.end()) return false;
    const auto entity = state_.entities.find("npc:" + npcId);
    return entity == state_.entities.end() || entity->second.lifecycle == EntityLifecycle::Active;
}

DecisionContext WorldService::decisionContext(const std::string& npcId) const {
    if (!hasNpc(npcId)) return {};
    const auto found = state_.npcs.find(npcId);
    if (found == state_.npcs.end()) return {};
    const auto& npc = found->second;
    DecisionContext context;
    context.npcId = npcId;
    context.now = state_.now;
    context.worldRevision = state_.revision;
    context.npcStateVersion = npc.stateVersion;
    context.perception = "At " + npc.location + ", it is " + state_.now.toString() + ". Grain=" +
                         std::to_string(state_.village.grain) + ", boat=" +
                         std::to_string(state_.village.boatIntegrity) + ", records=" +
                         std::to_string(state_.village.recordsIntegrity) + ".";
    std::vector<std::string> memories;
    for (const auto& memory : npc.memories) memories.push_back(memory.text);
    context.memories = join(memories, " | ");
    context.character = npc.definition.name + ": " + npc.definition.role + "; public goal=" +
                        npc.definition.publicGoal + "; hidden goal=" + npc.definition.hiddenGoal +
                        "; boundary=" + npc.definition.boundary + "; pressure=" +
                        std::to_string(npc.pressure) + "; strategy=" + npc.strategy;
    for (const auto& [peer, trust] : npc.peerTrust) {
        context.character += "; trust(" + peer + ")=" + std::to_string(trust);
    }
    for (const auto& authored : content_.npcs) {
        if (authored.definition.id == npcId) {
            context.candidates = authored.actions;
            break;
        }
    }
    // Utility ranking chooses the long-term intent deterministically. The
    // provider may still choose any authored candidate after seeing this list.
    const auto ranked = GoalSelector::rank(npc, state_.village, state_.now);
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
    TransactionScope transaction(*this);
    const auto target = state_.now.minute + minutes;
    while (state_.now.minute < target) {
        ++state_.now.minute;
        if (state_.now.minute % 60 == 0) processHourBoundary(state_.now.day(), state_.now.hour());
        if (state_.now.minute % 1440 == 0) processDayBoundary(state_.now.day());
        applyPromiseDeadline();
    }
    syncNpcEntityState();
}

void WorldService::processHourBoundary(int day, int hour) {
    const int minuteOfDay = hour * 60;
    for (auto& [id, npc] : state_.npcs) {
        const auto entity = state_.entities.find("npc:" + id);
        if (entity != state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        std::string scheduledLocation = npc.definition.home;
        for (const auto& authored : content_.npcs) {
            if (authored.definition.id != npc.definition.id) continue;
            for (const auto& slot : authored.schedule) {
                const bool within = slot.startMinute < slot.endMinute
                                        ? minuteOfDay >= slot.startMinute && minuteOfDay < slot.endMinute
                                        : (slot.startMinute == slot.endMinute || minuteOfDay >= slot.startMinute || minuteOfDay < slot.endMinute);
                if (within) {
                    scheduledLocation = slot.location;
                    break;
                }
            }
            break;
        }
        if (npc.location != scheduledLocation) ++npc.stateVersion;
        npc.location = scheduledLocation;
        npc.scheduleLabel = scheduledLocation;
        npc.mode = npc.location == npc.definition.home ? "at_home" : "working";
    }
    auto miraFound = state_.npcs.find("mira");
    auto lornFound = state_.npcs.find("lorn");
    auto eveFound = state_.npcs.find("eve");
    if (hour == 8) {
        const int ration = std::min(5, state_.village.grain);
        state_.village.grain -= ration;
        if (miraFound != state_.npcs.end()) {
            miraFound->second.resource = state_.village.grain;
            miraFound->second.pressure = state_.village.grain < 70 ? 0.8 : 0.3;
        }
        addEvent("life.ration", "mira", "village", "Mira records the morning ration",
                 Json{{"grain_used", ration}, {"grain_remaining", state_.village.grain}, {"day", day}});
    }
    if (hour == 10 && state_.village.boatIntegrity < 100) {
        if (state_.village.grain >= 5) state_.village.grain -= 5;
        state_.village.boatIntegrity = std::min(100, state_.village.boatIntegrity + 8);
        if (lornFound != state_.npcs.end()) lornFound->second.resource = state_.village.boatIntegrity;
        double previousTrust = 0.0;
        if (miraFound != state_.npcs.end()) {
            previousTrust = miraFound->second.peerTrust["lorn"];
            miraFound->second.peerTrust["lorn"] = std::min(1.0, previousTrust + 0.12);
            miraFound->second.pressure = std::max(0.0, miraFound->second.pressure - 0.05);
            miraFound->second.memories.push_back(Memory{"relationship", "Lorn spent village grain to keep the ferry running.", state_.now, 0.95});
            ++miraFound->second.stateVersion;
        }
        ++state_.village.cooperation;
        addEvent("life.repair", "lorn", "ferry", "Lorn patches the ferry before the next crossing",
                 Json{{"boat_integrity", state_.village.boatIntegrity}});
        const double nextTrust = miraFound == state_.npcs.end() ? previousTrust : miraFound->second.peerTrust["lorn"];
        addEvent("social.cooperation", "lorn", "mira", "Lorn spends grain and repairs the crossing for the village",
                 Json{{"peer", "lorn"}, {"trust_before", previousTrust}, {"trust_after", nextTrust}});
    }
    if (hour == 14) {
        state_.village.recordsIntegrity = std::min(100, state_.village.recordsIntegrity + 2);
        if (eveFound != state_.npcs.end()) eveFound->second.resource = state_.village.recordsIntegrity;
        addEvent("life.archive", "eve", "archive", "Eve indexes the tide registry",
                 Json{{"records_integrity", state_.village.recordsIntegrity}});
    }
}

void WorldService::processDayBoundary(int day) {
    state_.village.boatIntegrity = std::max(0, state_.village.boatIntegrity - 3);
    auto miraFound = state_.npcs.find("mira");
    auto lornFound = state_.npcs.find("lorn");
    auto eveFound = state_.npcs.find("eve");
    if (miraFound != state_.npcs.end()) miraFound->second.resource = state_.village.grain;
    if (lornFound != state_.npcs.end()) lornFound->second.resource = state_.village.boatIntegrity;
    if (eveFound != state_.npcs.end()) eveFound->second.resource = state_.village.recordsIntegrity;
    if (state_.village.grain < 85) {
        if (miraFound != state_.npcs.end()) {
            miraFound->second.goalStage = std::max(miraFound->second.goalStage, 1);
            miraFound->second.pressure = std::min(1.0, miraFound->second.pressure + 0.12);
        }
    }
    if (state_.village.boatIntegrity < 70) {
        if (lornFound != state_.npcs.end()) {
            lornFound->second.goalStage = std::max(lornFound->second.goalStage, 1);
            lornFound->second.pressure = std::min(1.0, lornFound->second.pressure + 0.1);
        }
    }
    if (state_.village.recordsIntegrity > 80 || day >= 2) {
        if (eveFound != state_.npcs.end()) {
            eveFound->second.goalStage = std::max(eveFound->second.goalStage, 1);
            eveFound->second.pressure = std::min(1.0, eveFound->second.pressure + 0.08);
        }
    }
    for (auto& [id, npc] : state_.npcs) {
        const auto entity = state_.entities.find("npc:" + id);
        if (entity != state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        ++npc.stateVersion;
        npc.memories.push_back(Memory{"episode", "A day passed while the world kept working.", state_.now, 1.0});
    }
    reviseNpcStrategies(day);
    addEvent("life.day", "world", "village", "The village completes another day",
             Json{{"day", day}, {"grain", state_.village.grain}, {"boat_integrity", state_.village.boatIntegrity},
                  {"records_integrity", state_.village.recordsIntegrity}});
}

void WorldService::reviseNpcStrategies(int day) {
    auto found = state_.npcs.find("mira");
    if (found == state_.npcs.end()) return;
    auto& mira = found->second;
    if (mira.strategy != "cooperative_transport" && mira.peerTrust["lorn"] >= 0.45) {
        mira.strategy = "cooperative_transport";
        mira.goalStage = std::max(mira.goalStage, 2);
        mira.pressure = std::max(0.0, mira.pressure - 0.08);
        mira.memories.push_back(Memory{"plan", "Mira trusts Lorn enough to coordinate transport with him.", state_.now, 0.9});
        ++mira.stateVersion;
        addEvent("npc.goal_revised", "mira", "lorn", "Mira changes her next-day plan to cooperative transport",
                 Json{{"strategy", mira.strategy}, {"reason", "repeated_cooperation"}, {"day", day}});
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
                from->second.pressure = std::min(1.0, from->second.pressure + 0.2);
                from->second.playerRelation.trust = std::max(0.0, from->second.playerRelation.trust - 0.08);
                ++from->second.stateVersion;
            }
            if (to != state_.npcs.end()) {
                to->second.pressure = std::min(1.0, to->second.pressure + 0.2);
                to->second.playerRelation.respect = std::max(0.0, to->second.playerRelation.respect - 0.08);
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
    TransactionScope transaction(*this);
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
        try {
            const auto minutes = std::stoll(command.detail);
            advanceMinutes(minutes);
        } catch (...) {
            return reject("invalid_duration", "Advance duration must be minutes.");
        }
        if (!command.turnId.empty()) state_.processedTurnIds.insert(command.turnId);
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
    const bool validFrom = from == "player" || hasNpc(from);
    const bool validTo = to == "player" || hasNpc(to);
    if (!validFrom || !validTo || from == to) return reject("unknown_actor", "Both promise parties must be distinct.");
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The turn was already applied.");
    const std::string id = "promise-" + std::to_string(state_.promises.size() + 1);
    Promise promise{id, from, to, description,
                    GameTime{state_.now.minute + static_cast<std::int64_t>(std::max(1, dueInDays)) * 1440},
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
    if (actor != "player" && actor != promise.from && actor != promise.to) {
        return reject("unauthorized", "Only a promise party can change it.");
    }
    if (promise.status == PromiseStatus::Broken || promise.status == PromiseStatus::Refused || promise.status == PromiseStatus::Fulfilled) {
        return reject("stale_promise", "That promise is already closed.");
    }
    if (decision == PromiseStatus::Renegotiated) {
        promise.due.minute += 1440;
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
            npc->second.playerRelation.trust = std::max(0.0, npc->second.playerRelation.trust - 0.04);
        } else {
            npc->second.playerRelation.trust = std::min(1.0, npc->second.playerRelation.trust + 0.03);
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
    addEvent("entity.created", "world", id, "Runtime entity created",
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
    addEvent("entity.lifecycle", "world", id, "Runtime entity lifecycle changed",
             Json{{"entity_id", id}, {"lifecycle", toString(lifecycle)}});
    return CommandResult{true, "ok", toString(lifecycle), state_.events.back()};
}

CommandResult WorldService::setLinkState(const std::string& linkId, bool traversable, bool locked,
                                         const std::string& turnId) {
    TransactionScope transaction(*this);
    const auto entity = state_.entities.find("link:" + linkId);
    if (entity == state_.entities.end()) return reject("unknown_link", "That link does not exist.");
    if (entity->second.lifecycle != EntityLifecycle::Active) return reject("inactive_link", "The link is not active.");
    if (!turnId.empty() && state_.processedTurnIds.contains(turnId)) return reject("duplicate_turn", "The turn was already applied.");
    state_.links[linkId] = RuntimeLinkState{traversable, locked};
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    addEvent("link.state", "world", "link:" + linkId, "Traversal link state changed",
             Json{{"link_id", linkId}, {"traversable", traversable}, {"locked", locked}});
    return CommandResult{true, "ok", linkId, state_.events.back()};
}

std::optional<std::vector<std::string>> WorldService::planRoute(const std::string& from,
                                                                const std::string& to) const {
    GlobalPlanner planner;
    for (const auto& spatial : content_.spatialEntities) planner.addNode(spatial.id);
    for (const auto& authored : content_.links) {
        const auto entity = state_.entities.find("link:" + authored.id);
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
    auto response = provider_->decide(context, playerInput);
    if (!response.ok) {
        // Rule fallback keeps the world playable when the local server is down.
        const std::string providerError = response.error;
        response = MockProvider{}.decide(context, playerInput);
        result.provider += "+fallback";
        result.error = "provider_fallback:" + providerError;
    }
    const auto validation = ActionValidator::validate(context, response.actionId);
    if (!validation.valid) {
        result.error = validation.code;
        return result;
    }
    if (response.actionId == "offer_ration") {
        if (state_.village.grain < 5) {
            result.error = "insufficient_grain";
            return result;
        }
        state_.village.grain -= 5;
        state_.village.reputation += 1;
    } else if (response.actionId == "repair_ferry") {
        state_.village.boatIntegrity = std::min(100, state_.village.boatIntegrity + 10);
    } else if (response.actionId == "share_record") {
        state_.village.recordsIntegrity = std::max(0, state_.village.recordsIntegrity - 1);
        state_.npcs.at(npcId).goalStage = std::max(1, state_.npcs.at(npcId).goalStage);
    } else if (response.actionId == "protect_secret" || response.actionId == "reveal_clue") {
        state_.npcs.at(npcId).goalStage = std::max(1, state_.npcs.at(npcId).goalStage);
    }
    if (!turnId.empty()) state_.processedTurnIds.insert(turnId);
    ++state_.npcs.at(npcId).stateVersion;
    state_.npcs.at(npcId).memories.push_back(Memory{"conversation", playerInput, state_.now, 1.0});
    addEvent("dialogue.turn", npcId, "player", response.dialogue,
             Json{{"turn_id", turnId}, {"action_id", response.actionId}, {"intent", response.intent},
                  {"motivation", response.motivation}, {"provider", result.provider}});
    result.accepted = true;
    result.actionId = response.actionId;
    result.intent = response.intent;
    result.motivation = response.motivation;
    result.dialogue = response.dialogue;
    result.event = state_.events.back();
    syncNpcEntityState();
    return result;
}

void WorldService::addEvent(std::string type, std::string actor, std::string target,
                            std::string detail, Json data) {
    ++state_.revision;
    state_.events.push_back(WorldEvent{1, state_.events.size() + 1, state_.now, std::move(type), std::move(actor),
                                       std::move(target), std::move(detail), state_.revision, std::move(data)});
    if (transactionDepth_ > 0) {
        deferredEvents_.push_back(state_.events.back());
    } else {
        eventBus_.publish(state_.events.back());
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
    service.state_ = deserializeWorldState(json.contains("state") ? json.at("state") : json);
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
    output << state.now.toString() << "; grain=" << state.village.grain
           << "; boat=" << state.village.boatIntegrity << "; records=" << state.village.recordsIntegrity
           << "; revision=" << state.revision;
    for (const auto& [id, npc] : state.npcs) {
        output << "; " << id << "@" << npc.location << ":stage=" << npc.goalStage
               << ":pressure=" << npc.pressure;
    }
    return output.str();
}


}  // namespace tide_marks
