#include "world/world_evolution_system.hpp"

#include "evolution/evolution.hpp"
#include "world/cognition_system.hpp"
#include "world/world_service.hpp"

#include <algorithm>
#include <map>

namespace tide_marks {

namespace {
// Keep the incremental window small enough that a daily reflection remains a
// cheap, cacheable request even after a busy day. The cursor and EventLog keep
// the complete history; the provider only needs a representative evidence
// slice plus unresolved issues.
constexpr std::size_t kMaxEvolutionEvidenceEntries = 4;

// EventLog is authoritative and remains complete in WorldState.  Providers
// receive a deterministic evidence projection instead of the raw event data:
// intent/dialogue events often contain the entire decision context and tool
// diagnostics, which are neither needed to cite an event nor safe to replay
// into a later prompt.  Keep bounded scalar/object data that explains the
// result while dropping duplicated reasoning payloads.
Json compactEvidenceJson(const Json& value, int depth = 0) {
    if (depth > 2) return value.is_object() ? Json::object() : Json();
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        return text.size() <= 256 ? value : Json(text.substr(0, 256) + "…");
    }
    if (value.is_array()) {
        Json result = Json::array();
        const auto limit = std::min<std::size_t>(value.size(), 8);
        for (std::size_t index = 0; index < limit; ++index) {
            result.push_back(compactEvidenceJson(value.at(index), depth + 1));
        }
        if (value.size() > limit) result.push_back("…");
        return result;
    }
    if (!value.is_object()) return value;
    Json result = Json::object();
    std::size_t fields = 0;
    for (auto item = value.begin(); item != value.end() && fields < 16; ++item) {
        // These fields are provider diagnostics or a copy of the complete
        // decision projection. The event's typed summary and source sequence
        // remain available to the model and to replay.
        if (item.key() == "rationale" || item.key() == "motivation" ||
            item.key() == "tradeoffs" || item.key() == "utterance_understanding" ||
            item.key() == "world_snapshot" || item.key() == "self_state" ||
            item.key() == "plan_catalog" || item.key() == "candidates" ||
            item.key() == "context" || item.key() == "proposal") {
            continue;
        }
        result[item.key()] = compactEvidenceJson(item.value(), depth + 1);
        ++fields;
    }
    if (value.size() > fields) result["fields_truncated"] = true;
    return result;
}

Json compactEventData(const WorldEvent& event) {
    return compactEvidenceJson(event.data);
}

std::vector<std::string> overlayIds(const WorldState& state) {
    std::vector<std::string> ids;
    ids.reserve(state.activeOverlays.size());
    for (const auto& overlay : state.activeOverlays) ids.push_back(overlay.proposalId);
    return ids;
}

Json knowledgeEvidence(const NpcState& npc) {
    Json result = Json::array();
    const auto begin = npc.knowledge.size() > kMaxEvolutionEvidenceEntries
                           ? npc.knowledge.size() - kMaxEvolutionEvidenceEntries
                           : 0;
    for (std::size_t index = begin; index < npc.knowledge.size(); ++index) {
        const auto& fact = npc.knowledge[index];
        result.push_back(Json{{"id", fact.id}, {"subject", fact.subject}, {"predicate", fact.predicate},
                              {"value", fact.value}, {"source_sequence", fact.sourceSequence},
                              {"source_type", fact.sourceType}, {"learned_at", fact.learnedAt.minute},
                              {"scope", fact.scope}, {"confidence", fact.confidence},
                              {"confirmed", fact.confirmed}});
    }
    return result;
}

Json experienceEvidence(const NpcState& npc) {
    Json result = Json::array();
    const auto begin = npc.experiences.size() > kMaxEvolutionEvidenceEntries
                           ? npc.experiences.size() - kMaxEvolutionEvidenceEntries
                           : 0;
    for (std::size_t index = begin; index < npc.experiences.size(); ++index) {
        const auto& experience = npc.experiences[index];
        result.push_back(Json{{"id", experience.id}, {"domain", experience.domain},
                              {"trigger", experience.trigger}, {"action", experience.action},
                              {"outcome", experience.outcome}, {"source_sequences", experience.sourceSequences},
                              {"learned_at", experience.learnedAt.minute},
                              {"last_used", experience.lastUsed.minute},
                              {"confidence", experience.confidence}, {"personal", experience.personal}});
    }
    return result;
}

Json memoryEvidence(const NpcState& npc) {
    Json result = Json::array();
    const auto begin = npc.memories.size() > kMaxEvolutionEvidenceEntries
                           ? npc.memories.size() - kMaxEvolutionEvidenceEntries
                           : 0;
    for (std::size_t index = begin; index < npc.memories.size(); ++index) {
        const auto& memory = npc.memories[index];
        result.push_back(Json{{"kind", memory.kind}, {"text", memory.text},
                              {"at", memory.at.minute}, {"confidence", memory.confidence}});
    }
    return result;
}

Json relationshipEvidence(const NpcState& npc) {
    Json result = Json::object();
    for (const auto& [target, axes] : npc.relationships) {
        result[target] = Json{{"trust", axes.trust}, {"respect", axes.respect},
                              {"fear", axes.fear}, {"owed", axes.owed}};
    }
    return result;
}

Json stimulusEvidence(const NpcState& npc) {
    Json result = Json::array();
    for (const auto& stimulus : npc.stimuli) {
        if (!stimulus.active) continue;
        result.push_back(Json{{"id", stimulus.id}, {"source_event", stimulus.sourceEvent},
                              {"source_sequence", stimulus.sourceSequence}, {"category", stimulus.category},
                              {"actor", stimulus.actor}, {"target", stimulus.target},
                              {"expires_at", stimulus.expiresAt.minute}, {"valence", stimulus.valence},
                              {"salience", stimulus.effectiveSalience},
                              {"visibility", stimulus.visibilityScope}});
    }
    return result;
}

bool isPrivateCognitiveEvent(const std::string& type);

Json reflectionEvidence(const WorldState& state, const NpcState& npc) {
    Json events = Json::array();
    for (const auto& event : state.events) {
        if (event.sequence <= npc.reflectionCursor.lastEventSequence) continue;
        // The cursor is a committed boundary. The same visibility rule as
        // Level 1 proposals applies to the incremental reflection window.
        if (isPrivateCognitiveEvent(event.type) &&
            event.actor != npc.definitionId && event.target != npc.definitionId &&
            event.target != npcEntityId(npc.definitionId)) continue;
        events.push_back(Json{{"sequence", event.sequence}, {"at", event.at.minute},
                              {"type", event.type}, {"actor", event.actor}, {"target", event.target},
                              {"detail", event.detail.size() <= 256 ? event.detail
                                                                      : event.detail.substr(0, 256) + "…"},
                              {"data", compactEventData(event)}});
        if (events.size() >= kMaxEvolutionEvidenceEntries * 3) break;
    }
    return events;
}

bool isPrivateCognitiveEvent(const std::string& type) {
    // These event families are projections of one NPC's cognition.  Other
    // world facts remain public evolution evidence, while private cognitive
    // evidence is visible only to the subject that owns it (or to a world
    // event explicitly addressed to that subject).
    return type.rfind("memory.", 0) == 0 || type.rfind("knowledge.", 0) == 0 ||
           type.rfind("experience.", 0) == 0 || type == "drive.adapted";
}

bool visibleInEvolutionContext(const WorldEvent& event, const std::string& evolutionLevel,
                               const std::string& targetNpcId) {
    if (evolutionLevel != "level1" || targetNpcId.empty() || !isPrivateCognitiveEvent(event.type)) {
        return true;
    }
    return event.actor == targetNpcId || event.target == targetNpcId ||
           event.target == npcEntityId(targetNpcId);
}
}  // namespace

Json WorldEvolutionSystem::proposalContext(const WorldService& world,
                                                const std::string& evolutionLevel,
                                                const std::string& trigger,
                                                const Json& validationFeedback,
                                                const std::string& targetNpcId) {
    const auto& state = world.state_;
    const auto& content = world.content_;
    if (evolutionLevel != "level1" && evolutionLevel != "level2") return Json::object();
    std::string scopedTarget = targetNpcId;
    if (evolutionLevel == "level1" && scopedTarget.empty() && !content.npcs.empty()) {
        scopedTarget = content.npcs.front().definition.id;
    }
    Json events = Json::array();
    const std::size_t evidenceLimit = kMaxEvolutionEvidenceEntries * 3;
    const std::size_t first = state.events.size() > evidenceLimit ? state.events.size() - evidenceLimit : 0;
    for (std::size_t index = first; index < state.events.size(); ++index) {
        const auto& event = state.events[index];
        if (!visibleInEvolutionContext(event, evolutionLevel, scopedTarget)) continue;
        Json eventData = event.data;
        // A Level 1 hypothesis is durable evidence for its owner. Level 2 may
        // use the existence and source events of that hypothesis, but does not
        // receive the owner's private proposal payload as a free broadcast.
        if (evolutionLevel == "level2" && event.type == "innovation.hypothesis_created" &&
            eventData.is_object()) {
            eventData.erase("proposal");
        }
        events.push_back(Json{{"sequence", event.sequence}, {"at", event.at.minute},
                              {"type", event.type}, {"actor", event.actor}, {"target", event.target},
                              {"detail", event.detail.size() <= 256 ? event.detail
                                                                      : event.detail.substr(0, 256) + "…"},
                              {"data", compactEvidenceJson(eventData)}});
    }
    Json resources = Json::object();
    for (const auto& [id, resource] : state.resources) {
        resources[id] = Json{{"amount", resource.amount}, {"reserved", resource.reserved}};
    }
    Json npcs = Json::object();
    for (const auto& [id, npc] : state.npcs) {
        // Level 1 is an individual reflection lane.  Peers remain visible as
        // coarse public context, but their memories, knowledge, experiences,
        // stimuli and relationship graph never cross the target's projection.
        if (evolutionLevel == "level1" && id != scopedTarget) {
            npcs[id] = Json{{"location", npc.location}, {"mode", npc.mode}};
            continue;
        }
        Json needs = Json::object();
        for (const auto& [needId, need] : npc.needs) {
            needs[needId] = Json{{"deficit", need.deficit}, {"sensitivity", need.sensitivity},
                                  {"confidence", need.evidenceConfidence}};
        }
        Json activity = Json{{"status", toString(npc.activity.status)}};
        if (npc.activity.goal) {
            activity["goal"] = Json{{"id", npc.activity.goal->id}, {"intent", npc.activity.goal->intent},
                                     {"target", npc.activity.goal->target},
                                     {"status", toString(npc.activity.goal->status)},
                                     {"deadline", npc.activity.goal->deadline.minute}};
        }
        if (npc.activity.process) {
            activity["process"] = Json{{"id", npc.activity.process->id},
                                        {"action", npc.activity.process->actionId},
                                        {"recipe", npc.activity.process->recipeId},
                                        {"status", toString(npc.activity.process->status)},
                                        {"progress", npc.activity.process->progress},
                                        {"completes_at", npc.activity.process->completesAt.minute}};
        }
        Json reflectionCursor{{"last_event_sequence", npc.reflectionCursor.lastEventSequence},
                              {"last_reflection_day", npc.reflectionCursor.lastReflectionDay},
                              {"reflection_revision", npc.reflectionCursor.reflectionRevision}};
        npcs[id] = Json{{"location", npc.location}, {"mode", npc.mode}, {"strategy", npc.strategy},
                        {"goal_stage", npc.goalStage}, {"activity", activity},
                        {"needs", needs}, {"active_process", npc.activity.process.has_value()},
                        {"memory_count", npc.memories.size()}, {"knowledge_count", npc.knowledge.size()},
                        {"experience_count", npc.experiences.size()}, {"knowledge", knowledgeEvidence(npc)},
                        {"experiences", experienceEvidence(npc)}, {"memories", memoryEvidence(npc)},
                        {"relationships", relationshipEvidence(npc)}, {"stimuli", stimulusEvidence(npc)},
                        {"reflection_cursor", reflectionCursor}, {"unresolved_issues", npc.unresolvedIssues}};
        if (evolutionLevel == "level1" && id == scopedTarget) {
            const auto* authored = world.authoredNpc(id);
            if (authored != nullptr) {
                Json traits = Json::object();
                for (const auto& [trait, value] : authored->definition.privateTraits) traits[trait] = value;
                npcs[id]["private_traits"] = std::move(traits);
            }
        }
    }
    Json allowedOperations = Json::array();
    for (const auto& operation : EvolutionManager::allowedOperationIds(evolutionLevel)) {
        allowedOperations.push_back(operation);
    }
    Json contentSummary{
        {"content_id", content.id}, {"content_version", content.contentVersion},
        {"entity_archetypes", content.entityArchetypes.size()},
        {"behavior_recipes", content.behaviorRecipes.size()},
        {"strategy_rules", content.strategyRules.size()},
        {"entity_archetype_ids", Json::array()}, {"behavior_recipe_ids", Json::array()},
        {"registered_meta_actions", Json::array()}, {"registered_conditions", Json::array()}};
    for (const auto& archetype : content.entityArchetypes) contentSummary["entity_archetype_ids"].push_back(archetype.id);
    for (const auto& recipe : content.behaviorRecipes) contentSummary["behavior_recipe_ids"].push_back(recipe.id);
    for (const auto& [id, definition] : content.metaActions) {
        (void)definition;
        contentSummary["registered_meta_actions"].push_back(id);
    }
    for (const auto& [id, definition] : content.conditions) {
        (void)definition;
        contentSummary["registered_conditions"].push_back(id);
    }
    return Json{{"trigger", trigger}, {"evolution_level", evolutionLevel},
                {"target_npc", scopedTarget},
                {"reflection", Json{{"cadence", "daily_boundary"},
                                     {"innovation_optional", true},
                                     {"purpose", evolutionLevel == "level1" ? "daily_reflection" : "world_reflection"}}},
                {"base_content_version", content.contentVersion},
                {"evolution_system_prompt", evolutionLevel == "level1"
                                                ? content.npcEvolutionPrompt
                                                : content.worldEvolutionPrompt},
                {"evolution_prompt_version", content.contentVersion + ":" +
                                                  (evolutionLevel == "level1" ? "npc_strategy" : "world_substrate")},
                {"base_world_revision", state.revision},
                {"runtime_metadata", world.provider_->runtimeMetadata(
                    evolutionLevel == "level1" ? "npc_evolution:proposal" : "world_evolution:proposal")},
                {"world_time", state.now.minute}, {"resources", resources},
                {"npcs", npcs}, {"content", contentSummary},
                {"allowed_operations", allowedOperations}, {"evidence_events", events},
                {"validation_feedback", validationFeedback}};
}

Json WorldEvolutionSystem::reflectionContext(const WorldService& world,
                                             const std::string& targetNpcId) {
    const auto found = world.state_.npcs.find(targetNpcId);
    if (found == world.state_.npcs.end()) return Json::object();
    const auto& npc = found->second;
    Json currentNeeds = Json::object();
    for (const auto& [id, need] : npc.needs) {
        currentNeeds[id] = Json{{"deficit", need.deficit}, {"sensitivity", need.sensitivity},
                                {"confidence", need.evidenceConfidence}};
    }
    Json activity = Json{{"status", toString(npc.activity.status)}};
    if (npc.activity.goal) {
        activity["goal"] = Json{{"id", npc.activity.goal->id}, {"intent", npc.activity.goal->intent},
                                 {"target", npc.activity.goal->target},
                                 {"priority", npc.activity.goal->priority},
                                 {"status", toString(npc.activity.goal->status)},
                                 {"deadline", npc.activity.goal->deadline.minute}};
    }
    if (npc.activity.process) {
        activity["process"] = Json{{"id", npc.activity.process->id},
                                    {"action", npc.activity.process->actionId},
                                    {"status", toString(npc.activity.process->status)},
                                    {"progress", npc.activity.process->progress}};
    }
    Json contentSummary{{"content_version", world.content_.contentVersion},
                         {"behavior_recipe_ids", Json::array()},
                         {"entity_archetype_ids", Json::array()},
                         {"registered_meta_actions", Json::array()},
                         {"registered_conditions", Json::array()}};
    for (const auto& recipe : world.content_.behaviorRecipes) {
        contentSummary["behavior_recipe_ids"].push_back(recipe.id);
    }
    for (const auto& archetype : world.content_.entityArchetypes) {
        contentSummary["entity_archetype_ids"].push_back(archetype.id);
    }
    for (const auto& [id, definition] : world.content_.metaActions) {
        (void)definition;
        contentSummary["registered_meta_actions"].push_back(id);
    }
    for (const auto& [id, definition] : world.content_.conditions) {
        (void)definition;
        contentSummary["registered_conditions"].push_back(id);
    }
    Json failureCandidates = Json::array();
    for (const auto& event : world.state_.events) {
        if (event.sequence <= npc.reflectionCursor.lastEventSequence ||
            event.type != "npc.process_failed" || event.actor != targetNpcId) {
            continue;
        }
        failureCandidates.push_back(Json{{"sequence", event.sequence},
                                         {"action_id", event.data.value("action_id", event.target)},
                                         {"process_id", event.data.value("process_id", "")}});
        if (failureCandidates.size() >= 8) break;
    }
    return Json{{"schema_version", 1}, {"trigger", "daily_reflection"},
                {"target_npc", targetNpcId}, {"base_world_revision", world.state_.revision},
                {"world_time", world.state_.now.minute},
                {"runtime_metadata", world.provider_->runtimeMetadata("reflection:proposal")},
                {"reflection_prompt", world.content_.npcReflectionPrompt.empty()
                                           ? world.content_.npcEvolutionPrompt
                                           : world.content_.npcReflectionPrompt},
                {"current_state", Json{{"location", npc.location}, {"mode", npc.mode},
                                        {"strategy", npc.strategy}, {"goal_stage", npc.goalStage},
                                        {"needs", currentNeeds}, {"activity", activity},
                                        {"memory_count", npc.memories.size()},
                                        {"knowledge", knowledgeEvidence(npc)},
                                        {"experiences", experienceEvidence(npc)},
                                        {"memories", memoryEvidence(npc)},
                                        {"relationships", relationshipEvidence(npc)},
                                        {"stimuli", stimulusEvidence(npc)},
                                        {"pending_innovation_ids", [&npc] {
                                             Json value = Json::array();
                                             for (const auto& hypothesis : npc.innovationHypotheses) {
                                                 value.push_back(Json{{"id", hypothesis.value("id", "")},
                                                                      {"status", hypothesis.value("innovation_status", "hypothesized")}});
                                             }
                                             return value;
                                         }()},
                                        {"innovation_adoptions", [&npc] { Json value = Json::array(); for (const auto& adoption : npc.innovationAdoptions) value.push_back(Json{{"proposal_id", adoption.proposalId}, {"recipe_id", adoption.recipeId}, {"status", toString(adoption.status)}, {"reason", adoption.reason}}); return value; }()}}},
                {"reflection_cursor", Json{{"last_event_sequence", npc.reflectionCursor.lastEventSequence},
                                             {"last_reflection_day", npc.reflectionCursor.lastReflectionDay},
                                             {"reflection_revision", npc.reflectionCursor.reflectionRevision}}},
                {"new_evidence", reflectionEvidence(world.state_, npc)},
                // This deterministic hint prevents the model from spending
                // innovation tokens on ordinary successes. Runtime still
                // checks the cited sequences and repeats the admission gate;
                // the hint is advisory context, never authority.
                {"failure_candidates", failureCandidates},
                {"unresolved_issues", npc.unresolvedIssues},
                {"pending_innovation_hypotheses", npc.innovationHypotheses}, {"content", contentSummary}};
}

ProviderResponse WorldEvolutionSystem::requestReflection(const WorldService& world,
                                                         const std::string& targetNpcId) {
    const auto context = reflectionContext(world, targetNpcId);
    if (context.empty()) return ProviderResponse{false, {}, {}, "unknown_npc", Json::object(), Json::object()};
    return world.provider_->proposeReflection(context, targetNpcId);
}

ProviderResponse WorldEvolutionSystem::requestProposal(const WorldService& world,
                                                       const std::string& evolutionLevel,
                                                       const std::string& trigger,
                                                       const Json& validationFeedback,
                                                       const std::string& targetNpcId) {
    if (evolutionLevel != "level1" && evolutionLevel != "level2") {
        return ProviderResponse{false, {}, {}, "invalid_evolution_level", Json::object()};
    }
    const auto context = proposalContext(world, evolutionLevel, trigger, validationFeedback, targetNpcId);
    return world.provider_->proposeEvolution(context, evolutionLevel, validationFeedback);
}

void WorldEvolutionSystem::scheduleAutomatic() {
    // Automatic evolution is a content-configured wakeup. It only captures a
    // read-only context and queues a real provider request; validation,
    // sandboxing and independent approval remain in pumpAsyncProposals().
    if (!world_.evolution_ || !world_.provider_->capabilities().automaticEvolution ||
        !world_.content_.evolutionPolicy.enabled || world_.state_.now.minute <= 0 ||
        world_.state_.now.minute % kMinutesPerDay != 0) {
        return;
    }
    const int day = world_.state_.now.day();
    const auto schedule = [this, day](const std::string& level, int intervalDays,
                                      const std::string& targetNpcId) {
        if (intervalDays <= 0 || day % intervalDays != 0) return;
        const std::string key = "day:" + std::to_string(day) + ":" + level + ":" + targetNpcId;
        if (world_.content_.evolutionPolicy.coalescePending && world_.pendingEvolutionKeys_.contains(key)) return;
        const std::string trigger = (level == "level1" ? "daily_reflection:" : "world_reflection:") +
                                    std::to_string(day);
        const bool reflection = level == "level1";
        const auto context = reflection
                                 ? reflectionContext(world_, targetNpcId)
                                 : proposalContext(world_, level, trigger, Json::object(), targetNpcId);
        if (context.empty()) return;
        EvolutionRequest request;
        request.requestId = "evolution:auto:" + level + ":" + std::to_string(day) + ":" +
                            std::to_string(world_.state_.revision) + ":" + targetNpcId + ":attempt:0";
        request.key = key;
        request.evolutionLevel = level;
        request.targetNpcId = targetNpcId;
        request.requestKind = reflection ? "reflection" : "evolution";
        request.trigger = trigger;
        request.baseContentVersion = world_.content_.contentVersion;
        request.baseWorldRevision = world_.state_.revision;
        request.createdAt = world_.state_.now;
        request.deadline = GameTime{world_.state_.now.minute + world_.content_.evolutionPolicy.maxProposalAgeMinutes};
        request.coalescePending = world_.content_.evolutionPolicy.coalescePending;
        request.context = context;
        if (!world_.evolution_->submit(std::move(request)).accepted) return;
        world_.pendingEvolutionKeys_.insert(key);
    };
    if (world_.content_.evolutionPolicy.level1IntervalDays > 0 &&
        day % world_.content_.evolutionPolicy.level1IntervalDays == 0) {
        for (const auto& [npcId, npc] : world_.state_.npcs) {
            (void)npc;
            if (world_.hasNpc(npcId)) {
                schedule("level1", world_.content_.evolutionPolicy.level1IntervalDays, npcId);
            }
        }
    }
    schedule("level2", world_.content_.evolutionPolicy.level2IntervalDays, {});
}

std::size_t WorldEvolutionSystem::pumpAsyncProposals() {
    if (!world_.evolution_) return 0;
    const auto results = world_.evolution_->drain();
    std::size_t consumed = 0;
    const auto retryableProviderFailure = [](const std::string& error) {
        return error.starts_with("provider_truncated:") ||
               error.starts_with("invalid_provider_json:");
    };
    for (auto& queued : results) {
        ++consumed;
        const auto finish = [this, &queued] {
            world_.pendingEvolutionKeys_.erase(queued.request.key);
        };
        const auto audit = [this, &queued](const std::string& type, const std::string& detail,
                                           Json data) {
            WorldService::TransactionScope transaction(world_);
            data["request_id"] = queued.request.requestId;
            data["evolution_level"] = queued.request.evolutionLevel;
            data["target_npc"] = queued.request.targetNpcId;
            data["trigger"] = queued.request.trigger;
            data["base_content_version"] = queued.request.baseContentVersion;
            data["base_world_revision"] = queued.request.baseWorldRevision;
            data["attempt"] = queued.request.attempt;
            data["request_kind"] = queued.request.requestKind;
            data["provider"] = queued.provider;
            world_.addEvent(type, kWorldActor, queued.request.evolutionLevel, detail, std::move(data));
        };
        if (queued.request.requestKind == "reflection") {
            if (queued.request.targetNpcId.empty() || !world_.hasNpc(queued.request.targetNpcId)) {
                audit("reflection.rejected", "The Runtime rejected a reflection for an unknown NPC",
                      Json{{"code", "unknown_npc"}});
            } else if (!queued.response.ok) {
                if (queued.request.attempt < 2 && world_.provider_->capabilities().structuredRepair &&
                    retryableProviderFailure(queued.response.error)) {
                    const Json feedback{{"attempt", queued.request.attempt + 1},
                                        {"code", queued.response.error},
                                        {"message", "The previous structured function call was truncated or invalid. Return fewer items, shorter text, and one complete JSON tool call matching the schema."}};
                    auto repair = queued.request;
                    ++repair.attempt;
                    repair.requestId += ":repair:" + std::to_string(repair.attempt);
                    repair.validationFeedback = feedback;
                    if (world_.evolution_->submit(std::move(repair)).accepted) continue;
                }
                audit("reflection.failed", "The configured LLM did not return a daily reflection",
                      Json{{"error", queued.response.error.empty() ? "provider_unavailable" : queued.response.error}});
            } else {
                const auto result = commitReflection(queued.request, queued.response, queued.provider);
                if (!result.accepted) {
                    if (queued.request.attempt < 2 && world_.provider_->capabilities().structuredRepair) {
                        const Json feedback{{"attempt", queued.request.attempt + 1},
                                            {"code", result.code}, {"message", result.message},
                                            {"previous_result", queued.response.reflectionResult}};
                        auto repair = queued.request;
                        ++repair.attempt;
                        repair.requestId += ":repair:" + std::to_string(repair.attempt);
                        repair.validationFeedback = feedback;
                        if (world_.evolution_->submit(std::move(repair)).accepted) continue;
                    }
                    audit("reflection.rejected", "The Runtime rejected a structured daily reflection",
                          Json{{"code", result.code}, {"message", result.message}});
                }
            }
            finish();
            continue;
        }
        // Evolution is a proposal, not an intent that immediately starts an
        // activity. Unrelated committed facts may arrive while the model is
        // thinking; the proposal is rebased through current validation and
        // sandbox below. Content changes and the explicit game-time deadline
        // still invalidate the captured context.
        const bool expired = world_.state_.now.minute > queued.request.deadline.minute ||
                             world_.content_.contentVersion != queued.request.baseContentVersion;
        if (expired) {
            audit("evolution.proposal_expired", "An automatic evolution proposal arrived after its captured context expired",
                  Json{{"current_world_revision", world_.state_.revision},
                       {"current_content_version", world_.content_.contentVersion},
                       {"current_minute", world_.state_.now.minute}});
            finish();
            continue;
        }
        if (!queued.response.ok) {
            if (queued.request.attempt < 2 && world_.provider_->capabilities().structuredRepair &&
                retryableProviderFailure(queued.response.error)) {
                const Json feedback{{"attempt", queued.request.attempt + 1},
                                    {"code", queued.response.error},
                                    {"message", "The previous structured function call was truncated or invalid. Return one small complete proposal tool call matching the schema."}};
                auto repair = queued.request;
                ++repair.attempt;
                repair.requestId += ":repair:" + std::to_string(repair.attempt);
                repair.validationFeedback = feedback;
                if (world_.evolution_->submit(std::move(repair)).accepted) continue;
            }
            audit("evolution.proposal_failed", "The configured LLM did not return an evolution proposal",
                  Json{{"error", queued.response.error.empty() ? "provider_unavailable" : queued.response.error}});
            finish();
            continue;
        }
        try {
            auto proposal = EvolutionManager::normalizeProviderProposal(
                queued.response.evolutionProposal, world_, queued.request.evolutionLevel, queued.provider);
            proposal.targetNpcId = queued.request.targetNpcId;
            const auto validation = EvolutionManager::validate(proposal, world_.content_, world_.state_);
            if (!validation.valid) {
                if (queued.request.attempt < 2 && world_.provider_->capabilities().structuredRepair) {
                    const Json feedback{{"attempt", queued.request.attempt + 1}, {"code", validation.code},
                                        {"message", validation.message},
                                        {"previous_proposal", EvolutionManager::toJson(proposal)}};
                    auto repair = queued.request;
                    ++repair.attempt;
                    repair.requestId += ":repair:" + std::to_string(repair.attempt);
                    repair.validationFeedback = feedback;
                    if (world_.evolution_->submit(std::move(repair)).accepted) continue;
                }
                audit("evolution.proposal_rejected", "The Runtime rejected an automatic evolution proposal",
                      Json{{"code", validation.code}, {"message", validation.message},
                           {"proposal", EvolutionManager::toJson(proposal)}});
                finish();
                continue;
            }
            const auto sandbox = EvolutionManager::sandbox(proposal, world_);
            Json sandboxJson{{"passed", sandbox.passed}, {"code", sandbox.code},
                             {"message", sandbox.message}, {"proposal_id", sandbox.proposalId},
                             {"overlay_version", sandbox.overlayVersion},
                             {"base_content_version", sandbox.baseContentVersion},
                             {"base_revision", sandbox.baseRevision}, {"final_revision", sandbox.finalRevision},
                             {"metrics", sandbox.metrics}, {"event_diff", sandbox.eventDiff}};
            audit(sandbox.passed ? "evolution.proposal_ready" : "evolution.proposal_rejected",
                  sandbox.passed ? "A validated automatic evolution proposal passed its deterministic sandbox"
                                 : "An automatic evolution proposal failed its deterministic sandbox",
                  Json{{"proposal", EvolutionManager::toJson(proposal)}, {"sandbox", sandboxJson}});
            finish();
        } catch (const std::exception& error) {
            if (queued.request.attempt < 2 && world_.provider_->capabilities().structuredRepair) {
                const Json feedback{{"attempt", queued.request.attempt + 1},
                                    {"code", "invalid_evolution_proposal"},
                                    {"message", error.what()},
                                    {"previous_proposal", queued.response.evolutionProposal}};
                auto repair = queued.request;
                ++repair.attempt;
                repair.requestId += ":repair:" + std::to_string(repair.attempt);
                repair.validationFeedback = feedback;
                if (world_.evolution_->submit(std::move(repair)).accepted) continue;
            }
            audit("evolution.proposal_rejected", "The Runtime could not parse or sandbox an automatic evolution proposal",
                  Json{{"error", error.what()}});
            finish();
        }
    }
    return consumed;
}

CommandResult WorldEvolutionSystem::commitReflection(const EvolutionRequest& request,
                                                     const ProviderResponse& response,
                                                     const std::string& provider) {
    WorldService::TransactionScope transaction(world_);
    const auto npcFound = world_.state_.npcs.find(request.targetNpcId);
    if (npcFound == world_.state_.npcs.end()) return world_.reject("unknown_npc", "Reflection target does not exist.");
    auto& npc = npcFound->second;
    if (npc.reflectionCursor.lastReflectionDay >= world_.state_.now.day()) {
        return world_.reject("reflection_already_processed", "This NPC already has a committed reflection for the current day.");
    }
    if (world_.content_.contentVersion != request.baseContentVersion) {
        return world_.reject("reflection_stale", "Reflection context no longer matches the current content version.");
    }
    if (!response.reflectionResult.is_object()) {
        return world_.reject("invalid_reflection", "The provider returned no structured reflection result.");
    }
    const auto& result = response.reflectionResult;
    if (result.value("schema_version", 1) != 1 || result.value("npc_id", request.targetNpcId) != request.targetNpcId) {
        return world_.reject("invalid_reflection_identity", "Reflection identity or schema version is invalid.");
    }
    const std::uint64_t boundarySequence = world_.state_.events.empty() ? 0 : world_.state_.events.back().sequence;
    std::set<std::uint64_t> evidence;
    const auto evidenceJson = result.value("evidence_event_sequences", Json::array());
    if (!evidenceJson.is_array() || evidenceJson.size() > 32) {
        return world_.reject("invalid_reflection_evidence", "Reflection evidence must be a bounded array.");
    }
    for (const auto& item : evidenceJson) {
        if (!item.is_number_unsigned()) return world_.reject("invalid_reflection_evidence", "Reflection evidence must use event sequences.");
        const auto sequence = item.get<std::uint64_t>();
        if (sequence <= npc.reflectionCursor.lastEventSequence || sequence > boundarySequence ||
            !evidence.insert(sequence).second) {
            return world_.reject("invalid_reflection_evidence", "Reflection evidence must be new, committed and unique.");
        }
        const auto event = std::find_if(world_.state_.events.begin(), world_.state_.events.end(),
                                       [sequence](const auto& candidate) { return candidate.sequence == sequence; });
        const bool privateEvent = event != world_.state_.events.end() &&
                                  (event->type.rfind("memory.", 0) == 0 ||
                                   event->type.rfind("knowledge.", 0) == 0 ||
                                   event->type.rfind("experience.", 0) == 0 ||
                                   event->type == "drive.adapted");
        if (event == world_.state_.events.end() ||
            (privateEvent && event->actor != request.targetNpcId && event->target != request.targetNpcId &&
             event->target != npcEntityId(request.targetNpcId))) {
            return world_.reject("reflection_private_evidence", "A reflection cannot cite another NPC's private cognition.");
        }
    }
    const auto hasSource = [&evidence, boundarySequence, &npc](std::uint64_t sequence) {
        return sequence > npc.reflectionCursor.lastEventSequence && sequence <= boundarySequence && evidence.contains(sequence);
    };
    const auto boundedText = [](const Json& value, const char* key, std::size_t max) {
        return value.is_object() && value.contains(key) && value.at(key).is_string() &&
               !value.at(key).get<std::string>().empty() && value.at(key).get<std::string>().size() <= max;
    };
    const auto cognition = result.value("cognition_updates", Json::object());
    if (!cognition.is_object()) return world_.reject("invalid_reflection_cognition", "cognition_updates must be an object.");
    const auto beliefs = cognition.value("belief_updates", Json::array());
    const auto experiences = cognition.value("experience_updates", Json::array());
    const auto memories = cognition.value("memory_updates", Json::array());
    if (!beliefs.is_array() || !experiences.is_array() || !memories.is_array() ||
        beliefs.size() > 8 || experiences.size() > 8 || memories.size() > 8) {
        return world_.reject("invalid_reflection_cognition", "Reflection cognition updates exceed bounded limits.");
    }
    for (const auto& item : beliefs) {
        if (!boundedText(item, "subject", 128) || !boundedText(item, "predicate", 128) ||
            !boundedText(item, "value", 512) || !item.contains("source_sequence") ||
            !item.at("source_sequence").is_number_unsigned() || !item.contains("confidence") ||
            !item.at("confidence").is_number() || item.at("confidence").get<double>() < 0.0 ||
            item.at("confidence").get<double>() > 1.0 ||
            !hasSource(item.at("source_sequence").get<std::uint64_t>())) {
            return world_.reject("invalid_reflection_cognition", "Belief updates require bounded sourced fields.");
        }
    }
    for (const auto& item : experiences) {
        if (!boundedText(item, "domain", 128) || !boundedText(item, "trigger", 256) ||
            !boundedText(item, "action", 128) || !boundedText(item, "outcome", 512) ||
            !item.contains("source_sequence") || !item.at("source_sequence").is_number_unsigned() ||
            !item.contains("confidence") || !item.at("confidence").is_number() ||
            item.at("confidence").get<double>() < 0.0 || item.at("confidence").get<double>() > 1.0 ||
            !hasSource(item.at("source_sequence").get<std::uint64_t>())) {
            return world_.reject("invalid_reflection_cognition", "Experience updates require bounded sourced fields.");
        }
    }
    for (const auto& item : memories) {
        if (!boundedText(item, "text", 512) || !item.contains("source_sequence") ||
            !item.at("source_sequence").is_number_unsigned() ||
            (item.contains("confidence") && (!item.at("confidence").is_number() ||
                                              item.at("confidence").get<double>() < 0.0 ||
                                              item.at("confidence").get<double>() > 1.0)) ||
            !hasSource(item.at("source_sequence").get<std::uint64_t>())) {
            return world_.reject("invalid_reflection_cognition", "Memory updates require a committed source event.");
        }
    }
    const auto strategy = result.value("strategy_adjustment", Json::object());
    if (!strategy.is_object() || (strategy.contains("strategy") &&
        (!strategy.at("strategy").is_string() || strategy.at("strategy").get<std::string>().empty() ||
         strategy.at("strategy").get<std::string>().size() > 128))) {
        return world_.reject("invalid_reflection_strategy", "Strategy adjustment must be a bounded string.");
    }
    const auto goal = result.value("goal_adjustment", Json::object());
    if (!goal.is_object()) return world_.reject("invalid_reflection_goal", "Goal adjustment must be an object.");
    if (goal.contains("priority_delta") && (!goal.at("priority_delta").is_number() ||
        goal.at("priority_delta").get<double>() < -1.0 || goal.at("priority_delta").get<double>() > 1.0)) {
        return world_.reject("invalid_reflection_goal", "Goal priority_delta must be bounded.");
    }
    if (goal.contains("status") && (!goal.at("status").is_string() || goal.at("status") != "cancelled")) {
        return world_.reject("invalid_reflection_goal", "Only a governed cancellation may be requested by reflection.");
    }
    const auto openIssues = result.value("unresolved_issues", Json::array());
    const auto resolvedIssues = result.value("resolved_issues", Json::array());
    if (!openIssues.is_array() || !resolvedIssues.is_array() || openIssues.size() > 16 || resolvedIssues.size() > 16) {
        return world_.reject("invalid_reflection_issues", "Reflection issue lists are bounded arrays.");
    }
    std::set<std::string> issueSet(npc.unresolvedIssues.begin(), npc.unresolvedIssues.end());
    for (const auto& item : openIssues) {
        if (!item.is_string() || item.get<std::string>().empty() || item.get<std::string>().size() > 256) {
            return world_.reject("invalid_reflection_issues", "Unresolved issue text must be bounded.");
        }
        issueSet.insert(item.get<std::string>());
    }
    for (const auto& item : resolvedIssues) {
        if (!item.is_string() || !issueSet.contains(item.get<std::string>())) {
            return world_.reject("invalid_reflection_issues", "A reflection may resolve only a persisted issue.");
        }
        issueSet.erase(item.get<std::string>());
    }
    const auto hypotheses = result.value("innovation_hypotheses", Json::array());
    if (!hypotheses.is_array() || hypotheses.size() > 2) {
        return world_.reject("invalid_reflection_innovation", "Innovation hypotheses are optional and bounded.");
    }
    Json innovationRejection = Json::object();
    if (!hypotheses.empty()) {
        // A reflection may suggest innovation only after a durable problem
        // exists and the cited window contains two failures of the same
        // authored process.  This is a deterministic admission gate: the
        // model may describe a hypothesis, but it cannot manufacture the
        // evidence needed to create one.
        std::map<std::string, std::size_t> failureCounts;
        for (const auto& event : world_.state_.events) {
            if (event.sequence <= npc.reflectionCursor.lastEventSequence ||
                event.sequence > boundarySequence || !evidence.contains(event.sequence) ||
                event.type != "npc.process_failed" || event.actor != request.targetNpcId) {
                continue;
            }
            const auto actionId = event.data.value("action_id", event.target);
            if (!actionId.empty()) ++failureCounts[actionId];
        }
        const bool repeatedFailure = std::any_of(
            failureCounts.begin(), failureCounts.end(),
            [](const auto& item) { return item.second >= 2; });
        if (npc.unresolvedIssues.empty() || !repeatedFailure) {
            innovationRejection = Json{
                {"code", "innovation_evidence_insufficient"},
                {"message", "An innovation hypothesis requires a persisted unresolved issue and two cited failures of the same process."}};
        }
    }
    std::vector<EvolutionProposal> normalizedHypotheses;
    if (innovationRejection.empty()) for (const auto& hypothesis : hypotheses) {
        try {
            auto proposal = EvolutionManager::normalizeProviderProposal(hypothesis, world_, "level1", provider);
            proposal.targetNpcId = request.targetNpcId;
            proposal.kind = "innovation";
            proposal.innovationStatus = "hypothesized";
            const auto validation = EvolutionManager::validate(proposal, world_.content_, world_.state_);
            if (!validation.valid) {
                innovationRejection = Json{{"code", validation.code}, {"message", validation.message}};
                normalizedHypotheses.clear();
                break;
            }
            normalizedHypotheses.push_back(std::move(proposal));
        } catch (const std::exception& error) {
            innovationRejection = Json{{"code", "invalid_reflection_innovation"}, {"message", error.what()}};
            normalizedHypotheses.clear();
            break;
        }
    }
    const auto innovationActions = result.value("innovation_actions", Json::array());
    if (!innovationActions.is_array() || innovationActions.size() > 8) {
        innovationRejection = Json{{"code", "invalid_reflection_innovation"},
                                   {"message", "Innovation lifecycle actions are bounded."}};
    }
    struct InnovationTransition { std::size_t index; std::string next; };
    std::vector<InnovationTransition> transitions;
    if (innovationRejection.empty()) for (const auto& action : innovationActions) {
        if (!action.is_object() || !boundedText(action, "proposal_id", 128) ||
            !boundedText(action, "status", 32)) {
            innovationRejection = Json{{"code", "invalid_reflection_innovation"},
                                       {"message", "Innovation lifecycle actions require bounded ids and statuses."}};
            transitions.clear();
            break;
        }
        const auto found = std::find_if(npc.innovationHypotheses.begin(), npc.innovationHypotheses.end(),
                                        [&action](const Json& item) {
                                            return item.value("id", "") == action.at("proposal_id").get<std::string>();
                                        });
        if (found == npc.innovationHypotheses.end()) {
            innovationRejection = Json{{"code", "unknown_innovation_hypothesis"},
                                       {"message", "A reflection may transition only its persisted hypotheses."}};
            transitions.clear();
            break;
        }
        const auto next = action.at("status").get<std::string>();
        if (next != "refining" && next != "evaluated" && next != "accepted" &&
            next != "rejected" && next != "abandoned") {
            innovationRejection = Json{{"code", "invalid_reflection_innovation"},
                                       {"message", "Innovation lifecycle status is not registered."}};
            transitions.clear();
            break;
        }
        const auto index = static_cast<std::size_t>(std::distance(npc.innovationHypotheses.begin(), found));
        auto proposal = EvolutionManager::fromJson(*found);
        const auto validation = EvolutionManager::transitionInnovation(proposal, next);
        if (!validation.valid) {
            innovationRejection = Json{{"code", validation.code}, {"message", validation.message}};
            transitions.clear();
            break;
        }
        transitions.push_back(InnovationTransition{index, next});
    }
    for (std::size_t index = 0; index < beliefs.size(); ++index) {
        const auto& item = beliefs.at(index);
        const auto applied = CognitionSystem(world_).assertKnownFact(
            request.targetNpcId, item.at("subject"), item.at("predicate"), item.at("value"),
            item.at("source_sequence"), item.value("scope", "inference"), item.at("confidence"),
            request.targetNpcId, "reflection:" + request.targetNpcId + ":" + std::to_string(world_.state_.now.day()) + ":belief:" + std::to_string(index));
        if (!applied.accepted) { transaction.rollback(); return applied; }
    }
    for (std::size_t index = 0; index < experiences.size(); ++index) {
        const auto& item = experiences.at(index);
        const auto applied = CognitionSystem(world_).recordExperience(
            request.targetNpcId, item.at("domain"), item.at("trigger"), item.at("action"), item.at("outcome"),
            item.at("source_sequence"), item.at("confidence"), item.value("personal", true),
            request.targetNpcId, "reflection:" + request.targetNpcId + ":" + std::to_string(world_.state_.now.day()) + ":experience:" + std::to_string(index));
        if (!applied.accepted) { transaction.rollback(); return applied; }
    }
    for (std::size_t index = 0; index < memories.size(); ++index) {
        const auto& item = memories.at(index);
        npc.memories.push_back(Memory{item.value("kind", "reflection"), item.at("text"), world_.state_.now,
                                      item.value("confidence", 1.0)});
        world_.addEvent("memory.reflected", request.targetNpcId, request.targetNpcId,
                        "Daily reflection consolidated a sourced memory",
                        Json{{"source_sequence", item.at("source_sequence")}, {"text", item.at("text")},
                             {"provider", provider}});
    }
    for (const auto& transition : transitions) {
        auto proposal = EvolutionManager::fromJson(npc.innovationHypotheses.at(transition.index));
        const auto changed = EvolutionManager::transitionInnovation(proposal, transition.next);
        if (!changed.valid) { transaction.rollback(); return world_.reject(changed.code, changed.message); }
        npc.innovationHypotheses.at(transition.index) = EvolutionManager::toJson(proposal);
        world_.addEvent("innovation.hypothesis_transitioned", request.targetNpcId, request.targetNpcId,
                        "Daily reflection advanced a private innovation hypothesis",
                        Json{{"proposal_id", proposal.id}, {"status", proposal.innovationStatus}});
    }
    if (!innovationRejection.empty()) {
        world_.addEvent("innovation.hypothesis_rejected", request.targetNpcId, request.targetNpcId,
                        "Daily reflection discarded an invalid optional innovation branch",
                        Json{{"code", innovationRejection.value("code", "invalid_reflection_innovation")},
                             {"message", innovationRejection.value("message", "innovation branch rejected")},
                             {"provider", provider}});
    }
    if (strategy.contains("strategy")) {
        npc.strategy = strategy.at("strategy");
        ++npc.stateVersion;
        world_.addEvent("strategy.reflected", request.targetNpcId, request.targetNpcId,
                        "Daily reflection adjusted the NPC strategy", Json{{"strategy", npc.strategy}});
    }
    if (goal.contains("priority_delta") && npc.activity.goal) {
        npc.activity.goal->priority = std::clamp(npc.activity.goal->priority + goal.at("priority_delta").get<double>(), 0.0, 1.0);
        ++npc.stateVersion;
        world_.addEvent("goal.reflected", request.targetNpcId, request.targetNpcId,
                        "Daily reflection adjusted the active goal priority",
                        Json{{"priority", npc.activity.goal->priority}});
    }
    if (goal.value("status", "") == "cancelled" && npc.activity.goal) {
        setActivityStatus(npc.activity, ActivityStatus::Cancelled);
        ++npc.stateVersion;
        world_.addEvent("goal.cancelled", request.targetNpcId, request.targetNpcId,
                        "Daily reflection cancelled the active goal", Json::object());
    }
    npc.unresolvedIssues.assign(issueSet.begin(), issueSet.end());
    for (const auto& hypothesis : normalizedHypotheses) {
        const auto serialized = EvolutionManager::toJson(hypothesis);
        const auto existing = std::find_if(npc.innovationHypotheses.begin(), npc.innovationHypotheses.end(),
                                           [&hypothesis](const Json& item) {
                                               return item.value("id", "") == hypothesis.id;
                                           });
        if (existing == npc.innovationHypotheses.end()) {
            if (npc.innovationHypotheses.size() >= 8) {
                transaction.rollback();
                return world_.reject("innovation_hypothesis_limit", "An NPC cannot retain more than eight pending hypotheses.");
            }
            npc.innovationHypotheses.push_back(serialized);
        } else {
            *existing = serialized;
        }
        world_.addEvent("innovation.hypothesis_created", request.targetNpcId, request.targetNpcId,
                        "Daily reflection produced a governed private innovation hypothesis",
                        Json{{"proposal_id", hypothesis.id}, {"evolution_level", "level1"},
                             {"target_npc", request.targetNpcId},
                             {"evidence_event_sequences", hypothesis.evidenceEventSequences},
                             {"innovation_status", hypothesis.innovationStatus},
                             {"proposal", serialized}, {"provider", provider}});
    }
    npc.reflectionCursor.lastReflectionDay = world_.state_.now.day();
    const auto reflectionEvent = world_.addEvent(
        "reflection.committed", request.targetNpcId, request.targetNpcId,
        "The Runtime committed a structured daily reflection",
        Json{{"provider", provider}, {"evidence_event_sequences", evidenceJson},
             {"innovation_count", normalizedHypotheses.size()}, {"unresolved_issue_count", npc.unresolvedIssues.size()},
             {"innovation_rejected", !innovationRejection.empty()}});
    npc.reflectionCursor.lastEventSequence = reflectionEvent.sequence;
    npc.reflectionCursor.reflectionRevision = reflectionEvent.worldRevision;
    ++npc.stateVersion;
    return CommandResult{true, "ok", "reflection committed", reflectionEvent};
}

CommandResult WorldEvolutionSystem::startAdoption(const std::string& npcId,
                                                  const std::string& proposalId) {
    WorldService::TransactionScope transaction(world_);
    const auto npcFound = world_.state_.npcs.find(npcId);
    if (npcFound == world_.state_.npcs.end()) return world_.reject("unknown_npc", "Adoption target does not exist.");
    const auto hypothesis = std::find_if(npcFound->second.innovationHypotheses.begin(),
                                         npcFound->second.innovationHypotheses.end(),
                                         [&proposalId](const Json& item) {
                                             return item.value("id", "") == proposalId;
                                         });
    if (hypothesis == npcFound->second.innovationHypotheses.end()) {
        return world_.reject("unknown_innovation_hypothesis", "Adoption requires a persisted hypothesis owned by the NPC.");
    }
    if (hypothesis->value("innovation_status", "hypothesized") != "accepted") {
        return world_.reject("innovation_not_accepted", "Only an accepted innovation hypothesis can be adopted.");
    }
    const auto active = std::find_if(world_.state_.activeOverlays.begin(), world_.state_.activeOverlays.end(),
                                     [&proposalId](const auto& overlay) { return overlay.proposalId == proposalId; });
    if (active == world_.state_.activeOverlays.end()) {
        return world_.reject("innovation_overlay_inactive", "Adoption requires an independently approved active overlay.");
    }
    std::string recipeId;
    const auto operations = hypothesis->value("operations", Json::array());
    if (operations.is_array() && operations.size() == 1 && operations.at(0).is_object()) {
        recipeId = operations.at(0).value("id", "");
    }
    const auto* recipe = recipeId.empty() ? nullptr : world_.recipeForAction(recipeId);
    if (recipe == nullptr) {
        return world_.reject("innovation_recipe_inactive", "The active overlay does not expose the adopted behavior recipe.");
    }
    if (std::find(recipe->allowedActors.begin(), recipe->allowedActors.end(), npcId) == recipe->allowedActors.end()) {
        return world_.reject("innovation_recipe_forbidden", "The adopted recipe is not authorized for this NPC.");
    }
    auto& adoptions = npcFound->second.innovationAdoptions;
    auto existing = std::find_if(adoptions.begin(), adoptions.end(),
                                       [&proposalId](const auto& item) { return item.proposalId == proposalId; });
    if (existing != adoptions.end()) {
        if (existing->status == InnovationAdoptionStatus::Executing ||
            existing->status == InnovationAdoptionStatus::Completed) {
            return world_.reject("innovation_adoption_exists", "That innovation is already executing or completed for this NPC.");
        }
        if (existing->recipeId != recipeId) {
            return world_.reject("innovation_adoption_recipe_changed", "An adoption record cannot change its recipe identity.");
        }
    }
    if (existing == adoptions.end()) {
        if (adoptions.size() >= 8) {
            return world_.reject("innovation_adoption_limit", "An NPC cannot retain more than eight adoption records.");
        }
        adoptions.push_back(InnovationAdoptionState{proposalId, recipeId, {},
                                                     InnovationAdoptionStatus::Pending, {-1}, {-1}, {}, 0,
                                                     Json::object(), Json::object()});
        existing = std::prev(adoptions.end());
    }
    auto& adoption = *existing;
    const std::map<std::string, int> requirements = recipe->reservationMode == "input_resources"
                                                        ? recipe->reservations
                                                        : std::map<std::string, int>{};
    const auto markBlocked = [&](const std::string& reason) {
        adoption.status = InnovationAdoptionStatus::ResourceBlocked;
        adoption.updatedAt = world_.state_.now;
        adoption.reason = reason;
        ++npcFound->second.stateVersion;
        const auto event = world_.addEvent("innovation.adoption_blocked", npcId, proposalId,
                                           "An accepted innovation is waiting for its required resources",
                                           Json{{"proposal_id", proposalId}, {"recipe_id", recipeId},
                                                {"status", toString(adoption.status)}, {"reason", reason},
                                                {"required_resources", requirements}});
        return CommandResult{true, "resource_blocked", reason, event};
    };
    if (npcFound->second.activity.status != ActivityStatus::Proposed || npcFound->second.activity.process) {
        return world_.reject("npc_busy", "An innovation adoption cannot replace an active NPC process.");
    }
    std::optional<std::vector<std::string>> victims = std::vector<std::string>{};
    if (recipe->preemptionPolicy == "lower_priority") {
        victims = world_.planResourcePreemption(npcId, requirements, recipe->priority);
        if (!victims) return markBlocked("required resources are reserved by equal or higher priority work");
    }
    for (const auto& [resource, amount] : requirements) {
        if (world_.availableResourceValue(resource) < amount) {
            return markBlocked("required resources are not available");
        }
    }
    if (victims) {
        for (const auto& victimId : *victims) {
            auto victim = world_.state_.npcs.find(victimId);
            if (victim != world_.state_.npcs.end() && victim->second.activity.process) {
                world_.suspendProcess(victimId, *victim->second.activity.process, "resource_preempted",
                                      Json{{"preempted_by", npcId}, {"preempting_proposal_id", proposalId}});
            }
        }
    }
    for (const auto& [resource, amount] : requirements) world_.state_.resources[resource].reserved += amount;
    adoption.status = InnovationAdoptionStatus::Executing;
    adoption.startedAt = world_.state_.now;
    adoption.updatedAt = world_.state_.now;
    adoption.reason = "accepted overlay available";
    adoption.processId = "innovation-process:" + npcId + ":" + proposalId + ":" +
                         std::to_string(world_.state_.events.size() + 1);
    adoption.startEventSequence = world_.state_.events.empty() ? 0 : world_.state_.events.back().sequence;
    adoption.observedMetrics = Json::object();
    adoption.metricEvaluation = Json::object();
    if (recipe->durationMinutes <= 0) {
        ProcessExecution instant;
        instant.id = adoption.processId;
        instant.actionId = recipeId;
        instant.recipeId = recipeId;
        instant.planId = recipe->planId;
        instant.trigger = "innovation_adoption:" + proposalId;
        instant.startedAt = world_.state_.now;
        instant.completesAt = world_.state_.now;
        instant.parameters = Json::object();
        instant.requiredResources = requirements;
        instant.reservedResources = requirements;
        const bool applied = world_.applyActionEffects(npcId, recipeId, &instant.reservedResources, &instant.parameters);
        if (!applied) {
            world_.releaseReservations(instant);
            adoption.status = InnovationAdoptionStatus::Failed;
            adoption.updatedAt = world_.state_.now;
            adoption.reason = "recipe completion failed";
            const auto event = world_.addEvent("innovation.adoption_failed", npcId, proposalId,
                                               "An innovation recipe failed during immediate adoption",
                                               Json{{"proposal_id", proposalId}, {"recipe_id", recipeId}});
            return CommandResult{true, "failed", adoption.reason, event};
        }
        world_.releaseReservations(instant);
        adoption.status = InnovationAdoptionStatus::Completed;
        adoption.updatedAt = world_.state_.now;
        adoption.reason = "recipe completed";
        const auto event = world_.addEvent("innovation.adoption_completed", npcId, proposalId,
                                           "An innovation recipe completed immediately",
                                           Json{{"proposal_id", proposalId}, {"recipe_id", recipeId}});
        ++npcFound->second.stateVersion;
        return CommandResult{true, "completed", adoption.reason, event};
    }
    GoalState goal{"innovation-goal:" + adoption.processId, "adopt:" + recipeId, recipeId, 0.5,
                   GoalStatus::Active, world_.state_.now,
                   GameTime{world_.state_.now.minute + recipe->durationMinutes},
                   "accepted innovation hypothesis"};
    ProcessExecution process;
    process.id = adoption.processId;
    process.goalId = goal.id;
    process.actionId = recipeId;
    process.recipeId = recipeId;
    process.planId = recipe->planId;
    process.trigger = "innovation_adoption:" + proposalId;
    process.startedAt = world_.state_.now;
    process.completesAt = GameTime{world_.state_.now.minute + recipe->durationMinutes};
    process.parameters = Json::object();
    process.interruptible = recipe->interruptible;
    process.interruptSalienceThreshold = recipe->interruptSalienceThreshold;
    process.reservationMode = recipe->reservationMode;
    process.reservedResources = requirements;
    process.requiredResources = requirements;
    process.priority = recipe->priority;
    process.preemptionPolicy = recipe->preemptionPolicy;
    npcFound->second.activity.goal = std::move(goal);
    npcFound->second.activity.process = std::move(process);
    setActivityStatus(npcFound->second.activity, ActivityStatus::Active);
    ++npcFound->second.stateVersion;
    const auto event = world_.addEvent("innovation.adoption_started", npcId, proposalId,
                                       "The NPC began adopting an accepted innovation",
                                       Json{{"proposal_id", proposalId}, {"recipe_id", recipeId},
                                            {"process_id", adoption.processId},
                                            {"required_resources", requirements},
                                            {"status", toString(adoption.status)}});
    adoption.startEventSequence = event.sequence;
    return CommandResult{true, "executing", "adoption process started", event};
}

CommandResult WorldEvolutionSystem::transitionInnovationHypothesis(const std::string& npcId,
                                                                    const std::string& proposalId,
                                                                    const std::string& nextStatus,
                                                                    const std::string& actor) {
    WorldService::TransactionScope transaction(world_);
    const auto npcFound = world_.state_.npcs.find(npcId);
    if (npcFound == world_.state_.npcs.end()) {
        return world_.reject("unknown_npc", "Innovation hypothesis owner does not exist.");
    }
    const auto found = std::find_if(npcFound->second.innovationHypotheses.begin(),
                                    npcFound->second.innovationHypotheses.end(),
                                    [&proposalId](const Json& item) {
                                        return item.value("id", "") == proposalId;
                                    });
    if (found == npcFound->second.innovationHypotheses.end()) {
        return world_.reject("unknown_innovation_hypothesis", "The innovation hypothesis is not owned by this NPC.");
    }
    try {
        auto proposal = EvolutionManager::fromJson(*found);
        const auto changed = EvolutionManager::transitionInnovation(proposal, nextStatus);
        if (!changed.valid) return world_.reject(changed.code, changed.message);
        *found = EvolutionManager::toJson(proposal);
        ++npcFound->second.stateVersion;
        const auto event = world_.addEvent(
            "innovation.hypothesis_transitioned", npcId, proposalId,
            "An independent governance actor advanced a private innovation hypothesis",
            Json{{"proposal_id", proposalId}, {"status", proposal.innovationStatus}, {"actor", actor}});
        return CommandResult{true, "ok", "innovation hypothesis transitioned", event};
    } catch (const std::exception& error) {
        return world_.reject("invalid_innovation_hypothesis", error.what());
    }
}

CommandResult WorldEvolutionSystem::transitionAdoption(const std::string& npcId,
                                                       const std::string& proposalId,
                                                       InnovationAdoptionStatus status,
                                                       const std::string& reason) {
    WorldService::TransactionScope transaction(world_);
    const auto npcFound = world_.state_.npcs.find(npcId);
    if (npcFound == world_.state_.npcs.end()) return world_.reject("unknown_npc", "Adoption target does not exist.");
    const auto adoption = std::find_if(npcFound->second.innovationAdoptions.begin(),
                                       npcFound->second.innovationAdoptions.end(),
                                       [&proposalId](const auto& item) { return item.proposalId == proposalId; });
    if (adoption == npcFound->second.innovationAdoptions.end()) {
        return world_.reject("unknown_innovation_adoption", "Adoption state does not exist for this NPC.");
    }
    const auto current = adoption->status;
    const bool allowed =
        (current == InnovationAdoptionStatus::Pending &&
         (status == InnovationAdoptionStatus::ResourceBlocked || status == InnovationAdoptionStatus::Executing ||
          status == InnovationAdoptionStatus::Abandoned)) ||
        (current == InnovationAdoptionStatus::ResourceBlocked &&
         (status == InnovationAdoptionStatus::Executing || status == InnovationAdoptionStatus::Abandoned)) ||
        (current == InnovationAdoptionStatus::Executing &&
         (status == InnovationAdoptionStatus::Completed || status == InnovationAdoptionStatus::Failed ||
          status == InnovationAdoptionStatus::Abandoned));
    if (!allowed) return world_.reject("invalid_innovation_adoption_transition", "Adoption lifecycle transition is not allowed.");
    adoption->status = status;
    adoption->updatedAt = world_.state_.now;
    adoption->reason = reason;
    ++npcFound->second.stateVersion;
    const auto event = world_.addEvent("innovation.adoption_transitioned", npcId, proposalId,
                                       "The NPC adoption lifecycle advanced",
                                       Json{{"proposal_id", proposalId}, {"from", toString(current)},
                                            {"to", toString(status)}, {"reason", reason}});
    return CommandResult{true, "ok", "adoption state updated", event};
}

CommandResult WorldEvolutionSystem::activateOverlay(const EvolutionProposal& proposal,
                                                     const EvolutionSandboxResult& sandbox) {
    WorldService::TransactionScope transaction(world_);
    if (proposal.status != EvolutionProposalStatus::Approved) {
        return world_.reject("overlay_not_approved", "Only an approved evolution proposal can activate an overlay.");
    }
    if (!sandbox.passed || sandbox.proposalId != proposal.id || sandbox.overlayVersion.empty()) {
        return world_.reject("overlay_sandbox_required", "A passing sandbox result for this proposal is required.");
    }
    if (sandbox.baseContentVersion != proposal.baseContentVersion ||
        sandbox.baseRevision != world_.state_.revision || proposal.baseWorldRevision != world_.state_.revision) {
        return world_.reject("overlay_sandbox_stale",
                             "The sandbox and proposal must target the current content version and world revision.");
    }
    if (std::any_of(world_.state_.activeOverlays.begin(), world_.state_.activeOverlays.end(),
                    [&proposal](const auto& active) { return active.proposalId == proposal.id; })) {
        return world_.reject("overlay_already_active", "That evolution overlay is already active.");
    }
    auto candidate = world_.content_;
    const auto applied = EvolutionManager::applyOverlay(candidate, proposal);
    if (!applied.valid) return world_.reject(applied.code, applied.message);
    if (candidate.contentVersion != sandbox.overlayVersion) {
        return world_.reject("overlay_version_mismatch", "The sandbox result does not match the activated content version.");
    }
    const auto previousVersion = world_.content_.contentVersion;
    world_.content_ = std::move(candidate);
    world_.state_.contentVersion = world_.content_.contentVersion;
    world_.state_.activeOverlays.push_back(
        ActiveOverlayState{proposal.id, proposal.baseContentVersion, world_.content_.contentVersion,
                           proposal.operations});
    const auto event = world_.addEvent(
        "evolution.overlay_activated", "developer", proposal.id, "An approved content overlay became active",
        Json{{"proposal_id", proposal.id}, {"overlay_version", world_.content_.contentVersion},
             {"previous_content_version", previousVersion},
             {"base_content_version", proposal.baseContentVersion},
             {"active_overlay_ids", overlayIds(world_.state_)}});
    world_.rebuildEntityRegistry();
    return CommandResult{true, "ok", world_.content_.contentVersion, event};
}

CommandResult WorldEvolutionSystem::rollbackOverlay(const std::string& proposalId) {
    WorldService::TransactionScope transaction(world_);
    const auto active = std::find_if(world_.state_.activeOverlays.begin(), world_.state_.activeOverlays.end(),
                                     [&proposalId](const auto& item) { return item.proposalId == proposalId; });
    if (active == world_.state_.activeOverlays.end()) {
        return world_.reject("overlay_not_active", "That overlay is not active.");
    }
    if (std::next(active) != world_.state_.activeOverlays.end()) {
        return world_.reject("overlay_not_top", "Roll back newer overlays before removing an older overlay.");
    }
    const auto previousVersion = world_.content_.contentVersion;
    world_.content_ = world_.baseContent_;
    for (auto record = world_.state_.activeOverlays.begin(); record != active; ++record) {
        EvolutionProposal replay{record->proposalId, "declarative_capability", "snapshot",
                                 record->baseContentVersion, 0, record->operations,
                                 EvolutionProposalStatus::Approved, {}, "level2", {}};
        const auto applied = EvolutionManager::applyOverlay(world_.content_, replay);
        if (!applied.valid) return world_.reject(applied.code, applied.message);
        if (world_.content_.contentVersion != record->overlayVersion) {
            return world_.reject("overlay_rebuild_version_mismatch",
                                 "The persisted overlay stack did not rebuild its recorded version.");
        }
    }
    world_.state_.contentVersion = world_.content_.contentVersion;
    world_.state_.activeOverlays.erase(active);
    const auto event = world_.addEvent(
        "evolution.overlay_rolled_back", "developer", proposalId, "An active content overlay was rolled back",
        Json{{"proposal_id", proposalId}, {"from_version", previousVersion},
             {"to_version", world_.content_.contentVersion},
             {"active_overlay_ids", overlayIds(world_.state_)}});
    world_.rebuildEntityRegistry();
    return CommandResult{true, "ok", world_.content_.contentVersion, event};
}

}  // namespace tide_marks
