#include "world/world_state_json.hpp"

#include <stdexcept>

namespace tide_marks {
namespace {

template <typename T>
T jsonValue(const Json& value, const char* key, T fallback) {
    if (!value.contains(key) || value.at(key).is_null()) return fallback;
    return value.at(key).get<T>();
}

Json memoryJson(const Memory& memory) {
    return Json{{"kind", memory.kind}, {"text", memory.text}, {"minute", memory.at.minute},
                {"confidence", memory.confidence}};
}

Memory memoryFromJson(const Json& json) {
    return Memory{jsonValue<std::string>(json, "kind", "fact"), jsonValue<std::string>(json, "text", ""),
                  GameTime{jsonValue<std::int64_t>(json, "minute", 0)},
                  jsonValue<double>(json, "confidence", 1.0)};
}

Json stimulusJson(const ExternalStimulus& stimulus) {
    return Json{{"id", stimulus.id}, {"source_event", stimulus.sourceEvent},
                {"source_sequence", stimulus.sourceSequence}, {"category", stimulus.category},
                {"actor", stimulus.actor}, {"target", stimulus.target},
                {"observed_at", stimulus.observedAt.minute}, {"expires_at", stimulus.expiresAt.minute},
                {"visibility_scope", stimulus.visibilityScope},
                {"source_confidence", stimulus.sourceConfidence}, {"relevance", stimulus.relevance},
                {"intensity", stimulus.intensity}, {"urgency", stimulus.urgency},
                {"novelty", stimulus.novelty}, {"valence", stimulus.valence},
                {"salience", stimulus.salience}, {"effective_salience", stimulus.effectiveSalience},
                {"active", stimulus.active}};
}

Json processJson(const ProcessExecution& process) {
    Json reserved = Json::object();
    for (const auto& [resource, amount] : process.reservedResources) reserved[resource] = amount;
    return Json{{"id", process.id}, {"goal_id", process.goalId}, {"action_id", process.actionId}, {"plan_id", process.planId},
                {"trigger", process.trigger}, {"started_at", process.startedAt.minute},
                {"completes_at", process.completesAt.minute}, {"suspended_at", process.suspendedAt.minute},
                {"progress", process.progress},
                {"status", toString(process.status)}, {"interruption_reason", process.interruptionReason},
                {"behavior_node_id", process.behaviorNodeId}, {"behavior_step", process.behaviorStep},
                {"behavior_status", process.behaviorStatus},
                {"interruptible", process.interruptible},
                {"interrupt_salience_threshold", process.interruptSalienceThreshold},
                {"reserved_resources", reserved},
                {"required_resources", [&process] { Json required = Json::object(); for (const auto& [resource, amount] : process.requiredResources) required[resource] = amount; return required; }()},
                {"proposal_world_revision", process.proposalWorldRevision},
                {"proposal_npc_state_version", process.proposalNpcStateVersion}};
}

ProcessExecution processFromJson(const Json& json) {
    ProcessExecution process;
    process.id = jsonValue<std::string>(json, "id", "");
    process.goalId = jsonValue<std::string>(json, "goal_id", "");
    process.actionId = jsonValue<std::string>(json, "action_id", "");
    process.planId = jsonValue<std::string>(json, "plan_id", "");
    process.trigger = jsonValue<std::string>(json, "trigger", "");
    process.startedAt = GameTime{jsonValue<std::int64_t>(json, "started_at", 0)};
    process.completesAt = GameTime{jsonValue<std::int64_t>(json, "completes_at", 0)};
    process.suspendedAt = GameTime{jsonValue<std::int64_t>(json, "suspended_at", -1)};
    process.progress = jsonValue<double>(json, "progress", 0.0);
    process.status = processStatusFromString(jsonValue<std::string>(json, "status", "running"));
    process.interruptionReason = jsonValue<std::string>(json, "interruption_reason", "");
    process.behaviorNodeId = jsonValue<std::string>(json, "behavior_node_id", process.planId);
    process.behaviorStep = jsonValue<std::size_t>(json, "behavior_step", 0);
    process.behaviorStatus = jsonValue<std::string>(json, "behavior_status", "running");
    process.interruptible = jsonValue<bool>(json, "interruptible", true);
    process.interruptSalienceThreshold = jsonValue<double>(json, "interrupt_salience_threshold", 1.0);
    const auto reserved = json.value("reserved_resources", Json::object());
    for (auto it = reserved.begin(); it != reserved.end(); ++it) {
        process.reservedResources[it.key()] = it.value().get<int>();
    }
    const auto required = json.value("required_resources", Json::object());
    for (auto it = required.begin(); it != required.end(); ++it) {
        process.requiredResources[it.key()] = it.value().get<int>();
    }
    process.proposalWorldRevision = jsonValue<std::uint64_t>(json, "proposal_world_revision", 0);
    process.proposalNpcStateVersion = jsonValue<std::uint64_t>(json, "proposal_npc_state_version", 0);
    return process;
}

ExternalStimulus stimulusFromJson(const Json& json) {
    ExternalStimulus stimulus;
    stimulus.id = jsonValue<std::string>(json, "id", "");
    stimulus.sourceEvent = jsonValue<std::string>(json, "source_event", "");
    stimulus.sourceSequence = jsonValue<std::uint64_t>(json, "source_sequence", 0);
    stimulus.category = jsonValue<std::string>(json, "category", "");
    stimulus.actor = jsonValue<std::string>(json, "actor", "");
    stimulus.target = jsonValue<std::string>(json, "target", "");
    stimulus.observedAt = GameTime{jsonValue<std::int64_t>(json, "observed_at", 0)};
    stimulus.expiresAt = GameTime{jsonValue<std::int64_t>(json, "expires_at", 0)};
    stimulus.visibilityScope = jsonValue<std::string>(json, "visibility_scope", "world");
    stimulus.sourceConfidence = jsonValue<double>(json, "source_confidence", 1.0);
    stimulus.relevance = jsonValue<double>(json, "relevance", 1.0);
    stimulus.intensity = jsonValue<double>(json, "intensity", 1.0);
    stimulus.urgency = jsonValue<double>(json, "urgency", 1.0);
    stimulus.novelty = jsonValue<double>(json, "novelty", 1.0);
    stimulus.valence = jsonValue<double>(json, "valence", 0.0);
    stimulus.salience = jsonValue<double>(json, "salience", 0.0);
    stimulus.effectiveSalience = jsonValue<double>(json, "effective_salience", 0.0);
    stimulus.active = jsonValue<bool>(json, "active", true);
    return stimulus;
}

Json needStateJson(const NeedState& state) {
    Json expectations = Json::object();
    for (const auto& [actionId, relief] : state.expectedRelief) expectations[actionId] = relief;
    return Json{{"deficit", state.deficit}, {"last_updated", state.lastUpdated.minute},
                {"last_relief_revision", state.lastReliefRevision},
                {"sensitivity", state.sensitivity}, {"expected_relief", expectations},
                {"evidence_confidence", state.evidenceConfidence},
                {"adaptation_version", state.adaptationVersion},
                {"last_adapted", state.lastAdapted.minute}};
}

NeedState needStateFromJson(const Json& json) {
    NeedState state;
    state.deficit = jsonValue<double>(json, "deficit", 0.0);
    state.lastUpdated = GameTime{jsonValue<std::int64_t>(json, "last_updated", 0)};
    state.lastReliefRevision = jsonValue<std::uint64_t>(json, "last_relief_revision", 0);
    state.sensitivity = jsonValue<double>(json, "sensitivity", 1.0);
    state.evidenceConfidence = jsonValue<double>(json, "evidence_confidence", 1.0);
    state.adaptationVersion = jsonValue<std::uint64_t>(json, "adaptation_version", 0);
    state.lastAdapted = GameTime{jsonValue<std::int64_t>(json, "last_adapted", 0)};
    const auto expectations = json.value("expected_relief", Json::object());
    for (auto it = expectations.begin(); it != expectations.end(); ++it) {
        state.expectedRelief[it.key()] = it.value().get<double>();
    }
    return state;
}

WorldEvent worldEventFromJson(const Json& json) {
    return WorldEvent{jsonValue<int>(json, "schema_version", 1), jsonValue<std::uint64_t>(json, "sequence", 0),
                      GameTime{jsonValue<std::int64_t>(json, "minute", 0)}, jsonValue<std::string>(json, "type", "unknown"),
                      jsonValue<std::string>(json, "actor", ""), jsonValue<std::string>(json, "target", ""),
                      jsonValue<std::string>(json, "detail", ""), jsonValue<std::uint64_t>(json, "world_revision", 0),
                      json.value("data", Json::object())};
}

}  // namespace

Json worldEventToJson(const WorldEvent& event) {
    return Json{{"schema_version", event.schemaVersion}, {"sequence", event.sequence},
                {"minute", event.at.minute}, {"type", event.type}, {"actor", event.actor},
                {"target", event.target}, {"detail", event.detail},
                {"world_revision", event.worldRevision}, {"data", event.data}};
}

Json worldStateToJson(const WorldState& state) {
    Json entities = Json::object();
    for (const auto& [id, entity] : state.entities) {
        entities[id] = Json{{"id", entity.id}, {"kind", entity.kind},
                            {"lifecycle", toString(entity.lifecycle)}, {"metadata", entity.metadata}};
    }
    Json links = Json::object();
    for (const auto& [id, link] : state.links) {
        links[id] = Json{{"traversable", link.traversable}, {"locked", link.locked}};
    }
    Json npcs = Json::object();
    for (const auto& [id, npc] : state.npcs) {
        Json memories = Json::array();
        for (const auto& memory : npc.memories) memories.push_back(memoryJson(memory));
        Json peerTrust = Json::object();
        for (const auto& [peer, trust] : npc.peerTrust) peerTrust[peer] = trust;
        Json needs = Json::object();
        for (const auto& [needId, need] : npc.needs) needs[needId] = needStateJson(need);
        Json stimulusSensitivity = Json::object();
        for (const auto& [category, sensitivity] : npc.stimulusSensitivity) {
            stimulusSensitivity[category] = sensitivity;
        }
        Json stimuli = Json::array();
        for (const auto& stimulus : npc.stimuli) stimuli.push_back(stimulusJson(stimulus));
        Json activity = nullptr;
        if (npc.activity.goal || npc.activity.process) {
            Json goal = nullptr;
            if (npc.activity.goal) {
                goal = Json{{"id", npc.activity.goal->id}, {"intent", npc.activity.goal->intent},
                            {"target", npc.activity.goal->target}, {"priority", npc.activity.goal->priority},
                            {"status", toString(npc.activity.goal->status)},
                            {"created_at", npc.activity.goal->createdAt.minute},
                            {"deadline", npc.activity.goal->deadline.minute},
                            {"reason", npc.activity.goal->reason}};
            }
            Json process = nullptr;
            if (npc.activity.process) process = processJson(*npc.activity.process);
            activity = Json{{"status", toString(npc.activity.status)}, {"goal", goal}, {"process", process}};
        }
        npcs[id] = Json{{"definition_id", npc.definitionId}, {"location", npc.location},
                        {"schedule_label", npc.scheduleLabel}, {"goal_stage", npc.goalStage},
                        {"needs", needs},
                        {"relation", {{"trust", npc.playerRelation.trust}, {"respect", npc.playerRelation.respect},
                                       {"fear", npc.playerRelation.fear}, {"owed", npc.playerRelation.owed}}},
                        {"memories", memories}, {"open_promises", npc.openPromises},
                        {"state_version", npc.stateVersion}, {"mode", npc.mode}, {"strategy", npc.strategy},
                        {"peer_trust", peerTrust}, {"stimulus_sensitivity", stimulusSensitivity},
                        {"stimuli", stimuli}, {"last_autonomous_turn", npc.lastAutonomousTurn.minute},
                        {"activity", activity}};
    }
    Json promises = Json::object();
    for (const auto& [id, promise] : state.promises) {
        promises[id] = Json{{"id", promise.id}, {"from", promise.from}, {"to", promise.to},
                            {"description", promise.description}, {"due", promise.due.minute},
                            {"status", toString(promise.status)}, {"version", promise.version}};
    }
    Json events = Json::array();
    for (const auto& event : state.events) events.push_back(worldEventToJson(event));
    Json resources = Json::object();
    for (const auto& [id, value] : state.resources) resources[id] = Json{{"amount", value.amount}, {"reserved", value.reserved}};
    return Json{{"save_schema_version", 10}, {"simulation_version", "adaptive-runtime-p0.10"},
                {"event_sequence", state.events.size()}, {"world_id", state.worldId},
                {"instance_id", state.instanceId},
                {"content_version", state.contentVersion}, {"now", state.now.minute},
                {"revision", state.revision},
                {"resources", resources},
                {"entities", entities}, {"links", links}, {"npcs", npcs}, {"promises", promises}, {"events", events},
                {"processed_turn_ids", state.processedTurnIds}};
}

WorldState deserializeWorldState(const Json& json) {
    WorldState state;
    state.worldId = jsonValue<std::string>(json, "world_id", "");
    state.instanceId = jsonValue<std::string>(json, "instance_id", "");
    state.contentVersion = jsonValue<std::string>(json, "content_version", "");
    state.now = GameTime{jsonValue<std::int64_t>(json, "now", 0)};
    state.revision = jsonValue<std::uint64_t>(json, "revision", 0);
    const auto resources = json.value("resources", Json::object());
    if (!resources.is_object()) throw std::runtime_error("snapshot resources must be an object");
    for (auto it = resources.begin(); it != resources.end(); ++it) {
        state.resources[it.key()] =
            ResourceState{jsonValue<int>(it.value(), "amount", 0), jsonValue<int>(it.value(), "reserved", 0)};
    }
    const auto entities = json.value("entities", Json::object());
    for (auto it = entities.begin(); it != entities.end(); ++it) {
        const auto value = it.value();
        state.entities[it.key()] = WorldEntityState{
            jsonValue<std::string>(value, "id", it.key()), jsonValue<std::string>(value, "kind", "world_object"),
            entityLifecycleFromString(jsonValue<std::string>(value, "lifecycle", "active")),
            value.value("metadata", Json::object())};
    }
    const auto links = json.value("links", Json::object());
    for (auto it = links.begin(); it != links.end(); ++it) {
        state.links[it.key()] = RuntimeLinkState{jsonValue<bool>(it.value(), "traversable", true),
                                                  jsonValue<bool>(it.value(), "locked", false)};
    }
    const auto npcs = json.value("npcs", Json::object());
    for (auto it = npcs.begin(); it != npcs.end(); ++it) {
        const auto value = it.value();
        NpcState npc;
        // Snapshots written before definition references existed are rejected by
        // WorldService::fromSnapshot; the loader does not guess from the map key.
        npc.definitionId = jsonValue<std::string>(value, "definition_id", "");
        npc.location = jsonValue<std::string>(value, "location", "");
        npc.scheduleLabel = jsonValue<std::string>(value, "schedule_label", "");
        npc.goalStage = jsonValue<int>(value, "goal_stage", 0);
        const auto needs = value.value("needs", Json::object());
        for (auto need = needs.begin(); need != needs.end(); ++need) {
            npc.needs[need.key()] = needStateFromJson(need.value());
        }
        const auto relation = value.value("relation", Json::object());
        npc.playerRelation = RelationshipAxes{jsonValue<double>(relation, "trust", 0.35),
                                              jsonValue<double>(relation, "respect", 0.35),
                                              jsonValue<double>(relation, "fear", 0.0),
                                              jsonValue<double>(relation, "owed", 0.0)};
        for (const auto& memory : value.value("memories", Json::array())) npc.memories.push_back(memoryFromJson(memory));
        npc.openPromises = value.value("open_promises", std::vector<std::string>{});
        npc.stateVersion = jsonValue<std::uint64_t>(value, "state_version", 0);
        npc.mode = jsonValue<std::string>(value, "mode", "working");
        npc.strategy = jsonValue<std::string>(value, "strategy", "routine");
        const auto peerTrust = value.value("peer_trust", Json::object());
        for (auto peer = peerTrust.begin(); peer != peerTrust.end(); ++peer) {
            npc.peerTrust[peer.key()] = peer.value().get<double>();
        }
        const auto stimulusSensitivity = value.value("stimulus_sensitivity", Json::object());
        for (auto sensitivity = stimulusSensitivity.begin(); sensitivity != stimulusSensitivity.end(); ++sensitivity) {
            npc.stimulusSensitivity[sensitivity.key()] = sensitivity.value().get<double>();
        }
        for (const auto& stimulus : value.value("stimuli", Json::array())) {
            npc.stimuli.push_back(stimulusFromJson(stimulus));
        }
        npc.lastAutonomousTurn = GameTime{jsonValue<std::int64_t>(value, "last_autonomous_turn", -1)};
        const auto activity = value.value("activity", Json::object());
        if (activity.is_object() && activity.contains("goal") && activity.at("goal").is_object()) {
            const auto& goal = activity.at("goal");
            npc.activity.goal = GoalState{jsonValue<std::string>(goal, "id", ""),
                                      jsonValue<std::string>(goal, "intent", ""),
                                      jsonValue<std::string>(goal, "target", ""),
                                      jsonValue<double>(goal, "priority", 0.0),
                                      goalStatusFromString(jsonValue<std::string>(goal, "status", "proposed")),
                                      GameTime{jsonValue<std::int64_t>(goal, "created_at", 0)},
                                      GameTime{jsonValue<std::int64_t>(goal, "deadline", 0)},
                                      jsonValue<std::string>(goal, "reason", "")};
        }
        if (activity.is_object() && activity.contains("process") && activity.at("process").is_object()) {
            npc.activity.process = processFromJson(activity.at("process"));
        }
        if (activity.is_object()) {
            npc.activity.status = activityStatusFromString(jsonValue<std::string>(activity, "status", "proposed"));
            if (npc.activity.goal || npc.activity.process) setActivityStatus(npc.activity, npc.activity.status);
        }
        state.npcs[it.key()] = std::move(npc);
    }
    const auto promises = json.value("promises", Json::object());
    for (auto it = promises.begin(); it != promises.end(); ++it) {
        const auto value = it.value();
        state.promises[it.key()] = Promise{
            jsonValue<std::string>(value, "id", it.key()), jsonValue<std::string>(value, "from", ""),
            jsonValue<std::string>(value, "to", ""), jsonValue<std::string>(value, "description", ""),
            GameTime{jsonValue<std::int64_t>(value, "due", 0)},
            promiseStatusFromString(jsonValue<std::string>(value, "status", "proposed")),
            jsonValue<int>(value, "version", 0)};
    }
    for (const auto& event : json.value("events", Json::array())) state.events.push_back(worldEventFromJson(event));
    for (const auto& turn : json.value("processed_turn_ids", std::vector<std::string>{})) {
        state.processedTurnIds.insert(turn);
    }
    return state;
}

}  // namespace tide_marks
