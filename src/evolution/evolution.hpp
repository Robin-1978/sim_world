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
    std::vector<std::uint64_t> evidenceEventSequences;
    // Explicitly records which governed evolution lane produced the proposal.
    // Existing hand-authored proposals default to level2 for compatibility
    // inside the still-unreleased prototype.
    std::string evolutionLevel{"level2"};
};

struct EvolutionProposalRequestResult {
    bool accepted{false};
    std::string code;
    std::string message;
    std::string provider;
    EvolutionProposal proposal;
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
                                                       const WorldState& state);
    [[nodiscard]] static EvolutionValidation approve(EvolutionProposal& proposal,
                                                       const std::string& approver,
                                                       bool sandboxPassed);
    [[nodiscard]] static EvolutionProposal fromJson(const Json& value);
    [[nodiscard]] static Json toJson(const EvolutionProposal& proposal);
    // Calls the configured LLM explicitly. The returned proposal is still
    // Proposed and must pass validate -> sandbox -> independent approval.
    [[nodiscard]] static EvolutionProposalRequestResult propose(const WorldService& world,
                                                                 const std::string& evolutionLevel,
                                                                 const std::string& trigger);
    [[nodiscard]] static EvolutionValidation applyOverlay(WorldContentPack& content,
                                                            const EvolutionProposal& proposal);
    [[nodiscard]] static EvolutionSandboxResult sandbox(const EvolutionProposal& proposal,
                                                         const WorldService& base,
                                                         std::int64_t advanceMinutes = 0);
};

std::string toString(EvolutionProposalStatus status);
EvolutionProposalStatus evolutionProposalStatusFromString(const std::string& value);

}  // namespace tide_marks
