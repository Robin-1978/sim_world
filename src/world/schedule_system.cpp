#include "world/schedule_system.hpp"

#include "npc/drive_system.hpp"
#include "npc/state_machine.hpp"
#include "world/ids.hpp"
#include "world/world_service.hpp"

namespace tide_marks {

namespace {
void applyNpcSignal(NpcState& npc, NpcSignal signal) {
    npc.mode = toString(transition(npcModeFromString(npc.mode), signal));
}
}  // namespace

void ScheduleSystem::processHourBoundary(int day, int hour,
                                          const std::set<std::string>& dueNpcIds) {
    (void)day;
    const int minuteOfDay = hour * static_cast<int>(kMinutesPerHour);
    for (auto& [id, npc] : world_.state_.npcs) {
        if (!dueNpcIds.contains(id)) continue;
        const auto* authored = world_.authoredNpc(id);
        if (authored != nullptr) {
            const GameTime previous = npc.lastDriveTick.minute >= 0
                                           ? npc.lastDriveTick
                                           : GameTime{world_.state_.now.minute - kMinutesPerHour};
            const auto changes = DriveSystem::advance(npc, authored->needs, previous, world_.state_.now);
            Json changed = Json::array();
            for (const auto& change : changes) {
                changed.push_back(Json{{"need_id", change.needId}, {"before", change.before},
                                       {"after", change.after}});
                if (change.becameCritical) {
                    world_.addEvent("need.critical", id, change.needId,
                                    "A need crossed its critical threshold",
                                    Json{{"need_id", change.needId}, {"before", change.before},
                                         {"deficit", change.after}, {"rule", "drive.time_decay"},
                                         {"from_minute", previous.minute},
                                         {"to_minute", world_.state_.now.minute}});
                }
            }
            if (!changed.empty()) {
                world_.addEvent("need.deprived", id, "self", "Need deficits advanced with world time",
                                Json{{"changes", changed}, {"rule", "drive.time_decay"},
                                     {"from_minute", previous.minute},
                                     {"to_minute", world_.state_.now.minute}});
            }
            npc.lastDriveTick = world_.state_.now;
        }
        const auto entity = world_.state_.entities.find(npcEntityId(id));
        if (entity != world_.state_.entities.end() && entity->second.lifecycle != EntityLifecycle::Active) continue;
        std::string scheduledLocation = authored != nullptr ? authored->definition.home : npc.location;
        if (authored != nullptr) {
            for (const auto& slot : authored->schedule) {
                const bool within = slot.startMinute < slot.endMinute
                                        ? minuteOfDay >= slot.startMinute && minuteOfDay < slot.endMinute
                                        : (slot.startMinute == slot.endMinute || minuteOfDay >= slot.startMinute ||
                                           minuteOfDay < slot.endMinute);
                if (within) {
                    scheduledLocation = slot.location;
                    break;
                }
            }
        }
        const auto previousLocation = npc.location;
        if (npc.location != scheduledLocation) {
            ++npc.stateVersion;
            world_.addEvent("npc.schedule_applied", id, scheduledLocation,
                            "The authored baseline schedule placed the NPC at its scheduled location",
                            Json{{"from", previousLocation}, {"to", scheduledLocation},
                                 {"rule", "content.schedule"}, {"minute_of_day", minuteOfDay}});
        }
        npc.location = scheduledLocation;
        npc.scheduleLabel = scheduledLocation;
        if (!npc.activity.process || npc.activity.process->status != ProcessStatus::Running) {
            const bool atHome = authored != nullptr && npc.location == authored->definition.home;
            applyNpcSignal(npc, atHome ? NpcSignal::AtHome : NpcSignal::AtWork);
        }
    }
    for (const auto& activity : world_.content_.activities) {
        if (activity.trigger != "hour" || activity.hour != hour) continue;
        if (!activity.conditionResource.empty() &&
            world_.resourceValue(activity.conditionResource) >= activity.conditionBelow) continue;
        world_.processActivity(activity);
    }
}

}  // namespace tide_marks
