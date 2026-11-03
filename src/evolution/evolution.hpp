#pragma once

#include "content/content_pack.hpp"
#include "core/types.hpp"

#include <string>

namespace tide_marks {

enum class EvolutionProposalStatus { Proposed, Validated, Approved, Rejected };

struct EvolutionProposal {
    std::string id;
    std::string kind;
    std::string source;
    std::string baseContentVersion;
    std::uint64_t baseWorldRevision{0};
    Json operations = Json::array();
    EvolutionProposalStatus status{EvolutionProposalStatus::Proposed};
};

struct EvolutionValidation {
    bool valid{false};
    std::string code;
    std::string message;
};

class EvolutionManager {
public:
    [[nodiscard]] static EvolutionValidation validate(const EvolutionProposal& proposal,
                                                       const WorldContentPack& content,
                                                       std::uint64_t currentRevision);
    [[nodiscard]] static EvolutionValidation approve(EvolutionProposal& proposal,
                                                       const std::string& approver,
                                                       bool sandboxPassed);
    [[nodiscard]] static EvolutionProposal fromJson(const Json& value);
    [[nodiscard]] static Json toJson(const EvolutionProposal& proposal);
};

std::string toString(EvolutionProposalStatus status);
EvolutionProposalStatus evolutionProposalStatusFromString(const std::string& value);

}  // namespace tide_marks
