#include "world/world_evolution_system.hpp"

#include "evolution/evolution.hpp"
#include "world/world_service.hpp"

#include <algorithm>

namespace tide_marks {

namespace {
constexpr std::size_t kMaxEvolutionEvidenceEntries = 8;

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
    const std::size_t first = state.events.size() > 32 ? state.events.size() - 32 : 0;
    for (std::size_t index = first; index < state.events.size(); ++index) {
        const auto& event = state.events[index];
        if (!visibleInEvolutionContext(event, evolutionLevel, scopedTarget)) continue;
        events.push_back(Json{{"sequence", event.sequence}, {"at", event.at.minute},
                              {"type", event.type}, {"actor", event.actor}, {"target", event.target},
                              {"detail", event.detail}, {"data", event.data}});
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
        npcs[id] = Json{{"location", npc.location}, {"mode", npc.mode}, {"strategy", npc.strategy},
                        {"goal_stage", npc.goalStage}, {"activity", activity},
                        {"needs", needs}, {"active_process", npc.activity.process.has_value()},
                        {"memory_count", npc.memories.size()}, {"knowledge_count", npc.knowledge.size()},
                        {"experience_count", npc.experiences.size()}, {"knowledge", knowledgeEvidence(npc)},
                        {"experiences", experienceEvidence(npc)}, {"memories", memoryEvidence(npc)},
                        {"relationships", relationshipEvidence(npc)}, {"stimuli", stimulusEvidence(npc)}};
        if (evolutionLevel == "level1" && id == scopedTarget) {
            const auto* authored = world.authoredNpc(id);
            if (authored != nullptr) {
                Json traits = Json::object();
                for (const auto& [trait, value] : authored->definition.privateTraits) traits[trait] = value;
                npcs[id]["private_traits"] = std::move(traits);
            }
        }
    }
    Json allowedOperations = evolutionLevel == "level1"
                                 ? Json::array({"overlay_strategy", "overlay_schedule", "register_behavior_recipe"})
                                 : Json::array({"create_entity_archetype", "register_behavior_recipe"});
    Json contentSummary{
        {"content_id", content.id}, {"content_version", content.contentVersion},
        {"entity_archetypes", content.entityArchetypes.size()},
        {"behavior_recipes", content.behaviorRecipes.size()},
        {"strategy_rules", content.strategyRules.size()},
        {"registered_meta_actions", Json::array()}, {"registered_conditions", Json::array()}};
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
                {"base_content_version", content.contentVersion},
                {"evolution_system_prompt", evolutionLevel == "level1"
                                                ? content.npcEvolutionPrompt
                                                : content.worldEvolutionPrompt},
                {"evolution_prompt_version", content.contentVersion + ":" +
                                                  (evolutionLevel == "level1" ? "npc_strategy" : "world_substrate")},
                {"base_world_revision", state.revision},
                {"world_time", state.now.minute}, {"resources", resources},
                {"npcs", npcs}, {"content", contentSummary},
                {"allowed_operations", allowedOperations}, {"evidence_events", events},
                {"validation_feedback", validationFeedback}};
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
    if (!world_.evolution_ || world_.provider_->name() == "mock" ||
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
        const std::string trigger = "day_boundary:" + std::to_string(day);
        const auto context = proposalContext(world_, level, trigger, Json::object(), targetNpcId);
        if (context.empty()) return;
        EvolutionRequest request;
        request.requestId = "evolution:auto:" + level + ":" + std::to_string(day) + ":" +
                            std::to_string(world_.state_.revision) + ":" + targetNpcId + ":attempt:0";
        request.key = key;
        request.evolutionLevel = level;
        request.targetNpcId = targetNpcId;
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
            data["provider"] = queued.provider;
            world_.addEvent(type, kWorldActor, queued.request.evolutionLevel, detail, std::move(data));
        };
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
                if (queued.request.attempt < 2 && queued.provider == "llama.cpp") {
                    const Json feedback{{"attempt", queued.request.attempt + 1}, {"code", validation.code},
                                        {"message", validation.message},
                                        {"previous_proposal", EvolutionManager::toJson(proposal)}};
                    auto repair = queued.request;
                    ++repair.attempt;
                    repair.requestId += ":repair:" + std::to_string(repair.attempt);
                    repair.validationFeedback = feedback;
                    repair.context["validation_feedback"] = feedback;
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
            if (queued.request.attempt < 2 && queued.provider == "llama.cpp") {
                const Json feedback{{"attempt", queued.request.attempt + 1},
                                    {"code", "invalid_evolution_proposal"},
                                    {"message", error.what()},
                                    {"previous_proposal", queued.response.evolutionProposal}};
                auto repair = queued.request;
                ++repair.attempt;
                repair.requestId += ":repair:" + std::to_string(repair.attempt);
                repair.validationFeedback = feedback;
                repair.context["validation_feedback"] = feedback;
                if (world_.evolution_->submit(std::move(repair)).accepted) continue;
            }
            audit("evolution.proposal_rejected", "The Runtime could not parse or sandbox an automatic evolution proposal",
                  Json{{"error", error.what()}});
            finish();
        }
    }
    return consumed;
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
