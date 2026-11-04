#include "world/security_system.hpp"

#include "world/meta_actions.hpp"
#include "world/ids.hpp"
#include "world/world_service.hpp"

#include <algorithm>
#include <cmath>

namespace tide_marks {

CommandResult SecuritySystem::applyOperator(const std::string& operation, const std::string& actor,
                                                  const std::string& target, const Json& args,
                                                  const std::string& turnId) {
    WorldService::TransactionScope transaction(world_);
    if (!turnId.empty() && world_.state_.processedTurnIds.contains(turnId)) {
        return world_.reject("duplicate_turn", "The security action was already applied.");
    }
    const auto resolve = [this](const std::string& value) -> std::string {
        if (value == "world" || value == kWorldActor) return kWorldActor;
        if (world_.state_.entities.contains(value)) return value;
        if (world_.state_.npcs.contains(value)) return npcEntityId(value);
        return {};
    };
    const auto actorEntity = resolve(actor);
    const auto requestedTarget = target.empty() ? actor : target;
    const auto targetEntity = resolve(requestedTarget == "self" ? actor : requestedTarget);
    const auto operatorKind = MetaActionRegistry::securityOperatorKind(operation);
    if (operatorKind == MetaActionRegistry::SecurityOperatorKind::Unknown) {
        return world_.reject("unknown_security_operator", "The security operator is not registered.");
    }
    if (actorEntity.empty() || targetEntity.empty() ||
        (actorEntity == targetEntity &&
         (operatorKind == MetaActionRegistry::SecurityOperatorKind::Conflict ||
          operatorKind == MetaActionRegistry::SecurityOperatorKind::Threaten))) {
        return world_.reject("invalid_security_target", "Security actions need distinct active actor and target entities.");
    }
    const auto active = [this](const std::string& id) {
        if (id == kWorldActor) return true;
        const auto found = world_.state_.entities.find(id);
        return found != world_.state_.entities.end() && found->second.lifecycle == EntityLifecycle::Active;
    };
    if (!active(actorEntity) || !active(targetEntity)) return world_.reject("inactive_security_target", "The security target is not active.");
    if (operatorKind == MetaActionRegistry::SecurityOperatorKind::Conflict ||
        operatorKind == MetaActionRegistry::SecurityOperatorKind::Threaten) {
        if (actorEntity == kWorldActor) return world_.reject("invalid_security_actor", "A conflict action needs a concrete actor.");
    }
    auto& security = world_.state_.security[targetEntity];
    const auto bounded = [](double value) { return std::clamp(value, 0.0, 1.0); };
    const double integrityBefore = security.integrity;
    const double availabilityBefore = security.availability;
    const double threatBefore = security.threat;
    const double exposureBefore = security.exposure;
    const bool evacuatedBefore = security.evacuated;
    const auto configured = std::find_if(
        world_.content_.metaActions.begin(), world_.content_.metaActions.end(),
        [&operation](const auto& entry) { return entry.second.operation == operation; });
    if (configured == world_.content_.metaActions.end()) {
        return world_.reject("security_operator_unconfigured",
                      "The security operator has no content-defined parameter policy.");
    }
    Json effectiveArgs = configured->second.defaultArgs;
    for (auto it = args.begin(); it != args.end(); ++it) effectiveArgs[it.key()] = it.value();
    const auto number = [&effectiveArgs](const char* key) -> std::optional<double> {
        if (!effectiveArgs.contains(key) || !effectiveArgs.at(key).is_number()) return std::nullopt;
        const auto value = effectiveArgs.at(key).get<double>();
        return std::isfinite(value) ? std::optional<double>{value} : std::nullopt;
    };
    const auto requiredNumber = [&number](const char* key) -> std::optional<double> {
        const auto value = number(key);
        if (!value.has_value()) return std::nullopt;
        return value;
    };
    const auto missing = [&operation](const char* key) {
        return CommandResult{false, "invalid_security_parameters",
                              "The content-defined " + operation + " operator needs numeric '" + key + "'.", std::nullopt};
    };
    switch (operatorKind) {
    case MetaActionRegistry::SecurityOperatorKind::Conflict: {
        const auto damage = requiredNumber("integrity_loss");
        const auto availabilityLoss = requiredNumber("availability_loss");
        const auto threatGain = requiredNumber("threat_gain");
        const auto exposureGain = requiredNumber("exposure_gain");
        if (!damage.has_value()) return missing("integrity_loss");
        if (!availabilityLoss.has_value()) return missing("availability_loss");
        if (!threatGain.has_value()) return missing("threat_gain");
        if (!exposureGain.has_value()) return missing("exposure_gain");
        security.integrity = bounded(security.integrity - *damage);
        security.availability = bounded(security.availability - *availabilityLoss);
        security.threat = bounded(security.threat + *threatGain);
        security.exposure = bounded(security.exposure + *exposureGain);
        break;
    }
    case MetaActionRegistry::SecurityOperatorKind::Threaten: {
        const auto threatGain = requiredNumber("threat_gain");
        const auto exposureGain = requiredNumber("exposure_gain");
        if (!threatGain.has_value()) return missing("threat_gain");
        if (!exposureGain.has_value()) return missing("exposure_gain");
        security.threat = bounded(security.threat + *threatGain);
        security.exposure = bounded(security.exposure + *exposureGain);
        break;
    }
    case MetaActionRegistry::SecurityOperatorKind::Defend: {
        const auto threatReduction = requiredNumber("threat_reduction");
        const auto exposureReduction = requiredNumber("exposure_reduction");
        if (!threatReduction.has_value()) return missing("threat_reduction");
        if (!exposureReduction.has_value()) return missing("exposure_reduction");
        security.threat = bounded(security.threat - *threatReduction);
        security.exposure = bounded(security.exposure - *exposureReduction);
        break;
    }
    case MetaActionRegistry::SecurityOperatorKind::Evacuate: {
        const auto exposureReduction = requiredNumber("exposure_reduction");
        if (!exposureReduction.has_value()) return missing("exposure_reduction");
        security.evacuated = true;
        security.exposure = bounded(security.exposure - *exposureReduction);
        break;
    }
    case MetaActionRegistry::SecurityOperatorKind::Repair: {
        const auto integrityRestore = requiredNumber("integrity_restore");
        const auto availabilityRestore = requiredNumber("availability_restore");
        const auto threatReduction = requiredNumber("threat_reduction");
        if (!integrityRestore.has_value()) return missing("integrity_restore");
        if (!availabilityRestore.has_value()) return missing("availability_restore");
        if (!threatReduction.has_value()) return missing("threat_reduction");
        const auto resource = effectiveArgs.value("resource", "");
        const int cost = effectiveArgs.value("amount", 0);
        if (resource.empty() || cost <= 0 || world_.availableResourceValue(resource) < cost) {
            return world_.reject("insufficient_resource", "Security repair lacks its authored repair resource.");
        }
        world_.state_.resources[resource].amount -= cost;
        security.integrity = bounded(security.integrity + *integrityRestore);
        security.availability = bounded(security.availability + *availabilityRestore);
        security.threat = bounded(security.threat - *threatReduction);
        security.evacuated = false;
        break;
    }
    case MetaActionRegistry::SecurityOperatorKind::Unknown:
        return world_.reject("unknown_security_operator", "The security operator is not registered.");
    }
    if (!turnId.empty()) world_.state_.processedTurnIds.insert(turnId);
    const auto eventActor = actorEntity.starts_with("npc:") ? actorEntity.substr(4) : actorEntity;
    const auto eventTarget = targetEntity.starts_with("npc:") ? targetEntity.substr(4) : targetEntity;
    const auto event = world_.addEvent(
        operation, eventActor, eventTarget, "A registered security operator changed target security facts",
        Json{{"operation", operation}, {"actor_entity", actorEntity}, {"target_entity", targetEntity},
             {"integrity_before", integrityBefore}, {"integrity_after", security.integrity},
             {"availability_before", availabilityBefore}, {"availability_after", security.availability},
             {"threat_before", threatBefore}, {"threat_after", security.threat},
             {"exposure_before", exposureBefore}, {"exposure_after", security.exposure},
             {"evacuated_before", evacuatedBefore}, {"evacuated_after", security.evacuated}});
    return CommandResult{true, "ok", operation, event};
}


}  // namespace tide_marks
