#pragma once

#include "content/content_pack.hpp"
#include "core/types.hpp"

#include <string>
#include <set>
#include <vector>

namespace tide_marks {

class WorldService;

enum class EvolutionProposalStatus { Proposed, Validated, Approved, Rejected };

// Innovation has a longer-lived lifecycle than a one-shot evolution request:
// a daily reflection may refine or abandon a hypothesis before it reaches the
// normal validate -> Runtime preflight -> approve -> overlay path. The full
// sandbox remains an explicit counterfactual experiment, not an activation gate.
enum class InnovationLifecycle { Hypothesized, Refining, Evaluated, Accepted, Rejected, Abandoned };

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
    // Level 1 is an individual strategy/experience lane and must identify its
    // target NPC; Level 2 proposals intentionally leave it empty.
    std::string targetNpcId;
    // An innovation is still a governed evolution proposal.  This envelope
    // records the hypothesis and its bounded evaluation plan; the actual
    // world change remains one of the registered declarative operations.
    // Keeping it as JSON lets the content schema evolve without creating a
    // second mutation path in Runtime Core.
    Json innovation = Json::object();
    // This status belongs only to the hypothesis. Overlay approval/activation
    // and NPC adoption are separate lifecycles recorded by their own facts.
    std::string innovationStatus{"hypothesized"};
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
    // The operation registry is the single source for lane membership. The
    // provider schema and validator may add field-level rules, but neither may
    // invent a new operation name independently.
    [[nodiscard]] static std::set<std::string> allowedOperationIds(const std::string& evolutionLevel);
    [[nodiscard]] static EvolutionValidation validate(const EvolutionProposal& proposal,
                                                       const WorldContentPack& content,
                                                       const WorldState& state);
    [[nodiscard]] static EvolutionValidation approve(EvolutionProposal& proposal,
                                                       const std::string& approver,
                                                       bool sandboxPassed);
    [[nodiscard]] static EvolutionValidation transitionInnovation(EvolutionProposal& proposal,
                                                                    const std::string& nextStatus);
    [[nodiscard]] static EvolutionProposal fromJson(const Json& value);
    [[nodiscard]] static Json toJson(const EvolutionProposal& proposal);
    // Converts one provider payload into the governed proposal envelope. It
    // does not validate, approve, sandbox, or mutate the world.
    [[nodiscard]] static EvolutionProposal normalizeProviderProposal(const Json& value,
                                                                       const WorldService& world,
                                                                       const std::string& evolutionLevel,
                                                                       const std::string& provider);
    // Calls the configured LLM explicitly. The returned proposal is still
    // Proposed and must pass validate -> Runtime preflight -> independent
    // Runtime Governor approval for ordinary activation.
    [[nodiscard]] static EvolutionProposalRequestResult propose(const WorldService& world,
                                                                 const std::string& evolutionLevel,
                                                                 const std::string& trigger,
                                                                 const std::string& targetNpcId = {});
    [[nodiscard]] static EvolutionValidation applyOverlay(WorldContentPack& content,
                                                            const EvolutionProposal& proposal);
    [[nodiscard]] static EvolutionSandboxResult sandbox(const EvolutionProposal& proposal,
                                                         const WorldService& base,
                                                         std::int64_t advanceMinutes = 0);
};

std::string toString(EvolutionProposalStatus status);
EvolutionProposalStatus evolutionProposalStatusFromString(const std::string& value);
std::string toString(InnovationLifecycle status);
InnovationLifecycle innovationLifecycleFromString(const std::string& value);

}  // namespace tide_marks
