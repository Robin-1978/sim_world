#pragma once

#include "content/content_pack.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tide_marks {

class WorldService;

struct ScenarioIntervention {
    std::string operation;
    std::string subject;
    std::string metric;
    double delta{0.0};
    std::string reason;
};

struct ScenarioProposal {
    std::string id;
    std::string source;
    std::string baseContentVersion;
    std::uint64_t baseWorldRevision{0};
    std::int64_t durationMinutes{0};
    std::vector<ScenarioIntervention> interventions;
};

struct ScenarioValidation {
    bool valid{false};
    std::string code;
    std::string message;
};

struct ScenarioExperimentResult {
    bool passed{false};
    std::string code;
    std::string message;
    std::string proposalId;
    std::uint64_t baseRevision{0};
    Json appliedInterventions = Json::array();
    Json baselineMetrics = Json::object();
    Json scenarioMetrics = Json::object();
    Json delta = Json::object();
    Json baselineEvents = Json::array();
    Json events = Json::array();
    Json snapshot = Json::object();
};

[[nodiscard]] Json scenarioValidationToJson(const ScenarioValidation& validation);
[[nodiscard]] Json scenarioExperimentResultToJson(const ScenarioExperimentResult& result);

class ScenarioManager {
public:
    [[nodiscard]] static ScenarioValidation validate(const ScenarioProposal& proposal,
                                                      const WorldService& world);
    [[nodiscard]] static ScenarioExperimentResult run(const ScenarioProposal& proposal,
                                                       const WorldService& world);
    [[nodiscard]] static ScenarioProposal fromJson(const Json& value);
    [[nodiscard]] static Json toJson(const ScenarioProposal& proposal);
};

}  // namespace tide_marks
