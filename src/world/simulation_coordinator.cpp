#include "world/simulation_coordinator.hpp"

#include "world/world_service.hpp"
#include "world/simulation_scheduler.hpp"
#include "world/stimulus_system.hpp"

#include <set>
#include <stdexcept>

namespace tide_marks {

void SimulationCoordinator::advanceMinutes(std::int64_t minutes) {
    if (minutes <= 0) return;

    // Provider work is consumed only at a world boundary. The worker never
    // runs while the WorldState transaction below is open.
    world_.pumpAsyncTurns();
    const auto target = world_.state_.now.minute + minutes;
    while (world_.state_.now.minute < target) {
        const auto boundary = SimulationScheduler::nextBoundary(world_.state_, world_.content_, target);
        const auto elapsedMinutes = boundary - world_.state_.now.minute;
        if (elapsedMinutes <= 0) throw std::logic_error("simulation scheduler did not advance time");

        // A time step is one state transition. A long offline jump is a
        // sequence of committed logical steps. Quiet minutes can be coalesced
        // into this transaction while retaining their revision count and final
        // state; observable boundaries still publish in timestamp order.
        std::set<std::string> dueNpcIds;
        bool autonomousBoundary = false;
        {
            WorldService::TransactionScope transaction(world_);
            world_.state_.now.minute = boundary;
            // TransactionScope commits one revision. Account for quiet logical
            // minutes coalesced before the final observable boundary.
            world_.transactionManager_.addRevisions(
                world_.state_, static_cast<std::uint64_t>(elapsedMinutes - 1));
            world_.simulationMetrics_.scheduledBoundaries++;
            world_.simulationMetrics_.simulatedMinutes += elapsedMinutes;
            StimulusSystem(world_).expire();

            for (auto& [id, npc] : world_.state_.npcs) {
                const auto* authored = world_.authoredNpc(id);
                const auto tier = authored != nullptr ? authored->simulationTier : std::string("near");
                const int interval = tier == "far" ? world_.content_.simulationPolicy.farTickMinutes
                                                    : world_.content_.simulationPolicy.nearTickMinutes;
                const bool processDue = npc.activity.process &&
                                        npc.activity.process->status == ProcessStatus::Running &&
                                        npc.activity.process->completesAt.minute <= world_.state_.now.minute;
                const bool due = (interval > 0 && world_.state_.now.minute % interval == 0) || processDue;
                if (due) {
                    dueNpcIds.insert(id);
                    if (tier == "far") ++world_.simulationMetrics_.farNpcTicks;
                    else ++world_.simulationMetrics_.nearNpcTicks;
                    npc.lastSimulationTick = world_.state_.now;
                }
                // No NPC wakeup occurred at a coalesced minute, so the next
                // boundary is selected as the earliest observable one.
                world_.simulationMetrics_.skippedNpcTicks +=
                    static_cast<std::uint64_t>(elapsedMinutes - (due ? 1 : 0));
            }

            world_.resumeSuspendedProcesses(dueNpcIds);
            world_.tickProcesses(dueNpcIds);
            if (world_.state_.now.minute % kMinutesPerHour == 0) {
                autonomousBoundary = true;
                world_.processHourBoundary(world_.state_.now.day(), world_.state_.now.hour(), dueNpcIds);
            }
            if (world_.state_.now.minute % kMinutesPerDay == 0) {
                world_.processDayBoundary(world_.state_.now.day(), dueNpcIds);
            }
            world_.applyPromiseDeadline();
            world_.applyExchangeDeadlines();

            // Time itself is authoritative state. Even a quiet minute
            // invalidates proposals captured before that minute elapsed.
            transaction.markChanged();
        }

        // Provider requests capture the revision after the time-step commits.
        // Scheduling them inside the transaction would make every result stale
        // as soon as the clock advanced.
        for (const auto& [npcId, trigger, channelId] : world_.pendingDecisionTriggers_) {
            world_.enqueueAutonomousTurn(npcId, trigger, channelId);
        }
        world_.pendingDecisionTriggers_.clear();
        world_.scheduleAutomaticEvolution();
        if (autonomousBoundary) world_.processAutonomousTurns(dueNpcIds);
        world_.pumpAsyncTurns();
        world_.maybeSaveDurableCheckpoint();
    }
    world_.pumpAsyncTurns();
    world_.syncNpcEntityState();
}

void SimulationCoordinator::advanceTo(GameTime target) {
    if (target.minute <= world_.state_.now.minute) return;
    advanceMinutes(target.minute - world_.state_.now.minute);
}

}  // namespace tide_marks
