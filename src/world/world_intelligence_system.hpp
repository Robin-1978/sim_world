#pragma once

#include "provider/turn_queue.hpp"

#include <cstdint>
#include <string>

namespace tide_marks {

class WorldService;
struct DialogueTurn;

// Owns the world-facing side of LLM decisions: bounded context capture,
// proposal validation/commit, process startup, and asynchronous result drain.
// Provider execution itself remains in IntelligenceCoordinator.
class WorldIntelligenceSystem {
public:
    explicit WorldIntelligenceSystem(WorldService& world) : world_(world) {}

    DialogueTurn converse(const std::string& npcId, const std::string& playerInput,
                          const std::string& turnId);
    DialogueTurn deliberate(const std::string& npcId, const std::string& trigger,
                            const std::string& turnId);
    TurnRequest makeTurnRequest(const std::string& npcId, const std::string& input,
                                const std::string& turnId, const std::string& channelId,
                                const std::string& trigger) const;
    DialogueTurn executeTurn(const std::string& npcId, const std::string& input,
                             const std::string& turnId, const std::string& channelId,
                             const std::string& trigger);
    DialogueTurn commitTurn(const TurnRequest& request, ProviderResponse response);
    void enqueueAutonomousTurn(const std::string& npcId, const std::string& trigger,
                               const std::string& channelId = {});
    std::size_t pumpAsyncTurns();
    [[nodiscard]] std::size_t pendingAsyncTurns() const;
    [[nodiscard]] static std::size_t pendingAsyncTurns(const WorldService& world);
    std::size_t waitForAsyncTurns(std::int64_t timeoutMilliseconds);

private:
    friend class WorldService;
    WorldService& world_;
};

}  // namespace tide_marks
