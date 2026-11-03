#include "npc/drive_system.hpp"

#include <algorithm>
#include <cmath>

namespace tide_marks {

namespace {

constexpr double kMinimumChange = 0.000001;

NeedState& stateFor(NpcState& npc, const NeedContent& definition, GameTime now) {
    auto [found, inserted] = npc.needs.try_emplace(definition.id);
    if (inserted) found->second = NeedState{definition.initialDeficit, now, 0};
    return found->second;
}

}  // namespace

void DriveSystem::initialize(NpcState& npc, const std::vector<NeedContent>& definitions, GameTime now) {
    npc.needs.clear();
    for (const auto& definition : definitions) {
        npc.needs.emplace(definition.id, NeedState{definition.initialDeficit, now, 0});
    }
}

std::vector<NeedChange> DriveSystem::advance(NpcState& npc,
                                             const std::vector<NeedContent>& definitions,
                                             GameTime from,
                                             GameTime to) {
    std::vector<NeedChange> changes;
    if (to <= from) return changes;
    for (const auto& definition : definitions) {
        auto& state = stateFor(npc, definition, from);
        const GameTime effectiveFrom{std::max(from.minute, state.lastUpdated.minute)};
        const double elapsedHours = static_cast<double>(to.minute - effectiveFrom.minute) / 60.0;
        if (elapsedHours <= 0.0) continue;
        const double before = std::clamp(state.deficit, 0.0, 1.0);
        const double after = std::clamp(before + definition.decayPerHour * elapsedHours, 0.0, 1.0);
        if (std::abs(after - before) < kMinimumChange) {
            state.lastUpdated = to;
            continue;
        }
        state.deficit = after;
        state.lastUpdated = to;
        changes.push_back(NeedChange{definition.id, before, after,
                                     before < definition.criticalThreshold &&
                                         after >= definition.criticalThreshold});
    }
    return changes;
}

std::vector<NeedChange> DriveSystem::relieve(NpcState& npc,
                                             const std::vector<NeedContent>& definitions,
                                             const std::string& actionId,
                                             GameTime now) {
    std::vector<NeedChange> changes;
    for (const auto& definition : definitions) {
        const auto satisfier = definition.satisfiers.find(actionId);
        if (satisfier == definition.satisfiers.end()) continue;
        auto& state = stateFor(npc, definition, now);
        const double before = std::clamp(state.deficit, 0.0, 1.0);
        const double after = std::clamp(before - std::max(0.0, satisfier->second), 0.0, 1.0);
        if (std::abs(after - before) < kMinimumChange) continue;
        state.deficit = after;
        state.lastUpdated = now;
        changes.push_back(NeedChange{definition.id, before, after,
                                     before < definition.criticalThreshold &&
                                         after >= definition.criticalThreshold});
    }
    return changes;
}

double DriveSystem::drive(const NeedContent& definition, const NeedState& state) {
    const double deficit = std::clamp(state.deficit, 0.0, 1.0);
    const double base = deficit * deficit;
    const double threshold = std::clamp(definition.criticalThreshold, 0.0, 1.0);
    double criticalMultiplier = 1.0;
    if (deficit >= threshold && threshold < 1.0) {
        criticalMultiplier += 2.0 * (deficit - threshold) / (1.0 - threshold);
    }
    return definition.priority + definition.weight * base * criticalMultiplier;
}

}  // namespace tide_marks
