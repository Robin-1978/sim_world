#pragma once

#include <set>
#include <string>

namespace tide_marks {

class WorldService;

// Applies authored schedule and hourly drive boundaries through the owning
// WorldService transaction. The system owns no WorldState of its own.
class ScheduleSystem final {
public:
    explicit ScheduleSystem(WorldService& world) : world_(world) {}

    void processHourBoundary(int day, int hour, const std::set<std::string>& dueNpcIds);

private:
    WorldService& world_;
};

}  // namespace tide_marks
