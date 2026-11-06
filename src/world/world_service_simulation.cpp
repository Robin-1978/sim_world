#include "world/world_service.hpp"

#include "world/contract_system.hpp"
#include "world/npc_simulation_system.hpp"
#include "world/schedule_system.hpp"
#include "world/simulation_coordinator.hpp"

namespace tide_marks {

void WorldService::advanceMinutes(std::int64_t minutes) {
    SimulationCoordinator(*this).advanceMinutes(minutes);
}

void WorldService::advanceTo(GameTime target) {
    SimulationCoordinator(*this).advanceTo(target);
}

void WorldService::processHourBoundary(int day, int hour, const std::set<std::string>& dueNpcIds) {
    ScheduleSystem(*this).processHourBoundary(day, hour, dueNpcIds);
}

void WorldService::processActivity(const ActivityContent& activity) {
    NpcSimulationSystem(*this).processActivity(activity);
}

void WorldService::tickProcesses(const std::set<std::string>& dueNpcIds) {
    NpcSimulationSystem(*this).tickProcesses(dueNpcIds);
}

void WorldService::resumeSuspendedProcesses(const std::set<std::string>& dueNpcIds) {
    NpcSimulationSystem(*this).resumeSuspendedProcesses(dueNpcIds);
}

void WorldService::releaseReservations(ProcessExecution& process) {
    NpcSimulationSystem(*this).releaseReservations(process);
}

void WorldService::suspendProcess(const std::string& npcId, ProcessExecution& process,
                                  const std::string& reason, Json eventData) {
    NpcSimulationSystem(*this).suspendProcess(npcId, process, reason, std::move(eventData));
}

std::optional<std::vector<std::string>> WorldService::planResourcePreemption(
    const std::string& requesterId, const std::map<std::string, int>& requirements,
    int requesterPriority) {
    return NpcSimulationSystem(*this).planResourcePreemption(
        requesterId, requirements, requesterPriority);
}

void WorldService::cancelProcess(const std::string& npcId, ProcessExecution& process,
                                 const std::string& reason) {
    NpcSimulationSystem(*this).cancelProcess(npcId, process, reason);
}

void WorldService::completeProcess(const std::string& npcId, ProcessExecution process) {
    NpcSimulationSystem(*this).completeProcess(npcId, std::move(process), true);
}

void WorldService::processAutonomousTurns(const std::set<std::string>& dueNpcIds) {
    NpcSimulationSystem(*this).processAutonomousTurns(dueNpcIds);
}

void WorldService::processDayBoundary(int day, const std::set<std::string>& dueNpcIds) {
    NpcSimulationSystem(*this).processDayBoundary(day, dueNpcIds);
}

void WorldService::reviseNpcStrategies(int day) {
    NpcSimulationSystem(*this).reviseNpcStrategies(day);
}

void WorldService::applyPromiseDeadline() {
    ContractSystem(*this).applyPromiseDeadline();
}

void WorldService::applyExchangeDeadlines() {
    ContractSystem(*this).applyExchangeDeadlines();
}

}  // namespace tide_marks
