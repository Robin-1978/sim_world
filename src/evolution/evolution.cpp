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

std::vector<std::uint64_t> requiredEventSequences(const Json& value, const char* key) {
    if (!value.contains(key) || !value.at(key).is_array() || value.at(key).empty()) {
        throw std::runtime_error(std::string("evolution proposal requires non-empty ") + key);
    }
    std::vector<std::uint64_t> sequences;
    for (const auto& item : value.at(key)) {
        if (!item.is_number_unsigned() || item.get<std::uint64_t>() == 0) {
            throw std::runtime_error(std::string("evolution proposal ") + key + " must contain event sequences");
        }
        sequences.push_back(item.get<std::uint64_t>());
    }
    return sequences;
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

EvolutionValidation validateParameterSchema(const Json& schema) {
    if (!schema.is_object()) {
        return {false, "invalid_behavior_parameters", "Behavior recipe parameters must be a JSON Schema object."};
    }
    if (schema.empty()) return {true, "ok", "The behavior recipe declares no decision parameters."};
    if (schema.value("type", "") != "object") {
        return {false, "invalid_behavior_parameters", "Behavior recipe parameters must declare type=object."};
    }
    if (schema.contains("properties") && !schema.at("properties").is_object()) {
        return {false, "invalid_behavior_parameters", "Behavior recipe parameter properties must be an object."};
    }
    if (schema.contains("required") && !schema.at("required").is_array()) {
        return {false, "invalid_behavior_parameters", "Behavior recipe parameter required must be an array."};
    }
    if (schema.contains("additionalProperties") && !schema.at("additionalProperties").is_boolean()) {
        return {false, "invalid_behavior_parameters", "Behavior recipe additionalProperties must be boolean."};
    }
    static const std::set<std::string> fields{"type", "properties", "required", "additionalProperties", "title", "description"};
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (!fields.contains(it.key())) {
            return {false, "invalid_behavior_parameters", "Unknown JSON Schema keyword in behavior parameters: " + it.key()};
        }
    }
    return {true, "ok", "Behavior recipe parameter schema is bounded."};
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
        } else if (type == "overlay_schedule") {
            const auto npcId = requiredString(operation, "npc_id");
            if (!hasNpc(content, npcId)) {
                return {false, "unknown_schedule_actor", "The schedule overlay references an unknown NPC."};
            }
            const auto slots = operation.value("slots", Json::array());
            if (!slots.is_array() || slots.empty()) {
                return {false, "schedule_slots_missing", "A schedule overlay needs at least one slot."};
            }
            auto npc = std::find_if(content.npcs.begin(), content.npcs.end(),
                                    [&npcId](const auto& item) { return item.definition.id == npcId; });
            std::vector<ScheduleSlot> replacement;
            for (const auto& slot : slots) {
                if (!slot.is_object() || !slot.contains("start_minute") || !slot.contains("end_minute") ||
                    !slot.at("start_minute").is_number_integer() || !slot.at("end_minute").is_number_integer() ||
                    !slot.contains("location") || !slot.at("location").is_string()) {
                    return {false, "invalid_schedule_slot", "Every schedule slot needs integer bounds and a location."};
                }
                const int start = slot.at("start_minute").get<int>();
                const int end = slot.at("end_minute").get<int>();
                const auto location = slot.at("location").get<std::string>();
                if (start < 0 || start >= static_cast<int>(kMinutesPerDay) ||
                    end < 0 || end >= static_cast<int>(kMinutesPerDay) || location.empty() ||
                    std::none_of(content.spatialEntities.begin(), content.spatialEntities.end(),
                                 [&location](const auto& item) { return item.id == location; })) {
                    return {false, "invalid_schedule_slot", "A schedule slot has an invalid time or location."};
                }
                replacement.push_back(ScheduleSlot{start, end, location});
            }
            npc->schedule = std::move(replacement);
        } else if (type == "register_behavior_recipe") {
            const auto id = requiredString(operation, "id");
            if (std::any_of(content.behaviorRecipes.begin(), content.behaviorRecipes.end(), [&id](const auto& item) {
                    return item.id == id;
                })) return {false, "duplicate_behavior_recipe", "The overlay recipe already exists."};
            BehaviorRecipeContent recipe;
            recipe.id = id;
            recipe.planId = operation.value("plan_id", id);
            recipe.parameterSchema = operation.value("parameters", Json::object());
            const auto parameterValidation = validateParameterSchema(recipe.parameterSchema);
            if (!parameterValidation.valid) return parameterValidation;
            const auto actors = operation.value("available_to", Json::array());
            if (!actors.is_array() || actors.empty()) {
                return {false, "behavior_recipe_actors_missing",
                        "An evolved behavior recipe must declare a non-empty available_to actor scope."};
            }
            for (const auto& actor : actors) {
                if (!actor.is_string() || actor.get<std::string>().empty() ||
                    !hasNpc(content, actor.get<std::string>())) {
                    return {false, "unknown_behavior_recipe_actor",
                            "An evolved behavior recipe references an unknown available_to NPC."};
                }
                recipe.allowedActors.push_back(actor.get<std::string>());
            }
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
    if (!value.contains("base_world_revision") || !value.at("base_world_revision").is_number_unsigned()) {
        throw std::runtime_error("evolution proposal requires an unsigned base_world_revision");
    }
    if (!value.contains("status") || !value.at("status").is_string()) {
        throw std::runtime_error("evolution proposal requires a status");
    }
    if (!value.contains("operations") || !value.at("operations").is_array() || value.at("operations").empty()) {
        throw std::runtime_error("evolution proposal requires non-empty operations");
    }
    EvolutionProposal proposal;
    proposal.id = requiredString(value, "id");
    proposal.kind = requiredString(value, "kind");
    proposal.source = requiredString(value, "source");
    proposal.baseContentVersion = requiredString(value, "base_content_version");
    proposal.baseWorldRevision = value.at("base_world_revision").get<std::uint64_t>();
    proposal.operations = value.at("operations");
    proposal.evidenceEventSequences = requiredEventSequences(value, "evidence_event_sequences");
    const auto status = value.at("status").get<std::string>();
    if (status != "proposed" && status != "validated" && status != "approved" && status != "rejected") {
        throw std::runtime_error("unsupported evolution proposal status");
    }
    proposal.status = evolutionProposalStatusFromString(status);
    proposal.evolutionLevel = value.value("evolution_level", "level2");
    if (proposal.evolutionLevel != "level1" && proposal.evolutionLevel != "level2") {
        throw std::runtime_error("unsupported evolution proposal evolution_level");
    }
    return proposal;
}

Json EvolutionManager::toJson(const EvolutionProposal& proposal) {
    Json evidence = Json::array();
    for (const auto sequence : proposal.evidenceEventSequences) evidence.push_back(sequence);
    return Json{{"kind", proposal.kind}, {"schema_version", 1}, {"id", proposal.id}, {"source", proposal.source},
                {"base_content_version", proposal.baseContentVersion},
                {"base_world_revision", proposal.baseWorldRevision},
                {"evidence_event_sequences", evidence}, {"operations", proposal.operations},
                {"status", toString(proposal.status)}, {"evolution_level", proposal.evolutionLevel}};
}

EvolutionProposalRequestResult EvolutionManager::propose(const WorldService& world,
                                                          const std::string& evolutionLevel,
                                                          const std::string& trigger) {
    EvolutionProposalRequestResult result;
    result.provider = world.providerName();
    Json feedback = Json::object();
    // A malformed/unsupported proposal may be repaired twice using the same
    // captured world context. Keeping this bound prevents an LLM validation
    // loop from becoming the simulation scheduler while allowing a generated
    // behavior tree to fix both its root and nested-node errors.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto response = world.requestEvolutionProposal(evolutionLevel, trigger, feedback);
        if (!response.ok) {
            result.code = response.error.empty() ? "evolution_provider_unavailable" : response.error;
            result.message = "The configured LLM did not return an evolution proposal.";
            return result;
        }
        try {
            if (!response.evolutionProposal.is_object()) {
                result.code = "invalid_evolution_proposal";
                result.message = "The provider returned a non-object evolution proposal.";
                return result;
            }
            Json normalized = response.evolutionProposal;
        normalized["schema_version"] = 1;
        static const std::set<std::string> proposalKinds{
            "content", "knowledge", "strategy", "declarative_capability"};
        // The tool schema calls the proposal category `proposal_kind` so it
        // cannot be confused with create_entity_archetype.kind.  Accept an
        // older provider payload as an adapter, but derive the category only
        // from the registered operation when the payload is not a governed
        // proposal kind; never treat an entity noun such as "facility" as a
        // new governance category.
        const auto operationKind = [&normalized]() {
            if (!normalized.contains("operations") || !normalized.at("operations").is_array() ||
                normalized.at("operations").empty() || !normalized.at("operations").at(0).is_object()) {
                return std::string{};
            }
            return normalized.at("operations").at(0).value("operation", "");
        }();
        std::string proposalKind = normalized.value("proposal_kind", "");
        if (!proposalKinds.contains(proposalKind)) proposalKind = normalized.value("kind", "");
        if (!proposalKinds.contains(proposalKind)) {
            proposalKind = (operationKind == "overlay_strategy" || operationKind == "overlay_schedule") ? "strategy"
                          : operationKind == "register_behavior_recipe" ? "declarative_capability"
                                                                          : "content";
        }
        normalized["kind"] = proposalKind;
        normalized["id"] = normalized.value("id", "llm-evolution-" + std::to_string(world.state().revision));
        normalized["source"] = "llm:" + result.provider;
        normalized["base_content_version"] = world.content().contentVersion;
        normalized["base_world_revision"] = world.state().revision;
        normalized["status"] = "proposed";
        normalized["evolution_level"] = evolutionLevel;
            const auto proposal = EvolutionManager::fromJson(normalized);
            const auto validation = EvolutionManager::validate(proposal, world.content(), world.state());
            if (!validation.valid) {
                if (attempt == 0) {
                    feedback = Json{{"attempt", attempt + 1}, {"code", validation.code},
                                    {"message", validation.message}, {"previous_proposal", normalized}};
                    continue;
                }
                result.code = validation.code;
                result.message = validation.message;
                return result;
            }
            result.accepted = true;
            result.code = "ok";
            result.message = attempt == 0
                                 ? "The LLM produced a validated proposal; sandbox and independent approval are still required."
                                 : "The LLM repaired its proposal after Runtime validation feedback; sandbox and independent approval are still required.";
            result.proposal = proposal;
            return result;
        } catch (const std::exception& error) {
            if (attempt == 0) {
                feedback = Json{{"attempt", attempt + 1}, {"code", "invalid_evolution_proposal"},
                                {"message", error.what()}, {"previous_proposal", response.evolutionProposal}};
                continue;
            }
            result.code = "invalid_evolution_proposal";
            result.message = error.what();
            return result;
        }
    }
    result.code = "invalid_evolution_proposal";
    result.message = "The provider did not produce a valid proposal after bounded repair.";
    return result;
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
                                               const WorldState& state) {
    if (proposal.id.empty() || proposal.source.empty()) {
        return {false, "invalid_identity", "Proposal id and source are required."};
    }
    static const std::set<std::string> kinds{
        "content", "knowledge", "strategy", "declarative_capability"};
    if (!kinds.contains(proposal.kind)) {
        return {false, "invalid_proposal_kind", "Proposal kind is outside the governed evolution registry."};
    }
    if (proposal.evidenceEventSequences.empty()) {
        return {false, "evidence_required", "An evolution proposal must cite committed world events."};
    }
    std::set<std::uint64_t> evidence;
    for (const auto sequence : proposal.evidenceEventSequences) {
        if (sequence == 0 || !evidence.insert(sequence).second) {
            return {false, "invalid_evidence", "Evidence event sequences must be positive and unique."};
        }
        if (std::none_of(state.events.begin(), state.events.end(), [sequence](const auto& event) {
                return event.sequence == sequence;
            })) {
            return {false, "unknown_evidence_event",
                    "Evolution evidence must cite committed events from the source world."};
        }
    }
    if (proposal.baseContentVersion != content.contentVersion) {
        return {false, "content_version_conflict", "Proposal was created against a different content version."};
    }
    if (proposal.baseWorldRevision != state.revision) {
        return {false, "revision_conflict", "Proposal must reference the current world revision."};
    }
    if (!proposal.operations.is_array() || proposal.operations.empty()) {
        return {false, "empty_operations", "A proposal must contain at least one operation."};
    }
    static const std::set<std::string> allowed{
        "create_entity_archetype", "register_behavior_recipe", "overlay_strategy", "overlay_schedule"};
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
        if (proposal.evolutionLevel == "level1" && type == "create_entity_archetype") {
            return {false, "level_operation_mismatch", "Level 1 may change an NPC strategy or behavior capability, not world content."};
        }
        if (proposal.evolutionLevel == "level1" && type == "overlay_schedule") {
            // A schedule is an individual strategy/availability overlay. It
            // is intentionally kept out of Level 2's world-foundation lane.
        }
        if (proposal.evolutionLevel == "level2" && type == "overlay_schedule") {
            return {false, "level_operation_mismatch", "Level 2 may change world content or behavior capability, not one NPC schedule."};
        }
        if (proposal.evolutionLevel == "level2" && type == "overlay_strategy") {
            return {false, "level_operation_mismatch", "Level 2 may change world content or behavior capability, not one NPC strategy."};
        }
        if (!operation.contains("id") || !operation.at("id").is_string() || operation.at("id").get<std::string>().empty()) {
            return {false, "operation_identity_missing", "Every operation needs a stable id."};
        }
        if (!ids.insert(operation.at("id").get<std::string>()).second) {
            return {false, "duplicate_operation", "Operation ids must be unique within a proposal."};
        }
        const std::set<std::string> commonFields{"operation", "id"};
        std::set<std::string> fields = commonFields;
        if (type == "create_entity_archetype") {
            fields.insert("kind");
            fields.insert("default_metadata");
        } else if (type == "overlay_strategy") {
            for (const auto& field : {"npc_id", "peer_id", "min_peer_trust", "strategy", "goal_stage",
                                      "memory", "event_type"}) fields.insert(field);
        } else if (type == "overlay_schedule") {
            fields.insert("npc_id");
            fields.insert("slots");
        } else if (type == "register_behavior_recipe") {
            for (const auto& field : {"plan_id", "duration_minutes", "interruptible",
                                      "interrupt_salience_threshold", "reservation_mode", "priority",
                                      "preemption_policy", "parameters", "available_to", "root"}) fields.insert(field);
        }
        for (auto item = operation.begin(); item != operation.end(); ++item) {
            if (!fields.contains(item.key())) {
                return {false, "unknown_operation_field", "Operation field is not allowed for this operation: " + item.key()};
            }
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
        if (type == "overlay_strategy") {
            for (const auto& field : {"npc_id", "peer_id", "strategy"}) {
                if (!operation.contains(field) || !operation.at(field).is_string() || operation.at(field).get<std::string>().empty()) {
                    return {false, "strategy_field_missing", "A strategy overlay needs " + std::string(field) + "."};
                }
            }
        }
        if (type == "overlay_schedule") {
            if (!operation.contains("npc_id") || !operation.at("npc_id").is_string() ||
                operation.at("npc_id").get<std::string>().empty()) {
                return {false, "schedule_actor_missing", "A schedule overlay needs npc_id."};
            }
            if (!operation.contains("slots") || !operation.at("slots").is_array() || operation.at("slots").empty()) {
                return {false, "schedule_slots_missing", "A schedule overlay needs a non-empty slots array."};
            }
        }
        if (type == "register_behavior_recipe") {
            try {
                if (!operation.contains("root") || !operation.at("root").is_object()) {
                    return {false, "behavior_recipe_root_missing", "A behavior recipe needs an object root node."};
                }
                BehaviorRecipeContent recipe;
                recipe.id = operation.at("id").get<std::string>();
                recipe.planId = operation.value("plan_id", recipe.id);
                recipe.parameterSchema = operation.value("parameters", Json::object());
                const auto parameterValidation = validateParameterSchema(recipe.parameterSchema);
                if (!parameterValidation.valid) return parameterValidation;
                const auto actors = operation.value("available_to", Json::array());
                if (!actors.is_array() || actors.empty()) {
                    return {false, "behavior_recipe_actors_missing",
                            "An evolved behavior recipe must declare a non-empty available_to actor scope."};
                }
                for (const auto& actor : actors) {
                    if (!actor.is_string() || actor.get<std::string>().empty() ||
                        !hasNpc(content, actor.get<std::string>())) {
                        return {false, "unknown_behavior_recipe_actor",
                                "An evolved behavior recipe references an unknown available_to NPC."};
                    }
                }
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
    auto candidate = content;
    EvolutionValidation applied;
    try {
        applied = applyOperations(candidate, proposal);
    } catch (const std::exception& error) {
        return {false, "invalid_overlay_operation", error.what()};
    }
    if (!applied.valid) return applied;
    candidate.contentVersion = candidate.contentVersion + "+overlay:" + proposal.id;
    candidate.version = candidate.version + "+overlay:" + proposal.id;
    if (candidate.instance) candidate.instance->contentVersion = candidate.contentVersion;
    content = std::move(candidate);
    return {true, "ok", "Approved overlay activated in the content view."};
}

EvolutionSandboxResult EvolutionManager::sandbox(const EvolutionProposal& proposal,
                                                 const WorldService& base,
                                                 std::int64_t advanceMinutes) {
    EvolutionSandboxResult result;
    result.proposalId = proposal.id;
    result.baseContentVersion = base.content().contentVersion;
    result.baseRevision = base.state().revision;
    const auto validation = validate(proposal, base.content(), base.state());
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
        // Evolution sandbox evaluation is a deterministic replay of the
        // captured context; it must not create new LLM decisions.
        auto sandboxWorld = WorldService::fromContentSnapshot(
            overlay, snapshot, std::make_unique<MockProvider>());
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
