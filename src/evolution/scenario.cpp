#include "evolution/scenario.hpp"
#include "evolution/anomaly.hpp"
#include "world/world_service.hpp"
#include "world/world_state_json.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tide_marks {
namespace {
double numericAt(const Json& value, const std::string& key, const std::string& context) {
    if (!value.is_object() || !value.contains(key) || !value.at(key).is_number()) {
        throw std::runtime_error("missing numeric " + key + " in " + context);
    }
    return value.at(key).get<double>();
}

Json metricsFor(const WorldContentPack& content, const WorldState& state) {
    Json resources = Json::object();
    for (const auto& [id, resource] : state.resources) resources[id] = resource.amount;
    Json inventories = Json::object();
    for (const auto& [owner, inventory] : state.inventories) inventories[owner] = inventory;
    Json groups = Json::object();
    for (const auto& [id, group] : state.socialGroups) groups[id] = group.metrics;
    Json needs = Json::object();
    Json relationships = Json::object();
    Json locations = Json::object();
    for (const auto& [npcId, npc] : state.npcs) {
        for (const auto& [needId, need] : npc.needs) needs[npcId][needId] = need.deficit;
        locations[npcId] = npc.location;
        for (const auto& [target, relation] : npc.relationships) {
            relationships[npcId][target] = Json{{"trust", relation.trust}, {"respect", relation.respect},
                                                 {"fear", relation.fear}, {"owed", relation.owed}};
        }
    }
    Json promises = Json::object();
    for (const auto& [id, promise] : state.promises) promises[id] = toString(promise.status);
    Json anomalies = Json::array();
    for (const auto& finding : AnomalyDetector::scan(content, state)) anomalies.push_back(anomalyFindingToJson(finding));
    return Json{{"resources", resources}, {"inventories", inventories}, {"social_groups", groups},
                {"needs", needs}, {"relationships", relationships}, {"locations", locations},
                {"promises", promises}, {"anomalies", anomalies}, {"event_count", state.events.size()},
                {"revision", state.revision}, {"now", state.now.minute}};
}

Json numericDelta(const Json& baseline, const Json& scenario) {
    const auto diff = [](const Json& before, const Json& after, const auto& self) -> Json {
        if (after.is_number()) {
            return after.get<double>() - (before.is_number() ? before.get<double>() : 0.0);
        }
        if (!after.is_object()) return Json();
        Json result = Json::object();
        for (auto item = after.begin(); item != after.end(); ++item) {
            const auto prior = before.is_object() && before.contains(item.key()) ? before.at(item.key()) : Json();
            const auto value = self(prior, item.value(), self);
            if (!value.is_null()) result[item.key()] = value;
        }
        return result;
    };
    Json result = Json::object();
    for (const auto& section : {"resources", "inventories", "social_groups", "needs", "relationships"}) {
        if (!baseline.contains(section) || !scenario.contains(section)) continue;
        result[section] = diff(baseline.at(section), scenario.at(section), diff);
    }
    return result;
}

bool isIntegral(double value) {
    return std::isfinite(value) && std::floor(value) == value;
}

Json applyIntervention(Json& snapshot, const ScenarioIntervention& intervention,
                       const WorldService& world) {
    if (intervention.operation == "resource.delta") {
        const auto definition = world.content().resources.find(intervention.subject);
        auto& entry = snapshot.at("resources").at(intervention.subject);
        const double before = numericAt(entry, "amount", "scenario resource");
        if (!definition->second.divisible && !isIntegral(intervention.delta)) {
            throw std::runtime_error("scenario delta is fractional for an indivisible resource");
        }
        const double after = before + intervention.delta;
        if (after < definition->second.minimum || after > definition->second.maximum) {
            throw std::runtime_error("scenario resource delta is outside the authored range");
        }
        entry["amount"] = static_cast<int>(std::llround(after));
        return Json{{"operation", intervention.operation}, {"subject", intervention.subject},
                    {"before", before}, {"after", static_cast<int>(std::llround(after))},
                    {"reason", intervention.reason}};
    }
    if (intervention.operation == "group_metric.delta") {
        auto& entry = snapshot.at("social_groups").at(intervention.subject).at("metrics")
                          .at(intervention.metric);
        const double before = entry.get<double>();
        entry = before + intervention.delta;
        return Json{{"operation", intervention.operation}, {"subject", intervention.subject},
                    {"metric", intervention.metric}, {"before", before}, {"after", before + intervention.delta},
                    {"reason", intervention.reason}};
    }
    if (intervention.operation == "npc.need.delta") {
        auto& entry = snapshot.at("npcs").at(intervention.subject).at("needs").at(intervention.metric)
                          .at("deficit");
        const double before = entry.get<double>();
        const double after = std::clamp(before + intervention.delta, 0.0, 1.0);
        entry = after;
        return Json{{"operation", intervention.operation}, {"subject", intervention.subject},
                    {"metric", intervention.metric}, {"before", before}, {"after", after},
                    {"reason", intervention.reason}};
    }
    throw std::runtime_error("unsupported scenario intervention");
}
}  // namespace

Json scenarioValidationToJson(const ScenarioValidation& validation) {
    return Json{{"valid", validation.valid}, {"code", validation.code}, {"message", validation.message}};
}

Json scenarioExperimentResultToJson(const ScenarioExperimentResult& result) {
    return Json{{"passed", result.passed},
                {"code", result.code},
                {"message", result.message},
                {"proposal_id", result.proposalId},
                {"base_revision", result.baseRevision},
                {"applied_interventions", result.appliedInterventions},
                {"baseline_metrics", result.baselineMetrics},
                {"scenario_metrics", result.scenarioMetrics},
                {"delta", result.delta},
                {"baseline_events", result.baselineEvents},
                {"events", result.events},
                {"snapshot", result.snapshot}};
}

ScenarioProposal ScenarioManager::fromJson(const Json& value) {
    if (!value.is_object() || value.value("kind", "") != "scenario_proposal") {
        throw std::runtime_error("scenario proposal kind must be scenario_proposal");
    }
    if (value.value("schema_version", 0) != 1) {
        throw std::runtime_error("unsupported scenario proposal schema_version");
    }
    ScenarioProposal proposal;
    proposal.id = value.value("id", "");
    proposal.source = value.value("source", "unknown");
    proposal.baseContentVersion = value.value("base_content_version", "");
    proposal.baseWorldRevision = value.value("base_world_revision", 0ULL);
    proposal.durationMinutes = value.value("duration_minutes", 0);
    const auto interventions = value.value("interventions", Json::array());
    if (!interventions.is_array()) throw std::runtime_error("scenario interventions must be an array");
    for (const auto& item : interventions) {
        proposal.interventions.push_back(ScenarioIntervention{
            item.value("operation", ""), item.value("subject", ""), item.value("metric", ""),
            item.value("delta", 0.0), item.value("reason", "")});
    }
    return proposal;
}

Json ScenarioManager::toJson(const ScenarioProposal& proposal) {
    Json interventions = Json::array();
    for (const auto& item : proposal.interventions) {
        interventions.push_back(Json{{"operation", item.operation}, {"subject", item.subject},
                                     {"metric", item.metric}, {"delta", item.delta},
                                     {"reason", item.reason}});
    }
    return Json{{"kind", "scenario_proposal"}, {"schema_version", 1}, {"id", proposal.id}, {"source", proposal.source},
                {"base_content_version", proposal.baseContentVersion},
                {"base_world_revision", proposal.baseWorldRevision},
                {"duration_minutes", proposal.durationMinutes}, {"interventions", interventions}};
}

ScenarioValidation ScenarioManager::validate(const ScenarioProposal& proposal, const WorldService& world) {
    if (proposal.id.empty() || proposal.source.empty()) {
        return {false, "invalid_identity", "A scenario proposal needs an id and source."};
    }
    if (proposal.baseContentVersion != world.content().contentVersion) {
        return {false, "content_version_conflict", "Scenario base content does not match the world."};
    }
    if (proposal.baseWorldRevision != world.state().revision) {
        return {false, "revision_conflict", "Scenario must reference the current world revision."};
    }
    if (proposal.durationMinutes < 0 || proposal.durationMinutes > 366 * kMinutesPerDay) {
        return {false, "invalid_duration", "Scenario duration must be between zero and one year."};
    }
    if (proposal.interventions.empty()) {
        return {false, "empty_interventions", "A scenario needs at least one modeled intervention."};
    }
    for (const auto& intervention : proposal.interventions) {
        if (!std::isfinite(intervention.delta)) {
            return {false, "invalid_delta", "Scenario deltas must be finite."};
        }
        if (intervention.operation == "resource.delta") {
            const auto resource = world.content().resources.find(intervention.subject);
            const auto current = world.state().resources.find(intervention.subject);
            if (resource == world.content().resources.end() || current == world.state().resources.end()) {
                return {false, "unknown_resource", "Scenario references an unknown resource."};
            }
            const double next = current->second.amount + intervention.delta;
            if (next < resource->second.minimum || next > resource->second.maximum) {
                return {false, "resource_out_of_range", "Scenario resource delta exceeds authored bounds."};
            }
            if (!resource->second.divisible && !isIntegral(intervention.delta)) {
                return {false, "fractional_indivisible_resource",
                        "Scenario cannot use a fractional delta for an indivisible resource."};
            }
        } else if (intervention.operation == "group_metric.delta") {
            const auto group = world.state().socialGroups.find(intervention.subject);
            if (group == world.state().socialGroups.end() || !group->second.metrics.contains(intervention.metric)) {
                return {false, "unknown_group_metric", "Scenario references an unknown group metric."};
            }
        } else if (intervention.operation == "npc.need.delta") {
            const auto npc = world.state().npcs.find(intervention.subject);
            if (npc == world.state().npcs.end() || !npc->second.needs.contains(intervention.metric)) {
                return {false, "unknown_need", "Scenario references an unknown NPC need."};
            }
        } else {
            return {false, "unsupported_intervention", "The intervention is not a modeled scenario lever."};
        }
    }
    return {true, "ok", "Scenario is valid for an isolated counterfactual run."};
}

ScenarioExperimentResult ScenarioManager::run(const ScenarioProposal& proposal, const WorldService& world) {
    ScenarioExperimentResult result;
    result.proposalId = proposal.id;
    result.baseRevision = world.state().revision;
    const auto validation = validate(proposal, world);
    result.code = validation.code;
    result.message = validation.message;
    if (!validation.valid) return result;
    try {
        // Scenario comparison replays the already captured world decisions;
        // it must not issue fresh provider calls or make the result depend on
        // an external LLM service.
        auto baselineWorld = WorldService::fromContentSnapshot(
            world.content(), world.snapshotJson(), std::make_unique<MockProvider>());
        auto scenarioSnapshot = world.snapshotJson();
        for (const auto& intervention : proposal.interventions) {
            result.appliedInterventions.push_back(applyIntervention(scenarioSnapshot, intervention, world));
        }
        auto scenarioWorld = WorldService::fromContentSnapshot(
            world.content(), scenarioSnapshot, std::make_unique<MockProvider>());
        const auto eventCutoff = world.state().events.size();
        if (proposal.durationMinutes > 0) {
            baselineWorld.advanceMinutes(proposal.durationMinutes);
            scenarioWorld.advanceMinutes(proposal.durationMinutes);
        }
        result.baselineMetrics = metricsFor(baselineWorld.content(), baselineWorld.state());
        result.scenarioMetrics = metricsFor(scenarioWorld.content(), scenarioWorld.state());
        result.delta = numericDelta(result.baselineMetrics, result.scenarioMetrics);
        result.baselineEvents = Json::array();
        for (std::size_t index = eventCutoff; index < baselineWorld.state().events.size(); ++index) {
            result.baselineEvents.push_back(worldEventToJson(baselineWorld.state().events[index]));
        }
        for (std::size_t index = eventCutoff; index < scenarioWorld.state().events.size(); ++index) {
            result.events.push_back(worldEventToJson(scenarioWorld.state().events[index]));
        }
        result.snapshot = scenarioWorld.snapshotJson();
        result.passed = true;
        result.code = "ok";
        result.message = "Counterfactual completed against the same snapshot; source world was not mutated.";
    } catch (const std::exception& error) {
        result.code = "scenario_exception";
        result.message = error.what();
    }
    return result;
}

}  // namespace tide_marks
