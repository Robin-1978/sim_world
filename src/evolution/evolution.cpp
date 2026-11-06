#include "evolution/evolution.hpp"
#include "world/meta_actions.hpp"
#include "world/world_service.hpp"
#include "world/world_state_json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
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
        std::uint64_t sequence = 0;
        if (item.is_number_unsigned()) {
            sequence = item.get<std::uint64_t>();
        } else if (item.is_number_integer()) {
            const auto signedValue = item.get<std::int64_t>();
            if (signedValue > 0) sequence = static_cast<std::uint64_t>(signedValue);
        } else if (item.is_number_float()) {
            const auto floating = item.get<double>();
            if (std::isfinite(floating) && floating > 0.0 && std::floor(floating) == floating &&
                floating <= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
                sequence = static_cast<std::uint64_t>(floating);
            }
        }
        if (sequence == 0) {
            throw std::runtime_error(std::string("evolution proposal ") + key + " must contain event sequences");
        }
        sequences.push_back(sequence);
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
    // This is deliberately a small, closed subset of JSON Schema.  The
    // provider may describe data needed by a recipe, but it cannot smuggle a
    // general schema interpreter or executable expression into an overlay.
    const auto validateNode = [&schema](const auto& self, const Json& node, bool root,
                                        const std::string& path) -> EvolutionValidation {
        if (!node.is_object()) {
            return {false, "invalid_behavior_parameters",
                    "Behavior recipe parameter schema node must be an object at " + path + "."};
        }
        if (root && !node.empty() && node.value("type", "") != "object") {
            return {false, "invalid_behavior_parameters",
                    "Behavior recipe parameters must declare type=object."};
        }
        static const std::set<std::string> fields{
            "type", "properties", "required", "additionalProperties", "title", "description",
            "items", "enum", "const", "minimum", "maximum", "minLength", "maxLength",
            "minItems", "maxItems", "uniqueItems"};
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (!fields.contains(it.key())) {
                return {false, "invalid_behavior_parameters",
                        "Unknown JSON Schema keyword in behavior parameters: " + it.key()};
            }
        }
        if (node.contains("type") && !node.at("type").is_string()) {
            return {false, "invalid_behavior_parameters", "Behavior parameter schema type must be a string at " + path + "."};
        }
        const auto type = node.value("type", "");
        static const std::set<std::string> types{"object", "array", "string", "integer", "number", "boolean"};
        if (!type.empty() && !types.contains(type)) {
            return {false, "invalid_behavior_parameters", "Unsupported behavior parameter type at " + path + "."};
        }
        if (node.contains("properties")) {
            if (!node.at("properties").is_object()) {
                return {false, "invalid_behavior_parameters",
                        "Behavior recipe parameter properties must be an object at " + path + "."};
            }
            for (auto it = node.at("properties").begin(); it != node.at("properties").end(); ++it) {
                const auto result = self(self, it.value(), false, path + ".properties." + it.key());
                if (!result.valid) return result;
            }
        }
        if (node.contains("required")) {
            if (!node.at("required").is_array()) {
                return {false, "invalid_behavior_parameters", "Behavior parameter required must be an array at " + path + "."};
            }
            std::set<std::string> names;
            for (const auto& item : node.at("required")) {
                if (!item.is_string() || item.get<std::string>().empty() ||
                    !names.insert(item.get<std::string>()).second) {
                    return {false, "invalid_behavior_parameters",
                            "Behavior parameter required must contain unique non-empty names at " + path + "."};
                }
            }
        }
        if (node.contains("additionalProperties") && !node.at("additionalProperties").is_boolean()) {
            return {false, "invalid_behavior_parameters",
                    "Behavior recipe additionalProperties must be boolean at " + path + "."};
        }
        if (node.contains("items")) {
            if (type != "array") {
                return {false, "invalid_behavior_parameters",
                        "Behavior parameter items is only valid for an array at " + path + "."};
            }
            const auto result = self(self, node.at("items"), false, path + ".items");
            if (!result.valid) return result;
        }
        if (node.contains("uniqueItems") && !node.at("uniqueItems").is_boolean()) {
            return {false, "invalid_behavior_parameters",
                    "Behavior parameter uniqueItems must be boolean at " + path + "."};
        }
        if (node.contains("uniqueItems") && type != "array") {
            return {false, "invalid_behavior_parameters",
                    "Behavior parameter uniqueItems is only valid for an array at " + path + "."};
        }
        for (const auto* key : {"minimum", "maximum"}) {
            if (node.contains(key) && !node.at(key).is_number()) {
                return {false, "invalid_behavior_parameters",
                        std::string("Behavior parameter ") + key + " must be numeric at " + path + "."};
            }
        }
        for (const auto* key : {"minLength", "maxLength", "minItems", "maxItems"}) {
            if (node.contains(key) &&
                (!node.at(key).is_number_integer() || node.at(key).template get<std::int64_t>() < 0 ||
                 static_cast<std::uint64_t>(node.at(key).template get<std::int64_t>()) >
                     static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))) {
                return {false, "invalid_behavior_parameters",
                        std::string("Behavior parameter ") + key + " must be a non-negative integer at " + path + "."};
            }
        }
        if (node.contains("enum") && !node.at("enum").is_array()) {
            return {false, "invalid_behavior_parameters", "Behavior parameter enum must be an array at " + path + "."};
        }
        return {true, "ok", "Behavior recipe parameter schema is bounded."};
    };

    if (!schema.is_object()) {
        return {false, "invalid_behavior_parameters", "Behavior recipe parameters must be a JSON Schema object."};
    }
    if (schema.empty()) return {true, "ok", "The behavior recipe declares no decision parameters."};
    return validateNode(validateNode, schema, true, "parameters");
}

EvolutionValidation validateInnovationSpec(const EvolutionProposal& proposal) {
    const bool supplied = proposal.innovation.is_object() && !proposal.innovation.empty();
    if (proposal.kind != "innovation") {
        if (supplied) {
            return {false, "innovation_kind_mismatch",
                    "An innovation envelope is only valid for a proposal with kind=innovation."};
        }
        return {true, "ok", "No innovation envelope is attached to this proposal."};
    }
    if (proposal.evolutionLevel != "level1" && proposal.evolutionLevel != "level2") {
        return {false, "innovation_level_mismatch", "Innovation proposals must use a governed evolution level."};
    }
    if (!supplied) {
        return {false, "innovation_spec_missing",
                "An innovation proposal must include a bounded hypothesis and evaluation plan."};
    }
    static const std::set<std::string> fields{
        "hypothesis", "novelty_basis", "operation_ids", "expected_benefits", "risks", "test_plan"};
    for (auto item = proposal.innovation.begin(); item != proposal.innovation.end(); ++item) {
        if (!fields.contains(item.key())) {
            return {false, "innovation_field_unknown",
                    "Innovation metadata contains an unsupported field: " + item.key()};
        }
    }
    const auto requiredText = [&proposal](const char* key, std::size_t maxLength) -> EvolutionValidation {
        if (!proposal.innovation.contains(key) || !proposal.innovation.at(key).is_string() ||
            proposal.innovation.at(key).get<std::string>().empty() ||
            proposal.innovation.at(key).get<std::string>().size() > maxLength) {
            return {false, "innovation_field_invalid",
                    std::string("Innovation ") + key + " must be a bounded non-empty string."};
        }
        return {true, "ok", "Innovation text is bounded."};
    };
    for (const auto* key : {"hypothesis", "novelty_basis"}) {
        const auto result = requiredText(key, std::string(key) == "hypothesis" ? 1024 : 64);
        if (!result.valid) return result;
    }
    static const std::set<std::string> noveltyBases{
        "new_combination", "new_capability", "new_archetype"};
    if (!noveltyBases.contains(proposal.innovation.at("novelty_basis").get<std::string>())) {
        return {false, "innovation_novelty_basis_invalid",
                "Innovation novelty_basis must describe a bounded declarative novelty claim."};
    }
    if (proposal.evolutionLevel == "level1" &&
        proposal.innovation.at("novelty_basis").get<std::string>() == "new_archetype") {
        return {false, "innovation_level_mismatch",
                "A Level 1 reflection cannot create a shared world archetype."};
    }
    const auto boundedStringArray = [&proposal](const char* key, std::size_t maxItems,
                                                 std::size_t maxLength) -> EvolutionValidation {
        if (!proposal.innovation.contains(key) || !proposal.innovation.at(key).is_array() ||
            proposal.innovation.at(key).empty() || proposal.innovation.at(key).size() > maxItems) {
            return {false, "innovation_field_invalid",
                    std::string("Innovation ") + key + " must contain a bounded non-empty list."};
        }
        for (const auto& item : proposal.innovation.at(key)) {
            if (!item.is_string() || item.get<std::string>().empty() || item.get<std::string>().size() > maxLength) {
                return {false, "innovation_field_invalid",
                        std::string("Innovation ") + key + " entries must be bounded strings."};
            }
        }
        return {true, "ok", "Innovation list is bounded."};
    };
    for (const auto* key : {"expected_benefits", "risks"}) {
        const auto result = boundedStringArray(key, 4, 512);
        if (!result.valid) return result;
    }
    if (!proposal.innovation.contains("operation_ids") ||
        !proposal.innovation.at("operation_ids").is_array() ||
        proposal.innovation.at("operation_ids").empty() ||
        proposal.innovation.at("operation_ids").size() > proposal.operations.size()) {
        return {false, "innovation_operation_scope_invalid",
                "Innovation operation_ids must identify one or more proposed operations."};
    }
    std::set<std::string> operationIds;
    for (const auto& item : proposal.innovation.at("operation_ids")) {
        if (!item.is_string() || item.get<std::string>().empty() ||
            !operationIds.insert(item.get<std::string>()).second) {
            return {false, "innovation_operation_scope_invalid",
                    "Innovation operation_ids must contain unique non-empty operation ids."};
        }
    }
    for (const auto& operation : proposal.operations) {
        if (!operation.is_object() || !operation.contains("id") ||
            !operationIds.contains(operation.at("id").get<std::string>())) {
            return {false, "innovation_operation_scope_invalid",
                    "Innovation operation_ids must cover every operation in the proposal."};
        }
    }
    if (operationIds.size() != proposal.operations.size()) {
        return {false, "innovation_operation_scope_invalid",
                "Innovation operation_ids cannot reference operations outside the proposal."};
    }
    if (proposal.evolutionLevel == "level1") {
        if (proposal.targetNpcId.empty()) {
            return {false, "innovation_target_missing",
                    "A Level 1 innovation hypothesis must identify its reflecting NPC."};
        }
        for (const auto& operation : proposal.operations) {
            if (operation.value("operation", "") != "register_behavior_recipe") {
                return {false, "innovation_level_mismatch",
                        "A Level 1 innovation hypothesis may only prepare a private behavior recipe."};
            }
            const auto actors = operation.value("available_to", Json::array());
            if (!actors.is_array() || actors.size() != 1 || !actors.at(0).is_string() ||
                actors.at(0).get<std::string>() != proposal.targetNpcId) {
                return {false, "innovation_target_mismatch",
                        "A Level 1 innovation recipe must remain private to its target NPC."};
            }
        }
    }
    if (!proposal.innovation.contains("test_plan") || !proposal.innovation.at("test_plan").is_object()) {
        return {false, "innovation_test_plan_missing", "An innovation must declare a bounded test_plan."};
    }
    const auto& testPlan = proposal.innovation.at("test_plan");
    static const std::set<std::string> testFields{"horizon_minutes", "metrics", "baseline", "scenario",
                                                   "expected_metrics", "constraints", "resource_budget",
                                                   "success_criteria", "failure_criteria"};
    for (auto item = testPlan.begin(); item != testPlan.end(); ++item) {
        if (!testFields.contains(item.key())) {
            return {false, "innovation_test_plan_invalid",
                    "Innovation test_plan contains an unsupported field: " + item.key()};
        }
    }
    if (!testPlan.contains("horizon_minutes") || !testPlan.at("horizon_minutes").is_number_integer() ||
        testPlan.at("horizon_minutes").get<int>() <= 0 || testPlan.at("horizon_minutes").get<int>() > 10080) {
        return {false, "innovation_test_plan_invalid",
                "Innovation test_plan.horizon_minutes must be between 1 and 10080."};
    }
    static const std::set<std::string> metricIds{
        "events_added", "final_revision", "npc_count", "knowledge_count", "experience_count",
        "advance_minutes", "resource_total"};
    if (!testPlan.contains("metrics") || !testPlan.at("metrics").is_array() ||
        testPlan.at("metrics").empty() || testPlan.at("metrics").size() > 6) {
        return {false, "innovation_test_plan_invalid",
                "Innovation test_plan.metrics must contain one to six registered metrics."};
    }
    std::set<std::string> metrics;
    for (const auto& metric : testPlan.at("metrics")) {
        if (!metric.is_string() || !metricIds.contains(metric.get<std::string>()) ||
            !metrics.insert(metric.get<std::string>()).second) {
            return {false, "innovation_test_plan_invalid",
                    "Innovation test_plan.metrics must use unique registered metric ids."};
        }
    }
    const auto requiredPlanText = [&testPlan](const char* key) -> EvolutionValidation {
        if (!testPlan.contains(key) || !testPlan.at(key).is_string() ||
            testPlan.at(key).get<std::string>().empty() || testPlan.at(key).get<std::string>().size() > 512) {
            return {false, "innovation_test_plan_invalid",
                    std::string("Innovation test_plan.") + key + " must be a bounded non-empty string."};
        }
        return {true, "ok", "Innovation test criterion is bounded."};
    };
    for (const auto* key : {"baseline", "scenario", "success_criteria", "failure_criteria"}) {
        const auto result = requiredPlanText(key);
        if (!result.valid) return result;
    }
    if (!testPlan.contains("expected_metrics") || !testPlan.at("expected_metrics").is_object() ||
        testPlan.at("expected_metrics").empty() || testPlan.at("expected_metrics").size() > 6) {
        return {false, "innovation_test_plan_invalid", "Innovation test_plan.expected_metrics must map registered metrics to numeric targets."};
    }
    for (auto item = testPlan.at("expected_metrics").begin(); item != testPlan.at("expected_metrics").end(); ++item) {
        if (!metrics.contains(item.key()) || !item.value().is_number()) {
            return {false, "innovation_test_plan_invalid", "Innovation expected_metrics must use selected registered metric ids and numeric targets."};
        }
    }
    const auto constraints = testPlan.value("constraints", Json::array());
    if (!constraints.is_array() || constraints.empty() || constraints.size() > 6) {
        return {false, "innovation_test_plan_invalid", "Innovation test_plan.constraints must be a bounded non-empty list."};
    }
    for (const auto& item : constraints) {
        if (!item.is_string() || item.get<std::string>().empty() || item.get<std::string>().size() > 512) {
            return {false, "innovation_test_plan_invalid", "Innovation test constraints must be bounded strings."};
        }
    }
    if (!testPlan.contains("resource_budget") || !testPlan.at("resource_budget").is_object() ||
        testPlan.at("resource_budget").empty() || testPlan.at("resource_budget").size() > 8) {
        return {false, "innovation_test_plan_invalid", "Innovation test_plan.resource_budget must be a bounded resource map."};
    }
    for (auto item = testPlan.at("resource_budget").begin(); item != testPlan.at("resource_budget").end(); ++item) {
        if (!item.value().is_number() || item.value().get<double>() < 0.0 || item.value().get<double>() > 1000000.0) {
            return {false, "innovation_test_plan_invalid", "Innovation resource budgets must be bounded non-negative numbers."};
        }
    }
    return {true, "ok", "Innovation hypothesis and evaluation plan are bounded."};
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
            std::set<std::string> actorIds;
            for (const auto& actor : actors) {
                if (!actor.is_string() || actor.get<std::string>().empty() ||
                    !hasNpc(content, actor.get<std::string>())) {
                    return {false, "unknown_behavior_recipe_actor",
                            "An evolved behavior recipe references an unknown available_to NPC."};
                }
                if (!actorIds.insert(actor.get<std::string>()).second) {
                    return {false, "duplicate_behavior_recipe_actor",
                            "An evolved behavior recipe must reference each available_to NPC exactly once."};
                }
                recipe.allowedActors.push_back(actor.get<std::string>());
            }
            recipe.durationMinutes = operation.value("duration_minutes", 0);
            recipe.interruptible = operation.value("interruptible", true);
            recipe.interruptSalienceThreshold = operation.value("interrupt_salience_threshold", 1.0);
            recipe.reservationMode = operation.value("reservation_mode", "none");
            const auto reservations = operation.value("reservations", Json::object());
            if (!reservations.is_object()) {
                return {false, "invalid_behavior_reservations", "Behavior recipe reservations must be an object."};
            }
            for (auto item = reservations.begin(); item != reservations.end(); ++item) {
                if (!content.resources.contains(item.key()) || !item.value().is_number_integer() ||
                    item.value().get<int>() <= 0) {
                    return {false, "invalid_behavior_reservations",
                            "Behavior recipe reservations must reference positive quantities of registered resources."};
                }
                recipe.reservations[item.key()] = item.value().get<int>();
            }
            recipe.priority = operation.value("priority", 0);
            recipe.preemptionPolicy = operation.value("preemption_policy", "none");
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

std::string toString(InnovationLifecycle status) {
    switch (status) {
    case InnovationLifecycle::Hypothesized: return "hypothesized";
    case InnovationLifecycle::Refining: return "refining";
    case InnovationLifecycle::Evaluated: return "evaluated";
    case InnovationLifecycle::Accepted: return "accepted";
    case InnovationLifecycle::Rejected: return "rejected";
    case InnovationLifecycle::Abandoned: return "abandoned";
    }
    return "hypothesized";
}

InnovationLifecycle innovationLifecycleFromString(const std::string& value) {
    if (value == "refining") return InnovationLifecycle::Refining;
    if (value == "evaluated") return InnovationLifecycle::Evaluated;
    if (value == "accepted") return InnovationLifecycle::Accepted;
    if (value == "rejected") return InnovationLifecycle::Rejected;
    if (value == "abandoned") return InnovationLifecycle::Abandoned;
    return InnovationLifecycle::Hypothesized;
}

EvolutionProposalStatus evolutionProposalStatusFromString(const std::string& value) {
    if (value == "validated") return EvolutionProposalStatus::Validated;
    if (value == "approved") return EvolutionProposalStatus::Approved;
    if (value == "rejected") return EvolutionProposalStatus::Rejected;
    return EvolutionProposalStatus::Proposed;
}

std::set<std::string> EvolutionManager::allowedOperationIds(const std::string& evolutionLevel) {
    if (evolutionLevel == "level1") {
        return {"overlay_strategy", "overlay_schedule", "register_behavior_recipe"};
    }
    if (evolutionLevel == "level2") {
        return {"create_entity_archetype", "register_behavior_recipe"};
    }
    return {};
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
    proposal.targetNpcId = value.value("target_npc", "");
    if (!proposal.targetNpcId.empty() && proposal.evolutionLevel != "level1") {
        throw std::runtime_error("target_npc is only valid for level1 evolution proposals");
    }
    proposal.innovation = value.value("innovation", Json::object());
    if (!proposal.innovation.is_object()) {
        throw std::runtime_error("innovation metadata must be an object");
    }
    proposal.innovationStatus = value.value("innovation_status", "hypothesized");
    static const std::set<std::string> innovationStatuses{
        "hypothesized", "refining", "evaluated", "accepted", "rejected", "abandoned"};
    if (!innovationStatuses.contains(proposal.innovationStatus)) {
        throw std::runtime_error("unsupported innovation lifecycle status");
    }
    if (proposal.kind != "innovation" && proposal.innovationStatus != "hypothesized") {
        throw std::runtime_error("innovation lifecycle status is only valid for innovation proposals");
    }
    return proposal;
}

Json EvolutionManager::toJson(const EvolutionProposal& proposal) {
    Json evidence = Json::array();
    for (const auto sequence : proposal.evidenceEventSequences) evidence.push_back(sequence);
    Json result{{"kind", proposal.kind}, {"schema_version", 1}, {"id", proposal.id}, {"source", proposal.source},
                {"base_content_version", proposal.baseContentVersion},
                {"base_world_revision", proposal.baseWorldRevision},
                {"evidence_event_sequences", evidence}, {"operations", proposal.operations},
                {"status", toString(proposal.status)}, {"evolution_level", proposal.evolutionLevel}};
    if (!proposal.targetNpcId.empty()) result["target_npc"] = proposal.targetNpcId;
    if (!proposal.innovation.empty()) {
        result["innovation"] = proposal.innovation;
        result["innovation_status"] = proposal.innovationStatus;
    }
    return result;
}

EvolutionProposalRequestResult EvolutionManager::propose(const WorldService& world,
                                                          const std::string& evolutionLevel,
                                                          const std::string& trigger,
                                                          const std::string& targetNpcId) {
    EvolutionProposalRequestResult result;
    result.provider = world.providerName();
    std::string scopedTarget = targetNpcId;
    if (evolutionLevel == "level1" && scopedTarget.empty() && !world.content().npcs.empty()) {
        scopedTarget = world.content().npcs.front().definition.id;
    }
    Json feedback = Json::object();
    // A malformed/unsupported proposal may be repaired twice using the same
    // captured world context. Keeping this bound prevents an LLM validation
    // loop from becoming the simulation scheduler while allowing a generated
    // behavior tree to fix both its root and nested-node errors.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto response = world.requestEvolutionProposal(evolutionLevel, trigger, feedback, scopedTarget);
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
            auto proposal = EvolutionManager::normalizeProviderProposal(
                response.evolutionProposal, world, evolutionLevel, result.provider);
            proposal.targetNpcId = scopedTarget;
            const auto validation = EvolutionManager::validate(proposal, world.content(), world.state());
            if (!validation.valid) {
                if (attempt == 0) {
                    feedback = Json{{"attempt", attempt + 1}, {"code", validation.code},
                                    {"message", validation.message}, {"previous_proposal", EvolutionManager::toJson(proposal)}};
                    continue;
                }
                result.code = validation.code;
                result.message = validation.message;
                return result;
            }
            result.accepted = true;
            result.code = "ok";
            result.message = attempt == 0
                                 ? "The LLM produced a validated proposal; Runtime preflight and autonomous admissibility remain."
                                 : "The LLM repaired its proposal after Runtime validation feedback; Runtime preflight and autonomous admissibility remain.";
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

EvolutionProposal EvolutionManager::normalizeProviderProposal(const Json& value,
                                                               const WorldService& world,
                                                               const std::string& evolutionLevel,
                                                               const std::string& provider) {
    if (!value.is_object()) throw std::runtime_error("The provider returned a non-object evolution proposal.");
    Json normalized = value;
    normalized["schema_version"] = 1;
    static const std::set<std::string> proposalKinds{
        "content", "knowledge", "strategy", "declarative_capability", "innovation"};
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
                      : operationKind == "register_behavior_recipe" ? "declarative_capability" : "content";
    }
    normalized["kind"] = proposalKind;
    normalized["id"] = normalized.value("id", "llm-evolution-" + std::to_string(world.state().revision));
    normalized["source"] = "llm:" + provider;
    normalized["base_content_version"] = world.content().contentVersion;
    normalized["base_world_revision"] = world.state().revision;
    normalized["status"] = "proposed";
    normalized["evolution_level"] = evolutionLevel;
    return EvolutionManager::fromJson(normalized);
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
        "content", "knowledge", "strategy", "declarative_capability", "innovation"};
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
    if (proposal.evolutionLevel == "level1" && proposal.targetNpcId.empty()) {
        return {false, "missing_target_npc", "A Level 1 proposal must identify the NPC whose strategy may evolve."};
    }
    if (proposal.evolutionLevel == "level1" && !hasNpc(content, proposal.targetNpcId)) {
        return {false, "unknown_target_npc", "A Level 1 proposal must target an NPC defined by the active content."};
    }
    if (proposal.evolutionLevel == "level2" && !proposal.targetNpcId.empty()) {
        return {false, "level_target_mismatch", "Level 2 proposals cannot carry an individual NPC target."};
    }
    if (!proposal.operations.is_array() || proposal.operations.empty()) {
        return {false, "empty_operations", "A proposal must contain at least one operation."};
    }
    const auto allowed = allowedOperationIds(proposal.evolutionLevel);
    if (allowed.empty()) {
        return {false, "invalid_evolution_level", "Proposal evolution level is outside the governed lanes."};
    }
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
                                      "preemption_policy", "reservations", "parameters", "available_to", "root"}) fields.insert(field);
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
            if (proposal.evolutionLevel == "level1" && !proposal.targetNpcId.empty() &&
                operation.at("npc_id").get<std::string>() != proposal.targetNpcId) {
                return {false, "level_target_mismatch",
                        "A Level 1 strategy overlay must target the NPC whose context was evaluated."};
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
            if (proposal.evolutionLevel == "level1" && !proposal.targetNpcId.empty() &&
                operation.at("npc_id").get<std::string>() != proposal.targetNpcId) {
                return {false, "level_target_mismatch",
                        "A Level 1 schedule overlay must target the NPC whose context was evaluated."};
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
                recipe.reservationMode = operation.value("reservation_mode", "none");
                const auto reservations = operation.value("reservations", Json::object());
                if (!reservations.is_object()) {
                    return {false, "invalid_behavior_reservations", "Behavior recipe reservations must be an object."};
                }
                for (auto item = reservations.begin(); item != reservations.end(); ++item) {
                    if (!content.resources.contains(item.key()) || !item.value().is_number_integer() ||
                        item.value().get<int>() <= 0) {
                        return {false, "invalid_behavior_reservations",
                                "Behavior recipe reservations must reference positive quantities of registered resources."};
                    }
                    recipe.reservations[item.key()] = item.value().get<int>();
                }
                recipe.priority = operation.value("priority", 0);
                recipe.preemptionPolicy = operation.value("preemption_policy", "none");
                const auto parameterValidation = validateParameterSchema(recipe.parameterSchema);
                if (!parameterValidation.valid) return parameterValidation;
                const auto actors = operation.value("available_to", Json::array());
                if (!actors.is_array() || actors.empty()) {
                    return {false, "behavior_recipe_actors_missing",
                            "An evolved behavior recipe must declare a non-empty available_to actor scope."};
                }
                std::set<std::string> actorIds;
                for (const auto& actor : actors) {
                    if (!actor.is_string() || actor.get<std::string>().empty() ||
                        !hasNpc(content, actor.get<std::string>())) {
                        return {false, "unknown_behavior_recipe_actor",
                            "An evolved behavior recipe references an unknown available_to NPC."};
                    }
                    if (!actorIds.insert(actor.get<std::string>()).second) {
                        return {false, "duplicate_behavior_recipe_actor",
                                "An evolved behavior recipe must reference each available_to NPC exactly once."};
                    }
                }
                if (proposal.evolutionLevel == "level1" && !proposal.targetNpcId.empty() &&
                    !actorIds.contains(proposal.targetNpcId)) {
                    return {false, "level_target_mismatch",
                            "A Level 1 behavior recipe must be available to the NPC whose context was evaluated."};
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
    const auto innovationValidation = validateInnovationSpec(proposal);
    if (!innovationValidation.valid) return innovationValidation;
    return {true, "ok", "Proposal is structurally valid and ready for Runtime preflight."};
}

EvolutionValidation EvolutionManager::approve(EvolutionProposal& proposal,
                                               const std::string& approver,
                                               bool sandboxPassed) {
    if (proposal.status != EvolutionProposalStatus::Validated) {
        return {false, "validation_required", "A proposal must pass validation before approval."};
    }
    // `runtime_governor` is the normal autonomous actor. It only reaches this
    // method after Runtime validation/preflight; the legacy headless actor is
    // retained for audit/replay tooling. Provider identities remain forbidden.
    if (approver != "runtime_governor" && approver != "developer" &&
        approver != "world_designer" && approver != "headless_governor") {
        proposal.status = EvolutionProposalStatus::Rejected;
        return {false, "approval_forbidden",
                "Only the Runtime Governor, developer, world designer, or explicit headless governor may approve evolution."};
    }
    if (!sandboxPassed) {
        proposal.status = EvolutionProposalStatus::Rejected;
        return {false, "preflight_failed", "A proposal cannot activate without a passing Runtime preflight."};
    }
    if (proposal.kind == "innovation" && proposal.innovationStatus != "accepted") {
        return {false, "innovation_acceptance_required",
                "An innovation hypothesis must be explicitly accepted before its overlay can be approved."};
    }
    proposal.status = EvolutionProposalStatus::Approved;
    return {true, "approved", "Proposal is approved for versioned overlay activation."};
}

EvolutionValidation EvolutionManager::transitionInnovation(EvolutionProposal& proposal,
                                                             const std::string& nextStatus) {
    if (proposal.kind != "innovation" || proposal.innovation.empty()) {
        return {false, "innovation_required", "Only an innovation proposal has an innovation lifecycle."};
    }
    static const std::set<std::string> statuses{
        "hypothesized", "refining", "evaluated", "accepted", "rejected", "abandoned"};
    if (!statuses.contains(nextStatus)) {
        return {false, "innovation_status_invalid", "Innovation lifecycle status is not registered."};
    }
    const auto current = proposal.innovationStatus;
    const bool allowed =
        (current == "hypothesized" && (nextStatus == "refining" || nextStatus == "evaluated" ||
                                       nextStatus == "accepted" || nextStatus == "abandoned")) ||
        (current == "refining" && (nextStatus == "refining" || nextStatus == "evaluated" || nextStatus == "abandoned")) ||
        (current == "evaluated" && (nextStatus == "accepted" || nextStatus == "rejected" || nextStatus == "abandoned")) ||
        (current == "accepted" && nextStatus == "abandoned");
    if (!allowed) {
        return {false, "innovation_status_transition_invalid",
                "Innovation lifecycle cannot move from " + current + " to " + nextStatus + "."};
    }
    proposal.innovationStatus = nextStatus;
    return {true, "ok", "Innovation lifecycle transitioned to " + nextStatus + "."};
}

EvolutionValidation EvolutionManager::applyOverlay(WorldContentPack& content,
                                                    const EvolutionProposal& proposal) {
    if (proposal.status != EvolutionProposalStatus::Approved) {
        return {false, "approval_required", "Only an approved proposal can activate an overlay."};
    }
    if (proposal.baseContentVersion != content.contentVersion) {
        return {false, "content_version_conflict", "Overlay base content does not match the active content."};
    }
    if (proposal.kind == "innovation" && proposal.innovationStatus != "accepted") {
        return {false, "innovation_acceptance_required",
                "An innovation hypothesis must be accepted before its overlay can activate."};
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
        int resourceTotal = 0;
        for (const auto& [id, resource] : sandboxWorld.state().resources) {
            resources[id] = resource.amount;
            resourceTotal += resource.amount;
        }
        std::size_t knowledge = 0;
        std::size_t experiences = 0;
        for (const auto& [id, npc] : sandboxWorld.state().npcs) {
            knowledge += npc.knowledge.size();
            experiences += npc.experiences.size();
        }
        result.metrics = Json{{"events_added", result.eventDiff.size()}, {"final_revision", result.finalRevision},
                              {"resources", resources}, {"npc_count", sandboxWorld.state().npcs.size()},
                              {"knowledge_count", knowledge}, {"experience_count", experiences},
                              {"advance_minutes", advanceMinutes}, {"resource_total", resourceTotal}};
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
