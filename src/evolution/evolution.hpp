#pragma once

#include "content/content_pack.hpp"
#include "core/types.hpp"

#include <string>
#include <vector>

namespace tide_marks {

class WorldService;

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

struct EvolutionSandboxResult {
    bool passed{false};
    std::string code;
    std::string message;
    std::string proposalId;
    std::string overlayVersion;
    std::string baseContentVersion;
    std::uint64_t baseRevision{0};
    std::uint64_t finalRevision{0};
    Json metrics = Json::object();
    Json eventDiff = Json::array();
    Json snapshot = Json::object();
};

class EvolutionProposalStore {
public:
    static void append(const std::string& path, const EvolutionProposal& proposal);
    static std::vector<EvolutionProposal> load(const std::string& path);
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
    [[nodiscard]] static EvolutionValidation applyOverlay(WorldContentPack& content,
                                                            const EvolutionProposal& proposal);
    [[nodiscard]] static EvolutionSandboxResult sandbox(const EvolutionProposal& proposal,
                                                         const WorldService& base,
                                                         std::int64_t advanceMinutes = 0);
};

std::string toString(EvolutionProposalStatus status);
EvolutionProposalStatus evolutionProposalStatusFromString(const std::string& value);

}  // namespace tide_marks
