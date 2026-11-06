#pragma once

#include "provider/provider.hpp"

#include <string>
#include <vector>

namespace tide_marks {

class WorldService;
struct CommandResult;
struct EvolutionProposal;
struct EvolutionSandboxResult;
struct EvolutionRequest;

// Coordinates world-facing Level 1/Level 2 evolution. Provider requests and
// overlay lifecycle are kept outside WorldService, while all fact changes
// still use WorldService's transaction and event boundary.
class WorldEvolutionSystem {
public:
    explicit WorldEvolutionSystem(WorldService& world) : world_(world) {}

    [[nodiscard]] static ProviderResponse requestProposal(const WorldService& world,
                                                           const std::string& evolutionLevel,
                                                           const std::string& trigger,
                                                           const Json& validationFeedback,
                                                           const std::string& targetNpcId = {});
    [[nodiscard]] static Json proposalContext(const WorldService& world,
                                              const std::string& evolutionLevel,
                                              const std::string& trigger,
                                              const Json& validationFeedback,
                                              const std::string& targetNpcId = {});
    [[nodiscard]] static Json reflectionContext(const WorldService& world,
                                                 const std::string& targetNpcId);
    [[nodiscard]] static ProviderResponse requestReflection(const WorldService& world,
                                                             const std::string& targetNpcId);
    std::size_t pumpAsyncProposals();
    void scheduleAutomatic();
    CommandResult activateOverlay(const EvolutionProposal& proposal,
                                  const EvolutionSandboxResult& sandbox);
    CommandResult rollbackOverlay(const std::string& proposalId);
    CommandResult commitReflection(const EvolutionRequest& request,
                                   const ProviderResponse& response,
                                   const std::string& provider);
    CommandResult startAdoption(const std::string& npcId, const std::string& proposalId);
    // An independent Runtime Governor may advance a persisted private
    // hypothesis before ordinary preflight -> approve -> activation. The
    // provider cannot call this entry point; the transition remains auditable.
    CommandResult transitionInnovationHypothesis(const std::string& npcId,
                                                  const std::string& proposalId,
                                                  const std::string& nextStatus,
                                                  const std::string& actor);
    CommandResult transitionAdoption(const std::string& npcId, const std::string& proposalId,
                                      InnovationAdoptionStatus status, const std::string& reason);

private:
    WorldService& world_;
};

}  // namespace tide_marks
