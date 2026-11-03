#include "evolution/evolution.hpp"
#include "world/meta_actions.hpp"

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
    return Json{{"kind", proposal.kind}, {"id", proposal.id}, {"source", proposal.source},
                {"base_content_version", proposal.baseContentVersion},
                {"base_world_revision", proposal.baseWorldRevision},
                {"operations", proposal.operations}, {"status", toString(proposal.status)}};
}

EvolutionValidation EvolutionManager::validate(const EvolutionProposal& proposal,
                                               const WorldContentPack& content,
                                               std::uint64_t currentRevision) {
    if (proposal.id.empty() || proposal.kind.empty()) return {false, "invalid_identity", "Proposal id and kind are required."};
    if (proposal.baseContentVersion != content.contentVersion) {
        return {false, "content_version_conflict", "Proposal was created against a different content version."};
    }
    if (proposal.baseWorldRevision > currentRevision) {
        return {false, "future_revision", "Proposal references a world revision that does not exist."};
    }
    if (!proposal.operations.is_array() || proposal.operations.empty()) {
        return {false, "empty_operations", "A proposal must contain at least one operation."};
    }
    static const std::set<std::string> allowed{
        "create_entity_archetype", "register_behavior_recipe", "overlay_strategy",
        "add_relationship", "schedule_event"};
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
    }
    return {true, "ok", "Proposal is structurally valid and ready for sandbox approval."};
}

EvolutionValidation EvolutionManager::approve(EvolutionProposal& proposal,
                                               const std::string& approver,
                                               bool sandboxPassed) {
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

}  // namespace tide_marks
