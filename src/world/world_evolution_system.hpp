#pragma once

#include "provider/provider.hpp"

#include <string>
#include <vector>

namespace tide_marks {

class WorldService;
struct CommandResult;
struct EvolutionProposal;
struct EvolutionSandboxResult;

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
    std::size_t pumpAsyncProposals();
    void scheduleAutomatic();
    CommandResult activateOverlay(const EvolutionProposal& proposal,
                                  const EvolutionSandboxResult& sandbox);
    CommandResult rollbackOverlay(const std::string& proposalId);

private:
    WorldService& world_;
};

}  // namespace tide_marks
