#include "world/simulation_scheduler.hpp"

#include <algorithm>

namespace tide_marks {

std::int64_t SimulationScheduler::nextBoundary(const WorldState& state,
                                               const WorldContentPack& content,
                                               std::int64_t target) {
    const auto current = state.now.minute;
    auto boundary = target;
    const auto consider = [&boundary, current, target](std::int64_t candidate) {
        if (candidate > current && candidate <= target) boundary = std::min(boundary, candidate);
    };
    const auto nextMultiple = [current](std::int64_t interval) {
        if (interval <= 0) return current + 1;
        return ((current / interval) + 1) * interval;
    };

    // Schedules, need decay, daily memories and activities observe these
    // boundaries even when no individual NPC wakes up there.
    consider(nextMultiple(kMinutesPerHour));
    consider(nextMultiple(kMinutesPerDay));

    for (const auto& [npcId, npc] : state.npcs) {
        const auto authored = std::find_if(content.npcs.begin(), content.npcs.end(),
                                           [&npcId](const auto& item) {
                                               return item.definition.id == npcId;
                                           });
        const auto interval = authored != content.npcs.end() && authored->simulationTier == "far"
                                  ? content.simulationPolicy.farTickMinutes
                                  : content.simulationPolicy.nearTickMinutes;
        consider(nextMultiple(std::max(1, interval)));
        if (npc.activity.process && npc.activity.process->status == ProcessStatus::Running) {
            consider(std::max(current + 1, npc.activity.process->completesAt.minute));
        }
        for (const auto& stimulus : npc.stimuli) {
            if (stimulus.active) consider(std::max(current + 1, stimulus.expiresAt.minute));
        }
    }
    for (const auto& [id, promise] : state.promises) {
        (void)id;
        if (promise.status == PromiseStatus::Accepted || promise.status == PromiseStatus::Renegotiated) {
            consider(std::max(current + 1, promise.due.minute));
        }
    }
    for (const auto& [id, contract] : state.exchangeContracts) {
        (void)id;
        if (contract.status == ExchangeContractStatus::Proposed ||
            contract.status == ExchangeContractStatus::Accepted) {
            consider(std::max(current + 1, contract.due.minute));
        }
    }
    return boundary;
}

}  // namespace tide_marks
