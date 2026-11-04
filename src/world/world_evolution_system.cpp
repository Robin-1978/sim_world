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
}  // namespace

ProviderResponse WorldEvolutionSystem::requestProposal(const WorldService& world,
                                                       const std::string& evolutionLevel,
                                                       const std::string& trigger,
                                                       const Json& validationFeedback) {
    // This path is read-only with respect to WorldState. Keeping the world
    // reference const makes the proposal context unable to mutate facts;
    // activation uses the non-const instance method below.
    const auto& state = world.state_;
    const auto& content = world.content_;
    if (evolutionLevel != "level1" && evolutionLevel != "level2") {
        return ProviderResponse{false, {}, {}, "invalid_evolution_level", Json::object()};
    }
    Json events = Json::array();
    const std::size_t first = state.events.size() > 32 ? state.events.size() - 32 : 0;
    for (std::size_t index = first; index < state.events.size(); ++index) {
        const auto& event = state.events[index];
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
    const Json context{{"trigger", trigger}, {"evolution_level", evolutionLevel},
                       {"base_content_version", content.contentVersion},
                       {"base_world_revision", state.revision},
                       {"world_time", state.now.minute}, {"resources", resources},
                       {"npcs", npcs}, {"content", contentSummary},
                       {"allowed_operations", allowedOperations}, {"evidence_events", events},
                       {"validation_feedback", validationFeedback}};
    return world.provider_->proposeEvolution(context, evolutionLevel, validationFeedback);
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
                                 EvolutionProposalStatus::Approved, {}};
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
