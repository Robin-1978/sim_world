#pragma once

#include "content/content_pack.hpp"
#include "core/types.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tide_marks {

class WorldService;

// Coordinates NPC activity lifecycles without owning world facts.  The
// WorldService remains the only transaction and EventLog boundary; this
// system only applies content-authored process rules through that boundary.
class NpcSimulationSystem {
public:
    explicit NpcSimulationSystem(WorldService& world) : world_(world) {}

    void processActivity(const ActivityContent& activity);
    void tickProcesses(const std::set<std::string>& dueNpcIds);
    void resumeSuspendedProcesses(const std::set<std::string>& dueNpcIds);
    void processAutonomousTurns(const std::set<std::string>& dueNpcIds);
    void processDayBoundary(int day, const std::set<std::string>& dueNpcIds);

private:
    friend class WorldService;
    void releaseReservations(ProcessExecution& process);
    void suspendProcess(const std::string& npcId, ProcessExecution& process,
                        const std::string& reason, Json eventData = Json::object());
    [[nodiscard]] std::optional<std::vector<std::string>> planResourcePreemption(
        const std::string& requesterId, const std::map<std::string, int>& requirements,
        int requesterPriority) const;
    void cancelProcess(const std::string& npcId, ProcessExecution& process,
                      const std::string& reason);
    void completeProcess(const std::string& npcId, ProcessExecution process);
    void finalizeInnovationAdoption(const std::string& npcId, const ProcessExecution& process,
                                    bool recipeApplied);
    void reviseNpcStrategies(int day);

    WorldService& world_;
};

}  // namespace tide_marks
