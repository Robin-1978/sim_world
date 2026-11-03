#include "core/types.hpp"
#include <algorithm>
#include <sstream>

namespace tide_marks {
int GameTime::day() const {
    return static_cast<int>(minute / kMinutesPerDay) + 1;
}

int GameTime::hour() const {
    return static_cast<int>((minute % kMinutesPerDay) / kMinutesPerHour);
}

int GameTime::minuteOfHour() const {
    return static_cast<int>(minute % kMinutesPerHour);
}

std::string GameTime::toString() const {
    std::ostringstream output;
    output << "day " << day() << ' ' << hour() << ':';
    if (minuteOfHour() < 10) {
        output << '0';
    }
    output << minuteOfHour();
    return output.str();
}

GameTime GameTime::fromDayHour(int dayValue, int hourValue, int minuteValue) {
    return GameTime{static_cast<std::int64_t>(std::max(1, dayValue) - 1) * kMinutesPerDay +
                    static_cast<std::int64_t>(hourValue) * kMinutesPerHour + minuteValue};
}

std::string toString(PromiseStatus status) {
    switch (status) {
    case PromiseStatus::Proposed: return "proposed";
    case PromiseStatus::Accepted: return "accepted";
    case PromiseStatus::Renegotiated: return "renegotiated";
    case PromiseStatus::Refused: return "refused";
    case PromiseStatus::Fulfilled: return "fulfilled";
    case PromiseStatus::Broken: return "broken";
    }
    return "unknown";
}

std::string toString(EntityLifecycle lifecycle) {
    switch (lifecycle) {
        case EntityLifecycle::Active: return "active";
        case EntityLifecycle::Dormant: return "dormant";
        case EntityLifecycle::Destroyed: return "destroyed";
    }
    return "active";
}

EntityLifecycle entityLifecycleFromString(const std::string& value) {
    if (value == "dormant") return EntityLifecycle::Dormant;
    if (value == "destroyed") return EntityLifecycle::Destroyed;
    return EntityLifecycle::Active;
}

PromiseStatus promiseStatusFromString(const std::string& value) {
    if (value == "accepted") return PromiseStatus::Accepted;
    if (value == "renegotiated") return PromiseStatus::Renegotiated;
    if (value == "refused") return PromiseStatus::Refused;
    if (value == "fulfilled") return PromiseStatus::Fulfilled;
    if (value == "broken") return PromiseStatus::Broken;
    return PromiseStatus::Proposed;
}

std::string toString(ProcessStatus status) {
    switch (status) {
        case ProcessStatus::Running: return "running";
        case ProcessStatus::Suspended: return "suspended";
        case ProcessStatus::Completed: return "completed";
        case ProcessStatus::Failed: return "failed";
        case ProcessStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

ProcessStatus processStatusFromString(const std::string& value) {
    if (value == "suspended") return ProcessStatus::Suspended;
    if (value == "completed") return ProcessStatus::Completed;
    if (value == "failed") return ProcessStatus::Failed;
    if (value == "cancelled") return ProcessStatus::Cancelled;
    return ProcessStatus::Running;
}

std::string toString(GoalStatus status) {
    switch (status) {
        case GoalStatus::Proposed: return "proposed";
        case GoalStatus::Active: return "active";
        case GoalStatus::Suspended: return "suspended";
        case GoalStatus::Completed: return "completed";
        case GoalStatus::Failed: return "failed";
        case GoalStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

GoalStatus goalStatusFromString(const std::string& value) {
    if (value == "active") return GoalStatus::Active;
    if (value == "suspended") return GoalStatus::Suspended;
    if (value == "completed") return GoalStatus::Completed;
    if (value == "failed") return GoalStatus::Failed;
    if (value == "cancelled") return GoalStatus::Cancelled;
    return GoalStatus::Proposed;
}

std::string toString(ActivityStatus status) {
    switch (status) {
    case ActivityStatus::Proposed: return "proposed";
    case ActivityStatus::Active: return "active";
    case ActivityStatus::Suspended: return "suspended";
    case ActivityStatus::Completed: return "completed";
    case ActivityStatus::Failed: return "failed";
    case ActivityStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

ActivityStatus activityStatusFromString(const std::string& value) {
    if (value == "active") return ActivityStatus::Active;
    if (value == "suspended") return ActivityStatus::Suspended;
    if (value == "completed") return ActivityStatus::Completed;
    if (value == "failed") return ActivityStatus::Failed;
    if (value == "cancelled") return ActivityStatus::Cancelled;
    return ActivityStatus::Proposed;
}

void setActivityStatus(NpcActivity& activity, ActivityStatus status) {
    activity.status = status;
    if (activity.goal) {
        switch (status) {
        case ActivityStatus::Proposed: activity.goal->status = GoalStatus::Proposed; break;
        case ActivityStatus::Active: activity.goal->status = GoalStatus::Active; break;
        case ActivityStatus::Suspended: activity.goal->status = GoalStatus::Suspended; break;
        case ActivityStatus::Completed: activity.goal->status = GoalStatus::Completed; break;
        case ActivityStatus::Failed: activity.goal->status = GoalStatus::Failed; break;
        case ActivityStatus::Cancelled: activity.goal->status = GoalStatus::Cancelled; break;
        }
    }
    if (activity.process) {
        switch (status) {
        case ActivityStatus::Proposed:
        case ActivityStatus::Suspended: activity.process->status = ProcessStatus::Suspended; break;
        case ActivityStatus::Active: activity.process->status = ProcessStatus::Running; break;
        case ActivityStatus::Completed: activity.process->status = ProcessStatus::Completed; break;
        case ActivityStatus::Failed: activity.process->status = ProcessStatus::Failed; break;
        case ActivityStatus::Cancelled: activity.process->status = ProcessStatus::Cancelled; break;
        }
    }
}


}  // namespace tide_marks
