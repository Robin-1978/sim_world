#pragma once

#include "content/content_pack.hpp"
#include "core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace tide_marks {

struct AnomalyFinding {
    std::string ruleId;
    std::string operation;
    std::string subject;
    std::string metric;
    double observed{0.0};
    double threshold{0.0};
    std::string severity;
    std::string description;
    std::uint64_t sourceSequence{0};
    std::string sourceEventType;
};

[[nodiscard]] Json anomalyFindingToJson(const AnomalyFinding& finding);

class AnomalyDetector {
public:
    [[nodiscard]] static std::vector<AnomalyFinding> scan(const WorldContentPack& content,
                                                           const WorldState& state);
};

}  // namespace tide_marks
