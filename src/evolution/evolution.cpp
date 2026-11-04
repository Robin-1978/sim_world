#include "evolution/evolution.hpp"
#include "world/meta_actions.hpp"
#include "world/world_service.hpp"
#include "world/world_state_json.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

namespace tide_marks {
namespace {
std::string requiredString(const Json& value, const char* key) {
    if (!value.contains(key) || !value.at(key).is_string() || value.at(key).get<std::string>().empty()) {
        throw std::runtime_error(std::string("evolution proposal requires ") + key);
    }
    return value.at(key).get<std::string>();
}

BehaviorNodeContent behaviorNodeFromJson(const Json& value) {
    if (!value.is_object()) throw std::runtime_error("evolution behavior node must be an object");
    BehaviorNodeContent node;
    node.type = requiredString(value, "type");
    node.id = requiredString(value, "id");
    node.name = value.value("name", "");
    node.args = value.value("args", Json::object());
    node.maxTicks = value.value("max_ticks", 0);
    for (const auto& child : value.value("children", Json::array())) node.children.push_back(behaviorNodeFromJson(child));
    return node;
}

EvolutionValidation validateBehaviorRecipeReferences(const BehaviorNodeContent& node,
                                                      const WorldContentPack& content) {
    if (node.type == "condition" && !content.conditions.contains(node.name)) {
        return {false, "unknown_condition_reference",
                "Behavior condition '" + node.name + "' is not registered by the active content."};
    }
    if (node.type == "meta_action" && !content.metaActions.contains(node.name)) {
        return {false, "unknown_meta_action_reference",
                "Behavior meta-action '" + node.name + "' is not registered by the active content."};
    }
    for (const auto& child : node.children) {
        const auto result = validateBehaviorRecipeReferences(child, content);
        if (!result.valid) return result;
    }
    return {true, "ok", "Behavior recipe references resolve against the active content."};
}

bool hasNpc(const WorldContentPack& content, const std::string& id) {
    return std::any_of(content.npcs.begin(), content.npcs.end(), [&id](const auto& npc) {
        return npc.definition.id == id;
    });
}

EvolutionValidation applyOperations(WorldContentPack& content, const EvolutionProposal& proposal) {
    for (const auto& operation : proposal.operations) {
        const auto type = operation.at("operation").get<std::string>();
        if (type == "create_entity_archetype") {
            const auto id = requiredString(operation, "id");
            if (std::any_of(content.entityArchetypes.begin(), content.entityArchetypes.end(), [&id](const auto& item) {
                    return item.id == id;
                })) return {false, "duplicate_archetype", "The overlay archetype already exists."};
            content.entityArchetypes.push_back(EntityArchetypeContent{id, requiredString(operation, "kind"),
                                                                       operation.value("default_metadata", Json::object())});
        } else if (type == "overlay_strategy") {
            const auto id = requiredString(operation, "id");
            if (!hasNpc(content, operation.value("npc_id", "")) || !hasNpc(content, operation.value("peer_id", ""))) {
                return {false, "unknown_strategy_actor", "The strategy overlay references an unknown NPC."};
            }
            content.strategyRules.push_back(StrategyRuleContent{
                id, operation.at("npc_id").get<std::string>(), operation.at("peer_id").get<std::string>(),
                operation.value("min_peer_trust", 0.0), requiredString(operation, "strategy"),
                operation.value("goal_stage", 0), operation.value("memory", "An overlay changed my strategy."),
                operation.value("event_type", "strategy.overlay")});
        } else if (type == "register_behavior_recipe") {
            const auto id = requiredString(operation, "id");
            if (std::any_of(content.behaviorRecipes.begin(), content.behaviorRecipes.end(), [&id](const auto& item) {
                    return item.id == id;
                })) return {false, "duplicate_behavior_recipe", "The overlay recipe already exists."};
            BehaviorRecipeContent recipe;
            recipe.id = id;
            recipe.planId = operation.value("plan_id", id);
            recipe.durationMinutes = operation.value("duration_minutes", 0);
            recipe.interruptible = operation.value("interruptible", true);
            recipe.interruptSalienceThreshold = operation.value("interrupt_salience_threshold", 1.0);
            recipe.root = behaviorNodeFromJson(operation.at("root"));
            BehaviorRecipeRuntime::validate(recipe);
            const auto references = validateBehaviorRecipeReferences(recipe.root, content);
            if (!references.valid) return references;
            content.behaviorRecipes.push_back(std::move(recipe));
        } else {
            return {false, "overlay_operation_unsupported", "This operation is not implemented by the declarative overlay engine."};
        }
    }
    return {true, "ok", "Overlay operations applied to the sandbox content view."};
}
}

std::string toString(EvolutionProposalStatus status) {
    switch (status) {
    case EvolutionProposalStatus::Proposed: return "proposed";
    case EvolutionProposalStatus::Validated: return "validated";
    case EvolutionProposalStatus::Approved: return "approved";
    case EvolutionProposalStatus::Rejected: return "rejected";
    }
    return "rejected";
}

EvolutionProposalStatus evolutionProposalStatusFromString(const std::string& value) {
    if (value == "validated") return EvolutionProposalStatus::Validated;
    if (value == "approved") return EvolutionProposalStatus::Approved;
    if (value == "rejected") return EvolutionProposalStatus::Rejected;
    return EvolutionProposalStatus::Proposed;
}

EvolutionProposal EvolutionManager::fromJson(const Json& value) {
    if (value.value("schema_version", 0) != 1) {
        throw std::runtime_error("unsupported evolution proposal schema_version");
    }
    EvolutionProposal proposal;
    proposal.id = requiredString(value, "id");
    proposal.kind = requiredString(value, "kind");
    proposal.source = value.value("source", "unknown");
    proposal.baseContentVersion = requiredString(value, "base_content_version");
    proposal.baseWorldRevision = value.value("base_world_revision", 0ULL);
    proposal.operations = value.value("operations", Json::array());
    proposal.status = evolutionProposalStatusFromString(value.value("status", "proposed"));
    if (!proposal.operations.is_array()) throw std::runtime_error("evolution operations must be an array");
    return proposal;
}

Json EvolutionManager::toJson(const EvolutionProposal& proposal) {
    return Json{{"kind", proposal.kind}, {"schema_version", 1}, {"id", proposal.id}, {"source", proposal.source},
                {"base_content_version", proposal.baseContentVersion},
                {"base_world_revision", proposal.baseWorldRevision},
                {"operations", proposal.operations}, {"status", toString(proposal.status)}};
}

void EvolutionProposalStore::append(const std::string& path, const EvolutionProposal& proposal) {
    std::ofstream output(path, std::ios::app);
    if (!output) throw std::runtime_error("cannot append evolution proposal: " + path);
    output << EvolutionManager::toJson(proposal).dump() << '\n';
}

std::vector<EvolutionProposal> EvolutionProposalStore::load(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read evolution proposal store: " + path);
    std::vector<EvolutionProposal> proposals;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) proposals.push_back(EvolutionManager::fromJson(Json::parse(line)));
    }
    return proposals;
}

EvolutionValidation EvolutionManager::validate(const EvolutionProposal& proposal,
                                               const WorldContentPack& content,
                                               std::uint64_t currentRevision) {
    if (proposal.id.empty() || proposal.kind.empty()) return {false, "invalid_identity", "Proposal id and kind are required."};
    if (proposal.baseContentVersion != content.contentVersion) {
        return {false, "content_version_conflict", "Proposal was created against a different content version."};
    }
    if (proposal.baseWorldRevision != currentRevision) {
        return {false, "revision_conflict", "Proposal must reference the current world revision."};
    }
    if (!proposal.operations.is_array() || proposal.operations.empty()) {
        return {false, "empty_operations", "A proposal must contain at least one operation."};
    }
    static const std::set<std::string> allowed{
        "create_entity_archetype", "register_behavior_recipe", "overlay_strategy"};
    static const std::set<std::string> forbidden{
        "runtime_core_mutation", "load_script", "inject_code", "json_patch",
        "change_event_schema", "change_transaction_rules"};
    std::set<std::string> ids;
    for (const auto& operation : proposal.operations) {
        if (!operation.is_object() || !operation.contains("operation") || !operation.at("operation").is_string()) {
            return {false, "invalid_operation", "Every evolution operation needs a type."};
        }
        const auto type = operation.at("operation").get<std::string>();
        if (forbidden.contains(type)) return {false, "core_mutation_forbidden", "The proposal attempts to mutate Runtime Core."};
        if (!allowed.contains(type)) return {false, "operation_not_allowed", "The operation is outside the declarative capability allowlist."};
        if (!operation.contains("id") || !operation.at("id").is_string() || operation.at("id").get<std::string>().empty()) {
            return {false, "operation_identity_missing", "Every operation needs a stable id."};
        }
        if (!ids.insert(operation.at("id").get<std::string>()).second) {
            return {false, "duplicate_operation", "Operation ids must be unique within a proposal."};
        }
        if (operation.contains("meta_actions")) {
            if (!operation.at("meta_actions").is_array()) return {false, "invalid_meta_actions", "meta_actions must be an array."};
            for (const auto& action : operation.at("meta_actions")) {
                if (!action.is_string() || !content.metaActions.contains(action.get<std::string>())) {
                    return {false, "unimplemented_meta_action", "An operation references an unimplemented meta action."};
                }
            }
        }
        if (type == "create_entity_archetype" && operation.value("kind", "").empty()) {
            return {false, "archetype_kind_missing", "An entity archetype needs a kind."};
        }
        if (type == "register_behavior_recipe") {
            try {
                if (!operation.contains("root") || !operation.at("root").is_object()) {
                    return {false, "behavior_recipe_root_missing", "A behavior recipe needs an object root node."};
                }
                BehaviorRecipeContent recipe;
                recipe.id = operation.at("id").get<std::string>();
                recipe.planId = operation.value("plan_id", recipe.id);
                recipe.root = behaviorNodeFromJson(operation.at("root"));
                BehaviorRecipeRuntime::validate(recipe);
                const auto references = validateBehaviorRecipeReferences(recipe.root, content);
                if (!references.valid) return references;
            } catch (const std::exception& error) {
                return {false, "invalid_behavior_recipe", error.what()};
            }
        }
    }
    return {true, "ok", "Proposal is structurally valid and ready for sandbox approval."};
}

EvolutionValidation EvolutionManager::approve(EvolutionProposal& proposal,
                                               const std::string& approver,
                                               bool sandboxPassed) {
    if (proposal.status != EvolutionProposalStatus::Validated) {
        return {false, "validation_required", "A proposal must pass validation before approval."};
    }
    if (approver != "developer" && approver != "world_designer") {
        proposal.status = EvolutionProposalStatus::Rejected;
        return {false, "approval_forbidden", "Only an independent developer or world designer may approve evolution."};
    }
    if (!sandboxPassed) {
        proposal.status = EvolutionProposalStatus::Rejected;
        return {false, "sandbox_failed", "A proposal cannot activate without a passing sandbox result."};
    }
    proposal.status = EvolutionProposalStatus::Approved;
    return {true, "approved", "Proposal is approved for versioned overlay activation."};
}

EvolutionValidation EvolutionManager::applyOverlay(WorldContentPack& content,
                                                    const EvolutionProposal& proposal) {
    if (proposal.status != EvolutionProposalStatus::Approved) {
        return {false, "approval_required", "Only an approved proposal can activate an overlay."};
    }
    if (proposal.baseContentVersion != content.contentVersion) {
        return {false, "content_version_conflict", "Overlay base content does not match the active content."};
    }
    EvolutionValidation applied;
    try {
        applied = applyOperations(content, proposal);
    } catch (const std::exception& error) {
        return {false, "invalid_overlay_operation", error.what()};
    }
    if (!applied.valid) return applied;
    content.contentVersion = content.contentVersion + "+overlay:" + proposal.id;
    content.version = content.version + "+overlay:" + proposal.id;
    if (content.instance) content.instance->contentVersion = content.contentVersion;
    return {true, "ok", "Approved overlay activated in the content view."};
}

EvolutionSandboxResult EvolutionManager::sandbox(const EvolutionProposal& proposal,
                                                 const WorldService& base,
                                                 std::int64_t advanceMinutes) {
    EvolutionSandboxResult result;
    result.proposalId = proposal.id;
    result.baseContentVersion = base.content().contentVersion;
    result.baseRevision = base.state().revision;
    const auto validation = validate(proposal, base.content(), base.state().revision);
    if (!validation.valid) {
        result.code = validation.code;
        result.message = validation.message;
        return result;
    }
    auto overlay = base.content();
    const auto overlayVersion = overlay.contentVersion + "+overlay:" + proposal.id;
    const auto applied = applyOperations(overlay, proposal);
    if (!applied.valid) {
        result.code = applied.code;
        result.message = applied.message;
        return result;
    }
    overlay.contentVersion = overlayVersion;
    overlay.version = overlay.version + "+overlay:" + proposal.id;
    if (overlay.instance) overlay.instance->contentVersion = overlayVersion;
    auto snapshot = base.snapshotJson();
    snapshot["content_version"] = overlayVersion;
    // The sandbox content view already includes the candidate operation. Do
    // not ask fromContentSnapshot to replay the source world's active stack on
    // top of that view; activation persists the stack separately.
    snapshot["active_overlays"] = Json::array();
    try {
        auto sandboxWorld = WorldService::fromContentSnapshot(overlay, snapshot);
        const auto beforeEvents = sandboxWorld.state().events.size();
        if (advanceMinutes > 0) sandboxWorld.advanceMinutes(advanceMinutes);
        result.finalRevision = sandboxWorld.state().revision;
        result.snapshot = sandboxWorld.snapshotJson();
        result.overlayVersion = overlayVersion;
        result.eventDiff = Json::array();
        for (std::size_t index = beforeEvents; index < sandboxWorld.state().events.size(); ++index) {
            result.eventDiff.push_back(worldEventToJson(sandboxWorld.state().events[index]));
        }
        Json resources = Json::object();
        for (const auto& [id, resource] : sandboxWorld.state().resources) resources[id] = resource.amount;
        std::size_t knowledge = 0;
        std::size_t experiences = 0;
        for (const auto& [id, npc] : sandboxWorld.state().npcs) {
            knowledge += npc.knowledge.size();
            experiences += npc.experiences.size();
        }
        result.metrics = Json{{"events_added", result.eventDiff.size()}, {"final_revision", result.finalRevision},
                              {"resources", resources}, {"npc_count", sandboxWorld.state().npcs.size()},
                              {"knowledge_count", knowledge}, {"experience_count", experiences},
                              {"advance_minutes", advanceMinutes}};
        result.passed = true;
        result.code = "ok";
        result.message = "Sandbox completed without mutating the source world.";
    } catch (const std::exception& error) {
        result.code = "sandbox_exception";
        result.message = error.what();
    }
    return result;
}

}  // namespace tide_marks
