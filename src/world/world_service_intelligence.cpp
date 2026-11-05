#include "world/world_service.hpp"

#include "world/world_evolution_system.hpp"
#include "world/world_intelligence_system.hpp"
#include "provider/evolution_queue.hpp"

#include <chrono>
#include <thread>

namespace tide_marks {

DialogueTurn WorldService::converse(const std::string& npcId, const std::string& playerInput,
                                    const std::string& turnId) {
    return WorldIntelligenceSystem(*this).converse(npcId, playerInput, turnId);
}

DialogueTurn WorldService::deliberate(const std::string& npcId, const std::string& trigger,
                                      const std::string& turnId) {
    return WorldIntelligenceSystem(*this).deliberate(npcId, trigger, turnId);
}

ProviderResponse WorldService::requestEvolutionProposal(const std::string& evolutionLevel,
                                                         const std::string& trigger,
                                                         const Json& validationFeedback,
                                                         const std::string& targetNpcId) const {
    return WorldEvolutionSystem::requestProposal(*this, evolutionLevel, trigger, validationFeedback, targetNpcId);
}

ProviderResponse WorldService::requestDailyReflection(const std::string& targetNpcId) const {
    return WorldEvolutionSystem::requestReflection(*this, targetNpcId);
}

CommandResult WorldService::commitDailyReflection(const std::string& targetNpcId,
                                                  const ProviderResponse& response) {
    EvolutionRequest request;
    request.requestKind = "reflection";
    request.evolutionLevel = "level1";
    request.targetNpcId = targetNpcId;
    request.trigger = "daily_reflection";
    request.baseContentVersion = content_.contentVersion;
    request.baseWorldRevision = state_.revision;
    request.createdAt = state_.now;
    request.context = WorldEvolutionSystem::reflectionContext(*this, targetNpcId);
    return WorldEvolutionSystem(*this).commitReflection(request, response, provider_->name());
}

TurnRequest WorldService::makeTurnRequest(const std::string& npcId, const std::string& input,
                                          const std::string& turnId, const std::string& channelId,
                                          const std::string& trigger) {
    return WorldIntelligenceSystem(*this).makeTurnRequest(npcId, input, turnId, channelId, trigger);
}

DialogueTurn WorldService::executeTurn(const std::string& npcId, const std::string& input,
                                       const std::string& turnId, const std::string& channelId,
                                       const std::string& trigger) {
    return WorldIntelligenceSystem(*this).executeTurn(npcId, input, turnId, channelId, trigger);
}

DialogueTurn WorldService::commitTurn(const TurnRequest& request, ProviderResponse response) {
    return WorldIntelligenceSystem(*this).commitTurn(request, std::move(response));
}

void WorldService::enqueueAutonomousTurn(const std::string& npcId, const std::string& trigger,
                                         const std::string& channelId) {
    WorldIntelligenceSystem(*this).enqueueAutonomousTurn(npcId, trigger, channelId);
}

void WorldService::scheduleAutomaticEvolution() {
    WorldEvolutionSystem(*this).scheduleAutomatic();
}

std::size_t WorldService::pumpAsyncTurns() {
    return WorldIntelligenceSystem(*this).pumpAsyncTurns() +
           WorldEvolutionSystem(*this).pumpAsyncProposals();
}

std::size_t WorldService::pendingAsyncTurns() const {
    return WorldIntelligenceSystem::pendingAsyncTurns(*this) +
           (evolution_ ? evolution_->pending() : 0);
}

std::size_t WorldService::waitForAsyncTurns(std::int64_t timeoutMilliseconds) {
    if (timeoutMilliseconds <= 0) return pumpAsyncTurns();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMilliseconds);
    std::size_t consumed = 0;
    while (pendingAsyncTurns() > 0 && std::chrono::steady_clock::now() < deadline) {
        consumed += pumpAsyncTurns();
        if (pendingAsyncTurns() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // A worker can publish its ready result between the last pending() check
    // and the final pump. Give that hand-off a short quiescence window so a
    // bounded drain does not report a phantom pending request at an exact
    // timeout boundary. This does not extend the provider deadline; it only
    // drains a result that has already completed.
    for (int settle = 0; settle < 20; ++settle) {
        consumed += pumpAsyncTurns();
        if (pendingAsyncTurns() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return consumed;
}

}  // namespace tide_marks
