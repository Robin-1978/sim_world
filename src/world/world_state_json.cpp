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

Json knowledgeJson(const KnowledgeFact& fact) {
    return Json{{"id", fact.id}, {"subject", fact.subject}, {"predicate", fact.predicate},
                {"value", fact.value}, {"source_sequence", fact.sourceSequence},
                {"source_type", fact.sourceType}, {"learned_at", fact.learnedAt.minute},
                {"confidence", fact.confidence}, {"scope", fact.scope}, {"confirmed", fact.confirmed}};
}

KnowledgeFact knowledgeFromJson(const Json& json) {
    return KnowledgeFact{jsonValue<std::string>(json, "id", ""),
                         jsonValue<std::string>(json, "subject", ""),
                         jsonValue<std::string>(json, "predicate", ""),
                         jsonValue<std::string>(json, "value", ""),
                         jsonValue<std::uint64_t>(json, "source_sequence", 0),
                         jsonValue<std::string>(json, "source_type", ""),
                         GameTime{jsonValue<std::int64_t>(json, "learned_at", 0)},
                         jsonValue<double>(json, "confidence", 0.0),
                         jsonValue<std::string>(json, "scope", "direct"),
                         jsonValue<bool>(json, "confirmed", false)};
}

Json experienceJson(const Experience& experience) {
    return Json{{"id", experience.id}, {"domain", experience.domain}, {"trigger", experience.trigger},
                {"action", experience.action}, {"outcome", experience.outcome},
                {"source_sequences", experience.sourceSequences}, {"learned_at", experience.learnedAt.minute},
                {"last_used", experience.lastUsed.minute}, {"confidence", experience.confidence},
                {"personal", experience.personal}};
}

Experience experienceFromJson(const Json& json) {
    Experience experience;
    experience.id = jsonValue<std::string>(json, "id", "");
    experience.domain = jsonValue<std::string>(json, "domain", "");
    experience.trigger = jsonValue<std::string>(json, "trigger", "");
    experience.action = jsonValue<std::string>(json, "action", "");
    experience.outcome = jsonValue<std::string>(json, "outcome", "");
    experience.sourceSequences = jsonValue<std::vector<std::uint64_t>>(json, "source_sequences", {});
    experience.learnedAt = GameTime{jsonValue<std::int64_t>(json, "learned_at", 0)};
    experience.lastUsed = GameTime{jsonValue<std::int64_t>(json, "last_used", 0)};
    experience.confidence = jsonValue<double>(json, "confidence", 0.0);
    experience.personal = jsonValue<bool>(json, "personal", true);
    return experience;
}

Json innovationAdoptionJson(const InnovationAdoptionState& adoption) {
    return Json{{"proposal_id", adoption.proposalId}, {"recipe_id", adoption.recipeId},
                {"process_id", adoption.processId},
                {"status", toString(adoption.status)}, {"started_at", adoption.startedAt.minute},
                {"updated_at", adoption.updatedAt.minute}, {"reason", adoption.reason},
                {"start_event_sequence", adoption.startEventSequence},
                {"observed_metrics", adoption.observedMetrics},
                {"metric_evaluation", adoption.metricEvaluation}};
}

InnovationAdoptionState innovationAdoptionFromJson(const Json& json) {
    return InnovationAdoptionState{
        jsonValue<std::string>(json, "proposal_id", ""),
        jsonValue<std::string>(json, "recipe_id", ""),
        jsonValue<std::string>(json, "process_id", ""),
        innovationAdoptionStatusFromString(jsonValue<std::string>(json, "status", "pending")),
        GameTime{jsonValue<std::int64_t>(json, "started_at", -1)},
        GameTime{jsonValue<std::int64_t>(json, "updated_at", -1)},
        jsonValue<std::string>(json, "reason", ""),
        jsonValue<std::uint64_t>(json, "start_event_sequence", 0),
        json.value("observed_metrics", Json::object()),
        json.value("metric_evaluation", Json::object())};
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
    return Json{{"id", process.id}, {"goal_id", process.goalId}, {"action_id", process.actionId},
                {"recipe_id", process.recipeId}, {"plan_id", process.planId},
                {"trigger", process.trigger}, {"started_at", process.startedAt.minute},
                {"completes_at", process.completesAt.minute}, {"suspended_at", process.suspendedAt.minute},
                {"progress", process.progress},
                {"status", toString(process.status)}, {"interruption_reason", process.interruptionReason},
                {"behavior_node_id", process.behaviorNodeId}, {"behavior_step", process.behaviorStep},
                {"behavior_status", process.behaviorStatus}, {"parameters", process.parameters},
                {"interruptible", process.interruptible},
                {"interrupt_salience_threshold", process.interruptSalienceThreshold},
                {"reservation_mode", process.reservationMode},
                {"reserved_resources", reserved},
                {"required_resources", [&process] { Json required = Json::object(); for (const auto& [resource, amount] : process.requiredResources) required[resource] = amount; return required; }()},
                {"priority", process.priority},
                {"preemption_policy", process.preemptionPolicy},
                {"proposal_world_revision", process.proposalWorldRevision},
                {"proposal_npc_state_version", process.proposalNpcStateVersion}};
}

ProcessExecution processFromJson(const Json& json) {
    ProcessExecution process;
    process.id = jsonValue<std::string>(json, "id", "");
    process.goalId = jsonValue<std::string>(json, "goal_id", "");
    process.actionId = jsonValue<std::string>(json, "action_id", "");
    process.recipeId = jsonValue<std::string>(json, "recipe_id", process.actionId);
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
    process.parameters = json.value("parameters", Json::object());
    if (!process.parameters.is_object()) throw std::runtime_error("process parameters must be an object");
    process.interruptible = jsonValue<bool>(json, "interruptible", true);
    process.interruptSalienceThreshold = jsonValue<double>(json, "interrupt_salience_threshold", 1.0);
    process.reservationMode = jsonValue<std::string>(json, "reservation_mode", "none");
    const auto reserved = json.value("reserved_resources", Json::object());
    for (auto it = reserved.begin(); it != reserved.end(); ++it) {
        process.reservedResources[it.key()] = it.value().get<int>();
    }
    const auto required = json.value("required_resources", Json::object());
    for (auto it = required.begin(); it != required.end(); ++it) {
        process.requiredResources[it.key()] = it.value().get<int>();
    }
    process.priority = jsonValue<int>(json, "priority", 0);
    process.preemptionPolicy = jsonValue<std::string>(json, "preemption_policy", "none");
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
                      json.value("data", Json::object()), jsonValue<std::string>(json, "event_id", ""),
                      jsonValue<std::string>(json, "correlation_id", ""),
                      jsonValue<std::uint64_t>(json, "causation_sequence", 0),
                      jsonValue<std::string>(json, "content_version", ""),
                      jsonValue<std::string>(json, "simulation_version", std::string{kSimulationVersion})};
}

}  // namespace

Json worldEventToJson(const WorldEvent& event) {
    return Json{{"schema_version", event.schemaVersion}, {"sequence", event.sequence},
                {"minute", event.at.minute}, {"type", event.type}, {"actor", event.actor},
                {"target", event.target}, {"detail", event.detail},
                {"world_revision", event.worldRevision}, {"data", event.data},
                {"event_id", event.eventId}, {"correlation_id", event.correlationId},
                {"causation_sequence", event.causationSequence},
                {"content_version", event.contentVersion},
                {"simulation_version", event.simulationVersion}};
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
        Json knowledge = Json::array();
        for (const auto& fact : npc.knowledge) knowledge.push_back(knowledgeJson(fact));
        Json experiences = Json::array();
        for (const auto& experience : npc.experiences) experiences.push_back(experienceJson(experience));
        Json relationships = Json::object();
        for (const auto& [target, relation] : npc.relationships) {
            relationships[target] = Json{{"trust", relation.trust}, {"respect", relation.respect},
                                          {"fear", relation.fear}, {"owed", relation.owed}};
        }
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
                        {"relationships", relationships},
                        {"memories", memories}, {"knowledge", knowledge}, {"experiences", experiences},
                        {"unresolved_issues", npc.unresolvedIssues},
                        {"innovation_hypotheses", npc.innovationHypotheses},
                        {"innovation_adoptions", [&npc] { Json value = Json::array(); for (const auto& adoption : npc.innovationAdoptions) value.push_back(innovationAdoptionJson(adoption)); return value; }()},
                        {"open_promises", npc.openPromises},
                        {"state_version", npc.stateVersion}, {"mode", npc.mode}, {"strategy", npc.strategy},
                        {"stimulus_sensitivity", stimulusSensitivity},
                        {"stimuli", stimuli}, {"last_autonomous_turn", npc.lastAutonomousTurn.minute},
                        {"last_simulation_tick", npc.lastSimulationTick.minute},
                        {"last_drive_tick", npc.lastDriveTick.minute},
                        {"reflection_cursor", Json{{"last_event_sequence", npc.reflectionCursor.lastEventSequence},
                                                     {"last_reflection_day", npc.reflectionCursor.lastReflectionDay},
                                                     {"reflection_revision", npc.reflectionCursor.reflectionRevision}}},
                        {"activity", activity}};
    }
    Json promises = Json::object();
    for (const auto& [id, promise] : state.promises) {
        promises[id] = Json{{"id", promise.id}, {"from", promise.from}, {"to", promise.to},
                            {"description", promise.description}, {"due", promise.due.minute},
                            {"status", toString(promise.status)}, {"version", promise.version}};
    }
    Json exchanges = Json::object();
    for (const auto& [id, contract] : state.exchangeContracts) {
        exchanges[id] = Json{{"id", contract.id}, {"proposer", contract.proposer},
                             {"counterparty", contract.counterparty},
                             {"offered", [&contract] { Json values = Json::object(); for (const auto& [resource, amount] : contract.offered) values[resource] = amount; return values; }()},
                             {"requested", [&contract] { Json values = Json::object(); for (const auto& [resource, amount] : contract.requested) values[resource] = amount; return values; }()},
                             {"created", contract.created.minute}, {"due", contract.due.minute},
                             {"settled_at", contract.settledAt.minute},
                             {"status", toString(contract.status)}, {"version", contract.version},
                             {"reason", contract.reason}};
    }
    Json socialGroups = Json::object();
    for (const auto& [id, group] : state.socialGroups) {
        Json relations = Json::object();
        for (const auto& [target, relation] : group.relationships) {
            relations[target] = Json{{"trust", relation.trust}, {"respect", relation.respect},
                                      {"fear", relation.fear}, {"owed", relation.owed}};
        }
        socialGroups[id] = Json{{"definition_id", group.definitionId}, {"metrics", group.metrics},
                                {"members", group.members}, {"relationships", relations}};
    }
    Json events = Json::array();
    for (const auto& event : state.events) events.push_back(worldEventToJson(event));
    Json resources = Json::object();
    for (const auto& [id, value] : state.resources) resources[id] = Json{{"amount", value.amount}, {"reserved", value.reserved}};
    Json inventories = Json::object();
    for (const auto& [entity, quantities] : state.inventories) {
        Json values = Json::object();
        for (const auto& [resource, amount] : quantities) values[resource] = amount;
        inventories[entity] = std::move(values);
    }
    Json security = Json::object();
    for (const auto& [entity, securityState] : state.security) {
        security[entity] = Json{{"integrity", securityState.integrity}, {"availability", securityState.availability},
                                {"threat", securityState.threat}, {"exposure", securityState.exposure},
                                {"evacuated", securityState.evacuated}};
    }
    Json overlays = Json::array();
    for (const auto& overlay : state.activeOverlays) {
        overlays.push_back(Json{{"proposal_id", overlay.proposalId},
                                {"base_content_version", overlay.baseContentVersion},
                                {"overlay_version", overlay.overlayVersion},
                                {"operations", overlay.operations}});
    }
    return Json{{"save_schema_version", kSnapshotSchemaVersion},
                {"simulation_version", kSimulationVersion},
                {"event_sequence", state.events.size()}, {"world_id", state.worldId},
                {"instance_id", state.instanceId},
                {"content_version", state.contentVersion}, {"now", state.now.minute},
                {"world_revision", state.revision},
                {"resources", resources}, {"inventories", inventories}, {"security", security},
                {"active_overlays", overlays},
                {"entities", entities}, {"links", links}, {"npcs", npcs}, {"social_groups", socialGroups},
                {"promises", promises}, {"exchange_contracts", exchanges}, {"events", events},
                {"processed_turn_ids", state.processedTurnIds}};
}

WorldState deserializeWorldState(const Json& json) {
    WorldState state;
    state.worldId = jsonValue<std::string>(json, "world_id", "");
    state.instanceId = jsonValue<std::string>(json, "instance_id", "");
    state.contentVersion = jsonValue<std::string>(json, "content_version", "");
    state.now = GameTime{jsonValue<std::int64_t>(json, "now", 0)};
    if (!json.contains("world_revision") || !json.at("world_revision").is_number_unsigned()) {
        throw std::runtime_error("snapshot must contain an unsigned world_revision");
    }
    state.revision = json.at("world_revision").get<std::uint64_t>();
    const auto resources = json.value("resources", Json::object());
    if (!resources.is_object()) throw std::runtime_error("snapshot resources must be an object");
    for (auto it = resources.begin(); it != resources.end(); ++it) {
        state.resources[it.key()] =
            ResourceState{jsonValue<int>(it.value(), "amount", 0), jsonValue<int>(it.value(), "reserved", 0)};
    }
    const auto inventories = json.value("inventories", Json::object());
    if (!inventories.is_object()) throw std::runtime_error("snapshot inventories must be an object");
    for (auto entity = inventories.begin(); entity != inventories.end(); ++entity) {
        if (!entity.value().is_object()) throw std::runtime_error("snapshot inventory must be an object");
        state.inventories.try_emplace(entity.key());
        for (auto quantity = entity.value().begin(); quantity != entity.value().end(); ++quantity) {
            state.inventories[entity.key()][quantity.key()] = quantity.value().get<int>();
        }
    }
    const auto security = json.value("security", Json::object());
    if (!security.is_object()) throw std::runtime_error("snapshot security must be an object");
    for (auto entity = security.begin(); entity != security.end(); ++entity) {
        state.security[entity.key()] = SecurityState{
            jsonValue<double>(entity.value(), "integrity", 1.0),
            jsonValue<double>(entity.value(), "availability", 1.0),
            jsonValue<double>(entity.value(), "threat", 0.0),
            jsonValue<double>(entity.value(), "exposure", 0.0),
            jsonValue<bool>(entity.value(), "evacuated", false)};
    }
    const auto overlays = json.value("active_overlays", Json::array());
    if (!overlays.is_array()) throw std::runtime_error("snapshot active_overlays must be an array");
    for (const auto& value : overlays) {
        if (!value.is_object() || !value.contains("proposal_id") || !value.at("proposal_id").is_string() ||
            !value.contains("base_content_version") || !value.at("base_content_version").is_string() ||
            !value.contains("overlay_version") || !value.at("overlay_version").is_string() ||
            !value.contains("operations") || !value.at("operations").is_array()) {
            throw std::runtime_error("snapshot active overlay is incomplete");
        }
        state.activeOverlays.push_back(ActiveOverlayState{
            value.at("proposal_id").get<std::string>(), value.at("base_content_version").get<std::string>(),
            value.at("overlay_version").get<std::string>(), value.at("operations")});
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
        const auto relationships = value.value("relationships", Json::object());
        for (auto relation = relationships.begin(); relation != relationships.end(); ++relation) {
            npc.relationships[relation.key()] = RelationshipAxes{
                jsonValue<double>(relation.value(), "trust", 0.35),
                jsonValue<double>(relation.value(), "respect", 0.35),
                jsonValue<double>(relation.value(), "fear", 0.0),
                jsonValue<double>(relation.value(), "owed", 0.0)};
        }
        for (const auto& memory : value.value("memories", Json::array())) npc.memories.push_back(memoryFromJson(memory));
        for (const auto& fact : value.value("knowledge", Json::array())) npc.knowledge.push_back(knowledgeFromJson(fact));
        for (const auto& experience : value.value("experiences", Json::array())) {
            npc.experiences.push_back(experienceFromJson(experience));
        }
        npc.unresolvedIssues = value.value("unresolved_issues", std::vector<std::string>{});
        npc.innovationHypotheses = value.value("innovation_hypotheses", std::vector<Json>{});
        if (npc.innovationHypotheses.size() > 8) {
            throw std::runtime_error("snapshot NPC innovation hypotheses exceed the bounded limit");
        }
        for (const auto& hypothesis : npc.innovationHypotheses) {
            if (!hypothesis.is_object()) throw std::runtime_error("snapshot NPC innovation hypothesis must be an object");
        }
        const auto adoptions = value.value("innovation_adoptions", Json::array());
        if (!adoptions.is_array() || adoptions.size() > 8) {
            throw std::runtime_error("snapshot NPC innovation adoptions exceed the bounded limit");
        }
        for (const auto& adoption : adoptions) {
            if (!adoption.is_object()) throw std::runtime_error("snapshot NPC innovation adoption must be an object");
            const auto observed = adoption.value("observed_metrics", Json::object());
            const auto evaluation = adoption.value("metric_evaluation", Json::object());
            if (!observed.is_object() || !evaluation.is_object()) {
                throw std::runtime_error("snapshot NPC innovation adoption metrics must be objects");
            }
            npc.innovationAdoptions.push_back(innovationAdoptionFromJson(adoption));
        }
        npc.openPromises = value.value("open_promises", std::vector<std::string>{});
        npc.stateVersion = jsonValue<std::uint64_t>(value, "state_version", 0);
        npc.mode = jsonValue<std::string>(value, "mode", "working");
        npc.strategy = jsonValue<std::string>(value, "strategy", "routine");
        const auto stimulusSensitivity = value.value("stimulus_sensitivity", Json::object());
        for (auto sensitivity = stimulusSensitivity.begin(); sensitivity != stimulusSensitivity.end(); ++sensitivity) {
            npc.stimulusSensitivity[sensitivity.key()] = sensitivity.value().get<double>();
        }
        for (const auto& stimulus : value.value("stimuli", Json::array())) {
            npc.stimuli.push_back(stimulusFromJson(stimulus));
        }
        npc.lastAutonomousTurn = GameTime{jsonValue<std::int64_t>(value, "last_autonomous_turn", -1)};
        npc.lastSimulationTick = GameTime{jsonValue<std::int64_t>(value, "last_simulation_tick", -1)};
        npc.lastDriveTick = GameTime{jsonValue<std::int64_t>(value, "last_drive_tick",
                                                            value.value("last_simulation_tick", -1))};
        const auto reflection = value.value("reflection_cursor", Json::object());
        if (!reflection.is_object()) throw std::runtime_error("snapshot reflection_cursor must be an object");
        npc.reflectionCursor.lastEventSequence =
            jsonValue<std::uint64_t>(reflection, "last_event_sequence", 0);
        npc.reflectionCursor.lastReflectionDay =
            jsonValue<std::int64_t>(reflection, "last_reflection_day", -1);
        npc.reflectionCursor.reflectionRevision =
            jsonValue<std::uint64_t>(reflection, "reflection_revision", 0);
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
            if (const auto error = npcActivityInvariantError(npc.activity)) {
                throw std::runtime_error("snapshot NPC " + it.key() + " has an invalid activity: " + *error);
            }
        } else if (!activity.is_null()) {
            throw std::runtime_error("snapshot NPC activity must be an object or null");
        }
        state.npcs[it.key()] = std::move(npc);
    }
    const auto socialGroups = json.value("social_groups", Json::object());
    if (!socialGroups.is_object()) throw std::runtime_error("snapshot social_groups must be an object");
    for (auto group = socialGroups.begin(); group != socialGroups.end(); ++group) {
        if (!group.value().is_object()) throw std::runtime_error("snapshot social group must be an object");
        SocialGroupState parsed;
        parsed.definitionId = jsonValue<std::string>(group.value(), "definition_id", "");
        const auto metrics = group.value().value("metrics", Json::object());
        for (auto metric = metrics.begin(); metric != metrics.end(); ++metric) {
            parsed.metrics[metric.key()] = metric.value().get<double>();
        }
        const auto members = group.value().value("members", Json::object());
        for (auto member = members.begin(); member != members.end(); ++member) {
            parsed.members[member.key()] = member.value().get<std::string>();
        }
        const auto relations = group.value().value("relationships", Json::object());
        for (auto relation = relations.begin(); relation != relations.end(); ++relation) {
            parsed.relationships[relation.key()] = RelationshipAxes{
                jsonValue<double>(relation.value(), "trust", 0.35),
                jsonValue<double>(relation.value(), "respect", 0.35),
                jsonValue<double>(relation.value(), "fear", 0.0),
                jsonValue<double>(relation.value(), "owed", 0.0)};
        }
        state.socialGroups[group.key()] = std::move(parsed);
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
    const auto exchanges = json.value("exchange_contracts", Json::object());
    if (!exchanges.is_object()) throw std::runtime_error("snapshot exchange_contracts must be an object");
    for (auto it = exchanges.begin(); it != exchanges.end(); ++it) {
        const auto value = it.value();
        ExchangeContract contract;
        contract.id = jsonValue<std::string>(value, "id", it.key());
        contract.proposer = jsonValue<std::string>(value, "proposer", "");
        contract.counterparty = jsonValue<std::string>(value, "counterparty", "");
        const auto offered = value.value("offered", Json::object());
        const auto requested = value.value("requested", Json::object());
        if (!offered.is_object() || !requested.is_object()) {
            throw std::runtime_error("snapshot exchange terms must be objects");
        }
        for (auto term = offered.begin(); term != offered.end(); ++term) {
            contract.offered[term.key()] = term.value().get<int>();
        }
        for (auto term = requested.begin(); term != requested.end(); ++term) {
            contract.requested[term.key()] = term.value().get<int>();
        }
        contract.created = GameTime{jsonValue<std::int64_t>(value, "created", 0)};
        contract.due = GameTime{jsonValue<std::int64_t>(value, "due", 0)};
        contract.settledAt = GameTime{jsonValue<std::int64_t>(value, "settled_at", -1)};
        contract.status = exchangeContractStatusFromString(jsonValue<std::string>(value, "status", "proposed"));
        contract.version = jsonValue<int>(value, "version", 0);
        contract.reason = jsonValue<std::string>(value, "reason", "");
        state.exchangeContracts[it.key()] = std::move(contract);
    }
    for (const auto& event : json.value("events", Json::array())) state.events.push_back(worldEventFromJson(event));
    for (const auto& [id, npc] : state.npcs) {
        if (npc.reflectionCursor.lastEventSequence > state.events.size()) {
            throw std::runtime_error("snapshot NPC " + id + " reflection cursor is beyond the event history");
        }
        if (npc.reflectionCursor.lastReflectionDay > state.now.day()) {
            throw std::runtime_error("snapshot NPC " + id + " reflection cursor is from the future");
        }
    }
    for (const auto& turn : json.value("processed_turn_ids", std::vector<std::string>{})) {
        state.processedTurnIds.insert(turn);
    }
    return state;
}

}  // namespace tide_marks
